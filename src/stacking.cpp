#include "stacking.hpp"

namespace atrium::stacking {

int in_front_of(std::span<const Window> mru, std::size_t i) {
    const Window& f = mru[i];
    for (std::size_t j = 0; j < i; ++j) {
        const Window& v = mru[j];
        if (v.shown && v.managed && v.output == f.output && v.space == f.space)
            return int(j);
    }
    return -1;
}

} // namespace atrium::stacking
