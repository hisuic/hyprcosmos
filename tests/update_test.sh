#!/usr/bin/env bash
set -euo pipefail

# This file also supplies executable command mocks through sandbox symlinks.
# No invocation below can reach a real Hyprland or Wayland socket.
case "$(basename -- "$0")" in
    hyprctl)
        [[ ! ${HYPRLAND_INSTANCE_SIGNATURE+x} ]] || { printf 'Parent instance environment leaked\n' >&2; exit 1; }
        if [[ "$*" == '-j instances' ]]; then
            pid=1111
            if [[ ${MOCK_CHANGED_PID:-false} == true && -f "$MOCK_PHASE" && $(< "$MOCK_PHASE") == loaded ]]; then pid=3333; fi
            printf '[{"instance":"selected-signature","pid":%s,"wl_socket":"wayland-selected"},{"instance":"parent-signature","pid":2222,"wl_socket":"wayland-parent"}]\n' "$pid"
            exit 0
        fi
        [[ ${1:-} == -i && ${2:-} == selected-signature ]] || { printf 'Wrong or implicit instance selected\n' >&2; exit 1; }
        shift 2
        case "${1:-}" in
            -j)
                case "${2:-}" in
                    version) printf 'version\n' >> "$MOCK_LOG"; printf '{"version":"fixture"}\n' ;;
                    monitors)
                        case "${MOCK_OUTPUTS:-awake}" in
                            awake) printf '[{"name":"OUTPUT-A","dpmsStatus":true},{"name":"OUTPUT-B","dpmsStatus":true}]\n' ;;
                            asleep) printf '[{"name":"OUTPUT-A","dpmsStatus":false}]\n' ;;
                            none) printf '[]\n' ;;
                        esac ;;
                    *) exit 1 ;;
                esac ;;
            repl)
                if [[ ${2:-} == *COSMIC_UPDATE_DISABLED* ]]; then
                    printf 'disable\n' >> "$MOCK_LOG"
                    if [[ ${MOCK_DISABLE_FAIL:-false} == true ]]; then printf 'disable assertion failed\n'; else printf 'COSMIC_UPDATE_DISABLED\n'; fi
                elif [[ ${2:-} == *COSMIC_UPDATE_READY* ]]; then
                    printf 'verify\n' >> "$MOCK_LOG"; printf 'COSMIC_UPDATE_READY\n'
                elif [[ ${2:-} == *COSMIC_UPDATE_UNLOADED* ]]; then
                    printf 'old_api_gone\n' >> "$MOCK_LOG"
                    if [[ ${MOCK_UNLOAD_RETAINS_API:-false} == true ]]; then printf 'old native API remained loaded\n'; else printf 'COSMIC_UPDATE_UNLOADED\n'; fi
                else exit 1; fi ;;
            reload)
                if rg -q '^require\("cosmic"\)$' "$MOCK_CONFIG"; then
                    printf 'load\n' >> "$MOCK_LOG"; printf 'loaded\n' > "$MOCK_PHASE"
                    [[ ${MOCK_LOAD_FAIL:-false} != true ]] || exit 1
                else printf 'unload\n' >> "$MOCK_LOG"; printf 'unloaded\n' > "$MOCK_PHASE"; fi
                printf 'ok\n' ;;
            configerrors) printf 'configerrors\n' >> "$MOCK_LOG"; printf '%s\n' "${MOCK_CONFIG_ERRORS:-ok}" ;;
            *) exit 1 ;;
        esac
        exit 0 ;;
    grim)
        [[ ! ${HYPRLAND_INSTANCE_SIGNATURE+x} && ${WAYLAND_DISPLAY:-} == wayland-selected ]] || { printf 'Parent Wayland environment leaked\n' >&2; exit 1; }
        [[ ${1:-} == -o && ${3:-} == -t && ${4:-} == ppm && ${5:-} == - ]] || exit 1
        printf 'frame:%s\n' "$2" >> "$MOCK_LOG"
        [[ ${MOCK_FRAME_FAIL:-false} != true ]] || exit 1
        printf 'PRIVATE_PIXEL_FIXTURE_DO_NOT_LOG\n'
        exit 0 ;;
    build.sh)
        printf 'build\n' >> "$MOCK_LOG"
        exit 0 ;;
esac

