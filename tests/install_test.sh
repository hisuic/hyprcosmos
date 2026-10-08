#!/usr/bin/env bash
set -euo pipefail

# Only the atomic settings publication is intercepted by this offline mock.
if [[ $(basename -- "$0") == ln ]]; then
    if [[ ${1:-} == -T && ${4:-} == "${MOCK_SETTINGS_FILE:-}" ]]; then
        case "${MOCK_SETTINGS_RACE:-}" in
            regular)
                printf 'return { idle_timeout = 137, rendering = { stars = 3 } }\n' > "$MOCK_SETTINGS_FILE"
                chmod 640 -- "$MOCK_SETTINGS_FILE" ;;
            directory)
                mkdir -- "$MOCK_SETTINGS_FILE"
                printf 'keep concurrent directory\n' > "$MOCK_SETTINGS_FILE/user-data" ;;
        esac
    fi
    exec "${MOCK_REAL_LN:?}" "$@"
fi

repository=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
sandbox=$(mktemp -d /tmp/hyprcosmos-install-test.XXXXXX)
trap 'if [[ "$sandbox" == /tmp/hyprcosmos-install-test.* && -d "$sandbox" ]]; then rm -rf -- "$sandbox"; fi' EXIT
config_dir="$sandbox/hypr"
actual_dir="$sandbox/actual config"
config="$config_dir/hyprland.lua"
actual="$actual_dir/hyprland.lua"
settings="$config_dir/hyprcosmos.lua"
state="$sandbox/state"
mkdir -p -- "$config_dir" "$actual_dir" "$sandbox/build"
printf 'installer fixture only: not a loadable shared object\n' > "$sandbox/build/cosmic.so"
printf -- '-- unrelated user configuration\nhl.config({ general = { border_size = 2 } })\n' > "$actual"
cp -- "$actual" "$sandbox/original"
printf 'unrelated file\n' > "$config_dir/other.lua"
cp -- "$config_dir/other.lua" "$sandbox/other-original"
ln -s -- "$actual" "$config"
checks=0
fail() { printf 'Installer test failed: %s\n' "$*" >&2; exit 1; }
check() { checks=$((checks+1)); "$@" || fail "check $checks: $*"; }
install_fixture() {
    "$repository/scripts/install.sh" --config "$config" --state-dir "$state" \
        --build-dir "$sandbox/build" --no-build "$@" > "$sandbox/install.log" 2>&1
}
uninstall_fixture() {
    "$repository/scripts/uninstall.sh" --config "$config" --state-dir "$state" \
        "$@" > "$sandbox/uninstall.log" 2>&1
}
expect_failure() {
    checks=$((checks+1))
    if "$@"; then fail "unexpected success: $*"; fi
}

install_fixture --dry-run
check cmp -s "$actual" "$sandbox/original"
check test ! -e "$state"
check test ! -e "$config_dir/cosmic"
check test ! -e "$settings"
check rg -q 'User settings: .*hyprcosmos.lua \(create;' "$sandbox/install.log"
install_fixture || { cat "$sandbox/install.log" >&2; fail 'first installation'; }
check test -L "$config"
check test "$(readlink -- "$config")" = "$actual"
check test "$(readlink -- "$config_dir/cosmic")" = "$repository/lua/cosmic"
check test "$(readlink -- "$config_dir/cosmic.so")" = "$sandbox/build/cosmic.so"
check test "$(rg -c '^require\("cosmic"\)$' "$actual")" = 1
check test -f "$state/installation"
check test "$(wc -l < "$state/installation")" = 9
check cmp -s "$settings" "$repository/examples/hyprcosmos.lua"
check test "$(stat -c %a -- "$settings")" = 600
check test ! -e "$actual_dir/hyprcosmos.lua"
cp -- "$settings" "$sandbox/settings-original"
check test "$(rg --files "$actual_dir" | awk '/hyprcosmos-backup/ { count++ } END { print count+0 }')" = 1
cp -- "$actual" "$sandbox/installed"
install_fixture
check cmp -s "$actual" "$sandbox/installed"
check cmp -s "$settings" "$sandbox/settings-original"
check rg -q 'User settings: .*hyprcosmos.lua \(preserve;' "$sandbox/install.log"
check test "$(rg --files "$actual_dir" | awk '/hyprcosmos-backup/ { count++ } END { print count+0 }')" = 1
uninstall_fixture --dry-run
check cmp -s "$actual" "$sandbox/installed"
check test -L "$config_dir/cosmic"
uninstall_fixture || { cat "$sandbox/uninstall.log" >&2; fail 'first removal'; }
check cmp -s "$actual" "$sandbox/original"
check test -L "$config"
check test ! -L "$config_dir/cosmic"
check test ! -L "$config_dir/cosmic.so"
check test ! -f "$state/installation"
check cmp -s "$settings" "$sandbox/settings-original"
check test "$(rg --files "$actual_dir" | awk '/hyprcosmos-backup/ { count++ } END { print count+0 }')" = 2
uninstall_fixture
check cmp -s "$actual" "$sandbox/original"
check cmp -s "$settings" "$sandbox/settings-original"
install_fixture
uninstall_fixture
check cmp -s "$actual" "$sandbox/original"
check cmp -s "$config_dir/other.lua" "$sandbox/other-original"

