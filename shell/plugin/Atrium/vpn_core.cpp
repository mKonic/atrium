#include "vpn_core.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <charconv>

namespace atrium::vpn {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// "Key=value" with the key matched as wg does, ignoring case.
bool match(std::string_view line, std::string_view key, std::string_view& value) {
    if (line.size() <= key.size() || line[key.size()] != '=' || lower(line.substr(0, key.size())) != lower(key))
        return false;
    value = line.substr(key.size() + 1);
    return true;
}

std::vector<std::string_view> words(std::string_view v) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start <= v.size()) {
        const size_t comma = v.find(',', start);
        const std::string_view w = v.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        if (!w.empty())
            out.push_back(w);
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    return out;
}

std::optional<long long> number(std::string_view v, long long max) {
    long long n = 0;
    int base = 10;
    if (v.starts_with("0x") || v.starts_with("0X")) {
        v.remove_prefix(2);
        base = 16;
    }
    const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), n, base);
    if (ec != std::errc() || end != v.data() + v.size() || n < 0 || n > max)
        return std::nullopt;
    return n;
}

bool ip(std::string_view s, bool& v6) {
    const std::string str(s);
    unsigned char buf[16];
    if (inet_pton(AF_INET, str.c_str(), buf) == 1) {
        v6 = false;
        return true;
    }
    if (inet_pton(AF_INET6, str.c_str(), buf) == 1) {
        v6 = true;
        return true;
    }
    return false;
}

std::optional<Address> address(std::string_view s) {
    Address a;
    const size_t slash = s.find('/');
    if (!ip(s.substr(0, slash), a.v6))
        return std::nullopt;
    a.ip = std::string(s.substr(0, slash));
    a.prefix = a.v6 ? 128 : 32;
    if (slash != std::string_view::npos) {
        const auto p = number(s.substr(slash + 1), a.v6 ? 128 : 32);
        if (!p)
            return std::nullopt;
        a.prefix = int(*p);
    }
    return a;
}

bool endpoint(std::string_view s) {
    const size_t colon = s.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || !number(s.substr(colon + 1), 65535))
        return false;
    std::string_view host = s.substr(0, colon);
    if (host.starts_with('[')) {
        bool v6 = false;
        return host.ends_with(']') && ip(host.substr(1, host.size() - 2), v6) && v6;
    }
    return host.find(':') == std::string_view::npos;  // a v6 address needs its brackets
}

} // namespace

bool valid_key(std::string_view key) {
    // 32 bytes: 43 characters and one '=' of padding.
    if (key.size() != 44 || key[43] != '=')
        return false;
    for (size_t i = 0; i < 43; ++i) {
        const char c = key[i];
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '/')
            return false;
    }
    // The last character holds two bits that must be zero.
    return std::string_view("AEIMQUYcgkosw048").find(key[42]) != std::string_view::npos;
}

