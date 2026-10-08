#!/usr/bin/env bash
set -euo pipefail

repository=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
config=${HYPRLAND_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/hypr/hyprland.lua}
build_dir="$repository/build"
state_dir=""
instance=""
build=true
dry_run=false

usage() {
    printf '%s\n' 'Usage: scripts/update.sh --instance SIGNATURE [--config FILE] [--state-dir DIR]' \
        '                         [--build-dir DIR] [--no-build] [--dry-run]' \
        'Safely replace an owned installation, including older plugins with retained render passes.' \
        'The selected compositor is never restarted; desktop screencopy pixels are discarded.'
}
die() { printf 'Update: %s\n' "$*" >&2; exit 1; }
while (($#)); do
    case "$1" in
        --instance) instance=${2:?--instance requires a signature}; shift 2 ;;
        --config) config=${2:?--config requires a file}; shift 2 ;;
        --state-dir) state_dir=${2:?--state-dir requires a directory}; shift 2 ;;
        --build-dir) build_dir=${2:?--build-dir requires a directory}; shift 2 ;;
        --no-build) build=false; shift ;;
        --dry-run) dry_run=true; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "unknown argument: $1" ;;
    esac
done
[[ -n "$instance" && "$instance" != *$'\n'* ]] || die '--instance must explicitly name one exact Hyprland signature'
# Children, including the existing installer tools, must not inherit a parent
# instance selector. This changes only this script's environment, never its caller.
unset HYPRLAND_INSTANCE_SIGNATURE
for program in hyprctl python3 grim timeout realpath sha256sum rg; do
    command -v "$program" >/dev/null || die "required program is missing: $program"
done
for path in "$config" "$build_dir" "$state_dir"; do
    [[ "$path" != *$'\n'* ]] || die 'paths containing newlines are unsupported'
