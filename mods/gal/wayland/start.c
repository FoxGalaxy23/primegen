#include "gal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <stdarg.h>

#ifdef HAVE_LOGGER
  #include "logger.h"
#endif

#ifdef HAVE_RENDER
  #include "ttf_render.h"
  extern TTFFont* g_ttf_font;
#endif

static struct wl_display *display = NULL;
static struct wl_registry *registry = NULL;
static struct wl_compositor *compositor = NULL;
static struct wl_shm *shm = NULL;
static struct wl_surface *surface = NULL;
static struct wl_shell *shell = NULL;
static struct wl_shell_surface *shell_surface = NULL;
static struct wl_seat *seat = NULL;
static struct wl_pointer *pointer = NULL;
static struct wl_keyboard *keyboard = NULL;

static struct xkb_context *xkb_ctx = NULL;
static struct xkb_keymap *xkb_keymap = NULL;
static struct xkb_state *xkb_state = NULL;

static int window_width = 1024;
static int window_height = 768;
static struct wl_buffer *wl_buffer = NULL;
static void *shm_data = NULL;

static int mouse_x = 0;
static int mouse_y = 0;
static uint32_t mouse_button_state = 0;
static int mouse_moved = 0;
static int mouse_down_x = 0;
static int mouse_down_y = 0;

static GAL_Event last_event;
static int has_event = 0;
static int quit_requested = 0;

void gal_log(const char* level, const char* format, ...) {
#ifdef HAVE_LOGGER
    va_list args;
    va_start(args, format);
    custom_logger(level, format, args);
    va_end(args);
#else
    (void)level; (void)format;
#endif
}

static void pointer_handle_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
    mouse_x = wl_fixed_to_int(sx);
    mouse_y = wl_fixed_to_int(sy);
}

static void pointer_handle_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface) {}

static void pointer_handle_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
    mouse_x = wl_fixed_to_int(sx);
    mouse_y = wl_fixed_to_int(sy);

    last_event.type = GAL_EVENT_MOUSE_MOVE;
    last_event.mouse_x = mouse_x;
    last_event.mouse_y = mouse_y;
    has_event = 1;

    if (mouse_button_state && (abs(mouse_x - mouse_down_x) > 3 || abs(mouse_y - mouse_down_y) > 3)) {
        mouse_moved = 1;
    }
}

static void pointer_handle_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    mouse_button_state = (state == WL_POINTER_BUTTON_STATE_PRESSED);

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        mouse_moved = 0;
        mouse_down_x = mouse_x;
        mouse_down_y = mouse_y;
        last_event.type = GAL_EVENT_MOUSE_DOWN;
    } else {
        last_event.type = mouse_moved ? GAL_EVENT_MOUSE_UP : GAL_EVENT_MOUSE_CLICK;
    }

    last_event.mouse_x = mouse_x;
    last_event.mouse_y = mouse_y;

    // Linux button codes: 272=LEFT, 273=RIGHT, 274=MIDDLE
    if (button == 272) last_event.mouse_button = GAL_MOUSE_LEFT;
    else if (button == 273) last_event.mouse_button = GAL_MOUSE_RIGHT;
    else if (button == 274) last_event.mouse_button = GAL_MOUSE_MIDDLE;

    has_event = 1;
}

static void pointer_handle_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {
    last_event.type = GAL_EVENT_MOUSE_SCROLL;
    last_event.mouse_x = mouse_x;
    last_event.mouse_y = mouse_y;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        last_event.scroll_y = wl_fixed_to_int(value) * 10;
    } else {
        last_event.scroll_x = wl_fixed_to_int(value) * 10;
    }
    has_event = 1;
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_handle_enter, pointer_handle_leave, pointer_handle_motion, pointer_handle_button, pointer_handle_axis
};

static void keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int fd, uint32_t size) {
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *map_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map_str == MAP_FAILED) { close(fd); return; }

    xkb_keymap_unref(xkb_keymap);
    xkb_keymap = xkb_keymap_new_from_string(xkb_ctx, map_str, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
    munmap(map_str, size);
    close(fd);

    xkb_state_unref(xkb_state);
    xkb_state = xkb_state_new(xkb_keymap);
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {}
static void keyboard_handle_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface) {}

static void keyboard_handle_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;

    uint32_t utf32 = xkb_state_key_get_one_sym(xkb_state, key + 8);
    char buf[32];
    int len = xkb_state_key_get_utf8(xkb_state, key + 8, buf, sizeof(buf));

    last_event.type = GAL_EVENT_KEY_PRESS;
    if (len > 0) {
        last_event.key_char = buf[0];
        memcpy(last_event.text, buf, len);
        last_event.text[len] = '\0';
        last_event.text_len = len;
    } else {
        last_event.key_char = 0;
    }
    last_event.key_code = (int)utf32;
    has_event = 1;
}

static void keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    xkb_state_update_mask(xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_handle_keymap, keyboard_handle_enter, keyboard_handle_leave, keyboard_handle_key, keyboard_handle_modifiers
};

static void seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
    }
}

static const struct wl_seat_listener seat_listener = { seat_handle_capabilities };

static void registry_handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    if (strcmp(interface, "wl_compositor") == 0) compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    else if (strcmp(interface, "wl_shm") == 0) shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, "wl_shell") == 0) shell = wl_registry_bind(registry, name, &wl_shell_interface, 1);
    else if (strcmp(interface, "wl_seat") == 0) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = { registry_handle_global, registry_handle_global_remove };

