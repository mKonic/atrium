#include "i18n.hpp"
#include "paths.hpp"

#include <clocale>

namespace atrium {

void i18n_init() {
    std::setlocale(LC_MESSAGES, "");
    bindtextdomain("atrium", ATRIUM_LOCALEDIR);
    bind_textdomain_codeset("atrium", "UTF-8");
}

} // namespace atrium
