#pragma once
// Changing one key of an INI-style file (desktop entries, mimeapps.list)
// as text, so every other line, group and comment stays as it was.

#include <optional>
#include <string>
#include <string_view>

namespace atrium::ini {

// `text` with `key` in `[group]` set to `value`, or removed when there is
// none. A missing group is added at the end; a missing key at the end of
// its group.
std::string set(std::string_view text, std::string_view group, std::string_view key,
                std::optional<std::string_view> value);

// The value of `key` in `[group]`, trimmed; nothing when it isn't there.
std::optional<std::string> get(std::string_view text, std::string_view group, std::string_view key);

} // namespace atrium::ini
