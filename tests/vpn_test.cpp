#include "vpn_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::vpn;

namespace {

// Keys in WireGuard's form, made up.
const std::string kPrivate = std::string(42, 'P') + "A=";
const std::string kPeerA = std::string(42, 'a') + "E=";
const std::string kPeerB = std::string(42, 'b') + "Q=";

// As Mullvad writes one server's file.
std::string mullvad(const std::string& peer, const std::string& endpoint) {
    return "[Interface]\n"
           "# Device: Tidy Hermit\n"
           "PrivateKey = " + kPrivate + "\n"
           "Address = 10.72.70.136/32,fc00:bbbb:bbbb:bb01::9:4687/128\n"
           "DNS = 100.64.0.1\n"
           "\n"
           "[Peer]\n"
           "PublicKey = " + peer + "\n"
           "AllowedIPs = 0.0.0.0/0,::0/0\n"
           "Endpoint = " + endpoint + "\n";
}

} // namespace

TEST(Vpn, ReadsAMullvadFile) {
    const Parsed p = parse(mullvad(kPeerA, "154.47.30.143:51820"));
    ASSERT_TRUE(p.config) << p.error;
    const Config& c = *p.config;
    EXPECT_EQ(c.private_key, kPrivate);
    ASSERT_EQ(c.addresses.size(), 2u);
    EXPECT_EQ(c.addresses[0], (Address{"10.72.70.136", 32, false}));
    EXPECT_EQ(c.addresses[1], (Address{"fc00:bbbb:bbbb:bb01::9:4687", 128, true}));
    EXPECT_EQ(c.dns4, std::vector<std::string>{"100.64.0.1"});
    EXPECT_TRUE(c.dns_search.empty());
    ASSERT_EQ(c.peers.size(), 1u);
    EXPECT_EQ(c.peers[0].public_key, kPeerA);
    EXPECT_EQ(c.peers[0].endpoint, "154.47.30.143:51820");
    EXPECT_EQ(c.peers[0].allowed_ips, (std::vector<std::string>{"0.0.0.0/0", "::0/0"}));
}

TEST(Vpn, ReadsAsWgDoes) {
    // Keys in any case, spaces anywhere, comments, the rest of wg-quick's keys.
    const Parsed p = parse("[ interface ]\n"
                           "privatekey=" + kPrivate + " # mine\n"
                           "Address = 10.0.0.2\n"
                           "DNS = 1.1.1.1, 2606:4700::1111, corp.example\n"
                           "MTU = 1420\nListenPort = 51000\nFwMark = 0x10\nTable = off\n"
                           "PostUp = iptables -A FORWARD\nSaveConfig = false\n"
                           "[Peer]\nPublicKey = " + kPeerA + "\nPresharedKey = " + kPeerB + "\n"
                           "Endpoint = [2001:db8::1]:51820\nPersistentKeepalive = 25\n");
    ASSERT_TRUE(p.config) << p.error;
    const Config& c = *p.config;
    EXPECT_EQ(c.addresses[0].prefix, 32);  // no prefix: the address alone
    EXPECT_EQ(c.dns6, std::vector<std::string>{"2606:4700::1111"});
    EXPECT_EQ(c.dns_search, std::vector<std::string>{"corp.example"});
    EXPECT_EQ(c.mtu, 1420u);
    EXPECT_EQ(c.listen_port, 51000u);
    EXPECT_EQ(c.fwmark, 16u);
    EXPECT_EQ(c.table, kTableOff);
    EXPECT_EQ(c.peers[0].preshared_key, kPeerB);
    EXPECT_EQ(c.peers[0].keepalive, 25);
}

TEST(Vpn, WrongFilesSaySo) {
    EXPECT_FALSE(parse("[Interface]\nAddress = 10.0.0.2/32\n").config);  // no key
    EXPECT_FALSE(parse("[Interface]\nPrivateKey = short=\n").config);
    EXPECT_FALSE(parse("PrivateKey = " + kPrivate + "\n").config);      // outside a section
    EXPECT_FALSE(parse("[Interface]\nPrivateKey = " + kPrivate + "\n[Peer]\nEndpoint = 1.2.3.4:5\n").config);
    EXPECT_FALSE(parse("[Interface]\nPrivateKey = " + kPrivate + "\n[Peer]\nPublicKey = " + kPeerA + "\nEndpoint = 2001:db8::1:51820\n").config);
    const Parsed bad = parse("[Interface]\nPrivateKey = " + kPrivate + "\nAddress = 10.0.0.300/32\n");
    EXPECT_EQ(bad.error, "line 3: invalid Address");
    EXPECT_EQ(parse("[Interface]\nColour = blue\n").error, "line 2: unrecognized");
}

