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
