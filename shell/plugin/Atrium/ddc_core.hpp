#pragma once
// DDC/CI's messages and which I2C buses can carry it, as ddcutil
// (src/base/ddc_packets.c, src/sysfs/sysfs_simple.c): the request with its
// checksum, the reply checked the way ddcutil checks it, and the buses it
// never probes (an SMBus or an AMD SMU hangs when probed).

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace atrium::ddc {

constexpr uint8_t kAddress = 0x37;  // the monitor's DDC/CI address; EDID is at 0x50
constexpr uint8_t kBrightness = 0x10;
// ddcutil reads a Get VCP reply into a buffer this size: monitors (and
// NVIDIA's adapter) repeat the 11-byte reply, and a short read can come
// back as the first byte over and over.
constexpr size_t kReplyRead = 40;

std::array<uint8_t, 5> get_request(uint8_t code);
std::array<uint8_t, 7> set_request(uint8_t code, uint16_t value);

struct Vcp {
    uint16_t current = 0;
    uint16_t max = 0;
};

enum class Reply {
    Ok,
    Null,         // the monitor isn't ready: ask again, later
    Unsupported,  // it doesn't have the feature
    Garbled,      // the wrong source, length, type, feature or checksum
};

// What a Get VCP Feature reply says about `code`.
Reply parse_get_reply(std::span<const uint8_t> bytes, uint8_t code, Vcp& out);

// Whether ddcutil leaves an adapter alone: by its name (sysfs `name`, the
// kernel `driver`), and by the PCI class of the device above it (0 when
// there's no PCI device: a platform adapter, judged by its name only).
bool ignorable_bus(std::string_view name, std::string_view driver, uint32_t pci_class);

// The fixed 8-byte header every EDID starts with.
bool edid_header(std::span<const uint8_t> bytes);

} // namespace atrium::ddc
