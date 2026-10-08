#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace cosmic {

namespace desktop_ui_detail {

constexpr unsigned char asciiLower(unsigned char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
}

constexpr bool containsAsciiInsensitive(std::string_view value, std::string_view token) noexcept {
    if (token.empty() || value.size() < token.size()) return false;
    const std::size_t last = value.size() - token.size();
    for (std::size_t start = 0; start <= last; ++start) {
        std::size_t matched = 0;
        while (matched < token.size() &&
               asciiLower(static_cast<unsigned char>(value[start + matched])) ==
                   asciiLower(static_cast<unsigned char>(token[matched])))
            ++matched;
        if (matched == token.size()) return true;
    }
    return false;
}

template <std::size_t Count>
constexpr bool containsAny(std::string_view value, const std::array<std::string_view, Count>& tokens) noexcept {
    for (const auto token : tokens)
        if (containsAsciiInsensitive(value, token)) return true;
    return false;
}

} // namespace desktop_ui_detail

// Conservative, allocation-free naming heuristics: match fixed ASCII tokens
// anywhere in the name, independently of case or the process locale. These are
// not a security boundary; protocol lock state, above-lock surfaces, keyboard
// interactivity and input grabs must be checked independently by the caller.
constexpr bool protectedDesktopNamespace(std::string_view name) noexcept {
    constexpr std::array<std::string_view, 9> tokens = {
        "hyprlock", "swaylock", "gtklock", "waylock", "session-lock",
        "polkit", "authentication", "permission", "pinentry",
    };
    return desktop_ui_detail::containsAny(name, tokens);
}

constexpr bool authenticationWindowClass(std::string_view name) noexcept {
    constexpr std::array<std::string_view, 4> tokens = {
        "polkit", "pinentry", "auth-agent", "authentication-agent",
    };
    return desktop_ui_detail::containsAny(name, tokens);
}

} // namespace cosmic
