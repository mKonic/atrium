#include "file_chooser.hpp"
#include "qt_test.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUrl>

#include <gtest/gtest.h>

using atrium::FileChooser;

namespace {

QByteArray request(const QJsonObject& q) {
    return QJsonDocument(q).toJson();
}

QJsonObject answered(const FileChooser& c) {
    return QJsonDocument::fromJson(c.answer()).object();
}

} // namespace

TEST(FileChooser, ReadsTheRequest) {
    const QJsonArray filters{
        QJsonObject{{"name", "Images"}, {"patterns", QJsonArray{QJsonObject{{"mime", "image/*"}}}}},
        QJsonObject{{"name", "Text"}, {"patterns", QJsonArray{QJsonObject{{"glob", "*.txt"}}}}},
    };
    FileChooser c(request({{"mode", "open"}, {"accept", "_Insert"}, {"multiple", true}, {"filters", filters},
                           {"filter", 1}, {"folder", "/nonexistent"}}));
    EXPECT_EQ(c.title(), "Open File");
    EXPECT_EQ(c.acceptLabel(), "Insert");
    EXPECT_TRUE(c.multiple());
    EXPECT_EQ(c.filters(), (QStringList{"Images", "Text"}));
    EXPECT_EQ(c.filter(), 1);
    EXPECT_EQ(c.patterns().value(0).toMap().value("glob"), "*.txt");
    EXPECT_EQ(c.folder(), QDir::homePath());  // where it was asked to start isn't there
    c.setFilter(0);
    EXPECT_EQ(c.patterns().value(0).toMap().value("mime"), "image/*");

    // Saving can't pick several, or a folder.
    FileChooser s(request({{"mode", "save"}, {"multiple", true}, {"directory", true}, {"name", "a.txt"}}));
    EXPECT_FALSE(s.multiple());
    EXPECT_FALSE(s.directory());
    EXPECT_EQ(s.acceptLabel(), "Save");
    EXPECT_EQ(s.filter(), -1);
}

TEST(FileChooser, AnswersWithUrisAndChoices) {
    const QJsonArray choices{
        QJsonObject{{"id", "encoding"}, {"label", "Encoding"},
                    {"options", QJsonArray{QJsonObject{{"id", "utf8"}, {"label", "UTF-8"}},
                                           QJsonObject{{"id", "latin1"}, {"label", "Latin-1"}}}},
                    {"value", "utf8"}},
        QJsonObject{{"id", "reencode"}, {"label", "Re-encode"}, {"options", QJsonArray{}}, {"value", "false"}},
    };
    FileChooser c(request({{"mode", "open"}, {"choices", choices}}));
    c.setChoice("encoding", "latin1");
    c.open({"/tmp/a b.txt", "/tmp/c.txt"});
    const QJsonObject a = answered(c);
    // One only, as it didn't ask for several; spaces as a URL has them.
    EXPECT_EQ(a.value("uris").toArray(), (QJsonArray{"file:///tmp/a%20b.txt"}));
    EXPECT_EQ(a.value("choices").toArray(),
              (QJsonArray{QJsonArray{"encoding", "latin1"}, QJsonArray{"reencode", "false"}}));
    // Answered once: a second answer changes nothing.
    c.open({"/tmp/c.txt"});
    EXPECT_EQ(answered(c).value("uris").toArray(), (QJsonArray{"file:///tmp/a%20b.txt"}));
}

TEST(FileChooser, AcceptFollowsGtksTable) {
    QTemporaryDir dir;
    QDir(dir.path()).mkdir("sub");
    const QString sub = dir.filePath("sub"), a = dir.filePath("a.txt"), b = dir.filePath("b.txt");
    FileChooser c(request({{"mode", "open"}, {"multiple", true}}));
    EXPECT_EQ(c.acceptList({}, dir.path()), QVariantMap{});  // nothing picked, nothing done
    EXPECT_TRUE(c.answer().isEmpty());
    EXPECT_EQ(c.acceptList({sub}, dir.path()), (QVariantMap{{"enter", sub}}));
    // A folder among files: GTK answers only when all are files.
    EXPECT_EQ(c.acceptList({a, sub}, dir.path()), QVariantMap{});
    EXPECT_TRUE(c.answer().isEmpty());
    c.acceptList({a, b}, dir.path());
    EXPECT_EQ(answered(c).value("uris").toArray().size(), 2);

    // Choosing a folder: the one picked, else the one it's in. A double
    // click is the list's business (it goes in); the button answers.
    FileChooser d(request({{"mode", "open"}, {"directory", true}}));
    EXPECT_EQ(d.action(), "folder");
    EXPECT_EQ(d.acceptList({a}, dir.path()), QVariantMap{});
    EXPECT_TRUE(d.answer().isEmpty());
    d.acceptList({}, dir.path());
    EXPECT_EQ(answered(d).value("uris").toArray(), (QJsonArray{QUrl::fromLocalFile(dir.path()).toString()}));

    FileChooser s(request({{"mode", "save"}}));
    EXPECT_EQ(s.acceptList({sub}, dir.path()), (QVariantMap{{"enter", sub}}));
    EXPECT_EQ(s.acceptList({}, dir.path()), (QVariantMap{{"entry", true}}));
}

