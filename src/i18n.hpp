#pragma once
// The compositor's few words of its own (notices, the not-responding
// dialog) through gettext, domain "atrium" (translations/atrium.pot): the
// shell translates everything else with Qt.
#include <libintl.h>

#include <format>
#include <string>

namespace atrium {

// Messages only (LC_MESSAGES): numbers keep the C locale the config and JSON
// parsing rely on.
void i18n_init();

inline const char* tr(const char* text) {
    return dgettext("atrium", text);
}

// A translated message with "{}" for each argument, in its order.
template <typename... Args>
std::string trf(const char* text, const Args&... args) {
    return std::vformat(tr(text), std::make_format_args(args...));
}

} // namespace atrium
