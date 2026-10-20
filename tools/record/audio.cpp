#include "audio.hpp"

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>

namespace atrium::record {

namespace {

int64_t monotonic_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// {"name":"alsa_output..."} → alsa_output...
std::string json_name(const char* v) {
    if (!v)
        return {};
    const char* k = std::strstr(v, "\"name\"");
    if (!k)
        return {};
    const char* q = std::strchr(k + 6, '"');
    if (!q)
        return {};
    const char* e = std::strchr(q + 1, '"');
    return e ? std::string(q + 1, e) : std::string();
}

} // namespace

void Clock::start(int64_t now_ns) {
    std::lock_guard g(lock_);
    start_ = now_ns;
}

void Clock::set_paused(bool paused, int64_t now_ns) {
    std::lock_guard g(lock_);
    if (paused && paused_at_ < 0) {
        paused_at_ = now_ns;
    } else if (!paused && paused_at_ >= 0) {
        paused_total_ += now_ns - paused_at_;
        paused_at_ = -1;
    }
}

bool Clock::paused() const {
    std::lock_guard g(lock_);
    return paused_at_ >= 0;
}

int64_t Clock::at(int64_t mono_ns) const {
    std::lock_guard g(lock_);
    if (start_ < 0 || paused_at_ >= 0)
        return -1;
    const int64_t t = mono_ns - start_ - paused_total_;
    return t < 0 ? -1 : t;
}

struct AudioCapture::Track {
    std::mutex lock;
    std::vector<Source*> sources;
    std::vector<std::string> descriptions;
    std::string name;
    int64_t read_pos = 0;     // the next sample to hand out
    std::vector<float> mix;   // samples from read_pos on, interleaved
};

struct AudioCapture::Source {
    Track* track = nullptr;
    const Clock* clock = nullptr;
    pw_stream* stream = nullptr;
    spa_hook listener{};
    int frame_size = 1024;
    std::atomic<int64_t> next{-1};  // where its next sample goes; -1 before the first
};

namespace {

void on_process(void* data) {
    auto* s = static_cast<AudioCapture::Source*>(data);
    pw_buffer* b = pw_stream_dequeue_buffer(s->stream);
    if (!b)
        return;
    spa_buffer* buf = b->buffer;
    const spa_data& d = buf->datas[0];
    if (d.data && d.chunk->size > 0 && !(d.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
        const auto* samples = reinterpret_cast<const float*>(static_cast<const uint8_t*>(d.data) + d.chunk->offset);
        const int n = int(d.chunk->size / (sizeof(float) * kChannels));
        // When the first of these was heard: the graph's time minus the
        // device's latency (gsr: clock - pa_stream_get_latency).
        pw_time t{};
        int64_t heard = monotonic_ns();
        if (pw_stream_get_time_n(s->stream, &t, sizeof t) == 0 && t.now > 0 && t.rate.denom > 0) {
            const int64_t delay_ns = t.delay * int64_t(SPA_NSEC_PER_SEC) * t.rate.num / t.rate.denom;
            heard = t.now - delay_ns;
        }
        heard -= int64_t(n) * 1000000000 / kSampleRate;
        const int64_t rec = s->clock->at(heard);
        if (rec >= 0) {  // before the start (gsr flushes it) or paused: dropped
            const int64_t pos = rec * kSampleRate / 1000000000;
            // In sequence while it keeps time; a gap of more than a frame
            // (the device stalled, a dropout) jumps ahead, leaving silence.
            if (s->next < 0 || pos - s->next > s->frame_size)
                s->next = pos;
            AudioCapture::Track& tr = *s->track;
            std::lock_guard g(tr.lock);
            int64_t at = s->next;
            int skip = 0;
            if (at < tr.read_pos) {  // too late for frames already encoded
                skip = int(std::min<int64_t>(n, tr.read_pos - at));
                at += skip;
            }
            const int count = n - skip;
            if (count > 0) {
                const size_t start = size_t(at - tr.read_pos) * kChannels;
                const size_t end = start + size_t(count) * kChannels;
                if (tr.mix.size() < end)
                    tr.mix.resize(end, 0.0f);
                for (size_t i = 0; i < size_t(count) * kChannels; i++)
                    tr.mix[start + i] += samples[size_t(skip) * kChannels + i];
            }
            s->next += n;
        }
    }
    pw_stream_queue_buffer(s->stream, b);
}

const pw_stream_events kStreamEvents = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.process = on_process;
    return e;
}();

} // namespace

AudioCapture::AudioCapture() {
    pw_init(nullptr, nullptr);
}

AudioCapture::~AudioCapture() {
    stop();
    if (core_) {
        pw_thread_loop_lock(loop_);
        pw_core_disconnect(core_);
        pw_thread_loop_unlock(loop_);
    }
    if (loop_)
        pw_thread_loop_stop(loop_);
    if (context_)
        pw_context_destroy(context_);
    if (loop_)
        pw_thread_loop_destroy(loop_);
}

bool AudioCapture::connect() {
    if (core_)
        return true;
    loop_ = pw_thread_loop_new("atrium-record", nullptr);
    if (!loop_)
        return false;
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_ || pw_thread_loop_start(loop_) != 0)
        return false;
    pw_thread_loop_lock(loop_);
    core_ = pw_context_connect(context_, nullptr, 0);
    pw_thread_loop_unlock(loop_);
    return core_ != nullptr;
}