done
config=$(realpath -ms -- "$config")
build_dir=$(realpath -ms -- "$build_dir")
config_key=$(printf '%s' "$config" | sha256sum)
config_key=${config_key%% *}
state_dir=$(realpath -ms -- "${state_dir:-${XDG_STATE_HOME:-$HOME/.local/state}/hyprcosmos/$config_key}")
common=(--config "$config" --state-dir "$state_dir")
install_args=("${common[@]}" --build-dir "$build_dir" --no-build)
ctl() { timeout 15s env -u HYPRLAND_INSTANCE_SIGNATURE hyprctl -i "$instance" "$@"; }
selected_instance() {
    timeout 15s env -u HYPRLAND_INSTANCE_SIGNATURE hyprctl -j instances | python3 -c '
import json, re, sys
matches = [item for item in json.load(sys.stdin) if item.get("instance") == sys.argv[1]]
if len(matches) != 1:
    sys.exit("The exact selected instance is unavailable or ambiguous")
item = matches[0]
pid, socket = item.get("pid"), item.get("wl_socket")
if type(pid) is not int or pid <= 0 or not isinstance(socket, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+", socket):
    sys.exit("The selected instance has invalid PID/socket information")
print(pid, socket)
' "$instance"
}
identity=$(selected_instance) || die 'cannot resolve the explicitly selected instance; no files or runtime state changed'
read -r original_pid socket <<< "$identity"
same_instance() {
    local current
    current=$(selected_instance) || { printf 'Update: selected instance disappeared; no compositor restart was attempted\n' >&2; return 1; }
    [[ "$current" == "$identity" ]] || { printf 'Update: selected compositor PID/socket changed; refusing to continue\n' >&2; return 1; }
}

# Both existing tools must accept the exact paths and ownership before any
# runtime mutation. Their dry runs contact only the explicitly selected socket.
"$repository/scripts/install.sh" "${install_args[@]}" --dry-run --reload --instance "$instance"
"$repository/scripts/uninstall.sh" "${common[@]}" --dry-run --reload --instance "$instance"
[[ -f "$state_dir/installation" ]] || die 'there is no owned installation to update'
mapfile -t ownership < "$state_dir/installation"
[[ ${#ownership[@]} -eq 9 && ${ownership[8]} == true ]] || die 'the require is user-owned; preserving it and refusing automatic unload'
real_config=$(realpath -e -- "$config")
rg -q '^-- hyprcosmos:begin$' "$real_config" || die 'the owned require block is missing; refusing automatic unload'
if awk '/^-- hyprcosmos:begin$/ { skip=1; next } /^-- hyprcosmos:end$/ { skip=0; next } !skip { print }' "$real_config" |
    rg -q '^[^-]*require[[:space:]]*(\([[:space:]]*)?["\x27]cosmic["\x27]'; then
    die 'an additional custom require exists; preserving it and refusing automatic unload'
fi
if "$dry_run"; then
    printf 'Dry run: selected PID=%s socket=%s; build=%s; disable -> discarded frame barrier -> unload -> install -> verify. No writes or disable performed.\n' \
        "$original_pid" "$socket" "$build"
    exit 0
fi
if "$build"; then "$repository/scripts/build.sh" --build-dir "$build_dir"; fi
[[ -f "$build_dir/cosmic.so" ]] || die 'replacement plugin is missing; no runtime state changed'
same_instance || die 'instance changed before runtime mutation'

disabled=false
reenable_hint() {
    if "$disabled"; then
        printf 'Cosmic was disabled safely. If the old native API remains loaded, it can be re-enabled explicitly with:\n' >&2
        printf '  hyprctl -i %q eval %q\n' "$instance" 'local api=hl.plugin.cosmic; assert(api); assert(api.enable())' >&2
    fi
}
result=$(ctl repl 'local api=hl.plugin.cosmic; if api then assert(api.disable()); local s=api.status(); assert(not s.active and not s.initialized, "Cosmic did not stop") end; print("COSMIC_UPDATE_DISABLED")') || die 'native disable request failed; no unload attempted'
[[ "$result" == COSMIC_UPDATE_DISABLED ]] || die "native disable assertion failed; no unload attempted: $result"
disabled=true

# An executed custom pass can outlive its frame in old Hyprland plugins. Finish
# a new compositor frame with the old ELF still loaded, destroying that retained
# pass before uninstall's reload reaches dlclose. Never save desktop pixels.
monitor_names=$(ctl -j monitors | python3 -c '
import json, sys
outputs = json.load(sys.stdin)
if not isinstance(outputs, list) or not outputs:
    sys.exit("No active outputs; a completed frame cannot be guaranteed")
for output in outputs:
    name = output.get("name")
    if output.get("dpmsStatus") is not True:
        sys.exit("An output is asleep; wake all outputs before updating")
    if not isinstance(name, str) or not name or "\n" in name or "\0" in name:
        sys.exit("An output has an invalid name")
    print(name)
') || { reenable_hint; die 'output preflight failed; no unload or configuration edit attempted'; }
mapfile -t monitors <<< "$monitor_names"
for monitor in "${monitors[@]}"; do
    if ! timeout 15s env -u HYPRLAND_INSTANCE_SIGNATURE WAYLAND_DISPLAY="$socket" grim -o "$monitor" -t ppm - >/dev/null; then
        reenable_hint
        die "completed frame barrier failed on $monitor; no unload or configuration edit attempted"
    fi
done
same_instance || { reenable_hint; die 'instance changed before unload'; }

restore_owned_config() {
    printf 'Restoring owned links, require block and installation record without reloading.\n' >&2
    if ! "$repository/scripts/install.sh" "${install_args[@]}"; then
        printf 'Automatic restoration failed; configuration backups remain available. No restart was attempted.\n' >&2
        return 1
    fi
    printf 'Owned installation restored; no reload performed.\n' >&2
}
if ! "$repository/scripts/uninstall.sh" "${common[@]}" --reload --instance "$instance"; then
    restore_owned_config || true
    reenable_hint
    die 'unload failed; restoration attempted without another reload'
fi
same_instance || { restore_owned_config || true; die 'instance changed after unload; restoration attempted without reloading'; }
result=$(ctl repl 'assert(hl.plugin.cosmic == nil, "old native API remained loaded"); print("COSMIC_UPDATE_UNLOADED")') || {
    restore_owned_config || true; die 'cannot verify old plugin removal; restoration attempted without reloading';
}
[[ "$result" == COSMIC_UPDATE_UNLOADED ]] || {
    restore_owned_config || true; reenable_hint; die 'old native API is still present; refusing replacement reload';
}
if ! "$repository/scripts/install.sh" "${install_args[@]}" --reload --instance "$instance"; then
    restore_owned_config || true
    die 'replacement installation/reload failed; restoration attempted without another reload'
fi
same_instance || die 'instance changed after replacement reload; no further reload attempted'
errors=$(ctl configerrors) || die 'cannot read selected compositor configuration errors'
[[ -z "$errors" || "$errors" == ok ]] || die "selected configuration has errors: $errors"
accepted=false
verify_deadline=$((SECONDS + 8))
while ((SECONDS < verify_deadline)); do
    if [[ $(ctl repl 'if hl.plugin.cosmic then local s=require("cosmic").status(); if s.module_initialized and not s.error and s.config and ((s.config.enabled and s.initialized and s.enabled) or (not s.config.enabled and not s.initialized and not s.enabled)) then print("COSMIC_UPDATE_READY") end end' 2>/dev/null) == COSMIC_UPDATE_READY ]]; then
        accepted=true
        break
    fi
    sleep .1
done
"$accepted" || die 'replacement native API did not accept module setup; inspect status before further reloads'
same_instance || die 'instance changed during verification; no further reload attempted'
printf 'Updated selected instance %s without restarting PID %s. Native setup accepted; configuration errors: none.\n' "$instance" "$original_pid"
