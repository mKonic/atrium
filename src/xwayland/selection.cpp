// Selections and drag and drop between X11 and Wayland, ported from
// wlroots' xwayland/selection/{selection,incoming,outgoing,dnd}.c (MIT).
#ifdef ATRIUM_XWAYLAND
#include "xwayland/selection.hpp"

#include "wl/selection.hpp"
#include "xwayland/atoms.hpp"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include <wlr/util/log.h>
}

namespace atrium::xwayland {

namespace {

void send_event(xcb_connection_t* c, xcb_window_t dest, uint32_t mask, const void* event, size_t length) {
    char buf[32] = {};
    std::memcpy(buf, event, std::min<size_t>(length, 32));
    xcb_send_event(c, 0, dest, mask, buf);
}

} // namespace

// ---- Transfer -----------------------------------------------------------------------

void Transfer::remove_event_source() {
    if (event_source) {
        wl_event_source_remove(event_source);
        event_source = nullptr;
    }
}

void Transfer::close_fd() {
    if (wl_client_fd >= 0) {
        close(wl_client_fd);
        wl_client_fd = -1;
    }
}

void Transfer::drop_property_reply() {
    free(property_reply);
    property_reply = nullptr;
}

Transfer::~Transfer() {
    drop_property_reply();
    remove_event_source();
    close_fd();
    if (incoming_window) {
        xcb_destroy_window(selection.xwm.conn_, incoming_window);
        selection.xwm.schedule_flush();
    }
}

// ---- the X11 owner's data, as a Wayland source ---------------------------------------

class XDataSource : public wl::DataSource {
public:
    XDataSource(Selection& s, std::vector<std::string> mimes, std::vector<xcb_atom_t> atoms)
        : selection_(s), atoms_(std::move(atoms)) {
        mime_types_ = std::move(mimes);
    }
    void send(const std::string& mime, int fd) override;

private:
    Selection& selection_;
    std::vector<xcb_atom_t> atoms_;
};

// ---- Selection ------------------------------------------------------------------------

Selection::Selection(Xwm& w, xcb_atom_t a) : xwm(w), atom(a) {
    xcb_connection_t* c = xwm.conn_;
    window = xcb_generate_id(c);
    const uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
    if (atom == xwm.atom(DND_SELECTION)) {
        xcb_create_window(c, XCB_COPY_FROM_PARENT, window, xwm.screen_->root, 0, 0, 8192, 8192, 0,
                          XCB_WINDOW_CLASS_INPUT_ONLY, xwm.screen_->root_visual, XCB_CW_EVENT_MASK, &mask);
        const uint32_t version = kXdndVersion;
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, xwm.atom(DND_AWARE), XCB_ATOM_ATOM, 32, 1, &version);
    } else {
        xcb_create_window(c, XCB_COPY_FROM_PARENT, window, xwm.screen_->root, 0, 0, 10, 10, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, xwm.screen_->root_visual, XCB_CW_EVENT_MASK, &mask);
        if (atom == xwm.atom(CLIPBOARD))
            xcb_set_selection_owner(c, window, xwm.atom(CLIPBOARD_MANAGER), XCB_TIME_CURRENT_TIME);
    }
    xcb_xfixes_select_selection_input(c, window, atom,
                                      XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
                                          XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
                                          XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);
}

Selection::~Selection() {
    outgoing.clear();
    incoming.clear();
    // The X11 owner's data can't be fetched any more.
    x_source.reset();
    xcb_destroy_window(xwm.conn_, window);
}

void Selection::set_owner(bool set) {
    if (set) {
        xcb_set_selection_owner(xwm.conn_, window, atom, XCB_TIME_CURRENT_TIME);
        xwm.schedule_flush();
    } else if (owner == window) {
        xcb_set_selection_owner(xwm.conn_, XCB_WINDOW_NONE, atom, timestamp);
        xwm.schedule_flush();
    }
}

