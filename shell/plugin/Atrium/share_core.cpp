#include "share_core.hpp"

namespace atrium::share {

std::vector<Source> parse(std::string_view input) {
    std::vector<Source> out;
    while (!input.empty()) {
        const size_t nl = input.find('\n');
        std::string_view line = input.substr(0, nl);
        input.remove_prefix(nl == std::string_view::npos ? input.size() : nl + 1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

        if (line.starts_with("Monitor: ")) {
            std::string_view rest = line.substr(9);
            const size_t space = rest.find(' ');
            if (rest.empty() || space == 0)
                continue;
            out.push_back({Source::Kind::Screen, std::string(line), std::string(rest.substr(0, space)),
                           space == std::string_view::npos ? "" : std::string(rest.substr(space + 1))});
        } else if (line.starts_with("Window: ") && line.ends_with(')')) {
            // The title may hold parentheses; the identifier is the last pair.
            std::string_view rest = line.substr(8, line.size() - 9);
            const size_t open = rest.rfind('(');
            if (open == std::string_view::npos || open + 1 == rest.size())
                continue;
            std::string_view title = rest.substr(0, open);
            if (title.ends_with(' '))
                title.remove_suffix(1);
            out.push_back({Source::Kind::Window, std::string(line), std::string(rest.substr(open + 1)),
                           std::string(title)});
        }
    }
    return out;
}

} // namespace atrium::share
