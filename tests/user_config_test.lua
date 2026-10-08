-- Isolated files and mocked I/O only; no compositor or user configuration is
-- opened. The loader must preserve normal Lua globals and fail closed on errors.
local root = arg[1] or "."
package.path = root .. "/lua/?.lua;" .. root .. "/lua/?/init.lua;" .. package.path
local Config = require("cosmic.config")
local original_open = io.open
local checks, fixtures = 0, {}
local function check(condition, message)
    checks = checks + 1
    assert(condition, message)
end

local opens = 0
io.open = function() opens = opens + 1; error("require must not open user configuration") end
local imported, UserConfig = pcall(require, "cosmic.user_config")
io.open = original_open
check(imported and type(UserConfig.load) == "function" and opens == 0,
    "requiring the loader does not read a user configuration")

local function fixture(source)
    local path = os.tmpname()
    fixtures[#fixtures + 1] = path
    local file = assert(original_open(path, "wb"))
    assert(file:write(source))
    assert(file:close())
    return path
end
local function rejected(path, expected_stage)
    local options, message, found = UserConfig.load(path)
    check(options == nil and type(message) == "string" and found == true,
        "a malformed existing configuration is rejected without default fallback")
    check(message:find(path, 1, true) ~= nil, "configuration errors identify their exact file path")
    if expected_stage then
        check(message:find(expected_stage, 1, true) ~= nil, "configuration errors identify the failing operation")
    end
end
local function with_open(opener, run)
    io.open = opener
    local ok, message = pcall(run)
    io.open = original_open
    assert(ok, message)
end

local function run()
    local missing = fixture("")
    assert(os.remove(missing))
    local defaults, message, found = UserConfig.load(missing)
    check(defaults and defaults.idle_timeout == Config.defaults.idle_timeout and message == nil and found == false,
        "a missing configuration returns normalized defaults without an error")
    defaults.effects.orbit = false
    check(Config.defaults.effects.orbit == true, "missing-file defaults are independent configuration copies")
    for _, delay in ipairs({20, 60}) do
        local path = fixture("return { idle_timeout = " .. delay .. " }")
        local options, error_message, present = UserConfig.load(path)
        check(options and options.idle_timeout == delay and error_message == nil and present == true,
            "a real configuration accepts an explicit twenty- or sixty-second delay")
        check(options.rendering.hide_desktop_ui and options.effects.orbit,
            "partial user configuration preserves the validated public defaults")
    end
    local path = fixture([[assert(_G == _ENV and type(math.sqrt) == "function")
return { idle_timeout = math.floor(20.9), rendering = { stars = 7, hide_desktop_ui = false },
         effects = { orbit = false }, controls = { preview = false } }]])
    local custom, custom_error, custom_found = UserConfig.load(path)
    check(custom and custom.idle_timeout == 20 and custom_error == nil and custom_found,
        "trusted user configuration executes with the ordinary global environment")
    check(custom.rendering.stars == 7 and not custom.rendering.hide_desktop_ui and
        not custom.effects.orbit and not custom.controls.preview and custom.effects.binary,
        "nested user configuration is fully normalized rather than returned as a raw patch")

    rejected(fixture("return { idle_timeout = "), "syntax error")
    rejected(fixture("error('fixture execution error')"), "execution failed")
    for _, source in ipairs({ "", "return nil", "return 20", "return false", "return 'text'", "return function() end" }) do
        rejected(fixture(source), "invalid return value")
    end
    for _, source in ipairs({
        "return { unknown = true }", "return { idle_timeout = 0 }",
        "return { idle_timeout = math.huge }", "return { idle_timeout = 0/0 }",
        "return { rendering = { hide_desktop_ui = 'true' } }",
        "return { controls = { preview = 'F6' } }",
        "return { controls = { preview = 'SUPER+ALT+C', emergency = 'ALT+SUPER+C' } }",
        "return { exclusions = { classes = {[2] = 'game'} } }",
    }) do rejected(fixture(source), "invalid configuration") end
    rejected(fixture("return setmetatable({}, {__pairs = function() error('fixture validation exception') end})"),
        "invalid configuration")
    rejected(fixture(string.dump(function() return { idle_timeout = 20 } end)), "syntax error")
    rejected(".", "read failed") -- A directory is not an empty user config.

    local limit = 1024 * 1024
    local prefix = "return { idle_timeout = 20 } --"
    local at_limit = fixture(prefix .. string.rep("x", limit - #prefix))
    local large, large_error, large_found = UserConfig.load(at_limit)
    check(large and large.idle_timeout == 20 and large_error == nil and large_found,
        "a text configuration exactly one MiB long is accepted")
    rejected(fixture(prefix .. string.rep("x", limit + 1 - #prefix)), "file too large")

    local mock_path = "/isolated-fixture/cosmic.lua"
    with_open(function(given, mode)
        check(given == mock_path and mode == "rb", "user files are opened read-only in binary byte mode")
        return nil, "localized missing-file diagnostic", 2
    end, function()
        local options, error_message, present = UserConfig.load(mock_path)
        check(options and error_message == nil and present == false,
            "ENOENT uses its numeric code rather than parsing localized diagnostic text")
    end)
    for _, failure in ipairs({
        function() return nil, "permission denied", 13 end,
        function() return nil, "unexpected open failure" end,
        function() error("fixture open exception") end,
    }) do with_open(failure, function() rejected(mock_path, "open failed") end) end

    for _, reader in ipairs({
        function() return nil, "fixture read error", 5 end,
        function() error("fixture read exception") end,
        function() return false end,
    }) do
        local closed = 0
        with_open(function()
            return { read = reader, close = function() closed = closed + 1; return true end }
        end, function() rejected(mock_path, "read failed") end)
        check(closed == 1, "read errors and exceptions still close the opened file exactly once")
    end
    for _, closer in ipairs({
        function() return nil, "fixture close error" end,
        function() error("fixture close exception") end,
    }) do
        local reads, closes = 0, 0
        with_open(function()
            return {
                read = function(_, count)
                    reads = reads + 1
                    check(count == limit + 1, "file reading has a hard one-MiB-plus-one-byte bound")
                    return "return { idle_timeout = 20 }"
                end,
                close = function() closes = closes + 1; return closer() end,
            }
        end, function() rejected(mock_path, "close failed") end)
        check(reads == 1 and closes == 1, "close failures are contained without repeated I/O")
    end
    local closed = 0
    with_open(function()
        return { read = function() return nil end, close = function() closed = closed + 1; return true end }
    end, function() rejected(mock_path, "invalid return value") end)
    check(closed == 1, "empty EOF is handled as an empty file and releases its handle")

    local normalize = Config.normalize
    Config.normalize = function() error("fixture unexpected normalization exception") end
    local ok, normalization_error = pcall(function() rejected(path, "validation failed") end)
    Config.normalize = normalize
    assert(ok, normalization_error)
    for _, invalid_path in ipairs({false, 23, {}, ""}) do
        local options, error_message, present = UserConfig.load(invalid_path)
        check(options == nil and type(error_message) == "string" and present == true,
            "invalid loader paths fail closed before opening files")
    end
    local options, error_message, present = UserConfig.load("invalid\0filename")
    check(options == nil and type(error_message) == "string" and present == true,
        "NUL-containing filenames fail closed before opening files")
end

local ok, message = pcall(run)
io.open = original_open
for _, path in ipairs(fixtures) do os.remove(path) end
assert(ok, message)
print("User configuration loading: " .. checks .. " checks passed")