Parsed parse(std::string_view text) {
    Config c;
    enum { Init, Interface, PeerSection } context = Init;
    bool have_key = false;
    size_t line_nr = 0;
    auto fail = [&](std::string_view why) { return Parsed{std::nullopt, "line " + std::to_string(line_nr) + ": " + std::string(why)}; };

    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos)
            end = text.size();
        const std::string_view raw = text.substr(pos, end - pos);
        pos = end + 1;
        ++line_nr;
        // As wg reads it: every space dropped, cut at '#'.
        std::string line;
        for (char ch : raw) {
            if (ch == '#')
                break;
            if (!std::isspace(static_cast<unsigned char>(ch)))
                line += ch;
        }
        if (line.empty())
            continue;
        if (lower(line) == "[interface]") {
            context = Interface;
            continue;
        }
        if (lower(line) == "[peer]") {
            context = PeerSection;
            c.peers.emplace_back();
            continue;
        }
        std::string_view v;
        if (context == Interface) {
            if (match(line, "Address", v)) {
                for (std::string_view w : words(v)) {
                    const auto a = address(w);
                    if (!a)
                        return fail("invalid Address");
                    c.addresses.push_back(*a);
                }
            } else if (match(line, "DNS", v)) {
                for (std::string_view w : words(v)) {
                    bool v6 = false;
                    if (ip(w, v6))
                        (v6 ? c.dns6 : c.dns4).emplace_back(w);
                    else
                        c.dns_search.emplace_back(w);
                }
            } else if (match(line, "MTU", v)) {
                const auto n = number(v, 0xffffffff);
                if (!n)
                    return fail("invalid MTU");
                c.mtu = unsigned(*n);
            } else if (match(line, "Table", v)) {
                if (v == "auto")
                    c.table = kTableAuto;
                else if (v == "off")
                    c.table = kTableOff;
                else if (const auto n = number(v, 0x7fffffff))
                    c.table = *n;
                else
                    return fail("invalid Table");
            } else if (match(line, "PreUp", v) || match(line, "PreDown", v) || match(line, "PostUp", v) || match(line, "PostDown", v)) {
                // Scripts aren't run, as NetworkManager runs none.
            } else if (match(line, "SaveConfig", v)) {
                if (v != "true" && v != "false")
                    return fail("invalid SaveConfig");
            } else if (match(line, "ListenPort", v)) {
                const auto n = number(v, 65535);
                if (!n)
                    return fail("invalid ListenPort");
                c.listen_port = unsigned(*n);
            } else if (match(line, "FwMark", v)) {
                if (v == "off")
                    c.fwmark = 0;
                else if (const auto n = number(v, 0x7fffffff))
                    c.fwmark = unsigned(*n);
                else
                    return fail("invalid FwMark");
            } else if (match(line, "PrivateKey", v)) {
                if (!valid_key(v))
                    return fail("invalid PrivateKey");
                c.private_key = std::string(v);
                have_key = true;
            } else {
                return fail("unrecognized");
            }
            continue;
        }
        if (context == PeerSection) {
            Peer& p = c.peers.back();
            if (match(line, "Endpoint", v)) {
                if (!endpoint(v))
                    return fail("invalid Endpoint");
                p.endpoint = std::string(v);
            } else if (match(line, "PublicKey", v)) {
                if (!valid_key(v))
                    return fail("invalid PublicKey");
                p.public_key = std::string(v);
            } else if (match(line, "AllowedIPs", v)) {
                for (std::string_view w : words(v)) {
                    if (!address(w))
                        return fail("invalid AllowedIPs");
                    p.allowed_ips.emplace_back(w);
                }
            } else if (match(line, "PersistentKeepalive", v)) {
                if (v == "off")
                    p.keepalive = 0;
                else if (const auto n = number(v, 0xffff))
                    p.keepalive = int(*n);
                else
                    return fail("invalid PersistentKeepalive");
            } else if (match(line, "PresharedKey", v)) {
                if (!valid_key(v))
                    return fail("invalid PresharedKey");
                p.preshared_key = std::string(v);
            } else {
                return fail("unrecognized");
            }
            continue;
        }
        return fail("outside [Interface] and [Peer]");
    }
    for (const Peer& p : c.peers)
        if (p.public_key.empty())
            return Parsed{std::nullopt, "a [Peer] without a PublicKey"};
    if (!have_key)
        return Parsed{std::nullopt, "no PrivateKey"};
    return Parsed{std::move(c), {}};
}

std::optional<std::pair<std::string, std::string>> location_of(std::string_view id) {
    // Mullvad's hostnames: country, city, "wg", number.
    if (id.size() < 11 || id[2] != '-' || id[6] != '-' || id.substr(7, 3) != "wg-")
        return std::nullopt;
    auto letters = [](std::string_view s) {
        return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::islower(c); });
    };
    if (!letters(id.substr(0, 2)) || !letters(id.substr(3, 3)))
        return std::nullopt;
    return std::pair{std::string(id.substr(0, 2)), std::string(id.substr(3, 3))};
}

