#include "placeholders.hpp"

#include <gtest/gtest.h>

using namespace atrium::placeholders;

TEST(Placeholders, Arguments) {
    const auto a = arguments("https://x/?q={argument}&lang={argument name=\"Lang\" default=\"en\"}&again={argument}");
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[0].name, "Query");
    EXPECT_FALSE(a[0].fallback);
    EXPECT_EQ(a[1].name, "Lang");
    EXPECT_EQ(a[1].fallback, "en");
    EXPECT_TRUE(arguments("plain {text} and {clipboard}").empty());
}

TEST(Placeholders, ExpandsWithEncoding) {
    Values v;
    v.arguments = {"a b&c", ""};
    v.clipboard = "clip";
    v.date = "27/09/2026";
    const std::string link = "https://x/?q={argument}&l={argument name=\"L\" default=\"en\"}&c={clipboard}";
    EXPECT_EQ(expand(link, v, Encoding::Url), "https://x/?q=a%20b%26c&l=en&c=clip");
    EXPECT_EQ(expand("echo {argument}", v, Encoding::Shell), "echo 'a b&c'");
    EXPECT_EQ(shell_quote("it's"), "'it'\\''s'");
    EXPECT_EQ(expand("Hi {name}, {date}", v, Encoding::Plain), "Hi {name}, 27/09/2026");
    v.uuid = [] { return std::string("u-1"); };
    EXPECT_EQ(expand("{uuid}", v, Encoding::Plain), "u-1");
}

TEST(Placeholders, Cursor) {
    Values v;
    EXPECT_EQ(expand("<b>{cursor}</b>", v, Encoding::Plain), "<b></b>");
    EXPECT_EQ(cursor_from_end("<b>{cursor}</b>", v), 4u);
    EXPECT_EQ(cursor_from_end("é{cursor}é", v), 1u);
    EXPECT_FALSE(cursor_from_end("none", v));
}
