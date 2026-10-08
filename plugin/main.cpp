#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/screenshare/ScreenshareManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <lua.hpp>
#include <linux/input-event-codes.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <regex>
#include <stdexcept>
#include <unordered_set>
#include "physics.hpp"
#include "renderer.hpp"

namespace {
using Clock = std::chrono::steady_clock;
using namespace cosmic;
HANDLE pluginHandle = nullptr;

double number(lua_State* L, int table, const char* key, double fallback, double low, double high) {
    lua_getfield(L, table, key);
    double value = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : fallback;
    lua_pop(L, 1);
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}
bool boolean(lua_State* L, int table, const char* key, bool fallback) {
    lua_getfield(L, table, key);
    bool value = lua_isboolean(L, -1) ? lua_toboolean(L, -1) : fallback;
    lua_pop(L, 1);
    return value;
}
std::string string(lua_State* L, int table, const char* key, const std::string& fallback) {
    lua_getfield(L, table, key);
    std::string value = lua_isstring(L, -1) ? lua_tostring(L, -1) : fallback;
    lua_pop(L, 1);
    return value;
}

struct Control { xkb_keysym_t symbol = XKB_KEY_NoSymbol; uint32_t mods = 0; };
struct Options {
    Config physics = Config::calm();
    double idleTimeout = 5, fps = 60, snapshotHz = 4;
    bool enabled = true, excludeFullscreen = true, excludeInhibit = true, excludeShare = true;
    std::size_t snapshotBudget = 128 * 1024 * 1024;
    std::vector<std::regex> excluded;
    std::vector<Control> controls;
};

class Cosmic;
Cosmic* instance = nullptr;
using RenderWindowFn = void (*)(Render::IHyprRenderer*, PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool, eRenderPassMode, bool, bool);
void renderWindowHook(Render::IHyprRenderer*, PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool, eRenderPassMode, bool, bool);

class Cosmic {
  public:
    Options options;
    Universe universe;
    CosmicRenderer renderer;
    std::vector<Snapshot> snapshots;
    std::vector<CHyprSignalListener> listeners;
    CHyprSignalListener reload;
    SP<CEventLoopTimer> timer;
    CFunctionHook* hook = nullptr;
    bool initialized = false, active = false, capturing = false, alternateRegion = false;
    bool previousScanoutBlocked = false;
    bool notified = false;
    std::unordered_set<std::string> shares;
    std::string reason = "not initialized";
    Clock::time_point lastInput = Clock::now(), lastTick = lastInput, lastCapture = lastInput;
    std::size_t refreshIndex = 0, snapshotBytes = 0;
    uint64_t physicsSteps = 0;

    Cosmic() {
        // This listener alone survives shutdown, so removing require on reload
        // cannot leave the old timer, input callbacks or render detour running.
        reload = Event::bus()->m_events.config.preReload.listen([this] { shutdown(); });
    }
    ~Cosmic() { shutdown(); }

