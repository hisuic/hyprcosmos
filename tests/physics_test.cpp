#include "physics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

using namespace cosmic;

namespace {
int assertions = 0;

void check(bool condition, std::string_view description) {
    ++assertions;
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        std::exit(EXIT_FAILURE);
    }
}
bool close(double a, double b, double epsilon = 1e-7) { return std::abs(a - b) <= epsilon; }
double magnitude(Vec2 value) { return std::hypot(value.x, value.y); }
Body body(uint64_t id, Vec2 position, Vec2 velocity = {}) {
    Body result;
    result.id = id;
    result.position = position;
    result.velocity = velocity;
    result.width = 160.0;
    result.height = 100.0;
    return result;
}
const Body& findBody(const Universe& universe, uint64_t id) {
    const auto found = std::find_if(universe.bodies().begin(), universe.bodies().end(),
                                    [&](const Body& current) { return current.id == id; });
    check(found != universe.bodies().end(), "expected physics identity remains available");
    return *found;
}
Config inert() {
    Config config;
    config.cursor_gravity = config.orbit = config.binary = config.collisions = false;
    config.expansion = config.wormholes = false;
    config.stellar_automatic = false;
    config.damping = 0.0;
    return config;
}
void advance(Universe& universe, double seconds, Vec2 cursor = {960.0, 540.0}) {
    const int frames = static_cast<int>(std::ceil(seconds * 120.0));
    for (int index = 0; index < frames; ++index)
        universe.step(1.0 / 120.0, cursor);
}
void checkFinite(const Universe& universe) {
    for (const auto& current : universe.bodies()) {
        check(std::isfinite(current.position.x) && std::isfinite(current.position.y), "positions remain finite");
        check(std::isfinite(current.velocity.x) && std::isfinite(current.velocity.y), "velocities remain finite");
        check(std::isfinite(current.angle) && std::isfinite(current.scale) && std::isfinite(current.stretch) &&
              std::isfinite(current.twist), "render transforms remain finite");
        check(std::isfinite(current.portal_progress) && current.portal_progress >= 0.0 && current.portal_progress <= 1.0 &&
              std::isfinite(current.portal_arc) && std::isfinite(current.portal_entry_duration) &&
              std::isfinite(current.portal_exit_duration), "automatic portal seeds and phase stay finite and bounded");
        check(std::isfinite(current.nova_age) && std::isfinite(current.nova_charge_seconds) &&
              std::isfinite(current.nova_fragment_seconds) && std::isfinite(current.nova_initial_scale) &&
              std::isfinite(current.nova_growth) && std::isfinite(current.nova_camera_zoom),
              "stellar nova seeds and phase remain finite");
        const auto fragments = universe.novaFragments(current);
        check(fragments.size() <= 16, "one stellar source never generates more than 16 fragments");
        for (const auto& fragment : fragments)
            check(std::isfinite(fragment.position.x) && std::isfinite(fragment.position.y) &&
                  std::isfinite(fragment.angle) && std::isfinite(fragment.scale) && fragment.scale >= 0.0 &&
                  std::isfinite(fragment.alpha) && fragment.alpha >= 0.0 && fragment.alpha <= 1.0,
                  "fragment kinematics and opacity stay finite and bounded");
        check(magnitude(current.velocity) <= universe.config().max_speed + 1e-6, "speed cap enforced");
    }
    for (const auto& camera : universe.cameras())
        check(std::isfinite(camera.zoom) && camera.zoom >= 0.02 && camera.zoom <= 1.0, "camera zoom finite and bounded");
    const auto sources = std::count_if(universe.bodies().begin(), universe.bodies().end(),
                                       [](const Body& current) { return current.fragment_grid == 0; });
    check(static_cast<std::size_t>(sources) <= universe.config().max_bodies, "real-window cap is independent of fragment count");
    // A configuration reduction deliberately preserves already-existing pieces.
    // The hard native actor limit still bounds that grandfathered population.
    check(universe.objectCount() <= 128, "normal windows and persistent pieces stay within the hard actor cap");
    check(universe.particles().size() <= universe.config().max_particles, "particle cap enforced");
    check(universe.waves().size() <= 16, "wave cap enforced");
    check(universe.historyFrames() <= universe.historyLimit(), "history cap enforced");
}

void fixedStepAndDeterminism() {
    auto config = inert();
    Universe first(config), second(config);
    first.reset({body(1, {500.0, 400.0}, {30.0, 10.0})}, {Region{}}, {960.0, 540.0});
    second.reset({body(1, {500.0, 400.0}, {30.0, 10.0})}, {Region{}}, {960.0, 540.0});
    for (int index = 0; index < 120; ++index)
        first.step(1.0 / 120.0, {});
    for (int index = 0; index < 60; ++index)
        second.step(1.0 / 60.0, {});
    check(close(first.bodies()[0].position.x, 530.0), "constant velocity uses supplied monotonic time");
    check(close(first.bodies()[0].position.x, second.bodies()[0].position.x), "fixed-step frame-rate independence");
    check(close(first.bodies()[0].angle, second.bodies()[0].angle), "seeded rotations reproducible");
    check(first.historyFrames() == 31, "history sampling obeys configured frequency without rounding drift");
    const Vec2 before = first.bodies()[0].position;
    first.step(86400.0, {});
    check(magnitude(first.bodies()[0].position - before) < 3.0, "day-long stall discards huge integration interval");
    const Vec2 after = first.bodies()[0].position;
    first.step(std::numeric_limits<double>::quiet_NaN(), {});
    first.step(-10.0, {});
    check(close(after.x, first.bodies()[0].position.x), "invalid and backwards deltas ignored");
}

void orbitsAndBinary() {
    auto config = inert();
    config.cursor_gravity = config.orbit = true;
    Universe universe(config);
    const Vec2 center{960.0, 540.0};
    universe.reset({body(1, {1200.0, 540.0})}, {Region{}}, center, 1);
    const auto initial = universe.bodies()[0];
    check(initial.velocity.y > 0.0 && close(initial.velocity.x, 0.0), "orbit seeds tangential velocity");
    check(initial.mass > 1.0, "last focused body has greater mass");
    advance(universe, 60.0, center);
    const double distance = magnitude(universe.bodies()[0].position - center);
    check(distance > 100.0 && distance < 600.0, "cursor orbit remains bounded over a minute");
    const Vec2 velocity_before = universe.bodies()[0].velocity;
    advance(universe, 1.0, {1300.0, 900.0});
    check(magnitude(universe.bodies()[0].velocity - velocity_before) > 1.0, "moving cursor perturbs gravity");

    config = inert();
    config.binary = true;
    config.collisions = false;
    universe.configure(config);
    Body heavy = body(2, {900.0, 540.0});
    heavy.mass = 4.0;
    universe.reset({body(1, {1000.0, 540.0}), heavy}, {Region{}}, center);
    universe.setBinaryPreset();
    check(universe.gravityMode() == GravityMode::Binary, "binary preset selects mutual gravity");
    const auto first = universe.bodies()[0], second = universe.bodies()[1];
    const Vec2 initial_center = (first.position * first.mass + second.position * second.mass) / (first.mass + second.mass);
    const Vec2 initial_momentum = first.velocity * first.mass + second.velocity * second.mass;
    check(magnitude(initial_momentum) < 1e-9, "binary initial momentum balances masses");
    advance(universe, 25.0);
    const auto a = universe.bodies()[0], b = universe.bodies()[1];
    const Vec2 final_center = (a.position * a.mass + b.position * b.mass) / (a.mass + b.mass);
    check(magnitude(final_center - initial_center) < 1e-6, "binary conserves common barycenter");
    check(magnitude(a.position - b.position) > 100.0, "binary does not collapse into coincident bodies");
    const Vec2 before_third = a.velocity;
    check(universe.addBody(body(3, a.position + Vec2{50.0, 0.0}, {20.0, 0.0}), false), "third body can join ongoing simulation");
    advance(universe, 0.5);
    check(magnitude(universe.bodies()[0].velocity - before_third) > 0.1, "third body changes orbit");
}

void collisionAndBoundary() {
    auto config = inert();
    config.collisions = true;
    config.collision_strength = 1.0;
    config.restitution = 0.8;
    Universe universe(config);
    Body a = body(1, {940.0, 540.0}, {40.0, 0.0});
    Body b = body(2, {970.0, 540.0}, {-40.0, 0.0});
    universe.reset({a, b}, {Region{}}, {960.0, 540.0});
    universe.step(1.0 / 120.0, {});
    check(universe.bodies()[0].velocity.x < 0.0 && universe.bodies()[1].velocity.x > 0.0, "colliding bodies rebound");
    check(close(universe.bodies()[0].velocity.x + universe.bodies()[1].velocity.x, 0.0), "equal-mass collision preserves momentum");
    universe.reset({body(1, {1900.0, 540.0}, {100.0, 0.0})}, {Region{}}, {});
    universe.step(1.0 / 120.0, {});
    check(universe.bodies()[0].velocity.x < 0.0, "virtual boundary reflects outward velocity");
}

void wormholesAndRegions() {
    auto config = inert();
    config.wormholes = true;
    Universe universe(config);
    Region main{42, 0.0, 0.0, 1920.0, 1080.0};
    Region alternate{1000042, 0.0, 0.0, 1920.0, 1080.0};
    universe.reset({}, {main, alternate}, {});
    const auto entrance = universe.wormholes().front();
    const auto exit = universe.wormholes()[entrance.partner];
    Body traveler = body(1, entrance.position, {120.0, 20.0});
    traveler.region = main.id;
    universe.reset({traveler}, {main, alternate}, {});
    universe.step(1.0 / 120.0, {});
    const auto entered = universe.bodies()[0];
    check(entered.region == main.id && entered.portal_progress > 0.0 && !entered.portal_emerging,
          "automatic portal entry starts visibly in its source region");
    check(close(entered.scale, 0.42) && magnitude(entered.position - entrance.position) < 1e-7,
          "automatic entry never instantly teleports or shrinks its first frame");
    int frames = 0;
    for (; frames < 600 && universe.bodies()[0].portal_progress > 0.0; ++frames)
        universe.step(1.0 / 120.0, {});
    const auto transferred = universe.bodies()[0];
    check(frames > 200 && frames < 400, "default portal transfer lasts a visibly cinematic interval");
    check(transferred.region == alternate.id, "portal changes virtual region with arbitrary region id");
    check(close(magnitude(transferred.velocity), magnitude(traveler.velocity)), "portal preserves speed");
    check(transferred.cooldown > 1.9, "portal sets repeat-transfer cooldown");
    check(magnitude(transferred.position - exit.position) > exit.radius, "exit emerges outside portal mouth");
    advance(universe, 0.5);
    check(universe.bodies()[0].region == alternate.id, "cooldown prevents immediate round trip");
    const auto screen = universe.worldToScreen(universe.bodies()[0].position, alternate.id);
    check(universe.hitTest(screen, main.id) == 0, "hidden virtual region cannot be selected");
    check(universe.hitTest(screen, alternate.id) == 1, "visible alternate region can be selected");
    const Vec2 world = universe.screenToWorld(screen, alternate.id);
    check(magnitude(world - universe.bodies()[0].position) < 1e-7, "camera mappings invert across virtual region");
}

