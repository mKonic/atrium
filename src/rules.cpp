#include "rules.hpp"

namespace atrium {

using json = nlohmann::json;

bool WindowRule::matches(const std::string& a, const std::string& t) const {
    if (app_id && !std::regex_search(a, *app_id))
        return false;
    if (title && !std::regex_search(t, *title))
        return false;
    return app_id || title;
}

std::vector<WindowRule> parse_rules(const json& rules, std::vector<std::string>* errors) {
    std::vector<WindowRule> out;
    auto fail = [&](const std::string& why) {
        if (errors)
            errors->push_back(why);
    };
    if (!rules.is_array()) {
        fail("window rules must be a list");
        return out;
    }
    for (const json& r : rules) {
        if (!r.is_object()) {
            fail("each window rule is an object");
            continue;
        }
        WindowRule rule;
        bool ok = true;
        auto regex = [&](const char* key, std::string& text, std::optional<std::regex>& re) {
            if (!r.contains(key))
                return;
            if (!r[key].is_string()) {
                fail(std::string(key) + " is text");
                ok = false;
                return;
            }
            text = r[key];
            try {
                re.emplace(text, std::regex::ECMAScript | std::regex::icase | std::regex::optimize);
            } catch (const std::regex_error&) {
                fail(std::string(key) + ": '" + text + "' isn't a valid pattern");
                ok = false;
            }
        };
        regex("app_id", rule.app_id_text, rule.app_id);
        regex("title", rule.title_text, rule.title);
        if (ok && !rule.app_id && !rule.title) {
            fail("a window rule needs app_id or title");
            ok = false;
        }
        if (r.contains("space")) {
            if (!r["space"].is_number_integer() || r["space"].get<int>() < 1 || r["space"].get<int>() > 99) {
                fail("space is a number from 1 to 99");
                ok = false;
            } else {
                rule.space = r["space"];
            }
        }
        if (r.contains("secret")) {
            if (!r["secret"].is_string() || r["secret"].get<std::string>().empty()) {
                fail("secret is the name of a secret space");
                ok = false;
            } else {
                rule.secret = r["secret"];
            }
        }
        if (r.contains("launch")) {
            if (!r["launch"].is_string()) {
                fail("launch is the command that starts the app");
                ok = false;
            } else if (rule.secret.empty()) {
                fail("launch goes with a secret space: it starts the app when that space is shown");
                ok = false;
            } else {
                rule.launch = r["launch"];
            }
        }
        for (auto [key, field] : {std::pair{"maximized", &rule.maximized}, std::pair{"fullscreen", &rule.fullscreen}}) {
            if (!r.contains(key))
                continue;
            if (!r[key].is_boolean()) {
                fail(std::string(key) + " is true or false");
                ok = false;
            } else {
                *field = r[key].get<bool>();
            }
        }
        if (ok)
            out.push_back(std::move(rule));
    }
    return out;
}

json default_rules() {
    // Chat apps live in their own secret space, called up with one key, and
    // Discord starts when it isn't running, as caelestia's toggle does.
    return json::array({
        {{"app_id", "discord"}, {"secret", "communication"}, {"launch", "discord"}},
        {{"app_id", "equibop|vesktop|whatsapp|zapzap"}, {"secret", "communication"}},
    });
}

RuleResult apply_rules(const std::vector<WindowRule>& rules, const std::string& app_id, const std::string& title) {
    RuleResult r;
    bool placed = false;
    for (const WindowRule& rule : rules) {
        if (!rule.matches(app_id, title))
            continue;
        if (!placed && (rule.space || !rule.secret.empty())) {
            r.space = rule.space;
            r.secret = rule.secret;
            placed = true;
        }
        if (!r.maximized && rule.maximized)
            r.maximized = rule.maximized;
        if (!r.fullscreen && rule.fullscreen)
            r.fullscreen = rule.fullscreen;
    }
    return r;
}

} // namespace atrium
