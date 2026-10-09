#pragma once
// Typing aids, as KWin's input filters (src/plugins/stickykeys, slowkeys,
// bouncekeys), on evdev keycodes. Seat runs keys through them before
// anything else sees them.
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace atrium::typing {

// Bounce keys: a key pressed again within `delay_ms` of its last press is
// dropped (a tremor's double press), and so is that press's release.
class BounceKeys {
public:
    // Whether to drop the event.
    bool filter(uint32_t key, bool pressed, uint32_t time_ms, uint32_t delay_ms);
    void clear() { keys_.clear(); }

private:
    struct Key {
        uint32_t last_press = 0;
        bool rejected = false;
    };
    std::unordered_map<uint32_t, Key> keys_;
};

// Slow keys: a key only counts once it has been held for the delay; let go
// sooner, it never happened. The caller times the delay per key.
class SlowKeys {
public:
    // A press: held back until expire().
    void press(uint32_t key) { keys_[key] = false; }
    // The delay ran out: whether the key is still down (its press is due now).
    bool expire(uint32_t key);
    // A release: whether its press went out (then so does the release).
    bool release(uint32_t key);
    bool waiting(uint32_t key) const;
    void clear() { keys_.clear(); }

private:
    std::unordered_map<uint32_t, bool> keys_;  // held keys → accepted
};

// Sticky keys: a modifier pressed and let go applies to the next key (or
// click); pressed twice it stays on until pressed again. `mod` is the
// modifier's xkb mask bit.
class StickyKeys {
public:
    struct Options {
        bool lock = true;       // a second press locks it (KWin's StickyKeysLatch)
        bool auto_off = false;  // two keys pressed together turn sticky keys off
    };
    void set_options(Options o);
    void modifier_pressed(uint32_t mod);
    void modifier_released(uint32_t mod);
    // Any other key, once it went out with the modifiers: latches end.
    // Whether sticky keys turned themselves off (auto_off and a modifier
    // was held with it).
    bool key_pressed();
    // A click let go, once it went out: latches end.
    void button_released() { latched_ = 0; }
    uint32_t latched() const { return latched_; }
    uint32_t locked() const { return locked_; }
    // Every modifier sticky keys may set: these bits are its to clear.
    uint32_t managed() const { return managed_; }
    void clear();

private:
    Options options_;
    uint32_t latched_ = 0, locked_ = 0, held_ = 0, managed_ = 0;
};

} // namespace atrium::typing
