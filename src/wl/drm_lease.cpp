#include "wl/drm_lease.hpp"

#include "drm-lease-v1-server.hpp"

#include <unistd.h>

#include <algorithm>

namespace atrium::wl {

DrmLease::DrmLease(wl_display* display, Provider& provider) : provider_(provider) {
    global_ = Global::create<WpDrmLeaseDeviceV1>(display, 1, [this](wl_client* client, uint32_t version, uint32_t id) {
        bind(client, version, id);
    });
}

DrmLease::~DrmLease() {
    global_.reset();
    for (auto& d : devices_)
        if (d)
            d->detach();
    for (auto& o : offers_)
        for (auto& r : o->resources)
            if (r)
                r->detach();
    for (auto& l : leases_)
        if (l.resource)
            l.resource->detach();
    for (auto& [w, id] : made_)
        if (w)
            w->detach();
}

void DrmLease::bind(wl_client* client, uint32_t version, uint32_t id) {
    auto* d = make<WpDrmLeaseDeviceV1>(client, version, id);
    if (!d)
        return;
    d->on_create_lease_request([this](WpDrmLeaseDeviceV1* self, uint32_t rid) {
        auto* req = make<WpDrmLeaseRequestV1>(self->client(), self->version(), rid);
        if (!req)
            return;
        auto picked = std::make_shared<std::vector<uint32_t>>();
        req->on_request_connector([this, picked](WpDrmLeaseRequestV1* r, WpDrmLeaseConnectorV1* c) {
            const uint32_t id = c ? connector_of(c) : 0;
            if (!id) {
                r->post_error(uint32_t(WpDrmLeaseRequestV1::Error::WrongDevice), "a connector of another device");
                return;
            }
            if (std::ranges::find(*picked, id) != picked->end()) {
                r->post_error(uint32_t(WpDrmLeaseRequestV1::Error::DuplicateConnector), "connector asked for twice");
                return;
            }
            picked->push_back(id);
        });
        req->on_submit([this, picked](WpDrmLeaseRequestV1* r, uint32_t lid) {
            if (picked->empty()) {
                r->post_error(uint32_t(WpDrmLeaseRequestV1::Error::EmptyLease), "a lease of no connectors");
                return;
            }
            submit(*picked, r->client(), r->version(), lid);
        });
    });
    d->on_release([](WpDrmLeaseDeviceV1* self) {
        self->send_released();  // a destructor event: the object goes with it
        self->destroy();
    });
    std::erase_if(devices_, [](const auto& w) { return !w; });
    devices_.push_back(d);

    const int fd = provider_.non_master_fd();
    if (fd >= 0) {
        d->send_drm_fd(fd);  // libwayland sends a duplicate
        close(fd);
    }
    for (auto& o : offers_)
        if (!o->lessee)
            announce(d, *o);
    d->send_done();
}

void DrmLease::announce(Resource* device_res, Offer& o) {
    auto* device = static_cast<WpDrmLeaseDeviceV1*>(device_res);
    if (device->inert())
        return;
    auto* c = make<WpDrmLeaseConnectorV1>(device->client(), device->version(), 0);
    if (!c)
        return;
    device->send_connector(c);
    c->send_name(o.name.c_str());
    c->send_description(o.description.c_str());
    c->send_connector_id(o.id);
    c->send_done();
    std::erase_if(o.resources, [](const auto& w) { return !w; });
    o.resources.push_back(c);
    std::erase_if(made_, [](const auto& m) { return !m.first; });
    made_.emplace_back(c, o.id);
}

void DrmLease::announce_all(Offer& o) {
    for (auto& d : devices_)
        if (d)
            announce(d.get(), o);
}

void DrmLease::hide(Offer& o) {
    for (auto& r : o.resources)
        if (r)
            static_cast<WpDrmLeaseConnectorV1*>(r.get())->send_withdrawn();
    // The objects stay until the client destroys them; requests naming them
    // now ask for something unavailable (the lease finishes at once).
    o.resources.clear();
}

void DrmLease::done_all() {
    for (auto& d : devices_)
        if (d && !d->inert())
            static_cast<WpDrmLeaseDeviceV1*>(d.get())->send_done();
}

uint32_t DrmLease::connector_of(Resource* object) {
    for (auto& [w, id] : made_)
        if (w.get() == object)
            return id;
    return 0;
}

void DrmLease::offer(uint32_t connector, const std::string& name, const std::string& description) {
    if (std::ranges::any_of(offers_, [&](const auto& o) { return o->id == connector; }))
        return;
    auto o = std::make_unique<Offer>();
    o->id = connector;
    o->name = name;
    o->description = description;
    offers_.push_back(std::move(o));
    announce_all(*offers_.back());
    done_all();
}

void DrmLease::withdraw(uint32_t connector) {
    auto it = std::ranges::find_if(offers_, [&](const auto& o) { return o->id == connector; });
    if (it == offers_.end())
        return;
    hide(**it);
    offers_.erase(it);
    done_all();
}

void DrmLease::submit(const std::vector<uint32_t>& connectors, wl_client* client, uint32_t version, uint32_t id) {
    auto* lease = make<WpDrmLeaseV1>(client, version, id);
    if (!lease)
        return;
    std::vector<Offer*> offers;
    for (uint32_t c : connectors) {
        auto it = std::ranges::find_if(offers_, [&](const auto& o) { return o->id == c; });
        if (it == offers_.end() || (*it)->lessee) {
            lease->send_finished();  // gone or taken since it was asked for
            return;
        }
        offers.push_back(it->get());
    }
    uint32_t lessee = 0;
    const int fd = provider_.create_lease(connectors, &lessee);
    if (fd < 0) {
        lease->send_finished();
        return;
    }
    lease->send_lease_fd(fd);
    close(fd);
    for (Offer* o : offers) {
        o->lessee = lessee;
        hide(*o);
    }
    done_all();
    leases_.push_back({lessee, connectors, lease});
    // Its holder letting go (or disconnecting) ends it.
    lease->on_gone([this, lessee] { end(lessee, true); });
}

void DrmLease::end(uint32_t lessee, bool revoke) {
    auto it = std::ranges::find_if(leases_, [&](const Lease& l) { return l.lessee == lessee; });
    if (it == leases_.end())
        return;
    Lease l = std::move(*it);
    leases_.erase(it);
    if (revoke)
        provider_.revoke_lease(lessee);
    for (auto& o : offers_)
        if (o->lessee == lessee) {
            o->lessee = 0;
            announce_all(*o);
        }
    done_all();
}

void DrmLease::lease_ended(uint32_t lessee) {
    auto it = std::ranges::find_if(leases_, [&](const Lease& l) { return l.lessee == lessee; });
    if (it == leases_.end())
        return;
    if (Resource* r = it->resource.get())
        static_cast<WpDrmLeaseV1*>(r)->send_finished();
    end(lessee, false);  // gone from leases_: the holder's destroy revokes nothing
}

} // namespace atrium::wl
