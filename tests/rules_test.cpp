#include "rules.hpp"

#include <gtest/gtest.h>

using namespace atrium;
using json = nlohmann::json;

TEST(Rules, DefaultSendsChatAppsToCommunication) {
    auto rules = parse_rules(default_rules());
    for (const char* app : {"discord", "Discord", "equibop", "vesktop", "com.github.eneshecan.WhatsAppForLinux", "zapzap"})
        EXPECT_EQ(apply_rules(rules, app, "").secret, "communication") << app;
    EXPECT_TRUE(apply_rules(rules, "foot", "").secret.empty());
}

TEST(Rules, FirstPlacementWinsOtherFieldsAccumulate) {
    auto rules = parse_rules(json::array({
        {{"app_id", "firefox"}, {"space", 2}},
        {{"app_id", "fire"}, {"space", 5}, {"maximized", true}},
        {{"title", "Picture"}, {"fullscreen", true}},
    }));
    ASSERT_EQ(rules.size(), 3u);
    RuleResult r = apply_rules(rules, "firefox", "Picture-in-Picture");
    EXPECT_EQ(r.space, 2);
    EXPECT_EQ(r.maximized, true);
    EXPECT_EQ(r.fullscreen, true);
    r = apply_rules(rules, "firefox", "Mozilla");
    EXPECT_FALSE(r.fullscreen);
}

TEST(Rules, TitleAndAppIdBothMustMatch) {
    auto rules = parse_rules(json::array({{{"app_id", "mpv"}, {"title", "^movie"}, {"space", 3}}}));
    EXPECT_EQ(apply_rules(rules, "mpv", "movie.mkv").space, 3);
    EXPECT_EQ(apply_rules(rules, "mpv", "music.flac").space, 0);
}

TEST(Rules, RejectsBadRules) {
    std::vector<std::string> errors;
    auto rules = parse_rules(json::array({
        {{"space", 2}},                          // matches nothing
        {{"app_id", "("}, {"space", 2}},         // bad regex
        {{"app_id", "x"}, {"space", 0}},         // space out of range
        {{"app_id", "x"}, {"secret", ""}},       // empty secret name
        {{"app_id", "x"}, {"maximized", "yes"}}, // not a bool
        {{"app_id", "ok"}, {"space", 4}},
    }), &errors);
    EXPECT_EQ(errors.size(), 5u);
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_EQ(rules[0].space, 4);
    errors.clear();
    parse_rules("nope", &errors);
    EXPECT_EQ(errors.size(), 1u);
}
