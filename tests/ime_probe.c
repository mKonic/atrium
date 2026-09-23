// A minimal input method for the smoke test. It grabs the keyboard while a
// text field is focused, turns the a key into "α" and the b key into "IME>"
// and a new line, and swallows the rest. Proves the relay both ways: keys from the keyboard
// to the IME, text from the IME to the app. Prints what it sees.
#include "input-method-unstable-v2-client-protocol.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

static struct wl_seat* seat;
static struct zwp_input_method_manager_v2* manager;
static struct zwp_input_method_v2* im;
static uint32_t serial;  // done events so far; commit() echoes it
static int active, pending_active;

static void keymap(void* data, struct zwp_input_method_keyboard_grab_v2* grab, uint32_t format, int32_t fd,
                   uint32_t size) {
    close(fd);
}

static void key(void* data, struct zwp_input_method_keyboard_grab_v2* grab, uint32_t s, uint32_t time,
                uint32_t keycode, uint32_t state) {
    printf("key %u %s\n", keycode, state ? "down" : "up");
    const char* text = keycode == 30 ? "α" : keycode == 48 ? "IME>\n" : NULL;  // KEY_A, KEY_B
    if (text && state == 1 && active) {
        zwp_input_method_v2_commit_string(im, text);
        zwp_input_method_v2_commit(im, serial);
    }
    fflush(stdout);
}

static void modifiers(void* data, struct zwp_input_method_keyboard_grab_v2* grab, uint32_t s, uint32_t depressed,
                      uint32_t latched, uint32_t locked, uint32_t group) {}
static void repeat_info(void* data, struct zwp_input_method_keyboard_grab_v2* grab, int32_t rate, int32_t delay) {}

static const struct zwp_input_method_keyboard_grab_v2_listener grab_listener = {keymap, key, modifiers, repeat_info};

static void activate(void* data, struct zwp_input_method_v2* m) { pending_active = 1; }
static void deactivate(void* data, struct zwp_input_method_v2* m) { pending_active = 0; }
static void surrounding_text(void* data, struct zwp_input_method_v2* m, const char* text, uint32_t cursor,
                             uint32_t anchor) {}
static void text_change_cause(void* data, struct zwp_input_method_v2* m, uint32_t cause) {}
static void content_type(void* data, struct zwp_input_method_v2* m, uint32_t hint, uint32_t purpose) {}

static void done(void* data, struct zwp_input_method_v2* m) {
    ++serial;
    if (pending_active != active) {
        active = pending_active;
        printf(active ? "activate\n" : "deactivate\n");
        fflush(stdout);
    }
}

static void unavailable(void* data, struct zwp_input_method_v2* m) {
    printf("unavailable\n");
    fflush(stdout);
}

static const struct zwp_input_method_v2_listener im_listener = {
    activate, deactivate, surrounding_text, text_change_cause, content_type, done, unavailable};

static void global(void* data, struct wl_registry* registry, uint32_t name, const char* interface,
                   uint32_t version) {
    if (!strcmp(interface, wl_seat_interface.name) && !seat)
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    else if (!strcmp(interface, zwp_input_method_manager_v2_interface.name))
        manager = wl_registry_bind(registry, name, &zwp_input_method_manager_v2_interface, 1);
}

static void global_remove(void* data, struct wl_registry* registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {global, global_remove};

int main(void) {
    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "ime_probe: no Wayland display\n");
        return 1;
    }
    wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!seat || !manager) {
        fprintf(stderr, "ime_probe: the compositor has no input-method-v2\n");
        return 1;
    }
    im = zwp_input_method_manager_v2_get_input_method(manager, seat);
    zwp_input_method_v2_add_listener(im, &im_listener, NULL);
    zwp_input_method_keyboard_grab_v2_add_listener(zwp_input_method_v2_grab_keyboard(im), &grab_listener, NULL);
    printf("ready\n");
    fflush(stdout);
    while (wl_display_dispatch(display) != -1) {
    }
    return 0;
}
