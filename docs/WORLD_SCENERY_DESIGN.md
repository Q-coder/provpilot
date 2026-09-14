# Worldwide & Extendable Scenery — Design

Design subtask from [gerys_plan.md](gerys_plan.md) (post-1.0.10): make ProvPilot's
scenery worldwide and extendable. First flyable pilot region: **Norway** —
data-light (no ortho), starting from coastlines and major airports, reachable by
flight from the existing Swiss / Baden-Württemberg scenery. The "Small Ville"
initial-download region (§4.4) is **deferred**.

Status: **approved for implementation** (gery decisions 2026-09-14: Copernicus
GLO-30 terrain, OSM water/coastline, OurAirports airports, 30 m/px default pack,
Prime Meridian coordinate display incl. HUD, `/Volumes/Data1` build storage).
All external sources listed below were probed on 2026-09-14 unless noted otherwise.

## 1. What we already have

| Piece | State | Relevance |
|-------|-------|-----------|
| Scenery3D (C++ GDExtension) | Streams `tile_{LV95_E}_{LV95_N}.raw` heightmap tiles (1024 m cells, SE-corner origin, float32 LE) with LOD rings + 200 km far chunks; multi-path priority merge (later path overrides earlier per pixel where non-zero) | Norway slots in as *another region directory* — no placement changes |
| world3d globe | Whole-Earth cube-sphere from 60″ ETOPO, mounted into the flat LV95 frame; elevation queries fall back to it when Scenery3D has no data (`node_3d.gd::_terrain_elevation_at`, patch-first) | Gives ground everywhere on the planet *today*; Norway just replaces the coarse fallback with real terrain |
| Flat world frame | Single LV95 tangent frame, origin Schaffhausen (E 2 682 880 / N 1 283 072), **Y = elevation ASL** | Governs how far "flat" is acceptable (see §3.2) |
| Scenery Library | Priority-ranked region directories, each with asset subdirs (`terrain/ orthophoto/ buildings/ water/ roads/ vegetation/ trees/ airports/`) | The extendability model already exists at the directory level |
| Distribution | `scenery_regions.json` → base + 4 detail-quadrant zips (ZIP_STORED) on md80.ch, sha256-keyed manifest consumed by the in-app downloader | New regions = new entries; mechanism is generic |
| OSM pipeline | `addons/scenery3d/tools/convert_osm_bw.py` streams a country PBF (pyosmium) and emits road/water GLB tiles on the 1024 m LV95 grid, with optional **baked vertex Y** (`elevation_baked` GLB root flag) | Reusable for Norway water/coastline almost as-is |
| Airport data | `scenery/poi/airports.json` (ICAO, name, type, elevation, runways with LV95 thresholds); procedural runway rendering from that JSON | Schema is region-agnostic; only the coordinate source changes |

Known current limitation (see [WORLD3D.md](WORLD3D.md)): `tile_px` is a **single
node property** (default 512) applied to *all* terrain paths, and the C++ tile
manager does not read per-region manifests — mixing 1 m/px (BW) with 2 m/px (CH)
is already a documented caveat. Norway at ~30 m/px makes this the central engine
question (§3.3).

## 2. Data sources for worldwide scenery

### 2.1 Terrain / DEM

