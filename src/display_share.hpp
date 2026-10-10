#pragma once
// The displays as the last session set them, handed to the login screen
// (atrium-login keeps them in its home): it comes up in the same mode, HDR
// and layout, so logging in takes the screen over without a modeset (a
// refresh rate or HDR switch blanks a monitor for a second or two).
#include "registry.hpp"

#include <nlohmann/json.hpp>

#include <vector>

namespace atrium {

nlohmann::json displays_to_json(const std::vector<DisplayRecord>& displays);
// What's well formed of it; anything else is left out.
std::vector<DisplayRecord> displays_from_json(const nlohmann::json& j);

// The session's, to atrium-login (its control socket): the line it takes.
std::string displays_line(const std::vector<DisplayRecord>& displays);

} // namespace atrium
