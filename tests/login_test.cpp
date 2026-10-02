#include "../login/core.hpp"

#include <gtest/gtest.h>

using namespace atrium::login;

namespace {

TEST(LoginIpc, ParsesGreetdRequests) {
    auto c = parse_request(R"({"type":"create_session","username":"ann"})");
    ASSERT_TRUE(c);
    EXPECT_EQ(c->type, Request::Type::CreateSession);
    EXPECT_EQ(c->username, "ann");
    auto a = parse_request(R"({"type":"post_auth_message_response","response":"pw"})");
    ASSERT_TRUE(a);
    EXPECT_EQ(a->response, "pw");
    auto none = parse_request(R"({"type":"post_auth_message_response"})");
    ASSERT_TRUE(none);
    EXPECT_FALSE(none->response);
    auto s = parse_request(R"({"type":"start_session","cmd":["atrium","-d"],"env":["A=1"]})");
    ASSERT_TRUE(s);
    EXPECT_EQ(s->cmd, (std::vector<std::string>{"atrium", "-d"}));
    EXPECT_EQ(s->env, (std::vector<std::string>{"A=1"}));
    EXPECT_FALSE(parse_request(R"({"type":"start_session","cmd":[]})"));
    EXPECT_FALSE(parse_request(R"({"type":"start_session","cmd":[1]})"));
    EXPECT_FALSE(parse_request(R"({"type":"create_session"})"));
    EXPECT_FALSE(parse_request(R"({"type":"reboot"})"));
    EXPECT_FALSE(parse_request("not json"));
}

TEST(LoginIpc, RepliesAsGreetdDoes) {
    EXPECT_EQ(success(), R"({"type":"success"})");
    EXPECT_EQ(error(true, "x"), R"({"description":"x","error_type":"auth_error","type":"error"})");
    EXPECT_EQ(auth_message(AuthKind::Secret, "Password: "),
              R"({"auth_message":"Password: ","auth_message_type":"secret","type":"auth_message"})");
}

TEST(LoginIpc, FramesSplitAndJoin) {
    std::string stream = frame("one") + frame("two");
    std::string partial = stream.substr(0, stream.size() - 1);
    bool bad = false;
    auto first = unframe(partial, &bad);
    EXPECT_EQ(first, std::vector<std::string>{"one"});
    EXPECT_EQ(partial, frame("two").substr(0, 6));  // the rest waits
    partial += stream.back();
    EXPECT_EQ(unframe(partial, &bad), std::vector<std::string>{"two"});
    EXPECT_TRUE(partial.empty());
    std::string huge = frame(std::string(100, 'x'));
    unframe(huge, &bad, 10);
    EXPECT_TRUE(bad);
}

TEST(LoginConfig, ReadsAndKeepsDefaults) {
    Config c = parse_config("[login]\nvt = 7\ngreeter = atrium --greeter -d\nsource_profile = false\n"
                            "[autologin]\nuser = ann\n");
    EXPECT_EQ(c.vt, 7);
    EXPECT_EQ(c.greeter_command, "atrium --greeter -d");
    EXPECT_EQ(c.greeter_user, "atrium-greeter");
    EXPECT_FALSE(c.source_profile);
    EXPECT_EQ(c.autologin_user, "ann");
    EXPECT_EQ(c.autologin_command, "atrium");
    EXPECT_EQ(parse_config("[login]\nvt = 99\n").vt, 1);
    EXPECT_EQ(parse_config("[login]\nvt = 2x\n").vt, 1);
}

TEST(LoginSession, CommandAndProfile) {
    EXPECT_EQ(split_command(R"(atrium --greeter "a b" 'c d')"),
              (std::vector<std::string>{"atrium", "--greeter", "a b", "c d"}));
    auto argv = session_argv({"atrium"}, true);
    ASSERT_EQ(argv.size(), 5u);
    EXPECT_EQ(argv[0], "/bin/sh");
    EXPECT_NE(argv[2].find(". /etc/profile"), std::string::npos);
    EXPECT_EQ(argv[4], "atrium");
    EXPECT_EQ(session_argv({"atrium"}, false)[2], "exec \"$@\"");
}

TEST(LoginSession, EnvironmentKeepsWhatTheGreeterMayNotSet) {
    const auto env = session_env({"XDG_SESSION_ID=3", "XDG_VTNR=1", "PATH=/pam/path"}, {"ann", "/home/ann", "/bin/zsh"},
                                 {"LD_PRELOAD=/evil.so", "XDG_SESSION_ID=9", "HOME=/tmp", "LANG=de_DE.UTF-8"});
    auto has = [&](const std::string& e) { return std::ranges::find(env, e) != env.end(); };
    EXPECT_TRUE(has("XDG_SESSION_ID=3"));
    EXPECT_TRUE(has("HOME=/home/ann"));
    EXPECT_TRUE(has("SHELL=/bin/zsh"));
    EXPECT_TRUE(has("USER=ann"));
    EXPECT_TRUE(has("PATH=/pam/path"));
    EXPECT_TRUE(has("LANG=de_DE.UTF-8"));
    EXPECT_FALSE(has("LD_PRELOAD=/evil.so"));
    EXPECT_FALSE(has("XDG_SESSION_ID=9"));
    EXPECT_FALSE(has("HOME=/tmp"));
}

} // namespace