TEST(FileChooser, SavesWhatsTyped) {
    QTemporaryDir dir;
    QDir(dir.path()).mkdir("sub");
    QFile(dir.filePath("taken.txt")).open(QIODevice::WriteOnly);
    FileChooser c(request({{"mode", "save"}, {"name", "taken.txt"}, {"folder", dir.path()}}));
    c.setRecentFile({});
    EXPECT_EQ(c.folder(), dir.path());
    EXPECT_TRUE(c.startsSomewhere());
    EXPECT_EQ(c.acceptText("", dir.path()), (QVariantMap{{"focus", true}}));
    // A folder typed is gone into, the field cleared.
    EXPECT_EQ(c.acceptText("sub", dir.path()), (QVariantMap{{"enter", dir.filePath("sub")}, {"clear", true}}));
    EXPECT_EQ(c.acceptText("sub/", dir.path()), (QVariantMap{{"enter", dir.filePath("sub")}, {"clear", true}}));
    EXPECT_TRUE(c.acceptText("missing/x.txt", dir.path()).contains("error"));
    EXPECT_TRUE(c.acceptText("taken.txt/x", dir.path()).value("error").toString().contains("not a folder"));
    const QVariantMap ask = c.acceptText("taken.txt", dir.path());
    EXPECT_TRUE(ask.value("confirm").toString().contains("already exists"));
    EXPECT_TRUE(c.answer().isEmpty());
    // A path relative to the folder, spaces around the name dropped.
    c.acceptText("sub/ new.txt ", dir.path());
    EXPECT_EQ(answered(c).value("uris").toArray(),
              (QJsonArray{QUrl::fromLocalFile(dir.filePath("sub/new.txt")).toString()}));
}

TEST(FileChooser, LocationFieldOpensAndMakesFolders) {
    QTemporaryDir dir;
    QFile(dir.filePath("a.txt")).open(QIODevice::WriteOnly);
    FileChooser o(request({{"mode", "open"}}));
    o.setRecentFile({});
    EXPECT_EQ(o.acceptText("", dir.path()), (QVariantMap{{"list", true}}));
    // A file that isn't there is still an answer when opening (GTK: Emacs uses it).
    o.acceptText("new.txt", dir.path());
    EXPECT_EQ(answered(o).value("uris").toArray(),
              (QJsonArray{QUrl::fromLocalFile(dir.filePath("new.txt")).toString()}));

    FileChooser f(request({{"mode", "open"}, {"directory", true}}));
    f.setRecentFile({});
    EXPECT_TRUE(f.acceptText("a.txt", dir.path()).value("error").toString().contains("only select folders"));
    // A folder typed that isn't there is made, then chosen.
    f.acceptText("made", dir.path());
    EXPECT_TRUE(QFileInfo(dir.filePath("made")).isDir());
    EXPECT_EQ(answered(f).value("uris").toArray(),
              (QJsonArray{QUrl::fromLocalFile(dir.filePath("made")).toString()}));
}

TEST(FileChooser, SavesSeveralUnderFreeNames) {
    QTemporaryDir dir;
    QFile(dir.filePath("a.png")).open(QIODevice::WriteOnly);
    FileChooser c(request({{"mode", "saveFiles"}, {"files", QJsonArray{"a.png", "b.png", "b.png"}}}));
    EXPECT_EQ(c.action(), "folder");
    EXPECT_EQ(c.saveFilesPaths(dir.path()),
              (QStringList{dir.filePath("a 2.png"), dir.filePath("b.png"), dir.filePath("b 2.png")}));
}

TEST(FileChooser, ReadOnlyAndRecent) {
    QTemporaryDir dir;
    QFile(dir.filePath("a.txt")).open(QIODevice::WriteOnly);
    FileChooser c(request({{"mode", "open"}, {"app", "org.example.App"}}));
    c.setRecentFile(dir.filePath("recent.xbel"));
    EXPECT_EQ(c.acceptLabel(), "Select");
    c.setReadOnly(true);
    c.open({dir.filePath("a.txt")});
    EXPECT_FALSE(answered(c).value("writable").toBool(true));
    QFile recent(dir.filePath("recent.xbel"));
    ASSERT_TRUE(recent.open(QIODevice::ReadOnly));
    const QByteArray xbel = recent.readAll();
    EXPECT_TRUE(xbel.contains(QUrl::fromLocalFile(dir.filePath("a.txt")).toEncoded()));
    EXPECT_TRUE(xbel.contains("org.example.App"));
}

TEST(FileChooser, ChecksNamesAndMakesFolders) {
    QTemporaryDir dir;
    FileChooser c(request({{"mode", "save"}}));
    EXPECT_EQ(c.makeFolder(dir.path(), "New"), (QVariantMap{{"path", dir.filePath("New")}}));
    EXPECT_TRUE(QFileInfo(dir.filePath("New")).isDir());
    EXPECT_EQ(c.makeFolder(dir.path(), "New").value("error"), "A folder with that name already exists");
    EXPECT_TRUE(c.makeFolder(dir.path(), "a/b").contains("error"));
    EXPECT_EQ(c.checkName(dir.path(), ".x", true).value("warning"), "Folder names starting with a “.” are hidden");
    const QVariantMap renamed = c.rename(dir.filePath("New"), "Old");
    EXPECT_EQ(renamed.value("path"), dir.filePath("Old"));
}
