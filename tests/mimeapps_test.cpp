#include "mimeapps_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::mimeapps;

TEST(MimeApps, ReadsTheFirstDefault) {
    const char* text = "[Added Associations]\ntext/html=chromium.desktop;\n\n"
                       "[Default Applications]\ntext/html = firefox.desktop;chromium.desktop;\n";
    EXPECT_EQ(default_for(text, "text/html"), "firefox.desktop");
    EXPECT_EQ(default_for(text, "image/png"), "");
    EXPECT_EQ(default_for("", "text/html"), "");
}

TEST(MimeApps, ReplacesAnExistingDefault) {
    const char* text = "# mine\n[Default Applications]\ntext/html=firefox.desktop;\nimage/png=gwenview.desktop;\n";
    EXPECT_EQ(set_default(text, "text/html", "chromium.desktop"),
              "# mine\n[Default Applications]\ntext/html=chromium.desktop;\nimage/png=gwenview.desktop;\n");
}

TEST(MimeApps, AddsToTheGroupAndKeepsTheRest) {
    const char* text = "[Default Applications]\nimage/png=gwenview.desktop;\n\n[Added Associations]\nx=y.desktop;\n";
    EXPECT_EQ(set_default(text, "text/html", "firefox.desktop"),
              "[Default Applications]\nimage/png=gwenview.desktop;\ntext/html=firefox.desktop;\n\n"
              "[Added Associations]\nx=y.desktop;\n");
}

TEST(MimeApps, MakesTheGroup) {
    EXPECT_EQ(set_default("", "text/html", "firefox.desktop"), "[Default Applications]\ntext/html=firefox.desktop;\n");
    EXPECT_EQ(set_default("[Added Associations]\nx=y.desktop;\n", "text/html", "firefox.desktop"),
              "[Added Associations]\nx=y.desktop;\n\n[Default Applications]\ntext/html=firefox.desktop;\n");
}

TEST(MimeApps, OnlyTheDefaultsGroupCounts) {
    const char* text = "[Added Associations]\ntext/html=chromium.desktop;\n";
    EXPECT_EQ(default_for(text, "text/html"), "");
    EXPECT_EQ(set_default(text, "text/html", "firefox.desktop"),
              "[Added Associations]\ntext/html=chromium.desktop;\n\n[Default Applications]\ntext/html=firefox.desktop;\n");
}
