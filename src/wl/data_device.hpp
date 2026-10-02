#pragma once
#include "wl/seat.hpp"

#include <string>

namespace atrium::wl {

// What a selection or a drag carries: offered as MIME types, sent through a
// pipe on request. Clients make them (wl_data_source, the primary-selection
// and data-control sources); atrium can make its own.
class DataSource {
public:
    virtual ~DataSource() { events.destroy.emit(); }

    const std::vector<std::string>& mime_types() const { return mime_types_; }
    bool offers(const std::string& mime) const;
    // Writes the data as `mime` into `fd`, and closes it.
    virtual void send(const std::string& mime, int fd) = 0;
    // No longer the selection (or the drag was cancelled).
    virtual void cancelled() {}
    // The client that made it; null for atrium's own.
    virtual wl_client* client() const { return nullptr; }

    // Drag and drop (wl_data_source v3).
    uint32_t dnd_actions() const { return dnd_actions_; }
    virtual void dnd_target(const char* /*mime*/) {}
    virtual void dnd_action(uint32_t /*action*/) {}
    virtual void dnd_drop_performed() {}
    virtual void dnd_finished() {}

    struct {
        Signal<> destroy;
    } events;

protected:
    std::vector<std::string> mime_types_;
    uint32_t dnd_actions_ = 0;
    bool actions_set_ = false;
};

class DataOffer;

// Which source a selection (the clipboard, the primary selection) holds.
// Replacing it cancels the old one; a source that goes empties it.
class SelectionSlot {
public:
    DataSource* get() const { return source_; }
    void set(DataSource* source);
    Signal<DataSource*> changed;

private:
    DataSource* source_ = nullptr;
    Signal<>::Connection gone_;
};

// A drag and drop in progress: the compositor moves it (motion over the
// surface under the pointer) and ends it (drop, cancel).
class Drag {
public:
    Drag(class DataDevices& devices, DataSource* source, Surface* origin, Surface* icon);
    ~Drag();

    DataSource* source() const { return source_; }
    Surface* icon() const { return icon_; }
    Surface* origin() const { return origin_; }
    Surface* focus() const { return focus_; }

    // The pointer is over `surface` (null: nothing that takes drops).
    void motion(Surface* surface, double sx, double sy, uint32_t time_ms);
    // Button released: dropped on the focus, or cancelled.
    void drop(uint32_t time_ms);
    void cancel();

    // A target outside the protocol (an X11 window, through the XWM) said
    // whether it takes the drag, and with which action.
    void set_external_target(bool accepted, uint32_t action);

    Signal<> ended;
    // For targets outside the protocol: where the drag is.
    Signal<Surface*> focus_changed;
    Signal<double, double, uint32_t> moved;  // over the focus: sx, sy, time
    Signal<uint32_t> dropped;  // on the focus, which took it

private:
    friend class DataOffer;
    void leave();
    void update_action();

    DataDevices& devices_;
    DataSource* source_;
    Surface* origin_;
    Surface* icon_;
    Surface* focus_ = nullptr;
    Weak<Resource> offer_;  // DataOffer made for the focus
    Signal<>::Connection source_gone_, icon_gone_, focus_gone_, origin_gone_;
    bool dropped_ = false;
    bool external_accepted_ = false;
};

// wl_data_device_manager for one seat: the clipboard selection and drag and
// drop. What clients ask (set a selection, start a drag) comes out as
// signals; the compositor's usual answer is set_selection / start_drag.
class DataDevices {
public:
    DataDevices(wl_display* display, Seat& seat);
    ~DataDevices();
    DataDevices(const DataDevices&) = delete;
    DataDevices& operator=(const DataDevices&) = delete;

    Seat& seat() { return seat_; }

    DataSource* selection() const { return slot_.get(); }
    void set_selection(DataSource* source) { slot_.set(source); }
    SelectionSlot& slot() { return slot_; }

    // Starts a drag with `source` (null: within the origin's client only).
    Drag* start_drag(DataSource* source, Surface* origin, Surface* icon);
    Drag* drag() const { return drag_.get(); }

    struct SelectionRequest {
        DataSource* source;
        uint32_t serial;
    };
    struct DragRequest {
        DataSource* source;
        Surface* origin;
        Surface* icon;
        uint32_t serial;
    };
    struct {
        Signal<const SelectionRequest&> request_selection;
        Signal<const DragRequest&> request_drag;
        Signal<Drag*> drag_started;
    } events;

private:
    friend class Drag;
    friend class DataOffer;
    void offer_selection(wl_client* client);
    std::vector<WlDataDevice*> devices_for(wl_client* client) const;
    DataOffer* make_offer(WlDataDevice* device, DataSource* source, bool dnd);

    Seat& seat_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<WlDataDeviceManager>> managers_;
    std::vector<Weak<WlDataDevice>> devices_;
    SelectionSlot slot_;
    Signal<DataSource*>::Connection slot_changed_;
    Signal<wl_client*>::Connection focus_changed_;
    std::unique_ptr<Drag> drag_;
};

// The kinds of drag-and-drop action (wl_data_device_manager.dnd_action).
namespace dnd {
constexpr uint32_t None = 0, Copy = 1, Move = 2, Ask = 4;
// What a source offering `source` and a target accepting `target`,
// preferring `preferred`, settle on.
uint32_t choose(uint32_t source, uint32_t target, uint32_t preferred);
} // namespace dnd

} // namespace atrium::wl