| Source | Resolution | Coverage | Access | License | Verdict |
|--------|-----------|----------|--------|---------|---------|
| **AWS `elevation-tiles-prod` S3** (SRTM-derived) | ~30 m effective (see note) | Global land + bathymetry (negative elevations) | Plain HTTP GET, no registration: `https://s3.amazonaws.com/elevation-tiles-prod/geotiff/{z}/{x}/{y}.tif`, XYZ slippy grid, **512×512** float32 GeoTIFF, EPSG:3857, nodata ≈ −3.4×10³⁸, **z = 0…14** (z=15 verified missing) | US public data (SRTM), de-facto free | **Fallback / no-registration alternative.** Verified live on 2026-09-14 (tile dims, CRS, nodata, bathymetry all measured from a downloaded z=14 tile). Only source here with real seafloor values under the sea. |
| **Copernicus DEM GLO-30** | 30 m global (≈15 m/px native at 60°N) | Global land (**ocean coverage unverified — see §5.3**) | Copernicus Data Space STAC, collection `cop-dem-glo-30-dged-cog`: **1°×1° COG GeoTIFF items** (`Copernicus_DSM_COG_10_N57_00_E017_00_DEM.tif`). Search is anonymous; **downloads need a free API key** (verified 2026-09-14: `s3.dataspace.copernicus.eu` → 403 unauthenticated, eox mirror unreachable) | Open (Copernicus/ESA) | **Recommended base source (gery decision 2026-09-14).** Verified live on 2026-09-14 (item list + tile naming). |
| Copernicus DEM EEA-10 | 10 m, SAR InSAR | Europe (EEA) | Same STAC, collection `cop-dem-eea-10-laea-tif` (LAEA tiles, OData download links) | Open (Copernicus/EEA) | Future "quality" tier for CH/BW/Norway upgrade packs (§2.4) |
| NASADEM / SRTM 1″ | ~30 m equator → **~15 m at 60°N** | Land (gaps in mountains/poles) | USGS EarthExplorer / OpenTopography, free registration | Public domain | Better native detail for high-latitude Norway |
| National DTMs (e.g. Kartverket Norway) | 20–40 m, locally best | One country at a time | WMTS/API — **access unverified** (api.kartverket.no returned 404 on 2026-09-14) | Usually open | Per-country upgrade path (tier 2), not for the pilot |

Note on the AWS product (fallback only): it is *not* 30 m at z=14. A spectral analysis of a
z=14 tile over Sognefjord showed **99.2 % of the energy in wavelengths > 30 m** —
the upper zoom levels are resamplings of ~30 m (SRTM-3″-class) data. Nominal grid
spacing at 60°N: z=10 ≈ 38 m/px, z=12 ≈ 9.5 m/px, z=14 ≈ 2.4 m/px; usable detail
tops out around 30 m regardless of zoom. For the pilot we therefore fetch **z=10
or z=11** (the finest that is not pure resampling at our target posting) and
reproject to our own grid — no need to download the whole z=14 pyramid.

### 2.2 Coastlines & water

| Source | Detail | Size | License | Verdict |
|--------|--------|------|---------|---------|
| **OpenStreetMap** (Geofabrik country PBF / Overpass) | Full fjord detail, `natural=water` polygons + `waterway` lines; also the only free source of runway geometry at this scale | Norway PBF ≈ 0.5 GB download (approx.) | ODbL 1.0 (attribution) | **Recommended for the Norway pilot** — fjords are the whole point |
| Natural Earth 1:10m physical vectors | Generalized coastlines, lakes + Europe supplement, rivers | ~5 MB for all of it | Public domain | Recommended as the *global far layer* (coastline context for every other region) and as land-mask for terrain clipping |
| GSHHG / GEBCO | Coastline polygons at several resolutions / 15″ bathymetry | small / large | free w/ registration | Later, if we want real ocean floors beyond SRTM's baked-in bathymetry |

### 2.3 Airports

| Source | Content | Access | Verdict |
|--------|---------|--------|---------|
| **OurAirports** | `airports.csv` + `runways.csv` (ICAO, IATA, type, elevation_ft, runway length/heading/surface), daily dump | Plain HTTP: `https://davidmegginson.github.io/ourairports-data/*.csv`, free for any use | **Recommended.** Covers all ~25 Norwegian international/primary airports; one script feeds *every* future region |
| OSM `aeroway=aerodrome` | Runway geometry + ICAO tags, per country PBF | Same PBF as water | Cross-check / fill-in where OurAirports is stale |
| National AIPs (Swiss AIP today) | Authoritative thresholds | Manual | Keep for CH; not scalable worldwide |

### 2.4 Later tiers (extendability, not the pilot)

