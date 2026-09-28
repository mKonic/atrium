#pragma once
#include "wl/data_device.hpp"

namespace atrium::wl {

// zwp_primary_selection_device_manager_v1: the middle-click selection, the
// clipboard's twin without drag and drop.
class PrimarySelection {
public:
    PrimarySelection(wl_display* display, Seat& seat);
    ~PrimarySelection();
    PrimarySelection(const PrimarySelection&) = delete;
    PrimarySelection& operator=(const PrimarySelection&) = delete;

    SelectionSlot& slot() { return slot_; }

    struct Request {
        DataSource* source;
        uint32_t serial;
    };
    Signal<const Request&> request_selection;  // unconnected: granted

private:
    void offer(wl_client* client);

    Seat& seat_;
    SelectionSlot slot_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_, devices_;
    Signal<DataSource*>::Connection changed_;
    Signal<wl_client*>::Connection focus_changed_;
};

// ext-data-control-v1 and wlr-data-control-unstable-v1: clipboard managers
// (cliphist, wl-clipboard, atrium's own clipsync) see and set both
// selections without having focus.
class DataControl {
public:
    DataControl(wl_display* display, Seat& seat, SelectionSlot& clipboard, SelectionSlot& primary);
    ~DataControl();
    DataControl(const DataControl&) = delete;
    DataControl& operator=(const DataControl&) = delete;

    struct Impl;

private:
    std::unique_ptr<Impl> ext_, wlr_;
};

} // namespace atrium::wl
