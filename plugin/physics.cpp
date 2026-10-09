#include "physics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cosmic {
namespace {
constexpr double PI = 3.14159265358979323846;
constexpr double TAU = 2.0 * PI;

double finiteClamp(double value, double low, double high, double fallback) {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}
double lengthSquared(Vec2 value) { return value.x * value.x + value.y * value.y; }
double length(Vec2 value) { return std::hypot(value.x, value.y); }
double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
double smoothProgress(double value, double begin) {
    const double progress = std::clamp((value - begin) / (1.0 - begin), 0.0, 1.0);
    return progress * progress * (3.0 - 2.0 * progress);
}
Vec2 rotate(Vec2 value, double angle) {
    const double cosine = std::cos(angle), sine = std::sin(angle);
    return {value.x * cosine - value.y * sine, value.x * sine + value.y * cosine};
}
Vec2 safeVector(Vec2 value, Vec2 fallback = {}) {
    return std::isfinite(value.x) && std::isfinite(value.y) ? value : fallback;
}
Vec2 limited(Vec2 value, double maximum) {
    value = safeVector(value);
    const double magnitude = length(value);
    return magnitude > maximum ? value * (maximum / magnitude) : value;
}
double radius(const Body& body) {
    return std::max(8.0, std::hypot(body.width, body.height) * body.scale * 0.30);
}
Vec2 renderedHalfExtent(const Body& body) {
    const double winding = std::min(PI * 0.25, std::abs(body.twist));
    const double extent = std::cos(winding) + std::sin(winding);
    const double local_x = body.width * body.scale * body.stretch * extent * 0.5;
    const double local_y = body.height * body.scale / body.stretch * extent * 0.5;
    const double cosine = std::abs(std::cos(body.angle)), sine = std::abs(std::sin(body.angle));
    return {local_x * cosine + local_y * sine, local_x * sine + local_y * cosine};
}
Vec2 softenedGravity(Vec2 distance, double strength, double softening) {
    const double squared = lengthSquared(distance) + softening * softening;
    return distance * (strength / (squared * std::sqrt(squared)));
}
uint64_t novaHash(uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}
double novaRandom(uint64_t value) {
    return static_cast<double>(novaHash(value) >> 11) / 9007199254740992.0;
}
bool actorEligible(const Body& body) {
    return !body.stored && body.sink_progress <= 0.0 && body.portal_progress <= 0.0 && body.nova_age < 0.0;
}
bool novaEligible(const Body& body) { return body.fragment_grid == 0 && actorEligible(body); }
bool novaRemnant(const Body& body) {
    return body.fragment_grid == 0 && body.nova_age >= body.nova_charge_seconds;
}
double segmentPortalEntry(Vec2 start, Vec2 end, Vec2 center, double portal_radius) {
    const Vec2 offset = start - center, travel = end - start;
    const double c = lengthSquared(offset) - portal_radius * portal_radius;
    if (c <= 0.0) return 0.0;
    const double a = lengthSquared(travel), b = dot(offset, travel);
    if (a < 1e-12 || b >= 0.0) return std::numeric_limits<double>::infinity();
    const double discriminant = b * b - a * c;
    if (discriminant < 0.0) return std::numeric_limits<double>::infinity();
    const double entry = (-b - std::sqrt(discriminant)) / a;
    return entry >= 0.0 && entry <= 1.0 ? entry : std::numeric_limits<double>::infinity();
}
} // namespace

Config Config::calm() { return {}; }

Config Config::demo() {
    Config result;
    result.cursor_strength = 2100000.0;
    result.mutual_strength = 36000.0;
    result.collision_strength = 0.8;
    result.restitution = 0.86;
    result.expansion_rate = 0.025;
    result.explosion_strength = 560.0;
    result.sink_duration = 2.2;
    return result;
}

Universe::Universe(Config config) : m_random(config.seed) {
    configure(config);
    reset({}, {Region{}}, {});
}

void Universe::configure(Config config) {
    config.fixed_step = finiteClamp(config.fixed_step, 1.0 / 240.0, 1.0 / 30.0, 1.0 / 120.0);
    config.max_substeps = std::clamp<std::size_t>(config.max_substeps, 1, 32);
    config.max_bodies = std::clamp<std::size_t>(config.max_bodies, 1, 128);
    config.max_objects = std::clamp<std::size_t>(config.max_objects, 16, 128);
    config.max_particles = std::min<std::size_t>(config.max_particles, 2048);
    config.history_seconds = finiteClamp(config.history_seconds, 0.0, 60.0, 12.0);
    config.history_hz = finiteClamp(config.history_hz, 1.0, 60.0, 30.0);
    config.history_bytes = std::clamp<std::size_t>(config.history_bytes, 1024U * 1024U, 64U * 1024U * 1024U);
    config.cursor_strength = finiteClamp(config.cursor_strength, 0.0, 20000000.0, 1400000.0);
    config.mutual_strength = finiteClamp(config.mutual_strength, 0.0, 2000000.0, 14000.0);
    config.softening = finiteClamp(config.softening, 10.0, 2000.0, 100.0);
    config.max_acceleration = finiteClamp(config.max_acceleration, 10.0, 10000.0, 1400.0);
    config.max_speed = finiteClamp(config.max_speed, 10.0, 5000.0, 950.0);
    config.damping = finiteClamp(config.damping, 0.0, 10.0, 0.012);
    config.restitution = finiteClamp(config.restitution, 0.0, 1.0, 0.68);
    config.collision_strength = finiteClamp(config.collision_strength, 0.0, 1.0, 0.35);
    config.expansion_rate = finiteClamp(config.expansion_rate, 0.0, 0.2, 0.006);
    config.sink_duration = finiteClamp(config.sink_duration, 0.3, 20.0, 2.8);
    config.wormhole_cooldown = finiteClamp(config.wormhole_cooldown, 0.2, 20.0, 2.0);
    config.explosion_strength = finiteClamp(config.explosion_strength, 0.0, 3000.0, 320.0);
    config.stellar_interval_min = finiteClamp(config.stellar_interval_min, 5.0, 1800.0, 70.0);
    config.stellar_interval_max = finiteClamp(config.stellar_interval_max, config.stellar_interval_min, 1800.0,
                                             std::max(config.stellar_interval_min, 130.0));
    config.stellar_charge_seconds = finiteClamp(config.stellar_charge_seconds, 0.5, 15.0, 3.5);
    config.stellar_fragment_seconds = finiteClamp(config.stellar_fragment_seconds, 1.0, 8.0, 3.0);
    config.stellar_growth = finiteClamp(config.stellar_growth, 1.05, 2.5, 1.7);
    config.stellar_fragments = config.stellar_fragments == 4 ? 4 : 16;
    const bool seed_changed = config.seed != m_config.seed;
    m_config = config;
    if (seed_changed)
        m_random.seed(m_config.seed);
    // max_bodies concerns captured apps, not their child actors. Never resize
    // the mixed actor vector: doing so would discard arbitrary persistent tiles.
    std::vector<uint64_t> excess_sources;
    std::size_t real_count = 0;
    for (const auto& body : m_bodies)
        if (body.fragment_grid == 0 && ++real_count > config.max_bodies)
            excess_sources.push_back(body.id);
    for (const auto id : excess_sources) removeBody(id);
    if (m_particles.size() > config.max_particles)
        m_particles.resize(config.max_particles);
    if (!config.supernova) {
        m_particles.clear();
        m_waves.clear();
        std::erase_if(m_bodies, [](const Body& body) { return body.fragment_grid != 0; });
        for (auto& frame : m_history)
            std::erase_if(frame.bodies, [](const Body& body) { return body.fragment_grid != 0; });
        for (auto& body : m_bodies) if (body.nova_age >= 0.0) cancelNova(body);
    }
    if (!config.wormholes) {
        for (auto& body : m_bodies) if (body.portal_progress > 0.0) cancelPortal(body);
        m_wormholes.clear();
    } else if (m_wormholes.empty() && !m_regions.empty())
        createWormholes();
    if (!config.rewind)
        m_rewinding = false;
    if (!config.binary && m_gravity_mode != GravityMode::Cursor)
        m_gravity_mode = GravityMode::Cursor;
    resizeHistory();
}

