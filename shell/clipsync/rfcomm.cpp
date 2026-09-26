#include "rfcomm.hpp"

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace atrium::clipsync {

namespace {

bool parseUuid(std::string_view s, uint128_t& out) {
    int n = 0;
    for (const char c : s) {
        if (c == '-')
            continue;
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0 || n >= 32)
            return false;
        out.data[n / 2] = std::uint8_t(n % 2 ? (out.data[n / 2] | v) : v << 4);
        n++;
    }
    return n == 32;
}

// The RFCOMM channel the device lists for the service, or 0.
int lookup(const bdaddr_t& addr, std::string_view uuid, std::string& error) {
    uint128_t u{};
    if (!parseUuid(uuid, u)) {
        error = "bad service UUID";
        return 0;
    }
    bdaddr_t any{};
    sdp_session_t* session = sdp_connect(&any, &addr, SDP_RETRY_IF_BUSY);
    if (!session) {
        error = std::string("service lookup: ") + std::strerror(errno);
        return 0;
    }
    uuid_t service;
    sdp_uuid128_create(&service, &u);
    sdp_list_t* search = sdp_list_append(nullptr, &service);
    uint32_t range = 0x0000ffff;
    sdp_list_t* attrs = sdp_list_append(nullptr, &range);
    sdp_list_t* records = nullptr;
    int channel = 0;
    if (sdp_service_search_attr_req(session, search, SDP_ATTR_REQ_RANGE, attrs, &records) == 0) {
        for (sdp_list_t* r = records; r && !channel; r = r->next) {
            auto* record = static_cast<sdp_record_t*>(r->data);
            sdp_list_t* protos = nullptr;
            if (sdp_get_access_protos(record, &protos) == 0) {
                channel = sdp_get_proto_port(protos, RFCOMM_UUID);
                sdp_list_foreach(protos, [](void* p, void*) { sdp_list_free(static_cast<sdp_list_t*>(p), nullptr); }, nullptr);
                sdp_list_free(protos, nullptr);
            }
        }
        sdp_list_free(records, [](void* r) { sdp_record_free(static_cast<sdp_record_t*>(r)); });
    } else {
        error = std::string("service lookup: ") + std::strerror(errno);
    }
    if (!channel && error.empty())
        error = "the phone doesn't offer the service (is the module installed and running?)";
    sdp_list_free(search, nullptr);
    sdp_list_free(attrs, nullptr);
    sdp_close(session);
    return channel;
}

} // namespace

int connectService(const std::string& address, std::string_view uuid, std::string& error) {
    bdaddr_t addr;
    if (str2ba(address.c_str(), &addr) < 0) {
        error = "bad address";
        return -1;
    }
    const int channel = lookup(addr, uuid, error);
    if (!channel)
        return -1;
    const int fd = socket(AF_BLUETOOTH, SOCK_STREAM | SOCK_CLOEXEC, BTPROTO_RFCOMM);
    if (fd < 0) {
        error = std::strerror(errno);
        return -1;
    }
    // Authenticated and encrypted: only the paired phone, and nobody listening in.
    bt_security security{};
    security.level = BT_SECURITY_MEDIUM;
    setsockopt(fd, SOL_BLUETOOTH, BT_SECURITY, &security, sizeof security);
    sockaddr_rc to{};
    to.rc_family = AF_BLUETOOTH;
    to.rc_bdaddr = addr;
    to.rc_channel = std::uint8_t(channel);
    if (connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof to) < 0) {
        error = std::string("connect: ") + std::strerror(errno);
        close(fd);
        return -1;
    }
    return fd;
}

} // namespace atrium::clipsync
