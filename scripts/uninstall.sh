#!/usr/bin/env bash
set -euo pipefail

config=${HYPRLAND_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/hypr/hyprland.lua}
state_dir=""
instance=""
reload=false
dry_run=false

usage() {
    printf '%s\n' 'Usage: scripts/uninstall.sh [--config FILE] [--state-dir DIR]' \
        '                            [--dry-run] [--reload --instance SIGNATURE]' \
        'Removes only recorded symlinks and the exact require block created by install.sh; backups and builds remain.'
}
die() { printf 'Uninstall: %s\n' "$*" >&2; exit 1; }

while (($#)); do
    case "$1" in
        --config) config=${2:?--config requires a file}; shift 2 ;;
        --state-dir) state_dir=${2:?--state-dir requires a directory}; shift 2 ;;
        --instance) instance=${2:?--instance requires a signature}; shift 2 ;;
        --reload) reload=true; shift ;;
        --dry-run) dry_run=true; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "unknown argument: $1" ;;
    esac
done
for path in "$config" "$state_dir"; do
    [[ "$path" != *$'\n'* ]] || die 'paths containing newlines are unsupported'
done
config=$(realpath -ms -- "$config")
config_key=$(printf '%s' "$config" | sha256sum)
config_key=${config_key%% *}
state_dir=${state_dir:-${XDG_STATE_HOME:-$HOME/.local/state}/hyprcosmos/$config_key}
state_dir=$(realpath -ms -- "$state_dir")
manifest="$state_dir/installation"
if [[ ! -f "$manifest" ]]; then printf 'No recorded Cosmic installation for %s; nothing changed.\n' "$config"; exit 0; fi
mapfile -t ownership < "$manifest"
[[ ${#ownership[@]} -eq 9 && ${ownership[0]} == hyprcosmos-v1 && ${ownership[2]} == "$config" ]] || die "invalid ownership record: $manifest"
real_config=${ownership[3]}
module_link=${ownership[4]}
module_target=${ownership[5]}
plugin_link=${ownership[6]}
plugin_target=${ownership[7]}
owned_block=${ownership[8]}
[[ "$owned_block" == true || "$owned_block" == false ]] || die 'invalid block ownership flag'
[[ "$module_link" == "$(dirname -- "$config")/cosmic" &&
    "$plugin_link" == "$(dirname -- "$config")/cosmic.so" ]] || die 'ownership record has unexpected link paths'

check_link() {
    local link=$1 target=$2
    if [[ -L "$link" ]]; then
        [[ $(readlink -- "$link") == "$target" ]] || die "symlink was changed; preserving it and stopping: $link"
    elif [[ -e "$link" ]]; then
        die "installed link was replaced by another file; preserving it and stopping: $link"
    fi
}
check_link "$module_link" "$module_target"
check_link "$plugin_link" "$plugin_target"
remove_block=false
if "$owned_block" && [[ -e "$config" ]]; then
    [[ $(realpath -e -- "$config") == "$real_config" ]] || die 'configuration symlink was retargeted; preserving the new target'
    # A user may have deleted the line already. An edited marked block is
    # preserved, since its extra contents are no longer installer-owned.
    if ! awk '
        /^-- hyprcosmos:begin$/ { if (state != 0 || seen++) exit 1; state=1; next }
        /^-- hyprcosmos:end$/ { if (state != 2) exit 1; state=0; next }
        state == 1 { if ($0 != "require(\"cosmic\")") exit 1; state=2; next }
        state == 2 { exit 1 }
        END { if (state != 0) exit 1 }
    ' "$real_config"; then die 'the marked require block was edited; preserving it for manual review'; fi
    if rg -q '^-- hyprcosmos:begin$' "$real_config"; then remove_block=true; fi
fi
if "$reload"; then
    [[ -n "$instance" ]] || die '--reload requires --instance; current/nested sessions are never selected implicitly'
    command -v hyprctl >/dev/null || die 'hyprctl is missing'
    hyprctl -i "$instance" -j version >/dev/null || die "cannot contact the explicitly selected instance: $instance"
fi
printf 'Remove recorded links: %s, %s\nRemove owned require block: %s\n' "$module_link" "$plugin_link" "$remove_block"
if "$dry_run"; then printf 'Dry run; no files changed.\n'; exit 0; fi

config_tmp=""
trap 'if [[ -n ${config_tmp:-} && -f "$config_tmp" ]]; then rm -- "$config_tmp"; fi' EXIT
if "$remove_block"; then
    backup=$(mktemp "${real_config}.hyprcosmos-backup.$(date +%Y%m%dT%H%M%S).XXXXXX")
    cp -p -- "$real_config" "$backup"
    config_tmp=$(mktemp "${real_config}.hyprcosmos-edit.XXXXXX")
    cp -p -- "$real_config" "$config_tmp"
    awk '/^-- hyprcosmos:begin$/ { skip=1; next } /^-- hyprcosmos:end$/ { skip=0; next } !skip { print }' \
        "$real_config" > "$config_tmp"
    mv -- "$config_tmp" "$real_config"
    config_tmp=""
    printf 'Configuration backup: %s\n' "$backup"
fi
if [[ -L "$module_link" ]]; then rm -- "$module_link"; fi
if [[ -L "$plugin_link" ]]; then rm -- "$plugin_link"; fi
rm -- "$manifest"
rmdir -- "$state_dir" 2>/dev/null || true
if "$reload"; then hyprctl -i "$instance" reload; fi
printf 'Removed owned symlinks, installation record and require block (when present). Backups and repository artifacts remain recoverable.\n'
if ! "$reload"; then printf 'Reload the intended Hyprland instance to stop and unload its plugin.\n'; fi
