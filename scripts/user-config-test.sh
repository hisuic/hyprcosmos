#!/usr/bin/env bash
set -euo pipefail

repository=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
plugin="$repository/build/cosmic.so"
output=""
while (($#)); do
    case "$1" in
        --plugin) plugin=${2:?}; shift 2 ;;
        --output) output=${2:?}; shift 2 ;;
        -h|--help) printf '%s\n' 'Usage: scripts/user-config-test.sh [--plugin FILE] [--output NEW_DIR]'; exit 0 ;;
        *) printf 'Unknown argument: %s\n' "$1" >&2; exit 2 ;;
    esac
done
for command in Hyprland hyprctl python3 sed rg realpath; do
    command -v "$command" >/dev/null || { printf 'Missing dependency: %s\n' "$command" >&2; exit 1; }
done
[[ -f "$plugin" ]] || { printf 'Build Cosmic first: %s\n' "$plugin" >&2; exit 1; }
[[ -n ${WAYLAND_DISPLAY:-} && -n ${XDG_RUNTIME_DIR:-} ]] || { printf 'Run from a Wayland session.\n' >&2; exit 1; }
if [[ -z "$output" ]]; then
    output=$(mktemp -d /tmp/hyprcosmos-user-config.XXXXXXXX)
else
    # Generated configuration is intentionally disposable. Never accept a
    # populated directory or symlink that could overwrite existing user files.
    [[ ! -e "$output" && ! -L "$output" ]] || { printf 'Output must be a new directory: %s\n' "$output" >&2; exit 1; }
    mkdir -p -- "$output"
fi
output=$(realpath -- "$output")
plugin=$(realpath -- "$plugin")
parent_instance=${HYPRLAND_INSTANCE_SIGNATURE:-}
socket="hyprcosmos-test-config-$$"
native_socket=""
instance=""
compositor_pid=""
disabled_outputs=()
cleanup() {
    if [[ -n "$compositor_pid" ]]; then
        if kill -0 "$compositor_pid" 2>/dev/null; then kill -TERM "$compositor_pid" 2>/dev/null || true; fi
        local child_exit=0
        wait "$compositor_pid" 2>/dev/null || child_exit=$?
        printf 'child_exit=%s\n' "$child_exit" > "$output/child-exit.txt"
    fi
    if [[ -n "$native_socket" && -L "$XDG_RUNTIME_DIR/$socket" && $(readlink -- "$XDG_RUNTIME_DIR/$socket") == "$native_socket" ]]; then
        rm -- "$XDG_RUNTIME_DIR/$socket"
    fi
    printf 'Isolated user-config evidence retained: %s\n' "$output"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

mkdir -p -- "$output/config"
settings="$output/config/hyprcosmos.lua"
expected_path=$(python3 -c 'import json,sys; print(json.dumps(sys.argv[1],ensure_ascii=False))' "$settings")
ln -s -- "$repository/lua/cosmic" "$output/config/cosmic"
ln -s -- "$plugin" "$output/config/cosmic.so"
write_config() {
    local mode=$1 name
    sed '/-- Cosmic module below/,$d' "$repository/tests/fullscreen.lua" > "$output/config/hyprland.lua"
    for name in "${disabled_outputs[@]}"; do
        printf 'hl.monitor({output="%s", disabled=true})\n' "$name" >> "$output/config/hyprland.lua"
    done
    if [[ ${#disabled_outputs[@]} -gt 0 ]]; then
        printf '%s\n' 'hl.monitor({output="COSMIC-CONFIG-TEST",mode="1280x720@60",position="0x0",scale=1,disabled=false})' >> "$output/config/hyprland.lua"
    fi
    case "$mode" in
        bare) printf '%s\n' 'require("cosmic")' >> "$output/config/hyprland.lua" ;;
        inline) printf '%s\n' 'require("cosmic").setup({idle_timeout=19,rendering={stars=31}})' >> "$output/config/hyprland.lua" ;;
        removed) ;;
        *) printf 'Unknown fixture config mode: %s\n' "$mode" >&2; exit 1 ;;
    esac
}
write_settings() {
    local label=$1 content=$2
    printf '%s\n' "$content" > "$settings"
    cp -- "$settings" "$output/settings-$label.lua"
}
write_config bare