Transfer* Selection::find_incoming(xcb_window_t w) {
    for (auto& t : incoming)
        if (t->incoming_window == w)
            return t.get();
    return nullptr;
}

void Selection::destroy_incoming(Transfer* t) {
    incoming.remove_if([t](const auto& p) { return p.get() == t; });
}

void Selection::destroy_outgoing(Transfer* t) {
    outgoing.remove_if([t](const auto& p) { return p.get() == t; });
}

wl::DataSource* Selection::wayland_source() const {
    if (this == xwm.clipboard_.get())
        return xwm.data_ ? xwm.data_->selection() : nullptr;
    if (this == xwm.primary_selection_.get())
        return xwm.primary_ ? xwm.primary_->slot().get() : nullptr;
    if (this == xwm.dnd_.get())
        return xwm.drag_ ? xwm.drag_->source() : xwm.drop_source_;
    return nullptr;
}

// ---- incoming: X11 data to Wayland -----------------------------------------------------

namespace {

bool get_incoming_property(Transfer* t, bool erase) {
    Xwm& xwm = t->selection.xwm;
    xcb_get_property_cookie_t cookie = xcb_get_property(xwm.connection(), erase, t->incoming_window,
                                                        xwm.atom(WL_SELECTION), XCB_GET_PROPERTY_TYPE_ANY, 0,
                                                        0x1fffffff);
    t->property_start = 0;
    t->property_reply = xcb_get_property_reply(xwm.connection(), cookie, nullptr);
    return t->property_reply != nullptr;
}

void ready_for_next_incr_chunk(Transfer* t) {
    Xwm& xwm = t->selection.xwm;
    xcb_delete_property(xwm.connection(), t->incoming_window, xwm.atom(WL_SELECTION));
    xcb_flush(xwm.connection());
    t->remove_event_source();
    t->drop_property_reply();
}

// Writes the property to the Wayland client: nonzero if it may take more
// later (the pipe was full).
int write_property(int fd, uint32_t, void* data) {
    auto* t = static_cast<Transfer*>(data);
    const auto* property = static_cast<const char*>(xcb_get_property_value(t->property_reply));
    const int remainder = xcb_get_property_value_length(t->property_reply) - t->property_start;
    const ssize_t len = write(fd, property + t->property_start, size_t(remainder));
    if (len == -1) {
        t->selection.destroy_incoming(t);
        return 0;
    }
    if (len < remainder) {
        t->property_start += int(len);
        return 1;
    }
    if (t->incr)
        ready_for_next_incr_chunk(t);
    else
        t->selection.destroy_incoming(t);
    return 0;
}

void write_property_to_client(Transfer* t) {
    if (t->incr && t->wl_client_fd < 0) {
        // The client closed its pipe early: keep draining the X11 side.
        ready_for_next_incr_chunk(t);
        return;
    }
    if (write_property(t->wl_client_fd, WL_EVENT_WRITABLE, t)) {
        wl_event_loop* loop = wl_display_get_event_loop(t->selection.xwm.display());
        t->event_source = wl_event_loop_add_fd(loop, t->wl_client_fd, WL_EVENT_WRITABLE, write_property, t);
    }
}

void get_incr_chunk(Transfer* t) {
    if (t->property_reply)
        return;  // a new property before the last was deleted
    if (!get_incoming_property(t, false))
        return;
    if (xcb_get_property_value_length(t->property_reply) > 0)
        write_property_to_client(t);
    else
        t->selection.destroy_incoming(t);  // the end
}

void get_data(Transfer* t) {
    if (!get_incoming_property(t, true))
        return;
    if (t->property_reply->type == t->selection.xwm.atom(INCR)) {
        t->incr = true;
        t->drop_property_reply();
    } else {
        write_property_to_client(t);
    }
}

} // namespace

