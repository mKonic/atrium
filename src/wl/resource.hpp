#pragma once
#include <wayland-server-core.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace atrium::wl {

// The one lifetime model for protocol objects (the classes tools/wlscanner.py
// generates derive from this).
//
// A Resource is owned by its wl_resource: it is deleted when the resource goes,
// whether the client destroyed it, the client disconnected, or the compositor
// called destroy(). Code that keeps a pointer past the current call keeps a
// Weak<T>, which reads null once the object is gone.
//
// An owner that dies before its clients (the compositor shutting down) calls
// detach() on what it made: the object turns inert, dropping its handlers so
// nothing calls back into freed memory, and still goes away with its resource.
class Resource {
public:
    Resource(const Resource&) = delete;
    Resource& operator=(const Resource&) = delete;

    wl_resource* resource() const { return resource_; }
    wl_client* client() const { return resource_ ? wl_resource_get_client(resource_) : nullptr; }
    uint32_t version() const { return version_; }
    bool inert() const { return inert_; }

    // Called once, as the resource goes, before the object is deleted.
    void on_gone(std::function<void()> fn) { gone_ = std::move(fn); }

    void destroy() {
        if (resource_)
            wl_resource_destroy(resource_);
    }
    void post_error(uint32_t code, const char* message) {
        if (resource_)
            wl_resource_post_error(resource_, code, "%s", message);
    }
    void post_no_memory() {
        if (resource_)
            wl_resource_post_no_memory(resource_);
    }

    // Drops every handler; requests from now on are ignored, but a destructor
    // request still destroys.
    void detach() {
        inert_ = true;
        gone_ = nullptr;
        clear_handlers();
    }

    // The liveness token Weak<T> watches.
    std::weak_ptr<void> token() const { return token_; }

protected:
    Resource(wl_client* client, const wl_interface* interface, uint32_t version, uint32_t id,
             wl_dispatcher_func_t dispatcher, const void* tag);
    // Deleting a live object directly destroys its resource without calling
    // on_gone (the owner is the one deleting).
    virtual ~Resource();

    virtual void clear_handlers() = 0;

    // The object behind a resource, if `r` is one of this kind: `tag` is the
    // class's own, which libwayland compares as the implementation.
    static Resource* lookup(wl_resource* r, const wl_interface* interface, const void* tag);
    static Resource* of_target(void* target) {
        return static_cast<Resource*>(wl_resource_get_user_data(static_cast<wl_resource*>(target)));
    }

private:
    static void resource_destroyed(wl_resource* r);

    wl_resource* resource_ = nullptr;
    uint32_t version_ = 0;
    bool inert_ = false;
    std::function<void()> gone_;
    std::shared_ptr<char> token_ = std::make_shared<char>();

    template <class T, class... A>
    friend T* make(wl_client*, uint32_t, uint32_t, A&&...);
};

// Creates a protocol object (or a class derived from one): null, with the
// client told it ran out of memory, if the resource could not be made.
template <class T, class... A>
T* make(wl_client* client, uint32_t version, uint32_t id, A&&... args) {
    auto* object = new T(client, version, id, std::forward<A>(args)...);
    if (!object->resource_) {
        delete object;
        wl_client_post_no_memory(client);
        return nullptr;
    }
    return object;
}

// A pointer to a protocol object that reads null once the object is gone.
template <class T>
class Weak {
public:
    Weak() = default;
    Weak(T* object) : object_(object), token_(object ? object->token() : std::weak_ptr<void>{}) {}

    T* get() const { return token_.expired() ? nullptr : object_; }
    T* operator->() const { return get(); }
    explicit operator bool() const { return get() != nullptr; }
    bool operator==(const Weak& other) const { return get() == other.get(); }

private:
    T* object_ = nullptr;
    std::weak_ptr<void> token_;
};

// A wl_global whose binds construct objects through `bind`. Removing it (the
// destructor) stops new binds; objects already bound live on. A bind already
// on its way as it goes gets an inert object, so its client doesn't fail.
class Global {
public:
    using Bind = std::function<void(wl_client* client, uint32_t version, uint32_t id)>;

    template <class T>
    static std::unique_ptr<Global> create(wl_display* display, uint32_t version, Bind bind) {
        return std::unique_ptr<Global>(new Global(display, T::interface(), version, std::move(bind),
                                                  [](wl_client* c, uint32_t v, uint32_t id) {
                                                      if (T* o = make<T>(c, v, id))
                                                          o->detach();
                                                  }));
    }
    ~Global();
    Global(const Global&) = delete;
    Global& operator=(const Global&) = delete;

    wl_global* global() const { return global_; }

private:
    struct State;
    Global(wl_display* display, const wl_interface* interface, uint32_t version, Bind bind, Bind inert);
    static void bound(wl_client* client, void* data, uint32_t version, uint32_t id);

    wl_global* global_ = nullptr;
    State* state_;
};

} // namespace atrium::wl
