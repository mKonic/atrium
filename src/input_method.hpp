#pragma once
#include "listener.hpp"

#include <memory>
#include <string>
#include <vector>

namespace atrium {

class Server;

// Input methods (fcitx5, ibus, ...): text-input-v3 in the apps, input-method-v2
// in the IME, and atrium passing text and state between them. While an app's
// text field is focused the IME may grab the keyboard: keys go to it first,
// and what it composes comes back to the app as preedit and commit strings.
// Its candidate popup is placed under the text cursor.
//
// Ported from dwl's ime.h.
class InputMethodRelay {
public:
    explicit InputMethodRelay(Server& server);
    ~InputMethodRelay();
    InputMethodRelay(const InputMethodRelay&) = delete;
    InputMethodRelay& operator=(const InputMethodRelay&) = delete;

    // A key or modifier change from `keyboard`: true when the IME took it.
    bool forward_key(wlr_keyboard* keyboard, bool is_virtual, const wlr_keyboard_key_event* event);
    bool forward_modifiers(wlr_keyboard* keyboard, bool is_virtual);
    // Type `text` into the focused text field, as an IME would: now, or as
    // soon as a field is active again (a picker had the keyboard). Nothing
    // within a moment: the shell hears "text.not_inserted".
    void insert_text(const std::string& text);

private:
    struct TextInput {
        explicit TextInput(wlr_text_input_v3* i) : input(i) {}
        wlr_text_input_v3* input;
        Listener<> enable, commit, disable, destroy;
    };
    struct Popup {
        Popup(wlr_input_popup_surface_v2* s, wlr_scene_tree* t) : surface(s), tree(t) {}
        wlr_input_popup_surface_v2* surface;
        wlr_scene_tree* tree;
        Listener<> commit, destroy;
    };

    void new_text_input(wlr_text_input_v3* input);
    void new_input_method(wlr_input_method_v2* im);
    void new_popup(wlr_input_popup_surface_v2* surface);
    void set_focus(wlr_surface* surface);
    void commit_pending();

    TextInput* find_active() const;
    void update_active();
    void update_focused_surfaces();
    void send_state();
    void place(Popup& popup);
    void place_popups();
    wlr_input_method_keyboard_grab_v2* grab_for(wlr_keyboard* keyboard, bool is_virtual) const;

    Server& server_;
    wlr_text_input_manager_v3* text_inputs_manager_;
    wlr_input_method_manager_v2* input_methods_manager_;
    wlr_input_method_v2* im_ = nullptr;
    wlr_surface* focused_ = nullptr;
    std::string pending_text_;
    wl_event_source* pending_timer_ = nullptr;
    TextInput* active_ = nullptr;
    std::vector<std::unique_ptr<TextInput>> text_inputs_;
    std::vector<std::unique_ptr<Popup>> popups_;

    Listener<wlr_text_input_v3> new_text_input_;
    Listener<wlr_input_method_v2> new_input_method_;
    Listener<> im_commit_, im_destroy_;
    Listener<wlr_input_method_keyboard_grab_v2> im_grab_;
    Listener<wlr_input_popup_surface_v2> im_new_popup_;
    Listener<> grab_destroy_;
    Listener<wlr_seat_keyboard_focus_change_event> focus_change_;
    Listener<> focused_destroy_;
};

} // namespace atrium
