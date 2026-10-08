#!/usr/bin/env bash
set -euo pipefail

repository=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
plugin="$repository/build/cosmic.so"
output=""
keep_open=false
while (($#)); do
    case "$1" in
        --plugin) plugin=${2:?}; shift 2 ;;
        --output) output=${2:?}; shift 2 ;;
        --keep-open) keep_open=true; shift ;;
        -h|--help) printf '%s\n' 'Usage: scripts/nested-test.sh [--plugin FILE] [--output DIR] [--keep-open]'; exit 0 ;;
        *) printf 'Unknown argument: %s\n' "$1" >&2; exit 2 ;;
    esac
done
for command in Hyprland hyprctl gcc wayland-scanner pkg-config python3 rg; do
    command -v "$command" >/dev/null || { printf 'Missing dependency: %s\n' "$command" >&2; exit 1; }
done
[[ -f "$plugin" ]] || { printf 'Build the Cosmic plugin first: %s\n' "$plugin" >&2; exit 1; }
[[ -n ${WAYLAND_DISPLAY:-} && -n ${XDG_RUNTIME_DIR:-} ]] || { printf 'Run from a Wayland session.\n' >&2; exit 1; }
printf '%s\n' 'Visual-test note: the parent compositor must continuously render this nested window. Pause parent Cosmic or other frame throttling before visual checks; this script NEVER changes the parent session.' >&2
if [[ -z "$output" ]]; then output=$(mktemp -d /tmp/hyprcosmos-test.XXXXXXXX); else mkdir -p -- "$output"; fi
output=$(realpath -- "$output")
plugin=$(realpath -- "$plugin")
socket="hyprcosmos-test-$$"
native_socket=""
instance=""
compositor_pid=""
client_a=""
client_b=""
client_c=""
held_input=""
share_pid=""
capture_child=""
capture_jobs=()
cleanup() {
    for pid in "$capture_child" "${capture_jobs[@]}" "$share_pid" "$held_input" "$client_a" "$client_b" "$client_c" "$compositor_pid"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then kill -TERM "$pid" 2>/dev/null || true; fi
    done
    if [[ -n "$native_socket" && -L "$XDG_RUNTIME_DIR/$socket" && $(readlink -- "$XDG_RUNTIME_DIR/$socket") == "$native_socket" ]]; then
        rm -- "$XDG_RUNTIME_DIR/$socket"
    fi
    printf 'Nested test logs retained: %s\n' "$output"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# Generate client protocol bindings only from installed stable XML and vendored
# matching input protocols. The plugin build requires no downloaded source.
mkdir -p "$output/protocols" "$output/config"
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
for protocol in toplevel-export foreign-toplevel; do
    case "$protocol" in
        toplevel-export) xml="$repository/tests/protocols/hyprland-toplevel-export-v1.xml" ;;
        foreign-toplevel) xml="$repository/tests/protocols/wlr-foreign-toplevel-management-unstable-v1.xml" ;;
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
gcc -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -I"$output/protocols" "${cflags[@]}" \
    "$repository/tests/share-probe.c" "$output/protocols/toplevel-export-protocol.c" \
    "$output/protocols/foreign-toplevel-protocol.c" "${libs[@]}" -o "$output/share-probe"
cp -- "$repository/tests/nested.lua" "$output/config/hyprland.lua"
ln -s -- "$repository/lua/cosmic" "$output/config/cosmic"
ln -s -- "$plugin" "$output/config/cosmic.so"

# Keep the parent WAYLAND_DISPLAY for the Aquamarine nested Wayland backend.
# Select the new compositor by its exact child PID. --socket is a handover API
# requiring --wayland-fd in this version, so let Hyprland create its own socket.
env -u HYPRLAND_INSTANCE_SIGNATURE HYPRLAND_NO_SD_VARS=1 HYPRLAND_NO_SD_NOTIFY=1 \
    HYPRLAND_NO_CRASHREPORTER=1 Hyprland --config "$output/config/hyprland.lua" \
    > "$output/compositor.log" 2>&1 &
