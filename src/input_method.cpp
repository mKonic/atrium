// Ported from dwl's ime.h (Guido Cella, dwl team).
#include "input_method.hpp"

#include "layer_surface.hpp"
#include "output.hpp"
#include "server.hpp"
#include "seat.hpp"
#include "view.hpp"

#include <algorithm>

namespace atrium {

InputMethodRelay::InputMethodRelay(Server& server) : server_(server) {
    text_inputs_manager_ = wlr_text_input_manager_v3_create(server.display);
    input_methods_manager_ = wlr_input_method_manager_v2_create(server.display);
    new_text_input_.connect(&text_inputs_manager_->events.new_text_input,
        [this](wlr_text_input_v3* t) { new_text_input(t); });
    new_input_method_.connect(&input_methods_manager_->events.new_input_method,
        [this](wlr_input_method_v2* im) { new_input_method(im); });
    // The IME follows the keyboard, wherever focus moves and however.
    focus_change_.connect(&server.seat->wlr->keyboard_state.events.focus_change,
        [this](wlr_seat_keyboard_focus_change_event* e) { set_focus(e->new_surface); });
}

InputMethodRelay::~InputMethodRelay() {
    // The managers go with the display; nothing may stay hooked to them.
    popups_.clear();
    text_inputs_.clear();
    new_text_input_.disconnect();
    new_input_method_.disconnect();
    im_commit_.disconnect();
    im_destroy_.disconnect();
    im_grab_.disconnect();
    im_new_popup_.disconnect();
    grab_destroy_.disconnect();
    focus_change_.disconnect();
    focused_destroy_.disconnect();
}

// --- keys --------------------------------------------------------------------------

// Only the physical keyboard is grabbed: the IME types back through a
// virtual keyboard of its own, which must reach the app, not loop.
wlr_input_method_keyboard_grab_v2* InputMethodRelay::grab_for(wlr_keyboard*, bool is_virtual) const {
    if (!im_ || !im_->keyboard_grab || is_virtual)
        return nullptr;
    return im_->keyboard_grab;
}

bool InputMethodRelay::forward_key(wlr_keyboard* keyboard, bool is_virtual, const wlr_keyboard_key_event* e) {
    wlr_input_method_keyboard_grab_v2* grab = grab_for(keyboard, is_virtual);
    if (!grab)
        return false;
    wlr_input_method_keyboard_grab_v2_set_keyboard(grab, keyboard);
    wlr_input_method_keyboard_grab_v2_send_key(grab, e->time_msec, e->keycode, e->state);
    return true;
}

bool InputMethodRelay::forward_modifiers(wlr_keyboard* keyboard, bool is_virtual) {
    wlr_input_method_keyboard_grab_v2* grab = grab_for(keyboard, is_virtual);
    if (!grab)
        return false;
    wlr_input_method_keyboard_grab_v2_set_keyboard(grab, keyboard);
    wlr_input_method_keyboard_grab_v2_send_modifiers(grab, &keyboard->modifiers);
    return true;
}

// --- text inputs (apps) ------------------------------------------------------------

InputMethodRelay::TextInput* InputMethodRelay::find_active() const {
    if (!im_)
        return nullptr;
    for (const auto& t : text_inputs_)
        if (t->input->focused_surface && t->input->current_enabled)
            return t.get();
    return nullptr;
}

void InputMethodRelay::update_active() {
    TextInput* now = find_active();
    if (im_ && now != active_) {
        if (now)
            wlr_input_method_v2_send_activate(im_);
        else
            wlr_input_method_v2_send_deactivate(im_);
        wlr_input_method_v2_send_done(im_);
    }
    active_ = now;
}

// Enter the focused surface on the text inputs of its client, leave elsewhere.
void InputMethodRelay::update_focused_surfaces() {
    for (const auto& t : text_inputs_) {
        wlr_text_input_v3* input = t->input;
        wlr_surface* target = nullptr;
        if (im_ && focused_ && wl_resource_get_client(input->resource) == wl_resource_get_client(focused_->resource))
            target = focused_;
        if (input->focused_surface == target)
            continue;
        if (input->focused_surface)
            wlr_text_input_v3_send_leave(input);
        if (target)
            wlr_text_input_v3_send_enter(input, target);
    }
}

void InputMethodRelay::send_state() {
    wlr_text_input_v3* input = active_->input;
    if (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT)
        wlr_input_method_v2_send_surrounding_text(im_, input->current.surrounding.text,
                                                  input->current.surrounding.cursor,
                                                  input->current.surrounding.anchor);
    wlr_input_method_v2_send_text_change_cause(im_, input->current.text_change_cause);
    if (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE)
        wlr_input_method_v2_send_content_type(im_, input->current.content_type.hint,
                                              input->current.content_type.purpose);
    wlr_input_method_v2_send_done(im_);
}

void InputMethodRelay::new_text_input(wlr_text_input_v3* input) {
    if (input->seat != server_.seat->wlr)
        return;
    auto t = std::make_unique<TextInput>(input);
    TextInput* tp = t.get();
    tp->enable.connect(&input->events.enable, [this, tp](void*) {
        update_active();
        if (active_ == tp) {
            place_popups();
            send_state();
        }
        wlr_text_input_v3_send_done(tp->input);
    });
    tp->disable.connect(&input->events.disable, [this](void*) { update_active(); });
    tp->commit.connect(&input->events.commit, [this, tp](void*) {
        if (active_ == tp) {
            place_popups();
            send_state();
        }
    });
    tp->destroy.connect(&input->events.destroy, [this, tp](void*) {
        if (active_ == tp)
            active_ = nullptr;
        std::erase_if(text_inputs_, [tp](const auto& p) { return p.get() == tp; });
        update_active();
    });
    text_inputs_.push_back(std::move(t));
    update_focused_surfaces();
}

void InputMethodRelay::set_focus(wlr_surface* surface) {
    if (focused_ == surface)
        return;
    focused_destroy_.disconnect();
    focused_ = surface;
    if (surface)
        focused_destroy_.connect(&surface->events.destroy, [this](void*) { set_focus(nullptr); });
    update_focused_surfaces();
    update_active();
}

// --- the input method ------------------------------------------------------------

void InputMethodRelay::new_input_method(wlr_input_method_v2* im) {
    if (im->seat != server_.seat->wlr)
        return;
    if (im_) {  // one at a time
        wlr_input_method_v2_send_unavailable(im);
        return;
    }
    im_ = im;

    // What the IME composed goes to the app.
    im_commit_.connect(&im->events.commit, [this](void*) {
        if (!active_)
            return;
        wlr_text_input_v3* input = active_->input;
        if (im_->current.preedit.text)
            wlr_text_input_v3_send_preedit_string(input, im_->current.preedit.text,
                                                  im_->current.preedit.cursor_begin,
                                                  im_->current.preedit.cursor_end);
        if (im_->current.commit_text)
            wlr_text_input_v3_send_commit_string(input, im_->current.commit_text);
        if (im_->current.delete_.before_length || im_->current.delete_.after_length)
            wlr_text_input_v3_send_delete_surrounding_text(input, im_->current.delete_.before_length,
                                                           im_->current.delete_.after_length);
        wlr_text_input_v3_send_done(input);
    });

    im_grab_.connect(&im->events.grab_keyboard, [this](wlr_input_method_keyboard_grab_v2* grab) {
        // Start it off with the modifiers held right now.
        if (wlr_keyboard* kb = server_.seat->physical_keyboard())
            wlr_input_method_keyboard_grab_v2_set_keyboard(grab, kb);
        grab_destroy_.connect(&grab->events.destroy, [this, grab](void*) {
            grab_destroy_.disconnect();
            // The app gets the modifier state back.
            if (grab->keyboard)
                wlr_seat_keyboard_notify_modifiers(server_.seat->wlr, &grab->keyboard->modifiers);
        });
    });

    im_new_popup_.connect(&im->events.new_popup_surface,
        [this](wlr_input_popup_surface_v2* s) { new_popup(s); });

    im_destroy_.connect(&im->events.destroy, [this](void*) {
        im_commit_.disconnect();
        im_grab_.disconnect();
        im_new_popup_.disconnect();
        im_destroy_.disconnect();
        grab_destroy_.disconnect();
        im_ = nullptr;
        update_focused_surfaces();
        update_active();
    });

    update_focused_surfaces();
    update_active();
}

// --- candidate popups ------------------------------------------------------------

void InputMethodRelay::new_popup(wlr_input_popup_surface_v2* surface) {
    auto p = std::make_unique<Popup>(surface, wlr_scene_tree_create(server_.layer(Layer::InputPopup)));
    Popup* pp = p.get();
    wlr_scene_subsurface_tree_create(pp->tree, surface->surface);
    pp->commit.connect(&surface->surface->events.commit, [this, pp](void*) { place(*pp); });
    pp->destroy.connect(&surface->events.destroy, [this, pp](void*) {
        wlr_scene_node_destroy(&pp->tree->node);
        std::erase_if(popups_, [pp](const auto& q) { return q.get() == pp; });
    });
    popups_.push_back(std::move(p));
    place(*pp);
}

void InputMethodRelay::place_popups() {
    for (const auto& p : popups_)
        place(*p);
}

// Under the text cursor, flipped above it or slid sideways to stay on screen.
void InputMethodRelay::place(Popup& popup) {
    if (!active_ || !focused_ || !popup.surface->surface->mapped)
        return;

    wlr_box cursor{};
    if (active_->input->current.features & WLR_TEXT_INPUT_V3_FEATURE_CURSOR_RECTANGLE) {
        double ox = 0, oy = 0;
        bool known = true;
        const Owner owner = Server::owner_of(focused_);
        if (owner.view) {
            owner.view->surface_origin(ox, oy);
        } else if (owner.layer && owner.layer->scene_layer) {
            int lx = 0, ly = 0;
            wlr_scene_node_coords(&owner.layer->scene_layer->tree->node, &lx, &ly);
            ox = lx;
            oy = ly;
        } else {
            known = false;
        }
        if (known) {
            cursor = active_->input->current.cursor_rectangle;
            cursor.x += int(ox);
            cursor.y += int(oy);
        }
    }

    double cx = 0, cy = 0;
    wlr_output_layout_closest_point(server_.output_layout, nullptr, cursor.x, cursor.y, &cx, &cy);
    Output* output = server_.output_at(cx, cy);
    if (!output || !output->enabled())
        return;

    wlr_xdg_positioner_rules rules{};
    rules.anchor_rect = cursor;
    rules.anchor = XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
    rules.gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
    rules.size = {popup.surface->surface->current.width, popup.surface->surface->current.height};
    rules.constraint_adjustment = static_cast<xdg_positioner_constraint_adjustment>(
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X);
    wlr_box box{};
    wlr_xdg_positioner_rules_get_geometry(&rules, &box);
    wlr_xdg_positioner_rules_unconstrain_box(&rules, &output->box, &box);

    wlr_scene_node_set_position(&popup.tree->node, box.x, box.y);
    wlr_scene_node_raise_to_top(&popup.tree->node);
    wlr_box relative{cursor.x - box.x, cursor.y - box.y, cursor.width, cursor.height};
    wlr_input_popup_surface_v2_send_text_input_rectangle(popup.surface, &relative);
}

} // namespace atrium