void cinematicAutomaticPortals() {
    constexpr double pi = 3.14159265358979323846;
    auto config = inert();
    config.wormholes = true;
    config.black_hole = false; // This exercises natural entry, never the F7 path.
    Region source{42, 0.0, 0.0, 1920.0, 1080.0};
    Region destination{1000042, 0.0, 0.0, 1920.0, 1080.0};
    Universe universe(config);
    universe.reset({}, {source, destination}, {});
    const Wormhole entrance = universe.wormholes().front();
    const Wormhole exit = universe.wormholes()[entrance.partner];
    Body target = body(1, entrance.position, {120.0, 20.0});
    target.region = source.id;
    target.width = 1600.0;
    target.height = 900.0;
    universe.reset({target}, {source, destination}, {});
    const Body initial = universe.bodies()[0];
    universe.step(1.0 / 120.0, {});
    const Body started = universe.bodies()[0];
    check(started.portal_source_region == source.id && started.portal_destination_region == destination.id,
          "automatic transit captures the actual linked pair's arbitrary region ids");
    check(close(started.portal_entry_duration, 1.68) && close(started.portal_exit_duration, 0.75),
          "calm automatic ingress and egress have readable default durations");
    check(magnitude(started.position - initial.position) < 1e-7 && close(started.scale, initial.scale),
          "exact-center natural portal entry is continuous at activation");
    check(started.sink_progress == 0.0 && !started.stored, "portal entry is not black-hole storage");
    check(universe.hitTest(universe.worldToScreen(started.position, started.region), source.id) == 0,
          "a transiting image cannot be selected for a conflicting F7 absorption");
    const Vec2 source_anchor = universe.worldToScreen(entrance.position, source.id);
    const Vec2 destination_anchor = universe.worldToScreen(exit.position, destination.id);
    bool switched = false, seen_emergence = false;
    double entry_scale = initial.scale, exit_scale = 0.0, maximum_motion = 0.0;
    int switches = 0, frames = 0;
    for (; frames < 700 && universe.bodies()[0].portal_progress > 0.0; ++frames) {
        const Body previous = universe.bodies()[0];
        universe.step(1.0 / 120.0, {1800.0, 900.0});
        const Body current = universe.bodies()[0];
        check(!current.stored && close(current.sink_progress, 0.0), "portal animation never stores or closes its living body");
        check(std::isfinite(current.position.x) && std::isfinite(current.velocity.x) &&
              magnitude(current.velocity) <= config.max_speed + 1e-6, "portal animation keeps finite capped velocities");
        if (current.region != previous.region) {
            ++switches;
            switched = true;
            check(current.portal_emerging && current.region == destination.id && current.scale == 0.0,
                  "logical regions switch only in an exactly invisible midpoint frame");
            check(magnitude(current.position - exit.position) < 1e-7, "midpoint arrives exactly at the linked exit center");
        } else {
            check(magnitude(current.position - previous.position) <= config.max_speed / 120.0 + 1e-6,
                  "actual same-region cinematic portal motion obeys the speed cap");
        }
        if (!switched) {
            check(current.region == source.id && !current.portal_emerging, "the entire visible ingress remains in its source region");
            check(current.scale <= entry_scale + 1e-12, "automatic ingress shrinks monotonically");
            entry_scale = current.scale;
            maximum_motion = std::max(maximum_motion, magnitude(current.position - initial.position));
            if (frames == 11)
                check(current.scale > initial.scale * 0.98 && current.stretch < 1.02,
                      "natural entry retains a readable application image during the first tenth-second");
            if (frames == 59)
                check(maximum_motion > 20.0 && current.scale > initial.scale * 0.80,
                      "centered natural entry visibly spirals instead of disappearing abruptly");
        } else {
            check(current.scale + 1e-12 >= exit_scale, "automatic emergence restores image size monotonically");
            exit_scale = current.scale;
            if (current.scale > initial.scale * 0.30 && current.scale < initial.scale * 0.95)
                seen_emergence = true;
        }
        if (current.portal_progress > 0.0) {
            check(magnitude(universe.worldToScreen(entrance.position, source.id) - source_anchor) < 1e-7 &&
                  magnitude(universe.worldToScreen(exit.position, destination.id) - destination_anchor) < 1e-7,
                  "both portal anchors stay fixed while the mouse moves during transit");
            check(current.cooldown == 0.0, "repeat-transfer cooldown begins only when emergence completes");
        }
    }
    const Body completed = universe.bodies()[0];
    check(switches == 1 && maximum_motion > 40.0 && seen_emergence, "one natural crossing visibly enters and emerges exactly once");
    check(frames < 400 && completed.portal_progress == 0.0 && !completed.portal_emerging,
          "normal calm transit completes and releases its cinematic state");
    check(close(completed.scale, initial.scale) && close(completed.stretch, initial.stretch) && close(completed.twist, initial.twist),
          "emergence restores the original full image transforms");
    const double turn = exit.angle - entrance.angle + pi;
    const Vec2 rotated_velocity{initial.velocity.x * std::cos(turn) - initial.velocity.y * std::sin(turn),
                               initial.velocity.x * std::sin(turn) + initial.velocity.y * std::cos(turn)};
    check(magnitude(completed.velocity - rotated_velocity) < 1e-7 && close(completed.cooldown, config.wormhole_cooldown),
          "emergence preserves speed, transforms exit direction, and starts a fresh cooldown");

    // A fast body can cross an entire small mouth between sampled endpoints.
    config.fixed_step = 1.0 / 30.0;
    config.max_speed = 5000.0;
    universe.configure(config);
    const Region compact{7, 0.0, 0.0, 320.0, 320.0};
    universe.reset({}, {compact}, {});
    const auto small = universe.wormholes().front();
    Body fast = body(2, small.position - Vec2{70.0, 0.0}, {5000.0, 0.0});
    fast.region = compact.id;
    universe.reset({fast}, {compact}, {});
    universe.step(1.0 / 30.0, {});
    check(universe.bodies()[0].portal_progress > 0.0 && !universe.bodies()[0].portal_emerging,
          "swept detection catches a portal skipped by both frame endpoints");
    check(close(magnitude(universe.bodies()[0].position - small.position), small.radius),
          "fast ingress starts at the first actual mouth intersection");

    // Rewind into an entry leg, then replay the identical transfer and emergence.
    config = inert(); config.wormholes = true; config.black_hole = false;
    universe.configure(config);
    universe.reset({target}, {source, destination}, {});
    universe.step(1.0 / 120.0, {});
    // The entry-trigger step also counts toward the 30 Hz history clock.
    for (int frame = 0; frame < 83; ++frame) universe.step(1.0 / 120.0, {});
    const Universe expected_seed = universe;
    Universe expected = expected_seed;
    const double recorded = universe.bodies()[0].portal_progress;
    advance(universe, 1.4);
    check(universe.bodies()[0].portal_emerging, "history test reaches the destination emergence leg");
    universe.setRewinding(true);
    for (int frame = 0; frame < 100 && universe.bodies()[0].portal_progress > recorded + 1e-9; ++frame)
        universe.step(1.0 / config.history_hz, {});
    check(close(universe.bodies()[0].portal_progress, recorded) && universe.bodies()[0].region == source.id,
          "rewind reverses the region switch and restores the recorded entry leg");
    universe.setRewinding(false);
    for (int frame = 0; frame < 250; ++frame) {
        universe.step(1.0 / 120.0, {});
        expected.step(1.0 / 120.0, {});
        const Body replayed = universe.bodies()[0], original = expected.bodies()[0];
        check(replayed.region == original.region && replayed.portal_emerging == original.portal_emerging &&
              close(replayed.portal_progress, original.portal_progress), "resumed portal playback repeats its phase and region exactly");
        check(magnitude(replayed.position - original.position) < 1e-7 && magnitude(replayed.velocity - original.velocity) < 1e-7 &&
              close(replayed.scale, original.scale) && close(replayed.angle, original.angle) &&
              close(replayed.stretch, original.stretch) && close(replayed.twist, original.twist),
              "resumed automatic transit follows identical motion and image deformation");
    }
    universe.removeBody(target.id);
    universe.setRewinding(true);
    advance(universe, 3.0);
    check(universe.bodies().empty(), "rewind never resurrects a real app closed during portal transit");

    for (const double cancel_time : {0.4, 1.9}) {
        universe.configure(config);
        universe.reset({target}, {source, destination}, {});
        universe.step(1.0 / 120.0, {});
        advance(universe, cancel_time);
        const Body before = universe.bodies()[0];
        auto disabled = config; disabled.wormholes = false;
        universe.configure(disabled);
        const Body canceled = universe.bodies()[0];
        check(canceled.portal_progress == 0.0 && !canceled.portal_emerging && !canceled.stored &&
              canceled.region == before.region && magnitude(canceled.position - before.position) < 1e-7,
              "disabling wormholes safely cancels either leg without teleporting or storing the app");
        check(close(canceled.scale, initial.scale) && close(magnitude(canceled.velocity), magnitude(initial.velocity)),
              "canceling a transit restores its normal image and conserved speed");
        universe.setRewinding(true);
        advance(universe, 0.3);
        check(universe.bodies()[0].portal_progress == 0.0 && close(universe.bodies()[0].scale, initial.scale),
              "disabled wormholes cannot be reactivated by historical transit frames");
    }

    config = inert(); config.wormholes = true; config.cursor_gravity = config.orbit = true;
    config.cursor_strength = 0.0; config.spaghetti = false;
    universe.configure(config);
    Body still = target; still.velocity = {};
    universe.reset({still}, {source, destination}, entrance.position);
    check(magnitude(universe.bodies()[0].velocity) == 0.0, "zero-speed portal regression uses a genuinely stationary body");
    universe.step(1.0 / 120.0, entrance.position);
    advance(universe, 0.4, entrance.position);
    check(magnitude(universe.bodies()[0].position - entrance.position) > 5.0 &&
          universe.bodies()[0].scale < initial.scale && close(universe.bodies()[0].stretch, 1.0) &&
          close(universe.bodies()[0].twist, 0.0),
          "stationary center entry still animates motion and shrinking with spaghetti disabled");
    Universe untouched = universe;
    auto strong = config;
    strong.cursor_strength = 20000000.0; strong.binary = strong.collisions = strong.expansion = true;
    strong.mutual_strength = 2000000.0; strong.explosion_strength = 3000.0;
    universe.configure(strong);
    Body neighbor = body(9, universe.bodies()[0].position, {50.0, 0.0}); neighbor.region = source.id;
    universe.addBody(neighbor, false);
    universe.setBinaryPreset();
    universe.supernova(universe.bodies()[0].position, source.id);
    for (int frame = 0; frame < 180; ++frame) {
        universe.step(1.0 / 120.0, {1800.0, 900.0});
        untouched.step(1.0 / 120.0, {1800.0, 900.0});
        const Body actual = universe.bodies()[0], expected = untouched.bodies()[0];
        check(magnitude(actual.position - expected.position) < 1e-7 && magnitude(actual.velocity - expected.velocity) < 1e-7 &&
              close(actual.scale, expected.scale) && actual.region == expected.region && close(actual.portal_progress, expected.portal_progress),
              "portal legs ignore external gravity, binary presets, collisions, expansion and explosions");
    }
    checkFinite(universe);

    // Non-sorted physical ids and delayed travelers share frozen camera seeds.
    config = inert(); config.wormholes = true;
    universe.configure(config);
    Region second_monitor{7, 1920.0, 0.0, 1280.0, 720.0};
    universe.reset({}, {source, second_monitor, destination}, {});
    const Wormhole forward = universe.wormholes().front();
    const Wormhole reverse = universe.wormholes()[forward.partner];
    Body first = body(20, forward.position, {5.0, 5.0}); first.region = source.id;
    Body delayed = body(21, forward.position + Vec2{5.0, 0.0}, {5.0, 0.0});
    delayed.region = source.id; delayed.cooldown = 0.30;
    universe.reset({first, delayed}, {source, second_monitor, destination}, {});
    universe.step(1.0 / 120.0, {});
    const Vec2 frozen_source = universe.worldToScreen(forward.position, source.id);
    const Vec2 frozen_exit = universe.worldToScreen(reverse.position, second_monitor.id);
    bool first_finished_while_second_transits = false;
    for (int frame = 0; frame < 350; ++frame) {
        universe.step(1.0 / 120.0, {});
        if (universe.bodies()[0].portal_progress == 0.0 && universe.bodies()[0].region == second_monitor.id &&
            universe.bodies()[1].portal_progress > 0.0)
            first_finished_while_second_transits = true;
        if (universe.bodies()[0].portal_progress > 0.0 || universe.bodies()[1].portal_progress > 0.0)
            check(magnitude(universe.worldToScreen(forward.position, source.id) - frozen_source) < 1e-7 &&
                  magnitude(universe.worldToScreen(reverse.position, second_monitor.id) - frozen_exit) < 1e-7,
                  "overlapping travelers retain the same source and destination cameras after the earlier one finishes");
    }
    check(first_finished_while_second_transits && universe.bodies()[0].region == second_monitor.id &&
          universe.bodies()[1].region == second_monitor.id,
          "delayed overlapping transits resolve actual linked monitor ids, not virtual-id arithmetic");
    Body returning = body(22, reverse.position, {10.0, 5.0}); returning.region = second_monitor.id;
    universe.reset({returning}, {source, second_monitor, destination}, {});
    universe.step(1.0 / 120.0, {});
    check(universe.bodies()[0].portal_source_region == second_monitor.id && universe.bodies()[0].portal_destination_region == source.id,
          "a reverse linked mouth captures the exact opposite source and destination");
    for (int frame = 0; frame < 400 && universe.bodies()[0].portal_progress > 0.0; ++frame)
        universe.step(1.0 / 120.0, {});
    check(universe.bodies()[0].region == source.id && close(magnitude(universe.bodies()[0].velocity), magnitude(returning.velocity)),
          "reverse automatic transfer emerges with its original speed in the real source monitor");

    config.max_speed = 180.0; config.sink_duration = 0.3;
    universe.configure(config);
    universe.reset({target}, {source, destination}, {});
    universe.step(1.0 / 120.0, {});
    check(universe.bodies()[0].portal_entry_duration >= 1.4 && universe.bodies()[0].portal_exit_duration >= 0.75,
          "an aggressive sink setting cannot make natural portal animation instantaneous");
    int limited_frames = 0;
    for (; limited_frames < 1700 && universe.bodies()[0].portal_progress > 0.0; ++limited_frames) {
        const Body before = universe.bodies()[0];
        universe.step(1.0 / 120.0, {});
        const Body after = universe.bodies()[0];
        if (before.region == after.region)
            check(magnitude(after.position - before.position) <= config.max_speed / 120.0 + 1e-6,
                  "low speed caps extend entry and exit durations to bound actual image displacement");
    }
    check(limited_frames < 1700 && universe.bodies()[0].portal_progress == 0.0,
          "speed-limited natural transit still completes rather than remaining in a hidden state");
}