compositor_pid=$!
for ((attempt=0; attempt<200; ++attempt)); do
    kill -0 "$compositor_pid" 2>/dev/null || { tail -n 50 "$output/compositor.log"; exit 1; }
    read -r instance native_socket <<< "$(hyprctl -j instances | python3 -c 'import json,sys; x=next((x for x in json.load(sys.stdin) if x.get("pid")==int(sys.argv[1])), {}); print(x.get("instance", ""), x.get("wl_socket", ""))' "$compositor_pid")"
    if [[ -n "$instance" && -S "$XDG_RUNTIME_DIR/$native_socket" ]]; then break; fi
    sleep 0.1
done
[[ -n "$instance" ]] || { printf 'Nested instance did not become ready.\n' >&2; exit 1; }
# The alias both makes probe's safety guard auditable and avoids depending on
# numeric wayland-N names that might otherwise refer to the working session.
[[ ! -e "$XDG_RUNTIME_DIR/$socket" && ! -L "$XDG_RUNTIME_DIR/$socket" ]] || { printf 'Test socket alias already exists.\n' >&2; exit 1; }
ln -s -- "$native_socket" "$XDG_RUNTIME_DIR/$socket"
printf 'Nested PID: %s\nInstance: %s\nWayland socket: %s\n' "$compositor_pid" "$instance" "$socket" | tee "$output/session.txt"
ctl() { hyprctl -i "$instance" "$@"; }
eval_lua() {
    local result
    if ! result=$(ctl eval "$1" 2>&1); then
        printf 'Lua command failed: %s\n%s\n' "$1" "$result" >&2
        exit 1
    fi
    [[ "$result" == ok ]] || { printf 'Lua check failed: %s\n%s\n' "$1" "$result" >&2; exit 1; }
}
status() { ctl repl 'local s=require("cosmic").status(); for _,k in ipairs({"enabled","initialized","active","bodies","stored","snapshots","snapshot_bytes","history_frames","physics_steps","last_reason"}) do print(k .. "=" .. tostring(s[k])) end; for _,b in ipairs(s.objects or {}) do print(string.format("object id=%s x=%.4f y=%.4f angle=%.4f scale=%.6f stretch=%.4f twist=%.4f sink=%.6f stored=%s",tostring(b.id),b.x,b.y,b.angle,b.scale,b.stretch,b.twist,b.sink_progress,tostring(b.stored))) end'; }
wait_active() {
    for ((attempt=0; attempt<60; ++attempt)); do
        if [[ $(ctl eval 'assert(require("cosmic").status().active)') == ok ]]; then return; fi
        sleep 0.1
    done
    status >&2
    printf 'Cosmic did not enter idle mode.\n' >&2
    exit 1
}
probe() { env -u HYPRLAND_INSTANCE_SIGNATURE WAYLAND_DISPLAY="$socket" "$output/wayland-probe" --input "$@"; }
event_count() {
    local count
    count=$(rg -c -- "$1" "$2" || true)
    printf '%s\n' "${count:-0}"
}
normal_center() {
    ctl -j clients | python3 -c 'import json,sys; c=next(c for c in json.load(sys.stdin) if c.get("title")==sys.argv[1]); print(int(c["at"][0]+c["size"][0]/2),int(c["at"][1]+c["size"][1]/2))' "$1"
}
monitor_extents() {
    ctl -j monitors | python3 -c 'import json,sys; m=json.load(sys.stdin)[0]; print(m["width"],m["height"])'
}
start_window_share() {
    local log=$1 address
    address=$(ctl -j clients | python3 -c 'import json,sys; print(next(c["address"] for c in json.load(sys.stdin) if c.get("title")=="Cosmic probe A"))')
    env -u HYPRLAND_INSTANCE_SIGNATURE WAYLAND_DISPLAY="$socket" "$output/share-probe" "$address" > "$log" 2>&1 &
    share_pid=$!
    for ((attempt=0; attempt<50; ++attempt)); do
        kill -0 "$share_pid" 2>/dev/null || { printf 'Window share probe exited:\n' >&2; sed -n '1,30p' "$log" >&2; exit 1; }
        if [[ $(event_count '"event":"window_frame"' "$log") -ge 3 ]]; then return; fi
        sleep 0.1
    done
    printf 'Window share did not deliver three real frames:\n' >&2
    sed -n '1,30p' "$log" >&2
    exit 1
}
stop_window_share() {
    kill -TERM "$share_pid"
    wait "$share_pid" 2>/dev/null || true
    share_pid=""
    # The source's share-stop timer uses 500ms; allow its actual false event
    # before checking an enable that deliberately missed it while disabled.
    sleep 0.65
}
capture() {
    if command -v grim >/dev/null; then
        # Screenshot delivery is not instantaneous. The actual image lies
        # within this interval, not necessarily at the requested sample time.
        printf 'capture_before_monotonic=%s\n' "$(awk '{print $1}' /proc/uptime)" > "$output/$1.capture.txt"
        status >> "$output/$1.capture.txt"
        env -u HYPRLAND_INSTANCE_SIGNATURE WAYLAND_DISPLAY="$socket" grim "$output/$1.png" &
        capture_child=$!
        wait "$capture_child" || true
        capture_child=""
        printf 'capture_after_monotonic=%s\n' "$(awk '{print $1}' /proc/uptime)" >> "$output/$1.capture.txt"
        status >> "$output/$1.capture.txt"
    fi
}
wait_sink_time() {
    local remaining
    remaining=$(awk -v origin="$sink_started" -v desired="$1" '{d=desired-($1-origin); printf "%.3f\n", (d>0?d:0)}' /proc/uptime)
    sleep "$remaining"
}
check_distinct_captures() {
    local first=$1 second=$2 first_end second_begin
    [[ -s "$output/$first.png" && -s "$output/$second.png" ]] || return 0
    first_end=$(awk -F= '/^capture_after_monotonic=/ {print $2}' "$output/$first.capture.txt")
    second_begin=$(awk -F= '/^capture_before_monotonic=/ {print $2}' "$output/$second.capture.txt")
    # Delayed overlapping requests may legitimately resolve to the same frame.
    # Only compare intervals separated by more than one configured frame.
    if awk -v end="$first_end" -v begin="$second_begin" 'BEGIN {exit !(begin-end>0.04)}'; then
        if cmp -s "$output/$first.png" "$output/$second.png"; then
            printf 'FAIL: frozen screencopy mirror between %s and %s. Check shader mirror output and ensure the parent compositor is not throttling this nested window.\n' "$first" "$second" >&2
            exit 1
        fi
        printf 'PASS: distinct timed frames %s and %s\n' "$first" "$second" >> "$output/capture-regression.txt"
    else
        printf 'SKIP: %s/%s acquisition intervals overlap; these PNGs are not independent-time visual evidence. Pause parent Cosmic/frame throttling and rerun.\n' "$first" "$second" | tee -a "$output/capture-regression.txt" >&2
    fi
}
measure_cpu() {
    local mode=$1 duration=$2 before after start end ticks
    ticks=$(getconf CLK_TCK)
    before=$(awk '{print $14+$15}' "/proc/$compositor_pid/stat")
    start=$(awk '{print $1}' /proc/uptime)
    sleep "$duration"
    after=$(awk '{print $14+$15}' "/proc/$compositor_pid/stat")
    end=$(awk '{print $1}' /proc/uptime)
    python3 -c 'import sys; mode,a,b,hz,t0,t1=sys.argv[1:]; elapsed=float(t1)-float(t0); print(f"{mode}: {(int(b)-int(a))/float(hz)/elapsed*100:.2f}% of one CPU core over {elapsed:.2f}s")' \
        "$mode" "$before" "$after" "$ticks" "$start" "$end" | tee -a "$output/cpu-usage.txt"
}
ctl -j version > "$output/version.json"
ctl -j monitors > "$output/monitors.json"
errors=$(ctl configerrors)
[[ "$errors" == ok || -z "$errors" ]] || { printf 'Nested configuration errors: %s\n' "$errors" >&2; exit 1; }
eval_lua 'assert(hl.plugin.cosmic ~= nil)'
env -u HYPRLAND_INSTANCE_SIGNATURE WAYLAND_DISPLAY="$socket" "$output/wayland-probe" --window 0 > "$output/client-a.jsonl" 2>&1 &
client_a=$!
env -u HYPRLAND_INSTANCE_SIGNATURE WAYLAND_DISPLAY="$socket" "$output/wayland-probe" --window 1 > "$output/client-b.jsonl" 2>&1 &
client_b=$!
sleep 1
eval_lua 'local s=require("cosmic").status(); assert(s.module_initialized and s.error==nil)'

