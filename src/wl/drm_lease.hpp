#pragma once
// wp-drm-lease-v1: screens the desktop doesn't use (a VR headset) handed to a
// client, which drives them itself through a leased DRM fd. One global per
// GPU; the backend behind it is a Provider.
#include "wl/resource.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace atrium::wl {

class DrmLease {
public:
    struct Provider {
        virtual ~Provider() = default;
        // A fd on the device without DRM master, to show clients (-1: none).
        virtual int non_master_fd() = 0;
        // A lease of these connectors: its fd and lessee id, -1 if refused.
        virtual int create_lease(const std::vector<uint32_t>& connectors, uint32_t* lessee) = 0;
        virtual void revoke_lease(uint32_t lessee) = 0;
    };

    DrmLease(wl_display* display, Provider& provider);
    ~DrmLease();
    DrmLease(const DrmLease&) = delete;
    DrmLease& operator=(const DrmLease&) = delete;

    // A connector clients may lease; withdrawn when it is unplugged.
    void offer(uint32_t connector, const std::string& name, const std::string& description);
    void withdraw(uint32_t connector);
    // The kernel ended a lease (its holder closed the fd, or a screen went):
    // the client hears it finished and its screens are offered again.
    void lease_ended(uint32_t lessee);

    size_t active_leases() const { return leases_.size(); }

private:
    struct Offer {
        uint32_t id = 0;
        std::string name, description;
        uint32_t lessee = 0;
        std::vector<Weak<Resource>> resources;  // WpDrmLeaseConnectorV1, one per device bound
    };
    struct Lease {
        uint32_t lessee = 0;
        std::vector<uint32_t> connectors;
        Weak<Resource> resource;  // WpDrmLeaseV1
    };

    void bind(wl_client* client, uint32_t version, uint32_t id);
    void announce(Resource* device, Offer& offer);
    void announce_all(Offer& offer);
    void hide(Offer& offer);  // withdrawn from every client
    void done_all();
    // The connector a connector object of this device names (0: not ours).
    uint32_t connector_of(Resource* object);
    void submit(const std::vector<uint32_t>& connectors, wl_client* client, uint32_t version, uint32_t id);
    void end(uint32_t lessee, bool revoke);

    Provider& provider_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> devices_;  // WpDrmLeaseDeviceV1
    std::vector<std::unique_ptr<Offer>> offers_;
    std::vector<Lease> leases_;
    std::vector<std::pair<Weak<Resource>, uint32_t>> made_;  // every connector object, withdrawn too
};

} // namespace atrium::wl
