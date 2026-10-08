#!/usr/bin/env bash
set -euo pipefail

repository=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
sandbox=$(mktemp -d /tmp/hyprcosmos-install-test.XXXXXX)
trap 'if [[ "$sandbox" == /tmp/hyprcosmos-install-test.* && -d "$sandbox" ]]; then rm -rf -- "$sandbox"; fi' EXIT
config_dir="$sandbox/hypr"
actual_dir="$sandbox/actual config"
config="$config_dir/hyprland.lua"
actual="$actual_dir/hyprland.lua"
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
install_fixture || { cat "$sandbox/install.log" >&2; fail 'first installation'; }
check test -L "$config"
check test "$(readlink -- "$config")" = "$actual"
check test "$(readlink -- "$config_dir/cosmic")" = "$repository/lua/cosmic"
check test "$(readlink -- "$config_dir/cosmic.so")" = "$sandbox/build/cosmic.so"
check test "$(rg -c '^require\("cosmic"\)$' "$actual")" = 1
check test -f "$state/installation"
check test "$(rg --files "$actual_dir" | awk '/hyprcosmos-backup/ { count++ } END { print count+0 }')" = 1
cp -- "$actual" "$sandbox/installed"
install_fixture
check cmp -s "$actual" "$sandbox/installed"
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
check test "$(rg --files "$actual_dir" | awk '/hyprcosmos-backup/ { count++ } END { print count+0 }')" = 2
uninstall_fixture
check cmp -s "$actual" "$sandbox/original"
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
printf 'Installer lifecycle: %s checks passed (isolated fixtures; no session contacted)\n' "$checks"
