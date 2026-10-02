#pragma once
// atrium's outputs: a screen of some backend (DRM, a nested window,
// headless), what can be set on it in one commit (OutputState), and the
// frame/present cycle. The field names follow wlroots' wlr_output, whose role
// this takes over.
#include "util/box.hpp"
#include "wl/signal.hpp"

extern "C" {
#include <drm_fourcc.h>
#include <pixman.h>
#include <wayland-server-core.h>
#define static
#include <wlr/render/color.h>
#undef static
#include <wlr/render/drm_format_set.h>
}

#include <memory>
#include <optional>
#include <string>
#include <vector>

struct wlr_buffer;
struct wlr_drm_syncobj_timeline;
struct wlr_renderer;

namespace atrium::backend {

class Allocator;
class Backend;
class Swapchain;

struct Mode {
    int width = 0, height = 0;
    int refresh = 0;  // mHz
    bool preferred = false;
    uint64_t native = 0;  // the backend's own handle on it
};

// What content the screen is told to expect (HDR10, say).
struct ImageDescription {
    wlr_color_transfer_function transfer_function = wlr_color_transfer_function(0);
    wlr_color_named_primaries primaries = wlr_color_named_primaries(0);
    wlr_color_primaries mastering_display_primaries{};
    struct {
        double min = 0, max = 0;
    } mastering_luminance;
    double max_cll = 0, max_fall = 0;
};

enum class AdaptiveSync { Disabled, Enabled };

// One commit's worth of changes; only the `committed` fields apply.
class OutputState {
public:
    enum Field : uint32_t {
        Buffer = 1 << 0,
        Damage = 1 << 1,
        ModeField = 1 << 2,
        Enabled = 1 << 3,
        Scale = 1 << 4,
        Transform = 1 << 5,
        AdaptiveSyncEnabled = 1 << 6,
        RenderFormat = 1 << 7,
        Subpixel = 1 << 8,
        WaitTimeline = 1 << 10,
        SignalTimeline = 1 << 11,
        ColorTransform = 1 << 12,
        ImageDescriptionField = 1 << 13,
    };
    enum class ModeType { Fixed, Custom };

    OutputState();
    ~OutputState();
    OutputState(const OutputState& other);
    OutputState& operator=(const OutputState& other);

    uint32_t committed = 0;
    pixman_region32_t damage;  // buffer-local
    bool enabled = false;
    float scale = 1;
    wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL;
    bool adaptive_sync_enabled = false;
    uint32_t render_format = 0;
    wl_output_subpixel subpixel = WL_OUTPUT_SUBPIXEL_UNKNOWN;
    wlr_buffer* buffer = nullptr;  // locked while held
    FBox buffer_src_box{};     // all zero: the whole buffer
    Box buffer_dst_box{};      // zero size: at its own size
    bool tearing_page_flip = false;
    // A real screen may go through a modeset for it (a flicker).
    bool allow_reconfiguration = false;
    ModeType mode_type = ModeType::Fixed;
    const Mode* mode = nullptr;
    struct {
        int32_t width = 0, height = 0, refresh = 0;
    } custom_mode;
    wlr_drm_syncobj_timeline* wait_timeline = nullptr;
    uint64_t wait_point = 0;
    wlr_drm_syncobj_timeline* signal_timeline = nullptr;
    uint64_t signal_point = 0;
    wlr_color_transform* color_transform = nullptr;  // referenced
    std::optional<ImageDescription> image_description;

    void set_enabled(bool on);
    void set_mode(const Mode* m);
    void set_custom_mode(int32_t width, int32_t height, int32_t refresh);
    void set_scale(float s);
    void set_transform(wl_output_transform t);
    void set_adaptive_sync_enabled(bool on);
    void set_render_format(uint32_t format);
    void set_subpixel(wl_output_subpixel s);
    void set_buffer(wlr_buffer* b);
    void set_damage(const pixman_region32_t* d);
    void set_wait_timeline(wlr_drm_syncobj_timeline* t, uint64_t point);
    void set_signal_timeline(wlr_drm_syncobj_timeline* t, uint64_t point);
    void set_color_transform(wlr_color_transform* t);
    void set_image_description(const ImageDescription* d);

private:
    void clear_buffer();
};

struct Present {
    uint32_t commit_seq = 0;
    bool presented = false;
    timespec when{};
    unsigned seq = 0;
    int refresh = 0;     // ns to the next refresh; 0 unknown
    uint32_t flags = 0;  // wp_presentation_feedback.kind
};

class Output {
public:
    virtual ~Output();
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    Backend& backend;
    std::string name, description, make, model, serial;
    int phys_width = 0, phys_height = 0;  // mm
    std::vector<Mode> modes;
    const Mode* current_mode = nullptr;
    int width = 0, height = 0, refresh = 0;  // buffer pixels, mHz
    bool enabled = false;
    float scale = 1;
    wl_output_subpixel subpixel = WL_OUTPUT_SUBPIXEL_UNKNOWN;
    wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL;
    AdaptiveSync adaptive_sync_status = AdaptiveSync::Disabled;
    bool adaptive_sync_supported = false;
    uint32_t render_format = DRM_FORMAT_XRGB8888;
    std::optional<ImageDescription> image_description;
    std::optional<wlr_color_primaries> default_primaries;  // the screen's own (EDID)
    uint32_t supported_primaries = 0;          // wlr_color_named_primaries bits
    uint32_t supported_transfer_functions = 0; // wlr_color_transfer_function bits
    bool non_desktop = false;

