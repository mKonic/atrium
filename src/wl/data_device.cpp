#include "wl/data_device.hpp"

#include <unistd.h>

#include <algorithm>

namespace atrium::wl {

namespace {

class IconRole : public Role {
public:
    static constexpr const char* kName = "wl_data_device-icon";
    const char* name() const override { return kName; }
};
IconRole g_icon_role;

// A client's wl_data_source.
class ClientDataSource : public WlDataSource, public DataSource {
public:
    ClientDataSource(wl_client* client, uint32_t version, uint32_t id) : WlDataSource(client, version, id) {
        on_offer([this](WlDataSource*, const char* mime) {
            if (!offers(mime))
                mime_types_.emplace_back(mime);
        });
        on_set_actions([this](WlDataSource*, uint32_t actions) {
            if (actions & ~(dnd::Copy | dnd::Move | dnd::Ask)) {
                post_error(uint32_t(Error::InvalidActionMask), "unknown drag-and-drop actions");
                return;
            }
            if (actions_set_) {
                post_error(uint32_t(Error::InvalidSource), "set_actions was already called");
                return;
            }
            dnd_actions_ = actions;
            actions_set_ = true;
        });
    }

    void send(const std::string& mime, int fd) override {
        send_send(mime.c_str(), fd);
        close(fd);
    }
    void cancelled() override { send_cancelled(); }
    wl_client* client() const override { return WlDataSource::client(); }
    void dnd_target(const char* mime) override { send_target(mime); }
    void dnd_action(uint32_t action) override {
        if (version() >= 3)
            send_action(action);
    }
    void dnd_drop_performed() override {
        if (version() >= 3)
            send_dnd_drop_performed();
    }
    void dnd_finished() override {
        if (version() >= 3)
            send_dnd_finished();
    }
};

} // namespace

bool DataSource::offers(const std::string& mime) const {
    return std::ranges::find(mime_types_, mime) != mime_types_.end();
}

uint32_t dnd::choose(uint32_t source, uint32_t target, uint32_t preferred) {
    const uint32_t both = source & target;
    if (preferred & both)
        return preferred & both & (~preferred + 1);  // one bit of it
    for (uint32_t a : {Copy, Move, Ask})
        if (both & a)
            return a;
    return None;
}

// ---- DataOffer -------------------------------------------------------------------

class DataOffer : public WlDataOffer {
public:
    DataOffer(wl_client* client, uint32_t version, uint32_t id, DataSource* source, bool dnd)
        : WlDataOffer(client, version, id), source(source), dnd(dnd) {
        source_gone = source->events.destroy.connect([this] { this->source = nullptr; });
        on_accept([this](WlDataOffer*, uint32_t, const char* mime) {
            if (!this->source || !this->dnd || finished)
                return;
            accepted = mime && this->source->offers(mime);
            this->source->dnd_target(mime);
        });
        on_receive([this](WlDataOffer*, const char* mime, int fd) {
            if (this->source && this->source->offers(mime))
                this->source->send(mime, fd);
            else
                close(fd);
        });
        on_finish([this](WlDataOffer*) {
            if (!this->dnd || !dropped || finished) {
                post_error(uint32_t(Error::InvalidFinish), "finish outside a finished drop");
                return;
            }
            finished = true;
            if (this->source)
                this->source->dnd_finished();
        });
        on_set_actions([this](WlDataOffer*, uint32_t actions, uint32_t preferred) {
            if ((actions | preferred) & ~(dnd::Copy | dnd::Move | dnd::Ask)) {
                post_error(uint32_t(Error::InvalidActionMask), "unknown drag-and-drop actions");
                return;
            }
            if (preferred & (preferred - 1)) {
                post_error(uint32_t(Error::InvalidAction), "more than one preferred action");
                return;
            }
            this->actions = actions;
            this->preferred = preferred;
            if (drag)
                drag->update_action();
        });
        on_gone([this] {
            // Dropped but never finished: for the source, as if cancelled.
            if (this->dnd && dropped && !finished && this->source && this->version() >= 3)
                this->source->cancelled();
        });
    }

