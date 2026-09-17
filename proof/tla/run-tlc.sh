#!/usr/bin/env bash
# Runs TLC on the Araya refinement model (proof/tla/MC.tla).
#
# Usage: run-tlc.sh [path-to-tla2tools.jar]
#
# The jar is looked up from the first argument, then $TLA2TOOLS, then the
# standard install locations. TLC needs Java (11+).
set -euo pipefail

JAR="${1:-${TLA2TOOLS:-}}"
if [ -z "$JAR" ]; then
    for candidate in /usr/local/share/tla+/tla2tools.jar /usr/share/java/tla2tools.jar; do
        if [ -f "$candidate" ]; then
            JAR="$candidate"
            break
        fi
    done
fi
if [ -z "$JAR" ] || [ ! -f "$JAR" ]; then
    echo "error: tla2tools.jar not found; pass it as an argument or set TLA2TOOLS" >&2
    exit 2
fi

DIR="$(cd "$(dirname "$0")" && pwd)"
exec java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -config "$DIR/MC.cfg" -workers auto "$DIR/MC.tla"