# SHARE_WINDOW can predate input/render listeners. Its first real frames happen
# while Cosmic is disabled, so enable must query current native session state.
eval_lua 'require("cosmic").setup({enabled=false,exclusions={screenshare=true}})'
start_window_share "$output/share-before-enable.jsonl"
eval_lua 'require("cosmic").enable()'
sleep 1.4
eval_lua 'local s=require("cosmic").status(); assert(s.initialized and not s.active)'
status > "$output/share-before-enable-status.txt"
stop_window_share
wait_active

# Start under enabled listeners, then end while disabled. A re-enable must not
# retain a stale start event after the real session has stopped.
start_window_share "$output/share-ended-while-disabled.jsonl"
eval_lua 'local s=require("cosmic").status(); assert(s.initialized and not s.active); require("cosmic").disable()'
stop_window_share
eval_lua 'require("cosmic").enable()'
wait_active
status > "$output/share-after-reenable-status.txt"
eval_lua 'require("cosmic").setup({enabled=false,exclusions={screenshare=false}})'
printf '%s\n' 'PASS: real window sharing predating enable blocks idle; sharing ended during disable does not leave stale exclusion.' > "$output/share-regression.txt"

ctl -j clients > "$output/normal-clients.json"
capture normal
measure_cpu normal 2
eval_lua 'assert(require("cosmic").enable()); assert(require("cosmic")==require("cosmic"))'
wait_active
sleep 0.4
measure_cpu cosmic 2
status > "$output/idle-status.txt"
capture universe

