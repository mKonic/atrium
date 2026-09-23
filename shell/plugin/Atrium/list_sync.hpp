#pragma once
// Turn a list into another with the fewest inserts, moves and removals, so a
// view keeps the delegates of items that stay (and their animations).
// `ops` wraps each change: ops.insert(i, f), ops.move(from, to, f),
// ops.remove(i, f) and ops.change(i, f) must call f() to make the change,
// between whatever they announce before and after it.

#include <algorithm>
#include <cstddef>
#include <vector>

namespace atrium {

template <class T, class Key, class Ops>
void sync_list(std::vector<T>& cur, const std::vector<T>& next, Key key, Ops& ops) {
    for (size_t i = 0; i < next.size(); ++i) {
        const auto k = key(next[i]);
        if (i >= cur.size() || key(cur[i]) != k) {
            auto it = std::find_if(cur.begin() + std::ptrdiff_t(std::min(i, cur.size())), cur.end(),
                                   [&](const T& t) { return key(t) == k; });
            if (it == cur.end()) {
                ops.insert(int(i), [&] { cur.insert(cur.begin() + std::ptrdiff_t(i), next[i]); });
                continue;
            }
            const size_t from = size_t(it - cur.begin());
            ops.move(int(from), int(i), [&] {
                std::rotate(cur.begin() + std::ptrdiff_t(i), cur.begin() + std::ptrdiff_t(from),
                            cur.begin() + std::ptrdiff_t(from) + 1);
            });
        }
        if (!(cur[i] == next[i]))
            ops.change(int(i), [&] { cur[i] = next[i]; });
    }
    while (cur.size() > next.size()) {
        const int last = int(cur.size()) - 1;
        ops.remove(last, [&] { cur.pop_back(); });
    }
}

} // namespace atrium
