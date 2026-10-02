#pragma once
// Where the screens sit in one coordinate space: each at a position of its
// own or placed automatically to the right of the others, sized by its
// effective (scaled, transformed) resolution. After wlroots'
// wlr_output_layout (MIT), whose placement rules it keeps.
#include "backend/output.hpp"

#include <memory>
#include <vector>

namespace atrium {

class OutputLayout {
public:
    OutputLayout() = default;
    ~OutputLayout();
    OutputLayout(const OutputLayout&) = delete;
    OutputLayout& operator=(const OutputLayout&) = delete;

    // At (x, y), or moved there if already in.
    void add(backend::Output* o, int x, int y);
    // To the right of everything placed by position.
    void add_auto(backend::Output* o);
    void remove(backend::Output* o);
    bool contains(const backend::Output* o) const;

    // Its box in the layout; empty if not in it.
    Box box(const backend::Output* o) const;
    // The box around all of them.
    Box extents() const;
    backend::Output* output_at(double lx, double ly) const;
    // The point nearest (lx, ly) on `reference`, or on any screen if null.
    void closest_point(const backend::Output* reference, double lx, double ly, double* cx, double* cy) const;
    bool empty() const { return entries_.empty(); }
    std::vector<backend::Output*> outputs() const;

    // Any box moved or resized, or a screen came or went.
    wl::Signal<> change;

private:
    struct Entry {
        backend::Output* output = nullptr;
        bool automatic = false;
        int x = 0, y = 0;  // the size is the output's own, read when asked
        wl::Connection commit, destroy;
    };
    Entry* find(const backend::Output* o);
    const Entry* find(const backend::Output* o) const;
    Entry& insert(backend::Output* o);
    void reconfigure();

    std::vector<std::unique_ptr<Entry>> entries_;
};

} // namespace atrium
