#pragma once
// One stream of a phone's audio: the UDP socket it arrives on, the jitter
// buffer, and a PipeWire node that pulls from it: a playback stream for the
// phone's sound (kSound), or a source apps record from for its microphone
// (kMic). Datagrams are read on a thread of their own and the node's process
// callback runs on PipeWire's; neither waits on Qt's loop.

#include "jitter.hpp"

#include <netinet/in.h>
#include <pipewire/stream.h>
#include <pipewire/thread-loop.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace atrium::phonelink {

class Playback {
public:
    // `key`: the session's datagram key. `nackCounter`: where this stream's
    // asks count from; a session's must never repeat (it's a nonce).
    // `phone`: where its datagrams come from (its link port), as a v4-mapped
    // or v6 address. A source calls `used` (on PipeWire's thread) when
    // something starts or stops recording from it.
    Playback(std::string key, std::uint32_t nackCounter, const std::string& phoneName, std::uint32_t targetFrames,
             const sockaddr_in6& phone, const Datagrams& kind = kSound, std::function<void(bool)> used = {});
    ~Playback();

    bool ok() const { return fd_ >= 0 && stream_; }
    std::uint16_t port() const { return port_; }
    std::uint32_t nackCounter() const { return nackCounter_; }
    const sockaddr_in6& phone() const { return phone_; }
    JitterStats stats();
    bool playing();

private:
    void receive();
    void knock();
    static void process(void* data);
    void process();
    static void stateChanged(void* data, enum pw_stream_state old, enum pw_stream_state state, const char* error);

    const Datagrams kind_;
    std::function<void(bool)> used_;
    std::string key_;
    std::uint32_t target_;
    int fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{true};
    std::thread thread_;
    std::atomic<std::uint32_t> nackCounter_;
    sockaddr_in6 phone_{};

    std::mutex mutex_;  // the buffer, between the two threads
    JitterBuffer buffer_;
    Drift drift_;
    bool wasPlaying_ = false;

    pw_thread_loop* loop_ = nullptr;
    pw_stream* stream_ = nullptr;
};

} // namespace atrium::phonelink
