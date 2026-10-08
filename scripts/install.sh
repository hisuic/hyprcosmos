#!/usr/bin/env bash
set -euo pipefail

repository=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
config=${HYPRLAND_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/hypr/hyprland.lua}
build_dir="$repository/build"
state_dir=""
instance=""
build=true
reload=false
dry_run=false

usage() {
    printf '%s\n' 'Usage: scripts/install.sh [--config FILE] [--build-dir DIR] [--state-dir DIR]' \
        '                          [--no-build] [--dry-run] [--reload --instance SIGNATURE]' \
        'The module and plugin remain in this repository; only owned symlinks and a require block are installed.'
}
die() { printf 'Install: %s\n' "$*" >&2; exit 1; }

while (($#)); do
    case "$1" in
        --config) config=${2:?--config requires a file}; shift 2 ;;
        --build-dir) build_dir=${2:?--build-dir requires a directory}; shift 2 ;;
        --state-dir) state_dir=${2:?--state-dir requires a directory}; shift 2 ;;
        --instance) instance=${2:?--instance requires a signature}; shift 2 ;;
        --no-build) build=false; shift ;;
        --reload) reload=true; shift ;;
        --dry-run) dry_run=true; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "unknown argument: $1" ;;
    esac
done

for path in "$config" "$build_dir" "$state_dir"; do
    [[ "$path" != *$'\n'* ]] || die 'paths containing newlines are unsupported'
done
config=$(realpath -ms -- "$config")
[[ -f "$config" ]] || die "configuration does not exist: $config"
real_config=$(realpath -e -- "$config")
config_dir=$(dirname -- "$config")
build_dir=$(realpath -ms -- "$build_dir")
module_link="$config_dir/cosmic"
module_target="$repository/lua/cosmic"
plugin_link="$config_dir/cosmic.so"
plugin_target="$build_dir/cosmic.so"
config_key=$(printf '%s' "$config" | sha256sum)
config_key=${config_key%% *}
state_dir=${state_dir:-${XDG_STATE_HOME:-$HOME/.local/state}/hyprcosmos/$config_key}
state_dir=$(realpath -ms -- "$state_dir")
manifest="$state_dir/installation"
owned_block=false

if [[ -e "$manifest" ]]; then
    mapfile -t ownership < "$manifest"
    [[ ${#ownership[@]} -eq 9 && ${ownership[0]} == hyprcosmos-v1 &&
        ${ownership[1]} == "$repository" && ${ownership[2]} == "$config" &&
        ${ownership[3]} == "$real_config" && ${ownership[4]} == "$module_link" &&
        ${ownership[5]} == "$module_target" && ${ownership[6]} == "$plugin_link" &&
        ${ownership[7]} == "$plugin_target" ]] || die "installation record differs; uninstall it before changing paths: $manifest"
    owned_block=${ownership[8]}
fi

check_link() {
    local link=$1 target=$2
    if [[ -L "$link" ]]; then
        [[ $(readlink -- "$link") == "$target" ]] || die "refusing to replace an unrelated symlink: $link"
    elif [[ -e "$link" ]]; then
        die "refusing to overwrite an existing file/directory: $link"
    fi
}
check_link "$module_link" "$module_target"
check_link "$plugin_link" "$plugin_target"
[[ -f "$module_target/init.lua" ]] || die 'Lua entry point is missing from this checkout'

markers=$(awk '/^-- hyprcosmos:begin$/ { begin++ } /^-- hyprcosmos:end$/ { end++ } END { print begin+0, end+0 }' "$real_config")
[[ "$markers" == '0 0' || "$markers" == '1 1' ]] || die 'configuration has malformed or duplicated hyprcosmos markers'
append=false
if [[ "$markers" == '0 0' ]] && ! rg -q '^[^-]*require[[:space:]]*\([[:space:]]*["\x27]cosmic["\x27][[:space:]]*\)' "$real_config"; then
    append=true
    owned_block=true
fi
if "$reload"; then
    [[ -n "$instance" ]] || die '--reload requires --instance; the current/nested session is never selected implicitly'
    command -v hyprctl >/dev/null || die 'hyprctl is missing'
    hyprctl -i "$instance" -j version >/dev/null || die "cannot contact the explicitly selected instance: $instance"
fi

printf 'Configuration: %s (edits target %s)\nLua link: %s -> %s\nPlugin link: %s -> %s\n' \
    "$config" "$real_config" "$module_link" "$module_target" "$plugin_link" "$plugin_target"
if "$dry_run"; then
    printf 'Dry run: build=%s, append require=%s, reload=%s; no files changed.\n' "$build" "$append" "$reload"
    exit 0
fi
if "$build"; then "$repository/scripts/build.sh" --build-dir "$build_dir"; fi
[[ -f "$plugin_target" ]] || die "plugin is missing; run scripts/build.sh first: $plugin_target"
[[ -w "$real_config" && -w "$config_dir" ]] || die 'configuration or module directory is not writable'

# Record ownership before changing anything, so a failed/partial installation
# remains recoverable with uninstall.sh. This file is data, never sourced.
mkdir -p -- "$state_dir"
chmod 700 -- "$state_dir"
manifest_tmp=$(mktemp "$state_dir/.installation.XXXXXX")
trap 'if [[ -n ${manifest_tmp:-} && -f "$manifest_tmp" ]]; then rm -- "$manifest_tmp"; fi' EXIT
printf '%s\n' hyprcosmos-v1 "$repository" "$config" "$real_config" "$module_link" \
    "$module_target" "$plugin_link" "$plugin_target" "$owned_block" > "$manifest_tmp"
chmod 600 -- "$manifest_tmp"
mv -- "$manifest_tmp" "$manifest"
manifest_tmp=""
if [[ ! -L "$module_link" ]]; then ln -s -- "$module_target" "$module_link"; fi
if [[ ! -L "$plugin_link" ]]; then ln -s -- "$plugin_target" "$plugin_link"; fi
if "$append"; then
    backup=$(mktemp "${real_config}.hyprcosmos-backup.$(date +%Y%m%dT%H%M%S).XXXXXX")
    cp -p -- "$real_config" "$backup"
    printf '\n-- hyprcosmos:begin\nrequire("cosmic")\n-- hyprcosmos:end\n' >> "$real_config"
    printf 'Configuration backup: %s\n' "$backup"
fi
if "$reload"; then hyprctl -i "$instance" reload; fi
printf 'Installed. %s\n' "$([[ "$reload" == true ]] && printf 'Selected session reloaded.' || printf 'Reload your intended Hyprland instance when ready.')"
