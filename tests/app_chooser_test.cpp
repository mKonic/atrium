#include "app_chooser.hpp"
#include "qt_test.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using atrium::AppChooser;

TEST(AppChooser, LastUsedFirst) {
    AppChooser c(true, nullptr);
    c.feed(R"({"choices":["org.kde.okular","org.gnome.Evince","firefox"],"last":"firefox",)"
           R"("contentType":"application/pdf","filename":"/home/me/Report 2.pdf"})");
    EXPECT_EQ(c.choices(), (QStringList{"firefox", "org.kde.okular", "org.gnome.Evince"}));
    EXPECT_EQ(c.last(), "firefox");
    EXPECT_EQ(c.subject(), "Report 2.pdf");
    EXPECT_EQ(c.kind(), "PDF document");
    // A link: its address.
    AppChooser l(true, nullptr);
    l.feed(R"({"choices":["firefox"],"uri":"https://example.org/a?b=c","contentType":"x-scheme-handler/https"})");
    EXPECT_EQ(l.subject(), "https://example.org/a?b=c");
}

TEST(AppChooser, UpdatesKeepWhatWasSaid) {
    AppChooser c(true, nullptr);
    int changed = 0;
    QObject::connect(&c, &AppChooser::choicesChanged, [&] { ++changed; });
    c.feed(R"({"choices":["a","b"],"last":"b","filename":"x.txt","contentType":"text/plain"})");
    c.feed(R"({"choices":["a","b","c"]})");
    EXPECT_EQ(c.choices(), (QStringList{"b", "a", "c"}));
    EXPECT_EQ(c.subject(), "x.txt");
    EXPECT_EQ(changed, 2);
    c.feed("not json");
    EXPECT_EQ(changed, 2);
}

TEST(AppChooser, AnswersOnlyWhatWasOffered) {
    AppChooser c(true, nullptr);
    c.feed(R"({"choices":["a","b"]})");
    c.choose("evil");
    EXPECT_TRUE(c.answer().isEmpty());
    c.choose("b");
    EXPECT_EQ(c.answer(), R"({"choice":"b"})");
    c.choose("a");
    EXPECT_EQ(c.answer(), R"({"choice":"b"})");
}

TEST(AppChooser, NamesFromDesktopFilesAndLinksByScheme) {
    QTemporaryDir data;
    QDir(data.path()).mkpath("applications");
    auto write = [&](const char* id, const char* text) {
        QFile f(data.path() + "/applications/" + id + ".desktop");
        f.open(QIODevice::WriteOnly);
        f.write(text);
    };
    write("google-chrome", "[Desktop Entry]\nType=Application\nName=Google Chrome\nIcon=google-chrome\nExec=x\n");
    write("com.google.Chrome", "[Desktop Entry]\nType=Application\nName=Google Chrome\nExec=x\n");
    write("firefox", "[Desktop Entry]\nType=Application\nName=Firefox\nIcon=firefox\nExec=x\n");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    qputenv("XDG_DATA_DIRS", "/nonexistent");
    AppChooser c(true, nullptr);
    c.feed(R"({"choices":["google-chrome","com.google.Chrome","firefox","gone"],"uri":"https://a.b",)"
           R"("contentType":"x-scheme-handler/https"})");
    qunsetenv("XDG_DATA_HOME");
    qunsetenv("XDG_DATA_DIRS");
    ASSERT_EQ(c.apps().size(), 4);
    const QVariantMap chrome = c.apps()[0].toMap(), flatpak = c.apps()[1].toMap(), firefox = c.apps()[2].toMap(),
                      gone = c.apps()[3].toMap();
    EXPECT_EQ(chrome.value("name"), "Google Chrome");
    EXPECT_EQ(chrome.value("icon"), "google-chrome");
    EXPECT_EQ(chrome.value("detail"), "google-chrome");  // two Google Chromes: which is which
    EXPECT_EQ(flatpak.value("detail"), "com.google.Chrome");
    EXPECT_EQ(flatpak.value("icon"), "com.google.Chrome");  // none given: its id
    EXPECT_EQ(firefox.value("detail"), "");
    EXPECT_EQ(gone.value("name"), "gone");
    EXPECT_EQ(c.kind(), "Web page");
}