void blackHoleAndLivingHistory() {
    auto config = inert();
    config.history_seconds = 8.0;
    Universe universe(config);
    Body target = body(1, {960.0, 540.0}, {20.0, 0.0});
    target.width = 400.0;
    target.height = 200.0;
    target.angle = 1.0;
    universe.reset({target, body(2, {1300.0, 700.0}, {30.0, 0.0})}, {Region{}}, {960.0, 540.0});
    check(universe.hitTest({960.0, 540.0}) == 1, "rotated visual body is selectable at its center");
    check(!universe.startBlackHole({20.0, 20.0}), "empty display point does not choose real window coordinates");
    check(universe.startBlackHole({960.0, 540.0}), "black hole starts for selected body");
    const double initial_scale = universe.bodies()[0].scale;
    advance(universe, 1.0, {1800.0, 1000.0});
    const auto falling = universe.bodies()[0];
    check(close(falling.sink_center.x, 960.0) && close(falling.sink_center.y, 540.0), "sink origin remains fixed while cursor moves");
    check(falling.scale < initial_scale && falling.stretch > 1.0 && falling.twist > 0.0, "sink shrinks and deforms whole texture");
    advance(universe, 2.0);
    check(universe.bodies()[0].stored && close(universe.bodies()[0].scale, 0.0), "completed sink virtually stores body");
    universe.removeBody(2);
    check(universe.addBody(body(3, {500.0, 300.0}, {10.0, 0.0}), false), "new body remains live across history replay");
    universe.setRewinding(true);
    check(universe.rewinding(), "history enables reverse playback");
    for (int index = 0; index < 400 && universe.rewinding(); ++index) {
        universe.step(1.0 / 60.0, {});
        check(std::none_of(universe.bodies().begin(), universe.bodies().end(), [](const Body& current) { return current.id == 2; }),
              "closed application is never resurrected");
    }
    check(!universe.bodies()[0].stored && close(universe.bodies()[0].scale, initial_scale), "rewind restores living stored window");
    check(close(universe.bodies()[0].sink_progress, 0.0), "rewind restores pre-sink deformation state");
    check(universe.bodies().size() == 2 && universe.bodies()[1].id == 3, "rewind preserves actual live application set");
    check(!universe.rewinding(), "reverse playback stops at finite history boundary");
}

