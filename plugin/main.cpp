#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/LayerState.hpp>
#include <hyprland/src/desktop/state/FadingOutState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/input/TextInput.hpp>
#include <hyprland/src/protocols/InputMethodV2.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/protocols/XDGDialog.hpp>
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
#include <ctime>
#include <cmath>
#include <regex>
#include <stdexcept>
#include "physics.hpp"
#include "renderer.hpp"
#include "desktop_ui.hpp"
#include "user_config_reader.hpp"

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
    cosmic::Config physics = cosmic::Config::calm();
    double idleTimeout = 60, fps = 60, snapshotHz = 4;
    bool enabled = true, excludeFullscreen = true, excludeInhibit = true, excludeShare = true;
    bool hideDesktopUI = true;
    std::size_t snapshotBudget = 128 * 1024 * 1024;
    std::vector<std::regex> excluded;
    std::vector<Control> controls;
};

class Cosmic;
Cosmic* instance = nullptr;
using RenderWindowFn = void (*)(Render::IHyprRenderer*, PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool, Render::eRenderPassMode, bool, bool);
void renderWindowHook(Render::IHyprRenderer*, PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool, Render::eRenderPassMode, bool, bool);
using RenderLayerFn = void (*)(Render::IHyprRenderer*, PHLLS, PHLMONITOR, const Time::steady_tp&, bool, bool);
void renderLayerHook(Render::IHyprRenderer*, PHLLS, PHLMONITOR, const Time::steady_tp&, bool, bool);
using RenderFadeoutsFn = void (*)(Render::IHyprRenderer*, PHLMONITOR, Desktop::eFadeoutPlane, PHLWORKSPACE);
void renderFadeoutsHook(Render::IHyprRenderer*, PHLMONITOR, Desktop::eFadeoutPlane, PHLWORKSPACE);
// Keep the exact C++ return type so the compiler supplies the same hidden
// structure-return argument as CScreenshareSession::nextFrame(bool).
using ScreenshareFrameFn = UP<Screenshare::CScreenshareFrame> (*)(Screenshare::CScreenshareSession*, bool);
UP<Screenshare::CScreenshareFrame> screenshareFrameHook(Screenshare::CScreenshareSession*, bool);

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
    CFunctionHook* shareFrameHook = nullptr;
    CFunctionHook* layerHook = nullptr;
    CFunctionHook* fadeoutsHook = nullptr;
    bool initialized = false, active = false, capturing = false, alternateRegion = false;
    bool manualPreview = false;
    bool previousScanoutBlocked = false;
    bool notified = false;
    std::string reason = "not initialized";
