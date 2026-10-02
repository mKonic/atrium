// wp-drm-lease-v1 (src/wl/drm_lease) against a real libwayland client, with
// a stand-in for the GPU.
#include "wl/drm_lease.hpp"
#include "wl_harness.hpp"

#include "drm-lease-v1-client-protocol.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <algorithm>
#include <unistd.h>

using namespace atrium;

namespace {

struct FakeGpu : wl::DrmLease::Provider {
    std::vector<std::vector<uint32_t>> granted;
    std::vector<uint32_t> revoked;
    bool refuse = false;
    uint32_t next = 100;
    int non_master_fd() override { return open("/dev/null", O_RDONLY | O_CLOEXEC); }
    int create_lease(const std::vector<uint32_t>& connectors, uint32_t* lessee) override {
        if (refuse)
            return -1;
        granted.push_back(connectors);
        *lessee = next++;
        return open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
    void revoke_lease(uint32_t lessee) override { revoked.push_back(lessee); }
};

// What the client hears.
struct Client {
    struct Conn {
        wp_drm_lease_connector_v1* obj;
        std::string name;
        uint32_t id = 0;
        bool withdrawn = false;
    };
    std::vector<Conn> conns;
    int drm_fds = 0, dones = 0, lease_fds = 0, finished = 0;