void XDataSource::send(const std::string& mime, int fd) {
    Xwm& xwm = selection_.xwm;
    auto it = std::ranges::find(mime_types_, mime);
    if (it == mime_types_.end()) {
        close(fd);
        return;
    }
    const xcb_atom_t target = atoms_[size_t(it - mime_types_.begin())];
    auto t = std::make_unique<Transfer>(selection_);
    t->incoming_window = xcb_generate_id(xwm.conn_);
    const uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_create_window(xwm.conn_, XCB_COPY_FROM_PARENT, t->incoming_window, xwm.screen_->root, 0, 0, 10, 10, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, xwm.screen_->root_visual, XCB_CW_EVENT_MASK, &mask);
    xcb_convert_selection(xwm.conn_, t->incoming_window, selection_.atom, target, xwm.atom(WL_SELECTION),
                          XCB_TIME_CURRENT_TIME);
    xwm.schedule_flush();
    fcntl(fd, F_SETFL, O_WRONLY | O_NONBLOCK);
    t->wl_client_fd = fd;
    selection_.incoming.push_front(std::move(t));
}

namespace {

// Reads the X11 owner's TARGETS into MIME types.
bool get_targets(Selection& sel, std::vector<std::string>& mimes, std::vector<xcb_atom_t>& atoms) {
    Xwm& xwm = sel.xwm;
    xcb_connection_t* c = xwm.connection();
    xcb_get_property_reply_t* reply = xcb_get_property_reply(
        c, xcb_get_property(c, 1, sel.window, xwm.atom(WL_SELECTION), XCB_GET_PROPERTY_TYPE_ANY, 0, 4096), nullptr);
    if (!reply)
        return false;
    if (reply->type != XCB_ATOM_ATOM) {
        free(reply);
        return false;
    }
    const auto* value = static_cast<const xcb_atom_t*>(xcb_get_property_value(reply));
    const size_t n = size_t(xcb_get_property_value_length(reply)) / sizeof(xcb_atom_t);
    for (size_t i = 0; i < n; ++i) {
        std::optional<std::string> mime;
        if (value[i] == xwm.atom(UTF8_STRING)) {
            mime = "text/plain;charset=utf-8";
        } else if (value[i] == xwm.atom(TEXT)) {
            mime = "text/plain";
        } else if (value[i] != xwm.atom(TARGETS) && value[i] != xwm.atom(TIMESTAMP)) {
            // Only what looks like a MIME type.
            std::string name = xwm.atom_name(value[i]);
            if (name.find('/') != std::string::npos)
                mime = std::move(name);
        }
        if (mime) {
            mimes.push_back(std::move(*mime));
            atoms.push_back(value[i]);
        }
    }
    free(reply);
    return true;
}

} // namespace

// ---- outgoing: Wayland data to X11 ------------------------------------------------------

