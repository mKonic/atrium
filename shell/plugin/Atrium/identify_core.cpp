#include "identify_core.hpp"

#include <algorithm>
#include <cmath>
#include <strings.h>

namespace atrium::identify {

namespace {

std::string name_of(const Screen& s, const std::vector<Screen>& all) {
    if (s.built_in)
        return "Built-in Screen";
    const bool twin = std::ranges::any_of(all, [&s](const Screen& o) {
        return &o != &s && o.make == s.make && o.model == s.model;
    });
    std::string out;
    auto add = [&out](const std::string& part) {
        if (part.empty())
            return;
        if (!out.empty())
            out += '\n';
        out += part;
    };
    add(s.make);
    add(s.model);
    if (twin)
        add(s.serial);
    add(s.connector);
    return out;
}

} // namespace

std::vector<Label> labels(const std::vector<Screen>& screens) {
    std::vector<const Screen*> sorted;
    for (const Screen& s : screens)
        sorted.push_back(&s);
    std::ranges::sort(sorted, [](const Screen* a, const Screen* b) {
        return strcasecmp(a->connector.c_str(), b->connector.c_str()) < 0;
    });
    std::vector<Label> out;
    for (const Screen& s : screens) {
        Label l;
        l.number = int(std::ranges::find(sorted, &s) - sorted.begin()) + 1;
        l.name = name_of(s, screens);
        l.mode = std::to_string(s.width) + "x" + std::to_string(s.height);
        if (s.scale != 1)
            l.mode += "@" + std::to_string(int(std::lround(s.scale * 100))) + "%";
        out.push_back(std::move(l));
    }
    return out;
}

} // namespace atrium::identify
