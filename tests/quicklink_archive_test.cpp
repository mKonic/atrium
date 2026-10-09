#include "quicklink_archive_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::quicklinks;

namespace {

Quicklink ql(const std::string& name, const std::string& url) {
    return Quicklink{name, url, "", "", true};
}

bool same(const Quicklink& a, const Quicklink& b) {
    return a.name == b.name && a.link == b.link && a.app == b.app && a.icon == b.icon && a.root == b.root;
}

} // namespace

// Tinycast's quicklink-test, archiveRoundTrip, archiveMerge and archiveAcceptsAHandWrittenFile.
TEST(QuicklinkArchive, EveryFieldSurvivesARoundTrip) {
    const std::vector<Quicklink> source = {ql("GitHub", "https://github.com"),
                                           {"Downloads", "~/Downloads", "org.kde.dolphin.desktop", "folder", false}};
    const auto back = decode(encode(source));
    ASSERT_TRUE(std::holds_alternative<std::vector<Quicklink>>(back));
    const auto& list = std::get<std::vector<Quicklink>>(back);
    ASSERT_EQ(list.size(), 2u);
    EXPECT_TRUE(same(list[0], source[0]));
    EXPECT_TRUE(same(list[1], source[1]));
}

TEST(QuicklinkArchive, DuplicatesByNameOrLinkAreSkipped) {
    const std::vector<Quicklink> existing = {ql("GitHub", "https://github.com"), ql("Downloads", "~/Downloads")};
    const std::vector<Quicklink> incoming = {
        ql("github", "https://elsewhere.com"),         // the same name
        ql("Repos", "https://github.com"),             // the same link
        ql("Jira", " https://jira.example.com "),      // new
        ql("Jira Two", "https://jira.example.com"),    // the one above, in the same file
        ql("  ", "https://nameless.example.com"),      // no name
    };
    const Merge m = merge(incoming, existing);
    ASSERT_EQ(m.additions.size(), 1u);
    EXPECT_EQ(m.additions[0].name, "Jira");
    EXPECT_EQ(m.additions[0].link, "https://jira.example.com");
    EXPECT_EQ(m.skipped, 4);
    const Merge again = merge(existing, existing);
    EXPECT_TRUE(again.additions.empty());
    EXPECT_EQ(again.skipped, 2);
}

TEST(QuicklinkArchive, AHandWrittenFileReads) {
    const auto hand = decode(R"({ "version": 1, "quicklinks": [ { "name": "Staging", "link": "https://staging.example.com" } ] })");
    ASSERT_TRUE(std::holds_alternative<std::vector<Quicklink>>(hand));
    const Quicklink& q = std::get<std::vector<Quicklink>>(hand).front();
    EXPECT_EQ(q.name, "Staging");
    EXPECT_TRUE(q.root);  // left out: listed
    EXPECT_EQ(std::get<std::vector<Quicklink>>(decode(R"([{"name": "A", "link": "https://a.example.com"}])")).size(), 1u);
    // atrium's own records say url.
    EXPECT_EQ(std::get<std::vector<Quicklink>>(decode(R"([{"name": "A", "url": "https://a"}])")).front().link, "https://a");
    EXPECT_EQ(std::get<std::string>(decode("{}")), "This file isn't a quicklinks export.");
    EXPECT_EQ(std::get<std::string>(decode("not json")), "This file isn't a quicklinks export.");
    EXPECT_EQ(std::get<std::string>(decode(R"({"version":1,"quicklinks":[]})")), "This file has no quicklinks.");
}
