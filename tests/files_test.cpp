#include "files.hpp"
#include "list_sync.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

using namespace atrium;
using namespace atrium::files;

TEST(Files, IconsBySuffix) {
    EXPECT_EQ(icon_for("PDF", false), "application-pdf");
    EXPECT_EQ(icon_for("tar", false), "package-x-generic");
    EXPECT_EQ(icon_for("pdf", true), "folder");
    EXPECT_EQ(icon_for("weird", false), "text-x-generic");
    EXPECT_EQ(icon_for("", false), "text-x-generic");
    EXPECT_TRUE(is_image("JPG"));
    EXPECT_FALSE(is_image("pdf"));
}

TEST(Files, ParsesLaunchers) {
    const Launcher l = parse_launcher(
        "#comment\n[Desktop Entry]\nType=Application\nName[de]=Spiel\nName = Portal 2 \r\nIcon=steam_icon_620\n"
        "[Desktop Action x]\nName=Other\n");
    EXPECT_EQ(l.name, "Portal 2");
    EXPECT_EQ(l.icon, "steam_icon_620");
    const Launcher none = parse_launcher("[Desktop Action x]\nName=Other\nIcon=y");
    EXPECT_EQ(none.name, "");
    EXPECT_EQ(none.icon, "");
}

TEST(Files, FreeNames) {
    std::set<std::string> have{"New Folder", "New Folder 2"};
    EXPECT_EQ(free_name("New Folder", [&](const std::string& n) { return have.contains(n); }), "New Folder 3");
    EXPECT_EQ(free_name("Other", [&](const std::string& n) { return have.contains(n); }), "Other");
    EXPECT_TRUE(valid_name("a.txt"));
    EXPECT_FALSE(valid_name(""));
    EXPECT_FALSE(valid_name(".."));
    EXPECT_FALSE(valid_name("a/b"));
}

namespace {

struct Item {
    char key;
    int value;
    bool operator==(const Item&) const = default;
};

struct Log {
    int inserts = 0, moves = 0, removes = 0, changes = 0;
    template <class F> void insert(int, F f) { ++inserts; f(); }
    template <class F> void move(int, int, F f) { ++moves; f(); }
    template <class F> void remove(int, F f) { ++removes; f(); }
    template <class F> void change(int, F f) { ++changes; f(); }
};

std::vector<Item> items(const std::string& keys, int value = 0) {
    std::vector<Item> out;
    for (char c : keys)
        out.push_back({c, value});
    return out;
}

} // namespace

TEST(ListSync, KeepsWhatStays) {
    const auto key = [](const Item& i) { return i.key; };
    std::vector<Item> cur = items("abcde");
    Log log;
    sync_list(cur, items("aecx"), key, log);
    EXPECT_EQ(cur, items("aecx"));
    EXPECT_EQ(log.inserts, 1);
    EXPECT_EQ(log.moves, 2);    // e and c come forward
    EXPECT_EQ(log.removes, 2);  // b, d
    EXPECT_EQ(log.changes, 0);

    Log again;
    sync_list(cur, items("aecx", 1), key, again);
    EXPECT_EQ(cur, items("aecx", 1));
    EXPECT_EQ(again.changes, 4);
    EXPECT_EQ(again.inserts + again.moves + again.removes, 0);

    Log empty;
    sync_list(cur, {}, key, empty);
    EXPECT_TRUE(cur.empty());
    EXPECT_EQ(empty.removes, 4);
}

#include "levels.hpp"

