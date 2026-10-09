#pragma once
// WireGuard configurations as wg-quick writes them, read the way
// NetworkManager's `nmcli connection import type wireguard` reads them
// (nm_conn_wireguard_import), without Qt; and a provider's download (Mullvad
// gives one file per server) gathered into one tunnel with its servers.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace atrium::vpn {

struct Address {
    std::string ip;
    int prefix = 0;
    bool v6 = false;
    bool operator==(const Address&) const = default;
};

struct Peer {
    std::string public_key;
    std::string endpoint;  // host:port, [v6]:port
    std::string preshared_key;
    std::vector<std::string> allowed_ips;
    int keepalive = 0;  // seconds; 0 off
    bool operator==(const Peer&) const = default;
};

constexpr long long kTableAuto = -1, kTableOff = -2;

struct Config {
    std::string private_key;
    std::vector<Address> addresses;
    std::vector<std::string> dns4, dns6, dns_search;
    unsigned mtu = 0, listen_port = 0, fwmark = 0;
    long long table = kTableAuto;
    std::vector<Peer> peers;
};

struct Parsed {
    std::optional<Config> config;
    std::string error;  // "line 4: unrecognized" and the like
};
Parsed parse(std::string_view text);

// A key as WireGuard writes it: 32 bytes in base64.
bool valid_key(std::string_view key);

// One server of a tunnel: its file's name (Mullvad's hostname,
// "se-sto-wg-001") and its peer.
struct Server {
    std::string id;
    Peer peer;
    std::string country;  // "se", from a Mullvad-style name; else empty
    std::string city;     // "sto"
};

struct Tunnel {
    std::string name;
    Config config;                // with its peers when it has no servers
    std::vector<Server> servers;  // one peer each; the first in use
};

// Configurations (file name, text) from one import, gathered: those with one
// peer and the same interface (key, addresses, DNS) are one tunnel's
// servers; the rest are tunnels of their own. `source` names the download
// ("mullvad_wireguard_linux_all_all.zip" → "Mullvad"). Unreadable files are
// left out, and said in `errors`.
struct Gathered {
    std::vector<Tunnel> tunnels;
    std::vector<std::string> errors;
};
Gathered gather(std::string_view source, const std::vector<std::pair<std::string, std::string>>& files);

// The name an import is known by: a single file's own name, a download's
// first word capitalized.
std::string tunnel_name(std::string_view source, bool single);

// "se-sto-wg-001" → {"se", "sto"}; nothing for any other name.
std::optional<std::pair<std::string, std::string>> location_of(std::string_view id);

// A place to connect to: "se" a country, "se/sto" a city in it.
// Whether a server in `country`, `city` is in `place`; an empty place is
// anywhere.
bool within(std::string_view place, std::string_view country, std::string_view city);
// `place` at the front of the recently used, once, at most `keep` of them.
std::vector<std::string> used(std::vector<std::string> recent, const std::string& place, size_t keep = 8);
// `codes` (in their usual order) with the recently used first, most
// recent first: a country counts as used when any of its cities was
// (`prefix` "se/" ranks the cities of Sweden).
std::vector<std::string> by_recent(const std::vector<std::string>& codes, const std::vector<std::string>& recent,
                                   std::string_view prefix = {});

// The tunnel's network interface: "wg-" and its name, at most 15
// characters, not one of `taken`.
std::string interface_name(std::string_view name, const std::vector<std::string>& taken);

} // namespace atrium::vpn
