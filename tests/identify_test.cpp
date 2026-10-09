#include "identify_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::identify;

TEST(Identify, NamesAScreenByMakerModelAndConnector) {
    const auto l = labels({{"DP-1", "Dell Inc.", "DELL U2720Q", "ABC123", false, 3840, 2160, 1.5}});
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].number, 1);
    EXPECT_EQ(l[0].name, "Dell Inc.\nDELL U2720Q\nDP-1");
    EXPECT_EQ(l[0].mode, "3840x2160@150%");
}

TEST(Identify, ALaptopPanelIsTheBuiltInScreen) {
    const auto l = labels({{"eDP-1", "BOE", "0x0BCA", "", true, 1920, 1080, 1}});
    EXPECT_EQ(l[0].name, "Built-in Screen");
    EXPECT_EQ(l[0].mode, "1920x1080");
}

TEST(Identify, TwinScreensShowTheirSerials) {
    const auto l = labels({
        {"HDMI-A-1", "LG", "27GL850", "111", false, 2560, 1440, 1},
        {"DP-2", "LG", "27GL850", "222", false, 2560, 1440, 1},
    });
    EXPECT_EQ(l[0].name, "LG\n27GL850\n111\nHDMI-A-1");
    EXPECT_EQ(l[1].name, "LG\n27GL850\n222\nDP-2");
}

TEST(Identify, NumberedByConnectorName) {
    const auto l = labels({
        {"HDMI-A-1", "A", "x", "", false, 1, 1, 1},
        {"dp-2", "B", "y", "", false, 1, 1, 1},
        {"DP-1", "C", "z", "", false, 1, 1, 1},
    });
    EXPECT_EQ(l[0].number, 3);
    EXPECT_EQ(l[1].number, 2);
    EXPECT_EQ(l[2].number, 1);
}

TEST(Identify, MissingPartsAreLeftOut) {
    const auto l = labels({{"HEADLESS-1", "", "", "", false, 1280, 720, 2}});
    EXPECT_EQ(l[0].name, "HEADLESS-1");
    EXPECT_EQ(l[0].mode, "1280x720@200%");
}