namespace {

struct Listing {
    pw_thread_loop* loop;
    pw_registry* registry = nullptr;
    spa_hook registry_listener{}, core_listener{}, metadata_listener{};
    pw_metadata* metadata = nullptr;
    std::vector<AudioDevice> devices;
    std::string default_sink, default_source;
    int pending = -1;
    bool done = false;
};

const pw_metadata_events kMetadataEvents = [] {
    pw_metadata_events e{};
    e.version = PW_VERSION_METADATA_EVENTS;
    e.property = [](void* data, uint32_t, const char* key, const char*, const char* value) -> int {
        auto* l = static_cast<Listing*>(data);
        if (key && std::strcmp(key, "default.audio.sink") == 0)
            l->default_sink = json_name(value);
        else if (key && std::strcmp(key, "default.audio.source") == 0)
            l->default_source = json_name(value);
        return 0;
    };
    return e;
}();

const pw_registry_events kRegistryEvents = [] {
    pw_registry_events e{};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global = [](void* data, uint32_t id, uint32_t, const char* type, uint32_t, const spa_dict* props) {
        auto* l = static_cast<Listing*>(data);
        if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
            const char* name = props ? spa_dict_lookup(props, PW_KEY_METADATA_NAME) : nullptr;
            if (name && std::strcmp(name, "default") == 0 && !l->metadata) {
                l->metadata = static_cast<pw_metadata*>(pw_registry_bind(l->registry, id, type, PW_VERSION_METADATA, 0));
                pw_metadata_add_listener(l->metadata, &l->metadata_listener, &kMetadataEvents, l);
            }
            return;
        }
        if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props)
            return;
        const char* cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        const char* desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
        if (!cls || !name)
            return;
        const std::string c = cls;
        if (c == "Audio/Sink" || c == "Audio/Duplex")
            l->devices.push_back({std::string(name) + ".monitor", std::string("Monitor of ") + (desc ? desc : name)});
        if (c.starts_with("Audio/Source") || c == "Audio/Duplex")
            l->devices.push_back({name, desc ? desc : name});
    };
    return e;
}();

const pw_core_events kCoreEvents = [] {
    pw_core_events e{};
    e.version = PW_VERSION_CORE_EVENTS;
    e.done = [](void* data, uint32_t id, int seq) {
        auto* l = static_cast<Listing*>(data);
        if (id == PW_ID_CORE && seq == l->pending) {
            l->done = true;
            pw_thread_loop_signal(l->loop, false);
        }
    };
    return e;
}();

} // namespace

bool AudioCapture::devices(std::vector<AudioDevice>& out, std::string& default_output, std::string& default_input) {
    if (!connect())
        return false;
    Listing l{loop_};
    pw_thread_loop_lock(loop_);
    l.registry = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(l.registry, &l.registry_listener, &kRegistryEvents, &l);
    pw_core_add_listener(core_, &l.core_listener, &kCoreEvents, &l);
    // Twice: the globals, then the default metadata's properties.
    for (int round = 0; round < 2; round++) {
        l.done = false;
        l.pending = pw_core_sync(core_, PW_ID_CORE, 0);
        while (!l.done)
            if (pw_thread_loop_timed_wait(loop_, 2) != 0)
                break;
    }
    spa_hook_remove(&l.core_listener);
    spa_hook_remove(&l.registry_listener);
    if (l.metadata) {
        spa_hook_remove(&l.metadata_listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(l.metadata));
    }
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(l.registry));
    pw_thread_loop_unlock(loop_);
    out = l.devices;
    default_output = l.default_sink.empty() ? std::string() : l.default_sink + ".monitor";
    default_input = l.default_source;
    return true;
}