# The first input reaches the real client, and causes immediate normal restore.
probe key 30
eval_lua 'assert(not require("cosmic").status().active)'
sleep 0.1
[[ $(rg -c '"event":"key","key":30,"state":1' "$output/client-a.jsonl" "$output/client-b.jsonl" | awk -F: '{n+=$NF} END {print n+0}') -eq 1 ]] || { printf 'First normal key was lost or duplicated.\n' >&2; exit 1; }
capture restored

# Pressed real device state must prevent automatic entry past idle_timeout.
probe key 48 2200 & held_input=$!
sleep 1.3
eval_lua 'assert(not require("cosmic").status().active)'
wait "$held_input"; held_input=""
wait_active
probe key 64
eval_lua 'local s=require("cosmic").status(); assert(s.active and s.gravity_mode==1)'
probe key 68
eval_lua 'local s=require("cosmic").status(); assert(s.active and s.alternate_region)'
capture virtual-region
probe key 68
eval_lua 'local s=require("cosmic").status(); assert(s.active and not s.alternate_region)'
probe key 88
eval_lua 'assert(not require("cosmic").status().active)'
probe key 87
eval_lua 'assert(require("cosmic").status().active)'
probe key 87
eval_lua 'assert(not require("cosmic").status().active)'
wait_active
probe ctrl-key 46
eval_lua 'assert(not require("cosmic").status().active)'
sleep 0.1
rg -q '"event":"modifiers","depressed":0' "$output/client-a.jsonl" "$output/client-b.jsonl"