void cinematicBlackHole() {
    constexpr double pi = 3.14159265358979323846;
    const Vec2 center{960.0, 540.0};
    for (const bool zero_velocity : {false, true}) {
        auto config = Config::calm();
        if (zero_velocity) config.cursor_strength = 0.0;
        Universe universe(config);
        Body target = body(1, center, zero_velocity ? Vec2{} : Vec2{80.0, 0.0});
        target.width = 1600.0;
        target.height = 900.0;
        universe.reset({target}, {Region{}}, center);
        const Body initial = universe.bodies()[0];
        check(!zero_velocity || magnitude(initial.velocity) == 0.0, "center fallback is exercised with genuinely zero initial velocity");
        check(universe.startBlackHole(center), "calm single-window center selection starts a sink");
        check(magnitude(universe.bodies()[0].position - initial.position) == 0.0, "black-hole activation never jumps position");
        check(close(universe.bodies()[0].angle, initial.angle) && close(universe.bodies()[0].scale, initial.scale),
              "black-hole activation preserves displayed angle and scale");
        check(close(universe.bodies()[0].sink_duration, config.sink_duration), "calm sink retains its requested duration");
        double previous_scale = initial.scale;
        double maximum_motion = 0.0;
        const int frames = static_cast<int>(std::ceil(config.sink_duration * 120.0));
        for (int frame = 0; frame < frames; ++frame) {
            const Vec2 before = universe.bodies()[0].position;
            universe.step(1.0 / 120.0, {1800.0, 900.0});
            const auto& current = universe.bodies()[0];
            check(std::isfinite(current.position.x) && std::isfinite(current.velocity.x), "centered sink motion stays finite");
            check(magnitude(current.position - before) <= config.max_speed / 120.0 + 1e-6, "actual sink displacement obeys the speed cap");
            check(current.scale <= previous_scale + 1e-12, "visible sink scale decreases monotonically");
            previous_scale = current.scale;
            maximum_motion = std::max(maximum_motion, magnitude(universe.worldToScreen(current.position, 0) - center));
            if (frame == 11) {
                check(!current.stored && current.scale > initial.scale * 0.95, "early take-in arc retains readable application imagery");
                check(current.stretch < 1.15 && current.twist < 0.05, "deformation starts gently instead of becoming a needle immediately");
            }
            if (frame == 59) {
                check(magnitude(universe.worldToScreen(current.position, 0) - center) > 20.0, "center selection produces clearly visible screen-space motion");
                check(!current.stored && current.scale > initial.scale * 0.85, "half-second image remains visible during calm absorption");
            }
            if (frame == 239)
                check(!current.stored && current.scale > initial.scale * 0.15, "application remains visible through the later absorption stage");
            if (!current.stored)
                check(magnitude(universe.worldToScreen(current.sink_center, 0) - center) < 1e-7, "screen-space sink center stays fixed while mouse moves");
        }
        check(maximum_motion > 35.0, "centered capture has a finite take-in arc");
        advance(universe, 0.05);
        check(universe.bodies()[0].stored && close(universe.bodies()[0].scale, 0.0), "continuous sink ends in virtual storage");
        check(magnitude(universe.bodies()[0].position - center) < 1e-7, "finished path converges to its fixed center");
    }

    auto config = Config::calm();
    config.spaghetti = false;
    Universe universe(config);
    universe.reset({body(3, center, {60.0, 0.0})}, {Region{}}, center);
    check(universe.startBlackHole(center), "sink starts with deformation disabled");
    advance(universe, 0.6);
    const auto undeformed = universe.bodies()[0];
    check(magnitude(undeformed.position - center) > 20.0 && undeformed.scale < undeformed.original_scale,
          "deformation-disabled capture still moves and shrinks");
    check(std::abs(undeformed.angle) > 0.1 && close(undeformed.stretch, 1.0) && close(undeformed.twist, 0.0),
          "deformation-disabled capture spins without stretching or twisting");

    // A point away from the image center must keep exactly its original pose at
    // activation and still follow a nondegenerate spiral.
    universe.reset({body(5, center, {20.0, 40.0})}, {Region{}}, center);
    const Vec2 offset_cursor = center + Vec2{20.0, -8.0};
    check(universe.startBlackHole(offset_cursor), "offset shape selection starts absorption");
    check(magnitude(universe.bodies()[0].position - center) == 0.0, "offset selection starts without a positional jump");
    advance(universe, 0.4);
    check(magnitude(universe.bodies()[0].position - center) > 10.0, "offset selection visibly follows its spiral");
    check(magnitude(universe.bodies()[0].sink_center - offset_cursor) < 1e-7, "offset selection captures the chosen center exactly");

    // Short requested durations can only finish when they respect the speed cap.
    config = inert();
    config.sink_duration = 0.3;
    config.max_speed = 180.0;
    universe.configure(config);
    universe.reset({body(7, center, {50.0, 0.0})}, {Region{}}, center);
    check(universe.startBlackHole(center + Vec2{25.0, 10.0}), "minimum-duration selection starts");
    const double effective_duration = universe.bodies()[0].sink_duration;
    check(effective_duration >= config.sink_duration, "speed-limited sink never shortens its minimum duration");
    const int frames = static_cast<int>(std::ceil(effective_duration * 120.0)) + 2;
    for (int frame = 0; frame < frames; ++frame) {
        const Vec2 previous = universe.bodies()[0].position;
        universe.step(1.0 / 120.0, {});
        const auto& current = universe.bodies()[0];
        check(magnitude(current.velocity) <= config.max_speed + 1e-6, "cinematic sink velocity stays bounded at minimum duration");
        check(magnitude(current.position - previous) <= config.max_speed / 120.0 + 1e-6, "minimum-duration path also bounds actual displacement");
    }
    check(universe.bodies()[0].stored, "speed-limited path still reaches virtual storage");

    // The fixed camera must not cancel movement or change the captured cursor
    // anchor, including a region whose initial camera was already zoomed out.
    config = Config::calm();
    universe.configure(config);
    const Region compact{42, 0.0, 0.0, 1280.0, 720.0};
    Body distant = body(9, {2100.0, 360.0}, {-80.0, 0.0});
    distant.region = compact.id;
    universe.reset({distant}, {compact}, {640.0, 360.0});
    const Vec2 anchor = universe.worldToScreen(distant.position, compact.id);
    const double captured_zoom = universe.cameras()[0].zoom;
    check(captured_zoom < 1.0, "camera-anchor regression begins with an expanded view");
    check(universe.startBlackHole(anchor, compact.id), "expanded-view center selection starts");
    advance(universe, 1.4, {100.0, 100.0});
    check(close(universe.cameras()[0].zoom, captured_zoom), "absorption camera does not undo presentation shrinking");
    check(magnitude(universe.worldToScreen(universe.bodies()[0].sink_center, compact.id) - anchor) < 1e-7,
          "expanded-view black-hole anchor stays at activation screen coordinates");

    // Test the actual shader vertex mapping against a small viewport, including
    // its twist/stretch/bend. This catches the former collision-radius padding
    // that clipped both ends of a rotating image.
    const Region small{77, 0.0, 0.0, 720.0, 480.0};
    Body large = body(11, small.center(), {60.0, 0.0});
    large.region = small.id;
    large.width = 1600.0;
    large.height = 900.0;
    universe.reset({large}, {small}, small.center());
    check(universe.startBlackHole(small.center(), small.id), "small-viewport capture starts");
    for (int frame = 0; frame < 320; ++frame) {
        universe.step(1.0 / 120.0, small.center());
        if (frame % 20 != 0) continue;
        const auto& current = universe.bodies()[0];
        for (int x = -2; x <= 2; ++x) for (int y = -2; y <= 2; ++y) {
            Vec2 normalized{static_cast<double>(x) * 0.5, static_cast<double>(y) * 0.5};
            const double twist = current.twist * std::min(1.0, magnitude(normalized));
            Vec2 offset{(normalized.x * std::cos(twist) - normalized.y * std::sin(twist)) * current.width * 0.5 * current.scale * current.stretch,
                        (normalized.x * std::sin(twist) + normalized.y * std::cos(twist)) * current.height * 0.5 * current.scale / current.stretch};
            offset.y += std::sin(static_cast<double>(x) * 0.5 * pi) * current.sink_progress * 0.24 * current.height * current.scale;
            const Vec2 turned{offset.x * std::cos(current.angle) - offset.y * std::sin(current.angle),
                              offset.x * std::sin(current.angle) + offset.y * std::cos(current.angle)};
            const Vec2 screen = universe.worldToScreen(current.position + turned, current.region);
            check(screen.x >= small.x && screen.x <= small.x + small.width &&
                  screen.y >= small.y && screen.y <= small.y + small.height,
                  "rotating deformed image remains inside the compact presentation viewport");
        }
    }

    config = Config::calm();
    universe.configure(config);
    universe.reset({body(13, center, {80.0, 0.0})}, {Region{}}, center);
    check(universe.startBlackHole(center), "deterministic rewind capture starts");
    advance(universe, 0.7);
    Universe expected = universe;
    const double rewind_target = expected.bodies()[0].sink_progress;
    advance(universe, 0.5);
    universe.setRewinding(true);
    for (int frame = 0; frame < 100 && universe.bodies()[0].sink_progress > rewind_target + 1e-9; ++frame)
        universe.step(1.0 / 30.0, {});
    check(close(universe.bodies()[0].sink_progress, rewind_target), "rewind returns to the recorded cinematic progress");
    universe.setRewinding(false);
    for (int frame = 0; frame < 120; ++frame) {
        universe.step(1.0 / 120.0, center);
        expected.step(1.0 / 120.0, center);
        const auto& replayed = universe.bodies()[0];
        const auto& original = expected.bodies()[0];
        check(magnitude(replayed.position - original.position) < 1e-7 && magnitude(replayed.velocity - original.velocity) < 1e-7,
              "forward playback after rewind follows the identical seeded absorption path");
        check(close(replayed.scale, original.scale) && close(replayed.angle, original.angle) &&
              close(replayed.stretch, original.stretch) && close(replayed.twist, original.twist),
              "forward playback after rewind preserves the identical visual deformation");
    }
}

void supernovaAndCaps() {
    auto config = inert();
    config.max_bodies = 3;
    config.max_particles = 80;
    config.history_seconds = 0.5;
    Universe universe(config);
    universe.reset({body(1, {700.0, 540.0}, {-10.0, 0.0}), body(2, {1200.0, 540.0}, {10.0, 0.0})}, {Region{}}, {});
    const double before = universe.bodies()[0].velocity.x;
    universe.supernova({960.0, 540.0});
    check(universe.bodies()[0].velocity.x < before, "supernova impulse points away from source");
    check(!universe.particles().empty() && !universe.waves().empty(), "supernova emits visible particles and wave");
    check(universe.addBody(body(3, {960.0, 540.0})), "real new window emits supernova");
    check(!universe.addBody(body(4, {960.0, 540.0})), "body cap rejects additional target");
    check(!universe.addBody(body(1, {960.0, 540.0})), "duplicate live ids rejected");
    for (int index = 0; index < 100; ++index)
        universe.supernova({960.0, 540.0});
    checkFinite(universe);
    advance(universe, 5.0);
    check(universe.particles().empty() && universe.waves().empty(), "transient emissions expire without unbounded retention");
    check(universe.historyFrames() <= 16, "short history remains a finite ring");
    config.rewind = false;
    universe.configure(config);
    check(universe.historyFrames() == 0, "disabling rewind releases history");
}

void compareNovaFragments(const Universe& first, const Universe& second, const Body& a, const Body& b) {
    const auto expected = first.novaFragments(a), actual = second.novaFragments(b);
    check(expected.size() == actual.size(), "seeded stellar playback preserves fragment count");
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto& left = expected[index];
        const auto& right = actual[index];
        check(left.column == right.column && left.row == right.row && left.grid == right.grid &&
              magnitude(left.position - right.position) < 1e-7 && close(left.angle, right.angle) &&
              close(left.scale, right.scale) && close(left.alpha, right.alpha),
              "rewound fragment geometry, spin and opaque appearance replay identically");
    }
}

