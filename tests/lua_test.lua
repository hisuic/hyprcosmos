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
check(default_idle.idle_timeout == 60 and Config.defaults.idle_timeout == 60,
    "the default cosmic idle activation delay is sixty seconds")
local short_idle = assert(Config.normalize({ idle_timeout = 5 }))
local explicit_idle = assert(Config.normalize({ idle_timeout = 20 }))
check(short_idle.idle_timeout == 5 and explicit_idle.idle_timeout == 20,
    "explicit five- and twenty-second idle delays retain their requested values")
local idle_demo = assert(Config.normalize({ preset = "demo" }, short_idle))
local idle_calm = assert(Config.normalize({ preset = "calm" }, idle_demo))
check(idle_demo.idle_timeout == 5 and idle_calm.idle_timeout == 5,
    "switching presets preserves the explicitly configured idle delay")

check(default_idle.max_windows == 24 and default_idle.max_objects == 64 and
    Config.defaults.max_objects == 64,
    "real window captures and persistent celestial objects have separate default limits")
for _, count in ipairs({16, 64, 128}) do
    local objects = assert(Config.normalize({ max_objects = count }))
    check(objects.max_objects == count and objects.max_windows == 24,
        "the total object limit accepts and preserves supported integer boundaries")
end
for _, invalid in ipairs({15, 129, 64.5, 0 / 0, math.huge, -math.huge, "64", false, {}}) do
    local objects, message = Config.normalize({ max_objects = invalid })
    check(objects == nil and type(message) == "string" and message:find("max_objects", 1, true),
        "invalid total object limits are rejected with the exact configuration path")
end
local object_limits = assert(Config.normalize({ max_objects = 96, max_windows = 48 }))
local partial_limits = assert(Config.normalize({ stellar = { fragments = 4 } }, object_limits))
local preset_limits = assert(Config.normalize({ preset = "demo" }, partial_limits))
check(partial_limits.max_objects == 96 and partial_limits.max_windows == 48 and
    preset_limits.max_objects == 96 and preset_limits.max_windows == 48 and
    object_limits.stellar.fragments == 16,
    "partial settings and preset switches preserve object budgets without mutating prior options")
check(Config.normalize({ max_objects = 16, max_windows = 48 }).max_windows == 48,
    "the total object budget does not silently rewrite the independent capture limit")

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

local default_stellar = assert(Config.normalize())
check(default_stellar.controls.stellar_nova == "F5" and default_stellar.controls.supernova == "F9",
    "targeted cinematic explosions use F5 without changing F9's immediate shockwave")
check(default_stellar.stellar.automatic and default_stellar.stellar.interval_min == 70 and
    default_stellar.stellar.interval_max == 130 and default_stellar.stellar.charge_seconds == 3.5 and
    default_stellar.stellar.fragment_seconds == 3 and default_stellar.stellar.fragments == 16 and
    default_stellar.stellar.growth == 1.7,
    "cinematic explosions default to rare automatic events, a visible charge and sixteen persistent shared-image fragments")
for _, boundary in ipairs({
    { "interval_min", 5, 1800 }, { "interval_max", 5, 1800 },
    { "charge_seconds", 0.5, 15 }, { "fragment_seconds", 1, 8 }, { "growth", 1.05, 2.5 },
}) do
    local name, low, high = boundary[1], boundary[2], boundary[3]
    for _, value in ipairs({low, high}) do
        local patch = { [name] = value }
        if name == "interval_min" then patch.interval_max = 1800 end
        if name == "interval_max" then patch.interval_min = 5 end
        local stellar = assert(Config.normalize({ stellar = patch }))
        check(stellar.stellar[name] == value, "stellar." .. name .. " retains its effective boundary")
    end
    for _, invalid in ipairs({low - 0.00001, high + 0.00001, 0 / 0, math.huge, -math.huge, "3"}) do
        local stellar, message = Config.normalize({ stellar = { [name] = invalid } }, default_stellar)
        check(stellar == nil and type(message) == "string" and message:find("stellar." .. name, 1, true),
            "stellar." .. name .. " rejects invalid values with a precise path")
    end
end
for _, count in ipairs({4, 16}) do
    check(Config.normalize({ stellar = { fragments = count } }).stellar.fragments == count,
        "four and sixteen fragment grids are supported without clamping")
