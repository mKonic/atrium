#include "rank.hpp"

#include <gtest/gtest.h>

using namespace atrium::rank;

namespace {

std::u32string u(std::string_view s) {
    return std::u32string(s.begin(), s.end());
}

std::optional<int> sc(std::string_view q, std::string_view text) {
    return score(u(q), prepare(u(text)));
}

bool shows(std::string_view q, std::string_view text, Sensitivity s = Sensitivity::Medium) {
    auto v = sc(q, text);
    return v && passes(*v, query_length(u(q)), s);
}

Fields fields(std::string_view title, std::string_view subtitle = "", std::string_view alias = "") {
    Fields f;
    f.title = prepare(u(title));
    f.subtitle = prepare(u(subtitle));
    f.alias = fold_all(u(alias));
    return f;
}

Facts facts(std::string_view q, const Fields& f, int kind = 4, const Visit* v = nullptr, double now = 0) {
    auto m = match(u(q), f, v, now, Sensitivity::Medium, kind);
    EXPECT_TRUE(m) << q;
    return m.value_or(Facts{});
}

} // namespace

TEST(Rank, WordStarts) {
    const Text t = prepare(u("OrbStack 1Password BBEdit"));
    auto at = [&](size_t i) { return bool(t.starts[i]); };
    EXPECT_TRUE(at(0));   // O
    EXPECT_TRUE(at(3));   // S of Stack
    EXPECT_TRUE(at(9));   // 1
    EXPECT_TRUE(at(10));  // P after a digit
    EXPECT_TRUE(at(19));  // B
    EXPECT_FALSE(at(20)); // second B
    EXPECT_TRUE(at(21));  // E, last capital before lowercase
    EXPECT_EQ(t.folded, u("orbstack 1password bbedit"));
}

TEST(Rank, ScorerPoints) {
    EXPECT_EQ(sc("code", "Xcode"), 8);       // mid-word run
    EXPECT_EQ(sc("vsc", "Visual Studio Code"), 8);  // 4 + (3-1) + (3-1)
    EXPECT_EQ(sc("pec", "Previous Track"), 6);
    EXPECT_EQ(sc("stack", "OrbStack"), 3 + 2 * 4);
    EXPECT_EQ(sc("fire", "Firefox"), 4 + 2 * 3);
    EXPECT_FALSE(sc("xyz", "Firefox"));
    EXPECT_FALSE(sc("xf", "Firefox"));  // letters must come in order
}

TEST(Rank, SeparatorsInTheQuery) {
    // A space on a dash scores 1, on a space 2; with nothing to land on it's skipped.
    EXPECT_EQ(sc("a b", "a-b"), 4 + 1 + 3);
    EXPECT_EQ(sc("a b", "a b"), 4 + 2 + 3);
    EXPECT_EQ(sc("visual studio", "VisualStudio"), sc("visualstudio", "VisualStudio"));
}

TEST(Rank, Sensitivity) {
    EXPECT_TRUE(shows("code", "Xcode"));
    EXPECT_FALSE(shows("code", "Xcode", Sensitivity::High));
    EXPECT_TRUE(shows("pec", "Accessibility Inspector"));
    EXPECT_FALSE(shows("pec", "Accessibility Inspector", Sensitivity::High));
    EXPECT_TRUE(shows("pec", "Previous Track"));
    EXPECT_TRUE(shows("vsc", "Visual Studio Code", Sensitivity::High));
    EXPECT_TRUE(shows("chrome", "Google Chrome", Sensitivity::High));
    EXPECT_FALSE(shows("oeo", "Google Chrome"));
    EXPECT_TRUE(shows("oeo", "Google Chrome", Sensitivity::Low));
}

TEST(Rank, Frecency) {
    Visit v;
    EXPECT_DOUBLE_EQ(frecency(v, 1000), 1);
    record_visit(v, 1000, u("fi"));
    EXPECT_NEAR(frecency(v, 1000), 101, 1e-6);
    // Ten days halve it.
    EXPECT_NEAR(frecency(v, 1000 + 10 * 86400), 50.5, 1e-6);
    record_visit(v, 1000, u("fire"));
    record_visit(v, 1000, u("fi"));
    record_visit(v, 1000, u("ff"));
    record_visit(v, 1000, u("fox"));
    EXPECT_EQ(v.terms, (std::vector<std::u32string>{u("fox"), u("ff"), u("fi")}));
    EXPECT_TRUE(terms_active(v, 1000 + 16 * 86400));
    EXPECT_FALSE(terms_active(v, 1000 + 18 * 86400));
    EXPECT_FALSE(expired(v, 1000 + 30 * 86400));
    EXPECT_TRUE(expired(v, 1000 + 200 * 86400));
}

TEST(Rank, NamesCompareNumerically) {
    EXPECT_LT(compare_names(u("space 2"), u("space 10")), 0);
    EXPECT_GT(compare_names(u("b"), u("a")), 0);
    EXPECT_EQ(compare_names(u("x07"), u("x7")), 0);
}

TEST(Rank, ComparatorRules) {
    // An exact alias beats everything.
    EXPECT_TRUE(before(facts("ff", fields("Firefox", "", "ff")), facts("ff", fields("ff"))));
    // Past three characters an exact title wins over a better-used prefix hit.
    Visit used;
    for (int i = 0; i < 5; ++i)
        record_visit(used, 0, u(""));
    EXPECT_TRUE(before(facts("zoom", fields("Zoom")), facts("zoom", fields("Zoom Client"), 4, &used)));
    // A learned term: typed "sp" and opened Spotify; it now beats Space Settings.
    Visit spot;
    record_visit(spot, 0, u("sp"));
    EXPECT_TRUE(before(facts("sp", fields("Spotify"), 4, &spot), facts("sp", fields("Spectacle"))));
    // An exact subtitle lists an extension's commands above a title that starts with it.
    EXPECT_TRUE(before(facts("zed", fields("Open Project", "zed"), 3), facts("zed", fields("Zed Editor"))));
    // Title prefix: App Store above AirPort Utility at the same score.
    const Facts store = facts("ap", fields("App Store")), airport = facts("ap", fields("AirPort Utility"));
    EXPECT_EQ(store.best, airport.best);
    EXPECT_TRUE(before(store, airport));
    // Apps win ties over commands of the same name.
    EXPECT_TRUE(before(facts("calc", fields("Calculator"), 4), facts("calc", fields("Calculator"), 3)));
}

TEST(Rank, KeywordsOnlyShow) {
    Fields f = fields("Dolphin");
    f.keywords.push_back(prepare(u("file manager")));
    const auto m = match(u("file"), f, nullptr, 0, Sensitivity::Medium, 4);
    ASSERT_TRUE(m);
    EXPECT_EQ(m->best, 0);
    EXPECT_FALSE(match(u("zzz"), f, nullptr, 0, Sensitivity::Medium, 4));
}

TEST(Rank, IdleOrder) {
    Visit v;
    record_visit(v, 0, u(""));
    EXPECT_TRUE(before_idle(idle(fields("Zed"), &v, 0, 4), idle(fields("Alacritty"), nullptr, 0, 4)));
    EXPECT_TRUE(before_idle(idle(fields("Alacritty"), nullptr, 0, 4), idle(fields("Btop"), nullptr, 0, 4)));
    EXPECT_TRUE(before_idle(idle(fields("Zed", "", "z"), nullptr, 0, 1), idle(fields("Alacritty"), nullptr, 0, 4)));
}