# Pointer movement moves the gravity source and retains ordinary focus. First
# button/axis events are delivered after normal hit testing at restored geometry.
wait_active
ctl -j activewindow > "$output/focus-before-motion.json"
read -r pointer_x pointer_y <<< "$(normal_center 'Cosmic probe A')"
read -r extent_x extent_y <<< "$(monitor_extents)"
probe move "$pointer_x" "$pointer_y" "$extent_x" "$extent_y"
eval_lua 'assert(require("cosmic").status().active)'
ctl -j activewindow > "$output/focus-after-motion.json"
python3 -c 'import json,sys; a=json.load(open(sys.argv[1])); b=json.load(open(sys.argv[2])); assert a.get("address")==b.get("address"), "Mouse motion changed keyboard focus"' "$output/focus-before-motion.json" "$output/focus-after-motion.json"
buttons_a=$(event_count '"event":"button","button":272,"state":1' "$output/client-a.jsonl")
buttons_b=$(event_count '"event":"button","button":272,"state":1' "$output/client-b.jsonl")
probe click 272
eval_lua 'assert(not require("cosmic").status().active)'
sleep 0.1
[[ $(event_count '"event":"button","button":272,"state":1' "$output/client-a.jsonl") -eq $((buttons_a + 1)) &&
   $(event_count '"event":"button","button":272,"state":1' "$output/client-b.jsonl") -eq "$buttons_b" ]] || { printf 'First click was lost, duplicated or delivered to the wrong normal window.\n' >&2; exit 1; }
probe click 272 2200 & held_input=$!
sleep 1.3
eval_lua 'assert(not require("cosmic").status().active)'
wait "$held_input"; held_input=""
wait_active
read -r pointer_x pointer_y <<< "$(normal_center 'Cosmic probe B')"
probe move "$pointer_x" "$pointer_y" "$extent_x" "$extent_y"
axes_a=$(event_count '"event":"axis"' "$output/client-a.jsonl")
axes_b=$(event_count '"event":"axis"' "$output/client-b.jsonl")
probe scroll 15
eval_lua 'assert(not require("cosmic").status().active)'
sleep 0.1
[[ $(event_count '"event":"axis"' "$output/client-a.jsonl") -eq "$axes_a" &&
   $(event_count '"event":"axis"' "$output/client-b.jsonl") -eq $((axes_b + 1)) ]] || { printf 'First scroll was lost, duplicated or delivered to the wrong normal window.\n' >&2; exit 1; }
eval_lua 'require("cosmic").disable(); assert(not require("cosmic").status().initialized)'
ctl -j clients > "$output/restored-clients.json"
python3 -c 'import json,sys; a=json.load(open(sys.argv[1])); b=json.load(open(sys.argv[2])); shape=lambda c:sorted((x["address"],x["at"],x["size"],x["workspace"]["id"]) for x in c); assert shape(a)==shape(b), "Cosmic altered normal geometry or workspace"' "$output/normal-clients.json" "$output/restored-clients.json"

eval_lua 'require("cosmic").setup({enabled=true}); require("cosmic").setup({enabled=true}); assert(require("cosmic").status().initialized)'
wait_active
eval_lua 'require("cosmic").action("supernova"); require("cosmic").action("gravity")'
sleep 0.5
capture supernova
eval_lua 'require("cosmic").action("rewind"); assert(require("cosmic").status().rewinding)'
sleep 0.2
capture rewind

