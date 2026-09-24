#include "mimeapps_core.hpp"

#include "ini_core.hpp"

namespace atrium::mimeapps {

namespace {
constexpr std::string_view kGroup = "Default Applications";
}

std::string default_for(std::string_view text, std::string_view mime) {
    const std::optional<std::string> value = ini::get(text, kGroup, mime);
    if (!value)
        return {};
    std::string first = value->substr(0, value->find(';'));
    while (!first.empty() && first.back() == ' ')
        first.pop_back();
    return first;
}

std::string set_default(std::string_view text, std::string_view mime, std::string_view desktop_id) {
    return ini::set(text, kGroup, mime, std::string(desktop_id) + ";");
}

} // namespace atrium::mimeapps
