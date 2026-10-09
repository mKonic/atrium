#include "ddc_core.hpp"

namespace atrium::ddc {

namespace {

// A request's checksum covers the address it goes to (0x6E, the monitor's
// write address) as well as the message.
constexpr uint8_t kDestination = kAddress << 1;
// A reply's checksum starts from 0x50 instead of the host's address.
constexpr uint8_t kReplySeed = 0x50;
constexpr uint8_t kReplySource = 0x6e;
constexpr uint8_t kGetReplyType = 0x02;

template <size_t N>
std::array<uint8_t, N> sealed(std::array<uint8_t, N> msg) {
    uint8_t sum = kDestination;
    for (size_t i = 0; i + 1 < N; ++i)
        sum ^= msg[i];
    msg[N - 1] = sum;
    return msg;
}

} // namespace

std::array<uint8_t, 5> get_request(uint8_t code) {
    return sealed<5>({0x51, 0x82, 0x01, code, 0});
}

std::array<uint8_t, 7> set_request(uint8_t code, uint16_t value) {
    return sealed<7>({0x51, 0x84, 0x03, code, uint8_t(value >> 8), uint8_t(value & 0xff), 0});
}

Reply parse_get_reply(std::span<const uint8_t> bytes, uint8_t code, Vcp& out) {
    // Some monitors send the source address twice.
    if (bytes.size() > 2 && bytes[0] == kReplySource && bytes[1] == kReplySource)
        bytes = bytes.subspan(1);
    if (bytes.size() < 3 || bytes[0] != kReplySource)
        return Reply::Garbled;
    const size_t length = bytes[1] & 0x7f;
    if (bytes.size() < 3 + length)
        return Reply::Garbled;
    uint8_t sum = kReplySeed;
    for (size_t i = 0; i < 2 + length; ++i)
        sum ^= bytes[i];
    if (sum != bytes[2 + length])
        return Reply::Garbled;
    // 6E 80 BE: nothing to say yet.
    if (length == 0)
        return Reply::Null;
    const auto data = bytes.subspan(2, length);
    if (length != 8 || data[0] != kGetReplyType || data[2] != code)
        return Reply::Garbled;
    if (data[1] == 0x01)
        return Reply::Unsupported;
    if (data[1] != 0x00)
        return Reply::Garbled;
    out.max = uint16_t(data[4] << 8 | data[5]);
    out.current = uint16_t(data[6] << 8 | data[7]);
    return Reply::Ok;
}

bool ignorable_bus(std::string_view name, std::string_view driver, uint32_t pci_class) {
    // An MST hub's bus has no class of its own, and carries DDC/CI.
    if (name == "DPMST")
        return false;
    static constexpr std::string_view kNever[] = {
        "SMBus", "Synopsys DesignWare", "soc:i2cdsi", "smu", "mac-io", "u4", "AMDGPU SMU", "AMDGPU DM i2c OEM bus",
    };
    for (std::string_view prefix : kNever)
        if (name.starts_with(prefix))
            return true;
    if (driver == "nouveau" && !name.starts_with("nvkm-"))
        return true;
    if (pci_class == 0)
        return name.empty();
    const uint32_t base = pci_class & 0xffff0000;
    return base != 0x030000 && base != 0x0a0000;  // a display controller, or a dock
}

bool edid_header(std::span<const uint8_t> bytes) {
    static constexpr uint8_t kHeader[] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    if (bytes.size() < sizeof kHeader)
        return false;
    for (size_t i = 0; i < sizeof kHeader; ++i)
        if (bytes[i] != kHeader[i])
            return false;
    return true;
}

} // namespace atrium::ddc