void Universe::reset(std::vector<Body> bodies, std::vector<Region> regions, Vec2 cursor, uint64_t focused_id) {
    m_bodies.clear();
    m_regions.clear();
    m_cameras.clear();
    m_particles.clear();
    m_waves.clear();
    m_history.clear();
    m_wormholes.clear();
    m_random.seed(m_config.seed);
    m_nova_serial = 0;
    m_next_fragment_id = UINT64_MAX;
    m_cursor = safeVector(cursor);
    m_focused_id = focused_id;
    m_accumulator = m_history_accumulator = m_rewind_accumulator = m_time = 0.0;
    scheduleNova();
    m_rewinding = false;
    m_gravity_mode = GravityMode::Cursor;
    for (auto region : regions) {
        if (m_regions.size() >= 32)
            break;
        if (std::any_of(m_regions.begin(), m_regions.end(), [&](const Region& old) { return old.id == region.id; }))
            continue;
        region.x = finiteClamp(region.x, -100000.0, 100000.0, 0.0);
        region.y = finiteClamp(region.y, -100000.0, 100000.0, 0.0);
        region.width = finiteClamp(region.width, 64.0, 32000.0, 1920.0);
        region.height = finiteClamp(region.height, 64.0, 32000.0, 1080.0);
        m_regions.push_back(region);
        m_cameras.push_back({region.id, region.center(), 1.0});
    }
    if (m_regions.empty()) {
        m_regions.push_back(Region{});
        m_cameras.push_back({0, m_regions.front().center(), 1.0});
    }
    for (auto body : bodies)
        addBody(body, false);
    createWormholes();
    updateCameras(0.0);
    recordFrame();
}

void Universe::normalizeBody(Body& body, bool initialize_velocity) {
    if (std::none_of(m_regions.begin(), m_regions.end(), [&](const Region& region) { return region.id == body.region; }))
        body.region = m_regions.front().id;
    const auto& region = regionFor(body.region);
    body.position = safeVector(body.position, region.center());
    body.position.x = std::clamp(body.position.x, region.center().x - region.width * 20.0, region.center().x + region.width * 20.0);
    body.position.y = std::clamp(body.position.y, region.center().y - region.height * 20.0, region.center().y + region.height * 20.0);
    body.width = finiteClamp(body.width, 1.0, 32000.0, 640.0);
    body.height = finiteClamp(body.height, 1.0, 32000.0, 480.0);
    body.mass = finiteClamp(body.mass, 0.1, 100.0, 1.0);
    body.scale = finiteClamp(body.scale, 0.01, 0.42, std::min(0.42, 450.0 / std::max(body.width, body.height)));
    body.scale = std::min(body.scale, 450.0 / std::max(body.width, body.height));
    body.original_scale = body.scale;
    body.nova_age = -1.0;
    body.stretch = finiteClamp(body.stretch, 1.0, 12.0, 1.0);
    body.twist = finiteClamp(body.twist, -8.0, 8.0, 0.0);
    body.angle = std::isfinite(body.angle) ? std::remainder(body.angle, TAU) : 0.0;
    body.cooldown = finiteClamp(body.cooldown, 0.0, 20.0, 0.0);
    body.sink_progress = finiteClamp(body.sink_progress, 0.0, 1.0, 0.0);
    body.sink_center = safeVector(body.sink_center, body.position);
    body.sink_start_offset = safeVector(body.sink_start_offset);
    body.sink_entry_axis = safeVector(body.sink_entry_axis, {1.0, 0.0});
    body.sink_arc = finiteClamp(body.sink_arc, 0.0, 100000.0, 0.0);
    body.sink_start_angle = finiteClamp(body.sink_start_angle, -TAU, TAU, body.angle);
    body.sink_turn_sign = body.sink_turn_sign < 0.0 ? -1.0 : 1.0;
    body.sink_duration = finiteClamp(body.sink_duration, 0.0, 40000.0, 0.0);
    body.sink_camera_center = safeVector(body.sink_camera_center, region.center());
    body.sink_camera_zoom = finiteClamp(body.sink_camera_zoom, 0.02, 1.0, 1.0);
    body.portal_progress = finiteClamp(body.portal_progress, 0.0, 1.0, 0.0);
    body.portal_entry_center = safeVector(body.portal_entry_center, region.center());
    body.portal_exit_center = safeVector(body.portal_exit_center, region.center());
    body.portal_start_offset = safeVector(body.portal_start_offset);
    body.portal_entry_axis = safeVector(body.portal_entry_axis, {1.0, 0.0});
    body.portal_exit_offset = safeVector(body.portal_exit_offset);
    body.portal_start_velocity = limited(body.portal_start_velocity, m_config.max_speed);
    body.portal_arc = finiteClamp(body.portal_arc, 0.0, 100000.0, 0.0);
    body.portal_start_angle = finiteClamp(body.portal_start_angle, -TAU, TAU, body.angle);
    body.portal_start_scale = finiteClamp(body.portal_start_scale, 0.01, 0.42, body.scale);
    body.portal_start_stretch = finiteClamp(body.portal_start_stretch, 1.0, 12.0, 1.0);
    body.portal_start_twist = finiteClamp(body.portal_start_twist, -8.0, 8.0, 0.0);
    body.portal_turn = finiteClamp(body.portal_turn, -TAU * 2.0, TAU * 2.0, 0.0);
    body.portal_turn_sign = body.portal_turn_sign < 0.0 ? -1.0 : 1.0;
    body.portal_entry_duration = finiteClamp(body.portal_entry_duration, m_config.fixed_step, 40000.0, 1.68);
    body.portal_exit_duration = finiteClamp(body.portal_exit_duration, m_config.fixed_step, 40000.0, 0.75);
    body.portal_source_camera_center = safeVector(body.portal_source_camera_center, region.center());
    body.portal_destination_camera_center = safeVector(body.portal_destination_camera_center, region.center());
    body.portal_source_camera_zoom = finiteClamp(body.portal_source_camera_zoom, 0.02, 1.0, 1.0);
    body.portal_destination_camera_zoom = finiteClamp(body.portal_destination_camera_zoom, 0.02, 1.0, 1.0);
    if (initialize_velocity) {
        if (body.id == m_focused_id)
            body.mass = std::min(100.0, body.mass * 3.5);
        if (lengthSquared(body.velocity) < 1.0) {
            const Vec2 offset = body.position - screenToWorld(m_cursor, body.region);
            const double distance = std::max(m_config.softening, length(offset));
            Vec2 tangent = length(offset) > 1.0 ? Vec2{-offset.y, offset.x} / length(offset)
                                              : rotate({1.0, 0.0}, randomUnit() * TAU);
            const double speed = m_config.orbit && m_config.cursor_gravity
                                     ? std::sqrt(m_config.cursor_strength / distance) * (0.85 + randomUnit() * 0.18)
                                     : 24.0 + randomUnit() * 28.0;
            body.velocity = tangent * speed;
        }
        body.angular_velocity = (randomUnit() - 0.5) * 0.20;
    }
    body.velocity = limited(body.velocity, m_config.max_speed);
    body.angular_velocity = finiteClamp(body.angular_velocity, -20.0, 20.0, 0.0);
}

bool Universe::addBody(Body body, bool explode) {
    const auto real_count = std::count_if(m_bodies.begin(), m_bodies.end(), [](const Body& current) {
        return current.fragment_grid == 0;
    });
    if (body.id == 0 || static_cast<std::size_t>(real_count) >= m_config.max_bodies ||
        reservedObjectCount() >= m_config.max_objects ||
        std::any_of(m_bodies.begin(), m_bodies.end(), [&](const Body& current) {
            return current.id == body.id && current.fragment_grid == 0;
        }))
        return false;
    // IDs supplied by the compositor are opaque: a later real window may use
    // an ID previously allocated to a fragment, even at UINT64_MAX.
    relocateFragmentId(body.id);
    body.source_id = body.id;
    body.fragment_grid = body.fragment_column = body.fragment_row = 0;
    body.fragment_age = 0.0;
    normalizeBody(body, true);
    m_bodies.push_back(body);
    if (explode)
        supernova(body.position, body.region);
    return true;
}