# No probes or input injection are needed. The only compositor selected for
# reloads is the new child PID's exact instance. A private headless output makes
# native initialization independent of the parent's rendering/idle state.
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
printf 'child_pid=%s\nchild_instance=%s\nchild_socket=%s\nplugin=%s\nsettings=%s\n' "$compositor_pid" "$instance" "$socket" "$plugin" "$settings" | tee "$output/session.txt"
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
config_ok() {
    local errors
    errors=$(ctl configerrors)
    printf '%s\n' "$errors" >> "$output/configerrors.txt"
    [[ "$errors" == ok || -z "$errors" ]] || { printf 'Unexpected compositor config errors: %s\n' "$errors" >&2; exit 1; }
}
reload_config() {
    local label=$1 result
    result=$(ctl reload 2>&1)
    [[ "$result" == ok ]] || { printf 'Reload failed: %s\n' "$result" >&2; exit 1; }
    wait_lua 'hl.plugin.cosmic~=nil and require("cosmic").status().available'
    config_ok
    ctl repl 'local s=require("cosmic").status(); print("enabled="..tostring(s.enabled).." initialized="..tostring(s.initialized).." module_initialized="..tostring(s.module_initialized).." watchers="..tostring(s.input_watchers).." idle="..tostring(s.config.idle_timeout).." stars="..tostring(s.config.rendering.stars).." file="..tostring(s.config_file).." loaded="..tostring(s.config_file_loaded).." file_error="..tostring(s.config_file_error))' > "$output/$label.status.txt"
}
ready() { wait_lua 'require("cosmic").status().module_initialized'; }
pass() { printf 'PASS: %s\n' "$1" | tee -a "$output/checks.txt"; }
check_invalid() {
    local label=$1 content=$2
    write_settings "$label" "$content"
    reload_config "$label"
    eval_lua "local c=require('cosmic'); local s=c.status(); assert(s.config_file==$expected_path and not s.config_file_loaded and type(s.config_file_error)=='string' and s.config_file_error:find($expected_path,1,true)); assert(s.error==s.config_file_error and not s.initialized and not s.module_initialized and not s.initialization_pending and not s.active and s.input_watchers==0); local ok,err=c.enable(); assert(ok==nil and err==s.config_file_error); c.status(); c.status(); assert(c.status().config_file_error==s.config_file_error and not c.status().initialized)"
    config_ok
    pass "$label is contained to Cosmic, with error path, zero native watchers and no compositor parsing error"
}

ctl -j version > "$output/version.json"
wait_lua 'hl.plugin.cosmic~=nil and require("cosmic").status().module_initialized'
config_ok
eval_lua "local s=require('cosmic').status(); assert(s.config_file==$expected_path and not s.config_file_loaded and s.config_file_error==nil and s.config.idle_timeout==60 and s.config.rendering.stars==240 and s.initialized and s.input_watchers>0)"
pass 'missing dedicated file uses the 60 s defaults and lexical module-sibling path'

mapfile -t disabled_outputs < <(ctl -j monitors | python3 -c 'import json,sys; print("\n".join(m["name"] for m in json.load(sys.stdin)))')
for name in "${disabled_outputs[@]}"; do
    [[ "$name" =~ ^[A-Za-z0-9_.:-]+$ ]] || { printf 'Unexpected nested monitor name: %s\n' "$name" >&2; exit 1; }
done
[[ $(ctl output create headless COSMIC-CONFIG-TEST) == ok ]] || { printf 'Could not create isolated headless output.\n' >&2; exit 1; }
eval_lua 'hl.monitor({output="COSMIC-CONFIG-TEST",mode="1280x720@60",position="0x0",scale=1,disabled=false})'
for name in "${disabled_outputs[@]}"; do eval_lua "hl.monitor({output=\"$name\",disabled=true})"; done
sleep .25
ctl -j monitors | python3 -c 'import json,sys; m=json.load(sys.stdin); assert len(m)==1 and m[0]["name"]=="COSMIC-CONFIG-TEST",m'
write_config bare