void cinematicStellarNova() {
    const Vec2 center{960.0, 540.0};
    for (const std::size_t count : {4U, 16U}) {
        auto config = inert();
        config.black_hole = false;
        config.stellar_fragments = count;
        config.stellar_charge_seconds = 1.0;
        config.stellar_fragment_seconds = 1.5;
        config.stellar_growth = 1.7;
        Universe universe(config);
        Body target = body(1, center, {40.0, -20.0});
        target.width = 800.0;
        target.height = 400.0;
        target.angle = 0.4;
        target.stretch = 1.3;
        target.twist = 0.2;
        universe.reset({target, body(2, {1400.0, 700.0})}, {Region{}}, center);
        const Body initial = universe.bodies()[0];
        const Vec2 anchor = universe.worldToScreen(initial.position, initial.region);
        check(!universe.startStellarNova(Vec2{30.0, 30.0}, 0), "F5 on empty sky does not select an unrelated window");
        check(!universe.startStellarNova(anchor, 42), "F5 cannot select a window in a hidden virtual region");
        check(universe.startStellarNova(anchor, 0), "F5 selection starts a stellar nova at the displayed window");
        const Body started = universe.bodies()[0];
        check(started.nova_age == 0.0 && !started.stored && close(started.scale, initial.scale) &&
              magnitude(started.position - initial.position) < 1e-7 && close(started.angle, initial.angle),
              "stellar activation preserves the currently displayed pose without a jump");
        check(universe.novaFragments(started).empty(), "the charging image is not split before its burst");
        check(!universe.startStellarNova(uint64_t{2}), "a second source cannot overlap a charging stellar event");
        check(!universe.startBlackHole(anchor, 0), "F7 cannot interrupt a charging stellar source");
        double previous_scale = initial.scale;
        for (int frame = 0; frame < 119; ++frame) {
            universe.step(1.0 / 120.0, {100.0, 100.0});
            const Body current = universe.bodies()[0];
            check(!current.stored && current.scale + 1e-12 >= previous_scale &&
                  current.scale <= initial.scale * config.stellar_growth + 1e-7,
                  "charging stellar image grows continuously within its configured size");
            check(magnitude(current.position - initial.position) < 1e-7 &&
                  magnitude(universe.worldToScreen(current.position, 0) - anchor) < 1e-7,
                  "stellar charge remains anchored while the cursor moves");
            check(universe.novaFragments(current).empty(), "stellar source remains intact throughout charge");
            previous_scale = current.scale;
        }
        check(universe.bodies()[0].scale > initial.scale * 1.6, "late stellar charge visibly approaches its expanded size");
        for (int frame = 0; frame < 3 && !universe.bodies()[0].stored; ++frame)
            universe.step(1.0 / 120.0, center);
        const Body burst = universe.bodies()[0];
        check(burst.stored && close(burst.scale, 0.0) && burst.nova_age >= burst.nova_charge_seconds,
              "burst virtually stores the intact image without removing its living app id");
        check(!universe.particles().empty() && !universe.waves().empty(), "stellar burst emits the existing shockwave and particle effects");
        check(!universe.startStellarNova(uint64_t{2}), "a second stellar event cannot overlap flying fragments");
        check(!universe.startStellarNova(uint64_t{1}), "an exploded source cannot explode again");
        const auto fragments = universe.novaFragments(burst);
        const std::size_t grid = count == 4 ? 2 : 4;
        check(fragments.size() == count && burst.nova_grid == grid, "configured 4 or 16 split yields the expected square tile grid");
        bool seen[4][4]{};
        for (const auto& fragment : fragments) {
            check(fragment.grid == grid && fragment.column < grid && fragment.row < grid,
                  "each fragment identifies a bounded crop from the shared source image");
            check(!seen[fragment.row][fragment.column], "each image crop appears exactly once");
            seen[fragment.row][fragment.column] = true;
            const Vec2 offset{
                (static_cast<double>(fragment.column) + 0.5 - static_cast<double>(grid) * 0.5) *
                    burst.width * burst.nova_initial_scale * burst.nova_growth / static_cast<double>(grid),
                (static_cast<double>(fragment.row) + 0.5 - static_cast<double>(grid) * 0.5) *
                    burst.height * burst.nova_initial_scale * burst.nova_growth / static_cast<double>(grid)};
            const Vec2 rotated{offset.x * std::cos(burst.angle) - offset.y * std::sin(burst.angle),
                               offset.x * std::sin(burst.angle) + offset.y * std::cos(burst.angle)};
            check(magnitude(fragment.position - burst.position - rotated) <= config.max_speed * config.fixed_step + 1e-7 &&
                  std::abs(fragment.angle - burst.angle) <= 4.0 * config.fixed_step + 1e-7 && close(fragment.alpha, 1.0),
                  "the first fragment frame starts at its expanded crop and advances only one bounded physical step");
        }
        advance(universe, 0.35, {1800.0, 900.0});
        const auto flying = universe.novaFragments(universe.bodies()[0]);
        check(flying.size() == count, "fragment tiles remain visible during their configured flight");
        bool differing_rotation = false, differing_displacement = false;
        const auto& exact = fragments;
        for (std::size_t index = 1; index < flying.size(); ++index) {
            differing_rotation |= !close(flying[index].angle, flying[0].angle);
            differing_displacement |= !close(magnitude(flying[index].position - exact[index].position),
                                              magnitude(flying[0].position - exact[0].position));
        }
        check(differing_rotation && differing_displacement, "fragment seeds produce varied spin and radial speeds rather than identical clones");
        check(magnitude(universe.bodies()[0].position - initial.position) < 1e-7,
              "the remnant hole stays at the actual explosion location while fragments fly");
        advance(universe, 5.0);
        const Body remnant = universe.bodies()[0];
        check(remnant.stored && universe.novaFragments(remnant).size() == count &&
              close(remnant.nova_age, remnant.nova_charge_seconds + remnant.nova_fragment_seconds),
              "settled fragments remain visible beside a fixed remnant with a bounded blast age");
        check(universe.bodies().size() == count + 2 && universe.bodies()[0].id == target.id &&
              universe.objectCount() == count + 1,
              "exploding the virtual image never removes or recreates the actual application id");
        check(universe.startStellarNova(uint64_t{2}), "settled persistent pieces do not block the next living source's stellar event");
        checkFinite(universe);
    }

    auto config = inert();
    Universe universe(config);
    Body unavailable = body(4, center);
    unavailable.stored = true;
    universe.reset({unavailable}, {Region{}}, center);
    check(!universe.startStellarNova(uint64_t{4}) && !universe.startStellarNova(uint64_t{999}),
          "stored and unknown app ids cannot become stellar sources");
    universe.reset({body(4, center)}, {Region{}}, center);
    check(universe.startBlackHole(center), "sink exclusion test starts a real cinematic sink");
    check(!universe.startStellarNova(uint64_t{4}), "a sinking source is excluded from stellar selection");
    config.wormholes = true;
    universe.configure(config);
    universe.reset({}, {Region{}}, center);
    universe.reset({body(4, universe.wormholes().front().position)}, {Region{}}, center);
    universe.step(1.0 / 120.0, center);
    check(universe.bodies()[0].portal_progress > 0.0 && !universe.startStellarNova(uint64_t{4}),
          "a source entering a natural wormhole cannot be selected for a stellar nova");
    config = inert();
    config.supernova = false;
    universe.configure(config);
    universe.reset({body(4, center)}, {Region{}}, center);
    check(!universe.startStellarNova(uint64_t{4}), "disabling supernova effects also disables F5 stellar events");
    config.supernova = true;
    universe.configure(config);
    advance(universe, 0.2);
    universe.setRewinding(true);
    check(universe.rewinding() && !universe.startStellarNova(uint64_t{4}), "reverse playback cannot initiate a new stellar event");

    config.stellar_interval_min = std::numeric_limits<double>::quiet_NaN();
    config.stellar_interval_max = -20.0;
    config.stellar_charge_seconds = std::numeric_limits<double>::infinity();
    config.stellar_fragment_seconds = -1.0;
    config.stellar_growth = std::numeric_limits<double>::quiet_NaN();
    config.stellar_fragments = 1000000;
    universe.configure(config);
    const Config normalized = universe.config();
    check(std::isfinite(normalized.stellar_interval_min) && normalized.stellar_interval_min >= 5.0 &&
          std::isfinite(normalized.stellar_interval_max) && normalized.stellar_interval_max >= normalized.stellar_interval_min &&
          std::isfinite(normalized.stellar_charge_seconds) && normalized.stellar_charge_seconds > 0.0 &&
          std::isfinite(normalized.stellar_fragment_seconds) && normalized.stellar_fragment_seconds > 0.0 &&
          std::isfinite(normalized.stellar_growth) && normalized.stellar_growth >= 1.0 &&
          (normalized.stellar_fragments == 4 || normalized.stellar_fragments == 16),
          "invalid direct-native stellar configuration is normalized to finite bounded safe values");

    for (const double cancel_time : {0.4, 1.4}) {
        config = inert();
        config.stellar_charge_seconds = 1.0;
        config.stellar_fragment_seconds = 2.0;
        universe.configure(config);
        universe.reset({body(4, center)}, {Region{}}, center);
        const double original_scale = universe.bodies()[0].scale;
        check(universe.startStellarNova(uint64_t{4}), "stellar cancellation regression starts a selected source");
        advance(universe, cancel_time);
        config.supernova = false;
        universe.configure(config);
        const Body canceled = universe.bodies()[0];
        check(canceled.nova_age < 0.0 && !canceled.stored && close(canceled.scale, original_scale) &&
              universe.novaFragments(canceled).empty(), "disabling stellar effects cancels either charge or flying fragments and restores the living image");
        universe.setRewinding(true);
        advance(universe, 0.3);
        check(universe.bodies()[0].nova_age < 0.0 && !universe.bodies()[0].stored &&
              universe.novaFragments(universe.bodies()[0]).empty(), "historical stellar states cannot reactivate disabled supernova effects");
    }
}

void stellarRemnantPhysics() {
    const Vec2 center{960.0, 540.0};
    auto config = inert();
    config.stellar_charge_seconds = 0.5;
    Universe universe(config);
    universe.reset({body(1, center)}, {Region{}}, center);
    check(universe.startStellarNova(uint64_t{1}), "remnant attraction test starts a stellar burst");
    advance(universe, 0.6);
    check(universe.bodies()[0].stored, "remnant physics begins only after the selected image bursts");
    check(universe.addBody(body(2, center + Vec2{300.0, 0.0}), false), "another living image can approach the persistent remnant");
    Universe disabled = universe;
    auto no_hole = config;
    no_hole.black_hole = false;
    disabled.configure(no_hole);
    advance(universe, 0.3);
    advance(disabled, 0.3);
    check(findBody(universe, 2).velocity.x < -0.1 && close(findBody(disabled, 2).velocity.x, 0.0),
          "stellar remnant supplies softened attraction independently of cursor gravity and obeys the black-hole effect switch");
    check(magnitude(universe.bodies()[0].position - center) < 1e-7 && findBody(universe, 2).sink_progress == 0.0,
          "distant remnant attraction does not move the hole or instantly hide its neighbors");

    config.fixed_step = 1.0 / 30.0;
    config.max_speed = 5000.0;
    config.cursor_strength = 0.0;
    universe.configure(config);
    universe.reset({body(1, center)}, {Region{}}, center);
    check(universe.startStellarNova(uint64_t{1}), "swept remnant absorption regression starts its source");
    advance(universe, 0.6);
    check(universe.addBody(body(3, center - Vec2{70.0, 0.0}, {5000.0, 0.0}), false),
          "a fast neighboring image approaches the remnant without a new-window explosion");
    universe.step(1.0 / 30.0, center);
    const Body falling = findBody(universe, 3);
    check(falling.sink_progress > 0.0 && !falling.stored &&
          magnitude(falling.sink_center - center) < 1e-7,
          "swept remnant mouth detection starts a visible F7-style absorption even when both endpoints skip its center");
    check(close(falling.scale, falling.original_scale), "remnant entry preserves the readable image at activation rather than making it disappear");
    advance(universe, 8.0);
    check(findBody(universe, 3).stored && universe.bodies().size() == 18,
          "remnant absorption ends in virtual storage while preserving both actual living app ids");
    checkFinite(universe);
}