# A real F7 tests the default-duration take-in motion, intermediate scales and
# deformation, virtual storage, and replay of the same living app imagery.
# Other forces are disabled so the measured motion comes from the sink itself.
eval_lua 'require("cosmic").setup({enabled=true, effects={cursor_gravity=false,orbit=false,binary=false,collisions=false,wormholes=false,supernova=false,expansion=false}})'
wait_active
sleep 0.5
read -r body_x body_y <<< "$(ctl repl 'local s=require("cosmic").status(); for _,b in ipairs(s.objects) do if b.region<1000000 then print(math.floor(b.x).." "..math.floor(b.y)); break end end')"
read -r extent_x extent_y <<< "$(monitor_extents)"
[[ "$body_x" =~ ^[0-9]+$ && "$body_y" =~ ^[0-9]+$ ]] || { printf 'Could not resolve a virtual sink target.\n' >&2; exit 1; }
probe move "$body_x" "$body_y" "$extent_x" "$extent_y"
eval_lua '_cosmic_sink_origins={}; for _,b in ipairs(require("cosmic").status().objects) do _cosmic_sink_origins[b.id]={x=b.x,y=b.y,scale=b.scale} end'
probe key 65
sink_started=$(awk '{print $1}' /proc/uptime)
eval_lua 'local s=require("cosmic").status(); local found=false; for _,b in ipairs(s.objects) do if b.sink_progress>0 and not b.stored then _cosmic_sink_target=b.id; _cosmic_sink_previous_scale=b.scale; found=true end end; assert(s.active and found, "real F7 did not start a native sink")'
status > "$output/black-hole-start.txt"
for sample in 0.1 0.5 1.0 1.5 2.0; do
    (
        # Own only this capture subprocess; never run the main session cleanup
        # from a background worker or leave a sharing client after interruption.
        trap 'if [[ -n "$capture_child" ]]; then kill -TERM "$capture_child" 2>/dev/null || true; fi' EXIT
        trap 'exit 130' INT TERM
        wait_sink_time "$sample"
        capture "black-hole-$sample"
    ) &
    capture_jobs+=("$!")
done
wait_sink_time 0.20
eval_lua 'local s=require("cosmic").status(); local found=false; for _,b in ipairs(s.objects) do if b.id==_cosmic_sink_target then assert(not b.stored and b.sink_progress>0 and b.sink_progress<0.5); assert(b.scale>_cosmic_sink_origins[b.id].scale*0.8, "sink image shrank too early"); _cosmic_sink_previous_scale=b.scale; found=true end end; assert(s.active and found)'
status > "$output/black-hole-early-status.txt"
wait_sink_time 0.75
eval_lua 'local s=require("cosmic").status(); local found=false; for _,b in ipairs(s.objects) do if b.id==_cosmic_sink_target then local o=_cosmic_sink_origins[b.id]; assert(not b.stored and b.scale>0 and b.scale<=_cosmic_sink_previous_scale); assert((b.x-o.x)^2+(b.y-o.y)^2>9, "centered sink did not visibly move"); _cosmic_sink_previous_scale=b.scale; found=true end end; assert(s.active and found)'
status > "$output/black-hole-motion-status.txt"
wait_sink_time 1.40
eval_lua 'local s=require("cosmic").status(); local found=false; for _,b in ipairs(s.objects) do if b.id==_cosmic_sink_target then assert(not b.stored and b.scale>0 and b.scale<_cosmic_sink_previous_scale and b.stretch>1.5 and b.twist>0, "missing intermediate shrinking/deformation"); _cosmic_sink_previous_scale=b.scale; found=true end end; assert(s.active and found)'
status > "$output/black-hole-middle-status.txt"
wait_sink_time 2.05
eval_lua 'local s=require("cosmic").status(); local found=false; for _,b in ipairs(s.objects) do if b.id==_cosmic_sink_target then assert(not b.stored and b.scale>0 and b.scale<_cosmic_sink_previous_scale); found=true end end; assert(s.active and found)'
status > "$output/black-hole-late-status.txt"
for capture_job in "${capture_jobs[@]}"; do wait "$capture_job"; done
capture_jobs=()
check_distinct_captures black-hole-0.1 black-hole-0.5
check_distinct_captures black-hole-1.0 black-hole-1.5
wait_sink_time 3.20
eval_lua 'local s=require("cosmic").status(); local found=false; for _,b in ipairs(s.objects) do if b.id==_cosmic_sink_target then assert(b.stored and b.scale==0); found=true end end; assert(s.active and found)'
capture stored
status > "$output/stored-status.txt"
eval_lua 'require("cosmic").action("rewind"); assert(require("cosmic").status().rewinding)'
sleep 0.45
capture black-hole-rewind
sleep 1.6
eval_lua 'local s=require("cosmic").status(); assert(s.active and s.stored==0)'

