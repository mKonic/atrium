#include "../shell/plugin/Atrium/chooser_core.hpp"

#include <gtest/gtest.h>

#include <ctime>

using namespace atrium::chooser;

TEST(ChooserCore, AcceptTable) {
    // GTK's should_respond(): open, save, select folder × 0, 1, many picked.
    EXPECT_EQ(on_accept(Action::Open, 0, false, false), Respond::Nothing);
    EXPECT_EQ(on_accept(Action::Open, 1, true, false), Respond::Enter);
    EXPECT_EQ(on_accept(Action::Open, 1, false, true), Respond::Answer);
    EXPECT_EQ(on_accept(Action::Open, 3, false, false), Respond::Nothing);
    EXPECT_EQ(on_accept(Action::Open, 3, false, true), Respond::Answer);
    EXPECT_EQ(on_accept(Action::Save, 0, false, false), Respond::SaveEntry);
    EXPECT_EQ(on_accept(Action::Save, 1, true, false), Respond::Enter);
    EXPECT_EQ(on_accept(Action::Save, 1, false, true), Respond::SaveEntry);
    EXPECT_EQ(on_accept(Action::SelectFolder, 0, false, false), Respond::Answer);
    EXPECT_EQ(on_accept(Action::SelectFolder, 1, true, false), Respond::Answer);
    EXPECT_EQ(on_accept(Action::SelectFolder, 2, false, false), Respond::Nothing);
}

TEST(ChooserCore, ActivatingAFolderGoesIn) {
    // The user's report: a double click on a folder while choosing a folder
    // chose it. GTK goes in, whatever is being chosen.
    for (Action a : {Action::Open, Action::Save, Action::SelectFolder})
        EXPECT_EQ(on_activate(a, true, true), Activate::Enter);
    EXPECT_EQ(on_activate(Action::Open, false, true), Activate::Answer);
    EXPECT_EQ(on_activate(Action::Save, false, true), Activate::Answer);
    EXPECT_FALSE(sensitive(Action::SelectFolder, false));
    EXPECT_EQ(on_activate(Action::SelectFolder, false, false), Activate::Nothing);
}

TEST(ChooserCore, DatesAsGtkWordsThem) {
    std::tm now{};
    now.tm_year = 126, now.tm_mon = 9, now.tm_mday = 16, now.tm_hour = 14, now.tm_min = 5, now.tm_isdst = -1;
    const std::int64_t t = std::mktime(&now);
    EXPECT_EQ(date_text(t, t, false, true), "14:05");
    EXPECT_EQ(date_text(t, t, false, false), "2:05 PM");
    EXPECT_EQ(date_text(t, t, true, true), "");  // the time column has it
    EXPECT_EQ(date_text(t - 86400, t, false, true), "Yesterday");
    EXPECT_EQ(date_text(t - 3 * 86400, t, false, true), "Tue");
    EXPECT_EQ(date_text(t - 30 * 86400, t, false, true), "16 Sep");
    EXPECT_EQ(date_text(t - 400 * 86400, t, false, true), "11 Sep 2025");
}

TEST(ChooserCore, Sizes) {
    EXPECT_EQ(size_text(1), "1 byte");
    EXPECT_EQ(size_text(999), "999 bytes");
    EXPECT_EQ(size_text(1500), "1.5 kB");
    EXPECT_EQ(size_text(3'400'000), "3.4 MB");
}

TEST(ChooserCore, NameChecks) {
    EXPECT_EQ(check_name("..", true).error, "A folder cannot be called “..”");
    EXPECT_EQ(check_name("a/b", false).error, "File names cannot contain “/”");
    EXPECT_EQ(check_name(" a", false).warning, "File names should not begin with a space");
    EXPECT_EQ(check_name("a ", true).warning, "Folder names should not end with a space");
    EXPECT_TRUE(check_name("fine", true).error.empty());
}

