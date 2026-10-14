-- Pure default card formatting: no timers, file I/O, compositor calls, titles,
-- or persistent state. The engine owns provenance, including its rewind state.
local M = {}
local kinds = {
    WINDOW = true, FRAGMENT = true, REMNANT = true, CHARGING = true,
    TRANSIT = true, SINKING = true,
}

local function integer(value, maximum)
    if type(value) ~= "number" or value ~= value or value == math.huge or value == -math.huge then return 0 end
    return math.floor(math.min(maximum or 1e12, math.max(0, value)))
end

local function ascii(value, fallback)
    if type(value) ~= "string" then return fallback or "UNKNOWN" end
    -- Bound work before sanitizing possibly long, non-ASCII application IDs.
    local text = value:sub(1, 256):gsub("[^ -~]", "?"):gsub(" +", " "):
        gsub("^ +", ""):gsub(" +$", "")
    return text ~= "" and text or (fallback or "UNKNOWN")
end

local function identifier(value)
    if type(value) == "number" and value == value and value ~= math.huge and value ~= -math.huge then
        -- Native opaque IDs use Lua integers; preserve their exact 64-bit text.
        if math.type and math.type(value) == "integer" then return tostring(value) end
        return string.format("%.0f", value)
    end
    return ascii(value, "?")
end

function M.format(context)
    if type(context) ~= "table" then context = {} end
    local source = identifier(context.source_id or context.id)
    local kind = kinds[context.kind] and context.kind or "WINDOW"
    local grid = integer(context.fragment_grid, 4)
    if grid ~= 2 and grid ~= 4 then grid = 0 end
    local name, detail = "WINDOW #" .. source, kind
    if grid > 0 then
        local row = integer(context.fragment_row, grid - 1) + 1
        local column = integer(context.fragment_column, grid - 1) + 1
        name = "SHARD " .. row .. "," .. column .. " OF #" .. source
        detail = "FRAGMENT " .. row .. "," .. column .. " / " .. grid .. "x" .. grid
        if kind ~= "FRAGMENT" then detail = detail .. " / " .. kind end
    elseif kind == "REMNANT" then
        name = "REMNANT #" .. source
    end
    local event = integer(context.nova_event)
    local lines = {
        "NAME: " .. name,
        "ORIGIN: " .. ascii(context.origin):upper(),
        "TYPE: " .. detail,
        "SUPERNOVA: " .. (event > 0 and ("#" .. event) or "NONE"),
        "WORMHOLE TRANSITS: " .. integer(context.wormhole_transits),
        "AGE: " .. integer(context.age_seconds) .. "s",
    }
    if context.rewinding == true then lines[#lines + 1] = "TIMELINE: REWIND" end
    for index, line in ipairs(lines) do lines[index] = line:sub(1, 52) end
    return lines
end

return M
