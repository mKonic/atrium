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

// As cliphist fed by `wl-paste --type text --watch` and `--type image --watch`.
TEST(Clipboard, ACopysTextAndItsPictureAreEachRecorded) {
    EXPECT_EQ(clipboard_history_mimes({"image/png", "text/plain", "text/plain;charset=utf-8"}),
              (std::vector<std::string>{"text/plain;charset=utf-8", "image/png"}));
    EXPECT_EQ(clipboard_history_mimes({"text/html", "text/plain"}), (std::vector<std::string>{"text/plain"}));
    EXPECT_EQ(clipboard_history_mimes({"image/jpeg", "image/png"}), (std::vector<std::string>{"image/jpeg"}));
}

TEST(Clipboard, TextIsAnyTextualTypeWhenThereIsNoPlainText) {
    // wl-paste: the first textual type (a file manager's file list, HTML).
    EXPECT_EQ(clipboard_history_mimes({"application/x-kde-cutselection", "text/uri-list"}),
              (std::vector<std::string>{"text/uri-list"}));
    EXPECT_EQ(clipboard_history_mimes({"UTF8_STRING"}), (std::vector<std::string>{"UTF8_STRING"}));
    EXPECT_EQ(clipboard_history_mimes({"application/x-foo"}), (std::vector<std::string>{}));
    EXPECT_EQ(clipboard_history_mimes({}), (std::vector<std::string>{}));
}

TEST(Clipboard, TextualTypesAsWlClipboardSeesThem) {
    for (const char* m : {"text/html", "TEXT", "STRING", "UTF8_STRING", "application/json", "application/xml",
                          "application/javascript", "text/csv"})
        EXPECT_TRUE(clipboard_is_text(m)) << m;
    for (const char* m : {"image/png", "application/octet-stream", "x-special/gnome-copied-files"})
        EXPECT_FALSE(clipboard_is_text(m)) << m;
}

TEST(Clipboard, APasswordManagersCopyIsntRecorded) {
    EXPECT_TRUE(clipboard_sensitive({"text/plain", "x-kde-passwordManagerHint"}));
    EXPECT_EQ(clipboard_history_mimes({"text/plain", "x-kde-passwordManagerHint"}), (std::vector<std::string>{}));
}

TEST(Clipboard, OnlyWhitespaceOrOverFiveMegabytesIsntRecorded) {
    EXPECT_FALSE(clipboard_worth_recording(" \n\t "));
    EXPECT_FALSE(clipboard_worth_recording(""));
    EXPECT_TRUE(clipboard_worth_recording(" x "));
    EXPECT_TRUE(clipboard_worth_recording(std::string(5'000'000, 'a')));
    EXPECT_FALSE(clipboard_worth_recording(std::string(5'000'001, 'a')));
}

TEST(Clipboard, PlainTextGoesBackUnderEveryNameForIt) {
    EXPECT_EQ(clipboard_offer_mimes("text/plain").size(), 5u);
    EXPECT_EQ(clipboard_offer_mimes("text/html"), (std::vector<std::string>{"text/html"}));
    EXPECT_EQ(clipboard_offer_mimes("image/png"), (std::vector<std::string>{"image/png"}));
}

// As wl-clip-persist: every type, once, but Firefox's SAVE_TARGETS.
TEST(Clipboard, EveryTypeIsKeptForWhenTheAppQuits) {
    EXPECT_EQ(clipboard_persist_mimes({"text/html", "SAVE_TARGETS", "text/plain", "text/html", "image/png"}),
              (std::vector<std::string>{"text/html", "text/plain", "image/png"}));
}

TEST(Clipboard, APreviewIsOneShortLine) {
    EXPECT_EQ(clipboard_preview("  hello\n\n\tworld  "), "hello world");
    EXPECT_EQ(clipboard_preview("abcdef", 3), "abc…");
    EXPECT_EQ(clipboard_preview("abc", 3), "abc");
    EXPECT_EQ(clipboard_preview("ab cd", 3), "ab…");
    EXPECT_EQ(clipboard_preview(" \n "), "");
}