bool AudioCapture::start(const std::vector<AudioTrack>& tracks, int frame_size, const Clock& clock, int& exit,
                         std::string& error) {
    std::vector<AudioDevice> list;
    std::string default_output, default_input;
    if (!devices(list, default_output, default_input)) {
        error = "can't reach PipeWire";
        exit = 1;
        return false;
    }
    // gsr's validate_audio_inputs_get_track_name: every device must exist.
    auto describe = [&](const std::string& n) -> std::string {
        if (n == "default_output")
            return default_output.empty() ? std::string() : "Default output";
        if (n == "default_input")
            return default_input.empty() ? std::string() : "Default input";
        for (const AudioDevice& d : list)
            if (d.name == n)
                return d.description;
        return {};
    };
    for (const AudioTrack& spec : tracks)
        for (const std::string& n : spec.sources)
            if (describe(n).empty()) {
                error = "Audio device '" + n + "' is not a valid audio device, expected one of:\n";
                if (!default_output.empty())
                    error += "    default_output (Default output)\n";
                if (!default_input.empty())
                    error += "    default_input (Default input)\n";
                for (const AudioDevice& d : list)
                    error += "    " + d.name + " (" + d.description + ")\n";
                exit = 50;
                return false;
            }

    pw_thread_loop_lock(loop_);
    bool ok = true;
    for (const AudioTrack& spec : tracks) {
        auto track = std::make_unique<Track>();
        track->name = spec.name;
        for (const std::string& n : spec.sources) {
            track->descriptions.push_back(describe(n));
            auto s = std::make_unique<Source>();
            s->track = track.get();
            s->clock = &clock;
            s->frame_size = frame_size;
            // One encoder frame a chunk (gsr's fragsize); not a mic in use
            // to the desktop (node.virtual, as gsr); a sink's monitor for
            // what the speakers play. The defaults have no target, so
            // PipeWire moves them when the default device changes.
            pw_properties* props = pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Production",
                PW_KEY_NODE_NAME, "atrium-record", PW_KEY_APP_NAME, "atrium-record", "node.virtual", "true", nullptr);
            pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d", frame_size, kSampleRate);
            if (n == "default_output") {
                pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
            } else if (n != "default_input") {
                if (n.ends_with(".monitor")) {
                    pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
                    pw_properties_set(props, PW_KEY_TARGET_OBJECT, n.substr(0, n.size() - 8).c_str());
                } else {
                    pw_properties_set(props, PW_KEY_TARGET_OBJECT, n.c_str());
                }
            }
            s->stream = pw_stream_new(core_, "Screen recording", props);
            if (!s->stream) {
                ok = false;
                break;
            }
            pw_stream_add_listener(s->stream, &s->listener, &kStreamEvents, s.get());
            uint8_t storage[1024];
            spa_pod_builder b = SPA_POD_BUILDER_INIT(storage, sizeof storage);
            spa_audio_info_raw info{};
            info.format = SPA_AUDIO_FORMAT_F32;
            info.rate = kSampleRate;
            info.channels = kChannels;
            info.position[0] = SPA_AUDIO_CHANNEL_FL;
            info.position[1] = SPA_AUDIO_CHANNEL_FR;
            const spa_pod* params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
            if (pw_stream_connect(s->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                                  pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1) < 0) {
                ok = false;
                break;
            }
            track->sources.push_back(s.get());
            sources_.push_back(std::move(s));
        }
        tracks_.push_back(std::move(track));
        if (!ok)
            break;
    }
    pw_thread_loop_unlock(loop_);
    if (!ok) {
        error = "a PipeWire capture stream couldn't be made";
        exit = 1;
    }
    return ok;
}

std::string AudioCapture::title(size_t track) const {
    const Track& t = *tracks_[track];
    if (!t.name.empty())
        return t.name;
    std::string out;
    for (const std::string& d : t.descriptions)
        out += (out.empty() ? "Devices: " : ", ") + d;
    return out;
}

bool AudioCapture::pop(size_t track, int n, int64_t now_rec_ns, int64_t late_ns, float* out, int64_t& pos) {
    Track& t = *tracks_[track];
    std::lock_guard g(t.lock);
    const int64_t end = t.read_pos + n;
    bool ready = now_rec_ns >= 0 && (now_rec_ns - late_ns) * kSampleRate / 1000000000 >= end;
    if (!ready) {
        ready = true;
        for (const Source* s : t.sources)
            if (s->next < end)
                ready = false;
    }
    if (!ready)
        return false;
    const size_t want = size_t(n) * kChannels;
    const size_t have = std::min(want, t.mix.size());
    std::copy_n(t.mix.begin(), have, out);
    std::fill(out + have, out + want, 0.0f);
    t.mix.erase(t.mix.begin(), t.mix.begin() + long(have));
    pos = t.read_pos;
    t.read_pos = end;
    return true;
}

void AudioCapture::stop() {
    if (!loop_)
        return;
    pw_thread_loop_lock(loop_);
    for (auto& s : sources_)
        if (s->stream) {
            spa_hook_remove(&s->listener);
            pw_stream_destroy(s->stream);
            s->stream = nullptr;
        }
    pw_thread_loop_unlock(loop_);
}

} // namespace atrium::record
