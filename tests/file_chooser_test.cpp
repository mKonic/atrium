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
    EXPECT_EQ(c.title(), "Open");
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

TEST(FileChooser, SavesUnderAName) {
    QTemporaryDir dir;
    QDir(dir.path()).mkdir("sub");
    QFile(dir.filePath("taken.txt")).open(QIODevice::WriteOnly);
    FileChooser c(request({{"mode", "save"}, {"name", "taken.txt"}, {"folder", dir.path()}}));
    EXPECT_EQ(c.folder(), dir.path());
    EXPECT_EQ(c.name(), "taken.txt");
    EXPECT_TRUE(c.exists(dir.path(), "taken.txt"));
    EXPECT_TRUE(c.saveProblem(dir.path(), "taken.txt").isEmpty());  // replacing it is asked about, not refused
    EXPECT_FALSE(c.saveProblem(dir.path(), "  ").isEmpty());
    EXPECT_FALSE(c.saveProblem(dir.path(), "a/b").isEmpty());
    EXPECT_FALSE(c.saveProblem(dir.path(), "sub").isEmpty());
    EXPECT_FALSE(c.saveProblem(dir.path() + "/missing", "x").isEmpty());
    c.save(dir.path(), "a/b");  // refused: no answer
    EXPECT_TRUE(c.answer().isEmpty());
    c.save(dir.path(), " new.txt ");
    EXPECT_EQ(answered(c).value("uris").toArray(),
              (QJsonArray{QUrl::fromLocalFile(dir.filePath("new.txt")).toString()}));
}

TEST(FileChooser, SavesSeveralUnderFreeNames) {
    QTemporaryDir dir;
    QFile(dir.filePath("a.png")).open(QIODevice::WriteOnly);
    FileChooser c(request({{"mode", "saveFiles"}, {"files", QJsonArray{"a.png", "b.png", "b.png"}}}));
    EXPECT_EQ(c.saveFilesPaths(dir.path()),
              (QStringList{dir.filePath("a 2.png"), dir.filePath("b.png"), dir.filePath("b 2.png")}));
}

TEST(FileChooser, AcceptOpensFilesAndGoesIntoFolders) {
    QTemporaryDir dir;
    QDir(dir.path()).mkdir("sub");
    const QString sub = dir.filePath("sub"), a = dir.filePath("a.txt"), b = dir.filePath("b.txt");
    FileChooser c(request({{"mode", "open"}, {"multiple", true}}));
    EXPECT_EQ(c.accept({}, dir.path(), {}), QVariantMap{});  // nothing picked, nothing done
    EXPECT_TRUE(c.answer().isEmpty());
    EXPECT_EQ(c.accept({sub}, dir.path(), {}), (QVariantMap{{"enter", sub}}));
    // Files picked with a folder: the files.
    EXPECT_EQ(c.accept({a, sub, b}, dir.path(), {}), QVariantMap{});
    EXPECT_EQ(answered(c).value("uris").toArray().size(), 2);

    // A folder chooser takes the folder picked, else the one it's in.
    FileChooser d(request({{"mode", "open"}, {"directory", true}}));
    d.accept({}, dir.path(), {});
    EXPECT_EQ(answered(d).value("uris").toArray(), (QJsonArray{QUrl::fromLocalFile(dir.path()).toString()}));
}

TEST(FileChooser, AcceptAsksBeforeReplacing) {
    QTemporaryDir dir;
    QDir(dir.path()).mkdir("sub");
    QFile(dir.filePath("taken.txt")).open(QIODevice::WriteOnly);
    FileChooser c(request({{"mode", "save"}}));
    EXPECT_EQ(c.accept({dir.filePath("sub")}, dir.path(), "x"), (QVariantMap{{"enter", dir.filePath("sub")}}));
    EXPECT_TRUE(c.accept({}, dir.path(), "").contains("error"));
    EXPECT_TRUE(c.accept({}, dir.path(), "taken.txt").contains("confirm"));
    EXPECT_TRUE(c.answer().isEmpty());
    EXPECT_EQ(c.accept({}, dir.path(), "taken.txt", true), QVariantMap{});
    EXPECT_EQ(answered(c).value("uris").toArray(),
              (QJsonArray{QUrl::fromLocalFile(dir.filePath("taken.txt")).toString()}));
}

TEST(FileChooser, MakesFolders) {
    QTemporaryDir dir;
    FileChooser c(request({{"mode", "save"}}));
    EXPECT_EQ(c.makeFolder(dir.path(), " New "), (QVariantMap{{"path", dir.filePath("New")}}));
    EXPECT_TRUE(QFileInfo(dir.filePath("New")).isDir());
    EXPECT_TRUE(c.makeFolder(dir.path(), "New").contains("error"));
    EXPECT_TRUE(c.makeFolder(dir.path(), "a/b").contains("error"));
}
