-- These mocks model only the documented 0.56.2 API and verify ownership and
-- lifecycle behavior independently of a live compositor session.
local root = arg[1] or "."
package.path = root .. "/lua/?.lua;" .. root .. "/lua/?/init.lua;" .. package.path
local Config = require("cosmic.config")
local checks = 0
local function check(condition, message)
    checks = checks + 1
    assert(condition, message)
end

local default_idle = assert(Config.normalize())
check(default_idle.idle_timeout == 20 and Config.defaults.idle_timeout == 20,
    "the default cosmic idle activation delay is twenty seconds")
local short_idle = assert(Config.normalize({ idle_timeout = 5 }))
local explicit_idle = assert(Config.normalize({ idle_timeout = 20 }))
check(short_idle.idle_timeout == 5 and explicit_idle.idle_timeout == 20,
    "explicit five- and twenty-second idle delays retain their requested values")
local idle_demo = assert(Config.normalize({ preset = "demo" }, short_idle))
local idle_calm = assert(Config.normalize({ preset = "calm" }, idle_demo))
check(idle_demo.idle_timeout == 5 and idle_calm.idle_timeout == 5,
    "switching presets preserves the explicitly configured idle delay")

local previous = Config.normalize({ idle_timeout = 8, effects = { orbit = false } })
check(previous.idle_timeout == 8 and previous.effects.binary and not previous.effects.orbit, "nested partial setup preserves defaults")
for _, invalid in ipairs({ false, "invalid", { idle_timeout = 0 }, { max_windows = 49 },
    { physics = { fixed_step = 0 } }, { physics = { max_speed = math.huge } },
    { rendering = { particles = -1 } }, { unknown = true }, { effects = { orbit = 1 } },
    { controls = { preview = "F6" } }, { exclusions = { classes = { [2] = "game" } } },
    { controls = { preview = "SUPER+ALT+C", emergency = "ALT+SUPER+C" } },
    { controls = { preview = "HYPER+F11" } }, { controls = { preview = "SUPER++F11" } },
    { plugin_path = "relative.so" }, { preset = "invalid" } }) do
    local result, message = Config.normalize(invalid, previous)
    check(result == nil and type(message) == "string", "invalid options are rejected")
    check(previous.idle_timeout == 8 and not previous.effects.orbit, "failed validation does not mutate previous options")
end
local demo = assert(Config.normalize({ preset = "demo", physics = { sink_duration = 3 } }, previous))
check(demo.physics.cursor_strength == 2100000 and demo.physics.expansion_rate == 0.025 and
    demo.physics.sink_duration == 3 and demo.idle_timeout == 8, "preset and explicit overrides compose")
local calm = assert(Config.normalize({ preset = "calm" }, demo))
check(calm.physics.cursor_strength == Config.defaults.physics.cursor_strength, "switching presets resets preset physics")
local chords = assert(Config.normalize({ controls = { preview = "alt + super + c" } }))
check(chords.controls.preview == "SUPER+ALT+C", "controls normalize case, whitespace and modifier order before native parsing")
-- These limits match both the native setup parser and Universe::configure;
-- successful options must retain their value rather than silently clamp later.
for _, boundary in ipairs({
    { "cursor_strength", 0, 20000000 }, { "mutual_strength", 0, 2000000 },
    { "softening", 10, 2000 }, { "max_acceleration", 10, 10000 },
    { "max_speed", 10, 5000 }, { "fixed_step", 1 / 240, 1 / 30 },
    { "sink_duration", 0.3, 20 }, { "wormhole_cooldown", 0.2, 10 },
    { "explosion_strength", 0, 3000 },
}) do
    local name, low, high = boundary[1], boundary[2], boundary[3]
    local minimum = assert(Config.normalize({ physics = { [name] = low } }))
    local maximum = assert(Config.normalize({ physics = { [name] = high } }))
    check(minimum.physics[name] == low and maximum.physics[name] == high, name .. " accepts effective boundaries")
    check(not Config.normalize({ physics = { [name] = low - 0.00001 } }), name .. " rejects below its effective minimum")
    check(not Config.normalize({ physics = { [name] = high + 0.00001 } }), name .. " rejects above its effective maximum")
end
check(Config.normalize({ history_hz = 60 }).history_hz == 60 and
    not Config.normalize({ history_hz = 60.01 }), "history frequency matches the simulation's sixty Hz cap")

local default_sky = assert(Config.normalize())
check(default_sky.rendering.background == 1 and default_sky.rendering.stars == 240,
    "default cosmic sky fully covers the wallpaper with 240 ASCII stars")