TEST(ChooserCore, TypedPaths) {
    EXPECT_EQ(directory_for_text("", "/home/me", "/home/me"), "/home/me");
    EXPECT_EQ(directory_for_text("Doc", "/home/me", "/home/me"), "/home/me");
    EXPECT_EQ(directory_for_text("Documents/", "/home/me", "/home/me"), "/home/me/Documents");
    EXPECT_EQ(directory_for_text("~/a/b", "/x", "/home/me"), "/home/me/a");
    EXPECT_EQ(directory_for_text("..", "/home/me", "/home/me"), "/home");
    EXPECT_EQ(directory_for_text("/usr/sh", "/x", "/home/me"), "/usr");
    EXPECT_EQ(directory_for_text("sftp://host/x", "/x", "/home/me"), "");
    EXPECT_EQ(file_for_text("file:///tmp/a%20b", "/x", "/h"), "/tmp/a b");
    EXPECT_EQ(split("a/b/c.txt").dir, "a/b/");
    EXPECT_EQ(split("a/b/c.txt").file, "c.txt");
    EXPECT_EQ(common_prefix({"Documents/", "Downloads/", "Desktop/"}, "Do"), "Do");
    EXPECT_EQ(common_prefix({"Documents/", "Downloads/", "Desktop/"}, "Doc"), "Documents/");
    EXPECT_EQ(common_prefix({"Music/"}, "x"), "x");
}

TEST(ChooserCore, Bookmarks) {
    const auto b = parse_bookmarks("file:///home/me/dev Dev stuff\nfile:///tmp\n\nsftp://host/x\n");
    ASSERT_EQ(b.size(), 3u);
    EXPECT_EQ(b[0].label, "Dev stuff");
    EXPECT_EQ(b[1].label, "");
    EXPECT_EQ(format_bookmarks(b), "file:///home/me/dev Dev stuff\nfile:///tmp\nsftp://host/x\n");
    EXPECT_EQ(uri_for_path("/home/me/a b#c"), "file:///home/me/a%20b%23c");
    EXPECT_EQ(path_for_uri("file:///home/me/a%20b%23c"), "/home/me/a b#c");
    EXPECT_FALSE(path_for_uri("sftp://host/x"));
}

TEST(ChooserCore, RecentlyUsed) {
    std::string x = add_recent("", "file:///tmp/a.txt", "text/plain", "app", "2026-10-16T00:00:00Z");
    EXPECT_NE(x.find("<bookmark href=\"file:///tmp/a.txt\""), std::string::npos);
    EXPECT_NE(x.find("count=\"1\""), std::string::npos);
    x = add_recent(x, "file:///tmp/a.txt", "text/plain", "app", "2026-10-17T00:00:00Z");
    EXPECT_NE(x.find("count=\"2\""), std::string::npos);
    EXPECT_NE(x.find("visited=\"2026-10-17T00:00:00Z\""), std::string::npos);
    EXPECT_EQ(x.find("<bookmark href", x.find("<bookmark href") + 1), std::string::npos);  // still one
    x = add_recent(x, "file:///tmp/b.txt", "text/plain", "other", "2026-10-17T00:00:00Z");
    EXPECT_NE(x.find("file:///tmp/b.txt"), std::string::npos);
    EXPECT_EQ(recent_folders({"/a/x", "/b/y", "/a/z"}), (std::vector<std::string>{"/a", "/b"}));
}

TEST(ChooserCore, Sorting) {
    SortKey dir{true, "b", 0, "", 5}, small{false, "a", 1, "Text", 9}, big{false, "c", 9, "Image", 1};
    EXPECT_TRUE(sorts_before(dir, small, SortColumn::Name, true, true));
    EXPECT_TRUE(sorts_before(dir, small, SortColumn::Name, false, true));  // folders stay first descending
    EXPECT_TRUE(sorts_before(small, dir, SortColumn::Name, true, false));
    EXPECT_TRUE(sorts_before(big, small, SortColumn::Size, false, true));
    EXPECT_TRUE(sorts_before(big, small, SortColumn::Type, true, true));
    EXPECT_TRUE(sorts_before(big, small, SortColumn::Modified, true, true));
}
