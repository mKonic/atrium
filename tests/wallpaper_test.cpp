#include "wallpaper_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::wallpaper;

TEST(Wallpaper, CleansPaths) {
    EXPECT_EQ(clean_path("~/Pictures/a.png", "/home/u"), "/home/u/Pictures/a.png");
    EXPECT_EQ(clean_path(" \"/x/y z.jpg\" ", "/home/u"), "/x/y z.jpg");
    EXPECT_EQ(clean_path("file:///x/y%20z%23.jpg", "/home/u"), "/x/y z#.jpg");
    EXPECT_EQ(clean_path("file:///x/100%", "/home/u"), "/x/100%");
}

TEST(Wallpaper, Hyprpaper) {
    EXPECT_EQ(from_hyprpaper("preload = ~/a.png\nwallpaper = DP-1,~/b.png\n", "/h"), "/h/b.png");
    EXPECT_EQ(from_hyprpaper("preload = ~/a.png\n# wallpaper = ,~/c.png\n", "/h"), "/h/a.png");
    EXPECT_EQ(from_hyprpaper("wallpaper = ,/w.jpg\n", "/h"), "/w.jpg");
    EXPECT_EQ(from_hyprpaper("wallpaper {\n    monitor = DP-1\n    path = ~/n.png\n}\n", "/h"), "/h/n.png");
    EXPECT_EQ(from_hyprpaper("splash = false\n", "/h"), "");
}

TEST(Wallpaper, Plasma) {
    const char* conf = "[Containments][1][General]\nImage=file:///not/this.png\n\n"
                       "[Containments][1][Wallpaper][org.kde.image][General]\n"
                       "Image=file:///usr/share/wallpapers/Next/contents/images/5120x2880.png\nSlidePaths=/x\n";
    EXPECT_EQ(from_plasma(conf, "/h"), "/usr/share/wallpapers/Next/contents/images/5120x2880.png");
    EXPECT_EQ(from_plasma("[General]\nImage=/x.png\n", "/h"), "");
}

TEST(Wallpaper, Waypaper) {
    EXPECT_EQ(from_waypaper("[Settings]\nfolder = ~/Pictures\nwallpaper = ~/Pictures/w.png\n", "/h"),
              "/h/Pictures/w.png");
    EXPECT_EQ(from_waypaper("[Settings]\nwallpaper =\n", "/h"), "");
}
