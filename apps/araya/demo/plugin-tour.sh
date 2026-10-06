#!/usr/bin/env bash
# Drives the araya script runner through as many first-party plugins as
# possible against the local llama.cpp LLM. Configure the endpoint with
# ARAYA_CONFIG (defaults to demo/local-llm.json beside this script).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
app="${ARAYA_BIN:-$root/build/apps/araya/araya}"
config="${ARAYA_CONFIG:-$here/local-llm.json}"
work="${ARAYA_TOUR_WORK:-/tmp/araya-tour/work}"

if [[ ! -x "$app" ]]; then
	echo "plugin-tour: araya binary not found at $app (build it, or set ARAYA_BIN)" >&2
	exit 1
fi
mkdir -p "$work"

cd "$work"
export XDG_STATE_HOME="${ARAYA_TOUR_STATE:-/tmp/araya-tour/state}"
export XDG_CONFIG_HOME="${ARAYA_TOUR_CONFIG:-/tmp/araya-tour/config}"
mkdir -p "$XDG_STATE_HOME" "$XDG_CONFIG_HOME"

# A local skill so `tool-skill` has something to load. The skill plugin
# discovers project skills under <cwd>/.dsh/skills/<name>/SKILL.md.
mkdir -p "$work/.dsh/skills/plugin-tour"
cat > "$work/.dsh/skills/plugin-tour/SKILL.md" <<'MD'
---
name: plugin-tour
description: A tiny skill used by the araya plugin tour to exercise tool-skill.
---

When this skill is loaded, the loader has reached the skill plugin. Reply with the token: skill-loaded.
MD

exec "$app" --config "$config" run "$here/plugin-tour.txt"
