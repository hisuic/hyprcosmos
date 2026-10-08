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

local previous = Config.normalize({ idle_timeout = 8, effects = { orbit = false } })
check(previous.idle_timeout == 8 and previous.effects.binary and not previous.effects.orbit, "nested partial setup preserves defaults")
for _, invalid in ipairs({ false, "invalid", { idle_timeout = 0 }, { max_windows = 49 },
    { physics = { fixed_step = 0 } }, { physics = { max_speed = math.huge } },
    { rendering = { particles = -1 } }, { unknown = true }, { effects = { orbit = 1 } },
    { controls = { preview = "F6" } }, { exclusions = { classes = { [2] = "game" } } },
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

local original_open = io.open
local function mock(mode)
    local state = { events = {}, bindings = {}, setups = 0, notifications = 0, loads = {},
        enable = 0, disable = 0, shutdown = 0, actions = {}, initialized = false, enabled = false }
    io.open = function(path)
        if mode == "missing" then return nil, "missing test plugin" end
        return { close = function() end }
    end
    local api = {
        setup = function(options)
            state.setups = state.setups + 1
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
        status = function() return { enabled = state.enabled, initialized = state.initialized, active = false } end,
    }
    _G.hl = {
        plugin = { load = function(path) state.loads[#state.loads + 1] = path end,
            cosmic = mode ~= "missing" and api or nil },
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
check(cosmic.disable() and not cosmic.status().initialized and cosmic.status().module_initialized, "disable reports native watcher teardown")
check(cosmic.enable() and cosmic.status().initialized, "enable restores native watcher state")
check(cosmic.action("black_hole") and state.actions[1] == "black_hole", "dedicated actions reach native API")
check(not cosmic.action("invalid"), "unknown actions are rejected")
check(cosmic.shutdown() and cosmic.shutdown(), "shutdown is repeatable")
check(state.shutdown == 1 and state.live_bindings() == 0 and state.live_events() == 0, "shutdown releases all Lua ownership once")
check(not cosmic.enable(), "enable after full shutdown requires setup")
check(cosmic.setup({ enabled = false }) == cosmic and state.setups == 3 and state.live_bindings() == 6, "setup restarts ownership after shutdown")
state.fire("hyprland.shutdown")
check(state.live_bindings() == 0 and state.live_events() == 0, "compositor shutdown cleans module ownership")

for _, mode in ipairs({ "nil-error", "throw", "missing" }) do
    local failed, failure = mock(mode)
    failure.fire("config.reloaded")
    failure.fire("config.reloaded")
    check(not failed.status().initialized and failure.live_bindings() == 0, "failed initialization never registers consuming controls")
    check(failure.notifications == 1 and failed.status().error, "failure is explained once per module lifecycle")
    failed.shutdown()
    check(failure.live_events() == 0, "failed module can be shut down completely")
end
io.open = original_open
_G.hl = nil
print("Lua configuration/lifecycle: " .. checks .. " checks passed")
