#include "dbus_menu.hpp"
#include "qt_test.hpp"

#include <QVariantMap>

#include <gtest/gtest.h>

using namespace atrium;

using dbusmenu::Node;

TEST(DBusMenu, ShortcutsAsAMacWritesThem) {
    EXPECT_EQ(dbusmenu::shortcutText(QVariant::fromValue(QList<QStringList>{{"Control", "Shift", "s"}})),
              QString::fromUtf8("⌃⇧S"));
    EXPECT_EQ(dbusmenu::shortcutText(QVariant::fromValue(QList<QStringList>{{"F1"}})), "F1");
    EXPECT_EQ(dbusmenu::shortcutText(QVariant::fromValue(QList<QStringList>{{"Alt", "Super", "Delete"}, {"x"}})),
              QString::fromUtf8("⌥⌘Delete"));
    EXPECT_EQ(dbusmenu::shortcutText(QVariant()), "");
}

TEST(DBusMenu, EntriesAsTheShellShowsThem) {
    const Node root{0, {}, {
        {1, {{"label", "_File"}, {"children-display", "submenu"}}, {}},
        {2, {{"type", "separator"}}, {}},
        {3, {{"label", "Save __As"}, {"enabled", false}}, {}},
        {4, {{"label", "Hidden"}, {"visible", false}}, {}},
        {5, {{"label", "Word Wrap"}, {"toggle-type", "checkmark"}, {"toggle-state", 1}}, {}},
        {6, {{"label", "Recent"}}, {{7, {{"label", "a.txt"}}, {}}}},
    }};
    const QVariantList e = dbusmenu::entries(root);
    ASSERT_EQ(e.size(), 5);  // the hidden one left out
    EXPECT_EQ(e[0].toMap().value("text"), "File");  // mnemonic gone
    EXPECT_TRUE(e[0].toMap().value("submenu").toBool());  // a submenu still to fill
    EXPECT_TRUE(e[1].toMap().value("separator").toBool());
    EXPECT_EQ(e[2].toMap().value("text"), "Save _As");
    EXPECT_FALSE(e[2].toMap().value("enabled").toBool());
    EXPECT_TRUE(e[3].toMap().value("checked").toBool());
    EXPECT_EQ(e[4].toMap().value("children").toList().value(0).toMap().value("text"), "a.txt");
}
