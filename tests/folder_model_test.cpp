#include "folder_model.hpp"
#include "qt_test.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using atrium::FolderModel;

namespace {

// A folder with a few files of known kinds.
struct Folder {
    QTemporaryDir dir;
    Folder() {
        for (const char* name : {"b.png", "a.txt", "c.JPG", "notes", "d.pdf"}) {
            QFile f(dir.filePath(name));
            f.open(QIODevice::WriteOnly);
            f.write(QByteArray(name) == "notes" ? "plain words\n" : "x");
        }
        QDir(dir.path()).mkdir("sub");
    }
};

QStringList names(const FolderModel& m) {
    QStringList out;
    for (int i = 0; i < m.count(); ++i)
        out << m.data(m.index(i), FolderModel::NameRole).toString();
    return out;
}

} // namespace

TEST(FolderModel, PatternsByGlobAndMime) {
    Folder f;
    FolderModel m;
    m.setFolder(f.dir.path());
    EXPECT_EQ(names(m), (QStringList{"sub", "a.txt", "b.png", "c.JPG", "d.pdf", "notes"}));
    m.setPatterns({QVariantMap{{"glob", "*.png"}}, QVariantMap{{"mime", "application/pdf"}}});
    EXPECT_EQ(names(m), (QStringList{"sub", "b.png", "d.pdf"}));
    // "image/*": every image, whatever its suffix's case.
    m.setPatterns({QVariantMap{{"mime", "image/*"}}});
    EXPECT_EQ(names(m), (QStringList{"sub", "b.png", "c.JPG"}));
    // text/plain by content, for a file with no suffix.
    m.setPatterns({QVariantMap{{"mime", "text/plain"}}});
    EXPECT_EQ(names(m), (QStringList{"sub", "a.txt", "notes"}));
    EXPECT_TRUE(m.resolve("notes").value("allowed").toBool());
    EXPECT_FALSE(m.resolve("b.png").value("allowed").toBool());
    m.setFoldersOnly(true);
    EXPECT_EQ(names(m), (QStringList{"sub"}));
    EXPECT_TRUE(m.resolve("sub").value("allowed").toBool());
    EXPECT_FALSE(m.resolve("notes").value("allowed").toBool());
}

TEST(FolderModel, PicksOneOrSeveral) {
    Folder f;
    FolderModel m;
    m.setFolder(f.dir.path());
    int changed = 0;
    QObject::connect(&m, &FolderModel::selectionChanged, [&] { ++changed; });
    const QString a = f.dir.filePath("a.txt"), b = f.dir.filePath("b.png"), c = f.dir.filePath("c.JPG");
    // One at a time: Ctrl and Shift pick only the one clicked.
    m.select(1);
    m.select(3, FolderModel::Toggle);
    EXPECT_EQ(m.selection(), QStringList{c});
    m.setMultiple(true);
    m.select(1, FolderModel::Toggle);
    EXPECT_EQ(m.selection(), (QStringList{a, c}));
    m.select(3, FolderModel::Toggle);
    EXPECT_EQ(m.selection(), QStringList{a});
    // Shift: from the last one clicked.
    m.select(1);
    m.select(3, FolderModel::Extend);
    EXPECT_EQ(m.selection(), (QStringList{a, b, c}));
    EXPECT_TRUE(m.data(m.index(2), FolderModel::SelectedRole).toBool());
    m.select(2);
    EXPECT_EQ(m.selection(), QStringList{b});
    // A change in the folder keeps the pick; another folder drops it.
    QFile(f.dir.filePath("e.txt")).open(QIODevice::WriteOnly);
    m.setShowHidden(true);
    EXPECT_EQ(m.selection(), QStringList{b});
    const int before = changed;
    m.setFolder(f.dir.filePath("sub"));
    EXPECT_TRUE(m.selection().isEmpty());
    EXPECT_EQ(changed, before + 1);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
