// An app that asks for its window back (xdg-session-management-v1), for the
// smoke test:
//
//   session_probe            a new session: prints "created ID"
//   session_probe ID         restores it: prints "restored" (the session) and
//                            "window restored" (its window, "main")
//
// Its window is "session-probe". Stays up until killed.
#define _GNU_SOURCE
#include "xdg-shell-client-protocol.h"
#include "xdg-session-management-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct xdg_wm_base* wm_base;
static struct xdg_session_manager_v1* session_manager;

struct window {
    struct wl_surface* surface;
    struct xdg_surface* xdg;
    struct xdg_toplevel* toplevel;
    int width, height;
    uint32_t color;
};
static struct window main_window;

static struct wl_buffer* make_buffer(int w, int h, uint32_t color) {
    const int stride = w * 4, size = stride * h;
    int fd = memfd_create("session-probe", 0);
    if (fd < 0 || ftruncate(fd, size) < 0)
        exit(1);
    uint32_t* px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int i = 0; i < w * h; ++i)
        px[i] = color;
    munmap(px, size);
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

static void configure(void* data, struct xdg_surface* xdg, uint32_t serial) {
    struct window* win = data;
    xdg_surface_ack_configure(xdg, serial);
    wl_surface_attach(win->surface, make_buffer(win->width, win->height, win->color), 0, 0);
    wl_surface_commit(win->surface);
}
static const struct xdg_surface_listener surface_listener = {configure};

static void top_configure(void* d, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* s) {}
static void top_close(void* d, struct xdg_toplevel* t) {}
static void top_bounds(void* d, struct xdg_toplevel* t, int32_t w, int32_t h) {}
static void top_caps(void* d, struct xdg_toplevel* t, struct wl_array* c) {}
static const struct xdg_toplevel_listener toplevel_listener = {top_configure, top_close, top_bounds, top_caps};

static void open_window(struct window* win, const char* app_id, int w, int h, uint32_t color) {
    win->width = w;
    win->height = h;
    win->color = color;
    win->surface = wl_compositor_create_surface(compositor);
    win->xdg = xdg_wm_base_get_xdg_surface(wm_base, win->surface);
    xdg_surface_add_listener(win->xdg, &surface_listener, win);
    win->toplevel = xdg_surface_get_toplevel(win->xdg);
    xdg_toplevel_add_listener(win->toplevel, &toplevel_listener, win);
    xdg_toplevel_set_app_id(win->toplevel, app_id);
    xdg_toplevel_set_title(win->toplevel, app_id);
}

static void ping(void* d, struct xdg_wm_base* b, uint32_t serial) { xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_base_listener = {ping};

static void global(void* data, struct wl_registry* registry, uint32_t name, const char* interface,
                   uint32_t version) {
    if (!strcmp(interface, wl_compositor_interface.name))
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    else if (!strcmp(interface, wl_shm_interface.name))
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, xdg_wm_base_interface.name))
        wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(interface, xdg_session_manager_v1_interface.name))
        session_manager = wl_registry_bind(registry, name, &xdg_session_manager_v1_interface, 1);
}
static void global_remove(void* data, struct wl_registry* registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {global, global_remove};

static void say(const char* what) {
    printf("%s\n", what);
    fflush(stdout);
}
static void session_created(void* d, struct xdg_session_v1* s, const char* id) {
    printf("created %s\n", id);
    fflush(stdout);
}
static void session_restored(void* d, struct xdg_session_v1* s) { say("restored"); }
static void session_replaced(void* d, struct xdg_session_v1* s) { say("replaced"); }
static const struct xdg_session_v1_listener session_listener = {session_created, session_restored, session_replaced};
static void window_restored(void* d, struct xdg_toplevel_session_v1* t) { say("window restored"); }
static const struct xdg_toplevel_session_v1_listener window_listener = {window_restored};

int main(int argc, char** argv) {
    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "session_probe: no Wayland display\n");
        return 1;
    }
    wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !wm_base || !session_manager) {
        fprintf(stderr, "session_probe: the compositor has no xdg-session-management-v1\n");
        return 1;
    }
    xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    const char* id = argc > 1 ? argv[1] : NULL;
    struct xdg_session_v1* session = xdg_session_manager_v1_get_session(
        session_manager, id ? XDG_SESSION_MANAGER_V1_REASON_SESSION_RESTORE : XDG_SESSION_MANAGER_V1_REASON_LAUNCH, id);
    xdg_session_v1_add_listener(session, &session_listener, NULL);
    open_window(&main_window, "session-probe", 320, 220, 0xff3d6eb4);
    struct xdg_toplevel_session_v1* window = id ? xdg_session_v1_restore_toplevel(session, main_window.toplevel, "main")
                                                : xdg_session_v1_add_toplevel(session, main_window.toplevel, "main");
    xdg_toplevel_session_v1_add_listener(window, &window_listener, NULL);
    wl_surface_commit(main_window.surface);
    while (wl_display_dispatch(display) != -1) {
    }
    return 0;
}
