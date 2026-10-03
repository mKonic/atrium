#include "fingerprint_core.hpp"

namespace atrium {

FingerprintStep fingerprint_step(std::string_view result, bool done, int misses) {
    using A = FingerprintStep::Action;
    FingerprintStep s;
    s.misses = misses;
    if (result == "verify-match") {
        s.action = A::Unlock;
        return s;
    }
    if (result == "verify-no-match") {
        s.misses = misses + 1;
        if (s.misses >= kFingerprintMisses) {
            s.action = A::Stop;
            s.message = "Fingerprint not recognized. Enter your password.";
        } else {
            s.action = A::Restart;
            s.message = "Fingerprint not recognized. Try again.";
        }
        return s;
    }
    // A bad scan, not a wrong finger: fprintd asks for another.
    if (result == "verify-retry-scan")
        s.message = "Try again.";
    else if (result == "verify-swipe-too-short")
        s.message = "Swipe was too short. Try again.";
    else if (result == "verify-finger-not-centered")
        s.message = "Center your finger and try again.";
    else if (result == "verify-remove-and-retry")
        s.message = "Lift your finger and try again.";
    else {
        // verify-disconnected, verify-unknown-error, anything new.
        s.action = A::Stop;
        return s;
    }
    s.action = done ? A::Restart : A::Continue;
    return s;
}

} // namespace atrium
