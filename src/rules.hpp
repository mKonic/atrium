#pragma once
#include <nlohmann/json.hpp>

#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace atrium {

// "Windows of this app go there, like this." Matched once, when a window opens.
//
//   {"app_id": "discord|vesktop", "secret": "communication"}
//   {"app_id": "^vesktop$", "secret": "communication", "launch": "vesktop"}
//   {"app_id": "^firefox$", "title": "Picture-in-Picture", "space": 2}
//   {"app_id": "mpv", "fullscreen": true}
//
// app_id and title are case-insensitive regular expressions matched anywhere
// in the text; a rule needs at least one of them.
struct WindowRule {
    std::string app_id_text, title_text;
    std::optional<std::regex> app_id, title;
    int space = 0;           // 0: wherever the window would open anyway
    std::string secret;      // non-empty: into this secret space
    std::string launch;      // with secret: started when the space is shown and the app isn't running
    std::optional<bool> maximized, fullscreen;

    bool matches(const std::string& app_id, const std::string& title) const;
};

std::vector<WindowRule> parse_rules(const nlohmann::json& rules, std::vector<std::string>* errors = nullptr);
nlohmann::json default_rules();

// First matching rule per field wins, like a stylesheet read top to bottom.
struct RuleResult {
    int space = 0;
    std::string secret;
    std::optional<bool> maximized, fullscreen;
};
RuleResult apply_rules(const std::vector<WindowRule>& rules, const std::string& app_id,
                       const std::string& title);

} // namespace atrium