    DataSource* source;
    bool dnd;
    Drag* drag = nullptr;
    bool accepted = false, dropped = false, finished = false;
    uint32_t actions = 0, preferred = 0, chosen = 0;
    Signal<>::Connection source_gone;
};

// ---- DataDevices -------------------------------------------------------------------

DataDevices::DataDevices(wl_display* display, Seat& seat) : seat_(seat) {
    global_ = Global::create<WlDataDeviceManager>(display, 3, [this](wl_client* client, uint32_t version,
                                                                     uint32_t id) {
        auto* m = make<WlDataDeviceManager>(client, version, id);
        if (!m)
            return;
        m->on_create_data_source([](WlDataDeviceManager* self, uint32_t id) {
            make<ClientDataSource>(self->client(), self->version(), id);
        });
        m->on_get_data_device([this](WlDataDeviceManager* self, uint32_t id, WlSeat* seat_resource) {
            auto* d = make<WlDataDevice>(self->client(), self->version(), id);
            if (!d)
                return;
            if (Seat::from(seat_resource) != &seat_) {
                d->detach();  // another seat's, or one gone
                return;
            }
            d->on_set_selection([this](WlDataDevice*, WlDataSource* source_resource, uint32_t serial) {
                auto* source = dynamic_cast<ClientDataSource*>(source_resource);
                if (source && source->dnd_actions()) {
                    source->post_error(uint32_t(WlDataSource::Error::InvalidSource),
                                       "a drag-and-drop source can't be a selection");
                    return;
                }
                if (events.request_selection.empty())
                    set_selection(source);
                else
                    events.request_selection.emit({source, serial});
            });
            d->on_start_drag([this](WlDataDevice* self, WlDataSource* source_resource, WlSurface* origin_resource,
                                    WlSurface* icon_resource, uint32_t serial) {
                auto* origin = dynamic_cast<Surface*>(origin_resource);
                auto* icon = dynamic_cast<Surface*>(icon_resource);
                if (icon) {
                    if (icon->role_name() && icon->role_name() != IconRole::kName) {
                        self->post_error(uint32_t(WlDataDevice::Error::Role), "the icon has another role");
                        return;
                    }
                    if (!icon->role())
                        icon->set_role(&g_icon_role, nullptr, 0);
                }
                // Only from a press the client has (an implicit grab).
                if (!origin || !seat_.validate_grab_serial(self->client(), serial) || drag_)
                    return;
                auto* source = dynamic_cast<ClientDataSource*>(source_resource);
                if (events.request_drag.empty())
                    start_drag(source, origin, icon);
                else
                    events.request_drag.emit({source, origin, icon, serial});
            });
            std::erase_if(devices_, [](const auto& w) { return !w; });
            devices_.push_back(d);
            if (Surface* f = seat_.keyboard_focus(); f && f->client() == self->client())
                offer_selection(self->client());
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
    slot_changed_ = slot_.changed.connect([this](DataSource*) {
        if (Surface* f = seat_.keyboard_focus())
            offer_selection(f->client());
    });
    focus_changed_ = seat.events.keyboard_client.connect([this](wl_client* client) {
        if (client)
            offer_selection(client);
    });
}

DataDevices::~DataDevices() {
    drag_.reset();
    global_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
    for (auto& d : devices_)
        if (d)
            d->detach();
}

std::vector<WlDataDevice*> DataDevices::devices_for(wl_client* client) const {
    std::vector<WlDataDevice*> out;
    for (const auto& w : devices_)
        if (WlDataDevice* d = w.get(); d && !d->inert() && d->client() == client)
            out.push_back(d);
    return out;
}

DataOffer* DataDevices::make_offer(WlDataDevice* device, DataSource* source, bool is_dnd) {
    auto* offer = make<DataOffer>(device->client(), device->version(), 0, source, is_dnd);
    if (!offer)
        return nullptr;
    device->send_data_offer(offer);
    for (const std::string& mime : source->mime_types())
        offer->send_offer(mime.c_str());
    if (is_dnd && offer->version() >= 3)
        offer->send_source_actions(source->dnd_actions());
    return offer;
}

void SelectionSlot::set(DataSource* source) {
    if (source == source_)
        return;
    DataSource* old = source_;
    source_ = source;
    gone_.disconnect();
    if (source)
        gone_ = source->events.destroy.connect([this] { set(nullptr); });
    if (old)
        old->cancelled();
    changed.emit(source);
}

void DataDevices::offer_selection(wl_client* client) {
    for (WlDataDevice* d : devices_for(client)) {
        if (!slot_.get()) {
            d->send_selection(nullptr);
            continue;
        }
        if (DataOffer* offer = make_offer(d, slot_.get(), false))
            d->send_selection(offer);
    }
}

Drag* DataDevices::start_drag(DataSource* source, Surface* origin, Surface* icon) {
    if (drag_)
        return nullptr;
    drag_ = std::make_unique<Drag>(*this, source, origin, icon);
    Drag* d = drag_.get();
    events.drag_started.emit(d);
    return d;
}

// ---- Drag ----------------------------------------------------------------------------

Drag::Drag(DataDevices& devices, DataSource* source, Surface* origin, Surface* icon)
    : devices_(devices), source_(source), origin_(origin), icon_(icon) {
    if (source)
        source_gone_ = source->events.destroy.connect([this] {
            source_ = nullptr;
            cancel();
        });
    if (icon)
        icon_gone_ = icon->events.destroy.connect([this] { icon_ = nullptr; });
    if (origin)
        origin_gone_ = origin->events.destroy.connect([this] { origin_ = nullptr; });
}

Drag::~Drag() {
    if (auto* o = static_cast<DataOffer*>(offer_.get()))
        o->drag = nullptr;
}

void Drag::leave() {
    if (!focus_)
        return;
    for (WlDataDevice* d : devices_.devices_for(focus_->client()))
        d->send_leave();
    if (auto* o = static_cast<DataOffer*>(offer_.get()))
        o->drag = nullptr;
    offer_ = {};
    focus_ = nullptr;
    focus_gone_.disconnect();
}

void Drag::motion(Surface* surface, double sx, double sy, uint32_t time_ms) {
    // Without a source, only the origin's own client takes it.
    if (surface && !source_ && (!origin_ || surface->client() != origin_->client()))
        surface = nullptr;
    if (surface != focus_) {
        leave();
        if (!surface)
            return;
        focus_ = surface;
        focus_gone_ = surface->events.destroy.connect([this] {
            focus_ = nullptr;
            offer_ = {};
            focus_gone_.disconnect();
        });
        const uint32_t serial = devices_.seat().next_serial();
        for (WlDataDevice* d : devices_.devices_for(surface->client())) {
            DataOffer* offer = source_ ? devices_.make_offer(d, source_, true) : nullptr;
            if (offer) {
                offer->drag = this;
                offer_ = offer;
            }
            d->send_enter(serial, surface, sx, sy, offer);
        }
        return;
    }
    if (focus_)
        for (WlDataDevice* d : devices_.devices_for(focus_->client()))
            d->send_motion(time_ms, sx, sy);
}

void Drag::update_action() {
    auto* o = static_cast<DataOffer*>(offer_.get());
    if (!o || !source_)
        return;
    const uint32_t chosen = dnd::choose(source_->dnd_actions(), o->actions, o->preferred);
    if (chosen == o->chosen)
        return;
    o->chosen = chosen;
    if (o->version() >= 3)
        o->send_action(chosen);
    source_->dnd_action(chosen);
}

void Drag::drop(uint32_t) {
    auto* o = static_cast<DataOffer*>(offer_.get());
    // A v3 target must have agreed on an action; any target, on a type.
    const bool takes = focus_ && (!source_ || (o && o->accepted && (o->version() < 3 || o->chosen != dnd::None)));
    if (takes) {
        for (WlDataDevice* d : devices_.devices_for(focus_->client()))
            d->send_drop();
        if (o)
            o->dropped = true;
        if (source_)
            source_->dnd_drop_performed();
        // The target finishes with the offer; the drag itself is over.
        if (o)
            o->drag = nullptr;
        offer_ = {};
        focus_ = nullptr;
    } else {
        leave();
        if (source_)
            source_->cancelled();
    }
    ended.emit();
    devices_.drag_.reset();  // deletes this: nothing after
}

void Drag::cancel() {
    leave();
    if (source_)
        source_->cancelled();
    ended.emit();
    devices_.drag_.reset();  // deletes this: nothing after
}

} // namespace atrium::wl