namespace {

void send_notify(Xwm& xwm, const xcb_selection_request_event_t& req, bool success) {
    xcb_selection_notify_event_t ev{};
    ev.response_type = XCB_SELECTION_NOTIFY;
    ev.time = req.time;
    ev.requestor = req.requestor;
    ev.selection = req.selection;
    ev.target = req.target;
    ev.property = success ? req.property : xcb_atom_t(XCB_ATOM_NONE);
    send_event(xwm.connection(), req.requestor, XCB_EVENT_MASK_NO_EVENT, &ev, sizeof(ev));
    xcb_flush(xwm.connection());
}

size_t flush_source_data(Transfer* t) {
    Xwm& xwm = t->selection.xwm;
    xcb_change_property(xwm.connection(), XCB_PROP_MODE_REPLACE, t->request.requestor, t->request.property,
                        t->request.target, 8, uint32_t(t->source_data.size()), t->source_data.data());
    xcb_flush(xwm.connection());
    t->property_set = true;
    const size_t length = t->source_data.size();
    t->source_data.clear();
    return length;
}

void start_outgoing(Transfer* t);

int read_source(int fd, uint32_t, void* data) {
    auto* t = static_cast<Transfer*>(data);
    Xwm& xwm = t->selection.xwm;
    const size_t current = t->source_data.size();
    if (t->source_data.size() < kIncrChunkSize)
        t->source_data.resize(current + kIncrChunkSize);
    else
        t->source_data.resize(std::max(t->source_data.capacity(), current));
    const size_t available = t->source_data.size() - current;
    const ssize_t len = read(fd, t->source_data.data() + current, available);
    if (len == -1) {
        if (errno == EAGAIN) {
            t->source_data.resize(current);
            return 1;
        }
        send_notify(xwm, t->request, false);
        t->selection.destroy_outgoing(t);
        return 0;
    }
    t->source_data.resize(current + size_t(len));
    if (t->source_data.size() >= kIncrChunkSize) {
        if (!t->incr) {
            // Too big for one property: INCR, a chunk at a time.
            const uint32_t chunk = kIncrChunkSize;
            xcb_change_property(xwm.connection(), XCB_PROP_MODE_REPLACE, t->request.requestor, t->request.property,
                                xwm.atom(INCR), 32, 1, &chunk);
            t->incr = true;
            t->property_set = true;
            t->flush_property_on_delete = true;
            t->remove_event_source();
            send_notify(xwm, t->request, true);
        } else if (t->property_set) {
            t->flush_property_on_delete = true;
            t->remove_event_source();
        } else {
            flush_source_data(t);
        }
    } else if (len == 0 && !t->incr) {
        flush_source_data(t);
        send_notify(xwm, t->request, true);
        t->selection.destroy_outgoing(t);
    } else if (len == 0 && t->incr) {
        t->flush_property_on_delete = true;
        if (!t->property_set)
            flush_source_data(t);
        t->remove_event_source();
        t->close_fd();
    }
    return 1;
}

void start_outgoing(Transfer* t) {
    wl_event_loop* loop = wl_display_get_event_loop(t->selection.xwm.display());
    t->event_source = wl_event_loop_add_fd(loop, t->wl_client_fd, WL_EVENT_READABLE, read_source, t);
}

void send_incr_chunk(Transfer* t) {
    t->property_set = false;
    if (!t->flush_property_on_delete)
        return;
    t->flush_property_on_delete = false;
    const size_t length = flush_source_data(t);
    if (t->wl_client_fd >= 0) {
        start_outgoing(t);
    } else if (length > 0) {
        // All read; an empty property after this chunk ends the transfer.
        t->flush_property_on_delete = true;
    } else {
        t->selection.destroy_outgoing(t);
    }
}

bool send_data(Selection& sel, const xcb_selection_request_event_t& req, const std::string& mime) {
    wl::DataSource* source = sel.wayland_source();
    if (!source || !source->offers(mime))
        return false;
    int p[2];
    if (pipe2(p, O_CLOEXEC | O_NONBLOCK) == -1)
        return false;
    auto t = std::make_unique<Transfer>(sel);
    t->request = req;
    t->wl_client_fd = p[0];
    source->send(mime, p[1]);
    // A requestor only reads its latest answer: older ones would hang.
    for (auto it = sel.outgoing.begin(); it != sel.outgoing.end();) {
        if ((*it)->request.requestor == req.requestor) {
            send_notify(sel.xwm, (*it)->request, false);
            it = sel.outgoing.erase(it);
        } else {
            ++it;
        }
    }
    Transfer* raw = t.get();
    sel.outgoing.push_front(std::move(t));
    start_outgoing(raw);
    return true;
}

} // namespace

// ---- Xwm's selection plumbing -------------------------------------------------------------

void Xwm::init_selections() {
    clipboard_ = std::make_unique<Selection>(*this, atoms_[CLIPBOARD]);
    primary_selection_ = std::make_unique<Selection>(*this, atoms_[PRIMARY]);
    dnd_ = std::make_unique<Selection>(*this, atoms_[DND_SELECTION]);
}

