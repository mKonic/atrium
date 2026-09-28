#include "wl/resource.hpp"

namespace atrium::wl {

Resource::Resource(wl_client* client, const wl_interface* interface, uint32_t version, uint32_t id,
                   wl_dispatcher_func_t dispatcher, const void* tag)
    : version_(version) {
    resource_ = wl_resource_create(client, interface, int(version), id);
    if (resource_)
        wl_resource_set_dispatcher(resource_, dispatcher, tag, this, &resource_destroyed);
}

Resource::~Resource() {
    if (!resource_)
        return;
    wl_resource_set_destructor(resource_, nullptr);
    wl_resource_set_user_data(resource_, nullptr);
    wl_resource_destroy(resource_);
}

void Resource::resource_destroyed(wl_resource* r) {
    auto* self = static_cast<Resource*>(wl_resource_get_user_data(r));
    if (!self)
        return;
    self->resource_ = nullptr;
    if (auto gone = std::move(self->gone_))
        gone();
    delete self;
}

Resource* Resource::lookup(wl_resource* r, const wl_interface* interface, const void* tag) {
    if (!r || !wl_resource_instance_of(r, interface, tag))
        return nullptr;
    return static_cast<Resource*>(wl_resource_get_user_data(r));
}

struct Global::State {
    Bind bind, inert;
    bool live = true;
    wl_global* global = nullptr;
    wl_event_source* timer = nullptr;
    wl_listener display_destroy{};
};

Global::Global(wl_display* display, const wl_interface* interface, uint32_t version, Bind bind, Bind inert)
    : state_(new State{std::move(bind), std::move(inert)}) {
    global_ = wl_global_create(display, interface, int(version), state_, &bound);
    state_->global = global_;
}

namespace {

// A removed global lingers: clients may be binding it as it goes (their
// bind is already on the wire), and destroying it at once would make that a
// protocol error. It goes for good a moment later, or with the display.
template <class State>
void finish(State* s, bool destroy_global) {
    wl_list_remove(&s->display_destroy.link);
    wl_event_source_remove(s->timer);
    if (destroy_global)
        wl_global_destroy(s->global);
    delete s;
}

} // namespace

Global::~Global() {
    if (!global_) {
        delete state_;
        return;
    }
    wl_display* display = wl_global_get_display(global_);
    wl_global_remove(global_);
    State* s = state_;
    s->live = false;
    s->bind = nullptr;
    s->timer = wl_event_loop_add_timer(wl_display_get_event_loop(display), [](void* data) {
        finish(static_cast<State*>(data), true);
        return 0;
    }, s);
    wl_event_source_timer_update(s->timer, 5000);
    s->display_destroy.notify = [](wl_listener* listener, void*) {
        State* s = wl_container_of(listener, s, display_destroy);
        finish(s, false);  // the display destroys its globals itself
    };
    wl_display_add_destroy_listener(display, &s->display_destroy);
}

void Global::bound(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* s = static_cast<State*>(data);
    if (s->live)
        s->bind(client, version, id);
    else
        s->inert(client, version, id);
}

} // namespace atrium::wl