TEST(Clipboard, APreviewCutsWholeCharacters) {
    // Two-, three- and four-byte sequences count once each.
    EXPECT_EQ(clipboard_preview("äöü", 2), "äö…");
    EXPECT_EQ(clipboard_preview("€€€", 1), "€…");
    EXPECT_EQ(clipboard_preview("😀x", 1), "😀…");
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

TEST(Clipboard, ARepeatOlderThanTheNewestHundredIsANewEntry) {
    ClipboardIndex index;
    bool fresh = false;
    ClipboardEntry first = entry("first", "same");
    index.add(first, &fresh);
    for (size_t i = 0; i < ClipboardIndex::kDedupe; ++i) {
        ClipboardEntry e = entry("e" + std::to_string(i), "data" + std::to_string(i));
        index.add(e, &fresh);
    }
    ClipboardEntry again = entry("again", "same");
    index.add(again, &fresh);
    EXPECT_TRUE(fresh);  // cliphist looks back only so far
    EXPECT_EQ(index.entries().front().id, "again");
}

TEST(Clipboard, SevenHundredFiftyAreKept) {
    EXPECT_EQ(ClipboardIndex::kLimit, 750u);
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

// Tinycast's pins: kept on top in the order they were pinned, past the
// limit and Clear All, where they are when copied again.
TEST(Clipboard, PinsLeadInTheOrderTheyWerePinned) {
    ClipboardIndex index;
    bool fresh = false;
    for (auto [id, data] : {std::pair{"1", "one"}, {"2", "two"}, {"3", "three"}, {"4", "four"}}) {
        ClipboardEntry e = entry(id, data);
        index.add(e, &fresh);
    }
    EXPECT_TRUE(index.set_pinned("2", true, 100));
    EXPECT_TRUE(index.set_pinned("4", true, 50));
    EXPECT_FALSE(index.set_pinned("nope", true, 1));
    std::vector<std::string> listed;
    for (const ClipboardEntry* e : index.listed())
        listed.push_back(e->id);
    EXPECT_EQ(listed, (std::vector<std::string>{"4", "2", "3", "1"}));
    // Pinning again keeps the first time: its place in the pins holds.
    index.set_pinned("4", true, 500);
    EXPECT_EQ(index.listed().front()->id, "4");
    EXPECT_TRUE(index.set_pinned("4", false, 0));
    EXPECT_EQ(index.listed().front()->id, "2");
}

TEST(Clipboard, APinStaysPutWhenCopiedAgain) {
    ClipboardIndex index;
    bool fresh = false;
    for (auto [id, data] : {std::pair{"1", "one"}, {"2", "two"}}) {
        ClipboardEntry e = entry(id, data);
        index.add(e, &fresh);
    }
    index.set_pinned("1", true, 10);
    index.to_front("1");
    EXPECT_EQ(ids(index), (std::vector<std::string>{"2", "1"}));
    ClipboardEntry again = entry("3", "one");
    index.add(again, &fresh);
    EXPECT_FALSE(fresh);
    EXPECT_EQ(again.id, "1");
    EXPECT_EQ(ids(index), (std::vector<std::string>{"2", "1"}));
}

TEST(Clipboard, APinNeverFallsOffTheEnd) {
    ClipboardIndex index;
    bool fresh = false;
    ClipboardEntry first = entry("0", "kept");
    index.add(first, &fresh);
    index.set_pinned("0", true, 1);
    std::vector<std::string> dropped;
    for (int i = 1; i <= 5; ++i) {
        ClipboardEntry e = entry(std::to_string(i), "data" + std::to_string(i));
        for (const std::string& d : index.add(e, &fresh, 3))
            dropped.push_back(d);
    }
    // Three unpinned are kept, the pin besides.
    EXPECT_EQ(dropped, (std::vector<std::string>{"1", "2"}));
    EXPECT_EQ(ids(index), (std::vector<std::string>{"5", "4", "3", "0"}));
}

TEST(Clipboard, ClearAllKeepsThePins) {
    ClipboardIndex index;
    bool fresh = false;
    for (auto [id, data] : {std::pair{"1", "one"}, {"2", "two"}, {"3", "three"}}) {
        ClipboardEntry e = entry(id, data);
        index.add(e, &fresh);
    }
    index.set_pinned("2", true, 5);
    EXPECT_EQ(index.clear_unpinned(), (std::vector<std::string>{"3", "1"}));
    EXPECT_EQ(ids(index), (std::vector<std::string>{"2"}));
    EXPECT_TRUE(index.remove("2"));
    EXPECT_TRUE(index.entries().empty());
}

TEST(Clipboard, APinSurvivesARoundTrip) {
    ClipboardIndex index;
    bool fresh = false;
    ClipboardEntry e = entry("a", "pinned");
    index.add(e, &fresh);
    index.set_pinned("a", true, 1700000000);
    const ClipboardIndex back = ClipboardIndex::from_json(index.to_json());
    ASSERT_EQ(back.entries().size(), 1u);
    EXPECT_EQ(back.entries()[0].pinned, 1700000000);
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

// --- what an entry is: Tinycast's clipboard-test cases ----------------------------------

namespace {

ClipboardKind kind(std::string_view text) {
    return clipboard_kind("text/plain;charset=utf-8", text);
}

bool near(const ClipboardColor& a, const ClipboardColor& b) {
    auto close = [](double x, double y) { return std::abs(x - y) < 0.005; };
    return close(a.red, b.red) && close(a.green, b.green) && close(a.blue, b.blue) && close(a.alpha, b.alpha);
}

} // namespace

TEST(Clipboard, LinksAddressesAndPlainText) {
    for (const char* t : {"https://apple.com", "http://apple.com/path?q=1", "apple.com", "apple.com/store",
                          "www.Apple.com", "vscode://file/tmp/x", "docs.google.com", "bit.ly/abc"})
        EXPECT_EQ(kind(t), ClipboardKind::Link) << t;
    for (const char* t : {"hi@apple.com", "mailto:hi@apple.com", "first.last@mail.example.co.uk"})
        EXPECT_EQ(kind(t), ClipboardKind::Email) << t;
    // Extensions that collide with a real top-level domain are why there's a list of them.
    for (const char* t : {"report.pdf", "index.html", "App.swift", "data.json", "Safari.app", "image.png",
                          "hello world", "visit apple.com today", "3.14", "", "   ", "no-dot-at-all",
                          "two@at@signs.com", "@apple.com", "hi@localhost", "line one\nline two"})
        EXPECT_EQ(kind(t), ClipboardKind::Text) << t;
    // A colour beats the prose test even written with spaces.
    for (const char* t : {"#FF5733", "#0f0", "rgb(255, 87, 51)", "hsl(11, 100%, 60%)"})
        EXPECT_EQ(kind(t), ClipboardKind::Color) << t;
    EXPECT_EQ(kind("https://apple.com/" + std::string(4096, 'a')), ClipboardKind::Text);
    EXPECT_EQ(clipboard_kind("image/png", "\x89PNG"), ClipboardKind::Image);
}

TEST(Clipboard, FilesAreAListOfFileUris) {
    EXPECT_EQ(kind("file:///home/u/a.pdf"), ClipboardKind::File);
    EXPECT_EQ(kind("file:///home/u/a.pdf\nfile:///home/u/My%20Notes.txt\n"), ClipboardKind::File);
    EXPECT_EQ(clipboard_kind("text/uri-list", "file:///x"), ClipboardKind::File);
    EXPECT_EQ(kind("file:///home/u/a.pdf\nand some text"), ClipboardKind::Text);
    EXPECT_EQ(kind("https://apple.com\nfile:///x"), ClipboardKind::Text);
}

TEST(Clipboard, ColoursInEveryNotation) {
    const std::pair<const char*, ClipboardColor> cases[] = {
        {"#FF5733", {1, 87 / 255.0, 51 / 255.0, 1}},
        // Shorthand doubles each digit rather than padding it with a zero.
        {"#0f0", {0, 1, 0, 1}},
        {"#0f08", {0, 1, 0, 136 / 255.0}},
        {"rgb(255, 87, 51)", {1, 87 / 255.0, 51 / 255.0, 1}},
        {"rgb(0 255 0 / 0.5)", {0, 1, 0, 0.5}},
        {"rgba(255,87,51,0.5)", {1, 87 / 255.0, 51 / 255.0, 0.5}},
        {"hsl(120, 100%, 50%)", {0, 1, 0, 1}},
        {"hsl(10.6deg 100% 60%)", {1, 87 / 255.0, 51 / 255.0, 1}},
        {"oklch(62.7955% 0.257683 29.2338)", {1, 0, 0, 1}},
        {"oklch(0.627955 64.42% 29.2338deg / 0.5)", {1, 0, 0, 0.5}},
    };
    for (const auto& [text, want] : cases) {
        const auto got = clipboard_color(text);
        ASSERT_TRUE(got) << text;
        EXPECT_TRUE(near(*got, want)) << text;
    }
    // rgba() is rgb() under CSS Color 4, so alpha is optional in both; a declaration spans lines.
    for (const char* t : {"rgba(255, 87, 51)", "rgb(255, 87, 51, 0.5)", "hsla(120, 100%, 50%)",
                          "hsl(120, 100%, 50%, 0.5)", "hsl(120, 100%, 50%, 50%)", "rgb(0 255 0)",
                          "rgb(255,\n87,\n51)", "rgb(0\n255\n0)"})
        EXPECT_TRUE(clipboard_color(t)) << t;
}

TEST(Clipboard, NearMissesArentColours) {
    for (const char* t :
         {"#FF5733 and more", "rgb(255, 87, 51) plus", "(255, 87, 51)", "#", "rgb", "#GGGGGG", "#12345",
          "report.pdf", "rgb(1, 2)", "rgb(1, 2, 3", "hsl(1, 2, 3, 4, 5)", "cmyk(0, 1, 1, 0)", "255, 87, 51",
          "rgb(nan, 0, 0)", "hsl(inf, 100%, 50%)", "rgb(1e400, 0, 0)", "rgba(0, 0, 0, nan)",
          // CSS writes HSL's channels as percentages: a bare 100 would clamp to white.
          "hsl(120, 100, 50)", "hsl(120 100 50)", "hsla(120, 100, 50, 1)",
          // A hole in the arguments is malformed, not dropped.
          "rgb(255,,87,51)", "rgb(255,87,51,)", "rgb(255, 87)", "rgb()", "rgb(1,2,3,4,5)",
          // The two spellings never mix.
          "rgb(1/2, 3, 4)", "rgb(255, 87, 51 / 0.5)", "rgb(0 255 0 / 0.5 / 1)", "rgb(/0.5)",
          // Each side of the slash is counted: flattened, an alpha reads as blue.
          "rgb(0 255 / 0.5)", "hsl(120 100% / 50%)", "rgb(0 / 255 0)", "rgb(0 255 0 0)",
          "rgb(0x10, 0, 0)", "rgb(1e2, 0, 0)", "rgb(+255, 0, 0)", "#\xef\xbd\x86\xef\xbd\x86\xef\xbd\x86"})
        EXPECT_FALSE(clipboard_color(t)) << t;
    EXPECT_FALSE(clipboard_color("#" + std::string(96, 'f')));
}

TEST(Clipboard, AColourAsTheSwatchDrawsIt) {
    EXPECT_EQ(clipboard_color_hex(*clipboard_color("#FF5733")), "#ff5733");
    EXPECT_EQ(clipboard_color_hex(*clipboard_color("#0f0f")), "#00ff00");  // opaque
    EXPECT_EQ(clipboard_color_hex(*clipboard_color("#000000FF")), "#000000");
    EXPECT_EQ(clipboard_color_hex(*clipboard_color("rgba(0, 255, 0, 0.5)")), "#00ff0080");
}
