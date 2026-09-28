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

Global::Global(wl_display* display, const wl_interface* interface, uint32_t version, Bind bind)
    : bind_(std::move(bind)) {
    global_ = wl_global_create(display, interface, int(version), this, &bound);
}

Global::~Global() {
    if (global_)
        wl_global_destroy(global_);
}

void Global::bound(wl_client* client, void* data, uint32_t version, uint32_t id) {
    static_cast<Global*>(data)->bind_(client, version, id);
}

} // namespace atrium::wl
