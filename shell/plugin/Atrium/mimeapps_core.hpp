#pragma once
// mimeapps.list (freedesktop MIME Applications Associations): which app
// opens a type by default. Read and changed as text so every other line,
// group and comment stays as the user or another desktop left it.

#include <string>
#include <string_view>

namespace atrium::mimeapps {

// The first app of `mime` under [Default Applications] ("firefox.desktop"),
// or "" when the file doesn't say.
std::string default_for(std::string_view text, std::string_view mime);

// `text` with `mime`'s default set to `desktop_id` (it replaces the whole
// list), the group made when missing.
std::string set_default(std::string_view text, std::string_view mime, std::string_view desktop_id);

} // namespace atrium::mimeapps
