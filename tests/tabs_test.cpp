#include "tabs_core.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace atrium::tabs;

namespace {

List<std::string> list(std::initializer_list<const char*> names, size_t current) {
    List<std::string> l;
    for (const char* n : names)
        l.items.push_back(n);
    l.current = current;
    return l;
}

} // namespace

TEST(Tabs, NewOnesGoAfterTheCurrent) {
    List<std::string> l;
    l.add("a");
    EXPECT_EQ(l.now(), "a");
    l.add("b");
    l.add("c");
    EXPECT_EQ(l.items, (std::vector<std::string>{"a", "b", "c"}));
    l.current = 0;
    l.add("d");  // next to the one in front, and in front itself
    EXPECT_EQ(l.items, (std::vector<std::string>{"a", "d", "b", "c"}));
    EXPECT_EQ(l.now(), "d");
    l.add("e", 0);  // dropped at the bar's start
    EXPECT_EQ(l.items.front(), "e");
    EXPECT_EQ(l.now(), "e");
    l.add("f", 99);  // past the end: last
    EXPECT_EQ(l.items.back(), "f");
    EXPECT_EQ(l.now(), "f");
}

TEST(Tabs, LeavingHandsToTheOneBefore) {
    auto l = list({"a", "b", "c"}, 1);
    ASSERT_TRUE(l.remove("b"));
    EXPECT_EQ(l.now(), "a");
    l = list({"a", "b", "c"}, 0);
    l.remove("a");  // the first: the next one then
    EXPECT_EQ(l.now(), "b");
    l = list({"a", "b", "c"}, 2);
    l.remove("a");  // one before the current: it stays in front
    EXPECT_EQ(l.now(), "c");
    l = list({"a", "b", "c"}, 0);
    l.remove("c");  // one after: nothing changes
    EXPECT_EQ(l.now(), "a");
    l = list({"a", "b", "c"}, 2);
    l.remove("c");
    EXPECT_EQ(l.now(), "b");
    EXPECT_FALSE(l.remove("zz"));
    l = list({"a"}, 0);
    l.remove("a");
    EXPECT_EQ(l.size(), 0u);
    EXPECT_EQ(l.current, 0u);
}

TEST(Tabs, DraggingKeepsTheCurrent) {
    auto l = list({"a", "b", "c", "d"}, 1);
    l.move(0, 3);
    EXPECT_EQ(l.items, (std::vector<std::string>{"b", "c", "d", "a"}));
    EXPECT_EQ(l.now(), "b");
    l.move(0, 2);
    EXPECT_EQ(l.items, (std::vector<std::string>{"c", "d", "b", "a"}));
    EXPECT_EQ(l.now(), "b");
    l.move(9, 0);  // nothing there
    EXPECT_EQ(l.now(), "b");
}

TEST(Tabs, StepsGoRound) {
    auto l = list({"a", "b", "c"}, 2);
    EXPECT_EQ(l.step(true), 0u);
    EXPECT_EQ(l.step(false), 1u);
    l.current = 0;
    EXPECT_EQ(l.step(false), 2u);
}

TEST(Tabs, Bar) {
    EXPECT_EQ(tab_at(0, 300, 3), 0u);
    EXPECT_EQ(tab_at(150, 300, 3), 1u);
    EXPECT_EQ(tab_at(299.9, 300, 3), 2u);
    EXPECT_FALSE(tab_at(300, 300, 3));
    EXPECT_FALSE(tab_at(-1, 300, 3));
    EXPECT_FALSE(tab_at(10, 300, 0));
    EXPECT_DOUBLE_EQ(tab_left(2, 300, 3), 200);
    // The close button at the second tab's left.
    EXPECT_TRUE(on_close(100 + kCloseInset + kCloseSize / 2, 14, 1, 300, 3, 28));
    EXPECT_FALSE(on_close(150, 14, 1, 300, 3, 28));
    // Drops go in before the tab whose middle is past.
    EXPECT_EQ(drop_slot(10, 300, 3), 0u);
    EXPECT_EQ(drop_slot(60, 300, 3), 1u);
    EXPECT_EQ(drop_slot(290, 300, 3), 3u);
}

TEST(Tabs, NewWindowsAsTabs) {
    EXPECT_TRUE(opens_as_tab(Prefer::Always, true, false));
    EXPECT_FALSE(opens_as_tab(Prefer::Always, false, true));
    EXPECT_TRUE(opens_as_tab(Prefer::Fullscreen, true, true));
    EXPECT_FALSE(opens_as_tab(Prefer::Fullscreen, true, false));
    EXPECT_FALSE(opens_as_tab(Prefer::Never, true, true));
}