end
for _, invalid in ipairs({0, 1, 8, 15, 17, 4.5, 0 / 0, math.huge, "16", false, {}}) do
    local stellar, message = Config.normalize({ stellar = { fragments = invalid } })
    check(not stellar and message:find("stellar.fragments", 1, true),
        "only four or sixteen fragments are accepted")
end
for _, invalid in ipairs({0, 1, "false", {}}) do
    local stellar, message = Config.normalize({ stellar = { automatic = invalid } })
    check(not stellar and message:find("stellar.automatic", 1, true),
        "automatic explosions require a boolean")
end
check(not Config.normalize({ stellar = { interval_min = 131 } }) and
    not Config.normalize({ stellar = { interval_max = 69 } }),
    "an inverted explosion interval is rejected after applying partial updates")
local custom_stellar = assert(Config.normalize({ stellar = {
    automatic = false, interval_min = 200, interval_max = 300, fragments = 4,
} }))
local partial_stellar = assert(Config.normalize({ stellar = { charge_seconds = 5 }, idle_timeout = 91 }, custom_stellar))
check(not partial_stellar.stellar.automatic and partial_stellar.stellar.fragments == 4 and
    partial_stellar.stellar.interval_min == 200 and partial_stellar.stellar.interval_max == 300 and
    partial_stellar.stellar.charge_seconds == 5 and custom_stellar.stellar.charge_seconds == 3.5 and
    partial_stellar.controls.stellar_nova == "F5",
    "partial cinematic configuration preserves custom values, manual controls and prior immutable options")
check(not Config.normalize({ stellar = { interval_min = 301 } }, custom_stellar) and
    custom_stellar.stellar.interval_min == 200,
    "invalid interval updates do not mutate the previously accepted settings")
local demo_stellar = assert(Config.normalize({ preset = "demo" }, partial_stellar))
check(not demo_stellar.stellar.automatic and demo_stellar.stellar.charge_seconds == 5 and
    demo_stellar.stellar.fragments == 4,
    "preset changes preserve deliberately configured cinematic timing and fragment count")
check(not Config.normalize({ controls = { preview = "F5" } }) and
    Config.normalize({ controls = { stellar_nova = false, preview = "F5" } }).controls.preview == "F5",
    "the cinematic control participates in duplicate detection and can be explicitly disabled")

local default_sky = assert(Config.normalize())
check(default_sky.rendering.background == 1 and default_sky.rendering.stars == 240,
    "default cosmic sky fully covers the wallpaper with 240 ASCII stars")
check(default_sky.rendering.hide_desktop_ui == true and Config.defaults.rendering.hide_desktop_ui == true,
    "cosmic hides ordinary desktop layer UI by default")
for _, hidden in ipairs({ false, true }) do
    local sky = assert(Config.normalize({ rendering = { hide_desktop_ui = hidden } }))
    check(sky.rendering.hide_desktop_ui == hidden, "desktop UI visibility retains either explicit boolean value")
end
local visible_ui = assert(Config.normalize({ rendering = { hide_desktop_ui = false } }))
local partial_ui = assert(Config.normalize({ rendering = { stars = 127 }, idle_timeout = 23 }, visible_ui))
check(partial_ui.rendering.hide_desktop_ui == false and partial_ui.rendering.stars == 127 and
    partial_ui.idle_timeout == 23 and visible_ui.rendering.stars == 240,
    "partial setup preserves desktop UI visibility without mutating previous options")
for _, invalid in ipairs({ 0, 1, "false", "true", {}, function() end }) do
    local sky, message = Config.normalize({ rendering = { hide_desktop_ui = invalid } }, visible_ui)
    check(sky == nil and type(message) == "string" and message:find("rendering.hide_desktop_ui", 1, true),
        "non-boolean desktop UI visibility is rejected with its configuration path")
    check(visible_ui.rendering.hide_desktop_ui == false,
        "invalid desktop UI visibility does not mutate the accepted rendering options")
end
local demo_ui = assert(Config.normalize({ preset = "demo" }, visible_ui))
local explicit_demo_ui = assert(Config.normalize({ preset = "demo",
    rendering = { hide_desktop_ui = false } }, visible_ui))
