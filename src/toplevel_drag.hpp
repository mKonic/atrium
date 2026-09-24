#pragma once
#include "listener.hpp"

#include <vector>

namespace atrium {

class Server;
class View;

// xdg-toplevel-drag-v1: a window that rides a drag and drop, the way a
// browser tab torn out of its window drags its new window along until it is
// dropped (or docked back into a window, which the app decides).
class ToplevelDrags {
public:
    explicit ToplevelDrags(Server& server);
    ~ToplevelDrags();
    ToplevelDrags(const ToplevelDrags&) = delete;
    ToplevelDrags& operator=(const ToplevelDrags&) = delete;

    // The window riding the drag in progress, if any. It is never the drop
    // target: the pointer looks through it.
    View* dragged() const;
    // The pointer moved during a drag: the window follows it.
    void motion(double lx, double ly);
    // Where a window attached to the drag appears when it maps: under the
    // pointer, at the offset the app asked for. False if it isn't attached.
    bool place(View* view);

private:
    struct Drag {
        ToplevelDrags* owner;
        wl_resource* resource;
        wlr_data_source* source;
        wlr_xdg_toplevel* toplevel = nullptr;
        int dx = 0, dy = 0;  // pointer in the window's geometry
        Listener<> source_destroy, toplevel_unmap, toplevel_destroy;
    };

    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void get_drag(wl_client* client, wl_resource* manager, uint32_t id, wl_resource* source);
    static void attach(wl_client* client, wl_resource* resource, wl_resource* toplevel, int32_t dx, int32_t dy);
    static void destroy_resource(wl_client* client, wl_resource* resource);
    static void drag_gone(wl_resource* resource);
    static void manager_gone(wl_resource* resource);
    Drag* current() const;
    View* view_of(const Drag& drag) const;
    void detach(Drag& drag);

    Server& server_;
    wl_global* global_ = nullptr;
    std::vector<wl_resource*> managers_;
    std::vector<Drag*> drags_;
};

} // namespace atrium
