#pragma once
// Screenshots taken by atrium itself (what grim did over screencopy): a
// screen, a window, an area or the whole desktop as a PNG file, through the
// same copies the capture protocols make (capture.cpp).
#include "util/box.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace atrium {

class Server;

struct ScreenshotRequest {
    std::string output;              // a screen by name...
    std::optional<uint64_t> window;  // ...or a window by id...
    std::string identifier;          // ...or by its ext-foreign-toplevel identifier
    std::optional<Box> region;       // part of the desktop, layout coordinates
    std::optional<double> scale;     // pixels a logical pixel; default: the screens' (largest)
    std::string path;                // where the PNG goes
};

// `done` gets "" once the file is written, else why not.
void take_screenshot(Server& server, const ScreenshotRequest& request, std::function<void(const std::string&)> done);

} // namespace atrium
