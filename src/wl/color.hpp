#pragma once
#include "wl/compositor.hpp"

#include <map>

namespace atrium::wl {

class Output;

// wp_color_manager_v1: apps say what colour space their content is in (an
// HDR video, a game's HDR10 swapchain) and hear what each screen, and each of
// their surfaces, would like. The surface's description is part of its
// committed state (SurfaceState::image_description).
class ColorManagement {
public:
    // What atrium can take: wp_color_manager_v1's enum values.
    struct Options {
        std::vector<uint32_t> intents, features, transfer_functions, primaries;
    };

    ColorManagement(wl_display* display, Options options);
    ~ColorManagement();

    // A screen's colour space (HDR on, say): its watchers hear it changed.
    void set_output_description(Output* output, const ImageDescription& d);
    // What `surface` should render in (its screen's): its feedback hears it.
    void set_preferred(Surface* surface, const ImageDescription& d);

    // One number per distinct description, the same for equal ones.
    uint64_t identity_of(const ImageDescription& d);

private:
    struct DescriptionResource;
    bool supports(const std::vector<uint32_t>& list, uint32_t v) const {
        return std::ranges::find(list, v) != list.end();
    }
    void make_description(wl_client* client, uint32_t version, uint32_t id,
                          std::shared_ptr<const ImageDescription> d, bool with_information);

    Options options_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
    std::map<Output*, ImageDescription> outputs_;
    std::map<Surface*, ImageDescription> preferred_;
    struct Watch {
        Output* output;
        Weak<Resource> resource;
    };
    std::vector<Watch> output_watches_;
    struct Feedback {
        Surface* surface;
        Weak<Resource> resource;
    };
    std::vector<Feedback> feedbacks_;
    std::map<Surface*, Weak<Resource>> surfaces_;
    std::vector<std::pair<ImageDescription, uint64_t>> identities_;
};

} // namespace atrium::wl