Buildings: Esri World Buildings or OSM `building=*` per region · Roads/rail:
OSM via the existing converter · Ortho: national programs (LGL BW already in use,
Kartverket for Norway) · Vegetation: OSM `landuse=forest` + procedural · Terrain
upgrade tier: Copernicus EEA-10 (`cop-dem-eea-10-laea-tif`, 10 m SAR InSAR,
Europe-wide — verified in the Data Space catalogue on 2026-09-14) or national DTMs.

## 3. Global tile & coordinate scheme

### 3.1 Keep LV95 as the single world frame

**Recommendation: no new projection.** The flat Scenery3D world is one LV95
tangent plane and every region directory is placed by its tiles' LV95 names.
LV95 (EPSG:2056) is a valid oblique Mercator for all of Europe, so Norway needs
no coordinate work beyond `pyproj` reprojection in the build scripts:

| Point | LV95 E | LV95 N | Flat-frame position vs origin |
|-------|--------|--------|-------------------------------|
| Schaffhausen (origin) | 2 682 880 | 1 283 072 | (0, 0) |
| Oslo ENGM | 2 789 881 | 2 663 366 | +107 km east, +1 380 km north |
| Bergen ENBR | 2 479 925 | 2 711 278 | −203 km west (i.e. +X), +1 428 km north |
| Kirkenes / Finnmark NE | 3 520 612 | 4 112 181 | +838 km east, +2 829 km north |

(Computed with pyproj EPSG:4326→EPSG:2056 on 2026-09-14.) Coordinates stay
positive and below 4.2 M m — trivial for the double-precision engine (mm-level
precision at 10⁷ m magnitude; our custom Godot build is `precision=double`, so
`Vector3` *is* `Vector3d`). P0 still verifies that no code path in the C++ tile
lookup downcasts LV95 coordinates to float. Norwegian tiles therefore stream,
LOD-ring, and far-chunk **exactly like Swiss tiles**, with zero placement changes.

Scale distortion of CH1903+ grows with distance from Switzerland; at Norway's
latitude it is on the order of 10⁻⁴ (tens of metres per 100 km) — far below the
~30 m terrain posting, and irrelevant because *all* layers (terrain, water,
airports) share the same frame. If we later target regions far from Europe
(equatorial Asia, Americas), revisit a WGS84/UTM global grid; that is a schema-v2
question, not a pilot question.

### 3.2 Flat-frame curvature limit (CH → Norway corridor)

The flat plane diverges from the ellipsoid by ≈ d²/2R: ~150 km at Oslo's
distance, ~500 km at Tromsø's. This does **not** break flight dynamics, because
the system already treats *flat-frame Y as ASL everywhere*: the world3d elevation
fallback (`_globe_elevation_at`) converts flat→ECEF→geodetic and returns the
globe's **ASL** value straight back as a flat Y. A Norwegian region whose tiles
are also ASL is therefore consistent with both the CH/BW patches and the globe
fallback — an aircraft flying Schaffhausen → North Sea → Oslo sees continuous
ground reference (fine patch → ETOPO sea floor → fine patch).

What *is* affected is the far-view backdrop: above 25 km altitude the curved
world3d globe is visible, and it sits at its true ECEF position — hundreds of km
"above" a flat Norwegian patch. That seam already exists today for any view
beyond the CH/BW patch; horizon fog hides it in practice. Accepted limitation
for v1; a per-region tangent rebase (moving `GlobeMount`/origin with the active
region) is the clean long-term fix and is out of scope here.

### 3.3 Per-region pixel resolution — the one engine change

Data-light regions must not store 1024×1024 float32 tiles for 30 m data (that is
a 10 000:1 waste). The on-disk format already carries `pixels_per_tile` +
`pixel_resolution_m` in every terrain manifest, and the Python side already emits
non-1 m/px pyramids (`streaming_terrain_generator.py`, `swissbathy3d.py`). The
gap is C++: `S3DTileManager` assumes one node-wide `tile_px` for all paths.

