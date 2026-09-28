#include "wl/selection.hpp"

#include "ext-data-control-v1-server.hpp"
#include "primary-selection-unstable-v1-server.hpp"
#include "wlr-data-control-unstable-v1-server.hpp"

#include <unistd.h>

#include <algorithm>

namespace atrium::wl {

namespace {

// A source made through one of the protocols here: `Base` is its generated
// class. All of them offer types, send on request and hear when replaced.
template <class Base>
class ProtocolSource : public Base, public DataSource {
public:
    ProtocolSource(wl_client* client, uint32_t version, uint32_t id) : Base(client, version, id) {
        this->on_offer([this](Base*, const char* mime) {
            if (!offers(mime))
                mime_types_.emplace_back(mime);
        });
    }
    void send(const std::string& mime, int fd) override {
        this->send_send(mime.c_str(), fd);
        close(fd);
    }
    void cancelled() override { this->send_cancelled(); }
    wl_client* client() const override { return Base::client(); }

    bool used = false;  // a source may be set once
};

// An offer of `source`'s types to one client.
template <class Base>
class ProtocolOffer : public Base {
public:
    ProtocolOffer(wl_client* client, uint32_t version, uint32_t id, DataSource* s)
        : Base(client, version, id), source(s) {
        gone = s->events.destroy.connect([this] { source = nullptr; });
        this->on_receive([this](Base*, const char* mime, int fd) {
            if (source && source->offers(mime))
                source->send(mime, fd);
            else
                close(fd);
        });
    }
    DataSource* source;
    Signal<>::Connection gone;
};

// Makes an offer of `source` through `device` (an event that makes the offer
// object), then lists its types.
template <class Offer, class Device>
Offer* offer_on(Device* device, DataSource* source) {
    auto* o = make<Offer>(device->client(), device->version(), 0, source);
    if (!o)
        return nullptr;
    device->send_data_offer(o);
    for (const std::string& mime : source->mime_types())
        o->send_offer(mime.c_str());
    return o;
}

} // namespace

// ---- primary selection ----------------------------------------------------------

using PrimarySource = ProtocolSource<ZwpPrimarySelectionSourceV1>;
using PrimaryOffer = ProtocolOffer<ZwpPrimarySelectionOfferV1>;

PrimarySelection::PrimarySelection(wl_display* display, Seat& seat) : seat_(seat) {
    global_ = Global::create<ZwpPrimarySelectionDeviceManagerV1>(display, 1, [this](wl_client* client,
                                                                                   uint32_t version, uint32_t id) {
        auto* m = make<ZwpPrimarySelectionDeviceManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_create_source([](ZwpPrimarySelectionDeviceManagerV1* self, uint32_t id) {
            make<PrimarySource>(self->client(), self->version(), id);
        });
        m->on_get_device([this](ZwpPrimarySelectionDeviceManagerV1* self, uint32_t id, wl_resource* seat) {
            auto* d = make<ZwpPrimarySelectionDeviceV1>(self->client(), self->version(), id);
            if (!d)
                return;
            if (Seat::from(seat) != &seat_) {
                d->detach();
                return;
            }
            d->on_set_selection([this](ZwpPrimarySelectionDeviceV1*, ZwpPrimarySelectionSourceV1* source_resource,
                                       uint32_t serial) {
                auto* source = dynamic_cast<PrimarySource*>(source_resource);
                if (request_selection.empty())
                    slot_.set(source);
                else
                    request_selection.emit({source, serial});
            });
            std::erase_if(devices_, [](const auto& w) { return !w; });
            devices_.push_back(d);
            if (Surface* f = seat_.keyboard_focus(); f && f->client() == self->client())
                offer(self->client());
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
    changed_ = slot_.changed.connect([this](DataSource*) {
        if (Surface* f = seat_.keyboard_focus())
            offer(f->client());
    });
    focus_changed_ = seat.events.keyboard_client.connect([this](wl_client* client) {
        if (client)
            offer(client);
    });
}

PrimarySelection::~PrimarySelection() {
    global_.reset();
    for (auto* list : {&managers_, &devices_})
        for (auto& w : *list)
            if (w)
                w->detach();
}

void PrimarySelection::offer(wl_client* client) {
    for (auto& w : devices_) {
        auto* d = static_cast<ZwpPrimarySelectionDeviceV1*>(w.get());
        if (!d || d->inert() || d->client() != client)
            continue;
        if (DataSource* s = slot_.get()) {
            if (auto* o = offer_on<PrimaryOffer>(d, s))
                d->send_selection(o);
        } else {
            d->send_selection(nullptr);
        }
    }
}

// ---- data control ------------------------------------------------------------------

struct DataControl::Impl {
    virtual ~Impl() = default;
};

namespace {

// One data-control protocol: `P` names its generated classes.
template <class Manager, class Device, class SourceBase, class OfferBase>
struct ControlImpl : DataControl::Impl {
    using Source = ProtocolSource<SourceBase>;
    using Offer = ProtocolOffer<OfferBase>;

    Seat& seat;
    SelectionSlot& clipboard;
    SelectionSlot& primary;
    std::unique_ptr<Global> global;
    std::vector<Weak<Resource>> managers, devices;
    Signal<DataSource*>::Connection clipboard_changed, primary_changed;

    ControlImpl(wl_display* display, uint32_t version, Seat& s, SelectionSlot& c, SelectionSlot& p)
        : seat(s), clipboard(c), primary(p) {
        global = Global::create<Manager>(display, version, [this](wl_client* client, uint32_t v, uint32_t id) {
            auto* m = make<Manager>(client, v, id);
            if (!m)
                return;
            m->on_create_data_source([](Manager* self, uint32_t id) {
                make<Source>(self->client(), self->version(), id);
            });
            m->on_get_data_device([this](Manager* self, uint32_t id, wl_resource* seat_resource) {
                auto* d = make<Device>(self->client(), self->version(), id);
                if (!d)
                    return;
                if (Seat::from(seat_resource) != &seat) {
                    d->detach();
                    return;
                }
                d->on_set_selection([this](Device* self, SourceBase* sr) { take(self, sr, clipboard); });
                d->on_set_primary_selection([this](Device* self, SourceBase* sr) { take(self, sr, primary); });
                std::erase_if(devices, [](const auto& w) { return !w; });
                devices.push_back(d);
                send(d, clipboard.get(), false);
                if (d->version() >= 2 || std::is_same_v<Device, ExtDataControlDeviceV1>)
                    send(d, primary.get(), true);
            });
            std::erase_if(managers, [](const auto& w) { return !w; });
            managers.push_back(m);
        });
        clipboard_changed = clipboard.changed.connect([this](DataSource* s) { send_all(s, false); });
        primary_changed = primary.changed.connect([this](DataSource* s) { send_all(s, true); });
    }

    ~ControlImpl() override {
        global.reset();
        for (auto* list : {&managers, &devices})
            for (auto& w : *list)
                if (w)
                    w->detach();
    }

    void take(Device* device, SourceBase* source_resource, SelectionSlot& slot) {
        auto* source = dynamic_cast<Source*>(source_resource);
        if (source) {
            if (source->used) {
                device->post_error(uint32_t(Device::Error::UsedSource), "the source was already used");
                return;
            }
            source->used = true;
        }
        slot.set(source);
    }

    void send(Device* d, DataSource* s, bool is_primary) {
        auto* o = s ? offer_on<Offer>(d, s) : nullptr;
        if (is_primary)
            d->send_primary_selection(o);
        else
            d->send_selection(o);
    }

    void send_all(DataSource* s, bool is_primary) {
        for (auto& w : devices) {
            auto* d = static_cast<Device*>(w.get());
            if (!d || d->inert())
                continue;
            if (is_primary && !(d->version() >= 2 || std::is_same_v<Device, ExtDataControlDeviceV1>))
                continue;
            send(d, s, is_primary);
        }
    }
};

} // namespace

DataControl::DataControl(wl_display* display, Seat& seat, SelectionSlot& clipboard, SelectionSlot& primary)
    : ext_(std::make_unique<ControlImpl<ExtDataControlManagerV1, ExtDataControlDeviceV1, ExtDataControlSourceV1,
                                        ExtDataControlOfferV1>>(display, 1, seat, clipboard, primary)),
      wlr_(std::make_unique<ControlImpl<ZwlrDataControlManagerV1, ZwlrDataControlDeviceV1,
                                        ZwlrDataControlSourceV1, ZwlrDataControlOfferV1>>(display, 2, seat,
                                                                                          clipboard, primary)) {}

DataControl::~DataControl() = default;

} // namespace atrium::wl
