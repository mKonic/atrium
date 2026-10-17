#include "keys.hpp"

#include "virtual-keyboard-unstable-v1-client-protocol.h"

#include <QLoggingCategory>

#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string_view>

namespace atrium::phonelink {

namespace {

Q_LOGGING_CATEGORY(lc, "atrium.phonelink.keys")

std::uint32_t nowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return std::uint32_t(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

} // namespace

const wl_registry_listener Keys::registryListener = {
    .global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
        auto* self = static_cast<Keys*>(data);
        const std::string_view i = interface;
        if (i == wl_seat_interface.name && !self->seat_)
            self->seat_ = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
        else if (i == zwp_virtual_keyboard_manager_v1_interface.name)
            self->manager_ = static_cast<zwp_virtual_keyboard_manager_v1*>(
                wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1));
    },
    .global_remove = [](void*, wl_registry*, uint32_t) {},
};

Keys::Keys(wl_display* display) : display_(display) {
    if (!display_)
        return;
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &registryListener, this);
    wl_display_roundtrip(display_);
}

Keys::~Keys() {
    if (keyboard_)
        zwp_virtual_keyboard_v1_destroy(keyboard_);
    if (manager_)
        zwp_virtual_keyboard_manager_v1_destroy(manager_);
    if (seat_)
        wl_seat_destroy(seat_);
    if (registry_)
        wl_registry_destroy(registry_);
    if (display_)
        wl_display_flush(display_);
}

bool Keys::make() {
    if (keyboard_)
        return true;
    if (!seat_ || !manager_) {
        qCWarning(lc) << "the compositor has no" << (seat_ ? "virtual keyboards" : "seat");
        return false;
    }
    xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap* keymap = context ? xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS) : nullptr;
    char* text = keymap ? xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1) : nullptr;
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    if (!text) {
        qCWarning(lc) << "no keymap";
        return false;
    }
    const std::size_t size = std::strlen(text) + 1;
    const int fd = memfd_create("atrium-phonelink-keymap", MFD_CLOEXEC);
    const bool written = fd >= 0 && write(fd, text, size) == ssize_t(size);
    std::free(text);
    if (!written) {
        if (fd >= 0)
            close(fd);
        qCWarning(lc) << "can't pass the keymap";
        return false;
    }
    keyboard_ = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(manager_, seat_);
    zwp_virtual_keyboard_v1_keymap(keyboard_, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, std::uint32_t(size));
    close(fd);
    return true;
}

void Keys::key(std::uint32_t key, bool down) {
    if (!make())
        return;
    zwp_virtual_keyboard_v1_key(keyboard_, nowMs(), key,
                                down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
    wl_display_flush(display_);
}

} // namespace atrium::phonelink
