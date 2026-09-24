#pragma once
// What the battery shows and says, from UPower's numbers, without Qt.

#include <cstdint>
#include <string>

namespace atrium::battery {

// UPower's Device.State.
enum class State : uint32_t { Unknown = 0, Charging = 1, Discharging = 2, Empty = 3, Full = 4, PendingCharge = 5,
                              PendingDischarge = 6 };

// Plugged in: charging, full, or held at a charge limit.
bool plugged(State s);

// Material Symbols name: battery_full, battery_5_bar … battery_0_bar,
// battery_alert when low, battery_charging_20 … battery_charging_full.
std::string glyph(double percent, State s);

// "2:05 left", "1:40 until full", "Fully charged", "Not charging" ("" when unknown).
std::string remaining(State s, int64_t to_empty_s, int64_t to_full_s);

// The warning due at `percent` while discharging: 0 none, 1 low (10%), 2
// critical (5%). `warned` is the last one given this discharge, so each
// comes once; plugging in starts over.
int warning(double percent, State s, int warned);

} // namespace atrium::battery
