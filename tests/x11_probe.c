// An X11 window of a given _NET_WM_WINDOW_TYPE, for checking how atrium
// treats splash screens, notifications and the like:
//
//   x11_probe TYPE [TITLE]      TYPE: normal, splash, notification, utility, ...
//
// Its WM_CLASS is "x11probe". It stays up until killed.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>

static xcb_atom_t atom(xcb_connection_t* c, const char* name) {
    xcb_intern_atom_reply_t* r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: x11_probe TYPE [TITLE]\n");
        return 1;
    }
    char type_name[64];
    snprintf(type_name, sizeof type_name, "_NET_WM_WINDOW_TYPE_%s", argv[1]);
    for (char* p = type_name; *p; ++p)
        if (*p >= 'a' && *p <= 'z')
            *p -= 'a' - 'A';
    const char* title = argc > 2 ? argv[2] : argv[1];

    xcb_connection_t* c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) {
        fprintf(stderr, "x11_probe: no X display\n");
        return 1;
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    xcb_window_t w = xcb_generate_id(c);
    const uint32_t values[] = {0xff3d6eb4};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, screen->root, 0, 0, 320, 160, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, XCB_CW_BACK_PIXEL, values);

    const xcb_atom_t type = atom(c, type_name);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, atom(c, "_NET_WM_WINDOW_TYPE"), XCB_ATOM_ATOM, 32, 1, &type);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, strlen(title), title);
    static const char cls[] = "x11probe\0x11probe";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof cls, cls);
    xcb_map_window(c, w);
    xcb_flush(c);

    xcb_generic_event_t* e;
    while ((e = xcb_wait_for_event(c)))
        free(e);
    return 0;
}
