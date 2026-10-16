#pragma once
// text-input-unstable-v1, the one Chromium and Electron speak to input
// methods with (--enable-wayland-ime) unless told to use v3. wlroots has
// no helper; Hyprland serves it (protocols/TextInputV1.cpp, managers/
// input/TextInput.cpp) and so does atrium, through the same relay as v3
// (InputMethodRelay): the IME (input-method-v2) can't tell them apart.

#include <wayland-server-core.h>

#include <functional>
#include <string>
#include <vector>

struct wlr_surface;
struct wlr_box;

namespace atrium {

class TextInputsV1;

// One app's text field.
class TextInputV1 {
public:
    TextInputsV1& owner;
    wl_resource* resource;

    // What the app last committed (commit_state).
    std::string surrounding;
    uint32_t cursor = 0, anchor = 0;
    bool has_surrounding = false;
    uint32_t hint = 0, purpose = 0;  // as text-input-v3 numbers them
    int cx = 0, cy = 0, cw = 0, ch = 0;  // cursor rectangle, surface coordinates
    bool has_cursor_rect = false;
    uint32_t serial = 0;

    // The surface the app asked to type into (activate), and whether it
    // has been entered there (it has the keyboard).
    wlr_surface* asked = nullptr;
    wlr_surface* entered = nullptr;

    wl_client* client() const { return wl_resource_get_client(resource); }
    void enter(wlr_surface* surface);
    void leave();
    // What the IME composed, in v1's terms: deletion (around the cursor) and
    // commit, then the preedit (cursor_begin as its cursor).
    void send(const char* preedit, int32_t preedit_cursor, const char* commit, uint32_t delete_before,
              uint32_t delete_after);

    TextInputV1(TextInputsV1& o, wl_resource* r) : owner(o), resource(r) {}
    ~TextInputV1();

    // The surface the app asked for (activate), or none (deactivate).
    void set_asked(wlr_surface* surface);

private:
    wl_listener asked_gone_{};
};

class TextInputsV1 {
public:
    explicit TextInputsV1(wl_display* display);
    ~TextInputsV1();
    TextInputsV1(const TextInputsV1&) = delete;
    TextInputsV1& operator=(const TextInputsV1&) = delete;

    std::vector<TextInputV1*> inputs;

    // The relay hears these.
    std::function<void(TextInputV1*)> on_new, on_activate, on_deactivate, on_commit, on_destroy;

private:
    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    wl_global* global_ = nullptr;
    std::vector<wl_resource*> managers_;
};

} // namespace atrium
