#include "../src/clipboard_core.hpp"

#include <gtest/gtest.h>

using namespace atrium;

namespace {

ClipboardEntry entry(const std::string& id, const std::string& data, const std::string& mime = "text/plain") {
    ClipboardEntry e;
    e.id = id;
    e.mime = mime;
    e.size = data.size();
    e.hash = clipboard_hash(data);
    return e;
}

std::vector<std::string> ids(const ClipboardIndex& index) {
    std::vector<std::string> out;
    for (const ClipboardEntry& e : index.entries())
        out.push_back(e.id);
    return out;
}

TEST(Clipboard, TextIsKeptBeforeAPicture) {
    EXPECT_EQ(clipboard_mime({"image/png", "text/plain", "text/plain;charset=utf-8"}), "text/plain;charset=utf-8");
    EXPECT_EQ(clipboard_mime({"UTF8_STRING"}), "UTF8_STRING");
    EXPECT_EQ(clipboard_mime({"image/jpeg", "image/png"}), "image/png");
    EXPECT_EQ(clipboard_mime({"image/x-portable-anymap"}), "image/x-portable-anymap");
}

TEST(Clipboard, WhatIsNeitherTextNorAPictureIsntKept) {
    EXPECT_EQ(clipboard_mime({"text/uri-list", "application/x-kde-cutselection"}), std::nullopt);
    EXPECT_EQ(clipboard_mime({}), std::nullopt);
}

TEST(Clipboard, APasswordManagersCopyIsntKept) {
    EXPECT_EQ(clipboard_mime({"text/plain", "x-kde-passwordManagerHint"}), std::nullopt);
}

TEST(Clipboard, TextGoesBackUnderEveryNameForText) {
    for (const std::string& m : clipboard_text_mimes())
        EXPECT_TRUE(clipboard_is_text(m)) << m;
    EXPECT_FALSE(clipboard_is_text("image/png"));
    EXPECT_FALSE(clipboard_is_text("text/html"));
}

TEST(Clipboard, APreviewIsOneShortLine) {
    EXPECT_EQ(clipboard_preview("  hello\n\n\tworld  "), "hello world");
    EXPECT_EQ(clipboard_preview("abcdef", 3), "abc");
    EXPECT_EQ(clipboard_preview("ab cd", 3), "ab ");
    EXPECT_EQ(clipboard_preview(" \n "), "");
}

TEST(Clipboard, APreviewCutsWholeCharacters) {
    // Two-, three- and four-byte sequences count once each.
    EXPECT_EQ(clipboard_preview("äöü", 2), "äö");
    EXPECT_EQ(clipboard_preview("€€€", 1), "€");
    EXPECT_EQ(clipboard_preview("😀x", 1), "😀");
    // A sequence cut off at the end is left out.
    EXPECT_EQ(clipboard_preview(std::string_view("a\xe2\x82", 3)), "a");
}

TEST(Clipboard, TheHashTellsCopiesApart) {
    EXPECT_EQ(clipboard_hash("abc"), clipboard_hash("abc"));
    EXPECT_NE(clipboard_hash("abc"), clipboard_hash("abd"));
    EXPECT_NE(clipboard_hash(""), clipboard_hash(std::string_view("\0", 1)));
}

TEST(Clipboard, NewestComesFirst) {
    ClipboardIndex index;
    bool fresh = false;
    for (auto [id, data] : {std::pair{"1", "one"}, {"2", "two"}, {"3", "three"}}) {
        ClipboardEntry e = entry(id, data);
        index.add(e, &fresh);
        EXPECT_TRUE(fresh);
    }
    EXPECT_EQ(ids(index), (std::vector<std::string>{"3", "2", "1"}));
}

TEST(Clipboard, CopyingAgainMovesItToTheTop) {
    ClipboardIndex index;
    bool fresh = false;
    for (auto [id, data] : {std::pair{"1", "one"}, {"2", "two"}}) {
        ClipboardEntry e = entry(id, data);
        index.add(e, &fresh);
    }
    ClipboardEntry again = entry("3", "one");
    again.time = 42;
    EXPECT_TRUE(index.add(again, &fresh).empty());
    EXPECT_FALSE(fresh);
    EXPECT_EQ(again.id, "1");  // the old entry's file keeps serving
    EXPECT_EQ(ids(index), (std::vector<std::string>{"1", "2"}));
    EXPECT_EQ(index.entries().front().time, 42);
}

TEST(Clipboard, TheSameBytesAsAnotherTypeAreAnotherEntry) {
    ClipboardIndex index;
    bool fresh = false;
    ClipboardEntry a = entry("1", "x", "image/png"), b = entry("2", "x", "image/jpeg");
    index.add(a, &fresh);
    index.add(b, &fresh);
    EXPECT_TRUE(fresh);
    EXPECT_EQ(index.entries().size(), 2u);
}

TEST(Clipboard, TheOldestFallOffPastTheLimit) {
    ClipboardIndex index;
    bool fresh = false;
    std::vector<std::string> dropped;
    for (int i = 0; i < 5; ++i) {
        ClipboardEntry e = entry(std::to_string(i), "data" + std::to_string(i));
        for (std::string& d : index.add(e, &fresh, 3))
            dropped.push_back(d);
    }
    EXPECT_EQ(ids(index), (std::vector<std::string>{"4", "3", "2"}));
    EXPECT_EQ(dropped, (std::vector<std::string>{"0", "1"}));
}

TEST(Clipboard, RemovingAndBringingToTheFront) {
    ClipboardIndex index;
    bool fresh = false;
    for (auto [id, data] : {std::pair{"1", "one"}, {"2", "two"}, {"3", "three"}}) {
        ClipboardEntry e = entry(id, data);
        index.add(e, &fresh);
    }
    index.to_front("1");
    EXPECT_EQ(ids(index), (std::vector<std::string>{"1", "3", "2"}));
    index.to_front("nope");
    EXPECT_EQ(ids(index), (std::vector<std::string>{"1", "3", "2"}));
    EXPECT_TRUE(index.remove("3"));
    EXPECT_FALSE(index.remove("3"));
    EXPECT_EQ(ids(index), (std::vector<std::string>{"1", "2"}));
    ASSERT_NE(index.find("2"), nullptr);
    EXPECT_EQ(index.find("3"), nullptr);
}

TEST(Clipboard, TheIndexSurvivesARoundTrip) {
    ClipboardIndex index;
    bool fresh = false;
    ClipboardEntry e = entry("17-1", "hello");
    e.time = 1700000000;
    e.preview = "hello";
    index.add(e, &fresh);
    const ClipboardIndex back = ClipboardIndex::from_json(index.to_json());
    ASSERT_EQ(back.entries().size(), 1u);
    const ClipboardEntry& b = back.entries()[0];
    EXPECT_EQ(b.id, "17-1");
    EXPECT_EQ(b.mime, "text/plain");
    EXPECT_EQ(b.time, 1700000000);
    EXPECT_EQ(b.size, 5u);
    EXPECT_EQ(b.hash, clipboard_hash("hello"));
    EXPECT_EQ(b.preview, "hello");
}

TEST(Clipboard, AnIdCantReachOutsideTheStore) {
    const ClipboardIndex index = ClipboardIndex::from_json(R"([
        {"id": "../../.bashrc", "mime": "text/plain"},
        {"id": ".hidden", "mime": "text/plain"},
        {"id": "x/../../.bashrc", "mime": "text/plain"},
        {"id": "", "mime": "text/plain"},
        {"id": "ok", "mime": "text/plain"}])");
    EXPECT_EQ(ids(index), (std::vector<std::string>{"ok"}));
}

TEST(Clipboard, ADamagedIndexReadsAsWhatIsUsable) {
    EXPECT_TRUE(ClipboardIndex::from_json("not json").entries().empty());
    EXPECT_TRUE(ClipboardIndex::from_json(R"({"id": "1"})").entries().empty());
    const ClipboardIndex index = ClipboardIndex::from_json(R"([
        {"id": 5, "mime": "text/plain"},
        {"id": "nomime"},
        {"id": "a", "mime": "text/plain", "time": "yesterday", "size": -1, "preview": 3}])");
    ASSERT_EQ(ids(index), (std::vector<std::string>{"a"}));
    EXPECT_EQ(index.entries()[0].time, 0);
    EXPECT_EQ(index.entries()[0].preview, "");
}

} // namespace
