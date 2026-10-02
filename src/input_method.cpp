// Ported from dwl's ime.h (Guido Cella, dwl team).
#include "input_method.hpp"

#include "layer_surface.hpp"
#include "output.hpp"
#include "ipc.hpp"
#include "server.hpp"
#include "surface_blur.hpp"
#include "seat.hpp"
#include "view.hpp"

#include <algorithm>

namespace atrium {

InputMethodRelay::InputMethodRelay(Server& server)
    : server_(server), text_inputs_(*server.wl->text_inputs), methods_(*server.wl->input_methods) {
    pending_timer_ = wl_event_loop_add_timer(server.loop, [](void* data) {
        // No field took it: the shell copies it instead.
        auto* self = static_cast<InputMethodRelay*>(data);
        if (!self->pending_text_.empty() && self->server_.ipc)
            self->server_.ipc->broadcast("shell", {{"event", "text.not_inserted"},
                                                   {"text", std::exchange(self->pending_text_, {})}});
        return 0;
    }, this);
    settle_timer_ = wl_event_loop_add_timer(server.loop, [](void* data) {
        auto* self = static_cast<InputMethodRelay*>(data);
        if (self->takes_text(self->active_) && !self->pending_text_.empty())
            self->commit_pending();
        return 0;
    }, this);
    auto& c = connections_;
    auto& ti = text_inputs_.events;
    c.push_back(ti.enable.connect([this](TextInput* t) {
        if (std::ranges::find(ready_, t) == ready_.end())
            ready_.push_back(t);
        update_active();
        if (active_ == t) {
            place_popups();
            send_state();
        }
        text_inputs_.send_done(t);
    }));
    c.push_back(ti.disable.connect([this](TextInput* t) {
        std::erase(ready_, t);
        update_active();
    }));
    c.push_back(ti.commit.connect([this](TextInput* t) {
        if (active_ == t) {
            place_popups();
            send_state();
            if (!pending_text_.empty() && takes_text(t))
                wl_event_source_timer_update(settle_timer_, kSettleMs);
        }
    }));
    c.push_back(ti.destroy.connect([this](TextInput* t) {
        std::erase(ready_, t);
        if (active_ == t)
            active_ = nullptr;
        update_active();
    }));

    auto& im = methods_.events;
    c.push_back(im.new_method.connect([this](wl::InputMethods::InputMethod*) {
        grab_keymap_.clear();
        // A field was already active (atrium focuses fields without an IME
        // too): this IME hasn't heard of it yet.
        if (active_)
            methods_.activate(active_->current);
    }));
    // What the IME composed goes to the app.
    c.push_back(im.commit.connect([this](wl::InputMethods::InputMethod* m) {
        if (!active_)
            return;
        const auto& cur = m->current;
        if (cur.preedit)
            text_inputs_.send_preedit(active_, cur.preedit->c_str(), cur.preedit_begin, cur.preedit_end);
        if (cur.commit)
            text_inputs_.send_commit(active_, cur.commit->c_str());
        if (cur.delete_before || cur.delete_after)
            text_inputs_.send_delete(active_, cur.delete_before, cur.delete_after);
        text_inputs_.send_done(active_);
    }));
    c.push_back(im.grab.connect([this](bool taken) {
        grab_keymap_.clear();
        wlr_keyboard* kb = server_.seat->physical_keyboard();
        if (taken) {
            // Start it off with the keymap and modifiers in use right now.
            grab_for(kb, nullptr);
            methods_.grab_modifiers({kb->modifiers.depressed, kb->modifiers.latched, kb->modifiers.locked,
                                     kb->modifiers.group});
        } else {
            // The app gets the modifier state back.
            server_.wl->seat->keyboard_modifiers({kb->modifiers.depressed, kb->modifiers.latched,
                                                  kb->modifiers.locked, kb->modifiers.group});
        }
    }));
    c.push_back(im.new_popup.connect([this](wl::InputMethods::Popup* p) {
        auto popup = std::make_unique<Popup>(p, scene::Tree::create(server_.layer(Layer::InputPopup)));
        Popup* pp = popup.get();
        scene::subsurface_tree_create(pp->tree, p->surface);
        attach_surface_blur(server_, pp->tree, p->surface);
        pp->commit = p->surface->events.commit.connect([this, pp] { place(*pp); });
        popups_.push_back(std::move(popup));
        place(*pp);
    }));
    c.push_back(im.destroy_popup.connect([this](wl::InputMethods::Popup* p) {
        std::erase_if(popups_, [p](const auto& q) {
            if (q->popup != p)
                return false;
            q->tree->destroy();
            return true;
        });
    }));
    c.push_back(im.destroy.connect([this](wl::InputMethods::InputMethod*) {
        grab_keymap_.clear();
        for (auto& p : popups_)
            p->tree->destroy();
        popups_.clear();
    }));
    // The IME follows the keyboard, wherever focus moves and however.
    c.push_back(server.wl->seat->events.keyboard_focus.connect([this](wl::Surface* s) { set_focus(s); }));
}

InputMethodRelay::~InputMethodRelay() {
    if (pending_timer_)
        wl_event_source_remove(pending_timer_);
    if (settle_timer_)
        wl_event_source_remove(settle_timer_);
    connections_.clear();
    for (auto& p : popups_)
        p->tree->destroy();
    popups_.clear();
}

// --- keys --------------------------------------------------------------------------

// Every keyboard is grabbed but the IME's own: it types back through a
// virtual keyboard of its own, which must reach the app, not loop. Another
// client's (an on-screen keyboard, wtype) is typing like a real one.
bool InputMethodRelay::grab_for(wlr_keyboard* keyboard, wl_client* virtual_owner) {
    const auto* m = methods_.current();
    if (!m || !methods_.grabbed())
        return false;
    if (virtual_owner && m->resource && virtual_owner == m->resource->client())
        return false;
    if (keyboard && keyboard->keymap) {
        char* text = xkb_keymap_get_as_string(keyboard->keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
        if (text && grab_keymap_ != text) {
            grab_keymap_ = text;
            methods_.grab_keymap(grab_keymap_);
        }
        free(text);
    }
    return true;
}

bool InputMethodRelay::forward_key(wlr_keyboard* keyboard, wl_client* virtual_owner, const wlr_keyboard_key_event* e) {
    if (!grab_for(keyboard, virtual_owner))
        return false;
    methods_.grab_key(e->time_msec, e->keycode, e->state == WL_KEYBOARD_KEY_STATE_PRESSED);
    return true;
}

bool InputMethodRelay::forward_modifiers(wlr_keyboard* keyboard, wl_client* virtual_owner) {
    if (!grab_for(keyboard, virtual_owner))
        return false;
    const wlr_keyboard_modifiers& m = keyboard->modifiers;
    methods_.grab_modifiers({m.depressed, m.latched, m.locked, m.group});
    return true;
}

// --- text inputs (apps) ------------------------------------------------------------

InputMethodRelay::TextInput* InputMethodRelay::find_active() const {
    for (TextInput* t : ready_)
        if (t->focus && t->current.enabled)
            return t;
    return nullptr;
}

void InputMethodRelay::update_active() {
    TextInput* now = find_active();
    if (methods_.current() && now != active_) {
        if (now)
            methods_.activate(now->current);
        else
            methods_.deactivate();
    }
    active_ = now;
    // Text waiting for a field (the emoji picker's): the field is back.
    // Given once it holds still: an app just enabled sends more commits
    // (cursor, content type), and a done answering an older one is dropped.
    if (takes_text(active_) && !pending_text_.empty())
        wl_event_source_timer_update(settle_timer_, kSettleMs);
}

// A window's field. The shell's own (the picker's search) is where the
// request came from, going away as it arrives: never its target.
bool InputMethodRelay::takes_text(const TextInput* t) const {
    return t && t->focus && !wl::LayerSurface::from(t->focus);
}

void InputMethodRelay::send_state() {
    if (methods_.current() && active_)
        methods_.send_state(active_->current);
}

void InputMethodRelay::insert_text(const std::string& text) {
    pending_text_ = text;
    if (takes_text(active_)) {
        commit_pending();
        return;
    }
    // The picker still has the keyboard: the field comes back when it goes.
    wl_event_source_timer_update(pending_timer_, 1500);
}

bool InputMethodRelay::replace_text(size_t before, const std::string& text) {
    if (!takes_text(active_))
        return false;
    text_inputs_.send_delete(active_, uint32_t(before), 0);
    text_inputs_.send_commit(active_, text.c_str());
    text_inputs_.send_done(active_);
    return true;
}

void InputMethodRelay::commit_pending() {
    text_inputs_.send_commit(active_, pending_text_.c_str());
    text_inputs_.send_done(active_);
    pending_text_.clear();
    wl_event_source_timer_update(pending_timer_, 0);
    wl_event_source_timer_update(settle_timer_, 0);
}

void InputMethodRelay::set_focus(wl::Surface* surface) {
    if (focused_ == surface)
        return;
    focused_ = surface;
    // Also without an IME: atrium itself types into fields (emoji).
    ready_.clear();
    text_inputs_.focus(surface);
    update_active();
}

// --- candidate popups ------------------------------------------------------------

void InputMethodRelay::place_popups() {
    for (const auto& p : popups_)
        place(*p);
}

// Under the text cursor, flipped above it or slid sideways to stay on screen.
void InputMethodRelay::place(Popup& popup) {
    wl::Surface* s = popup.popup->surface;
    if (!active_ || !focused_ || !s || !s->mapped())
        return;

    wlr_box cursor{};
    if (active_->current.cursor_rect) {
        double ox = 0, oy = 0;
        bool known = true;
        const Owner owner = Server::owner_of(focused_);
        if (owner.view) {
            owner.view->surface_origin(ox, oy);
        } else if (owner.layer && owner.layer->scene_layer) {
            int lx = 0, ly = 0;
            owner.layer->scene_layer->tree->coords(&lx, &ly);
            ox = lx;
            oy = ly;
        } else {
            known = false;
        }
        if (known) {
            const wl::Box& r = *active_->current.cursor_rect;
            cursor = {r.x + int(ox), r.y + int(oy), r.width, r.height};
        }
    }

    double cx = 0, cy = 0;
    wlr_output_layout_closest_point(server_.output_layout, nullptr, cursor.x, cursor.y, &cx, &cy);
    Output* output = server_.output_at(cx, cy);
    if (!output || !output->enabled())
        return;

    wl::PositionerRules rules;
    rules.anchor_rect = {cursor.x, cursor.y, std::max(1, cursor.width), std::max(1, cursor.height)};
    rules.anchor = wl::PositionerRules::BottomLeft;
    rules.gravity = wl::PositionerRules::BottomRight;
    rules.width = s->current().width;
    rules.height = s->current().height;
    rules.constraint_adjustment = wl::PositionerRules::FlipY | wl::PositionerRules::SlideX;
    wl::Box box = rules.geometry();
    rules.unconstrain({output->box.x, output->box.y, output->box.width, output->box.height}, box);

    popup.tree->set_position(box.x, box.y);
    popup.tree->raise_to_top();
    methods_.set_popup_rectangle(popup.popup, {cursor.x - box.x, cursor.y - box.y, cursor.width, cursor.height});
}

} // namespace atrium