**Recommended change (small, scoped to `s3d_tile_manager.cpp`):** when a region's
`terrain/manifest.json` declares `pixels_per_tile`, use it per path instead of
the node value; fall back to the node `tile_px` when absent. Mip levels
(`mip2/…mip4/`) already have their own manifests and would follow the same rule.
CH (512 px) and BW (1024 px) keep working via fallback, which also resolves the
existing mixed-resolution caveat as a side effect.

Until that lands, a Norway build could instead be forced to the node's `tile_px`
— but 512 px over 1024 m is 2 m/px (1 MiB/tile → ≈ 370 GB for Norway) or we'd
have to change the global node value and re-bake CH/BW. The per-path read is worth it.

### 3.4 Prime Meridian coordinate system (WGS84) for display & global addressing

LV95 stays the *runtime* frame (§3.1 — recommendation kept). On top of it we add
the **Prime Meridian system** — plain WGS84 latitude/longitude referenced to the
Greenwich prime meridian — as the canonical *human-facing and global* coordinate
system:

- **Global addressing:** build scripts take WGS84 bboxes (schema-v2 `bbox_wgs84`,
  §4.2); every region's source data is fetched in WGS84 and only reprojected to
  LV95 at the very end. No tile-naming or world-frame change.
- **HUD display:** the position readout gains a Prime Meridian line under the
  existing Swiss E/N, e.g. `PM: N 60°11.63′  E 011°06.42′` (degrees + decimal
  minutes with hemisphere letters; exact format is a one-line tweak in the HUD).

Implementation reuses what already exists: C++ `Geo::lv95_to_wgs84()`
(`src/geo.h`, swisstopo polynomial, ~1 m accuracy) is already bound for GDScript
and used by the world3d setup; the HUD update loop (`node_3d.gd`, coordinate
block of the `_update_hud` section) converts the LV95 position it already
computes and formats it. No new projection code, no engine change — a ~half-day
subtask, independent of the terrain work (§7).

## 4. Extendability model

The runtime is already extensible: a region is a directory with asset subdirs,
the Scenery Library ranks/merges them, and the downloader installs any pack that
follows the layout. What is missing is a *contract* that lets third parties (or
us, for new countries) produce a valid region without reading the codebase.

### 4.1 Region contract (what a pack must contain)

```
<RegionName>/                      # e.g. Norway/
├── terrain/                       # REQUIRED — tile_{E}_{N}.raw + manifest.json
│   ├── manifest.json              #   declares pixels_per_tile, pixel_resolution_m
│   └── mip2/ … mip4/              # optional far-chunk pyramids (own manifests)
├── water/                         # optional — water_*.glb (+manifest), baked-Y allowed
├── roads/  trees/  vegetation/    # optional — as today
├── buildings/  orthophoto/        # optional — detail tier
└── poi/airports.json              # optional — existing schema, LV95 coords
```

Plus a `licenses.json` (new, tiny): source name + license + attribution string
per asset class, surfaced in the Scenery Library UI. This keeps ODbL (OSM),
public-domain (SRTM/Natural Earth/OurAirports) and national licenses honest.

### 4.2 Region config schema v2 (`scenery_regions.json`)

Current entries are LV95-bbox + quadrants only. Proposed additions, all optional:

```json
{
  "id": "norway",
  "name": "Norway",
  "scenery_subdir": "Norway",
  "bbox_wgs84": [4.6, 57.9, 31.3, 71.2],      // new: lon_min,lat_min,lon_max,lat_max
  "bbox_lv95":  [2462000, 2453000, 3521000, 4113000],
  "terrain": { "resolution_m": 30.0 },        // new: per-region posting (→ tile_px)
  "quadrants": [ … ]                          // unchanged; may be empty for small regions
}
```

`bbox_wgs84` is the source of truth for build scripts; `bbox_lv95` stays for the
bundler. Quadrants become optional — a region that fits in one base pack (Norway,
Small Ville) declares none and ships a single `norway-base-v1.zip`.

### 4.3 Tiers

Keep the existing two classes (`base` = terrain/water/trees/airports/map,
`detail` = buildings/orthophoto/vegetation). Data-light regions simply have no
detail packs. A future third tier (`far`: Natural Earth coastline + ETOPO-only
terrain for the whole planet) is how "worldwide" eventually gets a default look
without per-country work — it would ship *inside* the game, not as a download.