repository=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
sandbox=$(mktemp -d /tmp/hyprcosmos-update-test.XXXXXX)
trap 'if [[ "$sandbox" == /tmp/hyprcosmos-update-test.* && -d "$sandbox" ]]; then rm -rf -- "$sandbox"; fi' EXIT
fixture="$sandbox/repository"
mkdir -p -- "$fixture/scripts" "$fixture/lua/cosmic" "$fixture/examples" "$fixture/build" "$sandbox/bin"
cp -- "$repository/scripts/update.sh" "$repository/scripts/install.sh" "$repository/scripts/uninstall.sh" "$fixture/scripts/"
cp -- "$repository/lua/cosmic/init.lua" "$fixture/lua/cosmic/init.lua"
cp -- "$repository/examples/hyprcosmos.lua" "$fixture/examples/hyprcosmos.lua"
printf 'non-loadable updater test fixture\n' > "$fixture/build/cosmic.so"
for program in hyprctl grim; do ln -s -- "$repository/tests/update_test.sh" "$sandbox/bin/$program"; done
ln -s -- "$repository/tests/update_test.sh" "$fixture/scripts/build.sh"
export PATH="$sandbox/bin:$PATH" HYPRLAND_INSTANCE_SIGNATURE=parent-signature WAYLAND_DISPLAY=wayland-parent
export MOCK_LOG="$sandbox/events" MOCK_PHASE="$sandbox/phase"
checks=0
cases=0
fail() { printf 'Updater test failed: %s\n' "$*" >&2; exit 1; }
check() { checks=$((checks+1)); "$@" || fail "check $checks: $*"; }
expect_failure() {
    checks=$((checks+1))
    if "$@"; then fail "unexpected success: $*"; fi
}
fresh_install() {
    cases=$((cases+1))
    config_dir="$sandbox/case-$cases/hypr"
    state_dir="$sandbox/case-$cases/state"
    mkdir -p -- "$config_dir"
    config="$config_dir/hyprland.lua"
    printf -- '-- unrelated configuration; must be preserved\n' > "$config"
    "$fixture/scripts/install.sh" --config "$config" --state-dir "$state_dir" --no-build > "$sandbox/install.log"
    printf 'return { idle_timeout = 87, rendering = { stars = 17 } }\n' > "$config_dir/hyprcosmos.lua"
    chmod 640 -- "$config_dir/hyprcosmos.lua"
    cp -- "$config_dir/hyprcosmos.lua" "$sandbox/settings-before"
    settings_inode=$(stat -c %i -- "$config_dir/hyprcosmos.lua")
    cp -- "$config" "$sandbox/before"
    cp -- "$state_dir/installation" "$sandbox/ownership-before"
    export MOCK_CONFIG="$config" MOCK_OUTPUTS=awake MOCK_FRAME_FAIL=false MOCK_DISABLE_FAIL=false MOCK_LOAD_FAIL=false MOCK_CHANGED_PID=false MOCK_CONFIG_ERRORS=ok MOCK_UNLOAD_RETAINS_API=false
    printf '' > "$MOCK_LOG"
    printf 'original\n' > "$MOCK_PHASE"
}
update_fixture() {
    "$fixture/scripts/update.sh" --config "$config" --state-dir "$state_dir" "$@" > "$sandbox/update.log" 2>&1
}
unchanged_installation() {
    check cmp -s "$config" "$sandbox/before"
    check cmp -s "$state_dir/installation" "$sandbox/ownership-before"
    check test -L "$config_dir/cosmic"
    check test -L "$config_dir/cosmic.so"
    check cmp -s "$config_dir/hyprcosmos.lua" "$sandbox/settings-before"
    check test "$(stat -c %i -- "$config_dir/hyprcosmos.lua")" = "$settings_inode"
    check test "$(stat -c %a -- "$config_dir/hyprcosmos.lua")" = 640
}
no_transition() {
    checks=$((checks+1))
    if rg -q '^(disable|frame:|unload|load|build)' "$MOCK_LOG"; then fail 'unexpected runtime mutation'; fi
}
no_unload() {
    checks=$((checks+1))
    if rg -q '^(unload|load)' "$MOCK_LOG"; then fail 'unload/reload occurred before a successful frame barrier'; fi
}

fresh_install
update_fixture --instance selected-signature --dry-run || { sed -n '1,160p' "$sandbox/update.log" >&2; fail 'dry-run preflight'; }
unchanged_installation
no_transition
check rg -q 'No writes or disable performed' "$sandbox/update.log"
expect_failure update_fixture --no-build
unchanged_installation
no_transition
expect_failure update_fixture --instance wrong-signature --no-build
unchanged_installation
no_transition

fresh_install
export MOCK_FRAME_FAIL=true
expect_failure update_fixture --instance selected-signature --no-build
unchanged_installation
no_unload
check rg -q '^disable$' "$MOCK_LOG"
check rg -q '^frame:OUTPUT-A$' "$MOCK_LOG"
check rg -q 're-enabled explicitly' "$sandbox/update.log"

for outputs in asleep none; do
    fresh_install
    export MOCK_OUTPUTS="$outputs"
    expect_failure update_fixture --instance selected-signature --no-build
    unchanged_installation
    no_unload
    checks=$((checks+1))
    if rg -q '^frame:' "$MOCK_LOG"; then fail 'frame requested for unavailable outputs'; fi
done

fresh_install
export MOCK_DISABLE_FAIL=true
expect_failure update_fixture --instance selected-signature --no-build
unchanged_installation
no_unload

