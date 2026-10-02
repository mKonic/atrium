#pragma once
// One stream of a phone's audio: the UDP socket it arrives on, the jitter
// buffer, and a PipeWire playback node that pulls from it. Datagrams are
// read on a thread of their own and the node's process callback runs on
// PipeWire's; neither waits on Qt's loop.

#include "jitter.hpp"

#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct pw_thread_loop;
struct pw_stream;

namespace atrium::phonelink {

class Playback {
public:
    // `key`: the session's datagram key. `nackCounter`: where this stream's
    // asks count from; a session's must never repeat (it's a nonce).
    // `phone`: where its datagrams come from (its link port), as a v4-mapped
    // or v6 address.
    Playback(std::string key, std::uint32_t nackCounter, const std::string& phoneName, std::uint32_t targetFrames,
             const sockaddr_in6& phone);
    ~Playback();

    bool ok() const { return fd_ >= 0 && stream_; }
    std::uint16_t port() const { return port_; }
    std::uint32_t nackCounter() const { return nackCounter_; }
    JitterStats stats();
    bool playing();

private:
    void receive();
    void knock();
    static void process(void* data);
    void process();

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
