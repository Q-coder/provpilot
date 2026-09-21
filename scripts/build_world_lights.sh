#!/bin/zsh
# Fetch individually mapped OSM airfield lights for every base region and
# re-run the flatten stage so each fixture gets its elev_m (design §12.6).
# Overpass drops requests under load: a region is retried until its light
# cache exists. Usage: scripts/build_world_lights.sh [region ...]
set -u
cd "$(dirname "$0")/.."
PY=.venv/bin/python
CACHE=/Volumes/Data1/scenery_in_world/osm_lights
regions=("$@")
if [[ ${#regions} -eq 0 ]]; then
  regions=(denmark germany austria italy france norway sweden finland switzerland-base)
fi
for r in $regions; do
  echo "=== $r lights"
  for attempt in {1..6}; do
    [[ -s $CACHE/$r.json ]] && break
    $PY scripts/build_world_runways.py --region $r || true
    [[ -s $CACHE/$r.json ]] || { echo "  retry $attempt in 120 s"; sleep 120; }
  done
  [[ -s $CACHE/$r.json ]] || { echo "  $r: no lights after retries — skipped"; continue; }
  $PY scripts/build_world_runways.py --region $r --offline
  $PY scripts/build_world_terrain.py flatten --region $r
done
echo BUILD_WORLD_LIGHTS_DONE
