#!/usr/bin/env bash
# Build the paired AIR/Mesa backend in this component's ignored out/ directory.
set -euo pipefail
module=$(cd "$(dirname "$0")/.." && pwd)
xnu_tree=${1:?usage: build-backend.sh /path/to/full-hackios-tree}
xnu_tree=$(cd "$xnu_tree" && pwd)
stage="$module/out/work"
mkdir -p "$stage/tools" "$stage/xnu-deps/build/mesa" "$module/out/tmp"
export TMPDIR="$module/out/tmp"
cp "$xnu_tree/xnu-env.sh" "$stage/xnu-env.sh"
for entry in host hostinc sdk; do
    if [[ ! -e "$stage/xnu-deps/$entry" ]]; then
        ln -s "$xnu_tree/xnu-deps/$entry" "$stage/xnu-deps/$entry"
    fi
done
cp -a "$module/backend/mesa" "$stage/tools/"
cp "$module/backend/mk-mesa.sh" "$stage/tools/mk-mesa.sh"
archive="$xnu_tree/xnu-deps/build/mesa/mesa-25.0.7.tar.xz"
if [[ -f "$archive" ]]; then cp "$archive" "$stage/xnu-deps/build/mesa/"; fi
venv="$xnu_tree/xnu-deps/build/mesa/venv"
if [[ -x "$venv/bin/python3" && ! -e "$stage/xnu-deps/build/mesa/venv" ]]; then
    ln -s "$venv" "$stage/xnu-deps/build/mesa/venv"
fi
AIR=1 bash "$stage/tools/mk-mesa.sh"
