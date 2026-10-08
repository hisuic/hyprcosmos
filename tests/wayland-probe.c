#define _GNU_SOURCE
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "xdg-shell-client-protocol.h"
#include "virtual-keyboard-client-protocol.h"
#include "virtual-pointer-client-protocol.h"

// A real Wayland surface and seat receiver; the second mode injects events only
// into an explicitly selected hyprcosmos-test socket. No compositor internals.
static struct wl_display* display;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct wl_seat* seat;
static struct xdg_wm_base* shell;
static struct zwp_virtual_keyboard_manager_v1* keyboard_manager;
static struct zwlr_virtual_pointer_manager_v1* pointer_manager;
static struct wl_surface* surface;
static struct wl_keyboard* receiving_keyboard;
static struct wl_pointer* receiving_pointer;
static int width = 640, height = 400, running = 1, configured = 0, variant = 0;
static unsigned tick = 0;

static uint32_t millis(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
static void pause_ms(unsigned duration) {
    struct timespec t = {duration / 1000, (long)(duration % 1000) * 1000000};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
static void fail(const char* message) { fprintf(stderr, "%s\n", message); exit(1); }
struct pixels { struct wl_buffer* buffer; void* data; size_t size; };
static void buffer_release(void* data, struct wl_buffer* buffer) {
    struct pixels* p = data;
    wl_buffer_destroy(buffer);
    munmap(p->data, p->size);
    free(p);
}
static const struct wl_buffer_listener buffer_listener = {buffer_release};
static void paint(void) {
    if (!configured) return;
    const size_t size = (size_t)width * height * 4;
    const int fd = memfd_create("hyprcosmos-probe-pixels", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size)) fail("Could not allocate probe pixels");
    struct pixels* p = calloc(1, sizeof(*p));
    p->size = size;
    p->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p->data == MAP_FAILED) fail("Could not map probe pixels");
    uint32_t* image = p->data;
    const uint32_t accent = variant % 2 ? 0xffcf63df : 0xff4fbeef;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint32_t color = ((x / 36 + y / 36) % 2) ? 0xff171c34 : 0xff202741;
            if (y < 42 || x < 5 || y > height - 6 || x > width - 6) color = accent;
            if (y >= 58 && y < 88 && x < 20 + (int)(tick % 20) * 18) color = 0xfff2c66d;
            if (x > width / 2 - 4 && x < width / 2 + 4) color = accent;
            if (y > height / 2 - 4 && y < height / 2 + 4) color = accent;
            image[(size_t)y * width + x] = color;
        }
    }
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    p->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
    wl_buffer_add_listener(p->buffer, &buffer_listener, p);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_surface_attach(surface, p->buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, width, height);
    wl_surface_commit(surface);
    wl_display_flush(display);
    ++tick;
}
static void keymap(void* d, struct wl_keyboard* k, uint32_t format, int32_t fd, uint32_t size) { close(fd); }
static void key_enter(void* d, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s, struct wl_array* keys) {
    puts("{\"event\":\"keyboard_enter\"}");
}
static void key_leave(void* d, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s) {
    puts("{\"event\":\"keyboard_leave\"}");
}
static void key(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t time, uint32_t code, uint32_t state) {
    printf("{\"event\":\"key\",\"key\":%u,\"state\":%u}\n", code, state);
}
static void modifiers(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    printf("{\"event\":\"modifiers\",\"depressed\":%u,\"latched\":%u,\"locked\":%u,\"group\":%u}\n", depressed, latched, locked, group);
}
static void repeat(void* d, struct wl_keyboard* k, int32_t rate, int32_t delay) {}
static const struct wl_keyboard_listener keyboard_listener = {keymap, key_enter, key_leave, key, modifiers, repeat};
static void pointer_enter(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y) {
    printf("{\"event\":\"pointer_enter\",\"x\":%.2f,\"y\":%.2f}\n", wl_fixed_to_double(x), wl_fixed_to_double(y));
}
static void pointer_leave(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s) {}
static void motion(void* d, struct wl_pointer* p, uint32_t time, wl_fixed_t x, wl_fixed_t y) {}
static void button(void* d, struct wl_pointer* p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    printf("{\"event\":\"button\",\"button\":%u,\"state\":%u}\n", button, state);
}
static void axis(void* d, struct wl_pointer* p, uint32_t time, uint32_t axis, wl_fixed_t value) {
    printf("{\"event\":\"axis\",\"axis\":%u,\"value\":%.2f}\n", axis, wl_fixed_to_double(value));
}
static void frame(void* d, struct wl_pointer* p) {}
static void axis_source(void* d, struct wl_pointer* p, uint32_t source) {}
static void axis_stop(void* d, struct wl_pointer* p, uint32_t time, uint32_t axis) {}
static void axis_discrete(void* d, struct wl_pointer* p, uint32_t axis, int32_t discrete) {}
static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = motion, .button = button,
    .axis = axis, .frame = frame, .axis_source = axis_source, .axis_stop = axis_stop,
    .axis_discrete = axis_discrete,
};
static void capabilities(void* d, struct wl_seat* s, uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !receiving_keyboard) {
        receiving_keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(receiving_keyboard, &keyboard_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !receiving_pointer) {
        receiving_pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(receiving_pointer, &pointer_listener, NULL);
    }
}
static void seat_name(void* d, struct wl_seat* s, const char* name) {}
static const struct wl_seat_listener seat_listener = {capabilities, seat_name};
static void ping(void* d, struct xdg_wm_base* wm, uint32_t serial) { xdg_wm_base_pong(wm, serial); }
static const struct xdg_wm_base_listener shell_listener = {ping};
static void registry(void* d, struct wl_registry* r, uint32_t id, const char* name, uint32_t version) {
    if (!strcmp(name, "wl_compositor")) compositor = wl_registry_bind(r, id, &wl_compositor_interface, 4);
    if (!strcmp(name, "wl_shm")) shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
    if (!strcmp(name, "wl_seat")) {
        seat = wl_registry_bind(r, id, &wl_seat_interface, version < 5 ? version : 5);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
    if (!strcmp(name, "xdg_wm_base")) {
        shell = wl_registry_bind(r, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(shell, &shell_listener, NULL);
    }
    if (!strcmp(name, "zwp_virtual_keyboard_manager_v1"))
        keyboard_manager = wl_registry_bind(r, id, &zwp_virtual_keyboard_manager_v1_interface, 1);
    if (!strcmp(name, "zwlr_virtual_pointer_manager_v1"))
        pointer_manager = wl_registry_bind(r, id, &zwlr_virtual_pointer_manager_v1_interface, version < 2 ? version : 2);
}
static void global_remove(void* d, struct wl_registry* r, uint32_t id) {}
static const struct wl_registry_listener registry_listener = {registry, global_remove};
static void configure(void* d, struct xdg_surface* s, uint32_t serial) {
    xdg_surface_ack_configure(s, serial);
    configured = 1;
    printf("{\"event\":\"configure\",\"width\":%d,\"height\":%d}\n", width, height);
    paint();
}
static const struct xdg_surface_listener surface_listener = {configure};
static void top_configure(void* d, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* states) {
    if (w > 0) width = w;
    if (h > 0) height = h;
}
static void top_close(void* d, struct xdg_toplevel* t) { running = 0; }
static const struct xdg_toplevel_listener toplevel_listener = {.configure = top_configure, .close = top_close};

static void inject(int argc, char** argv) {
    if (argc < 4) fail("--input key|click|scroll VALUE [HOLD_MS], or --input move X Y WIDTH HEIGHT");
    const unsigned value = strtoul(argv[3], NULL, 10);
    if (!strcmp(argv[2], "key") || !strcmp(argv[2], "ctrl-key")) {
        if (!keyboard_manager || !seat) fail("Virtual keyboard protocol unavailable");
        struct xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        struct xkb_keymap* map = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
        char* text = xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1);
        const size_t length = strlen(text) + 1;
        const int fd = memfd_create("hyprcosmos-probe-keymap", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, length) || write(fd, text, length) != (ssize_t)length) fail("Keymap allocation failed");
        struct zwp_virtual_keyboard_v1* keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(keyboard_manager, seat);
        zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, length);
        close(fd); free(text);
        wl_display_roundtrip(display);
        if (!strcmp(argv[2], "ctrl-key")) {
            zwp_virtual_keyboard_v1_key(keyboard, millis(), KEY_LEFTCTRL, WL_KEYBOARD_KEY_STATE_PRESSED);
            zwp_virtual_keyboard_v1_modifiers(keyboard, 1U << xkb_keymap_mod_get_index(map, XKB_MOD_NAME_CTRL), 0, 0, 0);
        }
        zwp_virtual_keyboard_v1_key(keyboard, millis(), value, WL_KEYBOARD_KEY_STATE_PRESSED);
        wl_display_roundtrip(display);
        pause_ms(argc >= 5 ? strtoul(argv[4], NULL, 10) : 60);
        zwp_virtual_keyboard_v1_key(keyboard, millis(), value, WL_KEYBOARD_KEY_STATE_RELEASED);
        if (!strcmp(argv[2], "ctrl-key")) {
            zwp_virtual_keyboard_v1_key(keyboard, millis(), KEY_LEFTCTRL, WL_KEYBOARD_KEY_STATE_RELEASED);
            zwp_virtual_keyboard_v1_modifiers(keyboard, 0, 0, 0, 0);
        }
        wl_display_roundtrip(display);
        zwp_virtual_keyboard_v1_destroy(keyboard);
        xkb_keymap_unref(map); xkb_context_unref(context);
    } else {
        if (!pointer_manager) fail("Virtual pointer protocol unavailable");
        struct zwlr_virtual_pointer_v1* pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(pointer_manager, seat);
        if (!strcmp(argv[2], "click")) {
            zwlr_virtual_pointer_v1_button(pointer, millis(), value, WL_POINTER_BUTTON_STATE_PRESSED);
            zwlr_virtual_pointer_v1_frame(pointer);
            wl_display_roundtrip(display);
            pause_ms(argc >= 5 ? strtoul(argv[4], NULL, 10) : 60);
            zwlr_virtual_pointer_v1_button(pointer, millis(), value, WL_POINTER_BUTTON_STATE_RELEASED);
        } else if (!strcmp(argv[2], "scroll")) {
            zwlr_virtual_pointer_v1_axis_source(pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
            zwlr_virtual_pointer_v1_axis_discrete(pointer, millis(), WL_POINTER_AXIS_VERTICAL_SCROLL,
                wl_fixed_from_int((int)value), value ? 1 : 0);
        } else if (!strcmp(argv[2], "move") && argc >= 7) {
            zwlr_virtual_pointer_v1_motion_absolute(pointer, millis(), value, strtoul(argv[4], NULL, 10),
                strtoul(argv[5], NULL, 10), strtoul(argv[6], NULL, 10));
        } else fail("Unknown input command");
        zwlr_virtual_pointer_v1_frame(pointer);
        wl_display_roundtrip(display);
        zwlr_virtual_pointer_v1_destroy(pointer);
    }
    wl_display_roundtrip(display);
}

