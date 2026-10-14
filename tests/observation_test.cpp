#include "observation.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace {

int assertions = 0;

void check(bool condition, std::string_view description) {
    ++assertions;
    if (!condition) throw std::runtime_error(std::string(description));
}

bool near(double a, double b) { return std::abs(a - b) < 1e-8; }

void checkContained(const cosmic::Region& monitor, const cosmic::ObservationLayout& card) {
    check(card.panelOrigin.x >= monitor.x && card.panelOrigin.y >= monitor.y, "panel starts inside output");
    check(card.panelOrigin.x + card.panelSize.x <= monitor.x + monitor.width + 1e-8, "panel right inside output");
    check(card.panelOrigin.y + card.panelSize.y <= monitor.y + monitor.height + 1e-8, "panel bottom inside output");
    check(card.textOrigin.x >= card.panelOrigin.x && card.textOrigin.y >= card.panelOrigin.y, "text starts inside panel");
    check(card.textOrigin.x + card.textSize.x <= card.panelOrigin.x + card.panelSize.x + 1e-8, "text right inside panel");
    check(card.textOrigin.y + card.textSize.y <= card.panelOrigin.y + card.panelSize.y + 1e-8, "text bottom inside panel");
    check(card.textSize.x > 0.0 && card.textSize.y > 0.0, "text has positive dimensions");
}

void sanitation() {
    using namespace cosmic;
    check(sanitizeObservationLines({}).empty(), "no lines stays empty");
    check(sanitizeObservationLines({"", ""}).empty(), "empty lines are hidden");
    const auto controls = sanitizeObservationLines({"safe\n\r\t\033[31m", std::string("A\0B", 3), "\xff"});
    check(controls == std::vector<std::string>{"safe????[31m", "A?B", "?"}, "all non-ASCII/control bytes replaced");
    const auto longLines = sanitizeObservationLines(std::vector<std::string>(20, std::string(1000, 'A')));
    check(longLines.size() == observationMaxLines, "input lines bounded");
    for (const auto& line : longLines) check(line.size() == observationMaxColumns, "columns bounded");
    std::vector<std::string> emptyPrefix(8);
    emptyPrefix.push_back("ignored");
    check(sanitizeObservationLines(emptyPrefix).empty(), "empty input prefix cannot bypass line bound");
    check(sanitizeObservationCard({0, {10, 10}, {"NAME: A"}}).id == 0, "zero ID hidden");
    check(sanitizeObservationCard({1, {10, 10}, {}}).id == 0, "empty card hidden");
    check(sanitizeObservationCard({1, {std::numeric_limits<double>::infinity(), 10}, {"A"}}).id == 0,
          "infinite cursor hidden");
    check(sanitizeObservationCard({1, {10, std::numeric_limits<double>::quiet_NaN()}, {"A"}}).id == 0,
          "NaN cursor hidden");
    check(sanitizeObservationCard({42, {-300, 900}, {"NAME: test"}}).id == 42, "negative desktop origins accepted");
    check(frameObservationLines({}).empty(), "empty card has no frame");
    const auto frame = frameObservationLines({"NAME: test", "ORIGIN: terminal", std::string(52, '*')});
    check(frame.find("COSMIC OBSERVATORY") != std::string::npos, "heading included");
    check(frame.find("| NAME: test") != std::string::npos, "fact row included");
    std::istringstream rows(frame);
    std::string row;
    int rowCount = 0;
    while (std::getline(rows, row)) {
        ++rowCount;
        check(row.size() == 56, "ASCII frame rows have equal width");
        for (unsigned char character : row)
            check(character >= 32 && character <= 126, "framed text is printable ASCII");
    }
    check(rowCount == 7, "frame adds exactly four rows");
}

void placement() {
    using namespace cosmic;
    const Region ordinary{.x = 0, .y = 0, .width = 1920, .height = 1080};
    auto card = observationCardLayout(ordinary, {100, 100}, {480, 220}, 1);
    check(card.has_value(), "ordinary layout exists");
    check(near(card->panelOrigin.x, 124) && near(card->panelOrigin.y, 124), "near cursor with offset");
    check(near(card->textSize.x, 480) && near(card->textSize.y, 220), "unscaled text retained");
    checkContained(ordinary, *card);
    card = observationCardLayout(ordinary, {1900, 1060}, {480, 220}, 1);
    check(card.has_value(), "corner layout exists");
    check(card->panelOrigin.x + card->panelSize.x < 1900 && card->panelOrigin.y + card->panelSize.y < 1060,
          "card flips above and left of bottom-right cursor");
    checkContained(ordinary, *card);
    check(!observationCardLayout(ordinary, {1920, 500}, {480, 220}, 1), "right edge belongs to adjacent output");
    check(!observationCardLayout(ordinary, {500, 1080}, {480, 220}, 1), "bottom edge belongs to adjacent output");
    check(!observationCardLayout(ordinary, {-1, 500}, {480, 220}, 1), "other output cursor hidden");
    check(!observationCardLayout(ordinary, {500, 500}, {0, 220}, 1), "empty texture hidden");
    check(!observationCardLayout(ordinary, {500, 500}, {480, 220}, 0), "zero pixel scale rejected");
    check(!observationCardLayout(ordinary, {500, 500}, {480, 220}, -2), "negative pixel scale rejected");
    check(!observationCardLayout(ordinary, {500, 500}, {480, 220}, std::numeric_limits<double>::quiet_NaN()),
          "NaN scale rejected");
    check(!observationCardLayout(ordinary, {500, 500}, {480, 220}, std::numeric_limits<double>::min()),
          "overflowing logical texture rejected");
    check(!observationCardLayout({.width = 0, .height = 100}, {0, 0}, {480, 220}, 1), "empty monitor rejected");
    check(!observationCardLayout(ordinary, {500, 500}, {480, std::numeric_limits<double>::infinity()}, 1),
          "infinite texture rejected");
    for (const Region monitor : {ordinary,
             Region{.x = -1920, .y = -200, .width = 1920, .height = 1080},
             Region{.x = 1920, .y = 80, .width = 1080, .height = 1920}, // Rotated logical output.
             Region{.x = 100, .y = -100, .width = 160, .height = 80},
             Region{.x = -2, .y = -3, .width = 1, .height = 1}}) {
        for (double scale : {0.75, 1.0, 1.25, 1.5, 2.0, 4.0}) {
            for (double x : {0.0, 0.01, 0.5, 0.99}) {
                for (double y : {0.0, 0.01, 0.5, 0.99}) {
                    card = observationCardLayout(monitor, {monitor.x + monitor.width * x, monitor.y + monitor.height * y},
                                                 {480 * scale, 220 * scale}, scale);
                    check(card.has_value(), "all edge/scale/origin combinations place card");
                    checkContained(monitor, *card);
                }
            }
        }
    }
    const auto one = observationCardLayout(ordinary, {100, 100}, {480, 220}, 1);
    const auto two = observationCardLayout(ordinary, {100, 100}, {960, 440}, 2);
    check(near(one->panelSize.x, two->panelSize.x) && near(one->panelSize.y, two->panelSize.y),
          "DPI scales textures without changing logical panel size");
}

} // namespace

int main() {
    try {
        sanitation();
        placement();
        std::cout << "Observation card: " << assertions << " checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << "Observation card: " << error.what() << '\n';
        return 1;
    }
}
