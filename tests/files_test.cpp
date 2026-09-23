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
