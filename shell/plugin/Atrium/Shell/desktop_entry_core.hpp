#pragma once
// Desktop entry files (freedesktop Desktop Entry Specification 1.5), parsed
// without Qt so the rules are unit-tested: groups, localized keys, escapes,
// lists, and Exec's quoting and field codes.

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atrium::desktop_entry {

struct Action {
    std::string id;
    std::string name;
    std::string icon;
    std::string exec;
};

struct Entry {
    std::string type;
    std::string name;
    std::string generic_name;
    std::string comment;
    std::string icon;
    std::string exec;
    std::string try_exec;
    std::string path;  // working directory
    std::string startup_wm_class;
    std::vector<std::string> keywords;
    std::vector<std::string> categories;
    std::vector<std::string> only_show_in;
    std::vector<std::string> not_show_in;
    bool no_display = false;
    bool hidden = false;
    bool terminal = false;
    std::vector<Action> actions;
};

// `locale` as in LC_MESSAGES ("de_DE.UTF-8@euro"); "" for the untranslated
// values. Nothing when the text has no [Desktop Entry] group.
std::optional<Entry> parse(std::string_view text, std::string_view locale);

// The value's escapes (\s \n \t \r \\) resolved.
std::string unescape(std::string_view value);
// A `;`-separated list (`\;` is a literal semicolon), without empty items.
std::vector<std::string> split_list(std::string_view value);

// Exec as argv: quoted arguments unquoted, field codes expanded (%i to
// --icon ICON, %c to the name, %k to the file) or dropped (%f %F %u %U and
// the deprecated ones), %% to %. Empty when the line is malformed.
std::vector<std::string> exec_argv(std::string_view exec, std::string_view name = {},
                                   std::string_view icon = {}, std::string_view file = {});

// Shown on this desktop, given XDG_CURRENT_DESKTOP (colon-separated).
bool shown_in(const Entry& e, std::string_view current_desktops);

} // namespace atrium::desktop_entry
