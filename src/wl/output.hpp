#pragma once
#include "wayland-server.hpp"

#include <wayland-server-protocol.h>

#include <memory>
#include <string>
#include <vector>

namespace atrium::wl {

// Everything a client learns about a screen.
struct OutputInfo {
    std::string name, description, make, model;
    int physical_width = 0, physical_height = 0;  // millimetres
    int32_t subpixel = WL_OUTPUT_SUBPIXEL_UNKNOWN;
    int32_t transform = WL_OUTPUT_TRANSFORM_NORMAL;
    double scale = 1;  // the fractional scale; wl_output gets it rounded up
    int mode_width = 0, mode_height = 0;
    int refresh = 0;  // mHz
    int x = 0, y = 0;  // in the layout, logical
    int logical_width = 0, logical_height = 0;

    bool operator==(const OutputInfo&) const = default;
};

class OutputResource;

// One screen's wl_output global (and what xdg-output says of it). The
// compositor makes one per enabled screen and updates it as it changes;
// removing it leaves bound objects inert.
class Output {
public:
    Output(wl_display* display, const OutputInfo& info);
    ~Output();
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    const OutputInfo& info() const { return info_; }
    // Sends what changed, then done.
    void update(const OutputInfo& info);

    std::vector<WlOutput*> resources_for(wl_client* client) const;
    // The screen a wl_output names; null when it is gone.
    static Output* from(wl_resource* resource);
    static Output* from(WlOutput* resource);

    // The compositor's own screen object.
    void* data = nullptr;

private:
    friend class OutputResource;
    friend class XdgOutputs;
    void send_all(OutputResource* r) const;

    OutputInfo info_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<OutputResource>> resources_;
    std::vector<Weak<Resource>> xdg_outputs_;  // ZxdgOutputV1
};

class OutputResource : public WlOutput {
public:
    OutputResource(wl_client* client, uint32_t version, uint32_t id, Output* output);
    Output* output = nullptr;  // null once the screen is gone
};

// zxdg_output_manager_v1: logical positions and sizes, and names.
class XdgOutputs {
public:
    explicit XdgOutputs(wl_display* display);
    ~XdgOutputs();

private:
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
};

} // namespace atrium::wl
