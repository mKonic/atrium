#pragma once
// A pointing device's settings: the shared Mouse & Touchpad ones, with the
// device's own (registry "devices") over them.

#include "config.hpp"
#include "registry.hpp"

namespace atrium {

struct PointerSettings {
    double speed;
    libinput_config_accel_profile profile;
    bool natural_scroll, left_handed;
};

PointerSettings pointer_settings(const Config& shared, const DeviceRecord* own, bool touchpad);

} // namespace atrium
