#include "placements.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>

namespace atrium {

namespace fs = std::filesystem;
using json = nlohmann::json;

Placements::Placements(fs::path file) : file_(std::move(file)) {
    load();
}

fs::path Placements::default_file() {
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state)
        return fs::path(state) / "atrium" / "placements.json";
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / ".local" / "state" / "atrium" / "placements.json";
}

const Placement* Placements::find(const std::string& app_id) const {
    auto it = by_app_.find(app_id);
    return it == by_app_.end() ? nullptr : &it->second;
}

void Placements::remember(const std::string& app_id, const Placement& p) {
    if (app_id.empty() || p.width <= 0 || p.height <= 0)
        return;
    auto [it, added] = by_app_.try_emplace(app_id, p);
    if (!added) {
        if (it->second == p)
            return;
        it->second = p;
    }
    save();
}

void Placements::load() {
    std::ifstream in(file_);
    if (!in)
        return;
    const json doc = json::parse(in, nullptr, false);
    if (!doc.is_object())
        return;  // unreadable: start over rather than refuse to run
    for (const auto& [app, v] : doc.items()) {
        if (!v.is_object())
            continue;
        Placement p;
        p.output = v.value("output", "");
        p.x = v.value("x", 0);
        p.y = v.value("y", 0);
        p.width = v.value("width", 0);
        p.height = v.value("height", 0);
        p.maximized = v.value("maximized", false);
        p.snapped = v.value("snapped", 0u);
        if (p.width > 0 && p.height > 0)
            by_app_[app] = p;
    }
}

bool Placements::save() const {
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    json doc = json::object();
    for (const auto& [app, p] : by_app_)
        doc[app] = {{"output", p.output}, {"x", p.x}, {"y", p.y}, {"width", p.width},
                    {"height", p.height}, {"maximized", p.maximized}, {"snapped", p.snapped}};
    const fs::path tmp = file_.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
            return false;
        out << doc.dump(2) << '\n';
        if (!out)
            return false;
    }
    fs::rename(tmp, file_, ec);
    return !ec;
}

} // namespace atrium
