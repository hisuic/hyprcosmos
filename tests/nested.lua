-- Dedicated test session. No user configuration, services or application binds.
-- Limit this first-launch permission to the isolated screenshot executable.
-- Without a rule the permission path can delay images by more than a second.
hl.permission({ binary = "^/usr/bin/grim$", type = "screencopy", mode = "allow" })
hl.permission({ binary = "^/usr/bin/wf-recorder$", type = "screencopy", mode = "allow" })
hl.monitor({ output = "", mode = "1280x720@60", position = "0x0", scale = 1 })
hl.config({
    general = { gaps_in = 8, gaps_out = 18, border_size = 2, layout = "dwindle" },
    decoration = { rounding = 14, shadow = { enabled = true } },
    animations = { enabled = false },
    input = { follow_mouse = 1 },
    misc = { disable_hyprland_logo = true, force_default_wallpaper = 0,
             enable_anr_dialog = false },
    debug = { disable_logs = false },
})
require("cosmic").setup({
    enabled = false,
    idle_timeout = 0.8,
    fps = 30,
    snapshot_hz = 8,
    -- grim is itself a real screenshare client. Test rendering with this one
    -- exclusion disabled; production defaults keep sharing excluded.
    exclusions = { screenshare = false },
    rendering = { background = 0.95, stars = 180 },
})
