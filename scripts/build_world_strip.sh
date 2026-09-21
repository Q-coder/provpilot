#!/bin/zsh
# Bake the base-tier regions given on the command line, one after another,
# logging to <build_root>/<id>/build.log. Resumable: re-running skips baked tiles.
set -u
cd "$(dirname "$0")/.."
PY=.venv/bin/python
for r in "$@"; do
  dir="/Volumes/Data1/provpilot/build/$r"
  mkdir -p "$dir"
  echo "=== $(date '+%F %T') build $r ===" | tee -a "$dir/build.log"
  $PY scripts/build_world_terrain.py build --region "$r" >> "$dir/build.log" 2>&1
  echo "=== $(date '+%F %T') $r exit $? ===" | tee -a "$dir/build.log"
done