void Xwm::finish_selections() {
    clipboard_.reset();
    primary_selection_.reset();
    dnd_.reset();
}

bool Xwm::selection_window(xcb_window_t w) const {
    for (const Selection* s : {clipboard_.get(), primary_selection_.get(), dnd_.get()})
        if (s && s->window == w)
            return true;
    return false;
}

Selection* Xwm::selection_for(xcb_atom_t a) {
    if (a == atoms_[CLIPBOARD])
        return clipboard_.get();
    if (a == atoms_[PRIMARY])
        return primary_selection_.get();
    if (a == atoms_[DND_SELECTION])
        return dnd_.get();
    return nullptr;
}

xcb_atom_t Xwm::mime_to_atom(const std::string& mime) {
    if (mime == "text/plain;charset=utf-8")
        return atoms_[UTF8_STRING];
    if (mime == "text/plain")
        return atoms_[TEXT];
    xcb_intern_atom_reply_t* r =
        xcb_intern_atom_reply(conn_, xcb_intern_atom(conn_, 0, uint16_t(mime.size()), mime.c_str()), nullptr);
    if (!r)
        return XCB_ATOM_NONE;
    const xcb_atom_t a = r->atom;
    free(r);
    return a;
}

std::optional<std::string> Xwm::mime_from_atom(xcb_atom_t a) {
    if (a == atoms_[UTF8_STRING])
        return "text/plain;charset=utf-8";
    if (a == atoms_[TEXT])
        return "text/plain";
    std::string name = atom_name(a);
    if (name.empty())
        return std::nullopt;
    return name;
}

void Xwm::set_seat(wl::Seat* seat, wl::DataDevices* data, wl::PrimarySelection* primary) {
    clipboard_changed_.disconnect();
    primary_changed_.disconnect();
    drag_started_.disconnect();
    // What X11 offered goes with the old seat.
    for (Selection* s : {clipboard_.get(), primary_selection_.get()})
        if (s)
            s->x_source.reset();
    seat_ = seat;
    data_ = data;
    primary_ = primary;
    if (!seat)
        return;
    // A Wayland client's selection: atrium's window owns the X11 one, and
    // answers for it.
    if (data_) {
        clipboard_changed_ = data_->slot().changed.connect([this](wl::DataSource* source) {
            if (source && source == clipboard_->x_source.get())
                return;
            clipboard_->set_owner(source != nullptr);
        });
        clipboard_->set_owner(data_->selection() != nullptr);
        drag_started_ = data_->events.drag_started.connect([this](wl::Drag* drag) { start_drag(drag); });
    }
    if (primary_) {
        primary_changed_ = primary_->slot().changed.connect([this](wl::DataSource* source) {
            if (source && source == primary_selection_->x_source.get())
                return;
            primary_selection_->set_owner(source != nullptr);
        });
        primary_selection_->set_owner(primary_->slot().get() != nullptr);
    }
}

// The X11 owner's data becomes the Wayland selection (or none).
static void request_wayland_selection(Xwm& xwm, wl::Seat* seat, wl::DataDevices* data,
                                      wl::PrimarySelection* primary, bool clipboard, wl::DataSource* source) {
    if (clipboard && data) {
        if (data->events.request_selection.empty())
            data->set_selection(source);
        else
            data->events.request_selection.emit({source, seat->next_serial()});
    } else if (!clipboard && primary) {
        if (primary->request_selection.empty())
            primary->slot().set(source);
        else
            primary->request_selection.emit({source, seat->next_serial()});
    }
    (void)xwm;
}

