#include "file_search_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::file_search;

TEST(FileSearch, Admitted) {
    const IgnoreList ignore(IgnoreList::defaults());
    const std::vector<std::string> roots{"/home/u"};
    EXPECT_TRUE(admitted("/home/u/Documents/report.pdf", roots, ignore));
    EXPECT_FALSE(admitted("/home/u/.config/x.txt", roots, ignore));       // hidden on the way
    EXPECT_FALSE(admitted("/home/u/src/app/node_modules/a.js", roots, ignore));
    EXPECT_FALSE(admitted("/home/u/build/main.o", roots, ignore));
    EXPECT_FALSE(admitted("/home/uother/a.txt", roots, ignore));          // a sibling, not under it
    EXPECT_FALSE(admitted("/home/u", roots, ignore));                     // the root itself
    EXPECT_TRUE(admitted("/mnt/data/.x/../y", {"/"}, IgnoreList({})) == false);
    EXPECT_TRUE(admitted("/opt/tool/readme", {"/"}, IgnoreList({})));
}

TEST(FileSearch, IgnorePatterns) {
    const IgnoreList ignore({"Cache", "*.LOG", "**/target/**"});
    EXPECT_TRUE(ignore.ignores("a/cache/b"));         // names match any component, any case
    EXPECT_TRUE(ignore.ignores("x/debug.log"));
    EXPECT_TRUE(ignore.ignores("/p/rust/target/debug/app"));
    EXPECT_FALSE(ignore.ignores("x/catalog.txt"));
}

TEST(FileSearch, Terms) {
    EXPECT_EQ(terms("  Annual  Report "), (std::vector<std::string>{"annual", "report"}));
    EXPECT_TRUE(name_matches("Report-Annual-2026.pdf", terms("annual report")));
    EXPECT_FALSE(name_matches("Report.pdf", terms("annual report")));
    EXPECT_FALSE(name_matches("x", {}));
}

TEST(FileSearch, TypeFilter) {
    EXPECT_TRUE(filter_accepts("folders", "inode/directory"));
    EXPECT_FALSE(filter_accepts("documents", "inode/directory"));
    EXPECT_TRUE(filter_accepts("images", "image/png"));
    EXPECT_TRUE(filter_accepts("documents", "application/pdf"));
    EXPECT_TRUE(filter_accepts("archives", "application/zip"));
    EXPECT_FALSE(filter_accepts("videos", "audio/flac"));
    EXPECT_TRUE(filter_accepts("all", "inode/directory"));
}

TEST(FileSearch, RecentlyUsed) {
    const char* xbel = R"(<?xml version="1.0"?>
<xbel version="1.0">
  <bookmark href="file:///home/u/Notes%20on%20it.md" added="2026-09-20T10:00:00Z" modified="2026-09-21T10:00:00Z" visited="2026-09-25T09:00:00.123Z">
  </bookmark>
  <bookmark href="https://example.org" added="2026-09-26T10:00:00Z" modified="2026-09-26T10:00:00Z" visited="2026-09-26T10:00:00Z"/>
  <bookmark href="file:///home/u/a&amp;b.txt" added="2026-09-26T08:00:00Z" modified="2026-09-26T08:00:00Z" visited="2026-09-26T08:00:00Z"/>
</xbel>)";
    const auto r = parse_recent(xbel);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].path, "/home/u/a&b.txt");
    EXPECT_EQ(r[1].path, "/home/u/Notes on it.md");
    EXPECT_GT(r[0].when, r[1].when);
}
