-- The public configuration is validated before touching compositor state.
local M = {}

M.defaults = {
    enabled = true,
    idle_timeout = 5,
    preset = "calm",
    seed = 0xC05C1C,
    max_windows = 24,
    fps = 60,
    snapshot_hz = 4,
    history_seconds = 12,
    history_hz = 30,
    history_mb = 16,
    plugin_path = false,
    effects = {
        cursor_gravity = true, orbit = true, binary = true, collisions = true,
        black_hole = true, spaghetti = true, wormholes = true, supernova = true,
        expansion = true, rewind = true,
    },
    physics = {
        fixed_step = 1 / 120, max_substeps = 8,
        cursor_strength = 1400000, mutual_strength = 14000, softening = 100,
        max_acceleration = 1400, max_speed = 950, damping = 0.012,
        restitution = 0.68, collision_strength = 0.35, expansion_rate = 0.006,
        sink_duration = 2.8, wormhole_cooldown = 2, explosion_strength = 320,
    },
    exclusions = {
        fullscreen = true, idle_inhibit = true, screenshare = true,
        classes = { "^steam_app_", "^steam$", "^gamescope$", "^mpv$" },
    },
    rendering = { particles = 96, stars = 120, background = 0.16 },
    -- Dedicated keys avoid modifier preambles: ordinary modifier presses must
    -- restore immediately, including modifiers used by input methods.
    controls = {
        gravity = "F6", black_hole = "F7", rewind = "F8", supernova = "F9",
        region = "F10", preview = "F11", emergency = "F12",
    },
}

local ranges = {
    idle_timeout = { 0.1, 86400 }, seed = { 0, 0xFFFFFFFF, true },
    max_windows = { 1, 64, true }, fps = { 10, 120, true },
    snapshot_hz = { 0.1, 30 }, history_seconds = { 0.1, 120 },
    history_hz = { 1, 120 }, history_mb = { 1, 128 },
    ["physics.fixed_step"] = { 1 / 1000, 1 / 20 },
    ["physics.max_substeps"] = { 1, 32, true },
    ["physics.cursor_strength"] = { 0, 100000000 },
    ["physics.mutual_strength"] = { 0, 100000000 },
    ["physics.softening"] = { 1, 10000 },
    ["physics.max_acceleration"] = { 1, 100000 },
    ["physics.max_speed"] = { 1, 10000 }, ["physics.damping"] = { 0, 100 },
    ["physics.restitution"] = { 0, 1 }, ["physics.collision_strength"] = { 0, 1 },
    ["physics.expansion_rate"] = { 0, 0.2 }, ["physics.sink_duration"] = { 0.1, 60 },
    ["physics.wormhole_cooldown"] = { 0.1, 60 },
    ["physics.explosion_strength"] = { 0, 10000 },
    ["rendering.particles"] = { 0, 1024, true },
    ["rendering.stars"] = { 0, 1024, true }, ["rendering.background"] = { 0, 1 },
}

function M.copy(value)
    if type(value) ~= "table" then return value end
    local result = {}
    for key, item in pairs(value) do result[key] = M.copy(item) end
    return result
end

local function fail(path, explanation)
    error("cosmic: " .. path .. " " .. explanation, 0)
end

local function validate(value, expected, path)
    if path == "plugin_path" then
        if value == false then return end
        if type(value) ~= "string" or value:sub(1, 1) ~= "/" or value:find("\0", 1, true) then
            fail(path, "must be false or an absolute plugin path")
        end
        return
    end
    if path:match("^controls%.") then
        if value ~= false and (type(value) ~= "string" or not value:find("%S") or value:find("\0", 1, true)) then
            fail(path, "must be a key chord or false")
        end
        return
    end
    if path == "preset" then
        if value ~= "calm" and value ~= "demo" then fail(path, "must be 'calm' or 'demo'") end
        return
    end
    if type(value) ~= type(expected) then fail(path, "must be a " .. type(expected)) end
    local range = ranges[path]
    if range and (value ~= value or value == math.huge or value == -math.huge or
        value < range[1] or value > range[2] or (range[3] and value % 1 ~= 0)) then
        fail(path, "must be " .. (range[3] and "an integer" or "a finite number") ..
            " between " .. range[1] .. " and " .. range[2])
    end
end

local function merge(target, patch, shape, prefix)
    if type(patch) ~= "table" then fail(prefix ~= "" and prefix or "setup options", "must be a table") end
    for key, value in pairs(patch) do
        if type(key) ~= "string" or shape[key] == nil then fail(prefix .. tostring(key), "is not a configuration option") end
        local path = prefix .. key
        if path == "exclusions.classes" then
            if type(value) ~= "table" then fail(path, "must be an array of class regular expressions") end
            local count = 0
            for index, pattern in pairs(value) do
                count = count + 1
                if type(index) ~= "number" or index % 1 ~= 0 or index < 1 or
                    type(pattern) ~= "string" or pattern == "" or pattern:find("\0", 1, true) then
                    fail(path, "must be an array of nonempty class regular expressions")
                end
            end
            if count > 128 or count ~= #value then fail(path, "must be a dense array with at most 128 entries") end
            target[key] = M.copy(value)
        elseif type(shape[key]) == "table" then
            merge(target[key], value, shape[key], path .. ".")
        else
            validate(value, shape[key], path)
            target[key] = value
        end
    end
end

function M.normalize(options, previous)
    local result = M.copy(previous or M.defaults)
    local ok, message = pcall(merge, result, options or {}, M.defaults, "")
    if not ok then return nil, message end
    local used = {}
    for action, chord in pairs(result.controls) do
        if chord then
            local canonical = chord:upper():gsub("%s", "")
            if used[canonical] then
                return nil, "cosmic: controls." .. action .. " duplicates controls." .. used[canonical]
            end
            used[canonical] = action
        end
    end
    return result
end

function M.preset(name)
    if name ~= "calm" and name ~= "demo" then return nil, "cosmic: unknown preset '" .. tostring(name) .. "'" end
    local result = M.copy(M.defaults)
    result.preset = name
    if name == "demo" then
        result.physics.mutual_strength = 42000
        result.physics.collision_strength = 0.8
        result.physics.expansion_rate = 0.014
        result.physics.explosion_strength = 640
        result.rendering.particles = 384
    end
    return result
end

return M
