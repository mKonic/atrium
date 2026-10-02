#include "util/format_set.hpp"

#include <algorithm>

namespace atrium {

bool DrmFormat::has(uint64_t modifier) const { return std::ranges::find(modifiers, modifier) != modifiers.end(); }

bool FormatSet::add(uint32_t format, uint64_t modifier) {
    auto it = std::ranges::find(formats_, format, &DrmFormat::format);
    if (it == formats_.end()) {
        formats_.push_back({format, {modifier}});
        return true;
    }
    if (it->has(modifier))
        return false;
    it->modifiers.push_back(modifier);
    return true;
}

const DrmFormat* FormatSet::get(uint32_t format) const {
    auto it = std::ranges::find(formats_, format, &DrmFormat::format);
    return it == formats_.end() ? nullptr : &*it;
}

bool FormatSet::has(uint32_t format, uint64_t modifier) const {
    const DrmFormat* f = get(format);
    return f && f->has(modifier);
}

} // namespace atrium
