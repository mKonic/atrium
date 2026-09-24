#include "ini_core.hpp"

#include <gtest/gtest.h>

using namespace atrium;

TEST(Ini, GetsFromItsGroupOnly) {
    const char* text = "[A]\nk=1\n# k=x\n[B]\nk = 2 \n";
    EXPECT_EQ(ini::get(text, "A", "k"), "1");
    EXPECT_EQ(ini::get(text, "B", "k"), "2");
    EXPECT_EQ(ini::get(text, "C", "k"), std::nullopt);
}

TEST(Ini, SetsReplacesAndRemoves) {
    const char* entry = "[Desktop Entry]\nName=App\nExec=app\n\n[Desktop Action new]\nName=New\n";
    EXPECT_EQ(ini::set(entry, "Desktop Entry", "Hidden", "true"),
              "[Desktop Entry]\nName=App\nExec=app\nHidden=true\n\n[Desktop Action new]\nName=New\n");
    EXPECT_EQ(ini::set("[Desktop Entry]\nHidden=true\nName=App\n", "Desktop Entry", "Hidden", std::nullopt),
              "[Desktop Entry]\nName=App\n");
    EXPECT_EQ(ini::set("[Desktop Entry]\nName=App\n", "Desktop Entry", "Name", "Other"),
              "[Desktop Entry]\nName=Other\n");
    // Removing what isn't there changes nothing, and makes no group.
    EXPECT_EQ(ini::set("[X]\na=b\n", "Desktop Entry", "Hidden", std::nullopt), "[X]\na=b\n");
}
