#pragma once
#include "wl/positioner.hpp"
#include "wl/seat.hpp"

#include <optional>
#include <string>

namespace atrium::wl {

class Output;

// zwp_text_input_manager_v3: a text field in an app talks to the input
// method (fcitx5) through the compositor. State is double-buffered; the
// compositor relays it to the input method and sends back what it types.
class TextInputs {
public:
    struct State {
        bool enabled = false;
        std::optional<std::string> surrounding;
        uint32_t cursor = 0, anchor = 0;
        uint32_t change_cause = 0;
        uint32_t content_hint = 0, content_purpose = 0;
        std::optional<Box> cursor_rect;  // surface coordinates
    };
    struct TextInput {
        Seat* seat;
        wl_client* client;
        Surface* focus = nullptr;  // entered
        State pending, current;
        uint32_t commits = 0;  // the serial `done` answers with
        Weak<Resource> resource;
        Connection focus_gone;
    };

    TextInputs(wl_display* display, Seat& seat);
    ~TextInputs();

    // The seat's keyboard focus moved: text inputs of its client enter it.
    void focus(Surface* surface);
    // Sends what the input method produced, then done.
    void send_preedit(TextInput* t, const char* text, int32_t begin, int32_t end);
    void send_commit(TextInput* t, const char* text);
    void send_delete(TextInput* t, uint32_t before, uint32_t after);
    void send_done(TextInput* t);
    const std::vector<std::unique_ptr<TextInput>>& inputs() const { return inputs_; }

    struct {
        Signal<TextInput*> enable, commit, disable, destroy;
    } events;

private:
    void drop(TextInput* t);
    Seat& seat_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
    std::vector<std::unique_ptr<TextInput>> inputs_;
};

// zwp_input_method_manager_v2: the input method itself (fcitx5), one per
// seat, with its keyboard grab and candidate popups.
class InputMethods {
public:
    struct Pending {
        std::optional<std::string> commit;
        std::optional<std::string> preedit;
        int32_t preedit_begin = 0, preedit_end = 0;
        uint32_t delete_before = 0, delete_after = 0;
    };
    struct Popup {
        Surface* surface;
        Weak<Resource> resource;
        Connection surface_gone;
    };
    struct InputMethod {
        Seat* seat;
        Pending pending, current;
        uint32_t serial = 0;  // how many `done`s it has been sent
        bool active = false;
        Weak<Resource> resource, grab;
        std::vector<std::unique_ptr<Popup>> popups;
    };

    InputMethods(wl_display* display, Seat& seat);
    ~InputMethods();

    InputMethod* current() const { return method_.get(); }
    void activate(const TextInputs::State& state);
    void deactivate();
    void send_state(const TextInputs::State& state);
    void send_done();
    // Keys and modifiers for the grab, if the input method holds one.
    bool grabbed() const;
    void grab_key(uint32_t time_ms, uint32_t key, bool pressed);
    void grab_modifiers(const Seat::Modifiers& mods);
    void grab_keymap(const std::string& keymap);
    void set_popup_rectangle(Popup* p, const Box& box);

    struct {
        Signal<InputMethod*> new_method, commit, destroy;
        Signal<Popup*> new_popup, destroy_popup;
        Signal<bool> grab;  // taken (true) or released
    } events;

private:
    void drop_method();
    Seat& seat_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
    std::unique_ptr<InputMethod> method_;
    std::string keymap_;
};

// zwp_virtual_keyboard_manager_v1 and zwlr_virtual_pointer_manager_v1:
// input made by programs (on-screen keyboards, remote desktops, wtype).
class VirtualInputs {
public:
    struct Keyboard {
        std::string keymap;
        Weak<Resource> resource;
        Signal<uint32_t, uint32_t, bool> key;  // time, key, pressed
        Signal<const Seat::Modifiers&> modifiers;
        Signal<> keymap_changed, destroy;
    };
    struct Pointer {
        Output* output;  // the one it was made for, if any
        Weak<Resource> resource;
        Signal<uint32_t, double, double> motion;  // time, dx, dy
        Signal<uint32_t, double, double> motion_absolute;  // time, x, y (0..1)
        Signal<uint32_t, uint32_t, bool> button;
        Signal<uint32_t, uint32_t, double, int32_t> axis;  // time, axis, value, discrete (0: none)
        Signal<uint32_t> axis_source;
        Signal<uint32_t, uint32_t> axis_stop;
        Signal<> frame, destroy;
    };

    VirtualInputs(wl_display* display, Seat& seat);
    ~VirtualInputs();

    Signal<Keyboard*> new_keyboard;
    Signal<Pointer*> new_pointer;

private:
    Seat& seat_;
    std::unique_ptr<Global> keyboard_global_, pointer_global_;
    std::vector<Weak<Resource>> managers_;
    std::vector<std::unique_ptr<Keyboard>> keyboards_;
    std::vector<std::unique_ptr<Pointer>> pointers_;
};

} // namespace atrium::wl