void stellarNovaHistoryAndScheduler() {
    const Vec2 center{960.0, 540.0};
    auto config = inert();
    config.stellar_charge_seconds = 1.0;
    config.stellar_fragment_seconds = 2.0;
    config.history_seconds = 10.0;
    Universe universe(config);
    Body clock_body = body(2, {2400.0, 350.0}, {12.0, 0.0});
    clock_body.region = 42;
    universe.reset({body(1, center), clock_body}, {Region{}, Region{42, 1920.0, 0.0, 1920.0, 1080.0}}, center);
    check(universe.startStellarNova(uint64_t{1}), "stellar rewind test starts a selected living source");
    advance(universe, 1.4);
    Universe expected = universe;
    const double clock = expected.bodies()[1].position.x;
    check(!expected.novaFragments(expected.bodies()[0]).empty(), "recorded rewind checkpoint contains flying image fragments");
    advance(universe, 1.0);
    universe.setRewinding(true);
    for (int frame = 0; frame < 100 && universe.bodies()[1].position.x > clock + 1e-7; ++frame)
        universe.step(1.0 / config.history_hz, center);
    check(close(universe.bodies()[1].position.x, clock) && close(universe.bodies()[0].nova_age, expected.bodies()[0].nova_age),
          "rewind restores the recorded fragment age and living simulation clock");
    compareNovaFragments(expected, universe, expected.bodies()[0], universe.bodies()[0]);
    universe.setRewinding(false);
    for (int frame = 0; frame < 240; ++frame) {
        universe.step(1.0 / 120.0, center);
        expected.step(1.0 / 120.0, center);
        const Body replayed = universe.bodies()[0], original = expected.bodies()[0];
        check(replayed.stored == original.stored && close(replayed.nova_age, original.nova_age) &&
              replayed.nova_seed == original.nova_seed && magnitude(replayed.position - original.position) < 1e-7,
              "forward stellar playback preserves remnant position, phase and seed after rewind");
        compareNovaFragments(expected, universe, original, replayed);
    }
    universe.setRewinding(true);
    for (int frame = 0; frame < 150 && universe.bodies()[0].stored; ++frame)
        universe.step(1.0 / config.history_hz, center);
    check(!universe.bodies()[0].stored && universe.bodies()[0].nova_age < universe.bodies()[0].nova_charge_seconds &&
          universe.novaFragments(universe.bodies()[0]).empty(), "rewinding through the burst reassembles the intact charging window and removes its hole");
    universe.removeBody(1);
    advance(universe, 10.0);
    check(universe.bodies().size() == 1 && universe.bodies()[0].id == 2,
          "closed stellar apps and their fragment or hole state are never resurrected by history");

    universe.configure(config);
    universe.reset({body(1, center), clock_body, body(3, {1400.0, 540.0})},
                   {Region{}, Region{42, 1920.0, 0.0, 1920.0, 1080.0}}, center);
    check(universe.startStellarNova(uint64_t{1}), "pre-burst replay regression starts its selected source");
    advance(universe, 0.4);
    expected = universe;
    const double before_burst_clock = expected.bodies()[1].position.x;
    advance(universe, 1.1);
    universe.setRewinding(true);
    for (int frame = 0; frame < 100 && universe.bodies()[1].position.x > before_burst_clock + 1e-7; ++frame)
        universe.step(1.0 / config.history_hz, center);
    universe.setRewinding(false);
    for (int frame = 0; frame < 160; ++frame) {
        universe.step(1.0 / 120.0, center);
        expected.step(1.0 / 120.0, center);
        for (std::size_t index = 0; index < universe.bodies().size(); ++index)
            check(magnitude(universe.bodies()[index].position - expected.bodies()[index].position) < 1e-7 &&
                  magnitude(universe.bodies()[index].velocity - expected.bodies()[index].velocity) < 1e-7,
                  "rewinding to charge preserves the exact seeded explosion impulse and remnant gravity on neighbors");
        check(universe.particles().size() == expected.particles().size(), "replayed burst regenerates the same bounded particle count");
        for (std::size_t index = 0; index < universe.particles().size(); ++index) {
            const auto& actual_particle = universe.particles()[index];
            const auto& expected_particle = expected.particles()[index];
            check(magnitude(actual_particle.position - expected_particle.position) < 1e-7 &&
                  magnitude(actual_particle.velocity - expected_particle.velocity) < 1e-7 &&
                  close(actual_particle.life, expected_particle.life) && close(actual_particle.size, expected_particle.size),
                  "rewind restores the RNG so burst particles replay with identical positions and lifetimes");
        }
    }

    config.stellar_automatic = true;
    config.stellar_interval_min = config.stellar_interval_max = 5.0;
    Universe first(config), second(config);
    const std::vector<Body> seeds{body(1, {750.0, 540.0}), body(2, {1100.0, 540.0}), body(3, {400.0, 300.0}, {12.0, 0.0})};
    first.reset(seeds, {Region{}}, center);
    second.reset(seeds, {Region{}}, center);
    advance(first, 4.0);
    advance(second, 4.0);
    check(std::all_of(first.bodies().begin(), first.bodies().end(), [](const Body& current) { return current.nova_age < 0.0; }),
          "automatic stellar explosions never begin before their minimum interval");
    Universe automatic_checkpoint = first;
    advance(first, 1.4);
    advance(second, 1.4);
    std::size_t charging = 0;
    for (std::size_t index = 0; index < first.bodies().size(); ++index) {
        const Body a = first.bodies()[index], b = second.bodies()[index];
        charging += a.nova_age >= 0.0;
        check(close(a.nova_age, b.nova_age) && a.nova_seed == b.nova_seed,
              "identically seeded automatic schedules pick the same source and cinematic seed");
    }
    check(charging == 1, "automatic scheduler picks exactly one eligible source per event");
    // The reset frame plus exact 30 Hz sampling makes the checkpoint's history
    // size a clock unaffected by whichever moving source the scheduler chose.
    first.setRewinding(true);
    for (int frame = 0; frame < 100 && first.historyFrames() > automatic_checkpoint.historyFrames(); ++frame)
        first.step(1.0 / config.history_hz, center);
    first.setRewinding(false);
    for (int frame = 0; frame < 180; ++frame) {
        first.step(1.0 / 120.0, center);
        automatic_checkpoint.step(1.0 / 120.0, center);
        for (std::size_t index = 0; index < first.bodies().size(); ++index) {
            const Body a = first.bodies()[index], b = automatic_checkpoint.bodies()[index];
            check(close(a.nova_age, b.nova_age) && a.nova_seed == b.nova_seed && a.stored == b.stored,
                  "rewind restores the automatic scheduler so future selection and timing replay identically");
        }
    }
    Universe lonely(config);
    lonely.reset({body(1, center)}, {Region{}}, center);
    advance(lonely, 12.0);
    check(lonely.bodies()[0].nova_age < 0.0 && !lonely.bodies()[0].stored,
          "automatic stellar events preserve the last intact window instead of emptying the whole scene");
    checkFinite(first);
    checkFinite(lonely);
}

void stellarHistoryBudgetAndNewLivingIds() {
    constexpr std::size_t mib = 1024U * 1024U;
    auto config = inert();
    config.max_bodies = 1;
    config.history_seconds = 60.0;
    config.history_hz = 60.0;
    config.history_bytes = mib;
    Universe budget(config);
    budget.reset({body(1, {960.0, 540.0})}, {Region{}}, {});
    // Each frame owns a body vector, scheduler fields and a complete RNG state;
    // even this minimum excludes padding and therefore gives a safe upper
    // bound, independent of private Frame layout changes.
    const std::size_t minimum_frame_bytes = sizeof(Body) + sizeof(std::vector<Body>) +
        2U * sizeof(double) + sizeof(uint64_t) + sizeof(std::mt19937_64);
    const auto small_limit = budget.historyLimit();
    check(small_limit >= 2 && small_limit <= mib / minimum_frame_bytes,
          "a 1 MiB history budget accounts for per-frame RNG state and scheduler overhead, not only body storage");
    check(small_limit < 350, "small histories cannot allocate all 3601 requested frames after stellar RNG snapshots grow");
    advance(budget, 60.0);
    check(budget.historyFrames() == small_limit, "a long-running 1 MiB history stops at its true memory-derived frame cap");
    config.history_bytes = 2U * mib;
    budget.configure(config);
    check(budget.historyLimit() > small_limit && budget.historyLimit() <= 2U * mib / minimum_frame_bytes,
          "raising the history budget permits more frames while preserving full snapshot memory accounting");
    config.history_bytes = 0;
    budget.configure(config);
    check(budget.config().history_bytes == mib && budget.historyFrames() <= budget.historyLimit(),
          "an invalid zero-byte history budget is safely clamped and immediately trims existing frames");
    config.history_bytes = std::numeric_limits<std::size_t>::max();
    budget.configure(config);
    check(budget.config().history_bytes == 64U * mib, "native history memory is capped at 64 MiB even for extreme inputs");

    config = inert();
    config.black_hole = false;
    config.stellar_charge_seconds = 1.0;
    config.stellar_fragment_seconds = 1.0;
    config.history_seconds = 8.0;
    Universe universe(config);
    universe.reset({body(1, {960.0, 540.0})}, {Region{}}, {});
    check(universe.startStellarNova(uint64_t{1}), "new-id rewind regression starts the original stellar source");
    advance(universe, 2.5);
    check(universe.bodies()[0].stored && universe.novaFragments(universe.bodies()[0]).size() == 16,
          "the original stellar blast settles with persistent pieces before another real app arrives");
    check(universe.addBody(body(2, {1400.0, 700.0}), false) && universe.startStellarNova(uint64_t{2}),
          "a later actual app may begin its own stellar event while earlier fragments keep drifting");
    advance(universe, 0.2);
    universe.setRewinding(true);
    bool restored_original_charge = false;
    for (int frame = 0; frame < 150 && universe.rewinding(); ++frame) {
        universe.step(1.0 / config.history_hz, {});
        std::size_t busy = 0;
        for (const auto& current : universe.bodies())
            busy += current.nova_age >= 0.0 && current.nova_age < current.nova_charge_seconds + current.nova_fragment_seconds;
        check(busy <= 1, "rewinding before a later app's creation never overlays its future stellar charge on the historical one");
        check(findBody(universe, 2).fragment_grid == 0,
              "rewind cancellation preserves the newer actual app id rather than deleting its living window");
        if (universe.bodies()[0].nova_age >= 0.0 && universe.bodies()[0].nova_age < universe.bodies()[0].nova_charge_seconds) {
            restored_original_charge = true;
            check(findBody(universe, 2).nova_age < 0.0 && !findBody(universe, 2).stored,
                  "a living app absent from the old snapshot resumes as an intact nonstellar image");
            break;
        }
    }
    check(restored_original_charge, "new-id regression really rewinds across creation into the earlier source's charge");
    checkFinite(universe);
}

void stellarSchedulerAfterLongUptime() {
    auto config = inert();
    config.fixed_step = 1.0 / 30.0;
    config.max_substeps = 1;
    config.history_seconds = 0.0;
    config.rewind = false;
    config.max_particles = 0;
    config.black_hole = false;
    config.stellar_automatic = true;
    config.stellar_interval_min = config.stellar_interval_max = 5.0;
    Universe universe(config);
    universe.reset({body(1, {750.0, 540.0})}, {Region{}}, {});
    // Exercise actual integration beyond the presentation clock's ten-hour
    // cap: passing one huge elapsed delta would intentionally discard the
    // interval and would not test the scheduler's long-uptime behavior.
    constexpr int frames = (36000 + 10) * 30;
    for (int frame = 0; frame < frames; ++frame)
        universe.step(config.fixed_step, {});
    check(universe.bodies()[0].nova_age < 0.0 && universe.historyFrames() == 0,
          "ten hours of lone-window uptime do not fabricate explosions or retained history");
    check(universe.addBody(body(2, {1200.0, 540.0}), false), "another real image can arrive after ten hours of Cosmic uptime");
    bool event_started = false;
    for (int frame = 0; frame < 180 && !event_started; ++frame) {
        universe.step(config.fixed_step, {});
        event_started = std::any_of(universe.bodies().begin(), universe.bodies().end(),
                                   [](const Body& current) { return current.nova_age >= 0.0; });
    }
    check(event_started, "automatic stellar countdown still fires after the capped ten-hour presentation clock stops advancing");
    checkFinite(universe);
}

