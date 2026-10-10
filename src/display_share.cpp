#include "display_share.hpp"

#include <algorithm>

namespace atrium {

using nlohmann::json;

json displays_to_json(const std::vector<DisplayRecord>& displays) {
    json out = json::array();
    for (const DisplayRecord& d : displays) {
        json j{{"id", d.id},
               {"enabled", d.enabled},
               {"width", d.width},
               {"height", d.height},
               {"refresh", d.refresh},
               {"scale", d.scale},
               {"transform", d.transform},
               {"adaptive_sync", d.adaptive_sync},
               {"hdr", d.hdr},
               {"sdr_brightness", d.sdr_brightness},
               {"sdr_color", d.sdr_color}};
        if (d.x && d.y) {
            j["x"] = *d.x;
            j["y"] = *d.y;
        }
        // Colour profiles are paths in the user's home: the login screen
        // can't read them.
        out.push_back(std::move(j));
    }
    return out;
}

std::vector<DisplayRecord> displays_from_json(const json& j) {
    std::vector<DisplayRecord> out;
    if (!j.is_array())
        return out;
    for (const json& e : j) {
        if (!e.is_object() || !e.contains("id") || !e["id"].is_string() || e["id"].get<std::string>().empty())
            continue;
        auto num = [&](const char* key, int def, int lo, int hi) {
            return e.contains(key) && e[key].is_number_integer() ? std::clamp(e[key].get<int>(), lo, hi) : def;
        };
        auto flag = [&](const char* key, bool def) {
            return e.contains(key) && e[key].is_boolean() ? e[key].get<bool>() : def;
        };
        DisplayRecord d;
        d.id = e["id"];
        d.enabled = flag("enabled", true);
        d.width = num("width", 0, 0, 16384);
        d.height = num("height", 0, 0, 16384);
        d.refresh = num("refresh", 0, 0, 1000000);
        d.scale = e.contains("scale") && e["scale"].is_number() ? std::clamp(e["scale"].get<double>(), 0.25, 8.0) : 1.0;
        d.transform = num("transform", 0, 0, 7);
        if (e.contains("x") && e["x"].is_number_integer() && e.contains("y") && e["y"].is_number_integer()) {
            d.x = e["x"].get<int>();
            d.y = e["y"].get<int>();
        }
        const std::string vrr =
            e.contains("adaptive_sync") && e["adaptive_sync"].is_string() ? e["adaptive_sync"].get<std::string>() : "";
        d.adaptive_sync = vrr == "off" || vrr == "on" ? vrr : "games";
        d.hdr = flag("hdr", false);
        d.sdr_brightness = num("sdr_brightness", 30, 0, 100);
        d.sdr_color = num("sdr_color", 100, 0, 100);
        out.push_back(std::move(d));
    }
    return out;
}

std::string displays_line(const std::vector<DisplayRecord>& displays) {
    return "displays " + displays_to_json(displays).dump() + "\n";
}

} // namespace atrium
