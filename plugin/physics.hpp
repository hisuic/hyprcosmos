#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <random>
#include <vector>

// The simulation owns no compositor objects. IDs are opaque handles supplied by
// the renderer, and every coordinate is in monitor-independent logical pixels.
namespace cosmic {

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
    Vec2 operator+(Vec2 other) const { return {x + other.x, y + other.y}; }
    Vec2 operator-(Vec2 other) const { return {x - other.x, y - other.y}; }
    Vec2 operator*(double factor) const { return {x * factor, y * factor}; }
    Vec2 operator/(double factor) const { return {x / factor, y / factor}; }
    Vec2& operator+=(Vec2 other) { x += other.x; y += other.y; return *this; }
    Vec2& operator-=(Vec2 other) { x -= other.x; y -= other.y; return *this; }
};

struct Region {
    int id = 0;
    double x = 0.0;
    double y = 0.0;
    double width = 1920.0;
    double height = 1080.0;
    Vec2 center() const { return {x + width * 0.5, y + height * 0.5}; }
};

struct Body {
    uint64_t id = 0;
    Vec2 position;
    Vec2 velocity;
    double width = 640.0;
    double height = 480.0;
    double mass = 1.0;
    double angle = 0.0;             // Radians.
    double scale = 1.0;
    double stretch = 1.0;           // Local X multiplier; local Y uses its reciprocal.
    double twist = 0.0;             // Radians at the outer texture edge.
    bool stored = false;
    int region = 0;
    double cooldown = 0.0;
    double sink_progress = 0.0;     // 0: untouched, (0, 1): falling, 1: stored.
    Vec2 sink_center;
    double angular_velocity = 0.0;
    double original_scale = 0.42;
    // Captured cinematic seeds make a centered selection move continuously and
    // make resumed playback follow exactly the same path after a rewind.
    Vec2 sink_start_offset;
    Vec2 sink_entry_axis{1.0, 0.0};
    double sink_arc = 0.0;
    double sink_start_angle = 0.0;
    double sink_turn_sign = 1.0;
    double sink_duration = 0.0;      // May extend an impossible duration/speed combination.
    Vec2 sink_camera_center;
    double sink_camera_zoom = 1.0;
};

struct Particle {
    Vec2 position;
    Vec2 velocity;
    double life = 0.0;
    double lifetime = 1.0;
    double size = 2.0;
    int region = 0;
    double hue = 0.0;
};

struct Wave {
    Vec2 position;
    double radius = 0.0;
    double life = 0.0;
    double lifetime = 1.0;
    int region = 0;
};

struct Wormhole {
    Vec2 position;
    double radius = 60.0;
    double angle = 0.0;
    int region = 0;
    std::size_t partner = 0;
};

struct Camera {
    int region = 0;
    Vec2 center;
    double zoom = 1.0;
};

enum class GravityMode { Cursor, Focused, Binary };

struct Config {
    bool cursor_gravity = true;
    bool orbit = true;
    bool binary = true;
    bool collisions = true;
    bool black_hole = true;
    bool spaghetti = true;
    bool wormholes = true;
    bool supernova = true;
    bool expansion = true;
    bool rewind = true;
    double fixed_step = 1.0 / 120.0;
    std::size_t max_substeps = 8;
    std::size_t max_bodies = 48;
    std::size_t max_particles = 384;
    double history_seconds = 12.0;
    double history_hz = 30.0;
    uint64_t seed = 0xC05C1C;
    double cursor_strength = 1400000.0;
    double mutual_strength = 14000.0;
    double softening = 100.0;
    double max_acceleration = 1400.0;
    double max_speed = 950.0;
    double damping = 0.012;
    double restitution = 0.68;
    double collision_strength = 0.35;
    double expansion_rate = 0.006;
    double sink_duration = 2.8;
    double wormhole_cooldown = 2.0;
    double explosion_strength = 320.0;
    static Config calm();
    static Config demo();
};

class Universe {
  public:
    explicit Universe(Config config = {});
    void configure(Config config);
    void reset(std::vector<Body> bodies, std::vector<Region> regions, Vec2 cursor,
               uint64_t focused_id = 0);
    // elapsed is a monotonic-clock delta. Long stalls are deliberately discarded.
    void step(double elapsed, Vec2 screen_cursor);
    bool addBody(Body body, bool explode = true);
    void removeBody(uint64_t id);
    void setRewinding(bool enabled);
    bool startBlackHole(Vec2 screen_cursor);
    bool startBlackHole(Vec2 screen_cursor, int visible_region);
    void supernova(Vec2 position, int region = 0);
    void cycleGravity();
    void setBinaryPreset();

    const Config& config() const { return m_config; }
    const std::vector<Body>& bodies() const { return m_bodies; }
    const std::vector<Region>& regions() const { return m_regions; }
    const std::vector<Particle>& particles() const { return m_particles; }
    const std::vector<Wave>& waves() const { return m_waves; }
    const std::vector<Wormhole>& wormholes() const { return m_wormholes; }
    const std::vector<Camera>& cameras() const { return m_cameras; }
    bool rewinding() const { return m_rewinding; }
    GravityMode gravityMode() const { return m_gravity_mode; }
    std::size_t historyFrames() const { return m_history.size(); }
    std::size_t historyLimit() const { return m_history_limit; }
    uint64_t hitTest(Vec2 screen_position) const;
    uint64_t hitTest(Vec2 screen_position, int visible_region) const;
    Vec2 worldToScreen(Vec2 position, int region) const;
    Vec2 screenToWorld(Vec2 position, int region) const;

  private:
    struct Frame { std::vector<Body> bodies; double time = 0.0; };
    Config m_config;
    std::vector<Body> m_bodies;
    std::vector<Region> m_regions;
    std::vector<Particle> m_particles;
    std::vector<Wave> m_waves;
    std::vector<Wormhole> m_wormholes;
    std::vector<Camera> m_cameras;
    std::deque<Frame> m_history;
    std::mt19937_64 m_random;
    std::size_t m_history_limit = 1;
    uint64_t m_focused_id = 0;
    Vec2 m_cursor;
    double m_accumulator = 0.0;
    double m_history_accumulator = 0.0;
    double m_rewind_accumulator = 0.0;
    double m_time = 0.0;
    bool m_rewinding = false;
    GravityMode m_gravity_mode = GravityMode::Cursor;

    const Region& regionFor(int id) const;
    const Camera* cameraFor(int id) const;
    void normalizeBody(Body& body, bool initialize_velocity);
    void integrate(double dt);
    void updateCameras(double dt);
    void createWormholes();
    void recordFrame();
    void restoreFrame(const Frame& frame);
    void resizeHistory();
    double randomUnit();
    uint64_t hitTestInRegion(Vec2 screen_position, const int* visible_region) const;
    bool startBlackHoleById(Vec2 screen_cursor, uint64_t id);
};

} // namespace cosmic