void persistentFragmentPhysics() {
    auto config = inert();
    config.max_bodies = 1;
    config.max_objects = 16;
    config.black_hole = false;
    config.collisions = true;
    config.collision_strength = 0.0; // Keep boundary reflections, isolate pair forces below.
    config.restitution = 1.0;
    config.stellar_charge_seconds = 0.5;
    config.stellar_fragment_seconds = 1.0;
    config.history_seconds = 12.0;
    const Vec2 center{960.0, 540.0};
    Universe universe(config);
    Body source = body(1, center);
    source.width = 640.0;
    source.height = 360.0;
    universe.reset({source}, {Region{}}, center);
    const double source_mass = universe.bodies()[0].mass;
    check(universe.startStellarNova(uint64_t{1}), "persistent-piece test starts an intact source");
    advance(universe, 0.55);
    check(universe.objectCount() == 16 && universe.bodies().size() == 17,
          "one exploded image becomes sixteen actors plus its non-counting remnant");
    double fragment_mass = 0.0;
    std::vector<uint64_t> identities;
    for (const auto& current : universe.bodies()) {
        if (current.fragment_grid == 0) continue;
        identities.push_back(current.id);
        fragment_mass += current.mass;
        check(current.source_id == 1 && current.nova_age < 0.0 &&
              close(current.width, source.width / 4.0) && close(current.height, source.height / 4.0),
              "each physical piece retains its source and a quarter-sized image rectangle");
        check(!universe.startStellarNova(current.id), "a physical piece cannot be selected for another stellar explosion");
    }
    check(close(fragment_mass, source_mass), "subdivision conserves the source's physical mass");
    bool reflected_x = false, reflected_y = false;
    for (int frame = 0; frame < 4200; ++frame) {
        std::vector<Vec2> previous;
        for (const auto id : identities) previous.push_back(findBody(universe, id).velocity);
        universe.step(1.0 / 120.0, center);
        for (std::size_t index = 0; index < identities.size(); ++index) {
            const auto& current = findBody(universe, identities[index]);
            reflected_x |= previous[index].x * current.velocity.x < -1e-6;
            reflected_y |= previous[index].y * current.velocity.y < -1e-6;
            const double cosine = std::abs(std::cos(current.angle)), sine = std::abs(std::sin(current.angle));
            const double half_x = (current.width * cosine + current.height * sine) * current.scale * 0.5;
            const double half_y = (current.width * sine + current.height * cosine) * current.scale * 0.5;
            check(!current.stored && current.position.x >= half_x - 1e-7 && current.position.x <= 1920.0 - half_x + 1e-7 &&
                  current.position.y >= half_y - 1e-7 && current.position.y <= 1080.0 - half_y + 1e-7,
                  "persistent pieces remain in the normal reflective world bounds instead of escaping or expiring");
        }
    }
    check(reflected_x && reflected_y, "persistent pieces visibly bounce on both horizontal and vertical monitor edges");
    const auto visible = universe.novaFragments(universe.bodies()[0]);
    check(visible.size() == 16 && std::all_of(visible.begin(), visible.end(),
                                           [](const NovaFragment& piece) { return close(piece.alpha, 1.0); }),
          "all sixteen pieces remain fully opaque beyond thirty seconds, not lifetime-faded particles");
    for (const auto id : identities)
        check(close(findBody(universe, id).scale, universe.bodies()[0].nova_initial_scale) &&
              close(findBody(universe, id).fragment_age, config.stellar_fragment_seconds),
              "initial expansion and blast age settle to bounded ordinary-window scale without removing the piece");

    Universe attracted = universe, baseline = universe;
    auto gravity = config;
    gravity.cursor_gravity = gravity.orbit = gravity.binary = true;
    gravity.cursor_strength = 5000000.0;
    attracted.configure(gravity);
    advance(attracted, 0.25, center);
    advance(baseline, 0.25, center);
    bool accelerated = false;
    for (const auto id : identities)
        accelerated |= magnitude(findBody(attracted, id).velocity - findBody(baseline, id).velocity) > 0.1;
    check(accelerated, "persistent pieces participate in the same cursor and mutual gravity as intact windows");

    Universe sinking = universe;
    auto with_sink = config;
    with_sink.black_hole = true;
    sinking.configure(with_sink);
    const auto& candidate = sinking.bodies().back();
    const Vec2 selected_screen = sinking.worldToScreen(candidate.position, candidate.region);
    const uint64_t selected = sinking.hitTest(selected_screen, candidate.region);
    check(selected != 0 && findBody(sinking, selected).fragment_grid != 0,
          "the normal cursor hit test can select a small piece rather than its stored parent");
    check(!sinking.startStellarNova(selected_screen, candidate.region), "F5 on a displayed physical piece never explodes it again");
    const auto reserved = sinking.objectCount();
    check(sinking.startBlackHole(selected_screen, candidate.region), "F7 uses ordinary cinematic absorption for a selected piece");
    advance(sinking, 8.0);
    check(findBody(sinking, selected).stored && sinking.objectCount() == reserved,
          "absorbed pieces retain their identities and reserved budget slots for rewind");
    sinking.setRewinding(true);
    for (int frame = 0; frame < 300 && (findBody(sinking, selected).stored || findBody(sinking, selected).sink_progress > 0.0); ++frame)
        sinking.step(1.0 / with_sink.history_hz, center);
    check(!findBody(sinking, selected).stored && findBody(sinking, selected).sink_progress == 0.0 &&
          findBody(sinking, selected).fragment_grid == 4 && sinking.objectCount() == reserved,
          "F8 restores the same physical crop before its F7 absorption without duplicating actors");
    checkFinite(universe);
    checkFinite(sinking);

    // A compact source makes neighboring tiles overlap their collision circles
    // at birth. Compare two otherwise-identical simulations with pair impulses
    // on/off, preserving the same boundary rule in each.
    config.max_speed = 10.0;
    config.collisions = false;
    universe.configure(config);
    source.width = 40.0;
    source.height = 24.0;
    universe.reset({source}, {Region{}}, center);
    check(universe.startStellarNova(uint64_t{1}), "piece collision regression starts a compact source");
    advance(universe, 0.5);
    Universe colliding = universe, collision_free = universe;
    config.collisions = true;
    config.collision_strength = 1.0;
    colliding.configure(config);
    config.collision_strength = 0.0;
    collision_free.configure(config);
    advance(colliding, 0.05);
    advance(collision_free, 0.05);
    bool separated = false;
    for (const auto& current : colliding.bodies()) if (current.fragment_grid != 0)
        separated |= magnitude(current.position - findBody(collision_free, current.id).position) > 0.1;
    check(separated, "small pieces use ordinary collision separation and impulses rather than passing through each other");
}

void persistentFragmentPortals() {
    auto config = inert();
    config.black_hole = false;
    config.wormholes = true;
    config.stellar_charge_seconds = 0.5;
    config.stellar_fragment_seconds = 1.0;
    config.stellar_growth = 1.05;
    config.wormhole_cooldown = 0.2;
    config.max_speed = 40.0;
    const Region source_region{42, 0.0, 0.0, 1920.0, 1080.0};
    const Region destination{1000042, 0.0, 0.0, 1920.0, 1080.0};
    Universe universe(config);
    universe.reset({}, {source_region, destination}, {});
    Body source = body(1, universe.wormholes().front().position);
    source.region = source_region.id;
    source.width = 80.0;
    source.height = 50.0;
    universe.reset({source}, {source_region, destination}, {});
    check(universe.startStellarNova(uint64_t{1}), "physical-piece portal test charges a source at a real mouth");
    uint64_t traveler = 0;
    for (int frame = 0; frame < 600 && traveler == 0; ++frame) {
        universe.step(1.0 / 120.0, {});
        for (const auto& current : universe.bodies())
            if (current.fragment_grid != 0 && current.portal_progress > 0.0) { traveler = current.id; break; }
    }
    check(traveler != 0 && findBody(universe, traveler).region == source_region.id && !findBody(universe, traveler).stored,
          "after its initial safety interval a physical piece naturally enters an ordinary wormhole");
    const Body entered = findBody(universe, traveler);
    Universe canceled = universe;
    auto disabled = config;
    disabled.wormholes = false;
    canceled.configure(disabled);
    check(findBody(canceled, traveler).portal_progress == 0.0 &&
          close(findBody(canceled, traveler).scale, entered.portal_start_scale) &&
          findBody(canceled, traveler).fragment_column == entered.fragment_column &&
          findBody(canceled, traveler).fragment_row == entered.fragment_row,
          "disabling wormholes restores the same piece and crop rather than a full-size source image");
    for (int frame = 0; frame < 12000 && findBody(universe, traveler).portal_progress > 0.0; ++frame)
        universe.step(1.0 / 120.0, {});
    const Body transferred = findBody(universe, traveler);
    check(transferred.portal_progress == 0.0 && transferred.region == destination.id && !transferred.stored &&
          close(transferred.scale, entered.portal_start_scale) && transferred.source_id == source.id &&
          transferred.fragment_grid == 4 && transferred.fragment_column == entered.fragment_column &&
          transferred.fragment_row == entered.fragment_row,
          "wormhole emergence preserves a piece's source texture crop, physical identity and small size in the other universe");
    check(!universe.startStellarNova(traveler), "a wormhole-transferred piece still cannot become a stellar source");
    checkFinite(universe);
}