local calm_ui = assert(Config.normalize({ preset = "calm" }, explicit_demo_ui))
check(demo_ui.rendering.hide_desktop_ui == true and explicit_demo_ui.rendering.hide_desktop_ui == false and
    calm_ui.rendering.hide_desktop_ui == true,
    "preset changes reset desktop UI visibility while honoring explicit rendering overrides")
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
local function mock(mode, user_config_source)
    local state = { events = {}, bindings = {}, timers = {}, setups = 0, notifications = 0, loads = {},
        enable = 0, disable = 0, shutdown = 0, actions = {}, initialized = false, enabled = false, active = false,
        config_opens = 0, user_config_source = user_config_source }
    io.open = function(path)
        if path:match("/hyprcosmos%.lua$") then
            error("user settings must never use blocking Lua io.open")
        end
        if mode == "missing" then return nil, "missing test plugin" end
        return { close = function() end }
    end
    local api = {
        read_user_config = function(path)
            state.config_opens = state.config_opens + 1
            state.config_path = path
            if state.reader_error then return nil, state.reader_error, true end
            return state.user_config_source, nil, state.user_config_source ~= nil
        end,
        setup = function(options)
            state.setups = state.setups + 1
            state.active = false
            if state.fail_setup then state.initialized = false; state.enabled = false; return nil, "runtime setup failure" end
            if mode == "nil-error" then return nil, "test initialization failure" end
            if mode == "throw" then error("test exception") end
            state.options = Config.copy(options)
            state.enabled = options.enabled
            state.initialized = options.enabled
            return true
        end,
        enable = function() state.enable = state.enable + 1; state.enabled = true; state.initialized = true; return true end,
        disable = function() state.disable = state.disable + 1; state.enabled = false; state.initialized = false; state.active = false; return true end,
        shutdown = function() state.shutdown = state.shutdown + 1; state.enabled = false; state.initialized = false; state.active = false; return true end,
        action = function(action)
            state.actions[#state.actions + 1] = action
            if state.fail_action then return nil, "fixture action failure" end
            return true
        end,
        status = function()
            if state.fail_status then return nil, "fixture status failure" end
            if state.throw_status then error("fixture status exception") end
            if state.invalid_status then return state.invalid_status end
            return { enabled = state.enabled, initialized = state.initialized, active = state.active,
                input_watchers = state.initialized and 1 or 0 }
        end,
    }
    if mode == "old-api" then api.read_user_config = nil end
    _G.hl = {
        plugin = { load = function(path) state.loads[#state.loads + 1] = path end,
            cosmic = (mode ~= "missing" and mode ~= "delayed") and api or nil },
        notification = { create = function(notification)
            state.notifications = state.notifications + 1
            state.notification_text = notification.text
        end },
        on = function(event, callback)
            local subscription = { active = true, callback = callback }
            function subscription:remove() self.active = false end
            state.events[event] = state.events[event] or {}
            table.insert(state.events[event], subscription)
            return subscription
        end,
        bind = function(chord, callback, bind_options)
            local binding = { chord = chord, callback = callback, active = true, options = bind_options }
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
    function state.binding(chord)
        for _, binding in ipairs(state.bindings) do
            if binding.active and binding.chord == chord then return binding end
        end
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
check(state.live_bindings() == 8 and state.live_events() == 2, "one set of controls and event ownership")
check(cosmic.setup({ effects = { wormholes = false }, controls = { supernova = false } }) == cosmic, "setup updates native options")
check(state.live_bindings() == 7 and state.setups == 2, "setup replaces bindings without duplication")
check(state.options.rendering.hide_desktop_ui == true,
    "native setup receives the default desktop UI suppression option")
check(cosmic.setup({ rendering = { hide_desktop_ui = false } }) == cosmic and
    state.options.rendering.hide_desktop_ui == false and not cosmic.status().config.rendering.hide_desktop_ui,
    "desktop UI visibility reconfiguration reaches native setup and module status")
local count = state.setups
local ok, message = cosmic.setup({ idle_timeout = -5 })
check(not ok and message and state.setups == count and cosmic.status().config.idle_timeout == 6, "invalid update never reaches native code")
local status = cosmic.status()
status.config.effects.orbit = false
check(cosmic.status().config.effects.orbit, "status returns independent configuration data")
check(cosmic.disable() and not cosmic.status().initialized and cosmic.status().module_initialized and state.live_bindings() == 0, "disable releases native watchers and consuming controls")
check(cosmic.enable() and cosmic.status().initialized and state.live_bindings() == 7, "enable restores one set of watchers and controls")
check(cosmic.action("black_hole") and state.actions[1] == "black_hole", "dedicated actions reach native API")
check(not cosmic.action("invalid"), "unknown actions are rejected")
check(cosmic.shutdown() and cosmic.shutdown(), "shutdown is repeatable")
check(state.shutdown == 1 and state.live_bindings() == 0 and state.live_events() == 0, "shutdown releases all Lua ownership once")
check(not cosmic.enable(), "enable after full shutdown requires setup")
check(cosmic.setup({ enabled = false }) == cosmic and state.setups == 4 and state.live_bindings() == 0, "setup restarts ownership after shutdown without enabling consuming controls")
state.fire("hyprland.shutdown")
check(state.live_bindings() == 0 and state.live_events() == 0 and cosmic.status().input_watchers == 0, "compositor shutdown cleans module ownership and exposes native counters")

local stellar_module, stellar_state = mock("success")
stellar_state.fire("config.reloaded")
local stellar_binding = assert(stellar_state.binding("F5"))
check(stellar_binding.options.auto_consuming and not stellar_binding.options.non_consuming and
    stellar_binding.options.submap_universal and stellar_state.live_bindings() == 8,
    "F5 uses Hyprland's native conditional-consumption option without adding a lifecycle poll")
local passed = stellar_binding.callback()
check(passed.ok == false and passed.pass_event == true and #stellar_state.actions == 0 and
    #stellar_state.timers == 0,
    "ordinary F5 passes to applications and never invokes a Cosmic action or polling timer")
stellar_state.active = true
local consumed = stellar_binding.callback()
check(consumed.ok == true and not consumed.pass_event and #stellar_state.actions == 1 and
    stellar_state.actions[1] == "stellar_nova",
    "F5 selects the cinematic action and consumes the key only while Cosmic is active")
stellar_state.fail_action = true
consumed = stellar_binding.callback()
check(consumed.ok == true and not consumed.pass_event,
    "an unavailable cinematic target never leaks F5 into the hidden application")
stellar_state.fail_action = false
local action_count = #stellar_state.actions
for _, failure in ipairs({"fail_status", "throw_status", "invalid_status"}) do
    stellar_state[failure] = true
    local guarded = stellar_binding.callback()
    check(guarded.ok == false and guarded.pass_event == true and #stellar_state.actions == action_count,
        "missing or invalid native active status safely passes F5 instead of globally consuming it")
    stellar_state[failure] = nil
end
local stellar_api, stellar_status = hl.plugin.cosmic, hl.plugin.cosmic.status
hl.plugin.cosmic.status = nil
passed = stellar_binding.callback()
check(not passed.ok and passed.pass_event and #stellar_state.actions == action_count,
    "an incompatible native status API cannot consume ordinary F5")
hl.plugin.cosmic.status = stellar_status
hl.plugin.cosmic = nil
passed = stellar_binding.callback()
check(not passed.ok and passed.pass_event and #stellar_state.actions == action_count,
    "a missing native plugin cannot consume F5 through a retained callback")
hl.plugin.cosmic = stellar_api
check(stellar_module.setup({ stellar = { automatic = false, fragments = 4 },
    controls = { stellar_nova = "F4" } }) == stellar_module and not stellar_binding.active and
    not stellar_state.binding("F5") and stellar_state.binding("F4").options.auto_consuming,
    "reassigning the cinematic control replaces the old F5 binding without changing conditional semantics")
check(not stellar_state.options.stellar.automatic and stellar_state.options.stellar.fragments == 4 and
    stellar_state.options.stellar.charge_seconds == 3.5,
    "validated cinematic settings reach native setup with defaults preserved")
passed = stellar_state.binding("F4").callback()
check(not passed.ok and passed.pass_event,
    "a reassigned cinematic key also passes outside Cosmic after setup resets its active phase")
stellar_state.active = true
consumed = stellar_state.binding("F4").callback()
check(consumed.ok and not consumed.pass_event and stellar_state.actions[#stellar_state.actions] == "stellar_nova",
    "disabling automatic explosions leaves manual targeted explosions available")
check(stellar_module.setup({ controls = { stellar_nova = false } }) == stellar_module and
    stellar_state.live_bindings() == 7 and not stellar_state.binding("F4") and stellar_state.binding("F9"),
    "disabling the cinematic control releases its binding while retaining the independent F9 shockwave")
check(stellar_module.setup({ controls = { stellar_nova = "F5" } }) == stellar_module,
    "the cinematic binding can be restored after being disabled")
local restored_stellar_binding = assert(stellar_state.binding("F5"))
check(stellar_module.disable() and not restored_stellar_binding.active and stellar_state.live_bindings() == 0,
    "module disable removes the conditional cinematic binding along with all ordinary controls")
passed = restored_stellar_binding.callback()
check(not passed.ok and passed.pass_event,
    "a stale conditional callback after disable cannot consume F5")
check(stellar_module.enable() and stellar_state.binding("F5").options.auto_consuming and
    stellar_state.live_bindings() == 8,
    "enable restores exactly one conditional cinematic binding")
restored_stellar_binding = assert(stellar_state.binding("F5"))
check(stellar_module.shutdown() and not restored_stellar_binding.active and stellar_state.live_bindings() == 0,
    "shutdown releases cinematic key ownership completely")
passed = restored_stellar_binding.callback()
check(not passed.ok and passed.pass_event,
    "a stale conditional callback after shutdown cannot consume F5")

local recovering, recovery = mock("success")
recovery.fire("config.reloaded")
recovery.fail_setup = true
check(not recovering.setup({ idle_timeout = 11 }) and recovery.live_bindings() == 0 and
    recovering.status().config.idle_timeout == Config.defaults.idle_timeout, "native reconfiguration failure releases old consuming controls and keeps accepted options")
recovery.fail_setup = false
check(recovering.setup({ idle_timeout = 7 }) == recovering and recovery.live_bindings() == 8 and
    recovering.status().initialized, "valid setup recovers after a native failure without a reload")
recovering.shutdown()

local reenabling, reenable = mock("success")
reenable.fire("config.reloaded")
reenable.fail_setup = true
check(not reenabling.setup({ idle_timeout = 11 }), "native reconfiguration failure prepares enable recovery")
check(not reenabling.enable() and reenable.setups == 3 and reenable.live_bindings() == 0 and
    not reenabling.status().initialized, "enable reports a continuing native initialization failure")
reenable.fail_setup = false
check(reenabling.enable() and reenable.setups == 4 and reenable.live_bindings() == 8 and
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

local delayed_file, deferred_file = mock("delayed", [[
    return { idle_timeout = 91, rendering = { stars = 127 }, effects = { orbit = false } }
]])
check(deferred_file.config_opens == 0 and not delayed_file.status().config_file_loaded,
    "startup does not open user settings before the safe native reader exists")
check(delayed_file.setup({ idle_timeout = 23 }) == delayed_file and
    delayed_file.setup({ rendering = { particles = 19 } }) == delayed_file,
    "multiple explicit setup patches can be recorded before the native API is published")
deferred_file.fire("config.reloaded")
deferred_file.publish_native()
deferred_file.fire_timers()
check(deferred_file.config_opens == 1 and delayed_file.status().config_file_loaded and
    deferred_file.options.idle_timeout == 23 and deferred_file.options.rendering.stars == 127 and
    deferred_file.options.rendering.particles == 19 and not deferred_file.options.effects.orbit,
    "deferred startup honors defaults, then user settings, then every explicit setup patch")
check(deferred_file.setups == 1 and deferred_file.notifications == 0 and
    not delayed_file.status().initialization_pending,
    "deferred settings initialization registers one native setup without warnings or stale probes")
delayed_file.shutdown()

local delayed_disabled, deferred_disabled = mock("delayed", "return {enabled=true, idle_timeout=91}")
check(delayed_disabled.disable(), "disable before native availability records the requested state")
deferred_disabled.fire("config.reloaded")
deferred_disabled.publish_native()
deferred_disabled.fire_timers()
check(not deferred_disabled.options.enabled and deferred_disabled.live_bindings() == 0 and
    deferred_disabled.options.idle_timeout == 91,
    "deferred user settings cannot undo an explicit early disable")
delayed_disabled.shutdown()

local delayed_bad, deferred_bad = mock("delayed", "return {idle_timeout=0}")
delayed_bad.setup({ rendering = { stars = 127 } })
deferred_bad.fire("config.reloaded")
deferred_bad.publish_native()
deferred_bad.fire_timers()
check(deferred_bad.config_opens == 1 and deferred_bad.setups == 0 and deferred_bad.disable == 1 and
    deferred_bad.notifications == 1 and delayed_bad.status().config_file_error and
    not delayed_bad.status().initialization_pending and deferred_bad.live_bindings() == 0,
    "invalid deferred settings fail closed rather than bypassing validation with prior setup patches")
check(delayed_bad.setup({idle_timeout=33}) == delayed_bad and deferred_bad.options.idle_timeout == 33 and
    deferred_bad.options.rendering.stars == 127 and not delayed_bad.status().config_file_error,
    "a deliberate valid setup can recover after a deferred file error")
delayed_bad.shutdown()

local delayed_conflict, deferred_conflict = mock("delayed", "return {controls={preview='SUPER+ALT+F12'}}")
delayed_conflict.setup({ controls = { emergency = "SUPER+ALT+F12" } })
deferred_conflict.fire("config.reloaded")
deferred_conflict.publish_native()
deferred_conflict.fire_timers()
check(deferred_conflict.setups == 0 and deferred_conflict.notifications == 1 and
    delayed_conflict.status().config_file_error:find("explicit setup conflicts", 1, true),
    "a control conflict between deferred setup and file values fails closed with an actionable error")
delayed_conflict.shutdown()

local early_controls, deferred_controls = mock("delayed", "return {controls={gravity=false}}")
check(early_controls.setup({controls={preview="F6"}}) == early_controls,
    "early setup may reuse a default chord disabled by the eventual user file")
check(not early_controls.setup({controls={preview="F9",emergency="F9"}}),
    "early setup still immediately rejects duplicate chords within its own patch")
check(not early_controls.setup({idle_timeout=0}) and not early_controls.setup({controls={preview=42}}),
    "deferred conflict checking does not relax field types or numeric ranges")
deferred_controls.fire("config.reloaded")
deferred_controls.publish_native()
deferred_controls.fire_timers()
check(deferred_controls.options.controls.gravity == false and deferred_controls.options.controls.preview == "F6" and
    deferred_controls.setups == 1 and deferred_controls.notifications == 0,
    "disabled file controls and reassigned early setup controls merge correctly at actual startup")
early_controls.shutdown()
local reload_controls, existing_controls = mock("success", "return {controls={gravity=false}}")
check(reload_controls.setup({controls={preview="F6"}}) == reload_controls and
    existing_controls.options.controls.gravity == false and existing_controls.options.controls.preview == "F6",
    "startup and reload accept the same file-dependent partial control override")
reload_controls.shutdown()

local obsolete, obsolete_state = mock("old-api", "return {idle_timeout=91}")
obsolete_state.fire("config.reloaded")
check(obsolete_state.config_opens == 0 and obsolete_state.setups == 0 and obsolete_state.live_bindings() == 0 and
    obsolete_state.notifications == 1 and obsolete.status().error:find("safe user settings reader", 1, true),
    "an obsolete native API cannot fall back to unsafe blocking Lua file I/O")
obsolete.shutdown()

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

local file_module, file_state = mock("success", [[
    return { idle_timeout = 91, rendering = { stars = 127 }, effects = { orbit = false } }
]])
check(file_module.status().config_file == root .. "/lua/hyprcosmos.lua" and
    file_state.config_path == file_module.status().config_file,
    "user settings resolve beside the module without following its symlink")
check(file_module.status().config_file_loaded and not file_module.status().config_file_error and
    file_module.status().config.idle_timeout == 91 and not file_module.status().config.effects.orbit,
    "valid user settings are visible before native plugin initialization")
check(file_module.status().config.rendering.stars == 127 and file_module.status().config.effects.binary,
    "nested user settings preserve unspecified defaults")
file_state.fire("config.reloaded")
check(file_state.options.idle_timeout == 91 and file_state.options.rendering.stars == 127 and
    file_state.setups == 1, "user settings reach native setup on the reload boundary")
file_state.user_config_source = "return {idle_timeout=37}"
check(require("cosmic") == file_module and file_state.config_opens == 1 and
    file_module.status().config.idle_timeout == 91,
    "repeated require does not reread a changed user file or duplicate initialization")
file_state.fire_timers()
check(file_state.config_opens == 1, "normal lifecycle timers do not poll the configuration file")
check(file_module.setup({ idle_timeout = 23 }) == file_module and file_state.options.idle_timeout == 23 and
    file_state.options.rendering.stars == 127 and not file_state.options.effects.orbit,
    "explicit setup overrides file values while preserving its other nested settings")
file_module.shutdown()

local reloaded_file, reloaded_state = mock("success", "return { idle_timeout=37, rendering={stars=211} }")
reloaded_state.fire("config.reloaded")
check(reloaded_state.options.idle_timeout == 37 and reloaded_state.options.rendering.stars == 211,
    "a fresh module lifecycle reads the edited user settings instead of cached prior values")
reloaded_file.shutdown()

local disabled_file, disabled_state = mock("success", "return { enabled=false, idle_timeout=92 }")
disabled_state.fire("config.reloaded")
check(disabled_file.status().module_initialized and not disabled_file.status().enabled and
    not disabled_file.status().initialized and disabled_state.live_bindings() == 0 and
    disabled_state.options.idle_timeout == 92,
    "enabled=false user settings are accepted without enabling input watchers or controls")
disabled_file.shutdown()

for _, source in ipairs({
    "return {", "error('test user settings failure')", "return nil", "return false", "return 42",
    "return {idle_timeout=0}", "return {unknown=true}", "return {controls={preview='F12'}}",
}) do
    local rejected_file, rejected_state = mock("success", source)
    rejected_state.fire("config.reloaded")
    rejected_state.fire("config.reloaded")
    local rejected_status = rejected_file.status()
    check(not rejected_status.config_file_loaded and rejected_status.config_file_error and
        rejected_status.error and rejected_status.error:find("hyprcosmos.lua", 1, true),
        "file errors report the precise user file without escaping into Hyprland configuration evaluation")
    check(rejected_state.setups == 0 and rejected_state.disable == 1 and
        not rejected_status.initialized and rejected_state.live_bindings() == 0 and
        rejected_state.notifications == 1 and #rejected_state.timers == 0,
        "invalid user settings stop native operation once without registering controls or retry timers")
    check(rejected_state.notification_text:find("Fix " .. rejected_state.config_path, 1, true) and
        not rejected_state.notification_text:find("Build/install", 1, true),
        "a file error recommends editing settings, not rebuilding the plugin")
    check(not rejected_file.enable() and rejected_state.enable == 0,
        "enable cannot silently bypass a rejected user settings file")
    check(not rejected_file.setup({idle_timeout=-1}) and rejected_file.status().config_file_error,
        "invalid explicit overrides keep file-error protection intact")
    check(rejected_file.setup({idle_timeout=33}) == rejected_file and rejected_state.setups == 1 and
        rejected_state.options.idle_timeout == 33 and not rejected_file.status().config_file_error and
        not rejected_file.status().error and rejected_state.notifications == 1,
        "a valid explicit setup can intentionally recover without duplicate warnings")
    rejected_file.shutdown()
    check(rejected_state.live_events() == 0 and rejected_state.live_bindings() == 0,
        "file error recovery retains complete lifecycle cleanup")
end

local missing_file, missing_state = mock("success")
missing_state.fire("config.reloaded")
check(not missing_file.status().config_file_loaded and not missing_file.status().config_file_error and
    missing_state.options.idle_timeout == 60 and missing_state.notifications == 0,
    "missing optional user settings preserve the one-minute default without a warning")
missing_file.shutdown()
io.open = original_open
_G.hl = nil
print("Lua configuration/lifecycle: " .. checks .. " checks passed")
