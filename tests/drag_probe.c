// A tab being torn out of a browser, for the smoke test: a window
// ("drag-probe") that, when clicked, starts a drag and hangs a new window
// ("drag-probe-torn") on it through xdg-toplevel-drag-v1. Prints "dragging",
// then "dropped" or "cancelled". Stays up until killed.
#define _GNU_SOURCE
#include "xdg-shell-client-protocol.h"
#include "xdg-toplevel-drag-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct xdg_wm_base* wm_base;
static struct wl_seat* seat;
static struct wl_data_device_manager* ddm;
static struct xdg_toplevel_drag_manager_v1* drag_manager;
static struct wl_data_device* data_device;

struct window {
    struct wl_surface* surface;
    struct xdg_surface* xdg;
    struct xdg_toplevel* toplevel;
    int width, height;
    uint32_t color;
};
static struct window main_window, torn;
static int dragging;

static struct wl_buffer* make_buffer(int w, int h, uint32_t color) {
    const int stride = w * 4, size = stride * h;
    int fd = memfd_create("drag-probe", 0);
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

static void source_target(void* d, struct wl_data_source* s, const char* mime) {}
static void source_send(void* d, struct wl_data_source* s, const char* mime, int32_t fd) { close(fd); }
static void source_cancelled(void* d, struct wl_data_source* s) {
    printf("cancelled\n");
    fflush(stdout);
}
static void source_dropped(void* d, struct wl_data_source* s) {
    printf("dropped\n");
    fflush(stdout);
}
static void source_finished(void* d, struct wl_data_source* s) {}
static void source_action(void* d, struct wl_data_source* s, uint32_t a) {}
static const struct wl_data_source_listener source_listener = {source_target, source_send, source_cancelled,
                                                               source_dropped, source_finished, source_action};

static void pointer_enter(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s, wl_fixed_t x,
                          wl_fixed_t y) {}
static void pointer_leave(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s) {}
static void pointer_motion(void* d, struct wl_pointer* p, uint32_t time, wl_fixed_t x, wl_fixed_t y) {}
static void pointer_button(void* d, struct wl_pointer* p, uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state) {
    if (state != WL_POINTER_BUTTON_STATE_PRESSED || dragging)
        return;
    dragging = 1;
    // Tear a "tab" off: a data source for the drag, the new window hung on it.
    struct wl_data_source* source = wl_data_device_manager_create_data_source(ddm);
    wl_data_source_add_listener(source, &source_listener, NULL);
    wl_data_source_offer(source, "application/x-drag-probe-tab");
    wl_data_source_set_actions(source, WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    struct xdg_toplevel_drag_v1* drag = xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(drag_manager, source);
    open_window(&torn, "drag-probe-torn", 240, 160, 0xffd08030);
    xdg_toplevel_drag_v1_attach(drag, torn.toplevel, 20, 20);
    wl_data_device_start_drag(data_device, source, main_window.surface, NULL, serial);
    wl_surface_commit(torn.surface);  // first configure maps it
    printf("dragging\n");
    fflush(stdout);
}
static void pointer_axis(void* d, struct wl_pointer* p, uint32_t time, uint32_t axis, wl_fixed_t v) {}
// A version 1 seat: only these arrive.
static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion, .button = pointer_button,
    .axis = pointer_axis};

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
    else if (!strcmp(interface, wl_seat_interface.name) && !seat)
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    else if (!strcmp(interface, wl_data_device_manager_interface.name))
        ddm = wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3);
    else if (!strcmp(interface, xdg_toplevel_drag_manager_v1_interface.name))
        drag_manager = wl_registry_bind(registry, name, &xdg_toplevel_drag_manager_v1_interface, 1);
}
static void global_remove(void* data, struct wl_registry* registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {global, global_remove};

int main(void) {
    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "drag_probe: no Wayland display\n");
        return 1;
    }
    wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !wm_base || !seat || !ddm || !drag_manager) {
        fprintf(stderr, "drag_probe: the compositor has no xdg-toplevel-drag-v1\n");
        return 1;
    }
    xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    wl_pointer_add_listener(wl_seat_get_pointer(seat), &pointer_listener, NULL);
    data_device = wl_data_device_manager_get_data_device(ddm, seat);
    open_window(&main_window, "drag-probe", 320, 220, 0xff3d6eb4);
    wl_surface_commit(main_window.surface);
    printf("ready\n");
    fflush(stdout);
    while (wl_display_dispatch(display) != -1) {
    }
    return 0;
}