    Conn* find(uint32_t id) {
        for (auto it = conns.rbegin(); it != conns.rend(); ++it)
            if (it->id == id && !it->withdrawn)
                return &*it;
        return nullptr;
    }
    size_t offered() const {
        return size_t(std::ranges::count_if(conns, [](const Conn& c) { return !c.withdrawn; }));
    }
};

const wp_drm_lease_connector_v1_listener kConn = {
    .name =
        [](void* d, wp_drm_lease_connector_v1* o, const char* n) {
            for (auto& c : static_cast<Client*>(d)->conns)
                if (c.obj == o)
                    c.name = n;
        },
    .description = [](void*, wp_drm_lease_connector_v1*, const char*) {},
    .connector_id =
        [](void* d, wp_drm_lease_connector_v1* o, uint32_t id) {
            for (auto& c : static_cast<Client*>(d)->conns)
                if (c.obj == o)
                    c.id = id;
        },
    .done = [](void*, wp_drm_lease_connector_v1*) {},
    .withdrawn =
        [](void* d, wp_drm_lease_connector_v1* o) {
            for (auto& c : static_cast<Client*>(d)->conns)
                if (c.obj == o)
                    c.withdrawn = true;
        },
};

const wp_drm_lease_device_v1_listener kDevice = {
    .drm_fd =
        [](void* d, wp_drm_lease_device_v1*, int fd) {
            ++static_cast<Client*>(d)->drm_fds;
            close(fd);
        },
    .connector =
        [](void* d, wp_drm_lease_device_v1*, wp_drm_lease_connector_v1* c) {
            static_cast<Client*>(d)->conns.push_back({c});
            wp_drm_lease_connector_v1_add_listener(c, &kConn, d);
        },
    .done = [](void* d, wp_drm_lease_device_v1*) { ++static_cast<Client*>(d)->dones; },
    .released = [](void*, wp_drm_lease_device_v1*) {},
};

const wp_drm_lease_v1_listener kLease = {
    .lease_fd =
        [](void* d, wp_drm_lease_v1*, int fd) {
            ++static_cast<Client*>(d)->lease_fds;
            close(fd);
        },
    .finished = [](void* d, wp_drm_lease_v1*) { ++static_cast<Client*>(d)->finished; },
};

struct Rig : wltest::Harness {
    FakeGpu gpu;
    wl::DrmLease lease{server, gpu};
    Client log;
    wp_drm_lease_device_v1* device = nullptr;
    Rig() {
        lease.offer(42, "DP-2", "Valve Index");
        lease.offer(43, "DP-3", "a second headset");
        device = bind<wp_drm_lease_device_v1>(&wp_drm_lease_device_v1_interface, 1);
        wp_drm_lease_device_v1_add_listener(device, &kDevice, &log);
        pump();
    }
    wp_drm_lease_v1* ask(std::initializer_list<uint32_t> ids) {
        auto* req = wp_drm_lease_device_v1_create_lease_request(device);
        for (uint32_t id : ids)
            wp_drm_lease_request_v1_request_connector(req, log.find(id) ? log.find(id)->obj : nullptr);
        auto* l = wp_drm_lease_request_v1_submit(req);
        wp_drm_lease_v1_add_listener(l, &kLease, &log);
        pump();
        return l;
    }
};

} // namespace

TEST(WlDrmLease, OffersLeasesAndTakesBack) {
    Rig r;
    EXPECT_EQ(r.log.drm_fds, 1);
    EXPECT_EQ(r.log.offered(), 2u);
    ASSERT_NE(r.log.find(42), nullptr);
    EXPECT_EQ(r.log.find(42)->name, "DP-2");

    wp_drm_lease_v1* l = r.ask({42});
    EXPECT_EQ(r.log.lease_fds, 1);
    ASSERT_EQ(r.gpu.granted.size(), 1u);
    EXPECT_EQ(r.gpu.granted[0], std::vector<uint32_t>{42});
    EXPECT_EQ(r.log.find(42), nullptr);  // withdrawn while leased
    EXPECT_EQ(r.log.offered(), 1u);
    EXPECT_EQ(r.lease.active_leases(), 1u);

    wp_drm_lease_v1_destroy(l);
    r.pump();
    EXPECT_EQ(r.gpu.revoked, std::vector<uint32_t>{100});
    EXPECT_NE(r.log.find(42), nullptr);  // offered again
    EXPECT_EQ(r.lease.active_leases(), 0u);
    EXPECT_EQ(r.error(), 0);
}

TEST(WlDrmLease, KernelEndedLeaseFinishesWithoutRevoking) {
    Rig r;
    wp_drm_lease_v1* l = r.ask({42, 43});
    ASSERT_EQ(r.log.lease_fds, 1);
    EXPECT_EQ(r.log.offered(), 0u);
    r.lease.lease_ended(100);
    r.pump();
    EXPECT_EQ(r.log.finished, 1);
    EXPECT_TRUE(r.gpu.revoked.empty());
    EXPECT_EQ(r.log.offered(), 2u);
    wp_drm_lease_v1_destroy(l);
    r.pump();
    EXPECT_TRUE(r.gpu.revoked.empty());  // over already
    EXPECT_EQ(r.error(), 0);
}

TEST(WlDrmLease, UnavailableOrRefusedFinishesAtOnce) {
    Rig r;
    // Asked for while still offered, withdrawn before the submit lands.
    auto* req = wp_drm_lease_device_v1_create_lease_request(r.device);
    wp_drm_lease_request_v1_request_connector(req, r.log.find(43)->obj);
    r.pump();
    r.lease.withdraw(43);
    auto* l = wp_drm_lease_request_v1_submit(req);
    wp_drm_lease_v1_add_listener(l, &kLease, &r.log);
    r.pump();
    EXPECT_EQ(r.log.finished, 1);
    EXPECT_TRUE(r.gpu.granted.empty());

    r.gpu.refuse = true;
    wp_drm_lease_v1* l2 = r.ask({42});
    EXPECT_EQ(r.log.finished, 2);
    EXPECT_NE(r.log.find(42), nullptr);  // still on offer
    wp_drm_lease_v1_destroy(l);
    wp_drm_lease_v1_destroy(l2);
    r.pump();
    EXPECT_TRUE(r.gpu.revoked.empty());
    EXPECT_EQ(r.error(), 0);
}

TEST(WlDrmLease, DuplicateConnectorIsAnError) {
    Rig r;
    auto* req = wp_drm_lease_device_v1_create_lease_request(r.device);
    wp_drm_lease_request_v1_request_connector(req, r.log.find(42)->obj);
    wp_drm_lease_request_v1_request_connector(req, r.log.find(42)->obj);
    r.pump();
    EXPECT_TRUE(r.posted("wp_drm_lease_request_v1", WP_DRM_LEASE_REQUEST_V1_ERROR_DUPLICATE_CONNECTOR));
}

TEST(WlDrmLease, EmptyLeaseIsAnError) {
    Rig r;
    auto* req = wp_drm_lease_device_v1_create_lease_request(r.device);
    wp_drm_lease_request_v1_submit(req);
    r.pump();
    EXPECT_TRUE(r.posted("wp_drm_lease_request_v1", WP_DRM_LEASE_REQUEST_V1_ERROR_EMPTY_LEASE));
}

TEST(WlDrmLease, ClientGoneRevokes) {
    Rig r;
    r.ask({42});
    ASSERT_EQ(r.lease.active_leases(), 1u);
    r.disconnect();
    EXPECT_EQ(r.gpu.revoked, std::vector<uint32_t>{100});
    EXPECT_EQ(r.lease.active_leases(), 0u);
}
