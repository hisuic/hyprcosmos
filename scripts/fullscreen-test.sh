#!/usr/bin/env bash
set -euo pipefail

repository=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
plugin="$repository/build/cosmic.so"
output=""
nested_window=false
unload_rounds=12
while (($#)); do
    case "$1" in
        --plugin) plugin=${2:?}; shift 2 ;;
        --output) output=${2:?}; shift 2 ;;
        --nested-window) nested_window=true; shift ;;
        --unload-rounds) unload_rounds=${2:?}; shift 2 ;;
        -h|--help) printf '%s\n' 'Usage: scripts/fullscreen-test.sh [--plugin FILE] [--output DIR] [--nested-window] [--unload-rounds N]'; exit 0 ;;
        *) printf 'Unknown argument: %s\n' "$1" >&2; exit 2 ;;
    esac
done
[[ "$unload_rounds" =~ ^[0-9]+$ && "$unload_rounds" -ge 1 && "$unload_rounds" -le 100 ]] || { printf 'Unload rounds must be 1..100.\n' >&2; exit 2; }
for command in Hyprland hyprctl gcc wayland-scanner pkg-config python3 rg grim timeout; do
    command -v "$command" >/dev/null || { printf 'Missing dependency: %s\n' "$command" >&2; exit 1; }
done
pkg-config --exists gtk-layer-shell-0 gtk+-3.0 libpng wayland-client xkbcommon || { printf 'Install GTK 3, gtk-layer-shell, libpng, Wayland and xkbcommon development files.\n' >&2; exit 1; }
[[ -f "$plugin" ]] || { printf 'Build Cosmic first: %s\n' "$plugin" >&2; exit 1; }
[[ -n ${WAYLAND_DISPLAY:-} && -n ${XDG_RUNTIME_DIR:-} ]] || { printf 'Run from a Wayland session.\n' >&2; exit 1; }
if [[ -z "$output" ]]; then output=$(mktemp -d /tmp/hyprcosmos-fullscreen.XXXXXXXX); else mkdir -p -- "$output"; fi
output=$(realpath -- "$output")
plugin=$(realpath -- "$plugin")
parent_instance=${HYPRLAND_INSTANCE_SIGNATURE:-}
socket="hyprcosmos-test-ui-$$"
native_socket=""
instance=""
compositor_pid=""
client_pid=""
top_pid=""
overlay_pid=""
extra_pid=""
capture_pid=""
disabled_outputs=()
cleanup() {
    local pid
    for pid in "$capture_pid" "$extra_pid" "$top_pid" "$overlay_pid" "$client_pid" "$compositor_pid"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then kill -TERM "$pid" 2>/dev/null || true; fi
    done
    if [[ -n "$native_socket" && -L "$XDG_RUNTIME_DIR/$socket" && $(readlink -- "$XDG_RUNTIME_DIR/$socket") == "$native_socket" ]]; then
        rm -- "$XDG_RUNTIME_DIR/$socket"
    fi
    printf 'Isolated fullscreen test evidence retained: %s\n' "$output"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

mkdir -p -- "$output/protocols" "$output/config"
xdg_xml="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml"
for protocol in xdg-shell virtual-keyboard virtual-pointer; do
    case "$protocol" in
        xdg-shell) xml="$xdg_xml" ;;
        virtual-keyboard) xml="$repository/tests/protocols/virtual-keyboard-unstable-v1.xml" ;;
        virtual-pointer) xml="$repository/tests/protocols/wlr-virtual-pointer-unstable-v1.xml" ;;
    esac
    wayland-scanner client-header "$xml" "$output/protocols/$protocol-client-protocol.h"
    wayland-scanner private-code "$xml" "$output/protocols/$protocol-protocol.c"
done
read -r -a cflags <<< "$(pkg-config --cflags wayland-client xkbcommon)"
read -r -a libs <<< "$(pkg-config --libs wayland-client xkbcommon)"
gcc -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -I"$output/protocols" "${cflags[@]}" \
    "$repository/tests/wayland-probe.c" "$output/protocols/xdg-shell-protocol.c" \
    "$output/protocols/virtual-keyboard-protocol.c" "$output/protocols/virtual-pointer-protocol.c" \
    "${libs[@]}" -o "$output/wayland-probe"
read -r -a cflags <<< "$(pkg-config --cflags gtk-layer-shell-0 gtk+-3.0 libpng)"
read -r -a libs <<< "$(pkg-config --libs gtk-layer-shell-0 gtk+-3.0 libpng)"
gcc -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter "${cflags[@]}" \
    "$repository/tests/layer-probe.c" "${libs[@]}" -o "$output/layer-probe"