TEST(Vpn, Keys) {
    EXPECT_TRUE(valid_key(kPrivate));
    EXPECT_FALSE(valid_key(std::string(42, 'a') + "B="));  // bits past the 32 bytes
    EXPECT_FALSE(valid_key(std::string(43, 'a')));
    EXPECT_FALSE(valid_key(std::string(42, 'a') + "!E"));
}

TEST(Vpn, AMullvadDownloadIsOneTunnel) {
    const Gathered g = gather("/home/u/Downloads/mullvad_wireguard_linux_all_all.zip",
                              {{"se-sto-wg-001.conf", mullvad(kPeerA, "1.2.3.4:51820")},
                               {"al-tia-wg-001.conf", mullvad(kPeerB, "5.6.7.8:51820")},
                               {"broken.conf", "nonsense"}});
    ASSERT_EQ(g.tunnels.size(), 1u);
    const Tunnel& t = g.tunnels[0];
    EXPECT_EQ(t.name, "Mullvad");
    EXPECT_TRUE(t.config.peers.empty());
    ASSERT_EQ(t.servers.size(), 2u);
    EXPECT_EQ(t.servers[0].id, "al-tia-wg-001");  // in order
    EXPECT_EQ(t.servers[0].country, "al");
    EXPECT_EQ(t.servers[0].city, "tia");
    EXPECT_EQ(t.servers[1].peer.public_key, kPeerA);
    ASSERT_EQ(g.errors.size(), 1u);
    EXPECT_EQ(g.errors[0].rfind("broken.conf: ", 0), 0u);
}

TEST(Vpn, OtherFilesAreTheirOwn) {
    // A lone file: its own name. Two peers in one file: one tunnel with both.
    const Gathered one = gather("/tmp/office.conf", {{"office.conf", mullvad(kPeerA, "1.2.3.4:1")}});
    ASSERT_EQ(one.tunnels.size(), 1u);
    EXPECT_EQ(one.tunnels[0].name, "office");
    EXPECT_EQ(one.tunnels[0].servers.size(), 1u);

    std::string two = mullvad(kPeerA, "1.2.3.4:1") + "[Peer]\nPublicKey = " + kPeerB + "\nAllowedIPs = 10.9.0.0/16\n";
    const Gathered g = gather("work.zip", {{"site.conf", two}, {"home.conf", mullvad(kPeerA, "9.9.9.9:1")}});
    ASSERT_EQ(g.tunnels.size(), 2u);
    EXPECT_EQ(g.tunnels[0].name, "home");  // one server of its own
    EXPECT_EQ(g.tunnels[1].name, "site");
    EXPECT_EQ(g.tunnels[1].config.peers.size(), 2u);
    EXPECT_TRUE(g.tunnels[1].servers.empty());
}

TEST(Vpn, Names) {
    EXPECT_EQ(location_of("us-nyc-wg-301"), (std::pair<std::string, std::string>{"us", "nyc"}));
    EXPECT_FALSE(location_of("office"));
    EXPECT_FALSE(location_of("US-NYC-wg-301"));
    EXPECT_EQ(tunnel_name("x/proton-configs.zip", false), "Proton");
    EXPECT_EQ(interface_name("Mullvad", {}), "wg-mullvad");
    EXPECT_EQ(interface_name("Mullvad", {"wg-mullvad"}), "wg-mullvad2");
    EXPECT_EQ(interface_name("A very long provider", {}), "wg-averylongpro");
    EXPECT_EQ(interface_name("A very long provider", {"wg-averylongpro"}), "wg-averylongpr2");
}

TEST(Vpn, Places) {
    EXPECT_TRUE(within("", "se", "sto"));
    EXPECT_TRUE(within("se", "se", "got"));
    EXPECT_TRUE(within("se/sto", "se", "sto"));
    EXPECT_FALSE(within("se/sto", "se", "got"));
    EXPECT_FALSE(within("de", "se", "sto"));
    EXPECT_FALSE(within("s", "se", "sto"));  // not a prefix match
}

TEST(Vpn, RecentlyUsed) {
    std::vector<std::string> r;
    r = used(r, "se/sto");
    r = used(r, "de");
    r = used(r, "se/sto");  // again: to the front, once
    EXPECT_EQ(r, (std::vector<std::string>{"se/sto", "de"}));
    for (int i = 0; i < 20; ++i)
        r = used(r, "x" + std::to_string(i), 3);
    EXPECT_EQ(r.size(), 3u);

    const std::vector<std::string> countries{"al", "de", "se", "us"};
    EXPECT_EQ(by_recent(countries, {"se/sto", "nl", "de", "se/got"}), (std::vector<std::string>{"se", "de", "al", "us"}));
    EXPECT_EQ(by_recent({"got", "mma", "sto"}, {"se/sto", "de", "se"}, "se/"), (std::vector<std::string>{"sto", "got", "mma"}));
    EXPECT_EQ(by_recent(countries, {}), countries);
}