bool Xwm::handle_selection_event(xcb_generic_event_t* ev) {
    if (!seat_)
        return false;
    switch (ev->response_type & 0x7f) {
    case XCB_SELECTION_NOTIFY: {
        auto* e = reinterpret_cast<xcb_selection_notify_event_t*>(ev);
        Selection* sel = selection_for(e->selection);
        if (!sel)
            return true;
        Transfer* t = sel->find_incoming(e->requestor);
        if (e->property == XCB_ATOM_NONE) {
            if (t)
                sel->destroy_incoming(t);
        } else if (e->target == atoms_[TARGETS]) {
            // Only a focused X11 window may set the clipboard.
            if (!focus_ || sel == dnd_.get())
                return true;
            std::vector<std::string> mimes;
            std::vector<xcb_atom_t> atoms;
            if (!get_targets(*sel, mimes, atoms))
                return true;
            auto source = std::make_unique<XDataSource>(*sel, std::move(mimes), std::move(atoms));
            wl::DataSource* raw = source.get();
            std::unique_ptr<wl::DataSource> old = std::exchange(sel->x_source, std::move(source));
            request_wayland_selection(*this, seat_, data_, primary_, sel == clipboard_.get(), raw);
        } else if (t) {
            get_data(t);
        }
        return true;
    }
    case XCB_PROPERTY_NOTIFY: {
        auto* e = reinterpret_cast<xcb_property_notify_event_t*>(ev);
        for (Selection* sel : {clipboard_.get(), primary_selection_.get(), dnd_.get()}) {
            if (e->state == XCB_PROPERTY_NEW_VALUE && e->atom == atoms_[WL_SELECTION]) {
                if (Transfer* t = sel->find_incoming(e->window)) {
                    if (t->incr)
                        get_incr_chunk(t);
                    return true;
                }
            }
            for (auto& out : sel->outgoing) {
                if (e->window == out->request.requestor) {
                    if (e->state == XCB_PROPERTY_DELETE && e->atom == out->request.property && out->incr)
                        send_incr_chunk(out.get());
                    return true;
                }
            }
        }
        return false;
    }
    case XCB_SELECTION_REQUEST: {
        auto* req = reinterpret_cast<xcb_selection_request_event_t*>(ev);
        if (req->selection == atoms_[CLIPBOARD_MANAGER]) {
            // The Wayland side already has the data.
            send_notify(*this, *req, true);
            return true;
        }
        Selection* sel = selection_for(req->selection);
        if (!sel || req->requestor == sel->window) {
            send_notify(*this, *req, false);
            return true;
        }
        if (sel->window != req->owner) {
            if (req->time != XCB_CURRENT_TIME && req->time < sel->timestamp)
                send_notify(*this, *req, false);
            // No longer the owner: not ours to answer.
            return true;
        }
        const bool dnd_allowed = sel == dnd_.get() && (drag_focus_ || drop_focus_);
        // Only a focused X11 window may read the clipboard.
        if (!focus_ && !dnd_allowed) {
            send_notify(*this, *req, false);
            return true;
        }
        if (req->target == atoms_[TARGETS]) {
            wl::DataSource* source = sel->wayland_source();
            if (!source) {
                send_notify(*this, *req, false);
                return true;
            }
            std::vector<xcb_atom_t> targets{atoms_[TIMESTAMP], atoms_[TARGETS]};
            for (const std::string& m : source->mime_types())
                targets.push_back(mime_to_atom(m));
            xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, req->requestor, req->property, XCB_ATOM_ATOM, 32,
                                uint32_t(targets.size()), targets.data());
            send_notify(*this, *req, true);
        } else if (req->target == atoms_[TIMESTAMP]) {
            xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, req->requestor, req->property, XCB_ATOM_INTEGER, 32, 1,
                                &sel->timestamp);
            send_notify(*this, *req, true);
        } else if (req->target == atoms_[DELETE]) {
            send_notify(*this, *req, true);
        } else {
            const std::optional<std::string> mime = mime_from_atom(req->target);
            if (!mime || !send_data(*sel, *req, *mime))
                send_notify(*this, *req, false);
        }
        return true;
    }
    }
    if (xfixes_ && ev->response_type - xfixes_->first_event == XCB_XFIXES_SELECTION_NOTIFY) {
        auto* e = reinterpret_cast<xcb_xfixes_selection_notify_event_t*>(ev);
        Selection* sel = selection_for(e->selection);
        if (!sel)
            return false;
        if (e->owner == XCB_WINDOW_NONE) {
            // A real X11 owner went (not atrium's proxy): so does its data.
            if (sel->owner != sel->window && sel != dnd_.get() && sel->x_source) {
                const bool clip = sel == clipboard_.get();
                wl::DataSource* current = clip ? (data_ ? data_->selection() : nullptr)
                                               : (primary_ ? primary_->slot().get() : nullptr);
                if (current == sel->x_source.get())
                    request_wayland_selection(*this, seat_, data_, primary_, clip, nullptr);
            }
            sel->owner = XCB_WINDOW_NONE;
            return true;
        }
        sel->owner = e->owner;
        if (sel->owner == sel->window) {
            // Claimed with CurrentTime: the real time, for TIMESTAMP.
            sel->timestamp = e->timestamp;
            return true;
        }
        // An X11 window owns it now: what does it offer?
        xcb_convert_selection(conn_, sel->window, sel->atom, atoms_[TARGETS], atoms_[WL_SELECTION], e->timestamp);
        schedule_flush();
        return true;
    }
    return false;
}