# Existing unrelated directories and foreign symlinks are never overwritten.
mkdir -- "$config_dir/cosmic"
printf 'keep me\n' > "$config_dir/cosmic/user.lua"
expect_failure install_fixture
check test -f "$config_dir/cosmic/user.lua"
check cmp -s "$actual" "$sandbox/original"
rm -- "$config_dir/cosmic/user.lua"
rmdir -- "$config_dir/cosmic"
ln -s -- "$sandbox/foreign.so" "$config_dir/cosmic.so"
expect_failure install_fixture
check test "$(readlink -- "$config_dir/cosmic.so")" = "$sandbox/foreign.so"
rm -- "$config_dir/cosmic.so"

# A require managed by the user is left byte-for-byte intact on removal.
printf 'local cosmic = require("cosmic")\ncosmic.setup({ idle_timeout = 9 })\n' > "$actual"
cp -- "$actual" "$sandbox/foreign-config"
install_fixture
check cmp -s "$actual" "$sandbox/foreign-config"
uninstall_fixture
check cmp -s "$actual" "$sandbox/foreign-config"

# Neither reinstall nor removal may discard edits within the marked block.
cp -- "$sandbox/original" "$actual"
install_fixture
printf -- '-- user change after installation\n' >> "$actual"
uninstall_fixture
check rg -q '^-- user change after installation$' "$actual"
cp -- "$sandbox/original" "$actual"
install_fixture
awk '/^require\("cosmic"\)$/ { print "require(\"cosmic\").setup({ idle_timeout = 8 })"; next } { print }' \
    "$actual" > "$sandbox/edited"
cp -- "$sandbox/edited" "$actual"
expect_failure install_fixture
expect_failure uninstall_fixture
check cmp -s "$actual" "$sandbox/edited"
check test -L "$config_dir/cosmic"
cp -- "$sandbox/installed" "$actual"
uninstall_fixture

# Session selection must be explicit before any external reload or mutation.
expect_failure install_fixture --reload
check cmp -s "$actual" "$sandbox/original"
check test ! -e "$state/installation"

# Changes made after installation remain user-owned, including replaced links
# and a config symlink pointing somewhere different from the recorded target.
install_fixture
cp -- "$actual" "$sandbox/before-retarget"
rm -- "$config_dir/cosmic"
ln -s -- "$sandbox/foreign-module" "$config_dir/cosmic"
expect_failure uninstall_fixture
check test "$(readlink -- "$config_dir/cosmic")" = "$sandbox/foreign-module"
check cmp -s "$actual" "$sandbox/before-retarget"
rm -- "$config_dir/cosmic"
ln -s -- "$repository/lua/cosmic" "$config_dir/cosmic"
printf -- '-- new configuration target; preserve me\n' > "$sandbox/new-target.lua"
cp -- "$sandbox/new-target.lua" "$sandbox/new-target-original"
rm -- "$config"
ln -s -- "$sandbox/new-target.lua" "$config"
expect_failure uninstall_fixture
check cmp -s "$sandbox/new-target.lua" "$sandbox/new-target-original"
check cmp -s "$actual" "$sandbox/before-retarget"
rm -- "$config"
ln -s -- "$actual" "$config"
uninstall_fixture
check cmp -s "$actual" "$sandbox/original"

