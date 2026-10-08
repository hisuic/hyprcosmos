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
for program in cmake ctest pkg-config sha256sum; do
    command -v "$program" >/dev/null || { printf 'Required program is missing: %s\n' "$program" >&2; exit 1; }
done
pkg-config --exists hyprland || { printf 'Install development headers for your exact Hyprland build.\n' >&2; exit 1; }
printf 'Building Cosmic against Hyprland headers %s\n' "$(pkg-config --modversion hyprland)"
cmake -S "$repository" -B "$build_dir" -DCMAKE_BUILD_TYPE=RelWithDebInfo "${cmake_arguments[@]}"
cmake --build "$build_dir" --parallel "$jobs"
if "$run_tests"; then ctest --test-dir "$build_dir" --output-on-failure; fi
build_dir=$(realpath -- "$build_dir")
linked_plugin="$build_dir/link/cosmic.so"
[[ -f "$linked_plugin" ]] || { printf 'Expected linker output was not produced: %s\n' "$linked_plugin" >&2; exit 1; }

# A loaded ELF object must never be truncated by the next linker invocation.
# CMake writes only link/cosmic.so; published objects have immutable names and
# the public path changes through a single atomic symlink rename.
artifact_dir="$build_dir/artifacts"
mkdir -p -- "$artifact_dir"
artifact_tmp=$(mktemp "$artifact_dir/.cosmic.XXXXXX")
publication_dir=""
cleanup() {
    if [[ -n ${artifact_tmp:-} && -f "$artifact_tmp" ]]; then rm -- "$artifact_tmp"; fi
    if [[ -n ${publication_dir:-} && -d "$publication_dir" ]]; then
        if [[ -L "$publication_dir/cosmic.so" ]]; then rm -- "$publication_dir/cosmic.so"; fi
        rmdir -- "$publication_dir"
    fi
}
trap cleanup EXIT
cp -- "$linked_plugin" "$artifact_tmp"
digest=$(sha256sum -- "$artifact_tmp")
digest=${digest%% *}
artifact="$artifact_dir/cosmic-$digest.so"
if [[ -e "$artifact" || -L "$artifact" ]]; then
    [[ -f "$artifact" && ! -L "$artifact" ]] && cmp -s -- "$artifact_tmp" "$artifact" || {
        printf 'Refusing to replace an inconsistent published artifact: %s\n' "$artifact" >&2; exit 1;
    }
else
    chmod 0444 -- "$artifact_tmp"
    # A hard link publishes a fully written object without overwriting a
    # concurrent publisher's existing artifact with the same digest.
    if ! ln -- "$artifact_tmp" "$artifact"; then
        [[ -f "$artifact" && ! -L "$artifact" ]] && cmp -s -- "$artifact_tmp" "$artifact" || {
            printf 'Could not publish immutable artifact: %s\n' "$artifact" >&2; exit 1;
        }
    fi
fi
rm -- "$artifact_tmp"
artifact_tmp=""
publication_dir=$(mktemp -d "$build_dir/.cosmic-publish.XXXXXX")
ln -s -- "artifacts/cosmic-$digest.so" "$publication_dir/cosmic.so"
mv -Tf -- "$publication_dir/cosmic.so" "$build_dir/cosmic.so"
rmdir -- "$publication_dir"
publication_dir=""
printf 'Plugin ready: %s/cosmic.so\nImmutable artifact: %s\n' "$build_dir" "$artifact"
