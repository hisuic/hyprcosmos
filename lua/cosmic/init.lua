-- Hyprland 0.56.2 loads configured plugins after evaluating hyprland.lua.
-- Native initialization runs after configuration/plugin loading. At compositor
-- startup config.reloaded may precede construction of the plugin manager.
local Config = require("cosmic.config")
local UserConfig = require("cosmic.user_config")
local M = {}
local options = Config.copy(Config.defaults)
local initialized, stopped, warned, configuration_ready = false, false, false, false
local last_error, plugin_path
local config_directory, config_file, config_file_loaded, config_file_error
local config_read, pending_updates = false, {}
local subscriptions, bindings = {}, {}
local initialization_timer, initialization_generation = nil, 0
local initialization_attempts, plugin_requested = 0, false

local function native()
    return type(hl) == "table" and type(hl.plugin) == "table" and hl.plugin.cosmic or nil
end

local function native_ready()
    local api = native()
    return api and type(api.setup) == "function"
end

local function cancel_initialization()
    initialization_generation = initialization_generation + 1
    if initialization_timer then
        -- HL.Timer has no remove method in 0.56.2. Draining a canceled oneshot
        -- releases its Lua registry reference; disabling it would retain that
        -- reference until the next config reload. The generation guard below
        -- prevents any canceled initialization from running.
        pcall(function() initialization_timer:set_timeout(1) end)
        initialization_timer = nil
    end
end

local function warn(message)
    last_error = tostring(message)
    if warned then return end
    warned = true
    local hint = config_file_error and (". Fix " .. config_file .. ", then reload Hyprland.") or
        ". Build/install with hyprcosmos/scripts/install.sh, then reload Hyprland."
    local text = "Cosmic stopped safely: " .. last_error .. hint
    if type(hl) == "table" and hl.notification and hl.notification.create then
        pcall(hl.notification.create, { text = text, timeout = 10000, color = "rgb(f6c177)", icon = "warning" })
    else
        print(text)
    end
end

local function call(method, ...)
    local api = native()
    if not api or type(api[method]) ~= "function" then
        return nil, "native plugin is unavailable (or has an incompatible Lua API)"
    end
    local ok, result, message = pcall(api[method], ...)
    if not ok then return nil, tostring(result) end
    if result == false or (result == nil and message ~= nil) then
        return nil, message or ("native " .. method .. " failed")
    end
    return result == nil and true or result, message
end

local function remove_bindings()
    for _, binding in ipairs(bindings) do pcall(function() binding:remove() end) end
    bindings = {}
end