void Xwm::handle_selection_destroy_notify(xcb_destroy_notify_event_t* ev) {
    for (Selection* sel : {clipboard_.get(), primary_selection_.get(), dnd_.get()})
        sel->outgoing.remove_if([ev](const auto& t) { return t->request.requestor == ev->window; });
}

// ---- drag and drop: Wayland drags over X11 windows ----------------------------------------

namespace {

uint32_t action_from_atom(Xwm& xwm, xcb_atom_t a) {
    if (a == xwm.atom(DND_ACTION_COPY) || a == xwm.atom(DND_ACTION_PRIVATE))
        return wl::dnd::Copy;
    if (a == xwm.atom(DND_ACTION_MOVE))
        return wl::dnd::Move;
    if (a == xwm.atom(DND_ACTION_ASK))
        return wl::dnd::Ask;
    return wl::dnd::None;
}

xcb_atom_t action_to_atom(Xwm& xwm, uint32_t action) {
    if (action & wl::dnd::Copy)
        return xwm.atom(DND_ACTION_COPY);
    if (action & wl::dnd::Move)
        return xwm.atom(DND_ACTION_MOVE);
    if (action & wl::dnd::Ask)
        return xwm.atom(DND_ACTION_ASK);
    return XCB_ATOM_NONE;
}

} // namespace

void Xwm::dnd_send(xcb_atom_t type, const xcb_client_message_data_t& data) {
    assert(drag_focus_);
    xcb_client_message_event_t ev{};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = drag_focus_->window_id;
    ev.type = type;
    ev.data = data;
    send_event(conn_, drag_focus_->window_id, XCB_EVENT_MASK_NO_EVENT, &ev, sizeof(ev));
    schedule_flush();
}

void Xwm::set_drag_focus(XSurface* focus) {
    if (focus == drag_focus_)
        return;
    if (drag_focus_) {
        if (drag_)
            drag_->set_external_target(false, wl::dnd::None);
        xcb_client_message_data_t d{};
        d.data32[0] = dnd_->window;
        dnd_send(atoms_[DND_LEAVE], d);
    }
    drag_focus_ = focus;
    drag_accepted_ = false;
    if (!focus || !drag_ || !drag_->source())
        return;
    // XdndEnter, with up to three types inline, else in XdndTypeList.
    const auto& mimes = drag_->source()->mime_types();
    xcb_client_message_data_t d{};
    d.data32[0] = dnd_->window;
    d.data32[1] = kXdndVersion << 24;
    if (mimes.size() <= 3) {
        for (size_t i = 0; i < mimes.size(); ++i)
            d.data32[2 + i] = mime_to_atom(mimes[i]);
    } else {
        d.data32[1] |= 1;
        std::vector<xcb_atom_t> targets;
        for (const std::string& m : mimes)
            targets.push_back(mime_to_atom(m));
        xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, dnd_->window, atoms_[DND_TYPE_LIST], XCB_ATOM_ATOM, 32,
                            uint32_t(targets.size()), targets.data());
    }
    dnd_send(atoms_[DND_ENTER], d);
}