### 4.4 Small Ville (initial-download region) — *deferred*

> **Deferred by gery, 2026-09-14:** dropped for now; revisit after Norway ships.
> If a small first pack is ever needed in the meantime, the 60 m/px Norway tier
> (§5.3, ~0.6 GB) temporarily plays that role.

Goal: a new install is flyable after a **~1 GB** download instead of the 46 GB
Switzerland base. Proposal:

- Area: 128 × 128 km box centred on the world origin (Schaffhausen / Lake
  Constance corner): Schaffhausen, Stein am Rhein, Altenrhein airfield (LSZR,
  54 km NE of the origin), the Swiss + German lake coast, and the BW edge
  already in the BW pack.
- Terrain: re-bake from the existing CH/BW sources at **10 m/px** (102 px/tile,
  41.6 KB/tile) → 125 × 125 tiles ≈ 650 MB + mips ≈ **~0.9 GB**; water + airports
  from the existing layers; no buildings/ortho.
- Ships as `smallville-base-v1` in the same manifest mechanism, pre-selected in
  the downloader ("Start here").
- Q1 was resolved by deferral (2026-09-14): the real-Schaffhausen-box vs
  synthetic-town question is parked together with the section.

### 4.5 Community / third-party regions

With §4.1–4.2 in place, a community pack is: directory + zips + one manifest
entry (or its own `scenery_manifest.json` URL — the downloader already takes the
manifest URL as a setting). No engine changes needed beyond §3.3.

## 5. Norway pilot spec

### 5.1 Scope & acceptance criteria

- **Flyable**: LSZH (Zürich) → over the BW/Germany gap → North Sea → ENGM (Oslo)
  in one flight, with continuous ground reference and no visible "edge of the
  world" before the Norwegian coast.
- **Coastlines**: OSM-quality fjord coastline (Sognefjord, Hardangerfjord,
  Lofoten archipelago recognisable), sea surface at 0 m ASL.
- **Major airports**: ~25 international/primary aerodromes with procedural
  runways and correct thresholds — ENGM, ENBR, ENSV, ENTQ, EENT, ENKR, ENBO,
  ENAAL, ENHB, ENML, ENHD, ENAL, … (final list = OurAirports `type` ∈
  {large_airport, medium_airport} ∩ NO, expected ~20–30).
- **Data-light**: no orthophoto, no buildings, no roads. Target pack size
  **≤ 4 GB** (see §5.3); default 30 m/px ≈ 2.2 GB terrain, with the 60 m/px
  low-bandwidth tier at ≈ 0.6 GB.

### 5.2 Coverage

Mainland Norway WGS84 bbox ≈ lon 4.6…31.3°E, lat 57.9…71.2°N
(Svalbard 74–81°N excluded for v1 — separate tiny region later). LV95 bbox
≈ E 2 462 000…3 521 000, N 2 453 000…4 113 000 (corners from §3.1; the SW
corner is open sea west of Møre, so the clipped land mask matters more than
the bbox).

### 5.3 Terrain build (new script `scripts/build_world_terrain.py`)

1. Compute the LV95 grid cells (1024 m) intersecting *land + 5 km margin*, where
   land = Natural Earth 1:10m `ne_10m_land` polygon (+ minor islands) clipped to
   the Norway bbox. ≈ 370 k tiles (mainland 323 800 km² ≈ 309 k tiles + margin).
2. Fetch the covering **Copernicus GLO-30 1° COG tiles** from the Data Space STAC
   API (`collections=cop-dem-glo-30-dged-cog`, bbox query; items such as
   `Copernicus_DSM_COG_10_N57_00_E017_00_DEM` — Norway spans ~28×14 degree tiles,
   land subset far smaller) into the raw cache, then reproject each needed cell
   WGS84→LV95 with `rasterio.warp.reproject` into a `tile_px × tile_px` buffer
   (SE-corner convention, same as CH). GLO-30 is natively ~15 m/px at 60°N, so
   the 30 m target is a *downsample*, not an upsample.