local function bind_controls()
    remove_bindings()
    if not options.enabled then return true end
    for action, chord in pairs(options.controls) do
        if chord then
            local ok, binding, bind_error = pcall(hl.bind, chord, function() M.action(action) end,
                { description = "Cosmic: " .. action, submap_universal = true })
            if not ok or binding == nil then
                remove_bindings()
                return nil, "could not register control '" .. chord .. "': " .. tostring(bind_error or binding)
            end
            bindings[#bindings + 1] = binding
        end
    end
    return true
end

local function resolve_config_directory()
    local module_file = package.searchpath and package.searchpath("cosmic", package.path)
    if not module_file and debug and debug.getinfo then
        local source = debug.getinfo(1, "S").source
        if source:sub(1, 1) == "@" then module_file = source:sub(2) end
    end
    if module_file then
        local directory = module_file:match("^(.*)/init%.lua$")
        -- Remove the final component lexically. Using cosmic/../ would follow
        -- the module symlink first and incorrectly resolve inside the checkout.
        if directory then return directory:match("^(.*)/[^/]+$") or "." end
    end
    local config_home = os.getenv("XDG_CONFIG_HOME") or ((os.getenv("HOME") or "") .. "/.config")
    return config_home .. "/hypr"
end

local function load_config_file()
    if config_read then return not config_file_error, config_file_error end
    local api = native()
    if not api or type(api.read_user_config) ~= "function" then
        if api then call("disable") end
        return nil, "native plugin is unavailable or lacks the safe user settings reader"
    end
    config_read = true
    local loaded, message, found = UserConfig.load(config_file, api.read_user_config)
    config_file_loaded = found and loaded ~= nil
    if loaded then
        -- Startup evaluates hyprland.lua before the native plugin exists. Replay
        -- explicit setup patches only after reading the file so their priority
        -- is identical to setup() during an ordinary configuration reload.
        for _, update in ipairs(pending_updates) do
            loaded, message = Config.normalize(update, loaded)
            if not loaded then
                message = config_file .. ": explicit setup conflicts with user settings: " .. message
                break
            end
        end
    end
    pending_updates = {}
    if not loaded then
        config_file_error = message
        -- A retained plugin can still own input/render hooks after a reload.
        -- Invalid settings must stop it, not leave the old universe running.
        call("disable")
        warn(message)
        return nil, message
    end
    options = loaded
    return true
end

local function initialize()
    if config_file_error then return nil, config_file_error end
    if initialized or stopped then return initialized end
    local ok, message = load_config_file()
    if not ok then warn(message); return nil, message end
    cancel_initialization()
    ok, message = call("setup", Config.copy(options))
    if not ok then warn(message); return nil, message end
    ok, message = bind_controls()
    if not ok then call("shutdown"); warn(message); return nil, message end
    initialized = true
    last_error = nil
    return true
end

local function await_plugin()
    if stopped or initialized or config_file_error then return end
    if native_ready() then
        initialize()
        return
    end
    if not plugin_requested or initialization_timer then return end
    if initialization_attempts >= 50 then
        warn("native plugin did not become available within 5 seconds; check plugin permissions and matching Hyprland headers")
        return
    end
    local generation = initialization_generation
    initialization_timer = hl.timer(function()
        if generation ~= initialization_generation or stopped then return end
        initialization_timer = nil
        initialization_attempts = initialization_attempts + 1
        await_plugin()
    end, { timeout = 100, type = "oneshot" })
end

local function attach()
    initialization_attempts = 0
    plugin_requested = false
    if type(hl) ~= "table" or type(hl.on) ~= "function" or not hl.plugin then
        warn("this module requires the Hyprland Lua configuration API")
        return false
    end
    subscriptions[#subscriptions + 1] = hl.on("config.reloaded", function()
        configuration_ready = true
        await_plugin()
    end)
    subscriptions[#subscriptions + 1] = hl.on("hyprland.shutdown", function() M.shutdown() end)
    plugin_path = config_directory .. "/cosmic.so"
    local file = io.open(plugin_path, "rb")
    if not file then
        warn("plugin file is missing: " .. plugin_path)
        return false
    end
    file:close()
    local ok, message = pcall(hl.plugin.load, plugin_path)
    if not ok then warn(message); return false end
    plugin_requested = true
    return true
end

function M.setup(update)
    local validated, message = Config.normalize(update, options)
    if not validated then return nil, message end
    if not config_read then pending_updates[#pending_updates + 1] = Config.copy(update or {}) end
    -- A valid explicit setup can recover from a file error. It remains the
    -- highest-priority override; reloading re-reads the file from defaults.
    config_file_error = nil
    if initialized then
        local ok
        ok, message = call("setup", Config.copy(validated))
        if not ok then
            remove_bindings()
            initialized = false
            warn(message)
            return nil, message
        end
        options = validated
        ok, message = bind_controls()
        if not ok then call("disable"); warn(message); return nil, message end
    else
        options = validated
        if stopped then
            stopped = false
            if attach() and native() then
                local ok
                ok, message = initialize()
                if not ok then return nil, message end
            end
        end
        if not initialized and native_ready() then
            local ok
            ok, message = initialize()
            if not ok then return nil, message end
        elseif configuration_ready then
            await_plugin()
        end
    end
    return M
end

function M.enable()
    if stopped then return nil, "cosmic: call setup() to restart after shutdown()" end
    if config_file_error then return nil, config_file_error end
    options.enabled = true
    if not config_read then pending_updates[#pending_updates + 1] = { enabled = true } end
    if initialized then
        local ok, message = call("enable")
        if not ok then warn(message); return nil, message end
        return bind_controls()
    end
    if native_ready() then return initialize() end
    if not plugin_requested then
        return nil, last_error or "cosmic: native plugin is unavailable; install it and reload Hyprland"
    end
    -- An explicit enable retries a previous availability deadline. Repeated
    -- calls while a probe is already pending keep the same bounded attempt.
    if not initialization_timer then initialization_attempts = 0 end
    await_plugin()
    return true
end

function M.disable()
    options.enabled = false
    if not config_read then pending_updates[#pending_updates + 1] = { enabled = false } end
    remove_bindings()
    if initialized then return call("disable") end
    return true
end

function M.action(action)
    if options.controls[action] == nil then return nil, "cosmic: unknown action '" .. tostring(action) .. "'" end
    if not initialized or stopped then return nil, "cosmic: native plugin is not initialized" end
    return call("action", action)
end

function M.status()
    local result = native() and call("status") or nil
    if type(result) ~= "table" then result = { enabled = false, active = false } end
    result.available = native() ~= nil
    result.module_initialized = initialized and not stopped
    result.initialization_pending = initialization_timer ~= nil
    if result.initialized == nil then result.initialized = false end
    result.error = last_error
    result.config = Config.copy(options)
    result.plugin_path = plugin_path
    result.config_file = config_file
    result.config_file_loaded = config_file_loaded
    result.config_file_error = config_file_error
    return result
end

function M.shutdown()
    if stopped then return true end
    stopped = true
    cancel_initialization()
    remove_bindings()
    for _, subscription in ipairs(subscriptions) do pcall(function() subscription:remove() end) end
    subscriptions = {}
    if initialized then call("shutdown") end
    initialized = false
    return true
end

function M.preset(name)
    return Config.preset(name)
end

config_directory = resolve_config_directory()
config_file = config_directory .. "/hyprcosmos.lua"
config_file_loaded = false
-- Never use blocking Lua file I/O for user settings on the compositor thread.
-- On a reload the API may already exist; on startup initialize() reads the
-- file once after the native plugin's bounded regular-file reader is ready.
if native() and type(native().read_user_config) == "function" then load_config_file() end
attach()
return M