for _, opacity in ipairs({ 0, 0.375, 1 }) do
    local sky = assert(Config.normalize({ rendering = { background = opacity } }))
    check(sky.rendering.background == opacity, "background opacity retains accepted values including both boundaries")
end
for _, opacity in ipairs({ -0.00001, 1.00001, 0 / 0, math.huge, -math.huge }) do
    local sky, message = Config.normalize({ rendering = { background = opacity } }, default_sky)
    check(sky == nil and type(message) == "string", "invalid background opacity is rejected")
    check(default_sky.rendering.background == 1, "invalid opacity does not mutate the accepted sky")
end
for _, stars in ipairs({ 0, 17, 240, 1024 }) do
    local sky = assert(Config.normalize({ rendering = { stars = stars } }))
    check(sky.rendering.stars == stars, "ASCII star count retains accepted integers including both boundaries")
end
for _, stars in ipairs({ -1, 1025, 0.5, 1023.5, 0 / 0, math.huge }) do
    local sky, message = Config.normalize({ rendering = { stars = stars } }, default_sky)
    check(sky == nil and type(message) == "string", "invalid ASCII star count is rejected")
    check(default_sky.rendering.stars == 240, "invalid star count does not mutate the accepted sky")
end
for _, seed in ipairs({ 0, 0xFFFFFFFF }) do
    local sky = assert(Config.normalize({ seed = seed }))
    check(sky.seed == seed, "sky seed retains both unsigned 32-bit boundaries")
end
local custom_sky = assert(Config.normalize({ seed = 0xFFFFFFFF,
    rendering = { background = 0.625, stars = 377 } }))
local partial_sky = assert(Config.normalize({ idle_timeout = 9,
    rendering = { particles = 12 } }, custom_sky))
check(partial_sky.rendering.background == 0.625 and partial_sky.rendering.stars == 377 and
    partial_sky.seed == 0xFFFFFFFF and partial_sky.rendering.particles == 12,
    "partial reconfiguration preserves custom opacity, ASCII star count and sky seed")
check(custom_sky.idle_timeout == Config.defaults.idle_timeout and custom_sky.rendering.particles == 96,
    "partial sky reconfiguration does not mutate the previous options")
local demo_sky = assert(Config.normalize({ preset = "demo" }, partial_sky))
check(demo_sky.rendering.background == 1 and demo_sky.rendering.stars == 240 and
    demo_sky.rendering.particles == 384 and demo_sky.seed == 0xFFFFFFFF and demo_sky.idle_timeout == 9,
    "switching to demo resets rendering defaults while preserving unrelated sky seed and idle timeout")
local custom_demo_sky = assert(Config.normalize({ rendering = { background = 0, stars = 0 } }, demo_sky))
local calm_sky = assert(Config.normalize({ preset = "calm" }, custom_demo_sky))
check(calm_sky.rendering.background == 1 and calm_sky.rendering.stars == 240 and
    calm_sky.rendering.particles == 96 and calm_sky.seed == 0xFFFFFFFF,
    "switching back to calm restores the opaque ASCII sky and calm particle default")
local explicit_demo_sky = assert(Config.normalize({ preset = "demo",
    rendering = { background = 0.25, stars = 1024 } }, calm_sky))
check(explicit_demo_sky.rendering.background == 0.25 and explicit_demo_sky.rendering.stars == 1024 and
    explicit_demo_sky.rendering.particles == 384,
    "explicit sky overrides compose with a preset change")