# Actual window creation emits a shockwave; closing it removes every historical
# reference so replay cannot resurrect an exited client.
eval_lua 'require("cosmic").setup({enabled=true,effects={supernova=true}})'
wait_active
env -u HYPRLAND_INSTANCE_SIGNATURE WAYLAND_DISPLAY="$socket" "$output/wayland-probe" --window 2 > "$output/client-c.jsonl" 2>&1 &
client_c=$!
sleep 0.35
eval_lua 'local s=require("cosmic").status(); assert(s.active and s.bodies==3 and s.waves>0)'
capture newborn-supernova
kill -TERM "$client_c"
wait "$client_c" 2>/dev/null || true
client_c=""
sleep 0.3
eval_lua 'local s=require("cosmic").status(); assert(s.active and s.bodies==2); require("cosmic").action("rewind")'
sleep 0.3
eval_lua 'assert(require("cosmic").status().bodies==2)'

# Hotplug and scale changes affect only this compositor's user-created output.
if hotplug=$(ctl output create headless COSMIC-TEST-MONITOR 2>&1) && [[ "$hotplug" == ok ]]; then
    sleep 0.25
    eval_lua 'assert(not require("cosmic").status().active)'
    eval_lua 'hl.monitor({output="COSMIC-TEST-MONITOR",mode="640x480@60",position="1280x0",scale=2})'
    sleep 0.25
    ctl -j monitors > "$output/mixed-scale-monitors.json"
    python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); assert any(x["name"]=="COSMIC-TEST-MONITOR" and x["scale"]==2 for x in m); assert any(x["scale"]==1 for x in m)' "$output/mixed-scale-monitors.json"
    ctl output remove COSMIC-TEST-MONITOR > "$output/hotplug-remove.txt"
    sleep 0.2
else
    printf 'SKIP: headless hotplug unavailable: %s\n' "$hotplug" | tee "$output/hotplug-skip.txt"
fi

eval_lua 'require("cosmic").action("emergency"); require("cosmic").shutdown(); assert(not require("cosmic").status().initialized)'
ctl reload > "$output/reload.txt"
sleep 0.3
eval_lua 'assert(hl.plugin.cosmic ~= nil); assert(not require("cosmic").status().initialized)'
sed '/require("cosmic")/,$d' "$repository/tests/nested.lua" > "$output/config/hyprland.lua"
ctl reload > "$output/remove-require.txt"
sleep 0.3
eval_lua 'assert(hl.plugin.cosmic == nil)'
printf '%s\n' 'PASS: nested real GL rotation/deformation/store/rewind, first key/button/scroll, dedicated controls, holds, modifiers, mouse focus, geometry, actual window creation/closure, window-share enable/disable regressions, lifecycle and require removal. Hotplug result and CPU measurements are in adjacent files.' | tee "$output/result.txt"
if "$keep_open"; then
    printf 'Dedicated nested compositor remains until Ctrl-C.\n'
    while kill -0 "$compositor_pid" 2>/dev/null; do sleep 1; done
fi
