#include "playback.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>

#include <QLoggingCategory>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

Q_LOGGING_CATEGORY(lcPlay, "atrium.phonelink.playback")

namespace atrium::phonelink {

namespace {

std::uint64_t nowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

const pw_stream_events kEvents = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    return e;
}();

} // namespace

Playback::Playback(std::string key, std::uint32_t nackCounter, const std::string& phoneName, std::uint32_t targetFrames,
                   const sockaddr_in6& phone)
    : key_(std::move(key)), target_(targetFrames), nackCounter_(nackCounter), phone_(phone), buffer_(targetFrames) {
    fd_ = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd_ < 0) {
        qCWarning(lcPlay) << "no UDP socket:" << strerror(errno);
        return;
    }
    int no = 0;
    setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof no);  // the phone is IPv4 too
    int big = 1 << 20;
    setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    int tos = 0xb8;
    setsockopt(fd_, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_any;
    socklen_t len = sizeof a;
    if (bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 ||
        getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len) < 0) {
        qCWarning(lcPlay) << "can't bind:" << strerror(errno);
        close(fd_);
        fd_ = -1;
        return;
    }
    port_ = ntohs(a.sin6_port);
    // A firewall here (ufw's default) drops datagrams nobody asked for, but
    // lets in replies on a flow this end opened: open it before the phone
    // sends, and keep it open through silences (receive()).
    knock();

    static pw_stream_events events = [] {
        pw_stream_events e = kEvents;
        e.process = &Playback::process;
        return e;
    }();
    loop_ = pw_thread_loop_new("phonelink", nullptr);
    const std::string latency = std::to_string(kPacketFrames) + "/" + std::to_string(kRate);
    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_NODE_NAME, "atrium-phonelink", PW_KEY_NODE_DESCRIPTION, phoneName.c_str(), PW_KEY_MEDIA_NAME,
        phoneName.c_str(), PW_KEY_APP_NAME, "Phone", PW_KEY_APP_ICON_NAME, "phone", PW_KEY_NODE_LATENCY,
        latency.c_str(), nullptr);
    stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_), "Phone audio", props, &events, this);

    std::uint8_t pod[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(pod, sizeof pod);
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_S16_LE;
    info.rate = kRate;
    info.channels = kChannels;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod* params[] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
    if (!stream_ || pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                      pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                      PW_STREAM_FLAG_RT_PROCESS),
                                      params, 1) < 0) {
        qCWarning(lcPlay) << "no PipeWire stream";
        if (stream_)
            pw_stream_destroy(stream_);
        stream_ = nullptr;
        return;
    }
    pw_thread_loop_start(loop_);
    thread_ = std::thread([this] { receive(); });
}

Playback::~Playback() {
    running_ = false;
    if (thread_.joinable())
        thread_.join();
    if (loop_)
        pw_thread_loop_stop(loop_);
    if (stream_)
        pw_stream_destroy(stream_);
    if (loop_)
        pw_thread_loop_destroy(loop_);
    if (fd_ >= 0)
        close(fd_);
    const JitterStats& s = buffer_.stats();
    qCInfo(lcPlay) << "ended:" << s.received << "packets," << s.recovered << "recovered," << s.late << "late,"
                   << s.concealed << "frames concealed," << s.skipped << "skipped," << s.resyncs << "resyncs";
}

JitterStats Playback::stats() {
    std::lock_guard l(mutex_);
    return buffer_.stats();
}

bool Playback::playing() {
    std::lock_guard l(mutex_);
    return buffer_.playing();
}

void Playback::receive() {
    char d[2048];
    std::uint64_t lastLog = nowUs(), lastKnock = lastLog;
    while (running_) {
        pollfd p{fd_, POLLIN, 0};
        poll(&p, 1, 5);
        for (;;) {
            const ssize_t n = recv(fd_, d, sizeof d, 0);
            if (n <= 0)
                break;
            std::optional<AudioPacket> a = unpackAudio(key_, std::string_view(d, std::size_t(n)));
            if (!a)
                continue;  // not the phone's, or not this session's
            std::lock_guard l(mutex_);
            buffer_.push(*a, nowUs());
        }
        const std::uint64_t now = nowUs();
        std::vector<std::uint32_t> lost;
        {
            std::lock_guard l(mutex_);
            lost = buffer_.nacks(now);
        }
        if (!lost.empty()) {
            const std::string n = packNack(key_, nackCounter_++, lost);
            sendto(fd_, n.data(), n.size(), 0, reinterpret_cast<const sockaddr*>(&phone_), sizeof phone_);
        }
        if (now - lastKnock > 10'000'000) {
            lastKnock = now;
            knock();
        }
        if (now - lastLog > 30'000'000) {
            lastLog = now;
            const JitterStats s = stats();
            qCInfo(lcPlay) << s.received << "packets," << s.recovered << "recovered," << s.late << "late,"
                           << s.concealed << "frames concealed," << s.skipped << "skipped";
        }
    }
}

// One byte the phone throws away (it is no NACK).
void Playback::knock() {
    const char b = 0;
    sendto(fd_, &b, 1, 0, reinterpret_cast<const sockaddr*>(&phone_), sizeof phone_);
}

void Playback::process(void* data) { static_cast<Playback*>(data)->process(); }

void Playback::process() {
    pw_buffer* b = pw_stream_dequeue_buffer(stream_);
    if (!b)
        return;
    spa_data& d = b->buffer->datas[0];
    if (!d.data) {
        pw_stream_queue_buffer(stream_, b);
        return;
    }
    std::uint32_t frames = d.maxsize / kFrameBytes;
    if (b->requested && b->requested < frames)
        frames = std::uint32_t(b->requested);
    bool playing;
    std::int64_t level;
    {
        std::lock_guard l(mutex_);
        buffer_.pull(static_cast<std::int16_t*>(d.data), frames);
        playing = buffer_.playing();
        level = buffer_.level();
    }
    // The phone's clock against the sound card's: play a touch faster or
    // slower to stay at the target.
    if (playing) {
        float rate = float(drift_.update(level, target_, frames));
        pw_stream_set_control(stream_, SPA_PROP_rate, 1, &rate, 0);
    } else if (wasPlaying_) {
        drift_.reset();
        float rate = 1;
        pw_stream_set_control(stream_, SPA_PROP_rate, 1, &rate, 0);
    }
    wasPlaying_ = playing;
    d.chunk->offset = 0;
    d.chunk->stride = kFrameBytes;
    d.chunk->size = frames * kFrameBytes;
    pw_stream_queue_buffer(stream_, b);
}

} // namespace atrium::phonelink
