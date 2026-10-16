#!/usr/bin/env bash
# build-plugin.sh - build the agentbridge plugin with the bundled toolchain.
# Usage: scripts/build-plugin.sh [make-args...]   (e.g. `clean`)
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export PATH="$ROOT/tools/mingw64/bin:$PATH"

exec mingw32-make -C "$ROOT/plugin/agentbridge" "$@"