local original_open = io.open
local function mock(mode)
    local state = { events = {}, bindings = {}, timers = {}, setups = 0, notifications = 0, loads = {},
        enable = 0, disable = 0, shutdown = 0, actions = {}, initialized = false, enabled = false }
    io.open = function(path)
        if mode == "missing" then return nil, "missing test plugin" end
        return { close = function() end }
    end
    local api = {
        setup = function(options)
            state.setups = state.setups + 1
            if state.fail_setup then state.initialized = false; state.enabled = false; return nil, "runtime setup failure" end
            if mode == "nil-error" then return nil, "test initialization failure" end
            if mode == "throw" then error("test exception") end
            state.options = Config.copy(options)
            state.enabled = options.enabled
            state.initialized = options.enabled
            return true
        end,
        enable = function() state.enable = state.enable + 1; state.enabled = true; state.initialized = true; return true end,
        disable = function() state.disable = state.disable + 1; state.enabled = false; state.initialized = false; return true end,
        shutdown = function() state.shutdown = state.shutdown + 1; state.enabled = false; state.initialized = false; return true end,
        action = function(action) state.actions[#state.actions + 1] = action; return true end,
        status = function() return { enabled = state.enabled, initialized = state.initialized, active = false, input_watchers = state.initialized and 1 or 0 } end,
    }
    _G.hl = {
        plugin = { load = function(path) state.loads[#state.loads + 1] = path end,
            cosmic = (mode ~= "missing" and mode ~= "delayed") and api or nil },
        notification = { create = function() state.notifications = state.notifications + 1 end },
        on = function(event, callback)
            local subscription = { active = true, callback = callback }
            function subscription:remove() self.active = false end
            state.events[event] = state.events[event] or {}
            table.insert(state.events[event], subscription)
            return subscription
        end,
        bind = function(chord, callback)
            local binding = { chord = chord, callback = callback, active = true }
            function binding:remove() self.active = false end
            state.bindings[#state.bindings + 1] = binding
            return binding
        end,
        timer = function(callback, timer_options)
            check(timer_options.type == "oneshot" and timer_options.timeout == 100, "initialization uses bounded one-shot probes")
            local timer = { active = true, callback = callback, timeout = timer_options.timeout }
            function timer:set_timeout(timeout) self.timeout = timeout; self.active = true end
            state.timers[#state.timers + 1] = timer
            return timer
        end,
    }
    function state.fire(event)
        local listeners = {}
        for _, listener in ipairs(state.events[event] or {}) do listeners[#listeners + 1] = listener end
        for _, listener in ipairs(listeners) do if listener.active then listener.callback() end end
    end
    function state.live_bindings()
        local count = 0
        for _, binding in ipairs(state.bindings) do if binding.active then count = count + 1 end end
        return count
    end
    function state.live_events()
        local count = 0
        for _, listeners in pairs(state.events) do
            for _, listener in ipairs(listeners) do if listener.active then count = count + 1 end end
        end
        return count
    end
    function state.fire_timers()
        local timers = {}
        for _, timer in ipairs(state.timers) do if timer.active then timers[#timers + 1] = timer end end
        for _, timer in ipairs(timers) do timer.active = false; timer.callback() end
    end
    function state.publish_native() hl.plugin.cosmic = api end
    package.loaded.cosmic = nil
    return require("cosmic"), state
end

local cosmic, state = mock("success")
check(require("cosmic") == cosmic and #state.loads == 1, "repeated require retains the single module instance")
check(state.loads[1] == root .. "/lua/cosmic.so", "plugin path removes the module component without symlink traversal")
check(state.setups == 0 and state.live_bindings() == 0, "initialization waits for plugin loading to complete")
check(cosmic.setup({ idle_timeout = 6 }) == cosmic, "setup before plugin load records options")
state.fire("config.reloaded")
state.fire("config.reloaded")
check(state.setups == 1 and state.options.idle_timeout == 6, "recursive plugin reload emits initialize only once")
check(state.live_bindings() == 7 and state.live_events() == 2, "one set of controls and event ownership")
check(cosmic.setup({ effects = { wormholes = false }, controls = { supernova = false } }) == cosmic, "setup updates native options")
check(state.live_bindings() == 6 and state.setups == 2, "setup replaces bindings without duplication")
local count = state.setups
local ok, message = cosmic.setup({ idle_timeout = -5 })
check(not ok and message and state.setups == count and cosmic.status().config.idle_timeout == 6, "invalid update never reaches native code")
local status = cosmic.status()
status.config.effects.orbit = false
check(cosmic.status().config.effects.orbit, "status returns independent configuration data")
check(cosmic.disable() and not cosmic.status().initialized and cosmic.status().module_initialized and state.live_bindings() == 0, "disable releases native watchers and consuming controls")
check(cosmic.enable() and cosmic.status().initialized and state.live_bindings() == 6, "enable restores one set of watchers and controls")
check(cosmic.action("black_hole") and state.actions[1] == "black_hole", "dedicated actions reach native API")
check(not cosmic.action("invalid"), "unknown actions are rejected")
check(cosmic.shutdown() and cosmic.shutdown(), "shutdown is repeatable")
check(state.shutdown == 1 and state.live_bindings() == 0 and state.live_events() == 0, "shutdown releases all Lua ownership once")
check(not cosmic.enable(), "enable after full shutdown requires setup")
check(cosmic.setup({ enabled = false }) == cosmic and state.setups == 3 and state.live_bindings() == 0, "setup restarts ownership after shutdown without enabling consuming controls")
state.fire("hyprland.shutdown")
check(state.live_bindings() == 0 and state.live_events() == 0 and cosmic.status().input_watchers == 0, "compositor shutdown cleans module ownership and exposes native counters")

local recovering, recovery = mock("success")
recovery.fire("config.reloaded")
recovery.fail_setup = true
check(not recovering.setup({ idle_timeout = 11 }) and recovery.live_bindings() == 0 and
    recovering.status().config.idle_timeout == Config.defaults.idle_timeout, "native reconfiguration failure releases old consuming controls and keeps accepted options")
recovery.fail_setup = false
check(recovering.setup({ idle_timeout = 7 }) == recovering and recovery.live_bindings() == 7 and
    recovering.status().initialized, "valid setup recovers after a native failure without a reload")
recovering.shutdown()

local reenabling, reenable = mock("success")
reenable.fire("config.reloaded")
reenable.fail_setup = true
check(not reenabling.setup({ idle_timeout = 11 }), "native reconfiguration failure prepares enable recovery")
check(not reenabling.enable() and reenable.setups == 3 and reenable.live_bindings() == 0 and
    not reenabling.status().initialized, "enable reports a continuing native initialization failure")
reenable.fail_setup = false
check(reenabling.enable() and reenable.setups == 4 and reenable.live_bindings() == 7 and
    reenabling.status().initialized and reenable.options.idle_timeout == Config.defaults.idle_timeout,
    "enable reinitializes accepted configuration after native failure")
reenabling.shutdown()
check(not reenabling.enable() and reenable.setups == 4, "enable still requires setup after full shutdown")

local delayed, deferred = mock("delayed")
deferred.fire("config.reloaded")
deferred.fire("config.reloaded")
check(#deferred.timers == 1 and delayed.status().initialization_pending and deferred.notifications == 0,
    "early startup reload starts one probe without a false unavailable warning")
deferred.fire_timers()
check(#deferred.timers == 2 and deferred.setups == 0 and deferred.notifications == 0,
    "plugin availability probes remain silent before their deadline")
deferred.publish_native()
deferred.fire_timers()
check(deferred.setups == 1 and deferred.notifications == 0 and not delayed.status().initialization_pending,
    "late plugin availability initializes exactly once and stops probing")
deferred.fire_timers()
check(deferred.setups == 1, "completed initialization schedules no further probes")
delayed.shutdown()

local canceled, cancellation = mock("delayed")
cancellation.fire("config.reloaded")
canceled.shutdown()
check(cancellation.timers[1].timeout == 1 and not canceled.status().initialization_pending,
    "shutdown cancels pending initialization work and drains its one-shot reference")
cancellation.publish_native()
cancellation.fire_timers()
check(cancellation.setups == 0 and cancellation.live_events() == 0 and cancellation.live_bindings() == 0,
    "a canceled probe cannot initialize even if the native plugin later becomes available")

local timed_out, unavailable = mock("delayed")
unavailable.fire("config.reloaded")
for _ = 1, 50 do unavailable.fire_timers() end
check(#unavailable.timers == 50 and unavailable.notifications == 1 and not timed_out.status().initialization_pending,
    "unavailable plugin reports once after fifty bounded probes and stops all scheduling")
unavailable.fire("config.reloaded")
check(#unavailable.timers == 50 and unavailable.notifications == 1, "the failed deadline does not create a perpetual retry timer")
check(timed_out.enable() and timed_out.status().initialization_pending and #unavailable.timers == 51,
    "explicit enable restarts a bounded availability attempt after its failed deadline")
check(timed_out.enable() and #unavailable.timers == 51, "repeated enable while pending preserves one availability probe")
unavailable.publish_native()
unavailable.fire_timers()
check(unavailable.setups == 1 and timed_out.status().initialized and unavailable.notifications == 1,
    "enable retry initializes when native becomes available without duplicate failure notifications")
timed_out.shutdown()

for _, mode in ipairs({ "nil-error", "throw", "missing" }) do
    local failed, failure = mock(mode)
    failure.fire("config.reloaded")
    failure.fire("config.reloaded")
    check(not failed.status().initialized and failure.live_bindings() == 0, "failed initialization never registers consuming controls")
    check(failure.notifications == 1 and failed.status().error, "failure is explained once per module lifecycle")
    if mode == "missing" then
        check(not failed.enable() and #failure.timers == 0,
            "enable reports a missing unregistered plugin without claiming success or scheduling futile probes")
    end
    failed.shutdown()
    check(failure.live_events() == 0, "failed module can be shut down completely")
end
io.open = original_open
_G.hl = nil
print("Lua configuration/lifecycle: " .. checks .. " checks passed")