Clock::time_point lastInput = Clock::now(), lastTick = lastInput, lastCapture = lastInput;
    Clock::time_point lastShareRequest{}, shareObservationUntil{};
    std::size_t refreshIndex = 0, snapshotBytes = 0;
    uint64_t physicsSteps = 0;
    double lastBootTime = bootTime();

    static double bootTime() {
        timespec time{};
        clock_gettime(CLOCK_BOOTTIME, &time);
        return static_cast<double>(time.tv_sec) + time.tv_nsec / 1e9;
    }

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
    int viewedRegion() const {
        const auto p = cursor();
        for (const auto& monitor : State::monitorState()->monitors()) {
            if (p.x >= monitor->m_position.x && p.x < monitor->m_position.x + monitor->m_size.x &&
                p.y >= monitor->m_position.y && p.y < monitor->m_position.y + monitor->m_size.y)
                return static_cast<int>(monitor->m_id) + (alternateRegion ? 1000000 : 0);
        }
        return universe.regions().empty() ? 0 : universe.regions().front().id;
    }
    std::optional<std::pair<Vec2, int>> actionLocation() const {
        const auto point = cursor();
        const int region = viewedRegion();
        if (const auto inset = peerViewLayout(universe, region); inset && inset->contains(point)) {
            // The visible inset must never select an occluded main-view image.
            // Its caption/frame deliberately does not fall through either.
            if (!inset->containsContent(point)) return std::nullopt;
            return std::pair{inset->toRegionScreen(point), inset->region.id};
        }
        return std::pair{point, region};
    }
    void damage() {
        for (const auto& m : State::monitorState()->monitors())
            if (m->m_dpmsStatus) g_pHyprRenderer->damageMonitor(m);
    }
    void fail(const std::string& message, bool notify = true) {
        options.enabled = false;
        shutdown();
        reason = message;
        if (notify && !notified) {
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
    bool sharesActive() {
        const auto& monitors = State::monitorState()->monitors();
        if (monitors.empty()) return false;
        if (!shareFrameHook || !shareFrameHook->m_original) return true;
        const auto now = Clock::now();
        // The compositor marks sharing stopped after 500 ms without frames.
        // Allow that timeout plus a normal 250 ms polling interval, without
        // confusing a long-lived session with ongoing sharing.
        if (std::chrono::duration<double>(now - lastShareRequest).count() < .75) return true;
        for (const auto& monitor : monitors) {
            const auto state = Screenshare::mgr()->outputCopyFBState(monitor);
            if (state.sharingSessions > 0 || state.pendingFrames > 0) return true;
        }
        return false;
    }
    bool protectedLayer(const PHLLS& layer) const {
        return layer && layer->m_mapped &&
            (layer->m_interactivity != 0 || layer->m_ruleApplicator->aboveLock().valueOrDefault() ||
             protectedDesktopNamespace(layer->m_namespace));
    }
    bool imeComposition() const {
        const auto ime = g_pInputManager->m_relay.m_inputMethod.lock();
        if (!ime) return false;
        const auto textInput = g_pInputManager->m_relay.getFocusedTextInput();
        // The relay can return a disabled focused input as a fallback. The
        // core also retains old preedit bytes when resetting transaction
        // flags; only a committed preedit is sent before TextInput's done.
        return activeIMEComposition(textInput && textInput->isEnabled(),
                                    ime->m_current.preeditString.committed,
                                    !ime->m_current.preeditString.string.empty());
    }
    bool desktopInteraction() const {
        // Do not turn a launcher, authentication prompt, composition or modal
        // seat grab into invisible input UI. Protocol state is the primary
        // safeguard; names only provide additional conservative exceptions.
        if (g_pSeatManager->m_seatGrab) return true;
        const auto focus = g_pSeatManager->m_state.keyboardFocus.lock();
        for (const auto& layer : Desktop::layerState()->layers()) {
            if (protectedLayer(layer)) return true;
            if (focus && layer && layer->m_mapped && layer->wlSurface() &&
                layer->wlSurface()->resource() == focus) return true;
        }
        // fcitx5 keeps its keyboard grab even while idle (also in Latin mode).
        // A grab means routing, not user activity. Only an actual focused
        // preedit inhibits entry; the core still draws IME UI above our pass,
        // and keyboard.key emits BEFORE ordinary delivery to an IME grab.
        if (imeComposition()) return true;
        return false;
    }
    bool blocked(bool preview = false) {
        if (g_pSessionLockManager->isSessionLocked() || g_pInputManager->isConstrained() || g_pInputManager->isLocked()) return true;
        if (desktopInteraction()) return true;
        for (const auto& m : State::monitorState()->monitors()) {
            if (!m->m_dpmsStatus) return true;
        }
        // Current compositor state also covers window sharing that predates
        // loading/enable, and cannot retain stale begin/end events across disable.
        if (options.excludeShare && sharesActive()) return true;
        for (const auto& w : Desktop::windowState()->windows()) {
            if (!w->m_isMapped || w->isHidden() || !g_pHyprRenderer->shouldRenderWindow(w)) continue;
            if (authenticationWindowClass(w->m_class)) return true;
            if (w->isModal()) return true;
            if (const auto xdg = w->m_xdgSurface.lock()) {
                if (const auto top = xdg->m_toplevel.lock()) {
                    if (const auto dialog = top->m_dialog.lock(); dialog && dialog->modal) return true;
                }
            }
            // An explicit preview may ignore a video's idle inhibitor, but
            // never the independent fullscreen, sharing or security guards.
            if (options.excludeInhibit && !preview && g_pInputManager->isWindowInhibiting(w, false)) return true;
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
    void stop(const std::string& why, bool refreshPointer = false, bool shareKnownActive = false) {
        const bool wasActive = active;
        active = false; // BEFORE any input hit testing or seat delivery.
        manualPreview = false;
        reason = why;
        lastInput = lastTick = Clock::now();
        if (wasActive) {
            universe.reset({}, {}, cursor());
            snapshots.clear();
            snapshotBytes = 0;
            // A nextFrame callback already knows a share is starting. Avoid
            // querying compositor share state while handling that request.
            bool shareBlocked = shareKnownActive || sharesActive();
            for (const auto& monitor : State::monitorState()->monitors())
                shareBlocked = shareBlocked || Screenshare::mgr()->outputCopyFBState(monitor).pendingFrames > 0;
            g_pHyprRenderer->m_directScanoutBlocked = previousScanoutBlocked || shareBlocked;
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
        if (shareFrameHook) {
            shareFrameHook->unhook();
            HyprlandAPI::removeFunctionHook(pluginHandle, shareFrameHook);
            shareFrameHook = nullptr;
        }
        if (layerHook) {
            layerHook->unhook();
            HyprlandAPI::removeFunctionHook(pluginHandle, layerHook);
            layerHook = nullptr;
        }
        if (fadeoutsHook) {
            fadeoutsHook->unhook();
            HyprlandAPI::removeFunctionHook(pluginHandle, fadeoutsHook);
            fadeoutsHook = nullptr;
        }
        lastShareRequest = shareObservationUntil = Clock::time_point{};
        if (g_pHyprRenderer && g_pHyprRenderer->glBackend()) {
            g_pHyprRenderer->glBackend()->makeEGLCurrent();
            // Hyprland retains the executed pass until the NEXT beginRender.
            // Its virtual methods and unique-pointer deleter live in this ELF:
            // destroy our passes while loaded, before releasing GL resources or
            // returning to PluginSystem::unloadPlugin's dlclose. Never clear
            // unrelated compositor passes. Shutdown runs outside pass.draw().
            g_pHyprRenderer->m_renderPass.removeAllOfType("CosmicUniverse");
            renderer.release();
        }
    }
    bool capture(PHLWINDOW window, Snapshot& result) {
        const auto monitor = window->m_monitor.lock();
        if (!monitor || !window->m_isMapped || window->isHidden()) return false;
        const auto bytes = static_cast<std::size_t>(monitor->m_pixelSize.x * monitor->m_pixelSize.y * 4);
        if (snapshotBytes + bytes > options.snapshotBudget) return false;
        capturing = true;
        struct CaptureGuard { bool& flag; ~CaptureGuard() { flag = false; } } guard{capturing};
        // makeSnapshotFB is deliberately outside a render-stage callback: it
        // starts/ends its own fake pass and would corrupt a nested render pass.
        auto framebuffer = g_pHyprRenderer->makeSnapshotFB(window);
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
        if (!initialized || !options.enabled || active || blocked(preview)) return;
        // Fading layer/popup snapshots have no retained namespace/aboveLock
        // identity. Let them finish in the normal desktop before entering.
        for (const auto& fadeout : Desktop::fadingOutState()->fadeouts()) {
            if (!fadeout) continue;
            const auto plane = fadeout->plane();
            if (plane == Desktop::FADEOUT_PLANE_LAYER_BACKGROUND || plane == Desktop::FADEOUT_PLANE_LAYER_BOTTOM ||
                plane == Desktop::FADEOUT_PLANE_LAYER_TOP || plane == Desktop::FADEOUT_PLANE_LAYER_OVERLAY ||
                plane == Desktop::FADEOUT_PLANE_POPUP) return;
        }
        // Observe existing window streams for the compositor's 500 ms stop
        // timeout plus one poll. This gate does not extend ordinary idle time.
        if (options.excludeShare && Clock::now() < shareObservationUntil) return;
        if (!preview && held()) return;
        g_pHyprRenderer->glBackend()->makeEGLCurrent();
        if (!renderer.initialize()) { fail(renderer.error()); return; }
        std::vector<Region> regions;
        for (const auto& monitor : State::monitorState()->monitors()) {
            regions.push_back({static_cast<int>(monitor->m_id), monitor->m_position.x, monitor->m_position.y, monitor->m_size.x, monitor->m_size.y});
            regions.push_back({static_cast<int>(monitor->m_id) + 1000000, monitor->m_position.x, monitor->m_position.y, monitor->m_size.x, monitor->m_size.y});
        }
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
        renderer.beginScene();
        active = true;
        manualPreview = preview;
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
        // An executed CosmicPass still owns copies of the snapshots until the
        // next frame begins. Release those references outside drawing, before
        // allocating a replacement, so the old framebuffer really is freed.
        g_pHyprRenderer->glBackend()->makeEGLCurrent();
        g_pHyprRenderer->m_renderPass.removeAllOfType("CosmicUniverse");
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
        const double boot = bootTime();
        const double bootElapsed = boot - lastBootTime;
        lastBootTime = boot;
        if (!options.enabled) return;
        // CLOCK_MONOTONIC excludes suspend on Linux. CLOCK_BOOTTIME lets us
        // detect resume without feeding suspend time into the simulation.
        if (elapsed > 1.0 || bootElapsed - elapsed > .5) { stop("resume or long pause"); return; }
        if (blocked(active && manualPreview)) { if (active) stop("excluded, locked or display off"); lastInput = now; return; }
        if (!active) {
            if (std::chrono::duration<double>(now - lastInput).count() >= options.idleTimeout && !held()) start(false);
        } else {
            if (!renderer.error().empty()) { fail(renderer.error()); return; }
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
        for (const auto& match : HyprlandAPI::findFunctionsByName(pluginHandle, "nextFrame")) {
            if (match.demangled.find("Screenshare::CScreenshareSession::nextFrame(bool)") == std::string::npos) continue;
            shareFrameHook = HyprlandAPI::createFunctionHook(pluginHandle, match.address, reinterpret_cast<void*>(screenshareFrameHook));
            break;
        }
        if (!shareFrameHook || !shareFrameHook->hook())
            throw std::runtime_error("screenshare-frame observation could not be installed");
        const auto matches = HyprlandAPI::findFunctionsByName(pluginHandle, "renderWindow");
        for (const auto& match : matches) {
            if (match.demangled.find("IHyprRenderer::renderWindow(") == std::string::npos) continue;
            hook = HyprlandAPI::createFunctionHook(pluginHandle, match.address, reinterpret_cast<void*>(renderWindowHook));
            break;
        }
        if (!hook || !hook->hook()) throw std::runtime_error("renderWindow detour could not be installed");
        for (const auto& match : HyprlandAPI::findFunctionsByName(pluginHandle, "renderLayer")) {
            if (match.demangled.find("IHyprRenderer::renderLayer(") == std::string::npos) continue;
            layerHook = HyprlandAPI::createFunctionHook(pluginHandle, match.address, reinterpret_cast<void*>(renderLayerHook));
            break;
        }
        if (!layerHook || !layerHook->hook()) throw std::runtime_error("renderLayer detour could not be installed");
        for (const auto& match : HyprlandAPI::findFunctionsByName(pluginHandle, "renderFadeouts")) {
            if (match.demangled.find("IHyprRenderer::renderFadeouts(") == std::string::npos) continue;
            fadeoutsHook = HyprlandAPI::createFunctionHook(pluginHandle, match.address, reinterpret_cast<void*>(renderFadeoutsHook));
            break;
        }
        if (!fadeoutsHook || !fadeoutsHook->hook()) throw std::runtime_error("renderFadeouts detour could not be installed");
        auto& events = Event::bus()->m_events;
        listeners.push_back(events.layer.opened.listen([this](PHLLS layer) {
            if (active && protectedLayer(layer)) stop("interactive or protected desktop UI");
        }));
        listeners.push_back(events.layer.updateRules.listen([this](PHLLS layer) {
            if (active && protectedLayer(layer)) stop("protected desktop UI rule changed");
        }));
        listeners.push_back(events.layer.closed.listen([this](PHLLS layer) {
            // closed emits before m_mapped is cleared. Reset idle even when
            // already stopped, so an authentication fadeout remains visible.
            if (protectedLayer(layer)) stop("protected desktop UI closed");
        }));
        listeners.push_back(g_pSeatManager->m_events.keyboardFocusChange.listen([this] {
            if (active && desktopInteraction()) stop("desktop UI keyboard focus");
        }));
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
        listeners.push_back(events.screenshare.state.listen([this](bool state, uint8_t, const std::string&) {
            if (options.excludeShare && state) stop("screen sharing");
        }));
        // The screenshare manager clears this flag on output commits. Reassert
        // before direct-scanout checks on EVERY frame, including app-driven ones.
        listeners.push_back(events.render.preChecks.listen([this](PHLMONITOR) {
            // Also inspect current state immediately BEFORE rendering, not
            // just the timer: a new modal/security UI must appear this frame.
            if (active && blocked(manualPreview)) stop("excluded or protected UI before render");
            if (active) g_pHyprRenderer->m_directScanoutBlocked = true;
        }));
        listeners.push_back(events.render.stage.listen([this](eRenderStage stage) {
            if (!active || capturing || stage != RENDER_POST_WINDOWS) return;
            try { renderer.draw(g_pHyprRenderer->m_renderData.pMonitor.lock(), universe, snapshots, alternateRegion); }
            catch (const std::exception& error) { fail(error.what()); }
        }));
        timer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(250), [this](SP<CEventLoopTimer> self, void*) {
            try { tick(); }
            catch (const std::exception& error) { fail(error.what()); }
            if (initialized) self->updateTimeout(std::chrono::milliseconds(active ? static_cast<int>(1000.0 / options.fps) : 250));
        }, nullptr);
        g_pEventLoopManager->addTimer(timer);
        lastInput = lastTick = Clock::now();
        lastShareRequest = Clock::time_point{};
        shareObservationUntil = lastInput + std::chrono::milliseconds(750);
        lastBootTime = bootTime();
        initialized = true;
        reason = "waiting for idle";
    }
    void action(const std::string& name) {
        lastInput = Clock::now();
        if (name == "emergency") { stop("emergency"); return; }
        if (name == "preview") { if (active) stop("preview ended"); else start(true); return; }
        if (!active) return;
        if (name == "gravity") universe.cycleGravity();
        else if (name == "black_hole") {
            if (const auto location = actionLocation()) universe.startBlackHole(location->first, location->second);
        }
        else if (name == "rewind") universe.setRewinding(!universe.rewinding());
        else if (name == "region") alternateRegion = !alternateRegion;
        else if (name == "supernova") {
            if (const auto location = actionLocation())
                universe.supernova(universe.screenToWorld(location->first, location->second), location->second);
        }
        damage();
    }
};

void renderWindowHook(Render::IHyprRenderer* self, PHLWINDOW window, PHLMONITOR monitor, const Time::steady_tp& time, bool decorate, Render::eRenderPassMode mode, bool ignorePosition, bool standalone) {
    // Window exports render offscreen with standalone=true; compositor-owned
    // snapshots also need the original image. Suppress only desktop rendering,
    // including the first shared frame before its sharing notification arrives.
    if (instance->active && !instance->capturing && !standalone && !self->m_bRenderingSnapshot &&
        std::ranges::any_of(instance->snapshots, [&](const auto& shot) { return shot.id == window->m_stableID; })) return;
    reinterpret_cast<RenderWindowFn>(instance->hook->m_original)(self, window, monitor, time, decorate, mode, ignorePosition, standalone);
}

void renderLayerHook(Render::IHyprRenderer* self, PHLLS layer, PHLMONITOR monitor, const Time::steady_tp& time, bool popups, bool lockscreen) {
    if (instance->active && instance->options.hideDesktopUI && !instance->capturing &&
        !self->m_bRenderingSnapshot && !lockscreen && !g_pSessionLockManager->isSessionLocked()) {
        if (instance->protectedLayer(layer)) instance->stop("protected desktop layer");
        else return; // Includes a layer's child surfaces and popup pass.
    }
    reinterpret_cast<RenderLayerFn>(instance->layerHook->m_original)(self, layer, monitor, time, popups, lockscreen);
}

void renderFadeoutsHook(Render::IHyprRenderer* self, PHLMONITOR monitor, Desktop::eFadeoutPlane plane, PHLWORKSPACE workspace) {
    if (instance->active && instance->options.hideDesktopUI && !instance->capturing &&
        !self->m_bRenderingSnapshot && !g_pSessionLockManager->isSessionLocked() &&
        (plane == Desktop::FADEOUT_PLANE_LAYER_BACKGROUND || plane == Desktop::FADEOUT_PLANE_LAYER_BOTTOM ||
         plane == Desktop::FADEOUT_PLANE_LAYER_TOP || plane == Desktop::FADEOUT_PLANE_LAYER_OVERLAY ||
         plane == Desktop::FADEOUT_PLANE_POPUP)) return;
    reinterpret_cast<RenderFadeoutsFn>(instance->fadeoutsHook->m_original)(self, monitor, plane, workspace);
}

UP<Screenshare::CScreenshareFrame> screenshareFrameHook(Screenshare::CScreenshareSession* session, bool overlayCursor) {
    // Only two timestamps are retained, not session pointers or an unbounded
    // cache. Stop BEFORE the compositor creates/copies the first shared frame.
    instance->lastShareRequest = Clock::now();
    if (instance->options.excludeShare && instance->active) instance->stop("screen sharing", false, true);
    return reinterpret_cast<ScreenshareFrameFn>(instance->shareFrameHook->m_original)(session, overlayCursor);
}

int setup(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    try {
        Options next;
        next.physics = string(L, 1, "preset", "calm") == "demo" ? cosmic::Config::demo() : cosmic::Config::calm();
        next.enabled = boolean(L, 1, "enabled", true);
        next.idleTimeout = number(L, 1, "idle_timeout", 60, .25, 3600);
        next.fps = number(L, 1, "fps", 60, 10, 120);
        next.snapshotHz = number(L, 1, "snapshot_hz", 4, .25, 30);
        next.physics.max_bodies = static_cast<std::size_t>(number(L, 1, "max_windows", 24, 1, 48));
        next.physics.seed = static_cast<uint64_t>(number(L, 1, "seed", 12606492, 0, 4294967295.0));
        next.physics.history_seconds = number(L, 1, "history_seconds", 12, .1, 60);
        next.physics.history_hz = number(L, 1, "history_hz", 30, 1, 60);
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
            PARAM(cursor_strength, 0, 2e7); PARAM(mutual_strength, 0, 2e6); PARAM(softening, 10, 2000);
            PARAM(max_acceleration, 10, 10000); PARAM(max_speed, 10, 5000); PARAM(damping, 0, 10);
            PARAM(restitution, 0, 1); PARAM(collision_strength, 0, 1); PARAM(expansion_rate, 0, .1);
            PARAM(sink_duration, .3, 20); PARAM(wormhole_cooldown, .2, 10); PARAM(explosion_strength, 0, 3000);
            PARAM(fixed_step, 1.0 / 240.0, 1.0 / 30.0); PARAM(max_substeps, 1, 16);
#undef PARAM
        }
        lua_pop(L, 1);
        lua_getfield(L, 1, "rendering");
        std::size_t stars = 240;
        double background = 1;
        if (lua_istable(L, -1)) {
            next.physics.max_particles = static_cast<std::size_t>(number(L, -1, "particles", 96, 0, 384));
            next.snapshotBudget = static_cast<std::size_t>(number(L, -1, "snapshot_mb", 128, 16, 512) * 1024 * 1024);
            stars = static_cast<std::size_t>(number(L, -1, "stars", 240, 0, 1024));
            background = number(L, -1, "background", 1, 0, 1);
            next.hideDesktopUI = boolean(L, -1, "hide_desktop_ui", true);
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
                        else if (word == "ALT") control.mods |= HL_MODIFIER_ALT;
                        else if (word == "CTRL" || word == "CONTROL") control.mods |= HL_MODIFIER_CTRL;
                        else if (word == "SHIFT") control.mods |= HL_MODIFIER_SHIFT;
                        else throw std::invalid_argument("unsupported control modifier: " + word);
                        begin = end + 1;
                    }
                    auto key = chord.substr(begin);
                    std::erase(key, ' ');
                    control.symbol = xkb_keysym_to_lower(xkb_keysym_from_name(key.c_str(), XKB_KEYSYM_CASE_INSENSITIVE));
                    if (control.symbol == XKB_KEY_NoSymbol) throw std::invalid_argument("unknown control key: " + key);
                    next.controls.push_back(control);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        instance->shutdown();
        instance->options = std::move(next);
        instance->universe.configure(instance->options.physics);
        instance->renderer.configure(stars, background);
        if (instance->options.enabled) instance->initialize();
        lua_pushboolean(L, true);
        return 1;
    } catch (const std::exception& error) {
        instance->shutdown();
        instance->fail(error.what(), false); // Lua reports synchronous failures once.
        lua_pushnil(L);
        lua_pushstring(L, error.what());
        return 2;
    }
}
int enable(lua_State* L) {
    try { instance->options.enabled = true; instance->initialize(); lua_pushboolean(L, true); }
    catch (const std::exception& error) { instance->shutdown(); instance->fail(error.what(), false); lua_pushboolean(L, false); lua_pushstring(L, error.what()); return 2; }
    return 1;
}
int disable(lua_State* L) { instance->options.enabled = false; instance->shutdown(); lua_pushboolean(L, true); return 1; }
int shutdown(lua_State* L) { instance->options.enabled = false; instance->shutdown(); lua_pushboolean(L, true); return 1; }
int action(lua_State* L) { instance->action(luaL_checkstring(L, 1)); lua_pushboolean(L, true); return 1; }
int read_user_config(lua_State* L) {
    std::size_t length = 0;
    const char* path = luaL_checklstring(L, 1, &length);
    try {
        const auto result = cosmic::readUserConfig(std::string_view(path, length));
        if (result.source) lua_pushlstring(L, result.source->data(), result.source->size());
        else lua_pushnil(L);
        if (result.error.empty()) lua_pushnil(L);
        else lua_pushlstring(L, result.error.data(), result.error.size());
        lua_pushboolean(L, result.found);
        return 3;
    } catch (const std::exception& error) {
        lua_pushnil(L);
        lua_pushstring(L, error.what());
    }
    lua_pushboolean(L, true);
    return 3;
}
int status(lua_State* L) {
    lua_newtable(L);
    auto b = [&](const char* k, bool v) { lua_pushboolean(L, v); lua_setfield(L, -2, k); };
    auto n = [&](const char* k, std::size_t v) { lua_pushinteger(L, static_cast<lua_Integer>(v)); lua_setfield(L, -2, k); };
    b("enabled", instance->options.enabled); b("initialized", instance->initialized); b("active", instance->active); b("rewinding", instance->universe.rewinding());
    n("bodies", instance->universe.bodies().size()); n("snapshots", instance->snapshots.size()); n("snapshot_bytes", instance->snapshotBytes);
    n("history_frames", instance->universe.historyFrames()); n("history_limit", instance->universe.historyLimit()); n("input_watchers", instance->listeners.size()); n("physics_steps", instance->physicsSteps);
    n("stored", std::ranges::count_if(instance->universe.bodies(), [](const auto& body) { return body.stored; }));
    n("particles", instance->universe.particles().size()); n("waves", instance->universe.waves().size());
    n("gravity_mode", static_cast<std::size_t>(instance->universe.gravityMode()));
    b("alternate_region", instance->alternateRegion);
    b("manual_preview", instance->active && instance->manualPreview);
    b("desktop_ui_hidden", instance->active && instance->options.hideDesktopUI);
    // Read-only diagnostics distinguish real activity from a persistent modal
    // guard when an otherwise idle desktop does not enter Cosmic.
    const auto ime = g_pInputManager->m_relay.m_inputMethod.lock();
    b("ime_keyboard_grab", ime && ime->hasGrab());
    b("ime_composing", instance->imeComposition());
    b("seat_grab", !!g_pSeatManager->m_seatGrab);
    b("desktop_interaction_blocked", instance->desktopInteraction());
    b("held_input", instance->held());
    b("entry_blocked", instance->blocked());
    b("manual_entry_blocked", instance->blocked(true));
    lua_pushnumber(L, std::chrono::duration<double>(Clock::now() - instance->lastInput).count());
    lua_setfield(L, -2, "idle_seconds");
    lua_newtable(L);
    int index = 1;
    for (const auto& body : instance->universe.bodies()) {
        lua_newtable(L);
        auto value = [&](const char* key, double number) { lua_pushnumber(L, number); lua_setfield(L, -2, key); };
        const auto display = instance->universe.worldToScreen(body.position, body.region);
        value("id", body.id); value("x", display.x); value("y", display.y);
        value("angle", body.angle); value("scale", body.scale); value("stretch", body.stretch);
        value("twist", body.twist); value("region", body.region); value("sink_progress", body.sink_progress);
        value("portal_progress", body.portal_progress);
        value("portal_source_region", body.portal_source_region);
        value("portal_destination_region", body.portal_destination_region);
        value("portal_entry_duration", body.portal_entry_duration);
        value("portal_exit_duration", body.portal_exit_duration);
        const auto entry = instance->universe.worldToScreen(body.portal_entry_center, body.portal_source_region);
        const auto exit = instance->universe.worldToScreen(body.portal_exit_center, body.portal_destination_region);
        value("portal_entry_x", entry.x); value("portal_entry_y", entry.y);
        value("portal_exit_x", exit.x); value("portal_exit_y", exit.y);
        lua_pushboolean(L, body.portal_emerging); lua_setfield(L, -2, "portal_emerging");
        lua_pushboolean(L, body.stored); lua_setfield(L, -2, "stored");
        lua_rawseti(L, -2, index++);
    }
    lua_setfield(L, -2, "objects");
    lua_newtable(L);
    index = 1;
    for (const auto& hole : instance->universe.wormholes()) {
        lua_newtable(L);
        const auto display = instance->universe.worldToScreen(hole.position, hole.region);
        auto value = [&](const char* key, double number) { lua_pushnumber(L, number); lua_setfield(L, -2, key); };
        value("region", hole.region); value("x", display.x); value("y", display.y);
        value("radius", hole.radius); value("partner", hole.partner + 1);
        lua_rawseti(L, -2, index++);
    }
    lua_setfield(L, -2, "wormholes");
    lua_newtable(L);
    index = 1;
    for (const auto& monitor : State::monitorState()->monitors()) {
        const int mainRegion = static_cast<int>(monitor->m_id) + (instance->alternateRegion ? 1000000 : 0);
        const auto inset = peerViewLayout(instance->universe, mainRegion);
        if (!inset) continue;
        lua_newtable(L);
        auto value = [&](const char* key, double number) { lua_pushnumber(L, number); lua_setfield(L, -2, key); };
        value("region", inset->region.id); value("x", inset->origin.x); value("y", inset->origin.y);
        value("width", inset->size.x); value("height", inset->size.y);
        value("content_x", inset->contentOrigin.x); value("content_y", inset->contentOrigin.y);
        value("content_width", inset->contentSize.x); value("content_height", inset->contentSize.y);
        value("scale", inset->scale);
        lua_rawseti(L, -2, index++);
    }
    lua_setfield(L, -2, "peer_views");
    lua_pushstring(L, instance->reason.c_str()); lua_setfield(L, -2, "last_reason");
    lua_pushstring(L, GIT_COMMIT_HASH); lua_setfield(L, -2, "hyprland_commit");
    lua_pushstring(L, "bounded refreshed snapshots"); lua_setfield(L, -2, "capture_mode");
    return 1;
}
} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    pluginHandle = handle;
    if (std::string(__hyprland_api_get_hash()) != __hyprland_api_get_client_hash() ||
        HyprlandAPI::getHyprlandVersion(handle).hash != GIT_COMMIT_HASH)
        throw std::runtime_error("Cosmic: Hyprland ABI mismatch; rebuild with the running compositor headers");
    instance = new Cosmic();
    for (auto [name, callback] : std::initializer_list<std::pair<const char*, PLUGIN_LUA_FN>>{{"setup", setup}, {"enable", enable}, {"disable", disable}, {"shutdown", shutdown}, {"action", action}, {"status", status}, {"read_user_config", read_user_config}}) {
        if (!HyprlandAPI::addLuaFunction(handle, "cosmic", name, callback)) {
            delete instance; instance = nullptr;
            throw std::runtime_error("Cosmic: Lua callback registration failed");
        }
    }
    return {"cosmic", "Idle desktop universe with reversible window imagery", "hisuic", "0.1.0"};
}
APICALL EXPORT void PLUGIN_EXIT() { delete instance; instance = nullptr; }