3. Write `tile_{E}_{N}.raw` (float32 LE) + manifest with
   `pixels_per_tile`, `pixel_resolution_m`, per-tile min/max elevation.
4. Build mip2/mip3/mip4 pyramids for far chunks (existing mipmap logic).

Storage (gery decision 2026-09-14): all downloads and intermediate processing go
to the external volume **`/Volumes/Data1`** (3.6 TiB, ~1.1 TiB free at design
time), following the existing drive conventions (`scenery_in_bw`,
`provpilot/build/bw-*`):

```
/Volumes/Data1/scenery_in_norway/          # raw inputs, versioned cache
├── glo30/                                  # Copernicus 1° COG tiles
├── norway-latest.osm.pbf                   # Geofabrik extract
└── ourairports/{airports.csv,runways.csv}
/Volumes/Data1/provpilot/build/norway/      # processing + final region dir
└── Norway/                                 # copied to the game data root for flight tests
```

Resolution options:

| Posting | tile_px | Bytes/tile | Land tiles ≈ 370 k | +mips | Pack (terrain only) |
|---------|---------|-----------|--------------------|-------|---------------------|
| 30 m/px | 34 | 4.6 KB | ~1.7 GB | ×1.30 | **~2.2 GB** ← **default (gery decision 2026-09-14)** |
| 60 m/px | 17 | 1.2 KB | ~0.45 GB | ×1.30 | **~0.6 GB** (stretch) |

Bathymetry: GLO-30 is a *land* DEM — ocean coverage must be verified in P1 by
inspecting one coastal tile (`gdalinfo` + sample values). If sea pixels are
nodata, margin cells over open water bake as flat 0 m under the +1.5 m water
plane (acceptable v1; the AWS SRTM fallback of §2.1 provides real seafloor if
the hard shelf edge ever looks wrong from above).

### 5.4 Water & coastline (reuse `tools/convert_osm_bw.py`)

- Input: Geofabrik `norway-latest.osm.pbf` (includes adjacent North Sea / Barents
  Sea inside the extract bbox).
- Generalise the converter: projection already LV95 (works), add baked vertex Y
  with a **per-polygon water level**: sea-connected water (polygons touching the
  bbox edge or another sea polygon — open sea, fjords) gets **Y = +1.5 m**
  (`elevation_baked=true`), so no runtime draping pulls the ocean surface down to
  the sea floor where terrain is absent. Closed inland lakes get a constant level
  from the DEM mean of their interior (e.g. Mjøsa ≈ 93 m ASL) instead of sea
  level — baking everything to +1.5 m would sink Mjøsa ~90 m below its shore.
- Rivers/lakes inside Norway from the same PBF (`waterway=*`, `natural=water`).
- Size estimate: unknown until built — coastline ≈ 25 000 km drives it; expect
  **50–300 MB**. Measure in P2 and revisit if > 400 MB (decimate interior sea
  polygons, keep coast detail).

### 5.5 Airports (new script `scripts/build_world_airports.py`)

- Download OurAirports `airports.csv` + `runways.csv` (daily, plain HTTP).
- Filter: ISO NO, type large/medium airport, has ≥1 paved runway.
- Emit `Norway/poi/airports.json` in the **existing schema** — WGS84→LV95 via
  pyproj for `center_e/n` and both runway thresholds; elevation from
  `elevation_ft`; runways keep designator/length/width/surface.
- Cross-check ICAO list against OSM `aeroway=aerodrome["icao_ref"]` in the same
  PBF (log-only diff). If threshold accuracy proves insufficient for curved or
  offset runways, switch runway *geometry* to OSM `aeroway=runway` features and
  keep OurAirports for metadata.

### 5.6 Integration & verification

1. Add `norway` to `scripts/scenery_regions.json` (schema v2, no quadrants).
2. Scenery Library: `Norway/` auto-scans like any region; priority below CH/BW
   (no overlap anyway). Downloader: one `norway-base-v1` row. Bundler must
   **resolve symlinks** when zipping (asset dirs are symlinks on the data root;
   Windows extraction of raw symlinks is unreliable).