TEST(Levels, StepsOnTheGrid) {
    using atrium::levels::step;
    EXPECT_DOUBLE_EQ(step(0.5, 1, 16), 0.5625);
    EXPECT_DOUBLE_EQ(step(0.5, -1, 16), 0.4375);
    EXPECT_DOUBLE_EQ(step(0.53, 1, 16), 0.5625);   // off the grid: to the next line
    EXPECT_DOUBLE_EQ(step(0.53, -1, 16), 0.5);
    EXPECT_DOUBLE_EQ(step(0.5625 - 1e-6, 1, 16), 0.625);  // rounding noise is on the line
    EXPECT_DOUBLE_EQ(step(1.0, 1, 16), 1.0);
    EXPECT_DOUBLE_EQ(step(0.0, -1, 16), 0.0);
    EXPECT_DOUBLE_EQ(step(1.3, -1, 16), 0.9375);
    EXPECT_DOUBLE_EQ(step(0.5, 1, 64), 0.515625);
}

#include "sysinfo_core.hpp"

#include <sstream>

TEST(SysInfo, ReadsOsRelease) {
    using atrium::sysinfo::os_release_value;
    const char* text = "NAME=\"CachyOS Linux\"\nPRETTY_NAME=\"CachyOS\"\nID=cachyos\nLOGO=cachyos\n";
    EXPECT_EQ(os_release_value(text, "NAME"), "CachyOS Linux");
    EXPECT_EQ(os_release_value(text, "ID"), "cachyos");
    EXPECT_EQ(os_release_value(text, "LOGO"), "cachyos");
    EXPECT_EQ(os_release_value(text, "VERSION"), "");
}

TEST(SysInfo, NamesPciDevices) {
    using atrium::sysinfo::pci_device_name;
    const std::string ids =
        "# comment\n"
        "10de  NVIDIA Corporation\n"
        "\t2d04  GB206 [GeForce RTX 5060 Ti]\n"
        "\t\t1043 8a2b  some subsystem\n"
        "\t2d05  Plain Name\n"
        "1002  Advanced Micro Devices, Inc. [AMD/ATI]\n"
        "\t164e  Raphael\n"
        "C 00  Unclassified device\n";
    std::istringstream a(ids), b(ids), c(ids), d(ids);
    EXPECT_EQ(pci_device_name(a, 0x10de, 0x2d04), "NVIDIA GeForce RTX 5060 Ti");
    EXPECT_EQ(pci_device_name(b, 0x10de, 0x2d05), "NVIDIA Plain Name");
    EXPECT_EQ(pci_device_name(c, 0x1002, 0x164e), "AMD/ATI Raphael");
    EXPECT_EQ(pci_device_name(d, 0x10de, 0x9999), "");
}

TEST(SysInfo, TidiesCpuNames) {
    using atrium::sysinfo::tidy_cpu_name;
    EXPECT_EQ(tidy_cpu_name("AMD Ryzen 7 7800X3D 8-Core Processor"), "AMD Ryzen 7 7800X3D");
    EXPECT_EQ(tidy_cpu_name("Intel(R) Core(TM) i7-8700K CPU @ 3.70GHz"), "Intel Core i7-8700K");
}

TEST(SysInfo, InstalledMemoryFromUdev) {
    using atrium::sysinfo::installed_memory;
    const char* dmi =
        "E:MEMORY_ARRAY_NUM_DEVICES=4\n"
        "E:MEMORY_DEVICE_1_SIZE=34359738368\n"
        "E:MEMORY_DEVICE_1_VOLATILE_SIZE=34359738368\n"
        "E:MEMORY_DEVICE_1_TYPE=DDR5\n"
        "E:MEMORY_DEVICE_1_CONFIGURED_SPEED_MTS=6000\n"
        "E:MEMORY_DEVICE_3_SIZE=34359738368\n"
        "E:MEMORY_DEVICE_3_TYPE=DDR5\n";
    EXPECT_EQ(installed_memory(dmi), "64 GB DDR5-6000");
    EXPECT_EQ(installed_memory("E:OTHER=1\n"), "");
}

TEST(SysInfo, LargestPciWindow) {
    using atrium::sysinfo::largest_bar;
    EXPECT_EQ(largest_bar("0x00000000fb000000 0x00000000fbffffff 0x0000000000040200\n"
                          "0x0000007800000000 0x0000007bffffffff 0x000000000014220c\n"
                          "0x0000000000000000 0x0000000000000000 0x0000000000000000\n"),
              0x400000000ull);
    EXPECT_EQ(largest_bar(""), 0ull);
}

