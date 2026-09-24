#!/usr/bin/env bash
# The restore smoke test: the save run writes a session and exits; the
# restore run boots a fresh process and restores it through the picker's
# `/session restore` path (switch if live, else load from disk).
set -euo pipefail

console=$1
save_script=$2
restore_script=$3

root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT

cd "$root"
"$console" run "$save_script" > save.log 2>&1
"$console" run "$restore_script" > restore.log 2>&1

grep -q "restored demo" restore.log
grep -q "persists across restarts" restore.log
