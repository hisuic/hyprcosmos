#define _POSIX_C_SOURCE 200809L
#include <wayland-client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "input-method-client-protocol.h"

// A real input-method-v2 keyboard grab scoped to the guarded child socket.
// There is no DBus name, daemon, system IME interaction or parent-seat binding.
static struct wl_seat* seat;
static struct zwp_input_method_manager_v2* manager;
static void fail(const char* message) { fprintf(stderr, "%s\n", message); exit(1); }
static void capabilities(void* data, struct wl_seat* seat, uint32_t capabilities) {}
static const struct wl_seat_listener seat_listener = {.capabilities = capabilities};

static void registry(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    if (!strcmp(interface, "wl_seat")) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
    if (!strcmp(interface, "zwp_input_method_manager_v2"))
        manager = wl_registry_bind(registry, name, &zwp_input_method_manager_v2_interface, 1);
}
static void removed(void* data, struct wl_registry* registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {registry, removed};

static void activate(void* data, struct zwp_input_method_v2* ime) { puts("{\"event\":\"activate\"}"); }
static void deactivate(void* data, struct zwp_input_method_v2* ime) { puts("{\"event\":\"deactivate\"}"); }
static void surrounding(void* data, struct zwp_input_method_v2* ime, const char* text, uint32_t cursor, uint32_t anchor) {}
static void cause(void* data, struct zwp_input_method_v2* ime, uint32_t cause) {}
static void content(void* data, struct zwp_input_method_v2* ime, uint32_t hint, uint32_t purpose) {}
static void done(void* data, struct zwp_input_method_v2* ime) { puts("{\"event\":\"done\"}"); }
static void unavailable(void* data, struct zwp_input_method_v2* ime) { fail("Child input method is unavailable, possibly another fixture is already attached"); }
static const struct zwp_input_method_v2_listener ime_listener = {
    .activate = activate, .deactivate = deactivate, .surrounding_text = surrounding,
    .text_change_cause = cause, .content_type = content, .done = done, .unavailable = unavailable,
};

static void keymap(void* data, struct zwp_input_method_keyboard_grab_v2* grab, uint32_t format, int32_t fd, uint32_t size) {
    close(fd);
    puts("{\"event\":\"keymap\"}");
}
static void key(void* data, struct zwp_input_method_keyboard_grab_v2* grab, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    printf("{\"event\":\"key\",\"key\":%u,\"state\":%u}\n", key, state);
}
static void modifiers(void* data, struct zwp_input_method_keyboard_grab_v2* grab, uint32_t serial, uint32_t depressed,
                      uint32_t latched, uint32_t locked, uint32_t group) {
    printf("{\"event\":\"modifiers\",\"depressed\":%u,\"latched\":%u,\"locked\":%u}\n", depressed, latched, locked);
}
static void repeat(void* data, struct zwp_input_method_keyboard_grab_v2* grab, int32_t rate, int32_t delay) {}
static const struct zwp_input_method_keyboard_grab_v2_listener grab_listener = {
    .keymap = keymap, .key = key, .modifiers = modifiers, .repeat_info = repeat,
};

int main(void) {
    const char* socket = getenv("WAYLAND_DISPLAY");
    if (!socket || strncmp(socket, "hyprcosmos-test-", 16))
        fail("Refusing input method outside a dedicated hyprcosmos-test- socket");
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct wl_display* display = wl_display_connect(NULL);
    if (!display) fail("Could not connect to dedicated child compositor");
    struct wl_registry* globals = wl_display_get_registry(display);
    wl_registry_add_listener(globals, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    if (!seat || !manager) fail("Child compositor has no input-method-v2 protocol or seat");
    struct zwp_input_method_v2* ime = zwp_input_method_manager_v2_get_input_method(manager, seat);
    zwp_input_method_v2_add_listener(ime, &ime_listener, NULL);
    struct zwp_input_method_keyboard_grab_v2* grab = zwp_input_method_v2_grab_keyboard(ime);
    zwp_input_method_keyboard_grab_v2_add_listener(grab, &grab_listener, NULL);
    wl_display_roundtrip(display);
    puts("{\"event\":\"grab_ready\"}");
    while (wl_display_dispatch(display) >= 0) {}
    wl_display_disconnect(display);
    return 0;
}