#include "netspeed_core.hpp"

TEST(NetSpeed, SumsRealInterfaces) {
    using namespace atrium::netspeed;
    const char* dev =
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
        "    lo: 5000 10 0 0 0 0 0 0 5000 10 0 0 0 0 0 0\n"
        "enp6s0: 1000 5 0 0 0 0 0 0 200 3 0 0 0 0 0 0\n"
        " wlan0: 300 1 0 0 0 0 0 0 50 1 0 0 0 0 0 0\n"
        "docker0: 9999 1 0 0 0 0 0 0 9999 1 0 0 0 0 0 0\n";
    const Totals t = read_totals(dev);
    EXPECT_EQ(t.rx, 1300u);
    EXPECT_EQ(t.tx, 250u);
    EXPECT_FALSE(counts("veth12ab"));
    EXPECT_TRUE(counts("enp6s0"));
}

TEST(NetSpeed, FormatsRates) {
    using atrium::netspeed::format_rate;
    EXPECT_EQ(format_rate(0), "0 KB/s");
    EXPECT_EQ(format_rate(340'000), "340 KB/s");
    EXPECT_EQ(format_rate(1'234'000), "1.2 MB/s");
    EXPECT_EQ(format_rate(12'400'000), "12 MB/s");
    EXPECT_EQ(format_rate(1'100'000'000), "1.1 GB/s");
}

// --- changing your password through passwd ------------------------------------

#include "passwd_core.hpp"
#include <crypt.h>
#include "paths.hpp"

namespace {
const std::string kFakePasswd = std::string(ATRIUM_SOURCE_DIR) + "/tests/data/fake-passwd";
}

TEST(Passwd, AnswersThePromptsAndSucceeds) {
    const auto r = atrium::passwd::change(kFakePasswd, "old", "correct horse");
    EXPECT_TRUE(r.ok) << r.message;
}

TEST(Passwd, AWrongCurrentPasswordSaysWhy) {
    const auto r = atrium::passwd::change(kFakePasswd, "nope", "correct horse");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.message, "Your current password is incorrect.");
}

TEST(Passwd, ARejectedNewPasswordStopsInsteadOfLooping) {
    const auto r = atrium::passwd::change(kFakePasswd, "old", "ab", 5000);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.message, "The password is shorter than 4 characters.");
}

TEST(Passwd, AccountNames) {
    EXPECT_TRUE(atrium::passwd::valid_user_name("jane"));
    EXPECT_TRUE(atrium::passwd::valid_user_name("jane_doe-2"));
    EXPECT_FALSE(atrium::passwd::valid_user_name("Jane"));
    EXPECT_FALSE(atrium::passwd::valid_user_name("2jane"));
    EXPECT_FALSE(atrium::passwd::valid_user_name("jane doe"));
    EXPECT_FALSE(atrium::passwd::valid_user_name(""));
    EXPECT_EQ(atrium::passwd::suggest_user_name("Jane Q. Doe"), "janedoe");
    EXPECT_EQ(atrium::passwd::suggest_user_name("  Plato "), "plato");
    EXPECT_EQ(atrium::passwd::suggest_user_name("3 Musketeers Club"), "musketeersclub");
    EXPECT_TRUE(atrium::passwd::valid_user_name(atrium::passwd::suggest_user_name("Mr Konic")));
}

TEST(Passwd, HashesTheWayShadowExpects) {
    const std::string h = atrium::passwd::hash("correct horse");
    ASSERT_FALSE(h.empty());
    EXPECT_TRUE(h.starts_with("$"));
    EXPECT_NE(h, atrium::passwd::hash("correct horse"));  // salted
    EXPECT_EQ(std::string(crypt("correct horse", h.c_str())), h);  // and it checks out
}
