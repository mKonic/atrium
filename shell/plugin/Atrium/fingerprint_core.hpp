#pragma once
// What the lock screen does with each of fprintd's VerifyStatus results.

#include <string>
#include <string_view>

namespace atrium {

struct FingerprintStep {
    enum class Action {
        Unlock,    // the finger matched
        Continue,  // the scan goes on (fprintd keeps listening)
        Restart,   // this attempt is over: VerifyStop, VerifyStart again
        Stop,      // no more fingerprints this time: the password it is
    };
    Action action = Action::Continue;
    std::string message;  // for under the password field ("" leaves it)
    int misses = 0;       // wrong fingers so far, this one included
};

// Wrong fingers before it gives up (fprintd's own pam module allows 3; the
// lock screen is the device's owner sitting in front of it).
inline constexpr int kFingerprintMisses = 5;

// `done`: fprintd ended this verify (VerifyStatus's second argument).
FingerprintStep fingerprint_step(std::string_view result, bool done, int misses);

} // namespace atrium
