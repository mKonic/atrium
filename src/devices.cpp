#include "devices.hpp"

#include <algorithm>

namespace atrium {

PointerSettings pointer_settings(const Config& c, const DeviceRecord* own, bool touchpad) {
    PointerSettings p{c.accel_speed, c.accel_profile, touchpad ? c.touchpad_natural_scroll : c.natural_scroll,
                      c.left_handed};
    if (!own)
        return p;
    if (own->speed)
        p.speed = std::clamp(*own->speed, -1.0, 1.0);
    if (own->acceleration == "flat")
        p.profile = LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT;
    else if (own->acceleration == "adaptive")
        p.profile = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
    if (own->natural_scroll)
        p.natural_scroll = *own->natural_scroll;
    if (own->left_handed)
        p.left_handed = *own->left_handed;
    return p;
}

} // namespace atrium