std::string tunnel_name(std::string_view source, bool single) {
    std::string_view base = source.substr(source.rfind('/') + 1);
    if (const size_t dot = base.find('.'); dot != std::string_view::npos)
        base = base.substr(0, dot);
    if (single)
        return std::string(base);
    const size_t cut = base.find_first_of("_-. ");
    std::string out(base.substr(0, cut));
    if (out.empty())
        return "VPN";
    out[0] = char(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

Gathered gather(std::string_view source, const std::vector<std::pair<std::string, std::string>>& files) {
    Gathered out;
    std::vector<std::pair<std::string, Config>> configs;
    for (const auto& [name, text] : files) {
        Parsed p = parse(text);
        if (!p.config) {
            out.errors.push_back(name + ": " + p.error);
            continue;
        }
        std::string id = name.substr(name.rfind('/') + 1);
        if (id.ends_with(".conf"))
            id.resize(id.size() - 5);
        configs.emplace_back(id, std::move(*p.config));
    }
    std::sort(configs.begin(), configs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    // One interface: the same key, addresses, DNS and settings.
    auto same = [](const Config& a, const Config& b) {
        return a.private_key == b.private_key && a.addresses == b.addresses && a.dns4 == b.dns4 && a.dns6 == b.dns6
               && a.dns_search == b.dns_search && a.mtu == b.mtu && a.listen_port == b.listen_port
               && a.fwmark == b.fwmark && a.table == b.table;
    };
    const bool single = configs.size() == 1;
    for (auto& [id, config] : configs) {
        if (config.peers.size() == 1) {
            auto it = std::find_if(out.tunnels.begin(), out.tunnels.end(),
                                   [&](const Tunnel& t) { return !t.servers.empty() && same(t.config, config); });
            if (it == out.tunnels.end()) {
                Tunnel t;
                t.config = config;
                t.config.peers.clear();
                out.tunnels.push_back(std::move(t));
                it = std::prev(out.tunnels.end());
            }
            Server s{id, config.peers.front(), {}, {}};
            if (const auto loc = location_of(id)) {
                s.country = loc->first;
                s.city = loc->second;
            }
            it->servers.push_back(std::move(s));
        } else {
            out.tunnels.push_back(Tunnel{id, std::move(config), {}});
        }
    }
    // Names: a lone file keeps its own; a download's tunnels its name.
    const std::string base = tunnel_name(source, single);
    int n = 0;
    for (Tunnel& t : out.tunnels) {
        if (!t.name.empty() && !single)
            continue;
        if (t.servers.size() == 1 && !single)
            t.name = t.servers.front().id;
        else
            t.name = ++n == 1 ? base : base + " " + std::to_string(n);
    }
    return out;
}

bool within(std::string_view place, std::string_view country, std::string_view city) {
    if (place.empty())
        return true;
    const size_t slash = place.find('/');
    if (place.substr(0, slash) != country)
        return false;
    return slash == std::string_view::npos || place.substr(slash + 1) == city;
}

std::vector<std::string> used(std::vector<std::string> recent, const std::string& place, size_t keep) {
    std::erase(recent, place);
    recent.insert(recent.begin(), place);
    if (recent.size() > keep)
        recent.resize(keep);
    return recent;
}

std::vector<std::string> by_recent(const std::vector<std::string>& codes, const std::vector<std::string>& recent,
                                   std::string_view prefix) {
    std::vector<std::string> out;
    for (const std::string& r : recent) {
        if (!r.starts_with(prefix))
            continue;
        // "se/sto" ranks "se" among countries, "sto" among Sweden's cities.
        std::string_view code = std::string_view(r).substr(prefix.size());
        code = code.substr(0, code.find('/'));
        if (std::find(codes.begin(), codes.end(), code) != codes.end() && std::find(out.begin(), out.end(), code) == out.end())
            out.emplace_back(code);
    }
    for (const std::string& c : codes)
        if (std::find(out.begin(), out.end(), c) == out.end())
            out.push_back(c);
    return out;
}

std::string interface_name(std::string_view name, const std::vector<std::string>& taken) {
    std::string clean;
    for (char c : name)
        if (std::isalnum(static_cast<unsigned char>(c)))
            clean += char(std::tolower(static_cast<unsigned char>(c)));
    if (clean.empty())
        clean = "vpn";
    std::string out = ("wg-" + clean).substr(0, 15);
    for (int i = 2; std::find(taken.begin(), taken.end(), out) != taken.end(); ++i) {
        const std::string suffix = std::to_string(i);
        out = ("wg-" + clean).substr(0, 15 - suffix.size()) + suffix;
    }
    return out;
}

} // namespace atrium::vpn
