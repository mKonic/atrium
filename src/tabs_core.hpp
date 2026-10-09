#pragma once
// Window tabs, as a Mac's: windows sharing one frame, the bar above showing a
// tab for each, one shown at a time. The bookkeeping is Hyprland's group
// (src/desktop/view/Group.cpp): its order, the current one, what becomes
// current when one leaves.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace atrium::tabs {

template <class T>
struct List {
    std::vector<T> items;
    size_t current = 0;

    size_t size() const { return items.size(); }
    const T& now() const { return items.at(current); }

    std::optional<size_t> index_of(const T& item) const {
        const auto it = std::ranges::find(items, item);
        if (it == items.end())
            return std::nullopt;
        return size_t(it - items.begin());
    }

    // In after the current one, which it then is (group:insert_after_current);
    // at `index` when given (dropped there on the bar).
    void add(T item, std::optional<size_t> index = std::nullopt) {
        if (items.empty()) {
            items.push_back(std::move(item));
            current = 0;
            return;
        }
        current = index ? std::min(*index, items.size()) : current + 1;
        items.insert(items.begin() + current, std::move(item));
    }

    // Out of the list: the one before it is current then, or the first when
    // the first one leaves (Hyprland's CGroup::remove).
    bool remove(const T& item) {
        const auto idx = index_of(item);
        if (!idx)
            return false;
        if ((current >= *idx && *idx != 0) || (current >= items.size() - 1 && current > 0))
            --current;
        items.erase(items.begin() + *idx);
        if (current >= items.size())
            current = items.empty() ? 0 : items.size() - 1;
        return true;
    }

    // Dragged along the bar; the current one stays current.
    void move(size_t from, size_t to) {
        if (from >= items.size() || to >= items.size() || from == to)
            return;
        const T shown = now();
        T item = std::move(items[from]);
        items.erase(items.begin() + from);
        items.insert(items.begin() + to, std::move(item));
        current = *index_of(shown);
    }

    // The next or previous one, round the end.
    size_t step(bool next) const {
        if (items.empty())
            return 0;
        return next ? (current + 1) % items.size() : (current + items.size() - 1) % items.size();
    }
};

// --- the bar ----------------------------------------------------------------------------------

// The tabs share the bar's width evenly.
inline double tab_left(size_t i, double width, size_t n) {
    return n ? width * double(i) / double(n) : 0;
}

// The tab under x, or nothing off the bar.
inline std::optional<size_t> tab_at(double x, double width, size_t n) {
    if (!n || x < 0 || x >= width)
        return std::nullopt;
    return std::min(size_t(x * double(n) / width), n - 1);
}

// The close button: a circle at the tab's left, as on a Mac.
constexpr double kCloseSize = 16;
constexpr double kCloseInset = 6;
inline bool on_close(double x, double y, size_t i, double width, size_t n, double height) {
    const double cx = tab_left(i, width, n) + kCloseInset + kCloseSize / 2, cy = height / 2;
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= (kCloseSize / 2) * (kCloseSize / 2);
}

// Where a tab dropped at x goes in: before the tab whose middle is past it.
inline size_t drop_slot(double x, double width, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (x < tab_left(i, width, n) + width / double(n) / 2)
            return i;
    return n;
}

// --- new windows ------------------------------------------------------------------------------

// System Settings > Desktop & Dock > "Prefer tabs when opening documents".
enum class Prefer { Never, Fullscreen, Always };

// A new window of the app in front opens as its tab.
inline bool opens_as_tab(Prefer prefer, bool same_app, bool front_fullscreen) {
    if (!same_app)
        return false;
    switch (prefer) {
    case Prefer::Always: return true;
    case Prefer::Fullscreen: return front_fullscreen;
    case Prefer::Never: return false;
    }
    return false;
}

} // namespace atrium::tabs
