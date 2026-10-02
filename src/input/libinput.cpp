// libinput's context and events, after wlroots' backend/libinput (MIT).
#include "input/libinput.hpp"

extern "C" {
#include <libudev.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>
}

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace atrium::input {

namespace {

uint32_t ms(uint64_t usec) {
    return uint32_t(usec / 1000);
}

} // namespace

std::string Device::output_name() const {
    const char* n = libinput_device_get_output_name(handle);
    return n ? n : "";
}

std::unique_ptr<Libinput> Libinput::create(DeviceSeat* seat, wl_event_loop* loop, Handler& handler) {
    if (!seat || !seat->udev())
        return nullptr;
    std::unique_ptr<Libinput> self(new Libinput(seat, handler));
    static const libinput_interface kInterface = {
        .open_restricted = [](const char* path, int, void* data) -> int {
            return static_cast<Libinput*>(data)->seat_->open(path);
        },
        .close_restricted = [](int fd, void* data) { static_cast<Libinput*>(data)->seat_->close(fd); },
    };
    self->li_ = libinput_udev_create_context(&kInterface, self.get(), seat->udev());
    if (!self->li_)
        return nullptr;
    libinput_log_set_priority(self->li_, LIBINPUT_LOG_PRIORITY_ERROR);
    if (libinput_udev_assign_seat(self->li_, seat->name()) != 0) {
        wlr_log(WLR_ERROR, "libinput: couldn't take seat %s", seat->name());
        return nullptr;
    }
    self->source_ = wl_event_loop_add_fd(loop, libinput_get_fd(self->li_), WL_EVENT_READABLE, dispatch, self.get());
    // Devices already there come as events now.
    dispatch(libinput_get_fd(self->li_), WL_EVENT_READABLE, self.get());
    return self;
}

Libinput::~Libinput() {
    if (source_)
        wl_event_source_remove(source_);
    for (auto& d : devices_)
        handler_.device_removed(*d);
    devices_.clear();
    if (li_)
        libinput_unref(li_);
}

void Libinput::set_active(bool active) {
    if (!li_)
        return;
    if (active)
        libinput_resume(li_);
    else
        libinput_suspend(li_);
}

int Libinput::dispatch(int, uint32_t, void* data) {
    auto* l = static_cast<Libinput*>(data);
    if (libinput_dispatch(l->li_) != 0) {
        wlr_log(WLR_ERROR, "libinput: dispatch failed");
        return 0;
    }
    while (libinput_event* e = libinput_get_event(l->li_)) {
        l->handle(e);
        libinput_event_destroy(e);
    }
    return 0;
}

Device* Libinput::device_of(libinput_device* d) const {
    return static_cast<Device*>(libinput_device_get_user_data(d));
}

void Libinput::handle(libinput_event* e) {
    libinput_device* ld = libinput_event_get_device(e);
    const libinput_event_type type = libinput_event_get_type(e);

    if (type == LIBINPUT_EVENT_DEVICE_ADDED) {
        auto d = std::make_unique<Device>();
        d->handle = libinput_device_ref(ld);
        d->name = libinput_device_get_name(ld);
        d->keyboard = libinput_device_has_capability(ld, LIBINPUT_DEVICE_CAP_KEYBOARD);
        d->pointer = libinput_device_has_capability(ld, LIBINPUT_DEVICE_CAP_POINTER);
        d->touch = libinput_device_has_capability(ld, LIBINPUT_DEVICE_CAP_TOUCH);
        d->tablet = libinput_device_has_capability(ld, LIBINPUT_DEVICE_CAP_TABLET_TOOL);
        d->pad = libinput_device_has_capability(ld, LIBINPUT_DEVICE_CAP_TABLET_PAD);
        d->gesture = libinput_device_has_capability(ld, LIBINPUT_DEVICE_CAP_GESTURE);
        d->switches = libinput_device_has_capability(ld, LIBINPUT_DEVICE_CAP_SWITCH);
        libinput_device_set_user_data(ld, d.get());
        Device* raw = d.get();
        devices_.push_back(std::move(d));
        handler_.device_added(*raw);
        return;
    }
    Device* d = device_of(ld);
    if (!d)
        return;
    if (type == LIBINPUT_EVENT_DEVICE_REMOVED) {
        handler_.device_removed(*d);
        libinput_device_set_user_data(ld, nullptr);
        libinput_device_unref(d->handle);
        std::erase_if(devices_, [d](const auto& p) { return p.get() == d; });
        return;
    }

    switch (type) {
    case LIBINPUT_EVENT_KEYBOARD_KEY: {
        auto* k = libinput_event_get_keyboard_event(e);
        // A key held on two devices comes once per device: counted per key.
        handler_.key(*d, ms(libinput_event_keyboard_get_time_usec(k)), libinput_event_keyboard_get_key(k),
                     libinput_event_keyboard_get_key_state(k) == LIBINPUT_KEY_STATE_PRESSED);
        break;
    }
    case LIBINPUT_EVENT_POINTER_MOTION: {
        auto* p = libinput_event_get_pointer_event(e);
        handler_.motion(*d, ms(libinput_event_pointer_get_time_usec(p)), libinput_event_pointer_get_dx(p),
                        libinput_event_pointer_get_dy(p), libinput_event_pointer_get_dx_unaccelerated(p),
                        libinput_event_pointer_get_dy_unaccelerated(p));
        handler_.frame(*d);
        break;
    }
    case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
        auto* p = libinput_event_get_pointer_event(e);
        handler_.motion_absolute(*d, ms(libinput_event_pointer_get_time_usec(p)),
                                 libinput_event_pointer_get_absolute_x_transformed(p, 1),
                                 libinput_event_pointer_get_absolute_y_transformed(p, 1));
        handler_.frame(*d);
        break;
    }
    case LIBINPUT_EVENT_POINTER_BUTTON: {
        auto* p = libinput_event_get_pointer_event(e);
        handler_.button(*d, ms(libinput_event_pointer_get_time_usec(p)), libinput_event_pointer_get_button(p),
                        libinput_event_pointer_get_button_state(p) == LIBINPUT_BUTTON_STATE_PRESSED);
        handler_.frame(*d);
        break;
    }
    case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
    case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
    case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS: {
        auto* p = libinput_event_get_pointer_event(e);
        const Scroll::Source source = type == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL    ? Scroll::Wheel
                                      : type == LIBINPUT_EVENT_POINTER_SCROLL_FINGER ? Scroll::Finger
                                                                                     : Scroll::Continuous;
        for (auto axis : {LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL}) {
            if (!libinput_event_pointer_has_axis(p, axis))
                continue;
            Scroll s{};
            s.time_ms = ms(libinput_event_pointer_get_time_usec(p));
            s.orientation = axis == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL ? 0 : 1;
            s.delta = libinput_event_pointer_get_scroll_value(p, axis);
            s.value120 = source == Scroll::Wheel ? int32_t(libinput_event_pointer_get_scroll_value_v120(p, axis)) : 0;
            s.source = source;
            s.stop = source != Scroll::Wheel && s.delta == 0;
            handler_.scroll(*d, s);
        }
        handler_.frame(*d);
        break;
    }
    case LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN:
    case LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE:
    case LIBINPUT_EVENT_GESTURE_SWIPE_END:
        handler_.swipe(*d, libinput_event_get_gesture_event(e), type);
        break;
    case LIBINPUT_EVENT_GESTURE_PINCH_BEGIN:
    case LIBINPUT_EVENT_GESTURE_PINCH_UPDATE:
    case LIBINPUT_EVENT_GESTURE_PINCH_END:
        handler_.pinch(*d, libinput_event_get_gesture_event(e), type);
        break;
    case LIBINPUT_EVENT_GESTURE_HOLD_BEGIN:
    case LIBINPUT_EVENT_GESTURE_HOLD_END:
        handler_.hold(*d, libinput_event_get_gesture_event(e), type);
        break;
    case LIBINPUT_EVENT_TOUCH_DOWN:
    case LIBINPUT_EVENT_TOUCH_UP:
    case LIBINPUT_EVENT_TOUCH_MOTION:
    case LIBINPUT_EVENT_TOUCH_CANCEL:
    case LIBINPUT_EVENT_TOUCH_FRAME:
        handler_.touch(*d, libinput_event_get_touch_event(e), type);
        break;
    case LIBINPUT_EVENT_TABLET_TOOL_AXIS:
    case LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY:
    case LIBINPUT_EVENT_TABLET_TOOL_TIP:
    case LIBINPUT_EVENT_TABLET_TOOL_BUTTON:
        handler_.tablet_tool(*d, libinput_event_get_tablet_tool_event(e), type);
        break;
    case LIBINPUT_EVENT_TABLET_PAD_BUTTON:
    case LIBINPUT_EVENT_TABLET_PAD_RING:
    case LIBINPUT_EVENT_TABLET_PAD_STRIP:
    case LIBINPUT_EVENT_TABLET_PAD_KEY:
    case LIBINPUT_EVENT_TABLET_PAD_DIAL:
        handler_.tablet_pad(*d, libinput_event_get_tablet_pad_event(e), type);
        break;
    case LIBINPUT_EVENT_SWITCH_TOGGLE: {
        auto* s = libinput_event_get_switch_event(e);
        handler_.toggle(*d, libinput_event_switch_get_switch(s),
                        libinput_event_switch_get_switch_state(s) == LIBINPUT_SWITCH_STATE_ON);
        break;
    }
    default:
        break;
    }
}

} // namespace atrium::input
