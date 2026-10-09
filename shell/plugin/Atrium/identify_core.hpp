#pragma once
// Identify in Displays, as KWin's Output Locator (outputlocator.cpp): each
// screen shows its number, its name and its mode. The name: "Built-in
// Screen" for a laptop's own panel, else its maker and model (and serial
// number when another screen is the same model) above its connector. Kept
// free of Qt to be testable.
#include <string>
#include <vector>

namespace atrium::identify {

struct Screen {
    std::string connector;  // "DP-1"
    std::string make, model, serial;
    bool built_in = false;
    int width = 0, height = 0;  // in pixels
    double scale = 1;
};

struct Label {
    int number = 0;     // from 1, in connector order (KWin: priority, then name)
    std::string name;   // lines joined by '\n'
    std::string mode;   // "2560x1440", "2560x1440@150%"
};

// One label for each screen, in the order given.
std::vector<Label> labels(const std::vector<Screen>& screens);

} // namespace atrium::identify