3. Engine: land the §3.3 per-path `tile_px` change first (prerequisite).
4. Flight test script/checklist: spawn LSZH → ENGM direct (~1 425 km / ~770 nm): verify
   (a) elevation continuity over the sea corridor, (b) coastline + water plane
   at the approach to ENGM/ENBR/ENTQ/EENT, (c) runway thresholds on ground truth
   (compare 4–6 airports against OSM), (d) far-chunk silhouettes of Jotunheimen
   from over the North Sea.

## 6. Risks & open questions

| # | Risk / question | Impact | Mitigation / decision needed |
|---|-----------------|--------|------------------------------|
| R1 | Copernicus Data Space access is free but account-gated (API key for downloads), no SLA; standard Copernicus/ESA data policy requires attribution | Build pipeline depends on it | Cache fetched 1° tiles in the versioned `/Volumes/Data1/scenery_in_norway/glo30/` store (rebuildable without network); one-off fetch, not runtime. Fallback: AWS SRTM bucket (no registration). Attribution strings in `licenses.json` regardless |
| R2 | 30 m terrain in mountainous Norway looks "blocky" at low altitude vs CH's 1–2 m | Visual step-change when crossing into the region | Accepted for pilot (data-light); per-region tier upgrade (Kartverket DTM, §2.1) is the planned fix; horizon fog + distance reduce visibility |
| R3 | Per-path `tile_px` C++ change touches the streaming hot path | Regression in CH/BW loading | Fallback-to-node-value keeps old regions byte-identical; validate with existing BW/CH test flights |
| R4 | OSM water GLB size unknown until built | Pack could exceed 4 GB | Measure in P2; decimate interior polygons, keep coastal detail; worst case split into `norway-base` + `norway-water-detail` |
| R5 | Flat-frame seam vs world3d globe at high altitude over Norway (§3.2) | Backdrop discontinuity above ~25 km | Accepted v1 (fog); long-term per-region tangent rebase |
| R6 | LV95 distortion for future non-European regions | Coordinates grow/distort far from CH | Fine through ~80°E/40°N; beyond that, schema-v2 WGS84 grid (§3.1) — not needed for Norway |
| Q1 | ~~Small Ville: real Schaffhausen box vs synthetic miniature town?~~ (§4.4) | Initial-download design | **Resolved 2026-09-14:** Small Ville deferred; revisit after Norway ships |
| Q2 | ~~30 m/px vs 60 m/px default?~~ | Pack size 0.6 GB vs 2.2 GB | **Resolved 2026-09-14:** 30 m/px is the default; 60 m/px stays an optional low-bandwidth tier |
| Q3 | Svalbard / Lofoten as separate regions or one Norway pack? | Region granularity | One pack for v1; split later if size demands |
| Q4 | ~~Self-referential `scenery/scenery` symlink — keep or repoint?~~ | Confusing layout | **Resolved 2026-09-14:** deleted by gery — leftover from earlier work, no replacement needed |
| Q5 | ~~Copernicus Data Space account?~~ | P1 terrain fetch | **Resolved 2026-09-14:** account `(account removed)` + temporary S3 key created (expires 2026-09-16; credentials in gitignored `scripts/.copernicus.env`). Key verified working but see R8 |
| R7 | GLO-30 ocean coverage unverified (land DEM) | Coastline shelf under the water plane | Verify one coastal tile in P1 (`gdalinfo` + sample values); flat 0 m seafloor accepted for v1 (§5.3) |
| R8 | GLO-30 403 **root cause identified 2026-09-14**: COP-DEM-GLO-30-R is filed under **Copernicus Contributing Missions (CCM)** in Data Space (collection provider URL `…/copernicus-contributing-missions/collections-description/COP-DEM`; license PDF `copernicus_contributing_mission_data_access_v2_cop_dem_licenses.pdf` has a dedicated "Licence for the use of Copernicus DEM instances COP-DEM-GLO-30-R" section). Account registered **without** ticking the CCM checkbox (ESA-User licence). Verified: old key *and* freshly generated keys read non-CCM data (`auxdata/SRTMGL1/`) but 403 on `auxdata/CopDEM_COG/*` → account-level licence scope, not a key problem. **Fix:** edit registration in Data Space → tick "accessing Copernicus Contributing Missions data" (Public user = auto-approved) → retest | P1 terrain fetch blocked until CCM box is ticked | Fallback if ticking doesn't grant access: browser OData/HTTPS download, or AWS SRTM. Key expiry (2026-09-16) no longer constraining — keys are regenerable via API. Also learned: official S3 endpoint is `https://eodata.dataspace.copernicus.eu` (not `s3.dataspace.copernicus.eu`); key generation is scriptable: password-grant token from `identity.dataspace.copernicus.eu/auth/realms/CDSE/…/token` (`client_id=cdse-public`) → POST `https://s3-keys-manager.cloudferro.com/api/user/credentials`; fresh keys take ~10–20 s to propagate |

