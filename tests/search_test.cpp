#include "search.hpp"

#include <gtest/gtest.h>

using namespace atrium::search;

TEST(Search, RanksHowNamesMatch) {
    // Whole > start > word start > initials > substring > letters in order > nothing.
    const int whole = score("firefox", "firefox");
    const int start = score("fire", "firefox");
    const int word = score("code", "visual studio code");
    const int initials = score("vsc", "visual studio code");
    const int inside = score("udio", "visual studio code");
    const int letters = score("vsd", "visual studio code");
    EXPECT_GT(whole, start);
    EXPECT_GT(start, word);
    EXPECT_GT(word, initials);
    EXPECT_GT(initials, inside);
    EXPECT_GT(inside, letters);
    EXPECT_GT(letters, 0);
    EXPECT_EQ(score("xyz", "firefox"), 0);
    EXPECT_EQ(score("fire", "file manager"), 0);  // scattered letters are chance
    EXPECT_EQ(score("vsc", "browse for zeroconf services available"), 0);
    EXPECT_EQ(score("", "firefox"), 0);
    EXPECT_GT(score("dol", "dolphin"), score("dol", "kdenlive tools dolphin helper"));
}

TEST(Search, Calculates) {
    EXPECT_EQ(calculate("2+2"), 4);
    EXPECT_EQ(calculate("2 * (3 + 4)"), 14);
    EXPECT_EQ(calculate("2^3^2"), 512);  // right-associative
    EXPECT_EQ(calculate("-3 + 1"), -2);
    EXPECT_EQ(calculate("10 % 4"), 2);
    EXPECT_EQ(calculate("6 \xc3\x97 7"), 42);  // ×
    EXPECT_EQ(calculate("1,5 + 1"), 2.5);
    EXPECT_NEAR(*calculate("sqrt(16) + pi - pi"), 4, 1e-12);
    EXPECT_EQ(format_number(*calculate("0.1 + 0.2")), "0.3");
    EXPECT_EQ(format_number(*calculate("1/3")), "0.333333333333");
    EXPECT_EQ(format_number(*calculate("2^40")), "1099511627776");
}

TEST(Search, KnowsWhatIsNotACalculation) {
    for (const char* t : {"", "42", "-7", "firefox", "2+", "(1", "foo(2)", "1/0", "inf+1", "0x10+1"})
        EXPECT_EQ(calculate(t), std::nullopt) << t;
}
