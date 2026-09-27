#include "notes_core.hpp"

#include <gtest/gtest.h>

#include <cctype>

using namespace atrium::notes;

namespace {

std::string lower(const std::string& s) {
    std::string o = s;
    for (char& c : o)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

} // namespace

TEST(Notes, Unnamed) {
    EXPECT_TRUE(unnamed("Untitled"));
    EXPECT_TRUE(unnamed("Untitled 12"));
    EXPECT_FALSE(unnamed("Untitled plan"));
    EXPECT_FALSE(unnamed("Groceries"));
}

TEST(Notes, FirstLine) {
    EXPECT_EQ(first_line("\n\n# **Trip** to [Kyoto](https://x)\nmore"), "Trip to Kyoto");
    EXPECT_EQ(first_line("---\n```\n- [ ] buy `milk`"), "buy milk");
    EXPECT_EQ(first_line("   \n"), "");
    EXPECT_EQ(first_line(std::string(200, 'a')).size(), 120u + std::string("…").size() - 1);
}

TEST(Notes, ListsCarryOn) {
    auto n = newline("- milk", 6);
    EXPECT_EQ(n.kind, Newline::Continue);
    EXPECT_EQ(n.insert, "\n- ");
    n = newline("intro\n  9. nine", 15);
    EXPECT_EQ(n.insert, "\n  10. ");
    n = newline("- [x] done", 10);
    EXPECT_EQ(n.insert, "\n- [ ] ");
    n = newline("a\n- ", 4);
    EXPECT_EQ(n.kind, Newline::EndList);
    EXPECT_EQ(n.line_start, 2u);
    EXPECT_EQ(newline("plain text", 10).kind, Newline::Plain);
}

TEST(Notes, FreeName) {
    const std::vector<std::string> taken{"Untitled.md", "Untitled 2.md", "Plan.md"};
    EXPECT_EQ(free_name("Untitled", taken, "", lower), "Untitled 3");
    EXPECT_EQ(free_name("plan", taken, "", lower), "plan 2");  // case doesn't make it another name
    EXPECT_EQ(free_name("PLAN", taken, "Plan.md", lower), "PLAN");  // renaming itself
    EXPECT_EQ(free_name("Ideas", taken, "", lower), "Ideas");
}
