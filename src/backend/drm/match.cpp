#include "backend/drm/match.hpp"

#include <algorithm>
#include <cstddef>

namespace atrium::backend::drm {

namespace {

struct State {
    const std::vector<uint32_t>& conns;
    const std::vector<uint32_t>& orig;
    std::vector<uint32_t> res;
    std::vector<uint32_t> best;
    size_t score = 0;
    size_t replaced = SIZE_MAX;
    bool exit_early = false;

    bool taken(size_t upto, uint32_t conn) const {
        return std::find(res.begin(), res.begin() + long(upto), conn) != res.begin() + long(upto);
    }

    // CRTC `k` onward, with `score` connectors matched and `replaced`
    // changes so far. True if a new best was found.
    bool step(size_t score_so_far, size_t replaced_so_far, size_t k) {
        const size_t crtcs = orig.size();
        if (k >= crtcs) {
            if (score_so_far > score || (score_so_far == score && replaced_so_far < replaced)) {
                score = score_so_far;
                replaced = replaced_so_far;
                best = res;
                exit_early = (score == crtcs || score == conns.size()) && replaced == 0;
                return true;
            }
            return false;
        }
        bool found = false;
        // Where it was, first.
        if (orig[k] != kUnmatched && !taken(k, orig[k])) {
            res[k] = orig[k];
            if (step(score_so_far + (conns[orig[k]] != 0 ? 1 : 0), replaced_so_far, k + 1))
                found = true;
        }
        if (exit_early)
            return true;
        if (orig[k] != kUnmatched)
            ++replaced_so_far;
        for (uint32_t c = 0; c < conns.size(); ++c) {
            if (c == orig[k] || !(conns[c] & (1u << k)) || taken(k, c))
                continue;
            res[k] = c;
            if (step(score_so_far + 1, replaced_so_far, k + 1))
                found = true;
            if (exit_early)
                return true;
        }
        // Or none.
        res[k] = kUnmatched;
        if (step(score_so_far, replaced_so_far, k + 1))
            found = true;
        return found;
    }
};

} // namespace

std::vector<uint32_t> match_crtcs(const std::vector<uint32_t>& possible, const std::vector<uint32_t>& previous) {
    State st{possible, previous, std::vector<uint32_t>(previous.size(), kUnmatched),
             std::vector<uint32_t>(previous.size(), kUnmatched)};
    st.step(0, 0, 0);
    return st.best;
}

} // namespace atrium::backend::drm
