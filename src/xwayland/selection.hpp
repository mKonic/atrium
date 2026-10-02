#pragma once
// The XWM's side of the clipboard, the primary selection and drag and drop
// (wlroots' xwayland/selection, MIT).
#include "xwayland/xwm.hpp"

#include <list>

namespace atrium::xwayland {

constexpr size_t kIncrChunkSize = 64 * 1024;
constexpr uint32_t kXdndVersion = 5;

struct Selection;

// One conversion: X11 data to a Wayland client (incoming), or Wayland data
// to an X11 window (outgoing).
struct Transfer {
    explicit Transfer(Selection& s) : selection(s) {}
    ~Transfer();
    Transfer(const Transfer&) = delete;
    Transfer& operator=(const Transfer&) = delete;

    Selection& selection;
    bool incr = false;
    bool flush_property_on_delete = false;
    bool property_set = false;
    std::vector<uint8_t> source_data;
    int wl_client_fd = -1;
    wl_event_source* event_source = nullptr;

    // Outgoing: what the X11 window asked.
    xcb_selection_request_event_t request{};

    // Incoming: the property read, and the window it comes to.
    int property_start = 0;
    xcb_get_property_reply_t* property_reply = nullptr;
    xcb_window_t incoming_window = 0;

    void remove_event_source();
    void close_fd();
    void drop_property_reply();
};

struct Selection {
    Selection(Xwm& xwm, xcb_atom_t atom);
    ~Selection();

    Xwm& xwm;
    xcb_atom_t atom;
    xcb_window_t window = 0;
    xcb_window_t owner = 0;
    xcb_timestamp_t timestamp = 0;
    std::list<std::unique_ptr<Transfer>> incoming, outgoing;
    // The X11 owner's data, offered to Wayland (null: none, or a Wayland
    // client owns it).
    std::unique_ptr<wl::DataSource> x_source;

    void set_owner(bool set);
    Transfer* find_incoming(xcb_window_t window);
    void destroy_incoming(Transfer* t);
    void destroy_outgoing(Transfer* t);
    // The Wayland side's current source for this selection.
    wl::DataSource* wayland_source() const;
};

} // namespace atrium::xwayland