void Universe::removeBody(uint64_t id) {
    std::erase_if(m_bodies, [&](const Body& body) { return body.id == id || body.source_id == id; });
    // Removing IDs from all frames also releases their history memory promptly.
    for (auto& frame : m_history)
        std::erase_if(frame.bodies, [&](const Body& body) { return body.id == id || body.source_id == id; });
    if (m_focused_id == id)
        m_focused_id = 0;
}

std::size_t Universe::objectCount() const {
    return static_cast<std::size_t>(std::count_if(m_bodies.begin(), m_bodies.end(), [](const Body& body) {
        return !novaRemnant(body);
    }));
}

std::size_t Universe::reservedObjectCount() const {
    std::size_t count = objectCount();
    for (const auto& body : m_bodies)
        if (body.fragment_grid == 0 && body.nova_age >= 0.0 && body.nova_age < body.nova_charge_seconds)
            count += static_cast<std::size_t>(body.nova_grid * body.nova_grid - 1);
    return count;
}

uint64_t Universe::allocateFragmentId(uint64_t excluded_id) {
    for (;;) {
        const uint64_t candidate = m_next_fragment_id--;
        if (candidate == 0 || candidate == excluded_id) continue;
        const auto contains = [&](const auto& bodies) {
            return std::any_of(bodies.begin(), bodies.end(), [&](const Body& body) { return body.id == candidate; });
        };
        if (contains(m_bodies)) continue;
        if (std::any_of(m_history.begin(), m_history.end(), [&](const Frame& frame) { return contains(frame.bodies); })) continue;
        return candidate;
    }
}

void Universe::relocateFragmentId(uint64_t id) {
    const auto contains = [&](const auto& bodies) {
        return std::any_of(bodies.begin(), bodies.end(), [&](const Body& body) {
            return body.fragment_grid != 0 && body.id == id;
        });
    };
    if (!contains(m_bodies) && !std::any_of(m_history.begin(), m_history.end(), [&](const Frame& frame) {
        return contains(frame.bodies);
    })) return;
    const uint64_t replacement = allocateFragmentId(id);
    const auto relocate = [&](auto& bodies) {
        for (auto& body : bodies) if (body.fragment_grid != 0 && body.id == id) body.id = replacement;
    };
    relocate(m_bodies);
    for (auto& frame : m_history) relocate(frame.bodies);
}

const Region& Universe::regionFor(int id) const {
    const auto found = std::find_if(m_regions.begin(), m_regions.end(), [&](const Region& region) { return region.id == id; });
    return found == m_regions.end() ? m_regions.front() : *found;
}

const Camera* Universe::cameraFor(int id) const {
    const auto found = std::find_if(m_cameras.begin(), m_cameras.end(), [&](const Camera& camera) { return camera.region == id; });
    return found == m_cameras.end() ? nullptr : &*found;
}

Vec2 Universe::worldToScreen(Vec2 position, int region_id) const {
    const Camera* camera = cameraFor(region_id);
    return camera ? regionFor(region_id).center() + (position - camera->center) * camera->zoom : position;
}

Vec2 Universe::screenToWorld(Vec2 position, int region_id) const {
    const Camera* camera = cameraFor(region_id);
    return camera ? camera->center + (position - regionFor(region_id).center()) / camera->zoom : position;
}

uint64_t Universe::hitTest(Vec2 screen_position) const {
    return hitTestInRegion(screen_position, nullptr);
}

uint64_t Universe::hitTest(Vec2 screen_position, int visible_region) const {
    return hitTestInRegion(screen_position, &visible_region);
}

uint64_t Universe::hitTestInRegion(Vec2 screen_position, const int* visible_region) const {
    if (!std::isfinite(screen_position.x) || !std::isfinite(screen_position.y))
        return 0;
    for (auto iterator = m_bodies.rbegin(); iterator != m_bodies.rend(); ++iterator) {
        const Body& body = *iterator;
        if (visible_region && body.region != *visible_region)
            continue;
        if (body.stored || body.sink_progress > 0.0 || body.portal_progress > 0.0 || body.nova_age >= 0.0 || body.scale < 0.001)
            continue;
        const auto& region = regionFor(body.region);
        if (screen_position.x < region.x || screen_position.x > region.x + region.width ||
            screen_position.y < region.y || screen_position.y > region.y + region.height)
            continue;
        Vec2 local = rotate(screenToWorld(screen_position, body.region) - body.position, -body.angle);
        local = {local.x / (body.scale * body.stretch), local.y * body.stretch / body.scale};
        // The shader twists normalized coordinates radially. Apply its inverse
        // using the same radius so selection follows the visible texture.
        Vec2 normalized{local.x / body.width * 2.0, local.y / body.height * 2.0};
        normalized = rotate(normalized, -body.twist * std::min(1.0, length(normalized)));
        if (std::abs(normalized.x) <= 1.0 && std::abs(normalized.y) <= 1.0)
            return body.id;
    }
    return 0;
}

bool Universe::startBlackHole(Vec2 screen_cursor) {
    return startBlackHoleById(screen_cursor, hitTest(screen_cursor));
}

bool Universe::startBlackHole(Vec2 screen_cursor, int visible_region) {
    return startBlackHoleById(screen_cursor, hitTest(screen_cursor, visible_region));
}

bool Universe::startBlackHoleById(Vec2 screen_cursor, uint64_t id) {
    if (!m_config.black_hole || m_rewinding)
        return false;
    const auto found = std::find_if(m_bodies.begin(), m_bodies.end(), [&](const Body& body) { return body.id == id; });
    if (found == m_bodies.end())
        return false;
    if (!actorEligible(*found)) return false;
    found->sink_progress = std::numeric_limits<double>::epsilon();
    // This center is captured once: later mouse motion does not drag the hole.
    found->sink_center = screenToWorld(screen_cursor, found->region);
    found->original_scale = found->scale;
    found->sink_start_offset = found->position - found->sink_center;
    const double distance = length(found->sink_start_offset);
    found->sink_entry_axis = distance > 1e-6 ? found->sink_start_offset / distance
        : length(found->velocity) > 1e-6 ? found->velocity / length(found->velocity)
        : rotate({1.0, 0.0}, static_cast<double>(found->id % 4096) / 4096.0 * TAU);
    found->sink_start_angle = found->angle;
    const double momentum = cross(found->sink_start_offset, found->velocity);
    found->sink_turn_sign = std::abs(momentum) > 1e-6 ? (momentum < 0.0 ? -1.0 : 1.0)
                                                   : (found->id % 2 ? 1.0 : -1.0);
    const Camera* camera = cameraFor(found->region);
    found->sink_camera_center = camera ? camera->center : regionFor(found->region).center();
    found->sink_camera_zoom = camera ? camera->zoom : 1.0;
    const double visible_size = std::max(found->width, found->height) * found->scale * found->sink_camera_zoom;
    const auto& region = regionFor(found->region);
    const double excursion = std::min(std::clamp(visible_size * 0.18, 36.0, 100.0),
                                     std::min(region.width, region.height) * 0.10);
    // sin(pi*p)*(1-p) peaks at approximately 0.58. Even an exact center
    // selection gets a visible take-in arc, with no displacement at activation.
    found->sink_arc = excursion / (0.58 * found->sink_camera_zoom);
    // Conservative analytic speed bounds for the radial and tangential terms.
    // Extend only durations that would make the requested path exceed the cap.
    const double radial_bound = 2.0 * distance + (PI + 1.0) * found->sink_arc;
    const double angular_bound = 3.0 * PI * (0.34 * distance + 0.60 * found->sink_arc);
    found->sink_duration = std::max(m_config.sink_duration, std::hypot(radial_bound, angular_bound) / m_config.max_speed);
    found->cooldown = found->sink_duration + m_config.wormhole_cooldown;
    return true;
}

double Universe::randomUnit() {
    return std::generate_canonical<double, 53>(m_random);
}