fresh_install
update_fixture --instance selected-signature --no-build || { sed -n '1,160p' "$sandbox/update.log" >&2; fail 'successful replacement'; }
unchanged_installation
check rg -q 'without restarting PID 1111' "$sandbox/update.log"
sequence=$(awk '/^(disable|frame:|unload|load|verify)/ { print }' "$MOCK_LOG")
check test "$sequence" = $'disable\nframe:OUTPUT-A\nframe:OUTPUT-B\nunload\nload\nverify'
checks=$((checks+1))
if rg -q 'PRIVATE_PIXEL_FIXTURE_DO_NOT_LOG' "$sandbox/update.log" "$MOCK_LOG"; then fail 'desktop pixels were retained or printed'; fi
check test "$HYPRLAND_INSTANCE_SIGNATURE" = parent-signature
check test "$WAYLAND_DISPLAY" = wayland-parent
check test "$(rg -c '^old_api_gone$' "$MOCK_LOG")" = 1

fresh_install
update_fixture --instance selected-signature
unchanged_installation
check rg -q '^build$' "$MOCK_LOG"
sequence=$(awk '/^(build|disable|frame:|unload|load|verify)/ { print }' "$MOCK_LOG")
check test "$sequence" = $'build\ndisable\nframe:OUTPUT-A\nframe:OUTPUT-B\nunload\nload\nverify'

# User settings may deliberately live elsewhere through a readable symlink.
fresh_install
settings_target="$sandbox/external-settings.lua"
cp -- "$sandbox/settings-before" "$settings_target"
chmod 400 -- "$settings_target"
settings_target_inode=$(stat -c %i -- "$settings_target")
rm -- "$config_dir/hyprcosmos.lua"
ln -s -- "$settings_target" "$config_dir/hyprcosmos.lua"
update_fixture --instance selected-signature --no-build
check cmp -s "$config" "$sandbox/before"
check cmp -s "$state_dir/installation" "$sandbox/ownership-before"
check test -L "$config_dir/hyprcosmos.lua"
check test "$(readlink -- "$config_dir/hyprcosmos.lua")" = "$settings_target"
check cmp -s "$settings_target" "$sandbox/settings-before"
check test "$(stat -c %i -- "$settings_target")" = "$settings_target_inode"
check test "$(stat -c %a -- "$settings_target")" = 400

fresh_install
export MOCK_LOAD_FAIL=true
expect_failure update_fixture --instance selected-signature --no-build
unchanged_installation
check test "$(rg -c '^load$' "$MOCK_LOG")" = 1
check rg -q 'without reloading' "$sandbox/update.log"

fresh_install
export MOCK_CHANGED_PID=true
expect_failure update_fixture --instance selected-signature --no-build
unchanged_installation
check rg -q 'PID/socket changed' "$sandbox/update.log"

fresh_install
export MOCK_CONFIG_ERRORS='fixture configuration error'
expect_failure update_fixture --instance selected-signature --no-build
unchanged_installation
check rg -q 'configuration has errors' "$sandbox/update.log"

fresh_install
export MOCK_UNLOAD_RETAINS_API=true
expect_failure update_fixture --instance selected-signature --no-build
unchanged_installation
check test "$(rg -c '^unload$' "$MOCK_LOG")" = 1
checks=$((checks+1))
if rg -q '^load$' "$MOCK_LOG"; then fail 'replacement loaded without confirming old API removal'; fi
check rg -q 'refusing replacement reload' "$sandbox/update.log"

fresh_install
printf 'local additional = require("cosmic")\n' >> "$config"
cp -- "$config" "$sandbox/before"
expect_failure update_fixture --instance selected-signature --no-build
unchanged_installation
no_transition
check rg -q 'additional custom require' "$sandbox/update.log"

# A custom/user-owned require must never be removed or replaced by the updater.
cases=$((cases+1))
config_dir="$sandbox/case-$cases/hypr"
state_dir="$sandbox/case-$cases/state"
mkdir -p -- "$config_dir"
config="$config_dir/hyprland.lua"
printf 'local cosmic = require("cosmic")\ncosmic.setup({ idle_timeout = 9 })\n' > "$config"
"$fixture/scripts/install.sh" --config "$config" --state-dir "$state_dir" --no-build > "$sandbox/install.log"
printf 'return { idle_timeout = 87, rendering = { stars = 17 } }\n' > "$config_dir/hyprcosmos.lua"
chmod 640 -- "$config_dir/hyprcosmos.lua"
cp -- "$config_dir/hyprcosmos.lua" "$sandbox/settings-before"
settings_inode=$(stat -c %i -- "$config_dir/hyprcosmos.lua")
cp -- "$config" "$sandbox/before"
cp -- "$state_dir/installation" "$sandbox/ownership-before"
export MOCK_CONFIG="$config" MOCK_CHANGED_PID=false MOCK_LOAD_FAIL=false
printf '' > "$MOCK_LOG"
printf 'original\n' > "$MOCK_PHASE"
expect_failure update_fixture --instance selected-signature --no-build
unchanged_installation
no_transition
check rg -q 'require is user-owned' "$sandbox/update.log"
printf 'Updater lifecycle: %s checks passed (isolated command mocks; no live socket contacted)\n' "$checks"
