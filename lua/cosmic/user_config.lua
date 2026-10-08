-- Trusted user configuration is read once by cosmic's public entry point.
-- Keep file contents local: callers receive validated options and diagnostics,
-- never source text or an open file handle.
local Config = require("cosmic.config")
local M = {}
local max_bytes = 1024 * 1024
local protected_call, compile, value_type, stringify = pcall, load, type, tostring

local function diagnostic(path, stage, reason)
    local ok, text = protected_call(stringify, reason)
    return "cosmic: user config " .. path .. ": " .. stage .. ": " ..
        (ok and text or "diagnostic unavailable")
end

local function normalize(options, path, found)
    local ok, result, message = protected_call(Config.normalize, options)
    if not ok then return nil, diagnostic(path, "validation failed", result), found end
    if not result then return nil, diagnostic(path, "invalid configuration", message), found end
    return result, nil, found
end

function M.load(path)
    if value_type(path) ~= "string" or path == "" or path:find("\0", 1, true) then
        return nil, diagnostic(value_type(path) == "string" and path or "<invalid path>",
            "invalid path", "expected a nonempty filename without NUL bytes"), true
    end

    local opened, file, open_error, open_code = protected_call(function() return io.open(path, "rb") end)
    if not opened then return nil, diagnostic(path, "open failed", file), true end
    if not file then
        -- Only ENOENT is absence. Permission errors and unexpected I/O failures
        -- must not silently enable the default universe instead of user options.
        if open_code == 2 then return normalize(nil, path, false) end
        return nil, diagnostic(path, "open failed", open_error or "no file handle returned"), true
    end

    local read_ok, source, read_error = protected_call(function() return file:read(max_bytes + 1) end)
    -- Close even if read raises or reports an error. A failed close also rejects
    -- the configuration, and must not escape into compositor initialization.
    local close_ok, closed, close_error = protected_call(function() return file:close() end)
    if not read_ok then return nil, diagnostic(path, "read failed", source), true end
    if not source and read_error then return nil, diagnostic(path, "read failed", read_error), true end
    if not close_ok then return nil, diagnostic(path, "close failed", closed), true end
    if not closed then return nil, diagnostic(path, "close failed", close_error or "file was not closed"), true end
    if source == nil then source = "" end -- An empty regular file reaches EOF.
    if value_type(source) ~= "string" then
        return nil, diagnostic(path, "read failed", "expected text bytes"), true
    end
    if #source > max_bytes then
        return nil, diagnostic(path, "file too large", "maximum size is " .. max_bytes .. " bytes"), true
    end

    -- Text-only loading rejects binary bytecode. This is an ordinary trusted
    -- Lua file, so standard globals remain available instead of a fake sandbox.
    local load_ok, chunk, syntax_error = protected_call(compile, source, "@" .. path, "t", _G)
    if not load_ok then return nil, diagnostic(path, "compile failed", chunk), true end
    if not chunk then return nil, diagnostic(path, "syntax error", syntax_error), true end
    local ran, options = protected_call(chunk)
    if not ran then return nil, diagnostic(path, "execution failed", options), true end
    if value_type(options) ~= "table" then
        return nil, diagnostic(path, "invalid return value", "expected an options table"), true
    end
    return normalize(options, path, true)
end

return M
