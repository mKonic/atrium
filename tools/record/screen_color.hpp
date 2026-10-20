#pragma once
// What a screen's colours are right now, asked of atrium over its control
// socket once a second (gsr polls KDE's night light the same way): whether
// it's in HDR, and the night light gain still in its copies.

#include "gpu.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace atrium::record {

class ScreenColorWatch {
public:
    ~ScreenColorWatch();
    // Asks once now; false when there's no atrium to ask (the colours are
    // then taken as plain SDR).
    bool start(const std::string& output);
    ScreenColor get() const;

private:
    bool query(ScreenColor& out);

    std::string output_;
    mutable std::mutex lock_;
    ScreenColor color_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace atrium::record