void Xwm::start_drag(wl::Drag* drag) {
    dnd_->set_owner(drag != nullptr);
    drag_ = drag;
    drag_focus_ = nullptr;
    drop_focus_ = nullptr;
    drag_accepted_ = false;
    drag_focus_c_ = drag->focus_changed.connect([this](wl::Surface* s) { set_drag_focus(s ? XSurface::from(s) : nullptr); });
    drag_moved_ = drag->moved.connect([this](double sx, double sy, uint32_t time) {
        if (!drag_focus_ || !drag_ || !drag_->source())
            return;
        xcb_client_message_data_t d{};
        d.data32[0] = dnd_->window;
        d.data32[2] = uint32_t(int16_t(drag_focus_->x + int16_t(sx))) << 16 |
                      uint16_t(int16_t(drag_focus_->y + int16_t(sy)));
        d.data32[3] = time;
        d.data32[4] = action_to_atom(*this, drag_->source()->dnd_actions());
        dnd_send(atoms_[DND_POSITION], d);
    });
    drag_dropped_ = drag->dropped.connect([this](uint32_t time) {
        if (!drag_focus_)
            return;
        drop_focus_ = drag_focus_;
        // XdndFinished comes after the drag itself is gone.
        drop_source_ = drag_ ? drag_->source() : nullptr;
        if (drop_source_)
            drop_source_gone_ = drop_source_->events.destroy.connect([this] {
                drop_source_ = nullptr;
                drop_focus_ = nullptr;
                drop_source_gone_.disconnect();
            });
        xcb_client_message_data_t d{};
        d.data32[0] = dnd_->window;
        d.data32[2] = time;
        dnd_send(atoms_[DND_DROP], d);
    });
    drag_ended_ = drag->ended.connect([this] {
        // Not taken: the X11 window is told it left.
        if (drag_focus_ && !drag_accepted_ && drop_focus_ != drag_focus_) {
            xcb_client_message_data_t d{};
            d.data32[0] = dnd_->window;
            dnd_send(atoms_[DND_LEAVE], d);
        }
        drag_focus_ = nullptr;
        drag_ = nullptr;
        drag_focus_c_.disconnect();
        drag_moved_.disconnect();
        drag_dropped_.disconnect();
        drag_ended_.disconnect();
    });
}

bool Xwm::handle_selection_client_message(xcb_client_message_event_t* ev) {
    if (ev->type == atoms_[DND_STATUS]) {
        if (!drag_ || !drag_focus_ || ev->data.data32[0] != drag_focus_->window_id)
            return true;
        drag_accepted_ = ev->data.data32[1] & 1;
        drag_->set_external_target(drag_accepted_, action_from_atom(*this, ev->data.data32[4]));
        return true;
    }
    if (ev->type == atoms_[DND_FINISHED]) {
        // After the drop, before the source goes.
        if (drag_ || !drop_source_ || !drop_focus_ || ev->data.data32[0] != drop_focus_->window_id)
            return true;
        wl::DataSource* source = drop_source_;
        drop_source_ = nullptr;
        drop_focus_ = nullptr;
        drop_source_gone_.disconnect();
        source->dnd_finished();
        return true;
    }
    return false;
}

} // namespace atrium::xwayland
#endif