write_settings created 'return {idle_timeout=91,rendering={stars=127},effects={orbit=false},controls={preview=false}}'
eval_lua 'local s=require("cosmic").status(); assert(not s.config_file_loaded and s.config.idle_timeout==60)'
reload_config created
ready
eval_lua 'local s=require("cosmic").status(); assert(s.config_file_loaded and s.config_file_error==nil and s.error==nil and s.config.idle_timeout==91 and s.config.rendering.stars==127 and s.config.effects.orbit==false and s.config.effects.black_hole and s.config.controls.preview==false and s.config.rendering.background==1)'
pass 'creating a file does not silently reread; explicit reload merges valid partial settings'

write_settings edited 'return {idle_timeout=37,rendering={stars=211}}'
eval_lua 'local s=require("cosmic").status(); assert(s.config.idle_timeout==91 and s.config.rendering.stars==127)'
reload_config edited
ready
eval_lua 'local s=require("cosmic").status(); assert(s.config.idle_timeout==37 and s.config.rendering.stars==211 and s.config.effects.orbit and s.config.controls.preview=="F11")'
pass 'editing and reloading rereads once from defaults: 91/127 to 37/211'

eval_lua 'local c=require("cosmic"); assert(c.setup({idle_timeout=13,rendering={stars=55}})); local s=c.status(); assert(s.config.idle_timeout==13 and s.config.rendering.stars==55 and s.config_file_loaded)'
pass 'runtime setup takes priority over the loaded dedicated file'
write_config inline
reload_config inline
ready
eval_lua 'local s=require("cosmic").status(); assert(s.config_file_loaded and s.config.idle_timeout==19 and s.config.rendering.stars==31)'
pass 'inline setup takes priority during the real compositor configuration evaluation'

write_config bare
write_settings disabled 'return {enabled=false,idle_timeout=37,rendering={stars=211}}'
reload_config disabled
ready
eval_lua 'local s=require("cosmic").status(); assert(s.config_file_loaded and s.config.enabled==false and not s.enabled and not s.initialized and not s.active and s.input_watchers==0 and s.config_file_error==nil)'
pass 'enabled=false keeps the native scene and input watchers stopped'

check_invalid syntax 'return {idle_timeout='
check_invalid range 'return {idle_timeout=0}'
check_invalid return-type 'return 42'
check_invalid unknown-key 'return {unknown_setting=true}'
check_invalid runtime-error 'error("fixture-only runtime failure")'

write_settings repaired 'return {idle_timeout=73,rendering={stars=29}}'
reload_config repaired
ready
eval_lua 'local s=require("cosmic").status(); assert(s.config_file_loaded and s.config_file_error==nil and s.error==nil and s.initialized and s.input_watchers>0 and s.config.idle_timeout==73 and s.config.rendering.stars==29)'
pass 'correcting the file recovers initialization and clears the previous error'
mv -- "$settings" "$output/settings-removed.lua"
reload_config missing-again
ready
eval_lua 'local s=require("cosmic").status(); assert(not s.config_file_loaded and s.config_file_error==nil and s.error==nil and s.config.idle_timeout==60 and s.config.rendering.stars==240 and s.config.enabled and s.initialized and s.input_watchers>0)'
pass 'removing the test settings restores fresh defaults without retaining old values'

write_config removed
[[ $(ctl reload) == ok ]] || { printf 'Require-removal reload failed.\n' >&2; exit 1; }
wait_lua 'hl.plugin.cosmic==nil'
[[ $(ctl -j plugin list) == '[]' ]] || { printf 'Native plugin remained loaded after removing require.\n' >&2; exit 1; }
config_ok
pass 'removing require unloads the native plugin and leaves the compositor responsive'
printf '%s\n' 'PASS: real bare-require dedicated config loading, single-read semantics, create/edit/reload, defaults and setup priority, disabled state, contained syntax/range/type/runtime errors, recovery, missing defaults and native require-removal. No parent configuration, input or IME was changed.' | tee "$output/result.txt"
