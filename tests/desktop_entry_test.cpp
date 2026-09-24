#include "desktop_entry_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::desktop_entry;

namespace {

const char* kFirefox = R"([Desktop Entry]
Type=Application
Name=Firefox
Name[de]=Firefox-Browser
GenericName=Web Browser
GenericName[de_DE]=Webbrowser
Comment=Browse the Web
Icon=firefox
Exec=/usr/lib/firefox/firefox %u
Keywords=Internet;WWW;Browser\;Web;
Categories=Network;WebBrowser;
StartupWMClass=firefox
Actions=new-window;new-private-window;

[Desktop Action new-window]
Name=New Window
Name[de]=Neues Fenster
Exec=/usr/lib/firefox/firefox --new-window %u

[Desktop Action new-private-window]
Name=New Private Window
Exec=/usr/lib/firefox/firefox --private-window %u
)";

} // namespace

TEST(DesktopEntry, ParsesTheEntryAndItsActions) {
    auto e = parse(kFirefox, "");
    ASSERT_TRUE(e);
    EXPECT_EQ(e->type, "Application");
    EXPECT_EQ(e->name, "Firefox");
    EXPECT_EQ(e->generic_name, "Web Browser");
    EXPECT_EQ(e->icon, "firefox");
    EXPECT_EQ(e->startup_wm_class, "firefox");
    ASSERT_EQ(e->actions.size(), 2u);
    EXPECT_EQ(e->actions[0].id, "new-window");
    EXPECT_EQ(e->actions[0].name, "New Window");
    EXPECT_EQ(e->actions[1].exec, "/usr/lib/firefox/firefox --private-window %u");
}

TEST(DesktopEntry, PicksTheBestTranslation) {
    // de_DE.UTF-8: GenericName[de_DE] exactly, Name through plain "de".
    auto e = parse(kFirefox, "de_DE.UTF-8");
    ASSERT_TRUE(e);
    EXPECT_EQ(e->name, "Firefox-Browser");
    EXPECT_EQ(e->generic_name, "Webbrowser");
    EXPECT_EQ(e->actions[0].name, "Neues Fenster");
    // No translation: the plain value.
    EXPECT_EQ(parse(kFirefox, "fr_FR.UTF-8")->name, "Firefox");
}

TEST(DesktopEntry, ListsKeepEscapedSemicolons) {
    auto e = parse(kFirefox, "");
    ASSERT_TRUE(e);
    EXPECT_EQ(e->keywords, (std::vector<std::string>{"Internet", "WWW", "Browser;Web"}));
    EXPECT_EQ(e->categories, (std::vector<std::string>{"Network", "WebBrowser"}));
}

TEST(DesktopEntry, NeedsTheMainGroup) {
    EXPECT_FALSE(parse("[Something Else]\nName=x\n", ""));
    EXPECT_FALSE(parse("", ""));
}

TEST(DesktopEntry, FirstDuplicateKeyWinsAndCommentsAreSkipped) {
    auto e = parse("# a comment\n[Desktop Entry]\nName=One\nName=Two\nNoDisplay=true\nTerminal=1\n", "");
    ASSERT_TRUE(e);
    EXPECT_EQ(e->name, "One");
    EXPECT_TRUE(e->no_display);
    EXPECT_TRUE(e->terminal);
}

TEST(DesktopEntry, UnescapesValues) {
    EXPECT_EQ(unescape(R"(a\sb\nc\\d)"), "a b\nc\\d");
}

TEST(DesktopEntry, ExecDropsFileCodesAndExpandsTheRest) {
    EXPECT_EQ(exec_argv("firefox %u"), (std::vector<std::string>{"firefox"}));
    EXPECT_EQ(exec_argv("app %F --flag"), (std::vector<std::string>{"app", "--flag"}));
    EXPECT_EQ(exec_argv("app %i", "App", "app-icon"), (std::vector<std::string>{"app", "--icon", "app-icon"}));
    EXPECT_EQ(exec_argv("app %i", "App", ""), (std::vector<std::string>{"app"}));
    EXPECT_EQ(exec_argv("app --name=%c", "My App"), (std::vector<std::string>{"app", "--name=My App"}));
    EXPECT_EQ(exec_argv("app %k", "", "", "/x.desktop"), (std::vector<std::string>{"app", "/x.desktop"}));
    EXPECT_EQ(exec_argv("echo 100%%"), (std::vector<std::string>{"echo", "100%"}));
}

TEST(DesktopEntry, ExecQuoting) {
    EXPECT_EQ(exec_argv(R"("/opt/My App/run" --x)"), (std::vector<std::string>{"/opt/My App/run", "--x"}));
    EXPECT_EQ(exec_argv(R"(sh -c "echo \"hi\" \$HOME")"),
              (std::vector<std::string>{"sh", "-c", "echo \"hi\" $HOME"}));
    EXPECT_TRUE(exec_argv(R"(app "unclosed)").empty());
}

TEST(DesktopEntry, ShownOnThisDesktop) {
    Entry e;
    EXPECT_TRUE(shown_in(e, "atrium"));
    e.only_show_in = {"KDE"};
    EXPECT_FALSE(shown_in(e, "atrium"));
    EXPECT_TRUE(shown_in(e, "atrium:KDE"));
    e.only_show_in.clear();
    e.not_show_in = {"atrium"};
    EXPECT_FALSE(shown_in(e, "atrium"));
    EXPECT_TRUE(shown_in(e, "GNOME"));
}