bool Universe::novaBusy() const {
    return std::any_of(m_bodies.begin(), m_bodies.end(), [](const Body& body) {
        return body.nova_age >= 0.0 && body.nova_age < body.nova_charge_seconds + body.nova_fragment_seconds;
    });
}

void Universe::scheduleNova() {
    const double jitter = novaRandom(m_config.seed ^ novaHash(m_nova_serial));
    // A remaining duration, not an absolute deadline: the bounded expansion
    // clock may reach its ten-hour ceiling without stopping future events.
    m_next_nova = m_config.stellar_interval_min +
                  (m_config.stellar_interval_max - m_config.stellar_interval_min) * jitter;
}

bool Universe::startStellarNova(Vec2 screen_cursor, int visible_region) {
    return startStellarNova(hitTest(screen_cursor, visible_region));
}

bool Universe::startStellarNova(uint64_t id) {
    if (!m_config.supernova || m_rewinding || novaBusy()) return false;
    const auto selected = std::find_if(m_bodies.begin(), m_bodies.end(), [&](const Body& body) { return body.id == id; });
    if (selected == m_bodies.end() || !novaEligible(*selected)) return false;
    // Reserve the entire eventual split at activation. New windows and later
    // events cannot steal these slots during the visible charging interval.
    if (reservedObjectCount() + m_config.stellar_fragments - 1 > m_config.max_objects) return false;
    auto& body = *selected;
    body.nova_age = 0.0;
    body.nova_charge_seconds = m_config.stellar_charge_seconds;
    body.nova_fragment_seconds = m_config.stellar_fragment_seconds;
    body.nova_initial_scale = body.scale;
    body.nova_growth = m_config.stellar_growth;
    body.nova_start_stretch = body.stretch;
    body.nova_start_twist = body.twist;
    body.nova_grid = m_config.stellar_fragments == 4 ? 2 : 4;
    body.nova_seed = novaHash(m_config.seed ^ body.id ^ novaHash(++m_nova_serial));
    const Camera* camera = cameraFor(body.region);
    body.nova_camera_center = camera ? camera->center : regionFor(body.region).center();
    body.nova_camera_zoom = camera ? camera->zoom : 1.0;
    // Keep the selected star in place so its warning and eventual remnant
    // clearly belong together. Only the presentation stops, never the app.
    body.velocity = {};
    scheduleNova();
    return true;
}

void Universe::advanceNova(Body& body, double dt) {
    const double old_age = body.nova_age;
    body.nova_age = std::min(body.nova_charge_seconds + body.nova_fragment_seconds, old_age + dt);
    if (body.nova_age < body.nova_charge_seconds - 1e-12) {
        const double progress = body.nova_age / body.nova_charge_seconds;
        const double ease = smoothProgress(progress, 0.0);
        body.scale = body.nova_initial_scale * (1.0 + (body.nova_growth - 1.0) * ease);
        body.stretch = 1.0 + (body.nova_start_stretch - 1.0) * (1.0 - ease);
        body.twist = body.nova_start_twist * (1.0 - ease);
        body.angle = std::remainder(body.angle + body.angular_velocity * dt * (1.0 - ease), TAU);
    } else if (old_age < body.nova_charge_seconds) {
        body.nova_age = std::max(body.nova_age, body.nova_charge_seconds);
        body.stored = true;
        body.scale = 0.0;
        body.stretch = 1.0;
        body.twist = 0.0;
        body.velocity = {};
        supernova(body.position, body.region);
    }
}

void Universe::cancelNova(Body& body) {
    body.stored = false;
    body.scale = body.nova_initial_scale;
    body.stretch = body.nova_start_stretch;
    body.twist = body.nova_start_twist;
    body.nova_age = -1.0;
}

std::vector<NovaFragment> Universe::novaFragments(const Body& body) const {
    std::vector<NovaFragment> result;
    if (!m_config.supernova || body.fragment_grid != 0 || !novaRemnant(body)) return result;
    result.reserve(static_cast<std::size_t>(body.nova_grid * body.nova_grid));
    for (const auto& child : m_bodies) {
        if (child.fragment_grid == 0 || child.source_id != body.id || child.stored || child.scale < 0.0001) continue;
        result.push_back({child.position, child.angle, child.scale, 1.0,
                          child.fragment_column, child.fragment_row, child.fragment_grid});
    }
    return result;
}

void Universe::createNovaFragments(const Body& parent, std::vector<Body>& fragments) {
    const int grid = parent.nova_grid == 2 ? 2 : 4;
    const double burst_scale = parent.nova_initial_scale * parent.nova_growth;
    for (int row = 0; row < grid; ++row) for (int column = 0; column < grid; ++column) {
        const uint64_t key = parent.nova_seed ^ novaHash(static_cast<uint64_t>(row * grid + column));
        const Vec2 local{(static_cast<double>(column) + 0.5 - grid * 0.5) * parent.width * burst_scale / grid,
                         (static_cast<double>(row) + 0.5 - grid * 0.5) * parent.height * burst_scale / grid};
        const Vec2 offset = rotate(local, parent.angle);
        const double direction = std::atan2(offset.y, offset.x) + (novaRandom(key) - 0.5) * 0.45;
        const double speed = std::min(m_config.max_speed, 150.0 + novaRandom(key + 1) * 290.0);
        const double spin = (novaRandom(key + 2) - 0.5) * 5.5;
        Body child;
        child.id = allocateFragmentId();
        child.source_id = parent.id;
        child.fragment_grid = grid;
        child.fragment_column = column;
        child.fragment_row = row;
        child.position = parent.position + offset;
        child.velocity = rotate({speed, 0.0}, direction);
        child.width = parent.width / grid;
        child.height = parent.height / grid;
        child.mass = std::max(0.001, parent.mass / static_cast<double>(grid * grid));
        child.angle = parent.angle;
        child.angular_velocity = spin;
        child.scale = burst_scale;
        child.original_scale = parent.nova_initial_scale;
        child.region = parent.region;
        child.nova_initial_scale = parent.nova_initial_scale;
        child.nova_growth = parent.nova_growth;
        child.nova_fragment_seconds = parent.nova_fragment_seconds;
        child.cooldown = parent.nova_fragment_seconds + m_config.wormhole_cooldown;
        fragments.push_back(child);
    }
}

void Universe::supernova(Vec2 position, int region) {
    if (!m_config.supernova || m_rewinding)
        return;
    position = safeVector(position, regionFor(region).center());
    for (auto& body : m_bodies) {
        if (body.stored || body.region != region || body.sink_progress > 0.0 || body.portal_progress > 0.0 || body.nova_age >= 0.0)
            continue;
        Vec2 displacement = body.position - position;
        const double distance = length(displacement);
        Vec2 direction = distance > 1.0 ? displacement / distance : rotate({1.0, 0.0}, randomUnit() * TAU);
        const double impulse = m_config.explosion_strength / (1.0 + distance / 320.0);
        body.velocity = limited(body.velocity + direction * impulse, m_config.max_speed);
        body.angular_velocity = std::clamp(body.angular_velocity + (randomUnit() - 0.5) * 1.8, -4.0, 4.0);
    }
    if (m_waves.size() >= 16)
        m_waves.erase(m_waves.begin());
    m_waves.push_back({position, 8.0, 1.35, 1.35, region});
    const std::size_t count = std::min<std::size_t>(64, m_config.max_particles);
    if (m_particles.size() + count > m_config.max_particles) {
        const std::size_t discard = m_particles.size() + count - m_config.max_particles;
        m_particles.erase(m_particles.begin(), m_particles.begin() + static_cast<std::ptrdiff_t>(discard));
    }
    for (std::size_t index = 0; index < count; ++index) {
        const double angle = (static_cast<double>(index) + randomUnit()) / static_cast<double>(count) * TAU;
        const double lifetime = 0.8 + randomUnit() * 1.4;
        m_particles.push_back({position, rotate({80.0 + randomUnit() * 320.0, 0.0}, angle), lifetime,
                               lifetime, 1.3 + randomUnit() * 3.0, region, 0.08 + randomUnit() * 0.15});
    }
}