int main(int argc, char** argv) {
    const char* socket = getenv("WAYLAND_DISPLAY");
    if (!socket || strncmp(socket, "hyprcosmos-test-", 16))
        fail("Refusing input outside an explicitly named hyprcosmos-test- Wayland socket");
    setvbuf(stdout, NULL, _IOLBF, 0);
    display = wl_display_connect(NULL);
    if (!display) fail("Could not connect to dedicated test compositor");
    struct wl_registry* globals = wl_display_get_registry(display);
    wl_registry_add_listener(globals, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    if (argc >= 2 && !strcmp(argv[1], "--input")) {
        inject(argc, argv);
        wl_display_disconnect(display);
        return 0;
    }
    if (!compositor || !shm || !shell) fail("Required surface protocols unavailable");
    if (argc >= 3) variant = atoi(argv[2]);
    surface = wl_compositor_create_surface(compositor);
    struct xdg_surface* xdg = xdg_wm_base_get_xdg_surface(shell, surface);
    xdg_surface_add_listener(xdg, &surface_listener, NULL);
    struct xdg_toplevel* top = xdg_surface_get_toplevel(xdg);
    xdg_toplevel_add_listener(top, &toplevel_listener, NULL);
    xdg_toplevel_set_app_id(top, "cosmic-probe");
    xdg_toplevel_set_title(top, variant % 2 ? "Cosmic probe B" : "Cosmic probe A");
    wl_surface_commit(surface);
    unsigned last_paint = millis();
    while (running) {
        wl_display_dispatch_pending(display);
        wl_display_flush(display);
        struct pollfd p = {.fd = wl_display_get_fd(display), .events = POLLIN};
        const int ready = poll(&p, 1, 50);
        if (ready > 0 && (p.revents & POLLIN) && wl_display_dispatch(display) < 0) break;
        if (ready < 0 && errno != EINTR) break;
        if (millis() - last_paint >= 250) { paint(); last_paint = millis(); }
    }
    wl_display_disconnect(display);
    return 0;
}
