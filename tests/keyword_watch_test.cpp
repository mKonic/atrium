#include "keyword_watch.hpp"

#include <gtest/gtest.h>

using namespace atrium;

namespace {

std::optional<int64_t> type(KeywordWatch& w, std::u32string_view text) {
    std::optional<int64_t> hit;
    for (char32_t c : text)
        if (auto m = w.typed(c))
            hit = m->first;
    return hit;
}

} // namespace

TEST(KeywordWatch, FiresWhenAKeywordIsTyped) {
    KeywordWatch w;
    w.set_keywords({{1, ";sig"}, {2, ";addr"}, {3, ""}});
    EXPECT_FALSE(type(w, U"hello ;si"));
    EXPECT_EQ(type(w, U"g"), 1);
    EXPECT_EQ(type(w, U"x;addr"), 2);
}

TEST(KeywordWatch, BackspaceAndResetsCount) {
    KeywordWatch w;
    w.set_keywords({{1, ";sig"}});
    type(w, U";six");
    w.backspace();
    EXPECT_EQ(type(w, U"g"), 1);  // ;si + g after the x was taken back
    type(w, U";si");
    w.reset();                    // the caret moved: not one go
    EXPECT_FALSE(type(w, U"g"));
}

TEST(KeywordWatch, LongestWinsAndUnicode) {
    KeywordWatch w;
    w.set_keywords({{1, "sig"}, {2, ";sig"}, {3, "→→"}});
    EXPECT_EQ(type(w, U";sig"), 2);
    EXPECT_EQ(type(w, U"a→→"), 3);
    KeywordWatch none;
    EXPECT_FALSE(type(none, U"anything"));
}