void Universe::createWormholes() {
    m_wormholes.clear();
    if (!m_config.wormholes || m_regions.empty())
        return;
    // All regions are linked into a ring. A lone region receives two mouths.
    if (m_regions.size() == 1) {
        const auto& region = m_regions.front();
        const double portal_radius = std::clamp(std::min(region.width, region.height) * 0.065, 22.0, 70.0);
        m_wormholes.push_back({{region.x + region.width * 0.18, region.y + region.height * 0.30}, portal_radius, 0.25, region.id, 1});
        m_wormholes.push_back({{region.x + region.width * 0.82, region.y + region.height * 0.70}, portal_radius, PI + 0.25, region.id, 0});
        return;
    }
    for (std::size_t index = 0; index < m_regions.size(); ++index) {
        const auto& region = m_regions[index];
        const auto& next = m_regions[(index + 1) % m_regions.size()];
        const double portal_radius = std::clamp(std::min(region.width, region.height) * 0.065, 22.0, 70.0);
        const double exit_radius = std::clamp(std::min(next.width, next.height) * 0.065, 22.0, 70.0);
        const std::size_t entrance = m_wormholes.size();
        m_wormholes.push_back({{region.x + region.width * 0.18, region.y + region.height * 0.32}, portal_radius,
                               0.35, region.id, entrance + 1});
        m_wormholes.push_back({{next.x + next.width * 0.82, next.y + next.height * 0.68}, exit_radius,
                               PI + 0.8, next.id, entrance});
    }
}

void Universe::beginPortal(Body& body, const Wormhole& entrance, const Wormhole& exit, Vec2 position) {
    body.position = position; // First swept intersection, not a skipped mouth.
    body.portal_progress = std::numeric_limits<double>::epsilon();
    body.portal_emerging = false;
    body.portal_source_region = entrance.region;
    body.portal_destination_region = exit.region;
    body.portal_entry_center = entrance.position;
    body.portal_exit_center = exit.position;
    body.portal_start_offset = body.position - entrance.position;
    const double distance = length(body.portal_start_offset);
    body.portal_entry_axis = distance > 1e-6 ? body.portal_start_offset / distance
        : length(body.velocity) > 1e-6 ? body.velocity / length(body.velocity)
        : rotate({1.0, 0.0}, static_cast<double>(body.id % 4096) / 4096.0 * TAU);
    body.portal_start_velocity = body.velocity;
    body.portal_start_angle = body.angle;
    body.portal_start_scale = body.scale;
    body.portal_start_stretch = body.stretch;
    body.portal_start_twist = body.twist;
    body.portal_turn = exit.angle - entrance.angle + PI;
    const double momentum = cross(body.portal_start_offset, body.velocity);
    body.portal_turn_sign = std::abs(momentum) > 1e-6 ? (momentum < 0.0 ? -1.0 : 1.0)
                                                    : (body.id % 2 ? 1.0 : -1.0);
    const Camera* source = cameraFor(entrance.region);
    const Camera* destination = cameraFor(exit.region);
    body.portal_source_camera_center = source ? source->center : regionFor(entrance.region).center();
    body.portal_source_camera_zoom = source ? source->zoom : 1.0;
    body.portal_destination_camera_center = destination ? destination->center : regionFor(exit.region).center();
    body.portal_destination_camera_zoom = destination ? destination->zoom : 1.0;
    const auto& region = regionFor(entrance.region);
    const double visible_size = std::max(body.width, body.height) * body.scale * body.portal_source_camera_zoom;
    const double excursion = std::min({std::clamp(visible_size * 0.13, 24.0, 64.0),
                                      std::min(region.width, region.height) * 0.08,
                                      entrance.radius * body.portal_source_camera_zoom * 0.80});
    body.portal_arc = excursion / (0.58 * body.portal_source_camera_zoom);
    const double radial_bound = 2.0 * distance + (PI + 1.0) * body.portal_arc;
    const double angular_bound = TAU * 1.7 * (distance + 0.60 * body.portal_arc);
    body.portal_entry_duration = std::max(std::max(1.4, std::min(m_config.sink_duration, 4.0) * 0.60),
                                          std::hypot(radial_bound, angular_bound) / m_config.max_speed);
    const Vec2 exit_velocity = rotate(body.portal_start_velocity, body.portal_turn);
    const Vec2 direction = length(exit_velocity) > 1e-6 ? exit_velocity / length(exit_velocity)
                                                     : rotate({1.0, 0.0}, exit.angle);
    body.portal_exit_offset = direction * (exit.radius + radius(body) * 0.30 + 5.0);
    body.portal_exit_duration = std::max(0.75, 1.5 * length(body.portal_exit_offset) / m_config.max_speed);
    body.cooldown = 0.0; // Ordinary detection is disabled until emergence ends.
}

void Universe::advancePortal(Body& body, double dt) {
    const Vec2 previous = body.position;
    const double duration = body.portal_entry_duration + body.portal_exit_duration;
    const double midpoint = body.portal_entry_duration / duration;
    body.portal_progress = std::min(1.0, body.portal_progress + dt / duration);
    if (!body.portal_emerging && body.portal_progress >= midpoint - 1e-12) {
        // Present one exactly invisible frame when changing logical regions.
        body.portal_progress = midpoint;
        body.portal_emerging = true;
        body.region = body.portal_destination_region;
        body.position = body.portal_exit_center;
        body.scale = 0.0;
        body.velocity = {};
        return;
    }
    if (!body.portal_emerging) {
        const double progress = body.portal_progress / midpoint;
        const double remaining = 1.0 - progress;
        const double turn = body.portal_turn_sign * TAU * (0.3 * progress + 0.7 * progress * progress);
        const Vec2 relative = body.portal_start_offset * (remaining * remaining) +
                              body.portal_entry_axis * (body.portal_arc * std::sin(PI * progress) * remaining);
        body.position = body.portal_entry_center + rotate(relative, turn);
        body.angle = std::remainder(body.portal_start_angle + body.portal_turn_sign * TAU *
                                   (0.6 * progress + 1.5 * progress * progress), TAU);
        const double scale = 1.0 - progress * progress;
        body.scale = body.portal_start_scale * scale * scale;
        body.stretch = m_config.spaghetti ? 1.0 + 1.5 * std::sin(PI * smoothProgress(progress, 0.15)) : 1.0;
        body.twist = m_config.spaghetti ? 2.4 * smoothProgress(progress, 0.25) : 0.0;
    } else {
        const double progress = (body.portal_progress - midpoint) / (1.0 - midpoint);
        const double ease = smoothProgress(progress, 0.0);
        body.position = body.portal_exit_center + body.portal_exit_offset * ease;
        body.scale = body.portal_start_scale * ease;
        body.angle = std::remainder(body.portal_start_angle + body.portal_turn +
                                   body.portal_turn_sign * TAU * (1.0 - progress) * (1.0 - progress), TAU);
        body.stretch = m_config.spaghetti ? 1.0 + 1.5 * (1.0 - ease) : 1.0;
        body.twist = m_config.spaghetti ? 2.4 * (1.0 - ease) : 0.0;
    }
    body.velocity = limited((body.position - previous) / dt, m_config.max_speed);
    if (body.portal_progress >= 1.0 - 1e-12) {
        body.portal_progress = 0.0;
        body.portal_emerging = false;
        body.scale = body.portal_start_scale;
        body.stretch = body.portal_start_stretch;
        body.twist = body.portal_start_twist;
        body.angle = std::remainder(body.portal_start_angle + body.portal_turn, TAU);
        body.velocity = limited(rotate(body.portal_start_velocity, body.portal_turn), m_config.max_speed);
        body.cooldown = m_config.wormhole_cooldown;
    }
}

void Universe::cancelPortal(Body& body) {
    body.velocity = limited(rotate(body.portal_start_velocity, body.portal_emerging ? body.portal_turn : 0.0), m_config.max_speed);
    body.angle = std::remainder(body.portal_start_angle + (body.portal_emerging ? body.portal_turn : 0.0), TAU);
    body.scale = body.portal_start_scale;
    body.stretch = body.portal_start_stretch;
    body.twist = body.portal_start_twist;
    body.portal_progress = 0.0;
    body.portal_emerging = false;
    body.cooldown = m_config.wormhole_cooldown;
}

