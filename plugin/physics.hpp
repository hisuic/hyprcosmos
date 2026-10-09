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
    // Automatic wormholes have two visible legs, not an instant teleport.
    // All seeds belong to the body so bounded history can replay both legs.
    double portal_progress = 0.0;   // 0: idle, (0, 1): entry then emergence.
    bool portal_emerging = false;   // Region switches at an invisible midpoint.
    int portal_source_region = 0;
    int portal_destination_region = 0;
    Vec2 portal_entry_center;
    Vec2 portal_exit_center;
    Vec2 portal_start_offset;
    Vec2 portal_entry_axis{1.0, 0.0};
    Vec2 portal_exit_offset;
    Vec2 portal_start_velocity;
    double portal_arc = 0.0;
    double portal_start_angle = 0.0;
    double portal_start_scale = 0.42;
    double portal_start_stretch = 1.0;
    double portal_start_twist = 0.0;
    double portal_turn = 0.0;
    double portal_turn_sign = 1.0;
    double portal_entry_duration = 1.68;
    double portal_exit_duration = 0.75;
    Vec2 portal_source_camera_center;
    Vec2 portal_destination_camera_center;
    double portal_source_camera_zoom = 1.0;
    double portal_destination_camera_zoom = 1.0;
    // A stellar event owns only seeds and age, never sixteen copies of a GPU
    // image. Analytic fragment poses can therefore be rewound within Body.
    double nova_age = -1.0;
    double nova_charge_seconds = 3.5;
    double nova_fragment_seconds = 3.0;
    double nova_initial_scale = 0.42;
    double nova_growth = 1.7;
    double nova_start_stretch = 1.0;
    double nova_start_twist = 0.0;
    int nova_grid = 4;
    uint64_t nova_seed = 0;
    Vec2 nova_camera_center;
    double nova_camera_zoom = 1.0;
};

struct NovaFragment {
    Vec2 position;
    double angle = 0.0;
    double scale = 1.0;
    double alpha = 1.0;
    int column = 0, row = 0, grid = 4;
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
    std::size_t history_bytes = 16U * 1024U * 1024U;
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
    bool stellar_automatic = true;
    double stellar_interval_min = 70.0;
    double stellar_interval_max = 130.0;
    double stellar_charge_seconds = 3.5;
    double stellar_fragment_seconds = 3.0;
    std::size_t stellar_fragments = 16;
    double stellar_growth = 1.7;
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
    bool startStellarNova(Vec2 screen_cursor, int visible_region);
    bool startStellarNova(uint64_t id);
    std::vector<NovaFragment> novaFragments(const Body& body) const;
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
    struct Frame {
        std::vector<Body> bodies;
        double time = 0.0, next_nova = 0.0;
        uint64_t nova_serial = 0;
        std::mt19937_64 random;
    };
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
    double m_next_nova = 0.0;
    uint64_t m_nova_serial = 0;
    bool m_rewinding = false;
    GravityMode m_gravity_mode = GravityMode::Cursor;

    const Region& regionFor(int id) const;
    const Camera* cameraFor(int id) const;
    void normalizeBody(Body& body, bool initialize_velocity);
    void integrate(double dt);
    void updateCameras(double dt);
    void createWormholes();
    void beginPortal(Body& body, const Wormhole& entrance, const Wormhole& exit, Vec2 position);
    void advancePortal(Body& body, double dt);
    void cancelPortal(Body& body);
    void recordFrame();
    void restoreFrame(const Frame& frame);
    void resizeHistory();
    double randomUnit();
    uint64_t hitTestInRegion(Vec2 screen_position, const int* visible_region) const;
    bool startBlackHoleById(Vec2 screen_cursor, uint64_t id);
    bool novaBusy() const;
    void scheduleNova();
    void advanceNova(Body& body, double dt);
    void cancelNova(Body& body);
};

} // namespace cosmic
