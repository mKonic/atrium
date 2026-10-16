#include "chooser_model.hpp"
#include "qt_test.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using atrium::ChooserModel;

namespace {

struct Folder {
    QTemporaryDir dir;
    Folder() {
        for (const char* name : {"file10.txt", "file2.txt", "b.png", ".hidden", "notes"}) {
            QFile f(dir.filePath(name));
            f.open(QIODevice::WriteOnly);
            f.write(QByteArray(name).repeated(int(qstrlen(name))));
        }
        QDir(dir.path()).mkpath("sub/deeper");
        QDir(dir.path()).mkdir("Zeta");
        QFile(dir.filePath("sub/deeper/file2 match.txt")).open(QIODevice::WriteOnly);
    }
    QString path(const char* name) const { return dir.filePath(name); }
};

QStringList names(const ChooserModel& m) {
    QStringList out;
    for (int i = 0; i < m.count(); ++i)
        out << m.row(i).value("name").toString();
    return out;
}

} // namespace

TEST(ChooserModel, ListsLikeGtk) {
    Folder f;
    ChooserModel m;
    m.setFolder(f.dir.path());
    // Folders first, numbers by value, hidden ones left out.
    EXPECT_EQ(names(m), (QStringList{"sub", "Zeta", "b.png", "file2.txt", "file10.txt", "notes"}));
    m.setShowHidden(true);
    EXPECT_TRUE(names(m).contains(".hidden"));
    m.setShowHidden(false);
    m.setSortColumn("size");
    m.setAscending(false);
    QStringList top = names(m).mid(0, 2);
    top.sort();
    EXPECT_EQ(top, (QStringList{"Zeta", "sub"}));  // folders still first
    m.setFoldersFirst(false);
    EXPECT_EQ(names(m).first(), "file10.txt");  // the biggest
}

TEST(ChooserModel, ChoosingAFolderGreysFilesAndGoesIn) {
    Folder f;
    ChooserModel m;
    m.setAction("folder");
    m.setFolder(f.dir.path());
    const int file = m.indexOf(f.path("b.png")), sub = m.indexOf(f.path("sub"));
    ASSERT_GE(file, 0);
    // Files are there, greyed, and can't be picked.
    EXPECT_FALSE(m.data(m.index(file), ChooserModel::SensitiveRole).toBool());
    m.select(file);
    EXPECT_TRUE(m.selection().isEmpty());
    m.select(sub);
    EXPECT_EQ(m.selection(), QStringList{f.path("sub")});
    // A double click goes in; it doesn't answer.
    EXPECT_EQ(m.activate(sub), (QVariantMap{{"enter", f.path("sub")}}));
    EXPECT_EQ(m.folder(), f.path("sub"));
}

TEST(ChooserModel, PathBarKeepsItsTrail) {
    Folder f;
    ChooserModel m;
    m.setFolder(f.path("sub/deeper"));
    m.goTrail(f.dir.path());
    // Going up keeps sub and deeper on the bar, and picks the one it came from.
    QStringList trail;
    for (const QVariant& v : m.trail())
        trail << v.toMap().value("path").toString();
    EXPECT_TRUE(trail.contains(f.path("sub/deeper")));
    EXPECT_EQ(m.selection(), QStringList{f.path("sub")});
    m.down();
    EXPECT_EQ(m.folder(), f.path("sub"));
    m.down();
    EXPECT_EQ(m.folder(), f.path("sub/deeper"));
    // Somewhere else: a new trail.
    m.setFolder(f.path("Zeta"));
    trail.clear();
    for (const QVariant& v : m.trail())
        trail << v.toMap().value("path").toString();
    EXPECT_FALSE(trail.contains(f.path("sub/deeper")));
}

TEST(ChooserModel, FiltersAndPicksAHiddenFile) {
    Folder f;
    ChooserModel m;
    m.setPatterns({QVariantMap{{"glob", "*.txt"}}});
    m.setFolder(f.dir.path());
    EXPECT_EQ(names(m), (QStringList{"sub", "Zeta", "file2.txt", "file10.txt"}));  // folders are never filtered
    // Picking a hidden file shows hidden ones.
    m.setPatterns({});
    m.selectPath(f.path(".hidden"));
    EXPECT_TRUE(m.showHidden());
    EXPECT_EQ(m.selection(), QStringList{f.path(".hidden")});
}

TEST(ChooserModel, SearchesUnderTheFolder) {
    Folder f;
    ChooserModel m;
    m.setFolder(f.dir.path());
    m.startSearch();
    m.setQuery("FILE2");
    QElapsedTimer t;
    t.start();
    while ((m.searching() || m.count() < 2) && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_EQ(m.mode(), "search");
    QStringList found = names(m);
    found.sort();
    EXPECT_EQ(found, (QStringList{"file2 match.txt", "file2.txt"}));
    // Where each is, for the Location column.
    const int deep = m.indexOf(f.path("sub/deeper/file2 match.txt"));
    EXPECT_FALSE(m.data(m.index(deep), ChooserModel::LocationRole).toString().isEmpty());
    m.stopSearch();
    EXPECT_EQ(m.mode(), "browse");
}

TEST(ChooserModel, CompletesTypedPaths) {
    Folder f;
    ChooserModel m;
    m.setFolder(f.dir.path());
    EXPECT_EQ(m.complete("su"), "sub/");
    EXPECT_EQ(m.complete("sub/de"), "sub/deeper/");
    EXPECT_EQ(m.complete("file"), "file");  // file2.txt and file10.txt
    const QVariantList c = m.completions("file");
    EXPECT_EQ(c.size(), 2);
    m.setAction("folder");
    EXPECT_TRUE(m.completions("file").isEmpty());  // only folders
}
