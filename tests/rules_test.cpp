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

// Hyprland's window rules: "workspace N" or "workspace N silent", "float",
// "pin", "no_initial_focus". The first rule to say each one wins.
TEST(Rules, HowAWindowOpens) {
    std::vector<std::string> errors;
    auto rules = parse_rules(json::array({
        {{"app_id", "^steam_app_"}, {"space", 5}, {"follow", true}, {"fullscreen", true}},
        {{"app_id", "steam"}, {"follow", false}, {"floating", true}, {"no_focus", true}, {"render_unfocused", true}},
        {{"title", "Picture-in-Picture"}, {"keep_above", true}, {"sticky", true}},
        {{"app_id", "x"}, {"sticky", "yes"}},
    }), &errors);
    ASSERT_EQ(rules.size(), 3u);
    EXPECT_EQ(errors.size(), 1u);
    RuleResult game = apply_rules(rules, "steam_app_570", "Dota 2");
    EXPECT_EQ(game.space, 5);
    EXPECT_EQ(game.follow, true);  // the first rule's, not the second's
    EXPECT_EQ(game.fullscreen, true);
    EXPECT_EQ(game.floating, true);
    EXPECT_EQ(game.no_focus, true);
    EXPECT_EQ(game.render_unfocused, true);
    EXPECT_FALSE(game.keep_above);
    RuleResult pip = apply_rules(rules, "firefox", "Picture-in-Picture");
    EXPECT_EQ(pip.keep_above, true);
    EXPECT_EQ(pip.sticky, true);
    EXPECT_FALSE(pip.follow);
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

TEST(Rules, LaunchGoesWithASecretSpace) {
    std::vector<std::string> errors;
    const auto rules = atrium::parse_rules(nlohmann::json::parse(R"([
        {"app_id": "^vesktop$", "secret": "communication", "launch": "vesktop"},
        {"app_id": "foot", "launch": "foot"}
    ])"), &errors);
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_EQ(rules[0].launch, "vesktop");
    ASSERT_EQ(errors.size(), 1u);  // launch without a secret space means nothing
}