void Universe::cycleGravity() {
    if (!m_config.binary) {
        m_gravity_mode = GravityMode::Cursor;
        return;
    }
    if (m_gravity_mode == GravityMode::Cursor)
        m_gravity_mode = GravityMode::Focused;
    else if (m_gravity_mode == GravityMode::Focused)
        setBinaryPreset();
    else
        m_gravity_mode = GravityMode::Cursor;
}

void Universe::setBinaryPreset() {
    if (!m_config.binary || m_bodies.size() < 2)
        return;
    Body* first = nullptr;
    Body* second = nullptr;
    for (auto& body : m_bodies) {
        if (body.stored || body.sink_progress > 0.0 || body.portal_progress > 0.0 || body.nova_age >= 0.0)
            continue;
        if (!first)
            first = &body;
        else if (body.region == first->region) {
            second = &body;
            break;
        }
    }
    if (!first || !second)
        return;
    const auto& region = regionFor(first->region);
    const Vec2 center = region.center();
    const double distance = std::max(radius(*first) + radius(*second) + 32.0,
                                     std::min(region.width, region.height) * 0.32);
    const double total_mass = first->mass + second->mass;
    const double orbit_speed = std::sqrt(m_config.mutual_strength * total_mass * distance * distance /
                                        std::pow(distance * distance + m_config.softening * m_config.softening, 1.5));
    first->position = center - Vec2{distance * second->mass / total_mass, 0.0};
    second->position = center + Vec2{distance * first->mass / total_mass, 0.0};
    first->velocity = limited({0.0, -orbit_speed * second->mass / total_mass}, m_config.max_speed);
    second->velocity = limited({0.0, orbit_speed * first->mass / total_mass}, m_config.max_speed);
    m_gravity_mode = GravityMode::Binary;
}

