#include "access_keys_core.hpp"

namespace atrium::typing {

bool BounceKeys::filter(uint32_t key, bool pressed, uint32_t time_ms, uint32_t delay_ms) {
    auto it = keys_.find(key);
    if (!pressed)
        return it != keys_.end() && it->second.rejected;
    if (it == keys_.end()) {
        keys_[key] = {time_ms, false};  // the first time is always good
        return false;
    }
    const uint32_t since = time_ms - it->second.last_press;
    it->second.last_press = time_ms;
    it->second.rejected = since < delay_ms;
    return it->second.rejected;
}

bool SlowKeys::expire(uint32_t key) {
    auto it = keys_.find(key);
    if (it == keys_.end() || it->second)
        return false;
    it->second = true;
    return true;
}

bool SlowKeys::release(uint32_t key) {
    auto it = keys_.find(key);
    if (it == keys_.end())
        return true;  // down from before slow keys: not ours
    const bool accepted = it->second;
    keys_.erase(it);
    return accepted;
}

bool SlowKeys::waiting(uint32_t key) const {
    auto it = keys_.find(key);
    return it != keys_.end() && !it->second;
}

void StickyKeys::set_options(Options o) {
    options_ = o;
    if (!o.lock)
        locked_ = 0;
}

void StickyKeys::modifier_pressed(uint32_t mod) {
    held_ |= mod;
    managed_ |= mod;
    if (locked_ & mod) {
        locked_ &= ~mod;  // a locked one, pressed: off
    } else if (latched_ & mod) {
        latched_ &= ~mod;
        if (options_.lock)
            locked_ |= mod;
    } else {
        latched_ |= mod;
    }
}

void StickyKeys::modifier_released(uint32_t mod) {
    held_ &= ~mod;
}

bool StickyKeys::key_pressed() {
    if (options_.auto_off && held_) {
        clear();
        return true;
    }
    latched_ = 0;
    return false;
}

void StickyKeys::clear() {
    latched_ = locked_ = held_ = 0;
}

} // namespace atrium::typing
