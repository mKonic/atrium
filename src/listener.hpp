#pragma once
#include "wlr.hpp"

#include <functional>
#include <utility>

namespace atrium {

// One wl_listener with a C++ callback. It disconnects itself when destroyed,
// so an object that dies can never leave a dangling entry in a wlroots signal
// list, which is the classic use-after-free in C compositors.
//
// Not copyable or movable: wlroots holds a pointer to the embedded wl_listener,
// so the owning object must stay put (atrium heap-allocates everything that
// owns listeners). A callback may delete its owner, and with it this
// Listener, as its last action; nothing here touches `this` after the call.
template <typename T = void>
class Listener {
public:
    using Fn = std::function<void(T*)>;

    Listener() {
        wl_list_init(&hook_.wl.link);
        hook_.self = this;
    }
    ~Listener() { disconnect(); }

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    void connect(wl_signal* signal, Fn fn) {
        disconnect();
        fn_ = std::move(fn);
        hook_.wl.notify = &Listener::thunk;
        wl_signal_add(signal, &hook_.wl);
    }

    void disconnect() {
        wl_list_remove(&hook_.wl.link);
        wl_list_init(&hook_.wl.link);
    }

    bool connected() const { return !wl_list_empty(&hook_.wl.link); }

private:
    // Standard-layout, wl_listener first: a wl_listener* is a Hook*.
    struct Hook {
        wl_listener wl;
        Listener* self;
    };

    static void thunk(wl_listener* l, void* data) {
        reinterpret_cast<Hook*>(l)->self->fn_(static_cast<T*>(data));
    }

    Hook hook_{};
    Fn fn_;
};

} // namespace atrium
