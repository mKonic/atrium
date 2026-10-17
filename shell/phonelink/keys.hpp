#pragma once
// Keys pressed for a phone: a virtual keyboard (zwp_virtual_keyboard_v1) on
// the session's seat, made at the first key. Its keymap is xkb's default:
// only layout-free keys go through it.

#include <cstdint>

struct wl_display;
struct wl_registry;
struct wl_seat;
struct zwp_virtual_keyboard_manager_v1;
struct zwp_virtual_keyboard_v1;
struct wl_registry_listener;

namespace atrium::phonelink {

class Keys {
public:
    // `display` is dispatched by its owner; null: no keys.
    explicit Keys(wl_display* display);
    ~Keys();
    Keys(const Keys&) = delete;
    Keys& operator=(const Keys&) = delete;

    // `key` is an evdev code (KEY_RIGHT).
    void key(std::uint32_t key, bool down);

private:
    static const ::wl_registry_listener registryListener;

    bool make();

    wl_display* display_;
    wl_registry* registry_ = nullptr;
    wl_seat* seat_ = nullptr;
    zwp_virtual_keyboard_manager_v1* manager_ = nullptr;
    zwp_virtual_keyboard_v1* keyboard_ = nullptr;
};

} // namespace atrium::phonelink
