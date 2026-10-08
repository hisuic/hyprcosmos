-- Hyprland 0.56.2 loads configured plugins after evaluating hyprland.lua.
-- Native initialization therefore runs on config.reloaded, once per Lua state.
local Config = require("cosmic.config")
local M = {}
local options = Config.copy(Config.defaults)
local initialized, stopped, warned, configuration_ready = false, false, false, false
local last_error, plugin_path
local subscriptions, bindings = {}, {}

local function native()
    return type(hl) == "table" and type(hl.plugin) == "table" and hl.plugin.cosmic or nil
end

local function warn(message)
    last_error = tostring(message)
    if warned then return end
    warned = true
    local text = "Cosmic stopped safely: " .. last_error ..
        ". Build/install with hyprcosmos/scripts/install.sh, then reload Hyprland."
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
            local ok, binding = pcall(hl.bind, chord, function() M.action(action) end,
                { description = "Cosmic: " .. action, submap_universal = true })
            if not ok then
                remove_bindings()
                return nil, "could not register control '" .. chord .. "': " .. tostring(binding)
            end
            bindings[#bindings + 1] = binding
        end
    end
    return true
end

local function resolve_plugin()
    local module_file = package.searchpath and package.searchpath("cosmic", package.path)
    if not module_file and debug and debug.getinfo then
        local source = debug.getinfo(1, "S").source
        if source:sub(1, 1) == "@" then module_file = source:sub(2) end
    end
    if module_file then
        local directory = module_file:match("^(.*)/init%.lua$")
        -- Remove the final component lexically. Using cosmic/../ would follow
        -- the module symlink first and incorrectly resolve inside the checkout.
        if directory then return (directory:match("^(.*)/[^/]+$") or ".") .. "/cosmic.so" end
    end
    local config_home = os.getenv("XDG_CONFIG_HOME") or ((os.getenv("HOME") or "") .. "/.config")
    return config_home .. "/hypr/cosmic.so"
end

local function initialize()
    if initialized or stopped then return initialized end
    local ok, message = call("setup", Config.copy(options))
    if not ok then warn(message); return nil, message end
    ok, message = bind_controls()
    if not ok then call("shutdown"); warn(message); return nil, message end
    initialized = true
    last_error = nil
    return true
end

local function attach()
    if type(hl) ~= "table" or type(hl.on) ~= "function" or not hl.plugin then
        warn("this module requires the Hyprland Lua configuration API")
        return false
    end
    subscriptions[#subscriptions + 1] = hl.on("config.reloaded", function()
        configuration_ready = true
        initialize()
    end)
    subscriptions[#subscriptions + 1] = hl.on("hyprland.shutdown", function() M.shutdown() end)
    plugin_path = resolve_plugin()
    local file = io.open(plugin_path, "rb")
    if not file then
        warn("plugin file is missing: " .. plugin_path)
        return false
    end
    file:close()
    local ok, message = pcall(hl.plugin.load, plugin_path)
    if not ok then warn(message); return false end
    return true
end

function M.setup(update)
    local validated, message = Config.normalize(update, options)
    if not validated then return nil, message end
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
        if not initialized and configuration_ready and native() then
            local ok
            ok, message = initialize()
            if not ok then return nil, message end
        end
    end
    return M
end

function M.enable()
    if stopped then return nil, "cosmic: call setup() to restart after shutdown()" end
    options.enabled = true
    if initialized then
        local ok, message = call("enable")
        if not ok then warn(message); return nil, message end
        return bind_controls()
    end
    return true
end

function M.disable()
    options.enabled = false
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
    if result.initialized == nil then result.initialized = false end
    result.error = last_error
    result.config = Config.copy(options)
    result.plugin_path = plugin_path
    return result
end

function M.shutdown()
    if stopped then return true end
    stopped = true
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

attach()
return M