void Universe::integrate(double dt) {
    m_next_nova = std::max(0.0, m_next_nova - dt);
    if (m_config.supernova && m_config.stellar_automatic && m_next_nova <= 1e-12) {
        if (!novaBusy()) {
            std::vector<uint64_t> eligible;
            for (const auto& body : m_bodies) if (novaEligible(body)) eligible.push_back(body.id);
            // Do not automatically destroy the only remaining floating image.
            // Manual selection deliberately still allows that final star.
            if (eligible.size() >= 2) {
                const auto index = static_cast<std::size_t>(novaHash(m_config.seed ^ m_nova_serial) % eligible.size());
                if (!startStellarNova(eligible[index])) {
                    // A full scene is expected; retry at the normal rare event
                    // interval instead of checking an unfulfillable split 120Hz.
                    ++m_nova_serial;
                    scheduleNova();
                }
            } else {
                ++m_nova_serial;
                scheduleNova();
            }
        }
    }
    // Spawn before any indexed acceleration arrays or actor references exist.
    // Child bodies subsequently follow exactly the ordinary integration path.
    std::vector<Body> fragments;
    for (auto& body : m_bodies) {
        if (body.nova_age < 0.0) continue;
        const bool charging = body.nova_age < body.nova_charge_seconds;
        advanceNova(body, dt);
        if (charging && novaRemnant(body)) createNovaFragments(body, fragments);
    }
    m_bodies.insert(m_bodies.end(), fragments.begin(), fragments.end());
    std::vector<Vec2> acceleration(m_bodies.size());
    // A completed transit still keeps its promised exit velocity for this step.
    std::vector<bool> portal_updated(m_bodies.size(), false);
    for (std::size_t index = 0; index < m_bodies.size(); ++index) {
        const auto& body = m_bodies[index];
        if (body.stored || body.sink_progress > 0.0 || body.portal_progress > 0.0 || body.nova_age >= 0.0)
            continue;
        if (m_config.cursor_gravity && m_gravity_mode == GravityMode::Cursor)
            acceleration[index] += softenedGravity(screenToWorld(m_cursor, body.region) - body.position,
                                                   m_config.cursor_strength, m_config.softening);
        if (m_config.expansion)
            acceleration[index] += (body.position - regionFor(body.region).center()) * (m_config.expansion_rate * 0.14);
        if (m_config.black_hole) {
            for (const auto& remnant : m_bodies) {
                if (remnant.id == body.id || remnant.region != body.region ||
                    remnant.nova_age < remnant.nova_charge_seconds) continue;
                acceleration[index] += softenedGravity(remnant.position - body.position,
                                                       m_config.cursor_strength * 0.6, m_config.softening);
            }
        }
    }
    if (m_config.binary) {
        for (std::size_t first = 0; first < m_bodies.size(); ++first) {
            const auto& a = m_bodies[first];
            if (a.stored || a.sink_progress > 0.0 || a.portal_progress > 0.0 || a.nova_age >= 0.0)
                continue;
            for (std::size_t second = first + 1; second < m_bodies.size(); ++second) {
                const auto& b = m_bodies[second];
                if (b.stored || b.sink_progress > 0.0 || b.portal_progress > 0.0 || b.nova_age >= 0.0 || a.region != b.region)
                    continue;
                const bool focused_pair = m_gravity_mode == GravityMode::Focused &&
                                          (a.source_id == m_focused_id || b.source_id == m_focused_id);
                const double strength = m_config.mutual_strength * (focused_pair ? 8.0 : 1.0);
                const Vec2 force = softenedGravity(b.position - a.position, strength, m_config.softening);
                acceleration[first] += force * b.mass;
                acceleration[second] -= force * a.mass;
            }
        }
    }
    const double damping = std::exp(-m_config.damping * dt);
    for (std::size_t index = 0; index < m_bodies.size(); ++index) {
        auto& body = m_bodies[index];
        body.cooldown = std::max(0.0, body.cooldown - dt);
        if (body.nova_age >= 0.0) continue;
        if (body.fragment_grid != 0) {
            body.fragment_age = std::min(body.nova_fragment_seconds, body.fragment_age + dt);
            if (!body.stored && body.sink_progress <= 0.0 && body.portal_progress <= 0.0) {
                const double ease = smoothProgress(body.fragment_age / body.nova_fragment_seconds, 0.0);
                body.scale = body.nova_initial_scale * (1.0 + (body.nova_growth - 1.0) * (1.0 - ease));
            }
        }
        if (body.stored)
            continue;
        if (body.portal_progress > 0.0) {
            portal_updated[index] = true;
            advancePortal(body, dt);
            continue;
        }
        if (body.sink_progress > 0.0) {
            const double previous_scale = body.scale;
            const double duration = std::max(m_config.fixed_step, body.sink_duration);
            body.sink_progress = std::min(1.0, body.sink_progress + dt / duration);
            const double progress = body.sink_progress;
            const double remaining = 1.0 - progress;
            const double phase = body.sink_turn_sign * 3.0 * PI * (0.2 * progress + 0.8 * progress * progress);
            const double phase_derivative = body.sink_turn_sign * 3.0 * PI * (0.2 + 1.6 * progress);
            const double kick = std::sin(PI * progress) * remaining;
            const double kick_derivative = PI * std::cos(PI * progress) * remaining - std::sin(PI * progress);
            const Vec2 relative = body.sink_start_offset * (remaining * remaining) + body.sink_entry_axis * (body.sink_arc * kick);
            const Vec2 radial_velocity = body.sink_start_offset * (-2.0 * remaining) + body.sink_entry_axis * (body.sink_arc * kick_derivative);
            const Vec2 tangent_velocity{-relative.y * phase_derivative, relative.x * phase_derivative};
            body.position = body.sink_center + rotate(relative, phase);
            body.velocity = limited(rotate(radial_velocity + tangent_velocity, phase) / duration, m_config.max_speed);
            body.angle = std::remainder(body.sink_start_angle + body.sink_turn_sign * TAU *
                                       (1.2 * progress + 3.0 * progress * progress), TAU);
            const double scale_envelope = 1.0 - progress * progress;
            body.scale = body.original_scale * scale_envelope * scale_envelope;
            body.stretch = m_config.spaghetti ? 1.0 + std::sin(smoothProgress(progress, 0.10) * PI) * 3.0 : 1.0;
            body.twist = m_config.spaghetti ? smoothProgress(progress, 0.20) * 6.0 : 0.0;
            const auto& region = regionFor(body.region);
            const Vec2 display = region.center() + (body.position - body.sink_camera_center) * body.sink_camera_zoom;
            // Bound the actual shader's rotated/stretched/twisted rectangle,
            // rather than its smaller collision-circle approximation. Twisting
            // a normalized square can expand each axis up to sqrt(2).
            const double winding = std::min(PI * 0.25, std::abs(body.twist));
            const double normalized_extent = std::cos(winding) + std::sin(winding);
            const double half_x = body.width * body.stretch * normalized_extent * 0.5;
            const double half_y = body.height / body.stretch * normalized_extent * 0.5 + body.height * progress * 0.24;
            const double cosine = std::abs(std::cos(body.angle)), sine = std::abs(std::sin(body.angle));
            const double bound_x = (half_x * cosine + half_y * sine) * body.sink_camera_zoom;
            const double bound_y = (half_x * sine + half_y * cosine) * body.sink_camera_zoom;
            const double available_x = std::min(display.x - region.x, region.x + region.width - display.x) - 4.0;
            const double available_y = std::min(display.y - region.y, region.y + region.height - display.y) - 4.0;
            body.scale = std::min(body.scale, previous_scale);
            if (available_x > 0.0 && available_y > 0.0)
                body.scale = std::min({body.scale, available_x / bound_x, available_y / bound_y});
            if (progress >= 1.0) {
                body.stored = true;
                body.position = body.sink_center;
                body.velocity = {};
                body.scale = 0.0;
            }
            continue;
        }
        const Vec2 previous_position = body.position;
        body.velocity = limited((body.velocity + limited(acceleration[index], m_config.max_acceleration) * dt) * damping,
                                m_config.max_speed);
        body.position += body.velocity * dt;
        if (m_config.expansion)
            body.position += (body.position - regionFor(body.region).center()) * (m_config.expansion_rate * dt);
        body.angle = std::remainder(body.angle + body.angular_velocity * dt, TAU);
        body.angular_velocity *= std::exp(-0.008 * dt);
        if (m_config.black_hole && body.cooldown <= 0.0) {
            for (const auto& remnant : m_bodies) {
                if (remnant.id == body.id || remnant.region != body.region ||
                    remnant.nova_age < remnant.nova_charge_seconds) continue;
                if (std::isfinite(segmentPortalEntry(previous_position, body.position, remnant.position, 22.0))) {
                    startBlackHoleById(worldToScreen(remnant.position, remnant.region), body.id);
                    break;
                }
            }
            if (body.sink_progress > 0.0) continue;
        }
        if (m_config.wormholes && body.cooldown <= 0.0) {
            const Wormhole* selected = nullptr;
            double nearest = std::numeric_limits<double>::infinity();
            for (const auto& entrance : m_wormholes) {
                if (entrance.region != body.region) continue;
                const double intersection = segmentPortalEntry(previous_position, body.position, entrance.position, entrance.radius);
                if (intersection < nearest) { nearest = intersection; selected = &entrance; }
            }
            if (selected) {
                beginPortal(body, *selected, m_wormholes[selected->partner],
                            previous_position + (body.position - previous_position) * nearest);
                portal_updated[index] = true;
            }
        }
    }
    if (m_config.collisions && m_config.collision_strength > 0.0) {
        for (std::size_t first = 0; first < m_bodies.size(); ++first) {
            auto& a = m_bodies[first];
            if (a.stored || a.sink_progress > 0.0 || a.portal_progress > 0.0 || a.nova_age >= 0.0 || portal_updated[first])
                continue;
            for (std::size_t second = first + 1; second < m_bodies.size(); ++second) {
                auto& b = m_bodies[second];
                if (b.stored || b.sink_progress > 0.0 || b.portal_progress > 0.0 || b.nova_age >= 0.0 || portal_updated[second] || a.region != b.region)
                    continue;
                const Vec2 offset = b.position - a.position;
                const double distance = length(offset), contact = radius(a) + radius(b);
                if (distance >= contact)
                    continue;
                const Vec2 normal = distance > 0.001 ? offset / distance
                    : rotate({1.0, 0.0}, static_cast<double>((a.id ^ b.id) % 6283) * 0.001);
                const double inverse_a = 1.0 / a.mass, inverse_b = 1.0 / b.mass;
                const double inverse_total = inverse_a + inverse_b;
                const Vec2 correction = normal * ((contact - distance + 0.01) * 0.70 / inverse_total);
                a.position -= correction * inverse_a;
                b.position += correction * inverse_b;
                const double relative_speed = dot(b.velocity - a.velocity, normal);
                if (relative_speed < 0.0) {
                    const double impulse = -(1.0 + m_config.restitution) * relative_speed / inverse_total * m_config.collision_strength;
                    a.velocity -= normal * (impulse * inverse_a);
                    b.velocity += normal * (impulse * inverse_b);
                }
            }
        }
    }
    for (std::size_t index = 0; index < m_bodies.size(); ++index) {
        auto& body = m_bodies[index];
        if (body.stored || body.sink_progress > 0.0 || body.portal_progress > 0.0 || body.nova_age >= 0.0 || portal_updated[index])
            continue;
        const auto& region = regionFor(body.region);
        const double expansion = m_config.expansion ? std::min(5.0, 1.0 + m_time * m_config.expansion_rate * 0.5) : 1.0;
        // Wall contacts cover the visible, rotated texture rectangle, including
        // its shader deformation, for both whole windows and small tile actors.
        const Vec2 bound = renderedHalfExtent(body);
        const double half_x = std::max(16.0, region.width * 0.5 * expansion - bound.x);
        const double half_y = std::max(16.0, region.height * 0.5 * expansion - bound.y);
        Vec2 offset = body.position - region.center();
        if (m_config.collisions) {
            if (std::abs(offset.x) > half_x) {
                const double sign = offset.x >= 0.0 ? 1.0 : -1.0;
                offset.x = sign * half_x;
                if (body.velocity.x * sign > 0.0)
                    body.velocity.x = -body.velocity.x * m_config.restitution;
            }
            if (std::abs(offset.y) > half_y) {
                const double sign = offset.y >= 0.0 ? 1.0 : -1.0;
                offset.y = sign * half_y;
                if (body.velocity.y * sign > 0.0)
                    body.velocity.y = -body.velocity.y * m_config.restitution;
            }
            body.position = region.center() + offset;
        } else {
            // Finite guardrails still bound collision-free, expanding orbits.
            body.position.x = std::clamp(body.position.x, region.center().x - region.width * 20.0, region.center().x + region.width * 20.0);
            body.position.y = std::clamp(body.position.y, region.center().y - region.height * 20.0, region.center().y + region.height * 20.0);
        }
        body.velocity = limited(body.velocity, m_config.max_speed);
        body.position = safeVector(body.position, region.center());
    }
    for (auto& particle : m_particles) {
        particle.life -= dt;
        particle.position += particle.velocity * dt;
        particle.velocity = particle.velocity * std::exp(-0.55 * dt);
    }
    std::erase_if(m_particles, [](const Particle& particle) { return particle.life <= 0.0; });
    for (auto& wave : m_waves) {
        wave.life -= dt;
        wave.radius += 550.0 * dt;
    }
    std::erase_if(m_waves, [](const Wave& wave) { return wave.life <= 0.0; });
    m_time = std::min(36000.0, m_time + dt);
    updateCameras(dt);
}