    bool frame_pending = false;
    bool needs_frame = false;
    uint32_t commit_seq = 0;

    wlr_renderer* renderer = nullptr;
    Allocator* allocator = nullptr;
    std::unique_ptr<Swapchain> swapchain;  // the primary plane's
    void* data = nullptr;            // the compositor's own

    struct {
        wl::Signal<> frame;
        wl::Signal<const pixman_region32_t*> damage;  // the backend lost what it showed there
        wl::Signal<> needs_frame;
        wl::Signal<const OutputState&> commit;
        wl::Signal<const Present&> present;
        wl::Signal<const OutputState&> request_state;  // the backend asks (a nested window resized)
        wl::Signal<> destroy;
    } events;

    // Whether the screen would take `state`; nothing changes.
    bool test_state(const OutputState& state);
    // Takes `state`; false (and nothing changed) if it wouldn't.
    bool commit_state(const OutputState& state);

    // A commit in steps, for backends committing several outputs at once:
    // `state` trimmed to what changes, checked, and given a blank buffer if a
    // modeset needs one; then, once the backend took it, the fields updated.
    bool prepare_commit(OutputState& state);
    void finish_commit(const OutputState& state);

    // A frame event soon: now if none is pending.
    void schedule_frame();
    // Something to show: the compositor should draw a frame.
    void update_needs_frame();
    // The frame event (backends call this at the right time).
    void send_frame();
    // The backend's present event.
    void send_present(Present p);

    const Mode* preferred_mode() const;
    void transformed_resolution(int* w, int* h) const;
    void effective_resolution(int* w, int* h) const;
    virtual size_t gamma_size() const { return 0; }
    // What the primary plane takes (null: anything the renderer makes).
    virtual const wlr_drm_format_set* primary_formats(uint32_t buffer_caps) const;
    // Only a buffer clients made can go straight to the screen.
    virtual bool direct_scanout_allowed() const { return true; }

    // Drawing: the renderer and allocator its frames come from.
    bool init_render(Allocator* allocator, wlr_renderer* renderer);
    // A swapchain fitting `state` (its size and format) in `swapchain`, kept
    // if the one there fits.
    bool configure_primary_swapchain(const OutputState* state, std::unique_ptr<Swapchain>& swapchain);
    // The resolution after `state` (its mode, else the current one).
    void pending_resolution(const OutputState& state, int* w, int* h) const;

    // A hardware cursor plane: the buffer (null: hidden) must be one of the
    // sizes and formats it takes. False when there is none (the scene draws
    // the cursor instead).
    virtual bool has_cursor_plane() const { return false; }
    virtual std::vector<std::pair<int, int>> cursor_sizes() const { return {}; }
    virtual const wlr_drm_format_set* cursor_formats(uint32_t /*buffer_caps*/) const { return nullptr; }
    virtual bool set_cursor(wlr_buffer* /*buffer*/, int /*hotspot_x*/, int /*hotspot_y*/) { return false; }
    virtual bool move_cursor(int /*x*/, int /*y*/) { return false; }

protected:
    explicit Output(Backend& backend);
    // Backend-specific checks and the commit itself. The backend that owns
    // an output emits events.destroy before deleting it.
    virtual bool test(const OutputState& state) = 0;
    virtual bool commit(const OutputState& state) = 0;
    // A frame now, or the backend's own way of scheduling one.
    virtual void schedule_frame_impl();
    // After a commit took: the fields follow the state.
    void apply(const OutputState& state);

private:
    // Fields of `state` that would change nothing.
    uint32_t unchanged(const OutputState& state) const;
    bool basic_test(const OutputState& state) const;
    // A blank buffer for a modeset that brought none (owned by the caller).
    bool ensure_buffer(OutputState& state, bool* added);
    bool pick_format(uint32_t format, std::vector<uint64_t>* modifiers) const;
    std::unique_ptr<Swapchain> create_swapchain(int w, int h, uint32_t format, bool allow_modifiers);

    wl_event_source* idle_frame_ = nullptr;
};

} // namespace atrium::backend
