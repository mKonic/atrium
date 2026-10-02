#pragma once
#include "listener.hpp"
#include "wl/data_device.hpp"
#include "wl/xdg_shell.hpp"

#include <memory>
#include <vector>

namespace atrium {

class Server;
class View;
namespace wl {
class XdgToplevelDragManagerV1;
class XdgToplevelDragV1;
}

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
        wl::Weak<wl::XdgToplevelDragV1> resource;
        wl::DataSource* source;
        wl::Toplevel* toplevel = nullptr;
        int dx = 0, dy = 0;  // pointer in the window's geometry
        wl::Connection source_destroy, toplevel_unmap, toplevel_destroy;
    };

    void get_drag(wl::XdgToplevelDragManagerV1* manager, uint32_t id, wl_resource* source);
    void attach(Drag* drag, wl::XdgToplevelDragV1* resource, wl_resource* toplevel, int32_t dx, int32_t dy);
    Drag* current() const;
    View* view_of(const Drag& drag) const;
    void detach(Drag& drag);

    Server& server_;
    std::unique_ptr<wl::Global> global_;
    std::vector<wl::Weak<wl::XdgToplevelDragManagerV1>> managers_;
    std::vector<Drag*> drags_;
};

} // namespace atrium
