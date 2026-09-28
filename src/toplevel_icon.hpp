#pragma once

#include "wl/resource.hpp"

#include <memory>
#include <vector>

namespace atrium {

class Server;
namespace wl {
class XdgToplevelIconManagerV1;
class XdgToplevelIconV1;
}

// xdg-toplevel-icon-v1, atrium's own: wlroots' sends wl_buffer.release when
// an app adds a second picture of the same size, which the protocol says is
// never sent. GTK destroys a buffer on release and again with the icon, and
// crashes; with every window of the app, when it is one process (Ghostty).
// Here a picture stays held until the app destroys its buffer, so release
// never goes out.
class ToplevelIcons {
public:
    explicit ToplevelIcons(Server& server);
    ~ToplevelIcons();
    ToplevelIcons(const ToplevelIcons&) = delete;
    ToplevelIcons& operator=(const ToplevelIcons&) = delete;

private:
    void set_icon(wl_resource* toplevel, wl::XdgToplevelIconV1* icon);

    Server& server_;
    std::unique_ptr<wl::Global> global_;
    std::vector<wl::Weak<wl::XdgToplevelIconManagerV1>> managers_;
};

} // namespace atrium
