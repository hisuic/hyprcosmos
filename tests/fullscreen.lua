-- Isolated desktop-UI regression session, never sourced by the live config.
hl.permission({ binary = "^/usr/bin/grim$", type = "screencopy", mode = "allow" })
hl.monitor({ output = "", mode = "1280x720@60", position = "0x0", scale = 1 })
hl.config({
    general = { gaps_in = 8, gaps_out = 18, border_size = 2, layout = "dwindle" },
    decoration = { rounding = 0, shadow = { enabled = false } },
    animations = { enabled = false },
    input = { follow_mouse = 1 },
    misc = { disable_hyprland_logo = true, force_default_wallpaper = 0,
             background_color = "rgb(682345)", enable_anr_dialog = false,
             -- Lifecycle checks explicitly choose their reload boundary. An
             -- inotify reload during config generation would test a race in
             -- the harness instead of an active direct plugin unload.
             disable_autoreload = true },
    debug = { disable_logs = false },
})

-- Cosmic module below. The script removes only this section for lifecycle tests.
require("cosmic").setup({
    enabled = false,
    -- Deliberately omit idle_timeout: the test verifies the public 20 s default.
    fps = 30,
    snapshot_hz = 4,
    -- The test's screenshots are real screencopy sessions. Live defaults remain
    -- unchanged and continue to exclude screen sharing.
    exclusions = { screenshare = false },
})