### 6.1 Review notes (remote-model review, 2026-09-14)

A full pass was delegated to the Windows inference server
(`scripts/llm_delegate.py`). Adjudication of its findings:

- **AWS tiles are 256×256, not 512×512** — *false positive.* Re-measured from a
  downloaded z=14 tile on 2026-09-14: 512×512 px, EPSG:3857. The reviewer
  assumed the standard 256 px slippy grid; all quoted z-level resolutions in
  §2.1 hold for this bucket. (Its side-effect suggestion — re-fetch at a higher
  zoom if needed — is already covered by the z=10/11 fetch plan.)
- **float32 position precision** — *mostly false positive* (custom double build,
  §3.1) but kept as a P0 verification item for C++ tile lookups.
- **Inland lakes baked to sea level** — *accepted*, fixed in §5.4 (per-polygon
  water levels, Mjøsa example).
- **Bucket license/attribution understated** — *accepted*, R1 strengthened.
- **OSM as primary runway geometry** — *accepted as fallback* (§5.5).
- **Symlinks in zips / bandwidth defaults** — *accepted* (§5.6, Q2).

## 7. Phased roadmap

- **P0 — Engine prerequisite** (~1–2 d): per-path `pixels_per_tile` from
  `terrain/manifest.json` in `S3DTileManager`; CH/BW regression flight.
- **P0b — Prime Meridian HUD display** (~0.5 d, independent of P0/P1): WGS84
  lat/lon line in the HUD via existing `Geo::lv95_to_wgs84` (§3.4).
- **P1 — Norway build pipeline** (~3–5 d): `build_world_terrain.py` (Copernicus
  GLO-30 STAC → LV95 tiles + mips, storage on `/Volumes/Data1`),
  `build_world_airports.py` (OurAirports → airports.json), generalise
  `convert_osm_bw.py` (baked-Y water, Norway PBF). Local test: drop `Norway/`
  into the data root, fly LSZH→ENGM.
- **P2 — Distribution** (~1–2 d): schema-v2 region entry, single-pack bundler
  path, downloader row, `licenses.json`, pack size measurement (revisit R4).
- **P3 — Small Ville (deferred)**: origin-box re-bake at 10 m/px, pre-selected
  initial download — revisit after Norway ships (§4.4).
- **P4 — Worldwide default** (later): in-game `far` tier (Natural Earth + ETOPO),
  per-region tangent rebase, Kartverket-quality Norway upgrade pack.

## See also

- [gerys_plan.md](gerys_plan.md) — the plan this subtask serves
- [SCENERY_ARCHITECTURE.md](SCENERY_ARCHITECTURE.md) / [WORLD3D.md](WORLD3D.md) — runtime & globe
- [SCENERY_DISTRIBUTION.md](SCENERY_DISTRIBUTION.md) / [SCENERY_LIBRARY.md](SCENERY_LIBRARY.md) — packs & library
- [TERRAIN_SETUP.md](TERRAIN_SETUP.md) — current CH/BW terrain provenance