ln -s -- "$repository/lua/cosmic" "$output/config/cosmic"
ln -s -- "$plugin" "$output/config/cosmic.so"
# The plugin manager keys configured plugins by the requested lexical path, not
# realpath. Use the same symlink path Cosmic's module requested for IPC unload.
loaded_plugin="$output/config/cosmic.so"
write_config() {
    local include_cosmic=$1 name
    sed '/-- Cosmic module below/,$d' "$repository/tests/fullscreen.lua" > "$output/config/hyprland.lua"
    for name in "${disabled_outputs[@]}"; do
        printf 'hl.monitor({output="%s", disabled=true})\n' "$name" >> "$output/config/hyprland.lua"
    done
    if [[ ${#disabled_outputs[@]} -gt 0 ]]; then
        printf '%s\n' 'hl.monitor({output="COSMIC-UI-TEST", mode="1280x720@60", position="0x0", scale=1, disabled=false})' >> "$output/config/hyprland.lua"
    fi
    if "$include_cosmic"; then
        sed -n '/-- Cosmic module below/,$p' "$repository/tests/fullscreen.lua" >> "$output/config/hyprland.lua"
    fi
}
write_config true

# Bootstrap using only the parent Wayland backend. Never open/disable a physical
# display or reload the parent. All later calls address the exact child PID's
# instance and an alias guarded by both probe programs.
env -u HYPRLAND_INSTANCE_SIGNATURE HYPRLAND_NO_SD_VARS=1 HYPRLAND_NO_SD_NOTIFY=1 \
    HYPRLAND_NO_CRASHREPORTER=1 AQ_DRM_DEVICES=/dev/null Hyprland --config "$output/config/hyprland.lua" \
    > "$output/compositor.log" 2>&1 &
compositor_pid=$!
for ((attempt=0; attempt<200; ++attempt)); do
    kill -0 "$compositor_pid" 2>/dev/null || { tail -n 40 "$output/compositor.log" >&2; exit 1; }
    read -r instance native_socket <<< "$(hyprctl -j instances | python3 -c 'import json,sys; s=next((s for s in json.load(sys.stdin) if s.get("pid")==int(sys.argv[1])), {}); print(s.get("instance", ""), s.get("wl_socket", ""))' "$compositor_pid")"
    if [[ -n "$instance" && -S "$XDG_RUNTIME_DIR/$native_socket" ]]; then break; fi
    sleep .1
done
[[ -n "$instance" && "$instance" != "$parent_instance" ]] || { printf 'Cannot identify a unique isolated child compositor.\n' >&2; exit 1; }
[[ ! -e "$XDG_RUNTIME_DIR/$socket" && ! -L "$XDG_RUNTIME_DIR/$socket" ]] || { printf 'Test socket alias already exists.\n' >&2; exit 1; }
ln -s -- "$native_socket" "$XDG_RUNTIME_DIR/$socket"
printf 'child_pid=%s\nchild_instance=%s\nchild_socket=%s\nplugin=%s\n' "$compositor_pid" "$instance" "$socket" "$plugin" | tee "$output/session.txt"
ctl() { hyprctl -i "$instance" "$@"; }
eval_lua() {
    local result
    result=$(ctl eval "$1" 2>&1) || { printf 'Lua request failed: %s\n%s\n' "$1" "$result" >&2; exit 1; }
    [[ "$result" == ok ]] || { printf 'Lua assertion failed: %s\n%s\n' "$1" "$result" >&2; exit 1; }
}
wait_lua() {
    local predicate=$1
    for ((attempt=0; attempt<100; ++attempt)); do
        kill -0 "$compositor_pid" 2>/dev/null || { printf 'Child compositor died; see compositor.log.\n' >&2; exit 1; }
        if [[ $(ctl repl "print($predicate)" 2>/dev/null) == true ]]; then return; fi
        sleep .05
    done
    printf 'Timed out waiting for: %s\n' "$predicate" >&2
    exit 1
}
probe() { env -u HYPRLAND_INSTANCE_SIGNATURE WAYLAND_DISPLAY="$socket" "$output/wayland-probe" --input "$@"; }
event_count() {
    local count
    count=$(rg -c -- "$1" "$2" || true)
    printf '%s\n' "${count:-0}"
}
start_layer() {
    local log=$1 layer=$2 namespace=$3 mode=${4:-none}
    env -u HYPRLAND_INSTANCE_SIGNATURE WAYLAND_DISPLAY="$socket" GDK_BACKEND=wayland NO_AT_BRIDGE=1 GIO_USE_VFS=local \
        "$output/layer-probe" "$layer" "$namespace" "$mode" > "$log" 2>&1 &
    extra_pid=$!
    for ((attempt=0; attempt<80; ++attempt)); do
        kill -0 "$extra_pid" 2>/dev/null || { sed -n '1,20p' "$log" >&2; exit 1; }
        if [[ $(event_count '"event":"draw"' "$log") -ge 2 ]]; then return; fi
        sleep .05
    done
    printf 'Layer surface never rendered: %s\n' "$namespace" >&2
    exit 1
}
stop_extra() {
    kill -TERM "$extra_pid"
    wait "$extra_pid" 2>/dev/null || true
    extra_pid=""
    sleep .15
}
capture() {
    local name=$1 expectation=$2
    timeout 8s env -u HYPRLAND_INSTANCE_SIGNATURE WAYLAND_DISPLAY="$socket" grim "$output/$name.png" &
    capture_pid=$!
    wait "$capture_pid"
    capture_pid=""
    "$output/layer-probe" --check-shot "$output/$name.png" "$expectation" | tee -a "$output/pixels.txt"
}
geometry() {
    ctl -j clients | python3 -c 'import json,sys; c=next(c for c in json.load(sys.stdin) if c.get("title")=="Cosmic probe A"); print(json.dumps({k:c.get(k) for k in ("address","at","size","workspace","monitor","fullscreen")},sort_keys=True))'
}
reserved() { ctl -j monitors | python3 -c 'import json,sys; print(json.dumps([(m["name"],m.get("reserved")) for m in json.load(sys.stdin)],sort_keys=True))'; }
preview() {
    eval_lua 'require("cosmic").action("preview"); assert(require("cosmic").status().active)'
    sleep .2
}
check_alive() {
    kill -0 "$compositor_pid" || { printf 'Child compositor crashed during lifecycle regression.\n' >&2; exit 1; }
    eval_lua 'assert(hl.plugin.cosmic==nil)'
    [[ $(ctl plugin list) == *'No plugins loaded'* || $(ctl -j plugin list) == '[]' ]] || { printf 'Plugin remained loaded after removal.\n' >&2; exit 1; }
    # A successful screencopy after unload forces a real subsequent render pass:
    # mere IPC responsiveness cannot expose a dangling custom-pass deleter.
    capture "$1" visible
}

ctl -j version > "$output/version.json"
wait_lua 'hl.plugin.cosmic~=nil and require("cosmic").status().module_initialized'
eval_lua 'assert(require("cosmic").status().config.idle_timeout==20)'
if ! "$nested_window"; then
    mapfile -t disabled_outputs < <(ctl -j monitors | python3 -c 'import json,sys; print("\n".join(m["name"] for m in json.load(sys.stdin)))')
    for name in "${disabled_outputs[@]}"; do
        [[ "$name" =~ ^[A-Za-z0-9_.:-]+$ ]] || { printf 'Unexpected nested monitor name: %s\n' "$name" >&2; exit 1; }
    done
    [[ $(ctl output create headless COSMIC-UI-TEST) == ok ]] || { printf 'Could not create isolated headless output.\n' >&2; exit 1; }
    eval_lua 'hl.monitor({output="COSMIC-UI-TEST",mode="1280x720@60",position="0x0",scale=1,disabled=false})'
    for name in "${disabled_outputs[@]}"; do eval_lua "hl.monitor({output=\"$name\",disabled=true})"; done
    sleep .35
    ctl -j monitors | python3 -c 'import json,sys; m=json.load(sys.stdin); assert len(m)==1 and m[0]["name"]=="COSMIC-UI-TEST",m'
    write_config true
else
    printf '%s\n' 'Nested-window mode requires the parent compositor to render continuously; this script never changes it.' >&2
fi

start_layer "$output/top.jsonl" top cosmic-test-panel
top_pid=$extra_pid; extra_pid=""
start_layer "$output/overlay.jsonl" overlay cosmic-test-overlay
overlay_pid=$extra_pid; extra_pid=""
env -u HYPRLAND_INSTANCE_SIGNATURE WAYLAND_DISPLAY="$socket" "$output/wayland-probe" --window 0 > "$output/window.jsonl" 2>&1 &
client_pid=$!
sleep .5
ctl -j layers > "$output/layers.json"
geometry > "$output/geometry-before.json"
reserved > "$output/reserved-before.json"
ctl -j monitors | python3 -c 'import json,sys; m=json.load(sys.stdin)[0]; assert max(m["reserved"])>=48,m'
capture normal visible
eval_lua 'require("cosmic").enable()'
preview
eval_lua 'assert(require("cosmic").status().desktop_ui_hidden)'
capture cosmic hidden
geometry > "$output/geometry-active.json"
reserved > "$output/reserved-active.json"
cmp -- "$output/geometry-before.json" "$output/geometry-active.json"
cmp -- "$output/reserved-before.json" "$output/reserved-active.json"

# Moving over the hidden bar must remain playful, not focus an invisible UI.
before_motion=$(event_count '"event":"motion"' "$output/top.jsonl")
probe move 140 20 1280 720
probe move 230 20 1280 720
eval_lua 'assert(require("cosmic").status().active)'
[[ $(event_count '"event":"motion"' "$output/top.jsonl") -eq "$before_motion" ]] || { printf 'Invisible panel received pointer motion.\n' >&2; exit 1; }
before_click=$(event_count '"event":"button","button":1,"state":1' "$output/top.jsonl")
probe click 272
wait_lua 'not require("cosmic").status().active'
sleep .1
[[ $(event_count '"event":"button","button":1,"state":1' "$output/top.jsonl") -eq $((before_click+1)) ]] || { printf 'First restoration click was lost or duplicated.\n' >&2; exit 1; }
capture restored visible
geometry > "$output/geometry-restored.json"
reserved > "$output/reserved-restored.json"
cmp -- "$output/geometry-before.json" "$output/geometry-restored.json"
cmp -- "$output/reserved-before.json" "$output/reserved-restored.json"

eval_lua 'require("cosmic").setup({rendering={hide_desktop_ui=false}})'
preview
eval_lua 'assert(not require("cosmic").status().desktop_ui_hidden)'
capture ui-opt-out visible
eval_lua 'require("cosmic").action("emergency"); require("cosmic").setup({rendering={hide_desktop_ui=true}})'

# Real interactive and lock-like surfaces must be shown rather than suppressed,
# including a lock namespace that intentionally requests no keyboard focus.
for spec in 'exclusive cosmic-test-launcher' 'none hyprlock'; do
    read -r keyboard_mode namespace <<< "$spec"
    preview
    start_layer "$output/$namespace.jsonl" overlay "$namespace" "$keyboard_mode"
    wait_lua 'not require("cosmic").status().active'
    eval_lua 'require("cosmic").action("preview"); assert(not require("cosmic").status().active); require("cosmic").setup({idle_timeout=.5})'
    sleep .85
    eval_lua 'assert(not require("cosmic").status().active)'
    stop_extra
    eval_lua 'require("cosmic").setup({idle_timeout=require("cosmic.config").defaults.idle_timeout})'
done

# Exercise unload without first shutting Cosmic down. Screenshot delivery proves
# an actual Cosmic pass has been rendered/retained immediately before unload.
    for ((round=1; round<=unload_rounds; ++round)); do
    preview
    capture "direct-active-$round" hidden
    # unloadPlugin queues an automatic config reload in 0.56.2. Remove require
    # from the generated child config first, without reloading: Cosmic is still
    # active at the direct unload, and that queued reload cannot load it again.
    write_config false
    [[ $(ctl plugin unload "$loaded_plugin") == ok ]] || { printf 'Direct plugin unload failed.\n' >&2; exit 1; }
    check_alive "direct-unloaded-$round"
    write_config true
    [[ $(ctl reload) == ok ]] || { printf 'Direct unload recovery reload failed.\n' >&2; exit 1; }
    wait_lua 'hl.plugin.cosmic~=nil and require("cosmic").status().module_initialized'
    eval_lua 'require("cosmic").enable()'
done
for ((round=1; round<=3; ++round)); do
    preview
    capture "remove-active-$round" hidden
    write_config false
    [[ $(ctl reload) == ok ]] || { printf 'Require-removal reload failed.\n' >&2; exit 1; }
    check_alive "require-removed-$round"
    write_config true
    [[ $(ctl reload) == ok ]] || { printf 'Require-restoration reload failed.\n' >&2; exit 1; }
    wait_lua 'hl.plugin.cosmic~=nil and require("cosmic").status().module_initialized'
    eval_lua 'require("cosmic").enable()'
done

eval_lua 'require("cosmic").setup({idle_timeout=require("cosmic.config").defaults.idle_timeout}); assert(require("cosmic").status().config.idle_timeout==20)'
probe move 640 360 1280 720
printf 'idle_start_monotonic=%s\n' "$(awk '{print $1}' /proc/uptime)" > "$output/idle-timing.txt"
sleep 5
eval_lua 'assert(not require("cosmic").status().active)'
printf 'inactive_at_5s_monotonic=%s\n' "$(awk '{print $1}' /proc/uptime)" >> "$output/idle-timing.txt"
sleep 16
eval_lua 'local s=require("cosmic").status(); assert(s.active and s.last_reason=="idle" and s.desktop_ui_hidden)'
printf 'active_at_21s_monotonic=%s\n' "$(awk '{print $1}' /proc/uptime)" >> "$output/idle-timing.txt"
capture idle-21s hidden
probe key 30
wait_lua 'not require("cosmic").status().active'
capture idle-restored visible
printf 'PASS: real TOP/OVERLAY suppression, exclusive-zone and geometry preservation, first panel click once, protected/interactive layer safety, optional UI visibility, %s active direct unloads, 3 active require removals, and default 20 s idle activation. Parent session was never changed.\n' "$unload_rounds" | tee "$output/result.txt"
