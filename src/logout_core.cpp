#include "logout.hpp"

#include <algorithm>

namespace atrium {

std::string app_name(std::string_view app_id) {
    if (const size_t dot = app_id.rfind('.'); dot != std::string_view::npos && dot + 1 < app_id.size())
        app_id.remove_prefix(dot + 1);
    std::string name(app_id);
    if (!name.empty() && name[0] >= 'a' && name[0] <= 'z')
        name[0] = char(name[0] - 'a' + 'A');
    return name;
}

std::vector<std::string> apps_to_reopen(const std::vector<std::string>& app_ids) {
    std::vector<std::string> out;
    for (const std::string& id : app_ids) {
        // The shell's own windows come back with it, or have no business coming back.
        if (id.empty() || id == "atrium-shell" || id.starts_with("atrium-capture") || id == "atrium-welcome")
            continue;
        if (std::ranges::find(out, id) == out.end())
            out.push_back(id);
    }
    return out;
}


Logout::Then Logout::parse(const std::string& arg) {
    if (arg == "restart")
        return Then::Restart;
    if (arg == "shutdown" || arg == "shut-down")
        return Then::ShutDown;
    return Then::LogOut;
}

} // namespace atrium
