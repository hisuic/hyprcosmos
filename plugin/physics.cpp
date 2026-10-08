#include "physics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cosmic {
namespace {
constexpr double PI = 3.14159265358979323846;
constexpr double TAU = 2.0 * PI;
constexpr std::size_t HISTORY_BYTES = 16U * 1024U * 1024U;

double finiteClamp(double value, double low, double high, double fallback) {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}
double lengthSquared(Vec2 value) { return value.x * value.x + value.y * value.y; }
double length(Vec2 value) { return std::hypot(value.x, value.y); }
double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
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
Vec2 softenedGravity(Vec2 distance, double strength, double softening) {
    const double squared = lengthSquared(distance) + softening * softening;
    return distance * (strength / (squared * std::sqrt(squared)));
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
    config.max_particles = std::min<std::size_t>(config.max_particles, 2048);
    config.history_seconds = finiteClamp(config.history_seconds, 0.0, 60.0, 12.0);
    config.history_hz = finiteClamp(config.history_hz, 1.0, 60.0, 30.0);
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
    const bool seed_changed = config.seed != m_config.seed;
    m_config = config;
    if (seed_changed)
        m_random.seed(m_config.seed);
    if (m_bodies.size() > config.max_bodies)
        m_bodies.resize(config.max_bodies);
    if (m_particles.size() > config.max_particles)
        m_particles.resize(config.max_particles);
    if (!config.supernova) {
        m_particles.clear();
        m_waves.clear();
    }
    if (!config.wormholes)
        m_wormholes.clear();
    else if (m_wormholes.empty() && !m_regions.empty())
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
    m_cursor = safeVector(cursor);
    m_focused_id = focused_id;
    m_accumulator = m_history_accumulator = m_rewind_accumulator = m_time = 0.0;
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
    body.stretch = finiteClamp(body.stretch, 1.0, 12.0, 1.0);
    body.twist = finiteClamp(body.twist, -8.0, 8.0, 0.0);
    body.angle = std::isfinite(body.angle) ? std::remainder(body.angle, TAU) : 0.0;
    body.cooldown = finiteClamp(body.cooldown, 0.0, 20.0, 0.0);
    body.sink_progress = finiteClamp(body.sink_progress, 0.0, 1.0, 0.0);
    body.sink_center = safeVector(body.sink_center, body.position);
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
    if (body.id == 0 || m_bodies.size() >= m_config.max_bodies ||
        std::any_of(m_bodies.begin(), m_bodies.end(), [&](const Body& current) { return current.id == body.id; }))
        return false;
    normalizeBody(body, true);
    m_bodies.push_back(body);
    if (explode)
        supernova(body.position, body.region);
    return true;
}

void Universe::removeBody(uint64_t id) {
    std::erase_if(m_bodies, [&](const Body& body) { return body.id == id; });
    // Removing IDs from all frames also releases their history memory promptly.
    for (auto& frame : m_history)
        std::erase_if(frame.bodies, [&](const Body& body) { return body.id == id; });
    if (m_focused_id == id)
        m_focused_id = 0;
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
    if (!std::isfinite(screen_position.x) || !std::isfinite(screen_position.y))
        return 0;
    for (auto iterator = m_bodies.rbegin(); iterator != m_bodies.rend(); ++iterator) {
        const Body& body = *iterator;
        if (body.stored || body.sink_progress > 0.0 || body.scale < 0.001)
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
    if (!m_config.black_hole || m_rewinding)
        return false;
    const uint64_t id = hitTest(screen_cursor);
    const auto found = std::find_if(m_bodies.begin(), m_bodies.end(), [&](const Body& body) { return body.id == id; });
    if (found == m_bodies.end())
        return false;
    found->sink_progress = std::numeric_limits<double>::epsilon();
    // This center is captured once: later mouse motion does not drag the hole.
    found->sink_center = screenToWorld(screen_cursor, found->region);
    found->original_scale = found->scale;
    found->cooldown = m_config.sink_duration + m_config.wormhole_cooldown;
    return true;
}

double Universe::randomUnit() {
    return std::generate_canonical<double, 53>(m_random);
}

void Universe::supernova(Vec2 position, int region) {
    if (!m_config.supernova || m_rewinding)
        return;
    position = safeVector(position, regionFor(region).center());
    for (auto& body : m_bodies) {
        if (body.stored || body.region != region || body.sink_progress > 0.0)
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
        if (body.stored || body.sink_progress > 0.0)
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
    std::vector<Vec2> acceleration(m_bodies.size());
    for (std::size_t index = 0; index < m_bodies.size(); ++index) {
        const auto& body = m_bodies[index];
        if (body.stored || body.sink_progress > 0.0)
            continue;
        if (m_config.cursor_gravity && m_gravity_mode == GravityMode::Cursor)
            acceleration[index] += softenedGravity(screenToWorld(m_cursor, body.region) - body.position,
                                                   m_config.cursor_strength, m_config.softening);
        if (m_config.expansion)
            acceleration[index] += (body.position - regionFor(body.region).center()) * (m_config.expansion_rate * 0.14);
    }
    if (m_config.binary) {
        for (std::size_t first = 0; first < m_bodies.size(); ++first) {
            const auto& a = m_bodies[first];
            if (a.stored || a.sink_progress > 0.0)
                continue;
            for (std::size_t second = first + 1; second < m_bodies.size(); ++second) {
                const auto& b = m_bodies[second];
                if (b.stored || b.sink_progress > 0.0 || a.region != b.region)
                    continue;
                const bool focused_pair = m_gravity_mode == GravityMode::Focused && (a.id == m_focused_id || b.id == m_focused_id);
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
        if (body.stored)
            continue;
        if (body.sink_progress > 0.0) {
            body.sink_progress = std::min(1.0, body.sink_progress + dt / m_config.sink_duration);
            const double progress = body.sink_progress;
            const Vec2 relative = body.position - body.sink_center;
            const double spin = 1.3 + 8.0 * progress * progress;
            body.position = body.sink_center + rotate(relative, spin * dt) * std::exp(-(0.8 + progress * 9.0) * dt);
            body.velocity = limited((body.sink_center - body.position) * (2.0 + progress * 10.0), m_config.max_speed);
            body.angle = std::remainder(body.angle + (2.0 + progress * 22.0) * dt, TAU);
            body.scale = body.original_scale * std::pow(1.0 - progress, 1.7);
            body.stretch = m_config.spaghetti ? 1.0 + std::sin(progress * PI) * 5.0 : 1.0;
            body.twist = m_config.spaghetti ? progress * 6.0 : 0.0;
            if (progress >= 1.0) {
                body.stored = true;
                body.position = body.sink_center;
                body.velocity = {};
                body.scale = 0.0;
            }
            continue;
        }
        body.velocity = limited((body.velocity + limited(acceleration[index], m_config.max_acceleration) * dt) * damping,
                                m_config.max_speed);
        body.position += body.velocity * dt;
        if (m_config.expansion)
            body.position += (body.position - regionFor(body.region).center()) * (m_config.expansion_rate * dt);
        body.angle = std::remainder(body.angle + body.angular_velocity * dt, TAU);
        body.angular_velocity *= std::exp(-0.008 * dt);
        if (m_config.wormholes && body.cooldown <= 0.0) {
            for (const auto& entrance : m_wormholes) {
                if (entrance.region != body.region || length(body.position - entrance.position) > entrance.radius)
                    continue;
                const auto& exit = m_wormholes[entrance.partner];
                const double turn = exit.angle - entrance.angle + PI;
                body.velocity = rotate(body.velocity, turn);
                const Vec2 direction = length(body.velocity) > 1.0 ? body.velocity / length(body.velocity)
                                                                 : rotate({1.0, 0.0}, exit.angle);
                body.position = exit.position + direction * (exit.radius + radius(body) * 0.30 + 5.0);
                body.angle = std::remainder(body.angle + turn, TAU);
                body.region = exit.region;
                body.cooldown = m_config.wormhole_cooldown;
                break;
            }
        }
    }
    if (m_config.collisions && m_config.collision_strength > 0.0) {
        for (std::size_t first = 0; first < m_bodies.size(); ++first) {
            auto& a = m_bodies[first];
            if (a.stored || a.sink_progress > 0.0)
                continue;
            for (std::size_t second = first + 1; second < m_bodies.size(); ++second) {
                auto& b = m_bodies[second];
                if (b.stored || b.sink_progress > 0.0 || a.region != b.region)
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
    for (auto& body : m_bodies) {
        if (body.stored || body.sink_progress > 0.0)
            continue;
        const auto& region = regionFor(body.region);
        const double expansion = m_config.expansion ? std::min(5.0, 1.0 + m_time * m_config.expansion_rate * 0.5) : 1.0;
        const double half_x = std::max(16.0, region.width * 0.5 * expansion - radius(body));
        const double half_y = std::max(16.0, region.height * 0.5 * expansion - radius(body));
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
        double minimum_x = region.x, maximum_x = region.x + region.width;
        double minimum_y = region.y, maximum_y = region.y + region.height;
        for (const auto& body : m_bodies) {
            if (body.region != camera.region || body.stored)
                continue;
            const double padding = radius(body) * std::max(1.0, body.stretch) + 40.0;
            minimum_x = std::min(minimum_x, body.position.x - padding);
            maximum_x = std::max(maximum_x, body.position.x + padding);
            minimum_y = std::min(minimum_y, body.position.y - padding);
            maximum_y = std::max(maximum_y, body.position.y + padding);
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
    const std::size_t frame_bytes = sizeof(Frame) + sizeof(Body) * m_config.max_bodies;
    m_history_limit = m_config.rewind && m_config.history_seconds > 0.0
                          ? std::clamp<std::size_t>(requested, 2, std::max<std::size_t>(2, HISTORY_BYTES / frame_bytes)) : 0;
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
    m_history.push_back({m_bodies, m_time});
}

void Universe::restoreFrame(const Frame& frame) {
    // Never replace the living-ID set with the historical one. New windows can
    // remain, while closed windows can never be resurrected by rewind.
    for (auto& body : m_bodies) {
        const auto previous = std::find_if(frame.bodies.begin(), frame.bodies.end(), [&](const Body& old) { return old.id == body.id; });
        if (previous != frame.bodies.end())
            body = *previous;
    }
    m_time = frame.time;
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
        while (m_rewind_accumulator + 1e-12 >= interval && m_history.size() > 1) {
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
            m_history_accumulator = std::fmod(m_history_accumulator, interval);
            recordFrame();
        }
        ++count;
    }
}

} // namespace cosmic
