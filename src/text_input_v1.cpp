#include "text_input_v1.hpp"

#include "wlr.hpp"

#include "text-input-unstable-v1-protocol.h"

#include <algorithm>
#include <cstring>

namespace atrium {

namespace {

TextInputV1* from(wl_resource* r) {
    return static_cast<TextInputV1*>(wl_resource_get_user_data(r));
}

// v1's purposes after "password" are one lower than v3's (v3 put "pin" there).
uint32_t purpose_to_v3(uint32_t purpose) {
    return purpose >= ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_DATE ? purpose + 1 : purpose;
}

const struct zwp_text_input_v1_interface input_impl = {
    .activate =
        [](wl_client*, wl_resource* r, wl_resource*, wl_resource* surface) {
            TextInputV1* t = from(r);
            if (!t || !surface)
                return;
            t->set_asked(wlr_surface_from_resource(surface));
            if (t->owner.on_activate)
                t->owner.on_activate(t);
        },
    .deactivate =
        [](wl_client*, wl_resource* r, wl_resource*) {
            TextInputV1* t = from(r);
            if (!t)
                return;
            t->set_asked(nullptr);
            if (t->owner.on_deactivate)
                t->owner.on_deactivate(t);
        },
    .show_input_panel = [](wl_client*, wl_resource*) {},
    .hide_input_panel = [](wl_client*, wl_resource*) {},
    .reset =
        [](wl_client*, wl_resource* r) {
            if (TextInputV1* t = from(r)) {
                t->has_surrounding = false;
            }
        },
    .set_surrounding_text =
        [](wl_client*, wl_resource* r, const char* text, uint32_t cursor, uint32_t anchor) {
            if (TextInputV1* t = from(r)) {
                t->surrounding = text ? text : "";
                t->cursor = cursor;
                t->anchor = anchor;
                t->has_surrounding = true;
            }
        },
    .set_content_type =
        [](wl_client*, wl_resource* r, uint32_t hint, uint32_t purpose) {
            if (TextInputV1* t = from(r)) {
                // The bits are v3's, but for "default" (completion,
                // correction and capitalization), which v3 spells out.
                t->hint = hint;
                t->purpose = purpose_to_v3(purpose);
            }
        },
    .set_cursor_rectangle =
        [](wl_client*, wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h) {
            if (TextInputV1* t = from(r)) {
                t->cx = x, t->cy = y, t->cw = w, t->ch = h;
                t->has_cursor_rect = true;
            }
        },
    .set_preferred_language = [](wl_client*, wl_resource*, const char*) {},
    .commit_state =
        [](wl_client*, wl_resource* r, uint32_t serial) {
            if (TextInputV1* t = from(r)) {
                t->serial = serial;
                if (t->owner.on_commit)
                    t->owner.on_commit(t);
            }
        },
    .invoke_action = [](wl_client*, wl_resource*, uint32_t, uint32_t) {},
};

const struct zwp_text_input_manager_v1_interface manager_impl = {
    .create_text_input =
        [](wl_client* client, wl_resource* manager, uint32_t id) {
            auto* owner = static_cast<TextInputsV1*>(wl_resource_get_user_data(manager));
            wl_resource* r = wl_resource_create(client, &zwp_text_input_v1_interface, 1, id);
            if (!r) {
                wl_client_post_no_memory(client);
                return;
            }
            if (!owner) {  // the compositor is going away
                wl_resource_set_implementation(r, &input_impl, nullptr, nullptr);
                return;
            }
            auto* t = new TextInputV1(*owner, r);
            wl_resource_set_implementation(r, &input_impl, t, [](wl_resource* res) {
                if (TextInputV1* t = from(res))
                    delete t;
            });
            owner->inputs.push_back(t);
            if (owner->on_new)
                owner->on_new(t);
        },
};

} // namespace

void TextInputV1::set_asked(wlr_surface* surface) {
    if (asked == surface)
        return;
    if (asked)
        wl_list_remove(&asked_gone_.link);
    asked = surface;
    if (surface) {
        asked_gone_.notify = [](wl_listener* l, void*) {
            TextInputV1* t = wl_container_of(l, t, asked_gone_);
            wl_list_remove(&t->asked_gone_.link);
            t->asked = nullptr;
            if (t->entered)
                t->entered = nullptr;  // no leave to a surface that's gone
            if (t->owner.on_deactivate)
                t->owner.on_deactivate(t);
        };
        wl_signal_add(&surface->events.destroy, &asked_gone_);
    }
}

void TextInputV1::enter(wlr_surface* surface) {
    if (entered == surface)
        return;
    if (entered)
        leave();
    entered = surface;
    zwp_text_input_v1_send_enter(resource, surface->resource);
}

void TextInputV1::leave() {
    if (!entered)
        return;
    entered = nullptr;
    zwp_text_input_v1_send_leave(resource);
}

void TextInputV1::send(const char* preedit, int32_t preedit_cursor, const char* commit, uint32_t before,
                       uint32_t after) {
    // v1 applies a deletion with the next commit_string: one follows it.
    if (before || after) {
        zwp_text_input_v1_send_delete_surrounding_text(resource, -int32_t(before), before + after);
        zwp_text_input_v1_send_commit_string(resource, serial, commit ? commit : "");
    } else if (commit) {
        zwp_text_input_v1_send_commit_string(resource, serial, commit);
    }
    const char* text = preedit ? preedit : "";
    zwp_text_input_v1_send_preedit_cursor(resource, preedit ? preedit_cursor : 0);
    zwp_text_input_v1_send_preedit_styling(resource, 0, uint32_t(std::strlen(text)),
                                           ZWP_TEXT_INPUT_V1_PREEDIT_STYLE_UNDERLINE);
    zwp_text_input_v1_send_preedit_string(resource, serial, text, "");
}

TextInputV1::~TextInputV1() {
    if (asked)
        wl_list_remove(&asked_gone_.link);
    std::erase(owner.inputs, this);
    if (owner.on_destroy)
        owner.on_destroy(this);
}

TextInputsV1::TextInputsV1(wl_display* display) {
    global_ = wl_global_create(display, &zwp_text_input_manager_v1_interface, 1, this, bind);
}

TextInputsV1::~TextInputsV1() {
    on_new = on_activate = on_deactivate = on_commit = on_destroy = nullptr;
    for (wl_resource* m : managers_) {
        wl_resource_set_user_data(m, nullptr);
        wl_resource_set_destructor(m, nullptr);
    }
    for (TextInputV1* t : std::vector<TextInputV1*>(inputs)) {
        wl_resource_set_user_data(t->resource, nullptr);
        wl_resource_set_destructor(t->resource, nullptr);
        delete t;
    }
    if (global_)
        wl_global_destroy(global_);
}

void TextInputsV1::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* r = wl_resource_create(client, &zwp_text_input_manager_v1_interface, int(version), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* self = static_cast<TextInputsV1*>(data);
    self->managers_.push_back(r);
    wl_resource_set_implementation(r, &manager_impl, data, [](wl_resource* res) {
        if (auto* owner = static_cast<TextInputsV1*>(wl_resource_get_user_data(res)))
            std::erase(owner->managers_, res);
    });
}

} // namespace atrium
