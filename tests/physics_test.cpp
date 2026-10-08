#include "physics.hpp"

#include <algorithm>
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
Config inert() {
    Config config;
    config.cursor_gravity = config.orbit = config.binary = config.collisions = false;
    config.expansion = config.wormholes = false;
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
        check(magnitude(current.velocity) <= universe.config().max_speed + 1e-6, "speed cap enforced");
    }
    for (const auto& camera : universe.cameras())
        check(std::isfinite(camera.zoom) && camera.zoom >= 0.02 && camera.zoom <= 1.0, "camera zoom finite and bounded");
    check(universe.bodies().size() <= universe.config().max_bodies, "body cap enforced");
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

int main() {
    fixedStepAndDeterminism();
    orbitsAndBinary();
    collisionAndBoundary();
    wormholesAndRegions();
    cinematicAutomaticPortals();
    blackHoleAndLivingHistory();
    cinematicBlackHole();
    supernovaAndCaps();
    expansionAndStress();
    std::cout << "Cosmic physics: " << assertions << " checks passed\n";
}
