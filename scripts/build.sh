#!/usr/bin/env bash
set -euo pipefail

repository=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
build_dir="$repository/build"
jobs=${HYPRCOSMOS_BUILD_JOBS:-2}
run_tests=true
cmake_arguments=()

usage() {
    printf '%s\n' 'Usage: scripts/build.sh [--build-dir DIR] [--jobs N] [--no-tests] [--cmake-arg ARG]'
}

while (($#)); do
    case "$1" in
        --build-dir) build_dir=${2:?--build-dir requires a directory}; shift 2 ;;
        --jobs) jobs=${2:?--jobs requires a count}; shift 2 ;;
        --no-tests) run_tests=false; shift ;;
        --cmake-arg) cmake_arguments+=("${2:?--cmake-arg requires an argument}"); shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { printf 'Jobs must be a positive integer.\n' >&2; exit 2; }
for program in cmake ctest pkg-config; do
    command -v "$program" >/dev/null || { printf 'Required program is missing: %s\n' "$program" >&2; exit 1; }
done
pkg-config --exists hyprland || { printf 'Install development headers for your exact Hyprland build.\n' >&2; exit 1; }
printf 'Building Cosmic against Hyprland headers %s\n' "$(pkg-config --modversion hyprland)"
cmake -S "$repository" -B "$build_dir" -DCMAKE_BUILD_TYPE=RelWithDebInfo "${cmake_arguments[@]}"
cmake --build "$build_dir" --parallel "$jobs"
if "$run_tests"; then ctest --test-dir "$build_dir" --output-on-failure; fi
[[ -f "$build_dir/cosmic.so" ]] || { printf 'Expected plugin was not produced: %s/cosmic.so\n' "$build_dir" >&2; exit 1; }
printf 'Plugin ready: %s/cosmic.so\n' "$(realpath -- "$build_dir")"
