#include "privacy_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::privacy;

TEST(Privacy, WhatCountsAsTheMicrophone) {
    const std::vector<Capture> streams{
        {"Discord", "RecordStream", false, false},
        {"OBS", "Desktop Audio", true, false},       // a speaker's monitor
        {"Firefox", "AudioStream", false, true},     // paused
        {"pavucontrol", "Peak detect", false, false},  // a level meter
        {"atrium", "Screen recording", false, false},  // our own
        {"", "", false, false},
    };
    EXPECT_EQ(microphone_apps(streams), (std::vector<std::string>{"Discord", "An app"}));
}

TEST(Privacy, Cameras) {
    EXPECT_TRUE(is_camera("/dev/video0"));
    EXPECT_TRUE(is_camera("/dev/video12"));
    EXPECT_FALSE(is_camera("/dev/video"));
    EXPECT_FALSE(is_camera("/dev/video0-old"));
    EXPECT_FALSE(is_camera("/dev/vhost-net"));
    const std::map<int, std::vector<std::string>> fds{
        {100, {"/dev/null", "/dev/video0"}},
        {200, {"/dev/dri/card1"}},
        {300, {"/dev/video2"}},
    };
    EXPECT_EQ(camera_apps(fds, {{100, "chrome"}, {300, "pipewire"}}),
              (std::vector<std::string>{"chrome", "An app (through PipeWire)"}));
}

TEST(Privacy, OneLineEachScreenFirst) {
    const auto merged = merge({{Kind::Microphone, "Discord"}, {Kind::Screen, "OBS"}, {Kind::Microphone, "Discord"},
                               {Kind::Camera, "Zoom"}, {Kind::Microphone, "Zoom"}});
    EXPECT_EQ(merged, (std::vector<Use>{{Kind::Screen, "OBS"}, {Kind::Camera, "Zoom"}, {Kind::Microphone, "Discord"},
                                        {Kind::Microphone, "Zoom"}}));
}