# User edits, metadata and even an external settings symlink remain user-owned.
printf 'return { idle_timeout = 91, rendering = { stars = 5 } }\n' > "$settings"
chmod 640 -- "$settings"
cp -- "$settings" "$sandbox/settings-edited"
settings_inode=$(stat -c %i -- "$settings")
install_fixture --dry-run
check rg -q 'User settings: .*hyprcosmos.lua \(preserve;' "$sandbox/install.log"
install_fixture
install_fixture
uninstall_fixture
check cmp -s "$settings" "$sandbox/settings-edited"
check test "$(stat -c %i -- "$settings")" = "$settings_inode"
check test "$(stat -c %a -- "$settings")" = 640
rm -- "$settings"
settings_target="$sandbox/private settings.lua"
cp -- "$sandbox/settings-edited" "$settings_target"
chmod 400 -- "$settings_target"
ln -s -- "$settings_target" "$settings"
install_fixture
uninstall_fixture
check test -L "$settings"
check test "$(readlink -- "$settings")" = "$settings_target"
check cmp -s "$settings_target" "$sandbox/settings-edited"
check test "$(stat -c %a -- "$settings_target")" = 400
rm -- "$settings"

# Invalid existing paths are refused before links, ownership or config edits.
for kind in directory fifo dangling unreadable; do
    case "$kind" in
        directory) mkdir -- "$settings"; printf 'preserve directory\n' > "$settings/user-data" ;;
        fifo) mkfifo -- "$settings" ;;
        dangling) ln -s -- "$sandbox/missing-settings.lua" "$settings" ;;
        unreadable) cp -- "$sandbox/settings-original" "$settings"; chmod 000 -- "$settings" ;;
    esac
    expect_failure install_fixture --dry-run
    expect_failure install_fixture
    check cmp -s "$actual" "$sandbox/original"
    check test ! -e "$state/installation"
    check test ! -L "$config_dir/cosmic"
    check test ! -L "$config_dir/cosmic.so"
    case "$kind" in
        directory)
            check test -f "$settings/user-data"
            rm -- "$settings/user-data"; rmdir -- "$settings" ;;
        fifo) check test -p "$settings"; rm -- "$settings" ;;
        dangling)
            check test -L "$settings"
            check test "$(readlink -- "$settings")" = "$sandbox/missing-settings.lua"
            rm -- "$settings" ;;
        unreadable)
            check test "$(stat -c %a -- "$settings")" = 0
            chmod 600 -- "$settings"
            check cmp -s "$settings" "$sandbox/settings-original"
            rm -- "$settings" ;;
    esac
done

# A readable file winning the publication race is preserved, not truncated.
mkdir -- "$sandbox/bin"
real_ln=$(command -v ln)
ln -s -- "$repository/tests/install_test.sh" "$sandbox/bin/ln"
MOCK_REAL_LN="$real_ln" MOCK_SETTINGS_FILE="$settings" MOCK_SETTINGS_RACE=regular \
    PATH="$sandbox/bin:$PATH" install_fixture
check rg -q '^return \{ idle_timeout = 137,' "$settings"
check test "$(stat -c %a -- "$settings")" = 640
check rg -q 'Preserved concurrently created user settings:' "$sandbox/install.log"
cp -- "$settings" "$sandbox/settings-race-original"
uninstall_fixture
check cmp -s "$settings" "$sandbox/settings-race-original"
rm -- "$settings"

# ln -T must reject a concurrent directory rather than publish a file inside it.
MOCK_REAL_LN="$real_ln" MOCK_SETTINGS_FILE="$settings" MOCK_SETTINGS_RACE=directory \
    PATH="$sandbox/bin:$PATH" expect_failure install_fixture
check test -f "$settings/user-data"
check test "$(find "$settings" -mindepth 1 -maxdepth 1 -type f | wc -l)" = 1
check cmp -s "$actual" "$sandbox/original"
check test ! -e "$state/installation"
check test ! -L "$config_dir/cosmic"
check test ! -L "$config_dir/cosmic.so"
check test "$(find "$config_dir" -maxdepth 1 -name '.hyprcosmos-settings.*' | wc -l)" = 0
rm -- "$settings/user-data"
rmdir -- "$settings"
printf 'Installer lifecycle: %s checks passed (isolated fixtures; no session contacted)\n' "$checks"
