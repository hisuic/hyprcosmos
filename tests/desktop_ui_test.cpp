#include "desktop_ui.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

int assertions = 0;

void check(bool condition, std::string_view description) {
    ++assertions;
    if (!condition) throw std::runtime_error(std::string(description));
}

// Keep the production helpers usable without a locale, runtime initialization
// or compositor linkage, including for checks made before plugin setup.
static_assert(cosmic::protectedDesktopNamespace("HyPrLoCk"));
static_assert(cosmic::authenticationWindowClass("PINENTRY-GTK-2"));
static_assert(!cosmic::protectedDesktopNamespace("waybar"));
static_assert(!cosmic::authenticationWindowClass("kitty"));
static_assert(noexcept(cosmic::protectedDesktopNamespace("")));
static_assert(noexcept(cosmic::authenticationWindowClass("")));

void testLayerNamespaces() {
    check(!cosmic::protectedDesktopNamespace(""), "empty namespaces are not classified by a name heuristic");
    for (const auto name : std::array<std::string_view, 9>{
             "hyprlock", "swaylock", "gtklock", "waylock", "session-lock",
             "polkit", "authentication", "permission", "pinentry",
         }) {
        check(cosmic::protectedDesktopNamespace(name), "every protected namespace token is recognized");
        const std::string wrapped = "org.example." + std::string(name) + ".dialog";
        check(cosmic::protectedDesktopNamespace(wrapped), "protected tokens also match namespace variants");
    }
    for (const auto name : std::array<std::string_view, 9>{
             "HyPrLoCk", "SWAYLOCK", "GtkLock", "WayLock", "SESSION-LOCK",
             "PolKit", "AUTHENTICATION", "Permission", "PiNeNtRy",
         })
        check(cosmic::protectedDesktopNamespace(name), "protected namespace matching ignores ASCII case");
    for (const auto name : std::array<std::string_view, 14>{
             "waybar", "mako", "dunst", "swaync", "osd", "eww", "quickshell",
             "hyprpaper", "rofi", "wlogout", "WAYBAR", "MAKO", "SWAYNC", "OSD",
         })
        check(!cosmic::protectedDesktopNamespace(name), "ordinary desktop UI namespaces remain suppressible");
    check(cosmic::protectedDesktopNamespace("prefix-pinentry"), "tokens can occur at the end of a namespace");
    check(cosmic::protectedDesktopNamespace("permission-suffix"), "tokens can occur at the beginning of a namespace");
    check(!cosmic::protectedDesktopNamespace("lock"), "a generic lock substring does not over-classify ordinary UI");
    check(!cosmic::protectedDesktopNamespace("polki"), "an incomplete token is not a protected namespace");
    check(!cosmic::protectedDesktopNamespace("\xFF\x80"), "non-ASCII bytes do not trigger ASCII case folding");
    const std::string longName(65536, 'x');
    check(!cosmic::protectedDesktopNamespace(longName), "long ordinary namespace names remain bounded and valid");
    check(cosmic::protectedDesktopNamespace(longName + "pInEnTrY"), "long names still recognize a trailing protected token");
}

void testWindowClasses() {
    check(!cosmic::authenticationWindowClass(""), "empty window classes are not classified by a name heuristic");
    for (const auto name : std::array<std::string_view, 4>{
             "polkit", "pinentry", "auth-agent", "authentication-agent",
         }) {
        check(cosmic::authenticationWindowClass(name), "every authentication class token is recognized");
        const std::string wrapped = "org.example." + std::string(name) + ".dialog";
        check(cosmic::authenticationWindowClass(wrapped), "authentication tokens also match class variants");
    }
    for (const auto name : std::array<std::string_view, 6>{
             "POLKIT", "PiNeNtRy", "AUTH-AGENT", "Authentication-Agent",
             "org.example.Polkit-Gnome-Authentication-Agent-1", "pinentry-qt",
         })
        check(cosmic::authenticationWindowClass(name), "authentication window class matching ignores ASCII case");
    for (const auto name : std::array<std::string_view, 12>{
             "kitty", "firefox", "Chromium", "code", "waybar", "mako", "dunst",
             "swaync", "osd", "hyprlock", "permission", "auth",
         })
        check(!cosmic::authenticationWindowClass(name), "ordinary or unrelated window classes are not authentication agents");
    check(!cosmic::authenticationWindowClass("polki"), "an incomplete token is not an authentication class");
    check(!cosmic::authenticationWindowClass("\xFF\x80"), "non-ASCII class bytes do not trigger ASCII case folding");
    constexpr std::string_view embeddedNull("prefix\0PINENTRY", sizeof("prefix\0PINENTRY") - 1);
    check(cosmic::authenticationWindowClass(embeddedNull),
          "string views are matched by their explicit length rather than an implicit C-string terminator");
}

} // namespace

int main() {
    try {
        testLayerNamespaces();
        testWindowClasses();
        std::cout << "Desktop UI safety policy: " << assertions << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Desktop UI safety policy: " << error.what() << '\n';
        return 1;
    }
}
