#include "wl/buffer.hpp"

namespace atrium::wl {

// Follows the Buffer, which can outlive us: release tells the client,
// while there is one.
struct ClientBuffer::Tracker {
    ClientBuffer* owner;
    wl_listener release;
    wl_listener destroy;
};

ClientBuffer::ClientBuffer(wl_client* client, uint32_t version, uint32_t id, Buffer* buffer)
    : WlBuffer(client, version, id), buffer_(buffer), tracker_(new Tracker{this, {}, {}}) {
    tracker_->release.notify = [](wl_listener* l, void*) {
        Tracker* t = wl_container_of(l, t, release);
        if (t->owner)
            t->owner->send_release();
    };
    tracker_->destroy.notify = [](wl_listener* l, void*) {
        Tracker* t = wl_container_of(l, t, destroy);
        wl_list_remove(&t->release.link);
        wl_list_remove(&t->destroy.link);
        if (t->owner)
            t->owner->tracker_ = nullptr;
        delete t;
    };
    wl_signal_add(&buffer->events.release, &tracker_->release);
    wl_signal_add(&buffer->events.destroy, &tracker_->destroy);
}

ClientBuffer::~ClientBuffer() {
    if (tracker_)
        tracker_->owner = nullptr;
    buffer_drop(buffer_);
}

ClientBuffer* ClientBuffer::from(wl_resource* resource) {
    return dynamic_cast<ClientBuffer*>(WlBuffer::from(resource));
}

} // namespace atrium::wl
