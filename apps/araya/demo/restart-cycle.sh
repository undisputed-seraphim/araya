#!/usr/bin/env bash
# The restart-cycle smoke test: the save run writes a session through the
# persistence barrier and exits; the load run boots a fresh process in the
# same directory and must find the history on disk.
set -euo pipefail

console=$1
save_script=$2
load_script=$3

root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT

cd "$root"
"$console" run "$save_script" > save.log 2>&1
"$console" run "$load_script" > load.log 2>&1

grep -q "persists across restarts" load.log