void persistentFragmentBudgets() {
    auto config = inert();
    config.black_hole = false;
    config.max_bodies = 8;
    config.max_objects = 19;
    config.stellar_charge_seconds = 0.5;
    config.stellar_fragment_seconds = 1.0;
    std::vector<Body> sources{body(1, {500.0, 300.0}), body(2, {1400.0, 300.0}),
                              body(3, {500.0, 780.0}), body(4, {1400.0, 780.0})};
    Universe universe(config);
    universe.reset(sources, {Region{}}, {});
    check(universe.startStellarNova(uint64_t{1}), "a sixteen-way split is accepted when its net fifteen new actors exactly fit");
    check(universe.objectCount() == 4 && !universe.addBody(body(5, {960.0, 540.0}), false),
          "an in-progress charge reserves all future tile slots so a new real image cannot overbook its burst");
    advance(universe, 1.6);
    check(universe.objectCount() == 19 && !universe.startStellarNova(uint64_t{2}) && findBody(universe, 2).nova_age < 0.0,
          "F5 rejects a split before changing the intact source when the resulting population would exceed its cap");
    auto automatic = config;
    automatic.stellar_automatic = true;
    automatic.stellar_interval_min = automatic.stellar_interval_max = 5.0;
    universe.configure(automatic);
    advance(universe, 12.0);
    check(universe.objectCount() == 19 && findBody(universe, 2).nova_age < 0.0 &&
          findBody(universe, 3).nova_age < 0.0 && findBody(universe, 4).nova_age < 0.0,
          "automatic selection obeys the same capacity check and never substitutes physical pieces as stellar sources");
    auto lower = config;
    lower.max_objects = 16;
    universe.configure(lower);
    check(universe.objectCount() == 19 && universe.novaFragments(findBody(universe, 1)).size() == 16 &&
          !universe.startStellarNova(uint64_t{2}) && !universe.addBody(body(5, {960.0, 540.0}), false),
          "lowering the actor budget preserves existing pieces and blocks new allocations instead of silently deleting tiles");
    universe.removeBody(1);
    check(universe.objectCount() == 3 && universe.bodies().size() == 3,
          "closing one real source removes all of its physical pieces and remnant without touching other apps");
    universe.configure(config);
    check(universe.startStellarNova(uint64_t{2}), "capacity released by a closed source becomes available for a later split");

    config.max_objects = 64;
    universe.configure(config);
    universe.reset({sources[0]}, {Region{}}, {});
    check(universe.startStellarNova(uint64_t{1}), "automatic piece exclusion test explodes its only intact image manually");
    advance(universe, 1.6);
    automatic = config;
    automatic.stellar_automatic = true;
    automatic.stellar_interval_min = automatic.stellar_interval_max = 5.0;
    universe.configure(automatic);
    advance(universe, 12.0);
    check(universe.objectCount() == 16 && universe.novaFragments(findBody(universe, 1)).size() == 16 &&
          std::all_of(universe.bodies().begin(), universe.bodies().end(),
                      [](const Body& current) { return current.fragment_grid == 0 || current.nova_age < 0.0; }),
          "a universe containing only pieces never auto-explodes them even when many free actor slots remain");
    check(universe.addBody(sources[1], false), "an intact app can join a scene already containing persistent pieces");
    advance(universe, 12.0);
    check(universe.objectCount() == 17 && findBody(universe, 2).nova_age < 0.0 && !findBody(universe, 2).stored,
          "physical pieces do not count as spare intact sources when automatic explosions preserve the last whole window");

    for (const std::size_t grid_count : {4U, 16U}) {
        config.max_objects = grid_count == 4 ? 16 : 64;
        config.stellar_fragments = grid_count;
        universe.configure(config);
        universe.reset(sources, {Region{}}, {});
        for (uint64_t id = 1; id <= 4; ++id) {
            check(universe.startStellarNova(id), "each intact window may split while settled older pieces remain alive");
            advance(universe, 1.6);
            check(universe.objectCount() == 4 + id * (grid_count - 1), "actor capacity counts each replacement's net piece increase exactly");
        }
        check(universe.objectCount() == config.max_objects && !universe.addBody(body(5, {960.0, 540.0}), false),
              "four source windows fill the exact configured cap with four-way or sixteen-way persistent pieces");
        const auto before = universe.objectCount();
        for (const auto& current : universe.bodies())
            check(!universe.startStellarNova(current.id), "neither stored remnants nor physical pieces can subdivide again at a full cap");
        check(universe.objectCount() == before, "rejected repeat explosions never remove or multiply existing actors");
        checkFinite(universe);
    }

    config.max_objects = 16;
    config.stellar_fragments = 16;
    universe.configure(config);
    universe.reset({sources[0], sources[1]}, {Region{}}, {});
    check(!universe.startStellarNova(uint64_t{1}) && findBody(universe, 1).nova_age < 0.0,
          "a two-window scene cannot initiate a sixteen-way split under a sixteen-actor budget");
    config.max_objects = 64;
    universe.configure(config);
    universe.reset({sources[0]}, {Region{}}, {});
    check(universe.startStellarNova(uint64_t{1}), "source identity collision test first creates actual pieces");
    advance(universe, 1.6);
    const Body original_piece = universe.bodies().back();
    check(universe.addBody(body(original_piece.id, {1400.0, 780.0}), false),
          "a newly-created real app can use an opaque id previously allocated to a virtual fragment");
    check(findBody(universe, original_piece.id).fragment_grid == 0,
          "opaque real ids take precedence and never become aliases to an unrelated physical piece");
    const auto relocated = std::find_if(universe.bodies().begin(), universe.bodies().end(), [&](const Body& current) {
        return current.fragment_grid == original_piece.fragment_grid && current.source_id == original_piece.source_id &&
               current.fragment_column == original_piece.fragment_column && current.fragment_row == original_piece.fragment_row;
    });
    check(relocated != universe.bodies().end() && relocated->id != original_piece.id && universe.objectCount() == 17,
          "colliding virtual ids are relocated without losing a piece or its reserved actor slot");
    universe.setRewinding(true);
    advance(universe, 0.4);
    check(findBody(universe, original_piece.id).fragment_grid == 0 && universe.objectCount() == 17,
          "history remaps colliding fragment identities without resurrecting an alias over the still-living real app");
    universe.removeBody(1);
    advance(universe, 8.0);
    check(universe.bodies().size() == 1 && universe.bodies()[0].id == original_piece.id && universe.objectCount() == 1,
          "closing and rewinding a fragmented source cannot resurrect any of its pieces or remove the unrelated real-id collision app");
    config.max_objects = 0;
    universe.configure(config);
    check(universe.config().max_objects == 16, "invalid zero actor budgets normalize to the safe native lower bound");
    config.max_objects = std::numeric_limits<std::size_t>::max();
    universe.configure(config);
    check(universe.config().max_objects == 128, "extreme direct-native actor budgets remain bounded by the hard one-hundred-twenty-eight limit");
}

void fragmentPhysicsBenchmark() {
    using Clock = std::chrono::steady_clock;
    for (const std::size_t target : {64U, 128U}) {
        auto config = inert();
        config.black_hole = false;
        config.max_bodies = target / 16;
        config.max_objects = target;
        config.stellar_charge_seconds = 0.5;
        config.stellar_fragment_seconds = 1.0;
        config.history_seconds = 12.0;
        std::vector<Body> sources;
        for (std::size_t index = 0; index < target / 16; ++index) {
            auto current = body(index + 1, {350.0 + static_cast<double>(index % 4) * 400.0,
                                           350.0 + static_cast<double>(index / 4) * 350.0});
            current.width = 320.0;
            current.height = 180.0;
            sources.push_back(current);
        }
        Universe universe(config);
        universe.reset(sources, {Region{}}, {960.0, 540.0});
        for (const auto& source : sources) {
            check(universe.startStellarNova(source.id), "benchmark source fits the requested actor population");
            advance(universe, 1.6);
        }
        config.cursor_gravity = config.orbit = config.binary = config.collisions = config.wormholes = true;
        config.damping = 0.012;
        config.collision_strength = 0.35;
        universe.configure(config);
        advance(universe, 1.0);
        check(universe.objectCount() == target, "benchmark retains the full physical-piece population");
        constexpr int samples = 10000;
        std::vector<double> timings;
        timings.reserve(samples);
        double total = 0.0;
        for (int sample = 0; sample < samples; ++sample) {
            const Vec2 cursor{960.0 + std::sin(sample * 0.013) * 500.0, 540.0 + std::cos(sample * 0.019) * 250.0};
            const auto start = Clock::now();
            universe.step(config.fixed_step, cursor);
            const double elapsed = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
            timings.push_back(elapsed);
            total += elapsed;
        }
        std::sort(timings.begin(), timings.end());
        std::cout << "Physical pieces=" << target << ", samples=" << samples << ", mean_us=" << total / samples
                  << ", p50_us=" << timings[samples / 2] << ", p95_us=" << timings[samples * 95 / 100]
                  << ", history_frames=" << universe.historyFrames() << '/' << universe.historyLimit()
                  << ", estimated_two_step_60Hz_mean_us=" << 2.0 * total / samples << '\n';
        checkFinite(universe);
    }
    std::cout << "CPU physics only; this benchmark does not measure capture, GPU drawing or end-to-end FPS.\n";
}

void expansionAndStress() {
    auto config = inert();
    config.expansion = true;
    config.expansion_rate = 0.1;
    Universe universe(config);
    universe.reset({body(1, {700.0, 540.0}, {-10.0, 0.0}), body(2, {1200.0, 540.0}, {10.0, 0.0})}, {Region{}}, {});
    const double before = magnitude(universe.bodies()[1].position - universe.bodies()[0].position);
    advance(universe, 30.0);
    const double after = magnitude(universe.bodies()[1].position - universe.bodies()[0].position);
    check(after > before * 2.0, "expansion grows virtual body separation");
    check(universe.cameras().front().zoom < 0.9, "expansion camera zooms out to keep bodies observable");
    for (const auto& current : universe.bodies()) {
        const Vec2 screen = universe.worldToScreen(current.position, current.region);
        check(screen.x > -10.0 && screen.x < 1930.0 && screen.y > -10.0 && screen.y < 1090.0,
              "expanded bodies remain inside visible camera view");
    }

    config = Config::demo();
    config.max_bodies = 32;
    config.max_particles = 128;
    config.history_seconds = 2.0;
    config.cursor_strength = 20000000.0;
    config.mutual_strength = 2000000.0;
    config.softening = 10.0;
    universe.configure(config);
    std::vector<Body> crowded;
    for (uint64_t id = 1; id <= 32; ++id)
        crowded.push_back(body(id, {960.0, 540.0}, {static_cast<double>(id * 20), 20.0}));
    universe.reset(crowded, {Region{}}, {960.0, 540.0}, 1);
    for (int frame = 0; frame < 12000; ++frame) {
        if (frame % 97 == 0)
            universe.supernova({960.0, 540.0});
        if (frame % 521 == 0)
            universe.cycleGravity();
        if (frame == 300)
            universe.startBlackHole(universe.worldToScreen(universe.bodies()[0].position, 0));
        if (frame % 241 == 0)
            universe.step(3600.0, {960.0, 540.0});
        else
            universe.step(1.0 / 120.0, {960.0 + std::sin(frame * 0.005) * 600.0, 540.0 + std::cos(frame * 0.007) * 300.0});
        if (frame % 100 == 0)
            checkFinite(universe);
    }
    checkFinite(universe);
    config.history_seconds = 60.0;
    config.history_hz = 60.0;
    config.max_bodies = 128;
    universe.configure(config);
    check(universe.historyLimit() < 1000, "maximum history is additionally bounded by 16 MiB memory budget");

    // Reducing targets must not make existing large history allocations exceed
    // the memory estimate used for subsequent sampling.
    const auto previous_limit = universe.historyLimit();
    config.max_bodies = 1;
    universe.configure(config);
    check(universe.historyLimit() <= 16U * 1024U * 1024U / (sizeof(Body) * 32),
          "existing large frames retain their memory accounting after body cap decreases");
    check(universe.historyLimit() >= previous_limit, "smaller cap can relax history only within actual allocation budget");
    config.max_bodies = 128;

    // Invalid runtime input cannot introduce NaN into rendering transforms.
    config.cursor_strength = std::numeric_limits<double>::infinity();
    config.fixed_step = std::numeric_limits<double>::quiet_NaN();
    config.max_substeps = 1000000;
    universe.configure(config);
    Body corrupt = body(900, {std::numeric_limits<double>::quiet_NaN(), 0.0});
    corrupt.scale = std::numeric_limits<double>::infinity();
    corrupt.mass = -1.0;
    universe.addBody(corrupt, false);
    universe.step(1.0, {std::numeric_limits<double>::quiet_NaN(), 0.0});
    checkFinite(universe);
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
        fragmentPhysicsBenchmark();
        return 0;
    }
    fixedStepAndDeterminism();
    orbitsAndBinary();
    collisionAndBoundary();
    wormholesAndRegions();
    cinematicAutomaticPortals();
    blackHoleAndLivingHistory();
    cinematicBlackHole();
    supernovaAndCaps();
    cinematicStellarNova();
    stellarRemnantPhysics();
    stellarNovaHistoryAndScheduler();
    stellarHistoryBudgetAndNewLivingIds();
    stellarSchedulerAfterLongUptime();
    persistentFragmentPhysics();
    persistentFragmentPortals();
    persistentFragmentBudgets();
    expansionAndStress();
    std::cout << "Cosmic physics: " << assertions << " checks passed\n";
}
