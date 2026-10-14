#pragma once

#include "physics.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cosmic {

// Presentation-only data. No titles, compositor references or input surfaces are
// needed by the card renderer, and this ID is never part of action hit-testing.
struct ObservationCard {
    uint64_t id = 0;
    Vec2 cursor; // Absolute logical desktop coordinates, including output origin.
    std::vector<std::string> lines;
};

inline constexpr std::size_t observationMaxLines = 8;
inline constexpr std::size_t observationMaxColumns = 52;
inline constexpr std::string_view observationHeading = "COSMIC OBSERVATORY";

inline std::vector<std::string> sanitizeObservationLines(const std::vector<std::string>& input) {
    std::vector<std::string> result;
    result.reserve(std::min(input.size(), observationMaxLines));
    for (std::size_t index = 0; index < std::min(input.size(), observationMaxLines); ++index) {
        const auto& original = input[index];
        std::string line;
        line.reserve(std::min(original.size(), observationMaxColumns));
        for (unsigned char character : original) {
            if (line.size() == observationMaxColumns) break;
            // Never interpret embedded newlines, ANSI escapes or font markup.
            line += character >= 32 && character <= 126 ? char(character) : '?';
        }
        if (!line.empty()) result.push_back(std::move(line));
    }
    return result;
}

inline ObservationCard sanitizeObservationCard(ObservationCard card) {
    if (card.id == 0 || !std::isfinite(card.cursor.x) || !std::isfinite(card.cursor.y)) return {};
    card.lines = sanitizeObservationLines(card.lines);
    if (card.lines.empty()) return {};
    return card;
}

inline std::string frameObservationLines(const std::vector<std::string>& input) {
    const auto lines = sanitizeObservationLines(input);
    if (lines.empty()) return {};
    std::size_t width = observationHeading.size();
    for (const auto& line : lines) width = std::max(width, line.size());
    const std::string border = "+-" + std::string(width, '-') + "-+";
    const auto row = [width](std::string_view line) {
        return "| " + std::string(line) + std::string(width - line.size(), ' ') + " |";
    };
    std::string text = border + '\n' + row(observationHeading) + '\n' + border;
    for (const auto& line : lines) text += '\n' + row(line);
    return text + '\n' + border;
}

struct ObservationLayout {
    Vec2 panelOrigin; // Absolute logical coordinates, never physical pixels.
    Vec2 panelSize;
    Vec2 textOrigin;
    Vec2 textSize;
};

inline bool observationContainsCursor(const Region& monitor, Vec2 cursor) {
    return std::isfinite(cursor.x) && std::isfinite(cursor.y) &&
           std::isfinite(monitor.x) && std::isfinite(monitor.y) &&
           std::isfinite(monitor.width) && std::isfinite(monitor.height) &&
           std::isfinite(monitor.x + monitor.width) && std::isfinite(monitor.y + monitor.height) &&
           monitor.width > 0.0 && monitor.height > 0.0 &&
           cursor.x >= monitor.x && cursor.y >= monitor.y &&
           cursor.x < monitor.x + monitor.width && cursor.y < monitor.y + monitor.height;
}

// Only the output containing the cursor draws a card. Use transformed logical
// output dimensions: scale and rotation are applied by the existing projection.
inline std::optional<ObservationLayout> observationCardLayout(const Region& monitor, Vec2 cursor,
                                                             Vec2 texturePixels, double pixelScale) {
    if (!observationContainsCursor(monitor, cursor) ||
        !std::isfinite(pixelScale) || pixelScale <= 0.0 ||
        !std::isfinite(texturePixels.x) || !std::isfinite(texturePixels.y) ||
        texturePixels.x <= 0.0 || texturePixels.y <= 0.0) return std::nullopt;
    const double margin = std::min({12.0, monitor.width * 0.05, monitor.height * 0.05});
    const Vec2 available{monitor.width - margin * 2.0, monitor.height - margin * 2.0};
    const double padding = std::min({10.0, available.x * 0.04, available.y * 0.04});
    const Vec2 original = texturePixels / pixelScale;
    if (!std::isfinite(original.x) || !std::isfinite(original.y)) return std::nullopt;
    const double fit = std::min({1.0, (available.x - padding * 2.0) / original.x,
                                (available.y - padding * 2.0) / original.y});
    if (!std::isfinite(fit) || fit <= 0.0) return std::nullopt;
    ObservationLayout layout;
    layout.textSize = original * fit;
    layout.panelSize = layout.textSize + Vec2{padding * 2.0, padding * 2.0};
    const Vec2 minimum{monitor.x + margin, monitor.y + margin};
    const Vec2 maximum{monitor.x + monitor.width - margin - layout.panelSize.x,
                       monitor.y + monitor.height - margin - layout.panelSize.y};
    constexpr double offset = 24.0;
    layout.panelOrigin = cursor + Vec2{offset, offset};
    if (layout.panelOrigin.x > maximum.x) layout.panelOrigin.x = cursor.x - offset - layout.panelSize.x;
    if (layout.panelOrigin.y > maximum.y) layout.panelOrigin.y = cursor.y - offset - layout.panelSize.y;
    layout.panelOrigin.x = std::clamp(layout.panelOrigin.x, minimum.x, std::max(minimum.x, maximum.x));
    layout.panelOrigin.y = std::clamp(layout.panelOrigin.y, minimum.y, std::max(minimum.y, maximum.y));
    layout.textOrigin = layout.panelOrigin + Vec2{padding, padding};
    return layout;
}

} // namespace cosmic
