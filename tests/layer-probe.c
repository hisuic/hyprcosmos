#define _POSIX_C_SOURCE 200809L
#include <gtk/gtk.h>
#include <gtk-layer-shell.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Genuine layer-shell surfaces, never windows merely styled to resemble a bar.
// The socket guard matches wayland-probe.c and prevents accidental live tests.
static GtkWidget* area;
static unsigned sequence;
static int overlay;

static void fail(const char* message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}

static gboolean draw(GtkWidget* widget, cairo_t* cr, gpointer data) {
    if (overlay) cairo_set_source_rgb(cr, 240.0 / 255, 16.0 / 255, 224.0 / 255);
    else cairo_set_source_rgb(cr, 16.0 / 255, 240.0 / 255, 48.0 / 255);
    cairo_paint(cr);
    printf("{\"event\":\"draw\",\"sequence\":%u}\n", ++sequence);
    return TRUE;
}

static gboolean redraw(gpointer data) {
    gtk_widget_queue_draw(area);
    return G_SOURCE_CONTINUE;
}

static gboolean button(GtkWidget* widget, GdkEventButton* event, gpointer data) {
    printf("{\"event\":\"button\",\"button\":%u,\"state\":%u}\n",
           event->button, event->type == GDK_BUTTON_RELEASE ? 0 : 1);
    return TRUE;
}

static gboolean motion(GtkWidget* widget, GdkEventMotion* event, gpointer data) {
    printf("{\"event\":\"motion\",\"x\":%.2f,\"y\":%.2f}\n", event->x, event->y);
    return TRUE;
}

static gboolean key(GtkWidget* widget, GdkEventKey* event, gpointer data) {
    printf("{\"event\":\"key\",\"key\":%u,\"state\":%u}\n",
           event->hardware_keycode, event->type == GDK_KEY_RELEASE ? 0 : 1);
    return TRUE;
}

static void allocation(GtkWidget* widget, GtkAllocation* box, gpointer data) {
    printf("{\"event\":\"configure\",\"width\":%d,\"height\":%d}\n", box->width, box->height);
}

static int check_shot(const char* path, const char* expected) {
    png_image image = {0};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path)) fail("Could not open screenshot PNG");
    image.format = PNG_FORMAT_RGB;
    if (image.width < 240 || image.height < 160) fail("Screenshot is too small for layer probes");
    unsigned char* pixels = malloc(PNG_IMAGE_SIZE(image));
    if (!pixels || !png_image_finish_read(&image, NULL, pixels, 0, NULL)) fail("Could not decode screenshot PNG");
    unsigned top = 0, bottom = 0, top_total = 0, bottom_total = 0;
    // Stay clear of the cursor, GTK edges and compositor output boundaries.
    for (unsigned y = 5; y < 43; ++y) {
        for (unsigned x = 40; x < image.width - 40; ++x) {
            const unsigned char* p = pixels + ((size_t)y * image.width + x) * 3;
            top += p[1] > 210 && p[0] < 45 && p[2] < 75;
            ++top_total;
        }
    }
    for (unsigned y = image.height - 76; y < image.height - 8; ++y) {
        for (unsigned x = image.width - 172; x < image.width - 8; ++x) {
            const unsigned char* p = pixels + ((size_t)y * image.width + x) * 3;
            bottom += p[0] > 210 && p[1] < 45 && p[2] > 190;
            ++bottom_total;
        }
    }
    const double top_ratio = (double)top / top_total, bottom_ratio = (double)bottom / bottom_total;
    const int visible = !strcmp(expected, "visible"), hidden = !strcmp(expected, "hidden");
    if (!visible && !hidden) fail("Expected screenshot mode must be visible or hidden");
    const int good = visible ? top_ratio > .96 && bottom_ratio > .96 : top_ratio < .005 && bottom_ratio < .005;
    printf("%s: %s top_green=%.5f overlay_magenta=%.5f expected=%s\n",
           good ? "PASS" : "FAIL", path, top_ratio, bottom_ratio, expected);
    free(pixels);
    png_image_free(&image);
    return good ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 4 && !strcmp(argv[1], "--check-shot")) return check_shot(argv[2], argv[3]);
    if (argc < 3 || argc > 4 || (strcmp(argv[1], "top") && strcmp(argv[1], "overlay")))
        fail("Usage: layer-probe top|overlay NAMESPACE [none|exclusive|on-demand], or --check-shot PNG visible|hidden");
    const char* socket = getenv("WAYLAND_DISPLAY");
    if (!socket || strncmp(socket, "hyprcosmos-test-", 16))
        fail("Refusing layer surface outside a dedicated hyprcosmos-test- socket");
    setvbuf(stdout, NULL, _IOLBF, 0);
    overlay = !strcmp(argv[1], "overlay");
    GtkLayerShellKeyboardMode mode = GTK_LAYER_SHELL_KEYBOARD_MODE_NONE;
    if (argc == 4) {
        if (!strcmp(argv[3], "exclusive")) mode = GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE;
        else if (!strcmp(argv[3], "on-demand")) mode = GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND;
        else if (strcmp(argv[3], "none")) fail("Invalid keyboard mode");
    }
    // GTK's argument parser must not reinterpret our layer/namespace arguments.
    gtk_init(NULL, NULL);
    if (!gtk_layer_is_supported()) fail("The selected compositor has no layer-shell protocol");
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_layer_init_for_window(GTK_WINDOW(window));
    gtk_layer_set_namespace(GTK_WINDOW(window), argv[2]);
    gtk_layer_set_layer(GTK_WINDOW(window), overlay ? GTK_LAYER_SHELL_LAYER_OVERLAY : GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(window), mode);
    if (overlay) {
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        gtk_layer_set_exclusive_zone(GTK_WINDOW(window), -1);
        gtk_widget_set_size_request(window, 180, 84);
    } else {
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        gtk_layer_set_exclusive_zone(GTK_WINDOW(window), 48);
        gtk_widget_set_size_request(window, 1, 48);
    }
    area = gtk_drawing_area_new();
    gtk_widget_set_app_paintable(area, TRUE);
    gtk_widget_set_can_focus(area, TRUE);
    gtk_widget_add_events(area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    gtk_container_add(GTK_CONTAINER(window), area);
    g_signal_connect(area, "draw", G_CALLBACK(draw), NULL);
    g_signal_connect(area, "size-allocate", G_CALLBACK(allocation), NULL);
    g_signal_connect(area, "button-press-event", G_CALLBACK(button), NULL);
    g_signal_connect(area, "button-release-event", G_CALLBACK(button), NULL);
    g_signal_connect(area, "motion-notify-event", G_CALLBACK(motion), NULL);
    g_signal_connect(window, "key-press-event", G_CALLBACK(key), NULL);
    g_signal_connect(window, "key-release-event", G_CALLBACK(key), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    gtk_widget_show_all(window);
    gtk_widget_grab_focus(area);
    g_timeout_add(100, redraw, NULL);
    printf("{\"event\":\"ready\",\"namespace\":\"%s\",\"keyboard_mode\":%d}\n", argv[2], mode);
    gtk_main();
    return 0;
}
