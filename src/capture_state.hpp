#pragma once
// What waits for capture frames (capture.cpp), per screen and per window.
#include "render/renderer.hpp"
#include "server.hpp"

#include <unordered_map>
#include <vector>

namespace atrium {

struct Server::CaptureState {
    struct Pending {
        wl::Capture::Copy copy;
        Box box;  // what of the frame, in its buffer's pixels
    };
    struct PerOutput {
        std::vector<Pending> copies;
        std::vector<wl::Capture::Export> exports;
        wl::Connection commit;
        // What the pointer (drawn in software) covered in this frame, for
        // copies without it.
        render::Target under;
        Box under_box{};
        bool under_valid = false;
        bool pointer_kept = false;  // the last such frame couldn't leave it out
        PerOutput() = default;
        PerOutput(const PerOutput&) = delete;
        ~PerOutput() {
            for (auto& p : copies)
                buffer_unlock(p.copy.buffer);
        }
    };
    struct PerView {
        scene::CaptureSource* source = nullptr;
        bool running = false;
        std::vector<wl::Capture::Copy> copies;
        wl_event_source* idle = nullptr;  // stops drawing once nobody asked for a while
        PerView() = default;
        PerView(const PerView&) = delete;
        ~PerView() {
            if (idle)
                wl_event_source_remove(idle);
            for (auto& c : copies)
                buffer_unlock(c.buffer);
        }
    };
    std::unordered_map<Output*, PerOutput> outputs;
    std::unordered_map<View*, PerView> views;
};

} // namespace atrium