void Universe::updateCameras(double dt) {
    for (auto& camera : m_cameras) {
        const auto& region = regionFor(camera.region);
        const auto nova = std::find_if(m_bodies.begin(), m_bodies.end(), [&](const Body& body) {
            return body.region == camera.region && body.nova_age >= 0.0 &&
                   body.nova_age < body.nova_charge_seconds + body.nova_fragment_seconds;
        });
        if (nova != m_bodies.end()) {
            camera.center = nova->nova_camera_center;
            camera.zoom = nova->nova_camera_zoom;
            continue;
        }
        const auto sinking = std::find_if(m_bodies.begin(), m_bodies.end(), [&](const Body& body) {
            return body.region == camera.region && body.sink_progress > 0.0 && !body.stored;
        });
        if (sinking != m_bodies.end()) {
            // Preserve the activation's screen-space anchor and apparent scale.
            // The seeds live in Body, so rewind restores this camera too.
            camera.center = sinking->sink_camera_center;
            camera.zoom = sinking->sink_camera_zoom;
            continue;
        }
        const auto transit = std::find_if(m_bodies.begin(), m_bodies.end(), [&](const Body& body) {
            return body.portal_progress > 0.0 && (camera.region == body.portal_source_region || camera.region == body.portal_destination_region);
        });
        if (transit != m_bodies.end()) {
            const bool source = camera.region == transit->portal_source_region;
            camera.center = source ? transit->portal_source_camera_center : transit->portal_destination_camera_center;
            camera.zoom = source ? transit->portal_source_camera_zoom : transit->portal_destination_camera_zoom;
            continue;
        }
        double minimum_x = region.x, maximum_x = region.x + region.width;
        double minimum_y = region.y, maximum_y = region.y + region.height;
        for (const auto& body : m_bodies) {
            if (body.region != camera.region || (body.stored && body.nova_age < 0.0))
                continue;
            const Vec2 extent = body.stored ? Vec2{90.0, 90.0} : renderedHalfExtent(body) + Vec2{40.0, 40.0};
            minimum_x = std::min(minimum_x, body.position.x - extent.x);
            maximum_x = std::max(maximum_x, body.position.x + extent.x);
            minimum_y = std::min(minimum_y, body.position.y - extent.y);
            maximum_y = std::max(maximum_y, body.position.y + extent.y);
        }
        const Vec2 center{(minimum_x + maximum_x) * 0.5, (minimum_y + maximum_y) * 0.5};
        const double target_zoom = std::clamp(std::min(region.width / (maximum_x - minimum_x),
                                                     region.height / (maximum_y - minimum_y)), 0.02, 1.0);
        const double amount = dt <= 0.0 ? 1.0 : 1.0 - std::exp(-dt * 3.0);
        camera.center += (center - camera.center) * amount;
        // Zoom out immediately to retain visibility; recovery may ease back in.
        camera.zoom = target_zoom < camera.zoom ? target_zoom : camera.zoom + (target_zoom - camera.zoom) * amount;
    }
}

void Universe::resizeHistory() {
    const std::size_t requested = static_cast<std::size_t>(std::ceil(m_config.history_seconds * m_config.history_hz)) + 1;
    // Existing snapshots may have been recorded before a runtime body-limit
    // reduction. Account for their allocation too, not just the new cap.
    // Exploded source bodies remain as image/remnant owners, but do not use
    // object slots. Include that real-window ghost overhead in every estimate.
    std::size_t largest_frame = std::max(m_config.max_objects + m_config.max_bodies, m_bodies.size());
    for (const auto& frame : m_history)
        largest_frame = std::max(largest_frame, frame.bodies.capacity());
    const std::size_t frame_bytes = sizeof(Frame) + sizeof(Body) * largest_frame;
    m_history_limit = m_config.rewind && m_config.history_seconds > 0.0
                          ? std::clamp<std::size_t>(requested, 2, std::max<std::size_t>(2, m_config.history_bytes / frame_bytes)) : 0;
    while (m_history.size() > m_history_limit)
        m_history.pop_front();
    if (m_history_limit == 0)
        m_rewinding = false;
}

void Universe::recordFrame() {
    if (m_history_limit == 0)
        return;
    if (m_history.size() >= m_history_limit)
        m_history.pop_front();
    m_history.push_back({m_bodies, m_time, m_next_nova, m_nova_serial, m_next_fragment_id, m_random});
}

void Universe::restoreFrame(const Frame& frame) {
    // Only real-window IDs define liveness. Fragments are timeline-owned actors:
    // discard future ones, restore old ones only while their source app lives.
    std::vector<Body> restored;
    restored.reserve(m_config.max_objects + m_config.max_bodies);
    for (auto body : m_bodies) {
        if (body.fragment_grid != 0) continue;
        const auto previous = std::find_if(frame.bodies.begin(), frame.bodies.end(), [&](const Body& old) {
            return old.fragment_grid == 0 && old.id == body.id;
        });
        if (previous != frame.bodies.end())
            body = *previous;
        else if (body.nova_age >= 0.0)
            // Its app still exists, but its later stellar event must not
            // overlap the historical event being restored on another body.
            cancelNova(body);
        if (!m_config.wormholes && body.portal_progress > 0.0)
            cancelPortal(body);
        if (!m_config.supernova && body.nova_age >= 0.0)
            cancelNova(body);
        restored.push_back(body);
    }
    if (m_config.supernova) {
        for (auto body : frame.bodies) {
            if (body.fragment_grid == 0) continue;
            const bool living_source = std::any_of(restored.begin(), restored.end(), [&](const Body& current) {
                return current.fragment_grid == 0 && current.id == body.source_id && novaRemnant(current);
            });
            if (!living_source) continue;
            if (!m_config.wormholes && body.portal_progress > 0.0) cancelPortal(body);
            restored.push_back(body);
        }
    }
    std::size_t reserved = 0;
    for (const auto& body : restored) {
        if (novaRemnant(body)) continue;
        ++reserved;
        if (body.fragment_grid == 0 && body.nova_age >= 0.0)
            reserved += static_cast<std::size_t>(body.nova_grid * body.nova_grid - 1);
    }
    // A runtime reduction can make earlier scenes larger than the new budget.
    // Keep existing actors intact and stop at that boundary; never discard
    // tiles or resurrect a closed app merely to make a history frame fit.
    if (reserved > m_config.max_objects) {
        m_rewinding = false;
        return;
    }
    m_bodies = std::move(restored);
    m_time = frame.time;
    m_next_nova = frame.next_nova;
    m_nova_serial = frame.nova_serial;
    m_next_fragment_id = frame.next_fragment_id;
    m_random = frame.random;
    updateCameras(0.0);
}

void Universe::setRewinding(bool enabled) {
    if (enabled && (!m_config.rewind || m_history.size() < 2))
        return;
    if (enabled && !m_rewinding) {
        // Capture the current between-sample state before consuming history.
        recordFrame();
        m_particles.clear();
        m_waves.clear();
    }
    m_rewinding = enabled;
    m_accumulator = m_rewind_accumulator = m_history_accumulator = 0.0;
}

void Universe::step(double elapsed, Vec2 screen_cursor) {
    m_cursor = safeVector(screen_cursor, m_cursor);
    if (!std::isfinite(elapsed) || elapsed <= 0.0)
        return;
    const double max_elapsed = std::min(0.25, m_config.fixed_step * static_cast<double>(m_config.max_substeps));
    elapsed = std::min(elapsed, max_elapsed);
    if (m_rewinding) {
        m_rewind_accumulator += elapsed;
        const double interval = 1.0 / m_config.history_hz;
        while (m_rewinding && m_rewind_accumulator + 1e-12 >= interval && m_history.size() > 1) {
            m_rewind_accumulator -= interval;
            m_history.pop_back();
            restoreFrame(m_history.back());
        }
        if (m_history.size() <= 1)
            setRewinding(false);
        return;
    }
    m_accumulator = std::min(m_accumulator + elapsed, max_elapsed);
    std::size_t count = 0;
    while (m_accumulator + 1e-12 >= m_config.fixed_step && count < m_config.max_substeps) {
        integrate(m_config.fixed_step);
        m_accumulator = std::max(0.0, m_accumulator - m_config.fixed_step);
        m_history_accumulator += m_config.fixed_step;
        const double interval = 1.0 / m_config.history_hz;
        if (m_history_accumulator + 1e-12 >= interval) {
            // Rounding may leave the accumulator just below interval despite
            // the epsilon comparison. Clamp that case to zero rather than
            // retaining a nearly full interval and oversampling next step.
            m_history_accumulator = m_history_accumulator < interval ? 0.0 : std::fmod(m_history_accumulator, interval);
            recordFrame();
        }
        ++count;
    }
}

} // namespace cosmic