    Vec2 cursor() const {
        auto p = g_pInputManager->getMouseCoordsInternal();
        return {p.x, p.y};
    }
    void damage() {
        for (const auto& m : State::monitorState()->monitors())
            if (m->m_dpmsStatus) g_pHyprRenderer->damageMonitor(m);
    }
    void fail(const std::string& message) {
        reason = message;
        options.enabled = false;
        stop(message);
        if (!notified) {
            notified = true;
            HyprlandAPI::addNotification(pluginHandle, "Cosmic: " + message + ". Run scripts/build.sh and reinstall for Hyprland 0.56.2.", CHyprColor(1, .4, .2, 1), 8000);
        }
    }
    bool held() const {
        if (g_pInputManager->hasHeldButtons()) return true;
        for (const auto& keyboard : g_pInputManager->m_keyboards) {
            if (!keyboard->m_enabled || !keyboard->m_allowed) continue;
            // Real device state also covers keys already down at plugin load,
            // repeats, two keyboards, and lost per-plugin keydown history.
            for (uint32_t key = 0; key <= KEY_MAX; ++key)
                if (keyboard->getPressed(key)) return true;
        }
        return false;
    }
    bool blocked() const {
        if (g_pSessionLockManager->isSessionLocked() || g_pInputManager->isConstrained() || g_pInputManager->isLocked()) return true;
        for (const auto& m : State::monitorState()->monitors()) {
            if (!m->m_dpmsStatus) return true;
            // Covers shares that predate require/setup or a configuration reload.
            if (options.excludeShare && Screenshare::mgr()->isOutputBeingSSd(m)) return true;
        }
        if (options.excludeShare && !shares.empty()) return true;
        if (options.excludeInhibit && !g_pInputManager->m_idleInhibitors.empty()) return true;
        for (const auto& w : Desktop::windowState()->windows()) {
            if (!w->m_isMapped || w->isHidden() || !g_pHyprRenderer->shouldRenderWindow(w)) continue;
            if (options.excludeFullscreen && Fullscreen::controller()->isFullscreen(w)) return true;
            for (const auto& pattern : options.excluded) if (std::regex_search(w->m_class, pattern)) return true;
        }
        return State::monitorState()->monitors().empty();
    }
    bool dedicated(const IKeyboard::SKeyEvent& event) const {
        const auto keyboard = g_pSeatManager->m_keyboard.lock();
        if (!keyboard || !keyboard->m_xkbSymState) return false;
        const auto symbol = xkb_keysym_to_lower(xkb_state_key_get_one_sym(keyboard->m_xkbSymState, event.keycode + 8));
        const auto mods = keyboard->getModifiers() & ~HL_MODIFIER_CAPS & ~HL_MODIFIER_MOD2;
        for (const auto& control : options.controls)
            if (control.symbol == symbol && control.mods == mods) return true;
        return false;
    }
    void stop(const std::string& why, bool refreshPointer = false) {
        const bool wasActive = active;
        active = false; // BEFORE any input hit testing or seat delivery.
        reason = why;
        lastInput = lastTick = Clock::now();
        if (wasActive) {
            universe.reset({}, {}, cursor());
            snapshots.clear();
            snapshotBytes = 0;
            g_pHyprRenderer->m_directScanoutBlocked = previousScanoutBlocked;
            damage();
            if (refreshPointer) g_pInputManager->simulateMouseMovement();
        }
    }
    void shutdown() {
        stop("shutdown");
        initialized = false;
        listeners.clear();
        if (timer) {
            timer->cancel();
            g_pEventLoopManager->removeTimer(timer);
            timer.reset();
        }
        if (hook) {
            hook->unhook();
            HyprlandAPI::removeFunctionHook(pluginHandle, hook);
            hook = nullptr;
        }
        if (g_pHyprRenderer && g_pHyprRenderer->glBackend()) {
            g_pHyprRenderer->glBackend()->makeEGLCurrent();
            renderer.release();
        }
    }
    bool capture(PHLWINDOW window, Snapshot& result) {
        const auto monitor = window->m_monitor.lock();
        if (!monitor || !window->m_isMapped || window->isHidden()) return false;
        const auto bytes = static_cast<std::size_t>(monitor->m_pixelSize.x * monitor->m_pixelSize.y * 4);
        if (snapshotBytes + bytes > options.snapshotBudget) return false;
        capturing = true;
        // makeSnapshotFB is deliberately outside a render-stage callback: it
        // starts/ends its own fake pass and would corrupt a nested render pass.
        auto framebuffer = g_pHyprRenderer->makeSnapshotFB(window);
        capturing = false;
        if (!framebuffer || !framebuffer->isAllocated()) return false;
        result.id = window->m_stableID;
        result.window = window;
        result.framebuffer = framebuffer;
        result.logicalBox = window->getFullWindowBoundingBox();
        result.monitorPosition = monitor->m_position;
        result.monitorScale = monitor->m_scale;
        snapshotBytes += bytes;
        return true;
    }
    Body bodyFor(const Snapshot& shot) {
        auto window = shot.window.lock();
        Body body;
        body.id = shot.id;
        body.position = {shot.logicalBox.x + shot.logicalBox.w / 2, shot.logicalBox.y + shot.logicalBox.h / 2};
        body.width = shot.logicalBox.w;
        body.height = shot.logicalBox.h;
        body.region = static_cast<int>(window->m_monitor->m_id);
        return body;
    }
    void start(bool preview) {
        if (!initialized || !options.enabled || active || blocked()) return;
        if (!preview && held()) return;
        std::vector<Region> regions;
        for (const auto& monitor : State::monitorState()->monitors())
            regions.push_back({static_cast<int>(monitor->m_id), monitor->m_position.x, monitor->m_position.y, monitor->m_size.x, monitor->m_size.y});
        snapshots.clear();
        snapshotBytes = 0;
        std::vector<Body> bodies;
        for (const auto& window : Desktop::windowState()->windows()) {
            if (bodies.size() >= options.physics.max_bodies) break;
            if (!window->m_isMapped || window->isHidden() || !g_pHyprRenderer->shouldRenderWindow(window)) continue;
            Snapshot shot;
            if (capture(window, shot)) {
                bodies.push_back(bodyFor(shot));
                snapshots.push_back(std::move(shot));
            }
        }
        if (bodies.empty()) { reason = "no visible windows within snapshot budget"; return; }
        auto focused = Desktop::focusState()->window();
        universe.reset(std::move(bodies), std::move(regions), cursor(), focused ? focused->m_stableID : 0);
        active = true;
        alternateRegion = false;
        lastTick = lastCapture = Clock::now();
        reason = preview ? "manual preview" : "idle";
        previousScanoutBlocked = g_pHyprRenderer->m_directScanoutBlocked;
        g_pHyprRenderer->m_directScanoutBlocked = true;
        damage();
    }
    void refresh() {
        if (snapshots.empty()) return;
        refreshIndex %= snapshots.size();
        auto& old = snapshots[refreshIndex++];
        auto window = old.window.lock();
        if (!window || !window->m_isMapped || window->isHidden()) return;
        auto monitor = window->m_monitor.lock();
        if (!monitor) return;
        const auto oldBytes = static_cast<std::size_t>(old.framebuffer->m_size.x * old.framebuffer->m_size.y * 4);
        // Drop the previous framebuffer before allocating its replacement, so
        // both logical and peak GPU allocations obey the configured budget.
        old.framebuffer.reset();
        snapshotBytes -= std::min(snapshotBytes, oldBytes);
        Snapshot next;
        if (capture(window, next)) old = std::move(next);
        else stop("snapshot unavailable");
    }
    void synchronizeWindows() {
        for (auto it = snapshots.begin(); it != snapshots.end();) {
            auto window = it->window.lock();
            if (!window || !window->m_isMapped || window->isHidden()) {
                universe.removeBody(it->id);
                if (it->framebuffer) snapshotBytes -= std::min(snapshotBytes, static_cast<std::size_t>(it->framebuffer->m_size.x * it->framebuffer->m_size.y * 4));
                it = snapshots.erase(it);
            } else ++it;
        }
        for (const auto& window : Desktop::windowState()->windows()) {
            if (snapshots.size() >= options.physics.max_bodies) break;
            if (!window->m_isMapped || window->isHidden() || !g_pHyprRenderer->shouldRenderWindow(window)) continue;
            if (std::ranges::any_of(snapshots, [&](const auto& shot) { return shot.id == window->m_stableID; })) continue;
            Snapshot shot;
            if (capture(window, shot)) {
                universe.addBody(bodyFor(shot), true);
                snapshots.push_back(std::move(shot));
            }
        }
        if (snapshots.empty()) stop("all windows closed");
    }
    void tick() {
        auto now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - lastTick).count();
        lastTick = now;
        if (!options.enabled) return;
        if (elapsed > 1.0) { stop("resume or long pause"); return; }
        if (blocked()) { if (active) stop("excluded, locked or display off"); lastInput = now; return; }
        if (!active) {
            if (std::chrono::duration<double>(now - lastInput).count() >= options.idleTimeout && !held()) start(false);
        } else {
            universe.step(elapsed, cursor());
            ++physicsSteps;
            if (std::chrono::duration<double>(now - lastCapture).count() >= 1.0 / options.snapshotHz) {
                lastCapture = now;
                synchronizeWindows();
                if (active) refresh();
            }
            if (active) damage();
        }
    }
    void initialize() {
        if (initialized) return;
        if (!g_pHyprRenderer->glBackend()) throw std::runtime_error("OpenGL renderer required");
        const auto matches = HyprlandAPI::findFunctionsByName(pluginHandle, "renderWindow");
        for (const auto& match : matches) {
            if (match.demangled.find("IHyprRenderer::renderWindow(") == std::string::npos) continue;
            hook = HyprlandAPI::createFunctionHook(pluginHandle, match.address, reinterpret_cast<void*>(renderWindowHook));
            break;
        }
        if (!hook || !hook->hook()) throw std::runtime_error("renderWindow detour could not be installed");
        auto& events = Event::bus()->m_events;
        listeners.push_back(events.input.keyboard.key.listen([this](IKeyboard::SKeyEvent event, Event::SCallbackInfo&) {
            lastInput = Clock::now();
            if (active && event.state == WL_KEYBOARD_KEY_STATE_PRESSED && !dedicated(event)) stop("keyboard input");
        }));
        listeners.push_back(events.input.mouse.move.listen([this](Vector2D, Event::SCallbackInfo& info) {
            if (active) info.cancelled = true; // Pointer already moved; preserve ordinary focus.
            else lastInput = Clock::now();
        }));
        listeners.push_back(events.input.mouse.button.listen([this](IPointer::SButtonEvent, Event::SCallbackInfo&) { stop("pointer button", true); }));
        listeners.push_back(events.input.mouse.axis.listen([this](IPointer::SAxisEvent, Event::SCallbackInfo&) { stop("pointer scroll", true); }));
        listeners.push_back(events.input.touch.down.listen([this] { stop("touch", true); }));
        listeners.push_back(events.input.tablet.tip.listen([this] { stop("tablet", true); }));
        listeners.push_back(events.monitor.layoutChanged.listen([this] { stop("monitor layout changed"); }));
        listeners.push_back(events.workspace.active.listen([this] { stop("workspace changed"); }));
        listeners.push_back(events.workspace.specialActive.listen([this] { stop("special workspace changed"); }));
        listeners.push_back(g_pSessionLockManager->m_events.lock.listen([this] { stop("session locked"); }));
        listeners.push_back(g_pSessionLockManager->m_events.unlock.listen([this] { stop("session unlocked"); }));
        listeners.push_back(events.screenshare.state.listen([this](bool state, uint8_t type, const std::string& name) {
            const auto key = std::to_string(type) + ":" + name;
            if (state) shares.insert(key); else shares.erase(key);
            if (options.excludeShare && state) stop("screen sharing");
        }));
        listeners.push_back(events.render.stage.listen([this](eRenderStage stage) {
            if (!active || capturing || stage != RENDER_POST_WINDOWS) return;
            try { renderer.draw(g_pHyprRenderer->m_renderData.pMonitor.lock(), universe, snapshots, alternateRegion); }
            catch (const std::exception& error) { fail(error.what()); }
        }));
        timer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(250), [this](SP<CEventLoopTimer> self, void*) {
            tick();
            if (initialized) self->updateTimeout(std::chrono::milliseconds(active ? static_cast<int>(1000.0 / options.fps) : 250));
        }, nullptr);
        g_pEventLoopManager->addTimer(timer);
        lastInput = lastTick = Clock::now();
        initialized = true;
        reason = "waiting for idle";
    }
    void action(const std::string& name) {
        lastInput = Clock::now();
        if (name == "emergency") { stop("emergency"); return; }
        if (name == "preview") { if (active) stop("preview ended"); else start(true); return; }
        if (!active) return;
        if (name == "gravity") universe.cycleGravity();
        else if (name == "black_hole") universe.startBlackHole(cursor());
        else if (name == "rewind") universe.setRewinding(!universe.rewinding());
        else if (name == "region") alternateRegion = !alternateRegion;
        else if (name == "supernova") {
            int region = 0;
            for (const auto& r : universe.regions()) {
                auto c = cursor();
                if (c.x >= r.x && c.x < r.x + r.width && c.y >= r.y && c.y < r.y + r.height) { region = r.id; break; }
            }
            universe.supernova(universe.screenToWorld(cursor(), region), region);
        }
        damage();
    }
};

