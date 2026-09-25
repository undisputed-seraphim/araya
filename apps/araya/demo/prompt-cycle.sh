#!/usr/bin/env bash
# The prompt-editing smoke test: ARAYA_SYSTEM_PROMPT seeds the persona,
# /prompt set/identity/clear edit it at runtime, and the agent loop
# commits the edited prompt into the session.
set -euo pipefail

console=$1
script=$2

root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT

cd "$root"
ARAYA_SYSTEM_PROMPT="You are from the environment." "$console" run "$script" > prompt.log 2>&1

grep -q 'You are from the environment.' prompt.log
grep -q 'prompt: prefix set' prompt.log
grep -q '"text":"Be terse.' prompt.log
grep -q 'prompt: cleared' prompt.log