static int set_cloexec_or_close(int fd) {
    long flags;
    if (fd == -1) return -1;
    flags = fcntl(fd, F_GETFD);
    if (flags == -1 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
        close(fd);
        return -1;
    }
    return fd;
}

static int create_tmpfile_cloexec(char *tmpname) {
    int fd = mkstemp(tmpname);
    if (fd >= 0) {
        fd = set_cloexec_or_close(fd);
        unlink(tmpname);
    }
    return fd;
}

static int os_create_anonymous_file(off_t size) {
    static const char template[] = "/gal-wayland-XXXXXX";
    const char *path = getenv("XDG_RUNTIME_DIR");
    if (!path) return -1;
    char *name = malloc(strlen(path) + sizeof(template));
    strcpy(name, path);
    strcat(name, template);
    int fd = create_tmpfile_cloexec(name);
    free(name);
    if (fd < 0) return -1;
    if (ftruncate(fd, size) < 0) { close(fd); return -1; }
    return fd;
}

static void shell_surface_ping(void *data, struct wl_shell_surface *shell_surface, uint32_t serial) {
    wl_shell_surface_pong(shell_surface, serial);
}

static void shell_surface_configure(void *data, struct wl_shell_surface *shell_surface, uint32_t edges, int32_t width, int32_t height) {
    if (width > 0 && height > 0) gal_resize(width, height);
}

static void shell_surface_popup_done(void *data, struct wl_shell_surface *shell_surface) {}

static const struct wl_shell_surface_listener shell_surface_listener = {
    shell_surface_ping, shell_surface_configure, shell_surface_popup_done
};

int gal_init(void) {
    display = wl_display_connect(NULL);
    if (!display) return 0;

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !shell) return 0;

    xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    surface = wl_compositor_create_surface(compositor);
    shell_surface = wl_shell_get_shell_surface(shell, surface);
    wl_shell_surface_add_listener(shell_surface, &shell_surface_listener, NULL);
    wl_shell_surface_set_toplevel(shell_surface);
    wl_shell_surface_set_title(shell_surface, "Meetlookers Browser Engine (Wayland)");

    gal_resize(SCREEN_WIDTH, SCREEN_HEIGHT);

#ifdef HAVE_RENDER
    g_ttf_font = ttf_load("assets/font.ttf");
    if (!g_ttf_font) gal_log("WARN", "TTF font not loaded");
#endif

    return 1;
}

void gal_flush(void) {
    wl_surface_attach(surface, wl_buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, window_width, window_height);
    wl_surface_commit(surface);
    wl_display_flush(display);
}

void gal_resize(int width, int height) {
    if (width == window_width && height == window_height && wl_buffer) return;

    SCREEN_WIDTH = window_width = width;
    SCREEN_HEIGHT = window_height = height;

    if (wl_buffer) wl_buffer_destroy(wl_buffer);
    if (shm_data) munmap(shm_data, window_width * window_height * 4);

    int stride = window_width * 4;
    int size = stride * window_height;
    int fd = os_create_anonymous_file(size);
    if (fd < 0) return;

    shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    wl_buffer = wl_shm_pool_create_buffer(pool, 0, window_width, window_height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    back_buffer = (uint32_t*)shm_data;
    last_event.type = GAL_EVENT_REDRAW;
    has_event = 1;
}

GAL_Event gal_wait_event(void) {
    while (!has_event && !quit_requested) {
        if (wl_display_dispatch(display) == -1) quit_requested = 1;
    }
    has_event = 0;
    if (quit_requested) { GAL_Event ev = {0}; ev.type = GAL_EVENT_QUIT; return ev; }
    return last_event;
}

GAL_Event gal_poll_event(void) {
    if (wl_display_prepare_read(display) == 0) {
        wl_display_read_events(display);
        wl_display_dispatch_pending(display);
    } else {
        wl_display_dispatch_pending(display);
    }

    if (has_event) { has_event = 0; return last_event; }
    GAL_Event ev = {0}; ev.type = GAL_EVENT_NONE;
    return ev;
}

void gal_close(void) {
#ifdef HAVE_RENDER
    if (g_ttf_font) { ttf_free(g_ttf_font); g_ttf_font = NULL; }
#endif
    if (wl_buffer) wl_buffer_destroy(wl_buffer);
    if (shm_data) munmap(shm_data, window_width * window_height * 4);
    if (shell_surface) wl_shell_surface_destroy(shell_surface);
    if (surface) wl_surface_destroy(surface);
    if (pointer) wl_pointer_destroy(pointer);
    if (keyboard) wl_keyboard_destroy(keyboard);
    if (seat) wl_seat_destroy(seat);
    if (shm) wl_shm_destroy(shm);
    if (shell) wl_shell_destroy(shell);
    if (compositor) wl_compositor_destroy(compositor);
    if (registry) wl_registry_destroy(registry);
    if (display) wl_display_disconnect(display);

    xkb_state_unref(xkb_state);
    xkb_keymap_unref(xkb_keymap);
    xkb_context_unref(xkb_ctx);
}

void gal_set_clipboard(const char* text) { (void)text; } // TODO: Implement Wayland clipboard
char* gal_get_clipboard(void) { return NULL; }