void renderWindowHook(Render::IHyprRenderer* self, PHLWINDOW window, PHLMONITOR monitor, const Time::steady_tp& time, bool decorate, eRenderPassMode mode, bool ignorePosition, bool standalone) {
    if (instance->active && !instance->capturing && std::ranges::any_of(instance->snapshots, [&](const auto& shot) { return shot.id == window->m_stableID; })) return;
    reinterpret_cast<RenderWindowFn>(instance->hook->m_original)(self, window, monitor, time, decorate, mode, ignorePosition, standalone);
}

int setup(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    try {
        Options next;
        next.physics = string(L, 1, "preset", "calm") == "demo" ? Config::demo() : Config::calm();
        next.enabled = boolean(L, 1, "enabled", true);
        next.idleTimeout = number(L, 1, "idle_timeout", 5, .25, 3600);
        next.fps = number(L, 1, "fps", 60, 10, 120);
        next.snapshotHz = number(L, 1, "snapshot_hz", 4, .25, 30);
        next.physics.max_bodies = static_cast<std::size_t>(number(L, 1, "max_windows", 24, 1, 48));
        next.physics.seed = static_cast<uint64_t>(number(L, 1, "seed", 12606492, 0, 4294967295.0));
        next.physics.history_seconds = number(L, 1, "history_seconds", 12, .1, 60);
        next.physics.history_hz = number(L, 1, "history_hz", 30, 1, 120);
        const auto historyBytes = number(L, 1, "history_mb", 16, 1, 64) * 1024 * 1024;
        next.physics.history_seconds = std::min(next.physics.history_seconds, historyBytes / (sizeof(Body) * next.physics.max_bodies * next.physics.history_hz));
        lua_getfield(L, 1, "effects");
        if (lua_istable(L, -1)) {
            int t = lua_gettop(L);
#define EFFECT(name) next.physics.name = boolean(L, t, #name, next.physics.name)
            EFFECT(cursor_gravity); EFFECT(orbit); EFFECT(binary); EFFECT(collisions); EFFECT(black_hole);
            EFFECT(spaghetti); EFFECT(wormholes); EFFECT(supernova); EFFECT(expansion); EFFECT(rewind);
#undef EFFECT
        }
        lua_pop(L, 1);
        lua_getfield(L, 1, "physics");
        if (lua_istable(L, -1)) {
            int t = lua_gettop(L);
#define PARAM(name, low, high) next.physics.name = number(L, t, #name, next.physics.name, low, high)
            PARAM(cursor_strength, 0, 1e8); PARAM(mutual_strength, 0, 1e7); PARAM(softening, 1, 1000);
            PARAM(max_acceleration, 1, 10000); PARAM(max_speed, 1, 5000); PARAM(damping, 0, 10);
            PARAM(restitution, 0, 1); PARAM(collision_strength, 0, 1); PARAM(expansion_rate, 0, .1);
            PARAM(sink_duration, .2, 30); PARAM(wormhole_cooldown, .1, 10); PARAM(explosion_strength, 0, 5000);
            PARAM(fixed_step, 1.0 / 240.0, .05); PARAM(max_substeps, 1, 16);
#undef PARAM
        }
        lua_pop(L, 1);
        lua_getfield(L, 1, "rendering");
        if (lua_istable(L, -1)) {
            next.physics.max_particles = static_cast<std::size_t>(number(L, -1, "particles", 96, 0, 384));
            next.snapshotBudget = static_cast<std::size_t>(number(L, -1, "snapshot_mb", 128, 16, 512) * 1024 * 1024);
        }
        lua_pop(L, 1);
        lua_getfield(L, 1, "exclusions");
        if (lua_istable(L, -1)) {
            const int t = lua_gettop(L);
            next.excludeFullscreen = boolean(L, t, "fullscreen", true);
            next.excludeInhibit = boolean(L, t, "idle_inhibit", true);
            next.excludeShare = boolean(L, t, "screenshare", true);
            lua_getfield(L, t, "classes");
            if (lua_istable(L, -1)) for (lua_Integer i = 1; i <= static_cast<lua_Integer>(lua_rawlen(L, -1)); ++i) {
                lua_rawgeti(L, -1, i);
                if (lua_isstring(L, -1)) next.excluded.emplace_back(lua_tostring(L, -1), std::regex::icase);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 1, "controls");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                if (lua_isstring(L, -1)) {
                    std::string chord = lua_tostring(L, -1);
                    Control control;
                    std::size_t begin = 0, end;
                    while ((end = chord.find('+', begin)) != std::string::npos) {
                        auto word = chord.substr(begin, end - begin);
                        std::erase(word, ' ');
                        if (word == "SUPER") control.mods |= HL_MODIFIER_META;
                        if (word == "ALT") control.mods |= HL_MODIFIER_ALT;
                        if (word == "CTRL" || word == "CONTROL") control.mods |= HL_MODIFIER_CTRL;
                        if (word == "SHIFT") control.mods |= HL_MODIFIER_SHIFT;
                        begin = end + 1;
                    }
                    auto key = chord.substr(begin);
                    std::erase(key, ' ');
                    control.symbol = xkb_keysym_to_lower(xkb_keysym_from_name(key.c_str(), XKB_KEYSYM_CASE_INSENSITIVE));
                    if (control.symbol != XKB_KEY_NoSymbol) next.controls.push_back(control);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        instance->shutdown();
        instance->options = std::move(next);
        instance->universe.configure(instance->options.physics);
        if (instance->options.enabled) instance->initialize();
        lua_pushboolean(L, true);
        return 1;
    } catch (const std::exception& error) {
        instance->shutdown();
        instance->fail(error.what());
        lua_pushnil(L);
        lua_pushstring(L, error.what());
        return 2;
    }
}
int enable(lua_State* L) {
    try { instance->options.enabled = true; instance->initialize(); lua_pushboolean(L, true); }
    catch (const std::exception& error) { instance->shutdown(); instance->fail(error.what()); lua_pushboolean(L, false); }
    return 1;
}
int disable(lua_State* L) { instance->options.enabled = false; instance->shutdown(); lua_pushboolean(L, true); return 1; }
int shutdown(lua_State* L) { instance->shutdown(); lua_pushboolean(L, true); return 1; }
int action(lua_State* L) { instance->action(luaL_checkstring(L, 1)); lua_pushboolean(L, true); return 1; }
int status(lua_State* L) {
    lua_newtable(L);
    auto b = [&](const char* k, bool v) { lua_pushboolean(L, v); lua_setfield(L, -2, k); };
    auto n = [&](const char* k, std::size_t v) { lua_pushinteger(L, static_cast<lua_Integer>(v)); lua_setfield(L, -2, k); };
    b("enabled", instance->options.enabled); b("initialized", instance->initialized); b("active", instance->active); b("rewinding", instance->universe.rewinding());
    n("bodies", instance->universe.bodies().size()); n("snapshots", instance->snapshots.size()); n("snapshot_bytes", instance->snapshotBytes);
    n("history_frames", instance->universe.historyFrames()); n("history_limit", instance->universe.historyLimit()); n("input_watchers", instance->listeners.size()); n("physics_steps", instance->physicsSteps);
    n("stored", std::ranges::count_if(instance->universe.bodies(), [](const auto& body) { return body.stored; }));
    lua_pushstring(L, instance->reason.c_str()); lua_setfield(L, -2, "last_reason");
    lua_pushstring(L, GIT_COMMIT_HASH); lua_setfield(L, -2, "hyprland_commit");
    lua_pushstring(L, "bounded refreshed snapshots"); lua_setfield(L, -2, "capture_mode");
    return 1;
}
} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    pluginHandle = handle;
    if (HyprlandAPI::getHyprlandVersion(handle).hash != GIT_COMMIT_HASH) throw std::runtime_error("Cosmic: Hyprland commit mismatch; rebuild with the running compositor headers");
    instance = new Cosmic();
    for (auto [name, callback] : std::initializer_list<std::pair<const char*, PLUGIN_LUA_FN>>{{"setup", setup}, {"enable", enable}, {"disable", disable}, {"shutdown", shutdown}, {"action", action}, {"status", status}}) {
        if (!HyprlandAPI::addLuaFunction(handle, "cosmic", name, callback)) {
            delete instance; instance = nullptr;
            throw std::runtime_error("Cosmic: Lua callback registration failed");
        }
    }
    return {"cosmic", "Idle desktop universe with reversible window imagery", "hisuic", "0.1.0"};
}
APICALL EXPORT void PLUGIN_EXIT() { delete instance; instance = nullptr; }
