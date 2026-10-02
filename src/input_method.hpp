#pragma once
#include "listener.hpp"
#include "scene/scene.hpp"
#include "input/keys.hpp"
#include "wl/ime.hpp"

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
    bool forward_key(const input::Keys& keys, wl_client* virtual_owner, uint32_t time_ms, uint32_t keycode,
                     bool pressed);
    bool forward_modifiers(const input::Keys& keys, wl_client* virtual_owner);
    // Type `text` into the focused text field, as an IME would: now, or as
    // soon as a field is active again (a picker had the keyboard). Nothing
    // within a moment: the shell hears "text.not_inserted".
    void insert_text(const std::string& text);
    // Take `before` bytes back from before the cursor and put `text` there,
    // in the focused field (a snippet keyword). False when no field takes text.
    bool replace_text(size_t before, const std::string& text);
    // A focused field takes text right now.
    bool takes_text_now() const { return takes_text(active_); }

private:
    using TextInput = wl::TextInputs::TextInput;
    struct Popup {
        Popup(wl::InputMethods::Popup* p, scene::Tree* t) : popup(p), tree(t) {}
        wl::InputMethods::Popup* popup;
        scene::Tree* tree;
        wl::Connection commit;
    };

    void set_focus(wl::Surface* surface);
    void commit_pending();
    bool takes_text(const TextInput* t) const;
    // Sends the IME the keymap of the keyboard it is about to hear.
    bool grab_for(const input::Keys& keys, wl_client* virtual_owner);

    TextInput* find_active() const;
    void update_active();
    void send_state();
    void place(Popup& popup);
    void place_popups();

    Server& server_;
    wl::TextInputs& text_inputs_;
    wl::InputMethods& methods_;
    wl::Surface* focused_ = nullptr;
    std::string pending_text_;
    wl_event_source* pending_timer_ = nullptr;
    wl_event_source* settle_timer_ = nullptr;  // the field's commits have settled
    static constexpr int kSettleMs = 40;
    TextInput* active_ = nullptr;
    // Enabled since it last entered a surface: text sent before the app
    // enables again is dropped.
    std::vector<TextInput*> ready_;
    std::vector<std::unique_ptr<Popup>> popups_;
    std::string grab_keymap_;
    std::vector<wl::Connection> connections_;
};

} // namespace atrium
