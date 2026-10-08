#define _GNU_SOURCE
#include <wayland-client.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "toplevel-export-client-protocol.h"

// Exercise real SHARE_WINDOW sessions using the compositor's public export
// protocol. Bind v1 and use the low 32 bits of hyprctl's window address, exactly
// as Hyprland 0.56.2's CViewQuery::byHandle() does. No compositor mutation.
static struct wl_display* display;
static struct wl_shm* shm;
static struct hyprland_toplevel_export_manager_v1* manager;
static struct wl_buffer* buffer;
static void* pixels;
static size_t pixel_size;
static uint32_t format, width, height, stride;
static int completed, failed;
static unsigned frames;

static uint64_t millis(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void fail(const char* message) { fprintf(stderr, "%s\n", message); exit(1); }
static void registry(void* data, struct wl_registry* r, uint32_t id, const char* name, uint32_t version) {
    if (!strcmp(name, "wl_shm")) shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
    if (!strcmp(name, "hyprland_toplevel_export_manager_v1"))
        manager = wl_registry_bind(r, id, &hyprland_toplevel_export_manager_v1_interface, 1);
}
static void global_remove(void* data, struct wl_registry* r, uint32_t id) {}
static const struct wl_registry_listener registry_listener = {registry, global_remove};
static void frame_buffer(void* data, struct hyprland_toplevel_export_frame_v1* frame,
                         uint32_t f, uint32_t w, uint32_t h, uint32_t s) {
    format = f; width = w; height = h; stride = s;
}
static void damage(void* data, struct hyprland_toplevel_export_frame_v1* frame,
                   uint32_t x, uint32_t y, uint32_t w, uint32_t h) {}
static void flags(void* data, struct hyprland_toplevel_export_frame_v1* frame, uint32_t value) {}
static void ready(void* data, struct hyprland_toplevel_export_frame_v1* frame,
                  uint32_t hi, uint32_t lo, uint32_t ns) {
    // SHM is newly zeroed for every capture. A blank first frame caused by a
    // render detour can still receive ready, so inspect the actual copied image.
    unsigned nonblack = 0, color_count = 0;
    uint32_t colors[8] = {0};
    const unsigned step_x = width / 64 ? width / 64 : 1;
    const unsigned step_y = height / 64 ? height / 64 : 1;
    for (unsigned y = 0; y < height; y += step_y) {
        const uint32_t* row = (const uint32_t*)((const uint8_t*)pixels + (size_t)y * stride);
        for (unsigned x = 0; x < width; x += step_x) {
            const uint32_t rgb = row[x] & 0x00ffffff;
            if (rgb) ++nonblack;
            unsigned index = 0;
            while (index < color_count && colors[index] != rgb) ++index;
            if (index == color_count && color_count < 8) colors[color_count++] = rgb;
        }
    }
    if (nonblack < 10 || color_count < 3)
        fail("Window export ready contained a blank or uniform frame instead of the probe checkerboard");
    completed = 1;
    printf("{\"event\":\"window_frame\",\"frames\":%u,\"width\":%u,\"height\":%u,\"nonblack_samples\":%u,\"colors\":%u}\n", ++frames, width, height, nonblack, color_count);
}
static void frame_failed(void* data, struct hyprland_toplevel_export_frame_v1* frame) { completed = failed = 1; }
static void dmabuf(void* data, struct hyprland_toplevel_export_frame_v1* frame,
                   uint32_t f, uint32_t w, uint32_t h) {}
static void buffer_done(void* data, struct hyprland_toplevel_export_frame_v1* frame) {
    pixel_size = (size_t)stride * height;
    if (!width || !height || stride < width * 4 || pixel_size > 64 * 1024 * 1024)
        fail("Unexpected or oversized SHM export format");
    const int fd = memfd_create("hyprcosmos-window-share-probe", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, pixel_size)) fail("Could not allocate export buffer");
    pixels = mmap(NULL, pixel_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) fail("Could not map export buffer");
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, pixel_size);
    buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);
    hyprland_toplevel_export_frame_v1_copy(frame, buffer, 1);
}
static const struct hyprland_toplevel_export_frame_v1_listener frame_listener = {
    .buffer = frame_buffer, .damage = damage, .flags = flags, .ready = ready,
    .failed = frame_failed, .linux_dmabuf = dmabuf, .buffer_done = buffer_done,
};

int main(int argc, char** argv) {
    const char* socket = getenv("WAYLAND_DISPLAY");
    if (!socket || strncmp(socket, "hyprcosmos-test-", 16))
        fail("Refusing export outside an explicitly named hyprcosmos-test- socket");
    if (argc != 2) fail("Usage: share-probe 0xWINDOW_ADDRESS (dedicated nested socket only)");
    char* end = NULL;
    errno = 0;
    const uint64_t address = strtoull(argv[1], &end, 0);
    if (errno || !address || !end || *end) fail("Invalid window address");
    setvbuf(stdout, NULL, _IOLBF, 0);
    display = wl_display_connect(NULL);
    if (!display) fail("Could not connect to dedicated test compositor");
    struct wl_registry* globals = wl_display_get_registry(display);
    wl_registry_add_listener(globals, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!shm || !manager) fail("Required Hyprland window export protocol unavailable");
    const uint64_t finish = millis() + 120000;
    while (millis() < finish) {
        completed = failed = 0;
        width = height = stride = 0;
        struct hyprland_toplevel_export_frame_v1* frame =
            hyprland_toplevel_export_manager_v1_capture_toplevel(manager, 0, (uint32_t)address);
        hyprland_toplevel_export_frame_v1_add_listener(frame, &frame_listener, NULL);
        const uint64_t deadline = millis() + 5000;
        while (!completed && millis() < deadline) {
            if (wl_display_dispatch_pending(display) < 0) fail("Export dispatch failed");
            wl_display_flush(display);
            if (completed) break;
            struct pollfd p = {.fd = wl_display_get_fd(display), .events = POLLIN};
            const int result = poll(&p, 1, 100);
            if (result > 0 && (p.revents & POLLIN) && wl_display_dispatch(display) < 0)
                fail("Export connection closed");
            if (result < 0 && errno != EINTR) fail("Export poll failed");
        }
        if (!completed || failed) fail("Window export failed or timed out");
        hyprland_toplevel_export_frame_v1_destroy(frame);
        if (buffer) { wl_buffer_destroy(buffer); buffer = NULL; }
        if (pixels) { munmap(pixels, pixel_size); pixels = NULL; }
        wl_display_flush(display);
        const struct timespec pause = {.tv_nsec = 100000000};
        nanosleep(&pause, NULL);
    }
    hyprland_toplevel_export_manager_v1_destroy(manager);
    wl_display_disconnect(display);
    return 0;
}
