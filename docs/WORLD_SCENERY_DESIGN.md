# Worldwide & Extendable Scenery — Design

Design subtask from [gerys_plan.md](gerys_plan.md) (post-1.0.10): make ProvPilot's
scenery worldwide and extendable. First flyable pilot region: **Norway** —
data-light (no ortho), starting from coastlines and major airports, reachable by
flight from the existing Swiss / Baden-Württemberg scenery. The "Small Ville"
initial-download region (§4.4) is **deferred**.

Status: **approved for implementation** (gery decisions 2026-09-14: AWS SRTM
terrain — Copernicus GLO-30 deferred pending manual CCM licence clearance, OSM
water/coastline, OurAirports airports, 30 m/px default pack, Prime Meridian
coordinate display incl. HUD, `/Volumes/Data1` build storage).
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
| **AWS `elevation-tiles-prod` S3** (SRTM-derived) | ~30 m effective (see note) | Global land + bathymetry (measured ≈ −84…−310 m under North Sea / Barents Sea) | Plain HTTP GET, no registration. Two layouts: **raw 1°×1° tiles** `skadi/N{lat}/N{lat}E{lon}.hgt.gz` — **big-endian int16**, 3601×3601 samples, metres, nodata −32768 (verified 2026-09-14: Oslo tile 109–779 m, Tromsø 0–1336 m, Nordkapp tile valid at 71°N); and an XYZ slippy grid `geotiff/{z}/{x}/{y}.tif` (512×512 float32, EPSG:3857, nodata ≈ −3.4×10³⁸, z = 0…14) | US public data (SRTM), de-facto free | **Primary terrain source (gery decision 2026-09-14 day 2 — Copernicus deferred).** Norway span N57..N71 × E004..E031 = **420 tiles, 1.27 GB gz** (smaller than the GLO-30 estimate). Only source here with real seafloor values under the sea. |
| **Copernicus DEM GLO-30** | 30 m global (≈15 m/px native at 60°N) | Global land (**ocean coverage unverified — see §5.3**) | Copernicus Data Space STAC, collection `cop-dem-glo-30-dged-cog`: **1°×1° COG GeoTIFF items** (`Copernicus_DSM_COG_10_N57_00_E017_00_DEM.tif`). Search is anonymous; downloads need a Data Space account + S3 key (see R8 — the collection sits under the CCM licence, whose approval needs company/institution + usage statement) | Open (Copernicus/ESA) | **Deferred upgrade tier** (gery decision 2026-09-14): manual CCM clearance deemed too much friction for the pilot; revisit if SRTM quality disappoints in flight tests. Verified live on 2026-09-14 (item list + tile naming). |
| Copernicus DEM EEA-10 | 10 m, SAR InSAR | Europe (EEA) | Same STAC, collection `cop-dem-eea-10-laea-tif` (LAEA tiles, OData download links) | Open (Copernicus/EEA) | Future "quality" tier for CH/BW/Norway upgrade packs (§2.4) |
| NASADEM / SRTM 1″ | ~30 m equator → **~15 m at 60°N** | Land (gaps in mountains/poles) | USGS EarthExplorer / OpenTopography, free registration | Public domain | Better native detail for high-latitude Norway |
| National DTMs (e.g. Kartverket Norway) | 20–40 m, locally best | One country at a time | WMTS/API — **access unverified** (api.kartverket.no returned 404 on 2026-09-14) | Usually open | Per-country upgrade path (tier 2), not for the pilot |

Note on the AWS product (primary source): the **raw `skadi/` 1° tiles are the
P1 fetch target** — genuine ~30 m SRTM posting, no resampling ambiguity, and
they include bathymetry. The slippy `geotiff/` pyramid is *not* 30 m at z=14:
a spectral analysis of a z=14 tile over Sognefjord showed **99.2 % of the
energy in wavelengths > 30 m** — the upper zoom levels are resamplings of ~30 m
(SRTM-3″-class) data (nominal grid spacing at 60°N: z=10 ≈ 38 m/px, z=12 ≈
9.5 m/px, z=14 ≈ 2.4 m/px). The pyramid remains useful for quick visual
previews and the global far layer, not for the Norway build.

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
2. Fetch the covering **AWS `skadi/` SRTM 1° tiles** from
   `https://s3.amazonaws.com/elevation-tiles-prod/skadi/N{lat}/N{lat}E{lon}.hgt.gz`
   (anonymous HTTP GET; full Norway span = 420 tiles / 1.27 GB gz, land subset
   far smaller) into the raw cache. Format: **big-endian int16**, 3601×3601 per
   degree (SE-corner convention already matches ours), metres, nodata −32768;
   ocean pixels carry real bathymetry (~−300 m). Reproject each needed cell
   WGS84→LV95 with `rasterio.warp.reproject` into a `tile_px × tile_px` buffer.
   SRTM is natively ~15 m/px at 60°N, so the 30 m target is a *downsample*,
   not an upsample. (Copernicus GLO-30 via Data Space STAC remains the swap-in
   alternative — same 1° tile addressing — if CCM clearance ever lands.)
3. Write `tile_{E}_{N}.raw` (float32 LE) + manifest with
   `pixels_per_tile`, `pixel_resolution_m`, per-tile min/max elevation.
4. Build mip2/mip3/mip4 pyramids for far chunks (existing mipmap logic).

Storage (gery decision 2026-09-14): all downloads and intermediate processing go
to the external volume **`/Volumes/Data1`** (3.6 TiB, ~1.1 TiB free at design
time), following the existing drive conventions (`scenery_in_bw`,
`provpilot/build/bw-*`):

```
/Volumes/Data1/scenery_in_norway/          # raw inputs, versioned cache
├── srtm/                                   # AWS skadi SRTM 1° .hgt.gz tiles
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

Bathymetry: **resolved in favour of the primary source** — the AWS SRTM tiles
carry real seafloor values (measured −84…−310 m over North Sea / Barents Sea
tiles, 2026-09-14), so margin cells under the +1.5 m water plane get genuine
shelf slopes instead of a flat 0 m floor. If we ever swap in GLO-30 (a land
DEM with unverified ocean coverage), re-check one coastal tile (`gdalinfo` +
sample values) before baking.

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
| R1 | Copernicus Data Space access is free but account-gated (S3 key for downloads) + CCM licence approval (see R8), no SLA; ESA data policy requires attribution. Primary pipeline (AWS SRTM bucket) has the same no-SLA character | Only gates a future GLO-30 swap-in — primary fetch is the no-registration AWS SRTM bucket | Cache fetched 1° tiles in the versioned `/Volumes/Data1/scenery_in_norway/srtm/` store (rebuildable without network); one-off fetch, not runtime. Attribution strings in `licenses.json` regardless |
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
| R7 | GLO-30 ocean coverage unverified (land DEM) | Only matters if the GLO-30 swap-in is ever used | **Moot for v1:** primary AWS SRTM source has measured bathymetry (−84…−310 m, §5.3); verify a coastal tile only when/if swapping to GLO-30 |
| R8 | GLO-30 403 **root cause identified 2026-09-14**: COP-DEM-GLO-30-R is filed under **Copernicus Contributing Missions (CCM)** in Data Space (collection provider URL `…/copernicus-contributing-missions/collections-description/COP-DEM`; license PDF `copernicus_contributing_mission_data_access_v2_cop_dem_licenses.pdf` has a dedicated "Licence for the use of Copernicus DEM instances COP-DEM-GLO-30-R" section). Account registered **without** ticking the CCM checkbox (ESA-User licence). Verified: old key *and* freshly generated keys read non-CCM data (`auxdata/SRTMGL1/`) but 403 on `auxdata/CopDEM_COG/*` → account-level licence scope, not a key problem. **Fix:** edit registration in Data Space → tick "accessing Copernicus Contributing Missions data" → retest. **Decision 2026-09-14 (gery):** ticking the box requires company/institution + a usage statement → manual clearance deemed too much friction; AWS SRTM promoted to primary (§2.1), Copernicus kept as optional swap-in (account stays valid) | **No longer blocking** — only gates a future GLO-30 quality tier | If revisiting: browser OData/HTTPS download works without S3 keys. Key expiry (2026-09-16) no longer constraining — keys are regenerable via API. Also learned: official S3 endpoint is `https://eodata.dataspace.copernicus.eu` (not `s3.dataspace.copernicus.eu`); key generation is scriptable: password-grant token from `identity.dataspace.copernicus.eu/auth/realms/CDSE/…/token` (`client_id=cdse-public`) → POST `https://s3-keys-manager.cloudferro.com/api/user/credentials`; fresh keys take ~10–20 s to propagate |

### 6.1 Review notes (remote-model review, 2026-09-14)

A full pass was delegated to the Windows inference server
(`scripts/llm_delegate.py`). Adjudication of its findings:

- **AWS tiles are 256×256, not 512×512** — *false positive.* Re-measured from a
  downloaded z=14 tile on 2026-09-14: 512×512 px, EPSG:3857. The reviewer
  assumed the standard 256 px slippy grid; all quoted z-level resolutions in
  §2.1 hold for this bucket. (Its side-effect suggestion — re-fetch at a higher
  zoom if needed — is moot for P1, which now fetches raw `skadi/` 1° tiles.)
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
- **P1 — Norway build pipeline** (~3–5 d): `build_world_terrain.py` (AWS skadi
  SRTM 1° tiles → LV95 tiles + mips, storage on `/Volumes/Data1`),
  `build_world_airports.py` (OurAirports → airports.json), generalise
  `convert_osm_bw.py` (baked-Y water, Norway PBF). Local test: drop `Norway/`
  into the data root, fly LSZH→ENGM.
- **P2 — Distribution** (~1–2 d): schema-v2 region entry, single-pack bundler
  path, downloader row, `licenses.json`, pack size measurement (revisit R4).
- **P3 — Small Ville (deferred)**: origin-box re-bake at 10 m/px, pre-selected
  initial download — revisit after Norway ships (§4.4).
- **P4 — Worldwide default** (later): in-game `far` tier (Natural Earth + ETOPO),
  per-region tangent rebase, Kartverket-quality Norway upgrade pack.

## 8. Continuation plan — decisions 2026-09-16

After the Norway pilot bake, four decisions were made (gery, 2026-09-16). This
section supersedes the parts of §5.6 / §6 / §7 it conflicts with.

| # | Decision |
|---|----------|
| D1 | Split the combined Fennoscandia bake into **three region packs: Norway, Sweden, Finland** (supersedes §5.6 item 1 "single `norway` region, no quadrants") |
| D2 | Worldwide strategy = **Option A**: keep the LV95 runtime frame; the world3d globe's tangent point tracks the aircraft continuously (per-frame `_rebase_globe_to`, design §3.2). True ECEF floating-origin world deferred until a non-European region demands it |
| D3 | World3D work order: **face-grid rebake + suppression rectangle first, texture atlas next** |
| D4 | High-res Norway terrain ships as an **opt-in addon pack** (overlay region, higher path priority than the `norway` base). Source v1: **Copernicus EEA-10** (verified 2026-09-14; covers all of Fennoscandia → Sweden/Finland addons reuse the same pipeline); Kartverket ASR stays a v2 upgrade if access is confirmed (unverified, §2.1) |

### 8.1 Phase plan

**Phase 1 — Pack split (D1).** Parameterize `build_world_terrain.py` with
`--country {norway,sweden,finland}` (per-country WGS84 bbox + UTM zones; LV95
bbox derived from the corners and rounded outward onto the 1024 m grid). Add
`sweden` / `finland` region entries to `scenery_regions.json` (2×2 quadrants,
uniform schema). Bake both new countries; Norway reuses its existing bake.
Bundle three base packs + downloader rows. Verify: LSZH→ENGM, ENGM→ESSA,
ESSA→EHTK legs; seam check at pack boundaries.

**Phase 2 — Continuous rebase + suppression rectangle (D2 + D3a).** Replace the
one-shot destination rebase (`node_3d.gd::_rebase_globe_to`) with per-frame
tracking while the AC is >~100 m from the current tangent point — in flight
the globe moves at aircraft speed, so there are no discrete transform events
(the ~150 km Oslo sag corrects itself gradually). `W3DGlobe` gains a static
suppression rectangle (union of enabled-region bboxes): chunks inside it are
culled because the Scenery3D patch is authoritative; the globe stays visible
over open ocean and closes the <25 km gap. Verify: 70 km pass over Oslo —
single surface, no snap; rebase cost ≪10 µs/frame; chunk-commit counter flat.

*Implemented (2026-09-16, branch `globe-rebase-suppress`, both repos).*
Tracking threshold is 500 m (`GLOBE_REBASE_TRACK_M`), not 100 m — the re-anchor
is a quantized event at most once per 500 m of travel, and the residual sag at
500 m is ~2 cm. The one-shot destination rebase stays as the initial anchor
during fly-loading (behind the intro zoom); `_track_globe_rebase()` in `node_3d.gd::_process`
re-anchors quietly afterwards (the `quiet` flag suppresses the log line). The
suppression API is `W3DGlobe::add_suppress_rect()/clear_suppress_rects()` —
**one rectangle per enabled region's terrain-manifest grid** (not a bounding
box, so seas between regions still show the globe); chunks whose ECEF centre
projects into any rect via the mount transform are culled in `update_node`
alongside horizon cull. Rectangles refresh at startup (after the settings
screen loads), on first flight, and when the settings screen closes. Known
approximation: the swisstopo LV95→WGS84 polynomial drifts tens of km at 70°N,
which shifts globe terrain features there but not the sag correction. Smoke
tests: `provpilot/tests/smoke_globe_suppress.gd` (API) and
`smoke_globe_tracking.gd` (anchor follows a 17 km AC jump). Remaining in-game
verification: the 70 km Oslo pass above, plus rebase cost / chunk-commit churn.

*Hole fix (2026-09-17, same branch).* First test flight showed holes to space
at altitude: the static full-extent rects (each country ~1500×1700 km) culled
the coarse globe across all of Fennoscandia, but Scenery3D only **streams**
detailed terrain inside a Chebyshev square of `far_radius` tiles (~205 km
half-side) around the **active camera** — beyond that square there were no
detailed tiles yet, so suppression left gaps that "filled in" on approach.
Fix: `_load_globe_suppress_regions()` caches the region extents once;
`_refresh_globe_suppress_rects()` (per frame from `_process`) clips each to a
square of `far_radius·1024 − GLOBE_SUPPRESS_MARGIN_M` (75 km = max-level chunk
half-edge ~58 km + LV95-drift pad) around the viewport's current camera — the
same centre Scenery3D streams from. Every culled chunk now sits fully inside
loaded coverage; at the streaming edge a double-surface sliver remains
(pre-Phase-2 look), which is cheaper than a hole. Companion world3d change:
the suppression test moved **after** the split decision in `update_node`, so
suppressed chunks near the camera still subdivide and only leaf-size pieces
are culled (a coarse parent culled by centre alone could hide up to a full
chunk edge past the coverage). Smoke test: `tests/smoke_globe_suppress_clip.gd`
(rects follow the camera, clip at the coverage radius).

*Hole fix, iteration 2 (2026-09-17, same branch).* The Fennoscandia test
flight still showed dark squares and stars seen through the globe during the
intro zoom: clipping to the streaming square assumed everything inside it was
**loaded**, but Scenery3D fills the far zone with chunks at
`CHUNKS_PER_FRAME = 1` (close-ring tiles at `load_budget`/frame), so for a
while after any teleport most of the square has no geometry yet — and the
old `GLOBE_SHOW_ALT_M = 25 km` gate in src/sky_atmosphere.cpp hid the coarse globe
entirely below 25 km, removing even the fallback. Fix: suppression now
follows **live coverage**. `S3DTileManager::get_coverage_grid()` reports a
per-cell (chunk-aligned) resident-geometry mask around the camera plus a
`get_coverage_generation()` counter; `_refresh_globe_suppress_rects()` merges
covered cells into maximal rectangles, clips them to each region's terrain
extent, insets by `GLOBE_SUPPRESS_MARGIN_M` and pushes them — refreshed only
when the camera moves ≥32 m or the generation changes. The 25 km visibility
gate is gone: the coarse globe is the fallback surface at every altitude and
is culled cell-by-cell as detailed geometry arrives, so a hole to space can
no longer exist (unloaded cells simply show the globe). Smoke test updated:
`tests/smoke_globe_suppress_clip.gd` now feeds synthetic coverage grids
(full / sparse) through the production conversion path.

*Hole fix, iteration 3 (2026-09-17, same branch).* BW test flight over the
Lake Constance area at ~7.5 km: the coarse globe **poked through** the local
near/far terrain in dendritic light-green patches (the zero-crossing contours
of ETOPO − local DEM; in flat lowlands the two differ by only a few metres, so
whichever is higher wins the depth test). Root cause: culling was
**centre-based** with a 75 km inset — built to keep centre-culled max-level
chunks (half-edge ~58 km) inside coverage. BW's data band is only ~170 km
wide, so after the 2×75 km inset only a ~20 km core actually suppressed the
globe; everywhere else both surfaces rendered and the globe won wherever
ETOPO ≥ local DEM. Fix (world3d + node_3d.gd): culling is now
**containment-based** — `W3DGlobe::chunk_fully_suppressed()` culls a chunk
only when ALL FOUR ground corners (`ecef_at` at the face-space corners,
mapped through the mount) lie inside ONE suppression rectangle (centre test
kept as a cheap early-out; rectangles are convex so it is necessary). A culled
chunk therefore always sits fully under resident geometry and no large inset
is needed: `GLOBE_SUPPRESS_MARGIN_M` drops 75 km → **10 km**, now covering
only the measured LV95-linear-vs-ECEF-tangent misalignment (≤ ~3.3 km over
the BW grid, grows with distance from TERRAIN_CENTER) plus tile-edge
quantization. Chunks straddling the coverage edge stay visible as fallback;
the double-surface sliver is bounded by ~half a max-level chunk edge instead
of 75 km. Containment is tested in FLAT coordinates (the frame Scenery3D
places meshes in), so hole-safety holds even where the globe's geographic
features are offset from local terrain — measured 24 km at 59°N rising to
~207 km at 71°N/25°E (swisstopo anchor polynomial vs the pyproj CH1903+
mapping `build_world_terrain.py` bakes with; that feature misalignment is a
pre-existing Phase 3 concern, and suppression now covers MORE of Fennoscandia
than before, not less). Smoke test unchanged (its +120/+300 km assertions
still hold).

*Hole fix, iteration 4 (2026-09-18, branch `settings-cpp`).* Stuttgart test
flight at 460 m AGL: the coarse globe still poked through in dendritic
light-green patches across the whole mid-distance. Root cause is geometric:
containment culls per **globe chunk**, and the max-level-6 leaf over
Stuttgart spans LV95 E 2651–2822 km × N 1361–1488 km (171 × 127 km), while
the BW rectangle after the 10 km inset ends at E 2816 km — the chunk
overshoots by ~6 km, is never culled, and wins the depth test wherever
ETOPO ≥ local DEM. Whether a chunk fits is luck of the cube-face grid, and a
chunk spanning two regions (CH/BW at Lake Constance) can never fit because
rectangles are clipped per region. Fix: suppression moved from **per chunk**
to **per fragment**. `W3DGlobe` renders with a ShaderMaterial that samples a
coverage-mask texture (one texel per Scenery3D chunk cell, world-XZ → cell via
an origin/step uniform pair) and `discard`s fragments in covered cells; the
host uploads the tile manager's live coverage grid straight into that mask
(`set_coverage_mask()`), with no region clipping, no margin and no rectangle
merging. Chunk size, region extents and the containment test are gone. The
globe ends exactly at the resident far-chunk boundary; unloaded and no-data
cells (sea, gaps) keep showing it as the fallback. This is step A of §9 and
is frame-independent: only the world-XZ → cell mapping changes in step C.

*Iteration 4b (same day).* Check flight Stuttgart → Lake Constance was clean,
but at the eastern data edge near Lindau the globe showed under resident
tiles in a band along the edge: the mask was one cell per 8 km chunk cell,
and inside the close ring a cell counted as covered only when all 64 tiles
had a mesh — an edge cell with one no-data tile left its 63 resident tiles
unmasked. The mask is now **tile resolution** (1 km): `S3DTileManager::
get_coverage_mask()` stamps every loaded far-zone chunk's footprint and every
resident individual tile into a (2r+1)² byte grid (r = far_radius +
unload_margin + chunk_size = 210 tiles, ~180 KB), built in C++ so the host
just uploads it. A no-data tile shows the globe on that tile only.

*Iteration 4c (same day, Oslo check flight).* Slewing south of Oslo at
19 km showed sky through 1 km-wide E–W bands ~15 tiles ahead that filled on
approach. Headless probe (`tests/smoke_mask_consistency.tscn`: every
mask-covered tile must have a visible node with a full-footprint mesh)
reproduced it: Norway's 34 px tiles at LOD 6 (outer ring, stride 64 > 33 px)
made `S3DTile::generate_mesh()` bail with **no mesh**, the far chunk had
already been dropped because the tiles counted as present, and the mask
stamped them because only `node != null` was checked. A pre-existing
Scenery3D hole (previously hidden by the globe underneath) that the mask
turned into sky. Fixes: the mesh stride is clamped to the tile's pixel count
(a 34 px tile at LOD 6 becomes a 2×2 quad), and the mask requires
`node->get_mesh()` to be valid. Probe: 0 phantom tiles over a 225 km slew.

**Phase 3 — Face rebake, then atlas (D3b/c).** Faces are baked at
`face_size=513` (~20 km grid spacing), which discards everything finer in the
ETOPO source regardless of chunk LOD. Rebake at ≤~5 km spacing (measure bake
time, pvpk size, `face_grid` RAM: 2049 ≈ 100 MB, 4097 ≈ 400 MB). Then replace
the procedural elevation color ramp with a single baked face-texture **atlas**
(UVs in `compute_chunk`, one shared material).

**Phase 4 — Norway DTM addon (D4).** New `scripts/build_fenno_dtm.py`: fetch
Copernicus EEA-10 LAEA tiles → resample onto the LV95 grid at native 10 m
(`pixels_per_tile: 103`, `pixel_resolution_m: 10.0` — per-path resolution is
already supported, P0/R3) → land mask + ~15 km coastal margin (ocean stays
with globe/ETOPO) → overlay manifest following the Switzerland-Bathymetry
precedent (`"overlay": true`, 0.0 = fall-through, listed after `norway`).
New opt-in pack `norway-dtm-base-v1` (~20 GB est.; quadrant-split if heavy).
Verify: low-altitude Sognefjord pass — 10 m relief beats the 30 m base; no
JSBSim ground spike on overlay handoff (NAN-skip in
`TerrainElevation::update_terrain_elevation`).

Phases 1 and 2 are independent (data pipeline vs runtime) and can run in
parallel, each on its own branch.

### 8.2 Roadmap deltas

- §7 **P4** "per-region tangent rebase" → implemented as Phase 2 continuous
  tracking; "Kartverket-quality Norway upgrade pack" → Phase 4 (EEA-10 v1).
- R5 (flat-frame seam above ~25 km) → mitigated by Phase 2 (rebase +
  suppression), no longer "accepted v1".
- R2 (blocky 30 m terrain) → fixed per-region by Phase 4.

## 9. Worldwide frame — decision 2026-09-18

Asked while reviewing the iteration-4 poke-through: *"for a world wide
scenery, does the centre need to be the Earth's core?"* Answer: **yes for the
runtime frame, no for the data.** This supersedes D2 (Option A).

### 9.1 Why the LV95 flat frame cannot scale

Three coordinate frames are in play today and they only agree near Switzerland:

| Frame | Where | Limit |
|-------|-------|-------|
| LV95 flat plane (X = West, Z = North, Y = ASL) | Scenery3D tile placement, `TERRAIN_CENTER_E/N` | Oblique Mercator tangent at Bern: fine across Europe, scale unbounded with distance, undefined a quarter-Earth away. No Americas, no Asia. |
| swisstopo approximation polynomial | `src/geo.cpp`, `s3d_coords.cpp`, globe mount, HUD, airports | Valid inside CH only. Measured globe-feature misplacement 24 km at 59°N, 207 km at 71°N (§8.1 iter 3) — the Norway bake used the rigorous projection, the runtime does not. |
| JSBSim runs at a *fake* location near 0°N 0°E with a linear flat mapping (lat = Z/R, lon = X/R, heading mirrored) | `src/jsbgodot.cpp` `initialize_at_terrain()` / `lat_lon_alt_to_local()` | Consistent with the grid (no drift), but JSBSim's psi is a GRID heading, gravity/Coriolis are equatorial, and N–S ground distance is scaled by a/M(0°) ≈ 1.0067. True runway bearings used as grid headings misalign by the convergence (−2.7° Oslo, −18° Kirkenes). *(Corrected 2026-09-18: an earlier version of this row claimed a convergence drift in the position; the fake-location mapping has none.)* |

The flat plane also puts Scandinavian tiles 150–500 km above the ellipsoid,
which is what forces the per-500 m globe rebase (Phase 2).

### 9.2 Target architecture

| # | Decision |
|---|----------|
| D5 | **Runtime world = ECEF** (origin at Earth centre, +Z north pole, double precision). Aircraft, cameras, sky live there; no rebasing, no floating origin. "Up" is the ellipsoid normal; Y-up code gets a per-frame east-north-up basis at the aircraft. JSBSim is native ECEF, so the bridge simplifies. Sky shader `planet_center` becomes a constant 0. |
| D6 | **Region packs stay projected and declare their CRS** (`"crs": "EPSG:2056"` in the terrain manifest; CH/BW unchanged, no rebake). Scenery3D builds each tile mesh through the region's rigorous projection → geodetic → ECEF relative to the tile's own centre (like world3d chunks). 1 km GLB tiles (buildings/water/roads/vegetation) are placed with the tile's ENU transform (2 cm curvature per tile, no re-export). Streaming/LOD keep tile indices; only camera → tile-index conversion changes. Any region anywhere uses its local UTM zone. |
| D7 | **One shared rigorous projection library** (exact LV95 Hotine oblique Mercator, transverse Mercator/UTM, WGS84 ↔ ECEF) replaces the polynomial and the equirectangular bridge in `geo.cpp`, `s3d_coords.cpp`, `W3DCoords`, `jsbgodot.cpp`. Fixes the Norway misalignment and JSBSim drift independently of D5. |
| D8 | **Globe suppression is per fragment** (§8.1 iter 4), written in cell space so it survives D5 unchanged. |
| D9 | **Heights**: ASL is treated as height above the ellipsoid everywhere (as the globe mount already does). The geoid offset is consistent across terrain, globe and aircraft. |

Rebake is **optional**: Scandinavia already uses the rigorous LV95 projection
and lines up once the runtime does too (D7). If rebaked with the 10 m EEA data
(Phase 4), use EPSG:25833 (Norway's national grid). ETOPO faces unchanged.

### 9.3 Order of work

| Step | Scope | Verification |
|------|-------|--------------|
| A. Fragment mask | world3d ShaderMaterial + `set_coverage_mask()`; `node_3d.gd` uploads the live coverage grid | headless `smoke_globe_suppress*.gd`; check flight Stuttgart + Lake Constance |
| B. Projection library | `geo.cpp`, `s3d_coords.cpp`, `W3DCoords`, JSBSim bridge | offline against pyproj; flight to ENGM (globe and scenery aligned, heading vs runway) |
| C. ECEF world | Scenery3D on the ellipsoid; host: cameras, slew, HUD, VFR map, intro zoom, atmosphere | one headless smoke per milestone + a check flight each |
| D. Optional Scandinavia rebake (UTM 33, 10 m) | `build_world_terrain.py --crs` | with Phase 4 |

A and B are safe on their own. C is the answer to the question and is the
prerequisite for any non-European region.

### 9.4 Step B — implemented 2026-09-18

`provpilot/addons/scenery3d/src/s3d_proj.h` (header-only, no Godot deps) is
the shared library: rigorous Swiss oblique Mercator on Bessel 1841 + the
CH1903+/WGS84 geocentric shift (EPSG:2056 exactly as pyproj does it without
the CHENyx06 grid), transverse Mercator/UTM (Krüger n⁴), grid convergence,
and a `Crs` value (LV95 | UTM zone) with `from_epsg()` for D6 manifests.
Verified against pyproj 3.7.2 at 8 points from Geneva to Kirkenes: forward
deviation ≤ 0.1 mm for both LV95 and UTM 33; round trip ≤ 8 mm (the 2D
datum-shift convention pyproj shares). The old polynomial was off by 1.3 km
at Oslo, 20 km at Tromsø and 32 km at Kirkenes.

Consumers switched: `S3DCoords`, ProVPilot `Geo` (adds
`lv95_grid_convergence_deg`, `wgs84_to_utm`, `utm_to_wgs84`, `utm_zone_for`;
the ProVPilot build adds the submodule's `src/` to CPPPATH), `VfrMap`
(airport/canton overlays now land on the pyproj-baked map tiles), and
`AirportsLib`, which drops its GDScript copy of the polynomial and now emits
`grid_heading_deg` per runway (true bearing + convergence at the airport);
thresholds and the welcome-screen spawn heading use it, the picker label keeps
the true bearing, and the HUD shows true heading (grid − convergence). The
JSBSim bridge itself is unchanged: it is rewritten wholesale in step C.
Smoke test: `provpilot/tests/smoke_geo_proj.gd`.

### 9.5 Step C — ECEF world: design and milestones (2026-09-18)

**Frame.** Godot world = WGS84 ECEF metres (origin Earth centre, +Z north
pole, +X through 0°/0°), double precision, no rebasing. "Up" is the local
ellipsoid normal; nothing global is Y-up any more. `W3DCoords`
(`geodetic_to_ecef`, `ecef_to_geodetic`, `enu_to_ecef_transform(lat, lon, h,
x_axis_west=true)`) provides the local **WUN** frame (+X West, +Y Up, +Z
North — the existing Scenery3D tile convention) at any point.

**Scenery3D placement (engine).** A `frame_mode` property on `Scenery3D`
(`flat` = today, `ecef` = new) keeps the flat path as a fallback during the
migration. In `ecef` mode:
- Tiles, chunks and every GLB/MultiMesh tile keep their tile-local mesh
  coordinates; the node transform becomes `WUN(lat_c, lon_c, 0) × A`, where
  (lat_c, lon_c) is the tile centre through the region CRS inverse and `A`
  is the 2×2 Jacobian of the CRS inverse at the centre (grid E/N → local
  metres east/north; carries the LV95 scale and grid convergence, so a tile
  is rotated by −convergence relative to true north). Vertices also get a
  curvature sag `y -= (dx² + dz²) / 2R` about the tile centre in
  `generate_mesh` — 2 cm at a 1 km tile corner (skipped for GLBs), 1.25 m at
  an 8 km chunk corner (kept). Neighbouring tiles compute their shared edge
  from different centres; the mismatch is second order (mm at 1 km, cm at
  8 km), below the existing 30 m chunk bias and ETOPO error.
- Streaming: camera ECEF → geodetic → CRS forward → tile index, once per
  update. Load/far radii stay in tiles.
- Elevation DB stays in grid E/N; hosts convert ECEF → geodetic → E/N
  (`S3DElevationDB::get_elevation_ecef` helper) — one projection per query.
- Chunk Y bias and water/road vertical offsets move along the node's local
  up (they are inside the node transform, so nothing changes in the meshes).
- Region CRS (D6): `terrain/manifest.json` may carry `"crs": "EPSG:2056"`
  (default) or a UTM code; the tile grid of a region is expressed in its
  CRS. Milestone 1 keeps one grid CRS for all paths (LV95); per-region grids
  are milestone 3.

**world3d.** `GlobeMount` becomes identity (globe at the ECEF origin);
`_rebase_globe_to` / `_track_globe_rebase` / `_ecef_from_flat` go away. The
coverage-mask shader maps a fragment's ECEF position → geodetic → grid E/N
in GLSL (rigorous LV95 forward, ~60 flops; UTM later) instead of the affine
world-XZ mapping; the mask itself (tile-resolution bytes in grid E/N) is
unchanged.

**JSBSim bridge.** JSBSim runs at the TRUE location: ICs via
`SetGeodLatitudeDegIC` / `SetLongitudeDegIC` / `SetAltitudeASLFtIC` /
`SetPsiDegIC` (true heading). Each tick the aircraft node gets
`origin = GetLocation()` (ECEF, ft → m) and `basis = Tb2ec × M`, where `M`
maps JSBSim body axes (X fwd, Y right, Z down) onto the model's Godot axes
(−Z fwd, +X right, +Y up). No fake location, no heading mirror, no
equirectangular reference; gravity/Coriolis at the real latitude. Slew mode
edits the node transform in the aircraft's local WUN frame and re-inits
JSBSim from geodetic(pose) + heading = yaw of the basis in the local frame.
Terrain feed: `set_terrain_elevation(h)` unchanged (ellipsoidal, D9).

**Host.** Cameras stay parented to the aircraft (they inherit the local
frame); the intro-zoom camera already works in ECEF. Sky: `planet_center =
(0,0,0)` constant, sun direction = ECEF unit vector from the ephemeris,
altitude ramps from `|p| − R(lat)` (geodetic h). HUD: geodetic from ECEF,
LV95 from `Geo`. VFR map: fed lat/lon → LV95. Spawn/teleport: airport
LV95 → geodetic → ECEF. `TERRAIN_CENTER_E/N` disappears from the runtime
(kept only as the default grid origin inside Scenery3D flat mode).

**Milestones** (each: headless smoke test, then one check flight).

| # | Scope | Test |
|---|-------|------|
| C1 | Scenery3D `ecef` mode: tile/chunk/GLB/vegetation placement, sag, streaming index, elevation helper; world3d mask shader in ECEF | scenery3d headless: stream at LSZH in ecef mode, tile transforms match `WUN(centre)`, elevation query round-trips; world3d demo shot with a mask |
*C1 implemented (2026-09-18).* scenery3d: `S3DFrame` (`s3d_frame.h`, held by
value in every manager; `frame_mode` property on `Scenery3D` fans out, the
host-wired vegetation manager has its own). `S3DTile::set_placement()`
replaces every `set_position`: 1 km tiles and all GLB/MultiMesh tiles are
affine (`WUN(centre) × grid-Jacobian × translate(anchor − centre)`), terrain
tiles subtract the sag in `generate_mesh`, 8 km chunks are placed **exactly**
(each vertex projected; an affine 8 km chunk would mismatch ~1 m at seams
1400 km from Bern where the LV95 scale is 1.024). Water/road draping samples
the DTM in grid E/N (`S3DElevationDB::get_elevation_grid`) and applies the
sag; `Scenery3D` gains `get_elevation_grid/geodetic/world`, `S3DCoords`
gains `frame_mode` and `wgs84_to_world_h`. world3d: mask frame 1 (fragment
ECEF → LV95 in the shader). Headless test `tests/smoke_scenery_ecef.tscn`
(standalone Scenery3D in ECEF over LSZH): tile corners back on the LV95
grid within 1.2 mm, up axis = ellipsoid normal, chunk bases orthonormal at
−30 m, elevation via geodetic/world entry points identical.
| C2 | Host on ECEF: node tree, JSBSim bridge, cameras, slew, HUD, sky, VFR map, spawn/teleport, intro zoom; globe at identity | app headless: spawn LSZH, `|AC.origin|` ≈ R+h, JSBSim lat/lon == spawn, tile manager streams; check flights LSZH then ENGM (runway alignment, coastline, horizon at altitude) |
*C2 implemented (2026-09-18, awaiting check flights).* `src/geo_frame.h`
(ECEF ↔ geodetic, WUN basis, aircraft basis/heading helpers). JSBGodot
`ecef_frame`: ICs via `SetGeodLatitudeDegIC`/`SetLongitudeDegIC` at the true
location with the TRUE heading, per tick `origin = GetLocation()` (ft → m)
and `basis = Tb2ec` columns mapped body (X fwd, Y right, Z down) → node
(X = −right, Y = up, Z = fwd) with the node scale preserved; no fake
location, no heading mirror; `reinitialize_from_pose(ac_global, kts,
terrain)` replaces the flat re-inits; `get_pitch_deg/get_roll_deg` added.
SlewMode/TerrainElevation/CameraRig/IntroZoom/Atmosphere gained `set_ecef`
(local WUN frame per update, pin height along local up, orbit/chase in the
local frame, intro straight up the normal with a north-up hint, geodetic
altitude ramps); VfrMap `update_lv95`. node_3d.gd: `_ac_transform_at`,
`_ac_true_heading`, `_geodetic_of`, `_ecef_of_lv95`; elevation queries are
geodetic; spawn/teleport build ECEF poses; HUD shows ECEF + LV95 + true
heading; the rebase machinery is gone and GlobeMount is the identity with
mask frame 1. src/sky_atmosphere.cpp derives the observer from the camera's
geodetic position, keeps the scattering sphere tangent under the camera
(centre = cam − n·(R+h)) and rotates the NOAA sun vector into ECEF.
Scene: `frame_mode = 1` on Scenery3D and Vegetation; the welcome screen
passes TRUE runway headings again (thresholds stay in the grid). Headless:
`tests/smoke_ecef_spawn.tscn` (welcome path at LSZH: node and JSBSim agree
on lat/lon/heading/altitude/pitch/roll to 0.01, ground creep forward only,
3681 tiles around the ECEF camera); aircraft-swap, VFR-map, globe-mask and
projection smokes updated/green.

*C2 check-flight fixes (2026-09-18).* (1) Orbit/chase cameras drifted and
rolled: `Node3D::look_at()` rejects a target within `CMP_EPSILON·|position|`
of the node as "the same position" — 64 m at ECEF magnitudes — so every
close aim silently failed and the camera kept the aircraft-relative basis.
`geo::aim()` (Basis::looking_at + set_global_transform, scale preserved)
replaces every look_at in CameraRig and IntroZoom; `tests/smoke_ecef_camera.
tscn` drives both modes headlessly (0° roll, 0° look-off). Rule: never call
look_at / rely on is_equal_approx on world positions in the ECEF world.
(2) Roll and yaw reversed: the flat frame ran JSBSim mirrored and all input
paths + control-surface animations were tuned to the mirror; in ECEF mode
the aileron and rudder commands are negated at the FDM boundary only, so
input_* and the animations stay as before. (3) Coverage-mask rebuilds are
throttled to 100 ms (the generation counter bumps every frame during heavy
streaming; each rebuild is a 180 KB grid + texture upload).
(4) Pre-flight NaN flood: the AC node kept the scene file's flat transform
(~2 km from the Earth's centre in ECEF) until the first flight; every
geodetic conversion was degenerate and the VFR map normalised NaN vectors
each frame (5.7 M warnings per run, welcome screen unresponsive). The
aircraft is parked on the ellipsoid at the LV95 origin in `_ready()`, VFR
feed and sky observer skip off-planet positions, and `ecef_to_geodetic`
returns a finite answer for degenerate input.
(5) Thin gap lines between Scandinavian tiles (check flight): the affine tile
placement used one grid-convergence rotation per tile centre, so shared
edges of neighbouring 1 km tiles were computed from different frames —
5–10 cm apart (the seam probe in `tests/smoke_scenery_ecef.tscn` measured
0.096 m at LSZH, 0.074 m at ENGM). Tiles now project their four corners
exactly, interpolate the interior bilinearly and lean the height direction
to the local normal (`tilt = 1 + h/R`), so a shared edge is computed from
the same two corner points on both sides: 0.3 mm. (A 3.2 m vertical step
between LOD 1 and LOD 2 tiles at LSZH is Scenery3D's pre-existing edge
sampling — reported by the probe, not asserted.)
(6) Remaining thin lines over Norway at 7.6 km were T-junction cracks between
LOD rings (the finer tile's intermediate edge vertices lie above/below the
coarser neighbour's straight edge; in the ECEF world the sky shows through
because the globe is discarded under resident tiles). Every terrain tile and
chunk now carries **skirts** (mesh surface 1: a vertical strip hanging from
each border edge, depth = max(20 m, 0.3 × vertex spacing), both windings),
the standard remedy; the LOD edge sampling itself is unchanged. Fog probe
(`tests/probe_fog.gd`) confirms the altitude ramps are unchanged by the ECEF
move: density 5.0e-5 at 500 m, 4.7e-5 at 7.6 km, 2.7e-5 at 30 km, off at
62 km.
| C3 | Remove flat-frame code paths (rebase, TERRAIN_CENTER), per-region CRS grids with UTM support (D6), docs | smoke tests green; existing CH/BW/Norway packs unchanged |

*C3 implemented (2026-09-18).* Host: the JSBSim bridge lost its flat branch
(fake location, equirectangular reference, heading mirror, `adjust_origin`,
the two flat re-inits; 1438 → 1085 lines), SlewMode/TerrainElevation/
CameraRig/IntroZoom lost their `set_ecef` switches (ECEF only), the dead C++
`Atmosphere` module (never updated since src/sky_atmosphere.cpp took over) is
deleted, `VfrMap` keeps only `update_lv95` and `setup(config_path)`;
`TERRAIN_CENTER_E/N` survives solely as the LV95 home point (park position,
initial sky observer). Scenery3D keeps `frame_mode 0` for its own demo.
**Per-region CRS (D6):** the terrain manifest `"crs"` field is now read and
enforced — this build streams a single LV95 grid, and a path declaring
another CRS is skipped with a clear warning instead of being placed on the
wrong grid. Full multi-CRS streaming (per-region tile keys/rings/chunks,
per-CRS coverage masks and a UTM branch in the globe mask shader) is
deferred until the first non-LV95 pack exists; the projection library
already has the UTM maths and `Crs::from_epsg()`. Docs updated:
`WORLD3D.md`, `TERRAIN_SETUP.md`, scenery3d and world3d READMEs.

*Check-flight fix (2026-09-18, ground steering).* Nose-wheel steering turned
the wrong way on the ground while the rudder was right in the air: the
bridge had always written `fcs/steer-cmd-norm = -input_rudder` (tuned to
the mirrored world) and only the rudder command got the un-mirroring flip.
JSBSim's two commands have opposite sign conventions for the same pedal
(right pedal = rudder-cmd −1, steer-cmd +1 — the FlightGear convention), so
the steering command is now `+input_rudder`. `JSBGodot.get_property(name)`
added for diagnostics; `tests/smoke_ground_steering.tscn` taxis with both
pedal signs and yaws in the air, and asserts all three agree.

*Startup / settings-save latency (2026-09-18).* `set_data_paths` JSON-parsed
every terrain `manifest.json` in full (twice, for `pixels_per_tile` and
`crs`); the Fennoscandia manifests list every tile and weigh 139–309 MB, so
each apply cost 11.7 s — once in `_ready()` and again on every settings
Save. The runtime now reads only the first 64 KB of a manifest and extracts
the scalar fields textually (full parse only for small manifests whose head
lacks the key), and an unchanged path set is a no-op: 0 ms. Follow-up for
the bake pipeline: stop writing the per-tile list into terrain manifests
(the runtime never reads it — tiles are probed by file name).

### 9.6 Teleport onto unloaded scenery (fix 2026-09-18)

Teleporting from an in-flight state to a region whose tiles were not
streamed yet (Geneva → Emmen) dropped the aircraft ~300 m and the resulting
gear-spring blow-up produced NaN poses and a renderer crash. Three causes,
all in the teleport path (the first-flight path already waited for the
precise tile):

1. `process_rebase` released JSBSim after a fixed 2 s / 12 s at the
   **coarse globe** elevation (the scenery query fell back to the globe, so
   it never returned NAN); the precise tile then arrived tens or hundreds
   of metres away (Schaffhausen: globe 574 m vs tile 466 m). Now it waits
   for the *precise* elevation while the tile streamer has pending work
   (fallback to the globe only after 12 s of streamer idleness, 90 s cap).
   The first-flight deferred init uses the same busy-aware wait.
2. A Fly! during a running intro zoom left the intro camera parked in
   space over the old airport while the loading screen blocked the zoom;
   the streamer follows the viewport camera, so the destination never
   loaded. The zoom is cancelled before a teleport
   (`IntroZoom.cancel()`), and `_on_teleport_requested` makes sure the
   current camera rides with the pinned aircraft. Since 2026-09-21 there
   is no fly-loading screen: the zoom starts with the Fly! request over the
   destination and is the loading view (status + progress on the intro
   HUD); `IntroZoom.set_hold_at_end(true)` keeps it on the spot view until
   `LoadingProgress` completes (Space then only jumps to the end). The
   loading screen remains the fallback when the zoom cannot start.
3. Safety nets: `TerrainElevation::update_terrain_elevation` lifts the
   aircraft onto the ground when a tile loads *above* it (terrain jump
   > 1 m and CG below ground; flying into a mountain is left to crash
   detection), and `copy_outputs_from_JSBSim` never hands a non-finite
   pose to the scene — it restarts the FDM at the last good pose instead.

4. Retractable gear: JSBSim computes **no ground contact while the gear is
   in transit** (`FGLGear`: contact only for gear-pos-norm > 0.99) and the
   A-10's gear kinematic takes 5 s, so a Fly! after a gear-up flight only
   *commanded* gear down: the aircraft fell through the runway for 5 s and
   bounced (Dübendorf). `reset_controls()` now snaps the actuated positions
   (`gear/gear-pos-norm` = 1, flap / speedbrake positions = 0) — the
   kinematic re-reads its output property each step, so the snap sticks.
   Test: `tests/smoke_teleport_gear.tscn` (A-10, gear in transit → Fly!).

The teleport now spawns 0.5 m above the ground like the first flight
(was 2.1 m, a visible plop). Test: `tests/smoke_teleport_far.tscn`
(LSZH → Geneva mid-intro: release on the precise tile in ~2 s, ≤ 0.3 m
bounce).

### 9.7 Terrain unlit by the sun / black patch from space (fix 2026-09-18)

From orbit the detailed patch rendered black until ~300 km, and the haze
between 50 and 30 km was far too dense.

- **Winding.** Godot's front faces are *clockwise*; `S3DTile` wound its
  triangles counter-clockwise. With the double-sided terrain material the
  renderer flipped the normals (normal buffer: terrain normals pointed away
  from the camera, the globe's toward it), so the DirectionalLight never lit
  the terrain — only the normal-independent ambient floor did, and the
  pre-lit orthophotos hid it. From space the ambient is dimmed to night
  level, hence black. Fixed by winding the tiles clockwise (scenery3d
  submodule); the terrain now has real relief shading. Diagnosed with
  `tests/probe_space_view.tscn` (`PROBE_DEBUG_DRAW=lighting|normals`) and
  `tests/probe_tile_normals.tscn`.
- **Haze.** The fog density used a linear 4..60 km altitude ramp, i.e. a
  40 km camera still had a third of the ground density along a 40 km path.
  Haze lives in the boundary layer, so `src/sky_atmosphere.cpp` now scales the
  density by `HAZE_LAYER_M / altitude` (2.5 km layer): the optical depth to
  the ground stays what it is at ground level, and fog is off above 120 km.

Known, pre-existing: a white staircase along the Swiss/Baden-Württemberg
data border seen from > 40 km (skirts/seam at the boundary of the two
tile sets).

## 10. Weather model — settings only (2026-09-18)

The Settings screen has a **Weather** tab: a list of layers, each spanning
*Base*..*Top*, with wind direction/speed, turbulence and cloud type +
coverage in oktas, plus global visibility (km) and QNH (hPa). Wind and
clouds share the slab; the air between layers is clear, with wind
interpolated across the gap. Keys live in the `[weather]` section of
`provpilot.cfg`: `weather_layers` (Array of Dictionaries: `base_m`, `top_m`, `wind_dir_deg`, `wind_speed_kts`, `turbulence`,
`cloud_type`, `coverage_oktas`), `visibility_km`, `qnh_hpa`, `live_weather`
(placeholder checkbox, disabled). The old Realism `turbulence` / `wind_*`
keys are gone.

**Implemented (static weather):** `JSBGodot::set_weather(layers, qnh)` keeps
the sorted layer stack; every physics tick `apply_weather_to_fdm()` samples
it at the aircraft's altitude (inside a layer: that layer; in a gap: linear
vector blend, nearer layer's turbulence; below/above the stack: the lowest /
highest layer) and pushes wind NED + MIL-F-8785C turbulence into `FGWinds`
and QNH into the atmosphere's sea-level pressure. `_push_weather_settings()`
in `node_3d.gd` feeds it at start-up and on Settings close, and turns
visibility into the fog density via Koschmieder (3.912 / V), which the
altitude ramp still fades toward space. Clouds are stored but not rendered.
Test: `tests/smoke_weather.tscn`.

**Implemented (live weather, METAR):** `src/live_weather.cpp` (C++ `LiveWeather`)
(`LiveWeather` node) fetches the nearest METAR from the Aviation Weather
Center data API (`aviationweather.gov/api/data/metar?format=json&bbox=…`,
decoded JSON, no key) in a ±1° × ±1.5° box around the aircraft, picks the
nearest station and translates the report into the layer model: surface
layer 0..1500 m AGL (capped 100 m under the lowest cloud base) with the
reported wind, gust factor → turbulence (≥5/10/20 kt = light/moderate/
severe); one layer per cloud group (base = station elevation + AGL base,
thickness by type; type from CB/TCU in the raw text, else by base height
and cover: stratus/stratocumulus/cumulus below 2000 m AGL, alto* to
6000 m, cirrus above); visibility (statute miles → km, "6+"/"9999" →
40 km) and QNH pass through. Refreshed every 10 min, on Settings close and
at each flight start; failures keep the current weather and show a
message in the Weather tab. The store's static layers are never modified;
while the box is ticked the tab shows the live layers read-only with a
status line. Sources are pluggable via `LiveWeather.source` (only `awc`
so far). Tests: `tests/smoke_live_weather.gd` (canned JSON, plus a real
fetch with `PROVPILOT_NET=1`) and `tests/smoke_live_weather_scene.tscn`
(network, skipped without `PROVPILOT_NET=1`).

Next steps, in order: cloud rendering (the METAR layers already carry
type/coverage/base), then Open-Meteo as a second source for winds aloft
and regional variation.

### 10.2 Cloud rendering (2026-09-19)

`src/cloud_layers.cpp` (C++ `CloudLayers`, child of the main scene) builds one
mesh per weather layer with clouds, from the same layer Dictionaries the
flight model uses (static or METAR):

- **Thick types** (cumulus, stratus, stratocumulus, cumulonimbus):
  `shaders/cloud_layer.gdshader`, a raymarched *spherical shell* between
  `base_m` and `top_m` above the ellipsoid, drawn inside a 120 km
  camera-centred box (`cull_front`, so it also works from inside). The
  node's frame is West/Up/North at the camera's ground point, re-anchored
  every frame, so the planet centre is (0, −R, 0) in shader space and the
  camera position / mesh offset are passed as uniforms in double precision
  (the shader's camera-relative built-ins cannot be mixed with them in
  the double build). Ray/shell intersection handles below / inside /
  above the layer; the march is clipped by the scene depth (terrain in
  front) and `max_distance`, steps adapt to the layer thickness with a
  cheap shape-only probe through empty air. Density = Perlin FBM shape
  (`NoiseTexture3D`, normalised to full range) thresholded by coverage
  (oktas/8, threshold rising with height for rounded tops) × per-type
  height profile, eroded by inverted worley detail. Lighting: 3-sample
  light march toward the sun (Beer-Lambert with a floor), Henyey-Greenstein
  forward scattering, powder term, ambient graded from base to top, then
  the scene fog by mean depth and a fade toward the box edge. Noise /
  erosion / lighting follow Clay John's MIT volumetric-cloud demo.
- **Thin types** (altostratus, altocumulus, cirrus):
  `shaders/cloud_sheet.gdshader`, a 180 km curved sheet at `base_m`
  (vertex y follows the planet curvature) with 2D-noise alpha per type.
- The noise is anchored to LV95 easting/northing of the frame origin
  (`world_offset`); without it the field moved with the camera and the
  clouds "followed the aircraft". The cloud materials use
  `render_priority` 100 so water and other transparent surfaces, which
  sort by distance, cannot draw over the volume.
- Wind per layer drifts the noise; `Graphics > Cloud Quality` selects
  Off / 20 / 32 / 56 steps. Sun direction, colour, energy and the ambient
  come from `src/sky_atmosphere.cpp` (`sun_dir_now` etc.).

Measured at 1280×800: no measurable cost at 600 m below a 5/8 cumulus
deck (25 fps with and without clouds — the near-ground scenery dominates),
100 fps above it. Probe: `tests/probe_space_view.tscn` with
`PROBE_WEATHER_JSON`, `PROBE_LOOK=pitch:-8`, `PROBE_CLOUD_DEBUG=1|2|3`.
Not done: cloud shadows on the ground, precipitation, half-resolution
rendering for 4K if it turns out to be needed.

**Weather profiles (2026-09-19):** the Weather tab has a right-hand column
listing `<install_root>/weather/*.json` (name, layers, visibility_km,
qnh_hpa). Load swaps the file's values into the store and refreshes the
tab (Save in the settings then applies them), Save writes the current
layers under the typed name, Delete removes the file.
`SettingsStore.save/load/delete_weather_profile`, `list_weather_profiles`.

*Draw order between layers (2026-09-19).* Seen from high altitude a low
layer could draw over a high one: Godot orders transparent meshes by the
depth of their AABB centre, and every layer's centre sits directly under
the camera, so with a level camera the depths tie. `Clouds._apply_draw_order`
now assigns `render_priority` per frame — the layer farthest from the
camera altitude draws first, the nearest last (layers on opposite sides of
the camera never overlap on screen).

### 10.1 How it maps onto JSBSim

JSBSim has no layered atmosphere: the host sets *one* wind vector and one
turbulence setting per tick, so the bridge interpolates the layer stack at
the aircraft's current altitude every frame.

- **Wind** — `FGWinds`, properties `atmosphere/wind-north-fps`,
  `wind-east-fps`, `wind-down-fps` (or `psiw-rad` + `wind-mag-fps`: psiw is
  the direction the wind blows *from*, like METAR). Meteorological
  direction *d*, speed *v*: north = −v·cos d, east = −v·sin d (kts → fps
  ×1.68781). Wind is applied in the NED frame at the aircraft — no
  altitude dependence inside JSBSim, hence the host-side interpolation.
- **Turbulence** — `atmosphere/turb-type` (0 none, 1 standard, 2 culp,
  3 milspec, 4 tustin). The MIL-F-8785C model (3) is driven by
  `atmosphere/turbulence/milspec/windspeed_at_20ft_AGL-fps` and
  `.../severity` (0..7). Mapping: None → type 0; Light/Moderate/Severe →
  type 3 with severity 2/4/6 and the 20 ft wind = the ground layer's wind
  speed, but at least the spec's 15/30/45 kt reference so still air still
  shakes (the low-altitude sigma is 0.1 × that wind). Gusts
  (`atmosphere/gust-*`, `cosine-gust/*`) can be layered on later.
- **Pressure** — QNH is `FGStandardAtmosphere::SetPressureSL(eHPa|eMillibar, qnh)`
  (property `atmosphere/P-sl-psf`, 1 hPa = 2.08854 psf). It rescales the
  whole pressure column so density, indicated airspeed and the pressure
  altimeter all react; `atmosphere/delta-T` shifts the temperature profile
  if we ever add a temperature field. Both are cheap to set every tick.
- **Clouds / visibility** — purely host-side: fog distance + sky/cloud
  rendering. Not a JSBSim input.

Frame note: JSBSim's wind NED frame is local to the aircraft's geodetic
position, so the ECEF world frame needs no conversion here.

## 11. Worldwide base tier — decision 2026-09-19

gery, 2026-09-19 (weather work parked): *"the Swiss and BW scenery should be
treated as add-ons due to the huge size … I want a similar basis as
Scandinavia for the rest of the globe, starting with a strip from Scandinavia
over Germany, Switzerland and Italy to the Mediterranean. We would continue
with Denmark and the water in the North Sea."*

### 11.1 Decisions

| # | Decision |
|---|----------|
| D10 | **Two tiers.** The *base tier* is what Fennoscandia already is: SRTM 30 m terrain (34 px / 1024 m tiles), OurAirports catalog, no ortho. It is meant to cover the whole globe eventually and to be the default install. The 1 m CH and BW packs (46 + 16 GB, ortho, buildings, roads) become **opt-in add-on overlays** on top of the base tier. |
| D11 | **Region packs are countries** (Natural Earth 1:10m admin-0 polygon + 5 km margin), not bounding boxes. Neighbouring packs overlap by the margin and bake identical values there (same source), so seams are impossible and a user with only one pack sees the ETOPO globe past the margin (fragment mask, D8). The Fennoscandia packs keep their bbox selection (`select: "bbox"`) — already baked and flight-tested. |
| D12 | **One grid for all of Europe: LV95 (EPSG:2056).** Scenery3D streams a single grid per build (C3) and the runtime places every tile through the rigorous projection with the local Jacobian, so LV95 scale distortion does not misplace anything; it only stretches the 31 m posting. Scale factor k−1 ≈ d²/2R²: Denmark 1.2 %, Sicily 1.8 %, Nordkapp 10 % (flown). Multi-CRS streaming (D6) starts with the **first pack outside Europe**, not before. |
| D13 | **Priority order and cross-resolution merge.** Base packs go **after** the detail packs in the scenery library (higher priority number). `S3DTileManager` merges same-resolution sources pixel-wise (later non-zero wins). Sources of *different* resolution are merged by `elev_fill_zeros_resampled` (2026-09-19): the finer tile is authoritative wherever it has data and its no-data (0.0) samples are filled from the coarser tile, bilinearly resampled from non-zero neighbours — both in the tile path and in the far-chunk composite. Before this the first source found won wholesale, which left the diagonal no-data wedge along the BW pack's eastern edge (UTM32 source cells projected into LV95) at 0 m next to the 30 m base: the dark cliffs of the first strip check flight (Bavaria, 2026-09-19). Smoke: `tests/smoke_tile_merge.tscn`. |
| D14 | **Terrain manifests carry scalars only** (`pixels_per_tile`, `crs`, `grid`, `tile_count`, …). The per-tile list of the Norway bake (139–309 MB per pack, never read by the runtime) is dropped. |

### 11.2 Pipeline

`scripts/world_regions.json` is the single region registry (id, name,
subdir, `select`, ISO codes, optional bbox, verify landmarks).
`scripts/build_world_terrain.py {plan,fetch,bake,verify,build} --region <id>`
bakes one region into `/Volumes/Data1/provpilot/build/<id>/<Name>/terrain/`;
`scripts/build_world_strip.sh <id>…` chains regions with per-region logs.
The SRTM cache is shared (`/Volumes/Data1/scenery_in_world/srtm`, the old
Norway cache is read as a fallback); ocean-only 1° tiles that the server does
not have (HTTP 404) are recorded as empty marker files and bake to 0.0
(no-data → globe). The bake is resumable (existing tiles of the right size
are skipped; `--force` rebakes). `scripts/build_world_airports.py --all`
fills the catalog for every region with ISO codes; an ICAO already present
under another region (the hand-curated CH/BW entries) is not duplicated.

Strip plan (2026-09-19, `plan` output; Norway bake rate was 186 tiles/s,
the Denmark bake ran at ~2000 tiles/s with a warm SRTM cache):

| Region | Selection | Tiles | SRTM 1° | Tile bytes |
|--------|-----------|------:|--------:|-----------:|
| denmark | DNK | 59 770 | 24 | 264 MB |
| germany | DEU | 368 911 | 71 | 1 627 MB |
| switzerland-base | CHE + LIE | 47 336 | 15 | 209 MB |
| austria | AUT | 91 036 | 25 | 401 MB |
| italy | ITA + SMR + VAT | 329 963 | 82 | 1 455 MB |
| france (2026-09-20) | FRA + MCO, clipped to 5.6°W–10°E / 41–51.3°N | 555 977 | 100 | 2 452 MB |

France was added after the strip check flights (gery, 2026-09-20: the
Swiss border against the bare globe looked odd). Its Natural Earth admin-0
feature includes the overseas departments, hence the `bbox_wgs84` clip;
OurAirports lists those under their own ISO codes, so the catalog needs no
clip but the builder applies the region bbox anyway.

Italy's southern islands (Lampedusa 35.5°N) give **negative LV95 northings**;
the tile manager already floors negative tile and chunk indices, so this is
supported (Crete and North Africa will need it too).

Verified 2026-09-19: every pack's landmarks inside tolerance (Denmark EKCH
2 m / Møllehøj 161 m, Germany Zugspitze 2940 m, Switzerland Dufourspitze
4499 m, Austria Großglockner 3629 m, Italy Etna 3270 m). First check flight
(gery, ENGM → Denmark → Germany, slew): BW loads over the base without
issues; the dark wedges at the BW/Bavaria edge were the cross-resolution
merge gap fixed in D13. The slew HUD also never updated the "PM:" WGS84
line (only the C++ SlewMode wrote the HUD; it now gets the label via
`set_pm_label`). Remaining: re-fly the BW east edge and a Basel seam pass
(CH add-on over Germany base over Switzerland base).

### 11.3 Open: pack naming and the CH/BW migration

The base packs are named by country (`Switzerland`, `Germany`, …) like
Norway. The installed detail packs live in `Switzerland/` and
`Germany/Baden-Wuertemberg/`, so a base `Germany/` next to the BW add-on
would nest two regions in one directory. Proposal: publish the add-ons as
`Switzerland-HD` / `Baden-Wuerttemberg-HD` (schema-v2 region entries with
`"overlay_of": "switzerland-base"`), keep the old directory names working
for existing installs, and let the base packs own the plain country names.
Decision pending (gery).

### 11.5 Morphing over the Alps — LOD for 30 m tiles (2026-09-20)

Check flight (gery, CH/BW add-ons disabled): the base scenery "morphs" a lot
over the Alps. Measured on the 8 km block around the Zugspitze (1061–2941 m)
by rebuilding the surface at each mesh spacing and comparing with the 31 m
data:

| Mesh spacing | RMS error | Max |
|--------------|----------:|----:|
| 62 m (stride 2) | 9.6 m | 318 m |
| 124 m (stride 4) | 20.4 m | 437 m |
| 248 m (stride 8 = old far chunk) | 40.0 m | 520 m |
| 496 m (stride 16) | 78.9 m | 771 m |
| 1 km (2 vertices, old ring 13–16) | 150.2 m | 658 m |

Cause: the ring table (`rebuild_lod_rings`) names LOD levels that were
applied as raw strides to every tile. For 1 m/px CH tiles LOD 3 means 8 m
cells at 8 km; for a 34 px base tile the same LOD 3 meant 256 m cells, and
ring 13–16 (LOD 6, stride 64 clamped to 33) collapsed each tile to one quad,
while the far chunks beyond 16 km were *finer* (250 m). Headless probe
(`tests/smoke_lod_base.tscn`, before): ring 2 → 289 verts, 3–4 → 81,
5–8 → 25, 9–12 → 9, 13–16 → 4, chunks 1089. Every ring crossing re-meshed
the tile with a different surface, and the chunk→tile handoff at 16 km
jumped the surface by the 30 m chunk bias.

Fixes (scenery3d):
- **Resolution-aware LOD** — `S3DTile::effective_lod(lod, img_w, tile_m)`:
  a LOD level names a target spacing of 2^lod m; each tile subtracts
  log2 of its own pixel spacing. 34 px tiles stay native (31 m) through
  ring 12 and use stride 2 (62 m) in ring 13–16; 1 m tiles are unchanged;
  2 m BW tiles now keep native resolution in ring 2 (target 2 m).
  Ring crossings that map to the same stride no longer regenerate the mesh,
  and small heightmaps (≤ 64 KB) are no longer discarded at LOD ≥ 4.
- **Far chunks by distance** — composite 129 px (64 m) for chunks whose
  nearest tile is within 32 tiles, 65 px (128 m) within 64, 33 px beyond,
  with 4 tiles of hysteresis; chunks are rebuilt when their target changes,
  so the tile/chunk handoff at ring 16 is 62 m ↔ 64 m instead of 1 km ↔ 250 m.
- **Chunk Y bias scales with spacing** (30 m at 250 m → 7.7 m at 64 m), so
  the handoff no longer lifts the terrain by 30 m.

After (same probe): rings 0–12 → 1156 verts (native), 13–16 → 289, nearest
chunk 16641 verts, farthest 1089. `smoke_scenery_ecef` and `smoke_tile_merge`
still pass. Remaining morphing is the real data change at ring 12 (31 → 62 m,
RMS ~10 m over the Alps) and at 32 / 64 tiles between chunk resolutions.
A geomorph (vertex blend over ~1 s on LOD change) would hide those too and
is the next step if they still show in flight.

### 11.4 Next: the sea (North Sea first)

The base packs stop 5 km off the coast; beyond that the globe shows the ETOPO
sea floor colour ramp, and inside the packs the SRTM bathymetry is rendered
as dry terrain below 0 m. Neither is water. Options considered:

1. **Bake sea polygons per pack** (OSM water polygons from osmdata, on the
   1 km grid through `convert_osm_bw.py`, baked Y = 0). Exact coastline,
   handles land below sea level (Dutch polders), but the open sea is
   hundreds of thousands of trivial full-tile quads per pack.
2. **Engine sea surface at 0 m on the ellipsoid** (world3d draws a water
   material at h = 0 wherever ETOPO < 0, ignoring the coverage mask; inside
   packs the surface simply sits above the baked sea floor). Worldwide for
   free, no data; wrong only where land lies below sea level.
3. **Hybrid (recommended):** option 2 as the surface, plus a per-pack
   1 km **sea mask** (sea / land / coast, one byte per tile, from the OSM
   water polygons) that the engine samples: land tiles suppress the surface,
   coast tiles get an exact polygon GLB from option 1, open-sea tiles use the
   engine quad. Compact, exact at the shore, and the same mechanism serves
   lakes at their own level later (§5.4 per-polygon water level).

To be designed after the strip is flown; the Denmark pack is the test bed.

## 12. Runways for the base tier — plan 2026-09-20

gery: *"the next base scenery item to do are runways … what source is best
for the generic runways and how can we keep the runways generic but as
precise as possible? … Keep all the scenery elements like runways so that
it can be replaced easily with bespoke more detailed scenery later."*

### 12.1 Findings

- The base tier draws **no runways**; the procedural loader named in
  SCENERY_ARCHITECTURE.md no longer exists. Runways are visible only where
  the Swiss ortho shows them.
- The catalog stores heading + length per runway and derives both
  thresholds from the airport reference point, so every runway is centred
  on the ARP (wrong for offset runways). The curated Zurich entry lists only
  16/34.
- Sources compared on the nine regions (442 large/medium airports, 623
  runways): **OurAirports** runways.csv (public domain) has runway-end
  coordinates for 96 %, width 99 %, end elevations 91 %, displaced
  thresholds, lighting — but 36 of 595 runways contradict their own declared
  length by > 10 % (up to 1.5 km). **OpenStreetMap** `aeroway=runway`
  centrelines (ODbL, already credited for roads/water) are imagery-traced,
  typically within a few metres, carry `width`/`ref`/`surface`, but ways are
  split or duplicated and small fields are patchy. **X-Plane Gateway**
  apt.dat is the most precise but its reuse licence is unclear → not used.
- OurAirports `le/he_latitude_deg` are the **runway ends**, not the
  displaced thresholds; at Zurich they agree with the OSM pavement ends
  within 10–20 m.

### 12.2 Decisions

| # | Decision |
|---|----------|
| D15 | **Geometry from OSM, identity and metadata from OurAirports.** Ways with the same designators (or none) on the runway axis (bearing ±6°, lateral ≤ 80 m, compatible surface class, not already claimed by an exact-ref match) are merged; the pavement ends are the extreme node projections along the axis, the width is the OSM `width` tag. OurAirports supplies designators, end elevations, displaced thresholds, surface, lighting, and is the geometric fallback when OSM has nothing. Disagreements (> 30 m at an end, > 10 % in length) are **reported, not resolved silently**. |
| D16 | **Runways are a per-pack asset**: `<Pack>/airports/runways.json` (schema v1 below), resolved by scenery-library priority like every other asset class. The runtime merges the enabled packs **per airport**: the highest-priority pack that lists an ICAO defines that airport completely (all its runways); lower packs are ignored for it. A bespoke or detail pack therefore overrides an airport by listing it, and removes a base runway by listing the airport without it. |
| D17 | **Replaceable rendering.** The procedural runway mesh is one renderer of the same file. An airport entry may carry `"model": "LSZH/apron.glb"` (path inside the pack) with `"procedural": false`: the model is placed at the airport reference point in its WUN frame and the procedural runways are suppressed. Terrain flattening (D18) still applies, driven by the same runway records, so a bespoke model sits on the same plateau. |
| D18 | **Precision comes from the terrain, not the coordinates.** Each runway is a plane between its two end elevations (OurAirports, else terrain samples at the ends); the bake flattens the terrain tiles under the strip plus a blend margin onto that plane. On 30 m SRTM the Zurich ends differ by 8 m with bumps between; without this even perfect coordinates give a runway that cuts the ground. |

### 12.3 Schema v1 (`airports/runways.json`)

```
{ "schema_version": 1, "region": "germany", "built": "2026-09-20", "sources": [...],
  "airports": [
    { "icao": "EDDF", "name": "…", "lat": …, "lon": …, "elevation_m": 111, "type": "international_airport",
      "model": "EDDF/terminals.glb",     # optional (D17)
      "procedural": true,                 # optional, default true
      "runways": [
        { "designators": ["07L", "25R"],
          "ends": [ { "ident": "07L", "pavement": [lat, lon], "end_ourairports": [lat, lon],
                      "elev_m": 108.5, "displaced_m": 0, "heading_deg": 69.0 },
                    { "ident": "25R", … } ],
          "width_m": 45.0, "length_m": 4000, "surface": "asphalt", "lighted": true,
          "geometry_source": "osm" | "ourairports" | "arp+heading",
          "flags": ["length_osm_4000_vs_declared_3800"] } ] } ] }
```

`ends[0]` is the end you land over on `designators[0]`; `pavement` is the
end of the strip, `displaced_m` is measured inward from it; `elev_m` absent
→ the runtime samples the terrain. Coordinates WGS84.

### 12.5 Taxiways and aprons (2026-09-20)

Same source, same file, same renderer. `build_world_runways.py` also
fetches `aeroway=taxiway` ways and `aeroway=apron` areas (closed ways and
multipolygon relations) per region bbox (cache `scenery_in_world/
osm_airfield/`) and assigns each to the nearest airport reference point
within its reach (4 km international, 3 km regional, 2 km grass strip).
The pack file gains `taxiways[] {path[[lat, lon, elev_m]], width_m
(OSM width, else 23 m), surface}` and `aprons[] {polygon[[lat, lon,
elev_m]], surface}`.

**Terrain (flatten stage).** Every surface class is rasterised jointly
with priorities: runway pavement 4 > apron 3 > runway margin 2 > taxiway
1; within a class the *lowest* surface wins where features overlap
(crossing runways, adjoining aprons, overlapping ribbons), so no mesh of
that class ends below the terrain — the higher one floats by the overlap
difference instead. Aprons are merged per airport (pieces closer than two
dilations become one polygon) and fitted as planes (least squares on the
phase-A terrain, slope ≤ 3 %, lowered wherever they would rise above a
runway strip, checked on interior pixels and outline vertices). Taxiway
node heights are the terrain after runways and aprons, smoothed ±60 m
along the path, then refined twice against the final terrain so ribbons
sit on it to 5 mm. Aprons and ribbons are flattened one pixel spacing
(31 m) past their outline so the bilinear terrain under a mesh edge only
sees flat pixels. Residual: at Zurich one apron edge on the steep west
side is buried 0.8 m; Geneva, Bern and St. Gallen are within 0.2 m.

**Runtime.** `src/runways.cpp` (C++ `Runways`) builds ONE `Surfaces` mesh per airport holding
its runway strips, taxiway ribbons (mitred corners, stored profile) and
apron polygons (`Geometry2D.triangulate_polygon` in the airport frame,
vertices at the stored plane heights); every vertex is projected exactly,
so the airport frame is only an origin. Lifts: runway 5 cm, taxiway 3 cm,
apron 2 cm, so at junctions the runway draws on top. Shader kinds 4
(taxiway: asphalt, yellow centreline) and 5 (apron: concrete slabs).
*Check flight LSZH (2026-09-20): ribbons and aprons submerged in patches,
ribbons drawn across runways.* Ribbons had vertices only at the OSM
nodes, so between nodes a straight segment cut through terrain that had
been flattened to a runway or apron plane it crossed. The bake now
densifies every taxiway to ≤ 20 m nodes and inserts exact nodes where a
path enters or leaves a runway pavement (duplicated, so the flag switches
crisply); path records are `[lat, lon, elev_m, on_runway]`. In the shader
a taxiway fragment with the on-runway flag draws only the yellow guidance
line — the pavement stops at the runway edge, the line continues across —
and the taxiway lift (6 cm) is above the runway (5 cm) so the line shows;
aprons stay lowest (2 cm).
*Second LSZH check (2026-09-20): a ramp from the apron up to the runway 28
end, and messy crossings.* The ramp was a datum conflict: runways use the
surveyed end elevations (28: 431.6 m, 428.5 after the crossing shift)
while taxiways and aprons followed SRTM, 5–7 m lower there. The bake now
anchors taxiway and apron heights to the runway planes (`runway_datum`:
inverse-distance blend of the airport's runway planes at their nearest
points, full weight within 300 m of a runway, fading to SRTM by 1 200 m);
taxiway nodes within 250 m of the 28 end are now within 1.2 m of the
runway plane. The crossings were geometry: mitred ribbon corners folded
at sharp turns (dark back faces) and overlapping ribbons at identical
heights z-fought. Ribbons are now one quad per segment plus a flat disc
at each interior node (1 cm lower, fills the wedge), and each taxiway
gets a deterministic lift offset (0–2 cm) so junctions have a fixed order.
Better DEMs around airports (gery's suggestion) remain the way to fix the
surroundings — the datum only levels the airfield itself; a per-airport
DEM patch from national data would slot into the add-on packs (§12.2).
*Third LSZH check (2026-09-20): small gaps where one OSM way ends on
another, and the yellow line vanished under the other ribbon at taxiway
crossings.* Discs now sit at every node (ends included), and the guidance
line is its own 0.5 m strip (kind 6) at 9 cm, above every pavement, so it
reads through taxiway crossings exactly as it does across runways;
taxiway pavement (kind 4) carries no line and is discarded on runways.
The probe (`tests/probe_runway_shader.gd`) takes PROBE_LAT/LON/ALT and
PROBE_PNG and saves a top-down render — the check of record for these.
*Fourth LSZH check (2026-09-20): still gaps at multi-way junctions;
gery: "what if these were roads — grey polygons".* Taxiway pavement is now
built like a road network: the bake unions every ribbon of an airport
(`LineString.buffer` with round joins, minus the runway pavement
rectangles for the cut at the runway edge), densifies the outline to
20 m, constrained-Delaunay-triangulates it (shapely 2.1), subdivides
triangles longer than 24 m, samples vertex heights from the final terrain
and then sinks the terrain to the mesh under every triangle (the
lowest-surface rule leaves creases where two ribbon profiles meet).
Stored per airport as `taxiway_mesh {vertices[[lat, lon, elev]],
triangles[]}`; Zurich 21 629 vertices / 26 503 triangles, terrain never
more than 7 cm above the pavement (lift 8 cm, lines 11 cm). Ribbons and
discs are gone from the renderer; the paths only feed the line strips.
*Distance-driven build (2026-09-20).* With unioned pavements the nine
packs hold ~2.1 M triangles (260 MB of runways.json) and building all
airports took ~6 s at startup and on every settings change. `Runways`
now takes the merged set (`set_merged`) and `update(cam_pos, delta)`
builds airports entering 150 km of the camera (4 per half-second) and
frees them beyond 250 km; the merge itself (JSON parse, ~1 s) stays at
startup. `build()` remains for tests and probes.
*Fifth LSZH check (2026-09-20): ground through the 10/28 × 16/34 crossing;
side lines of the crossing runway across the other.* Measured: terrain
stood ≤ 11 cm above the runway planes, only at pavement edges and at the
crossing — the shorter runway's margin took the longer one's (higher)
pavement value and the bilinear terrain at the edge rose through the 5 cm
lift. Fixes: runway flat zones are dilated one pixel like aprons and
taxiways; a final *runway ceiling* pass caps the terrain at every runway's
plane inside its flat zone; the taxiway-pavement sinking grows each
triangle by one pixel (plane extrapolated, capped at 0.5 m). Residuals:
runways ≤ 4.7 cm, pavements ≤ 4.7 cm at LSZH/LSGG/LSZB/LSZR; lifts are now
runway 8 cm, taxiway 10 cm, line 13 cm, apron 3 cm. Marking cut at
runway crossings: `reconcile_crossings` stores, on the shorter runway,
the along-track interval inside the longer one's pavement
(`crossings: [[t0, t1]]`); `_add_runway` inserts duplicated vertex rows at
those bounds with flag 1 in CUSTOM1.w and the shader draws no markings
there, so the through runway's centreline and edge lines continue while
the crossing runway's stop at its edge (Zurich 10/28: 745–851 m).
*ENGM check (gery, 2026-09-20): surfaces 6 m above the ground, aircraft
rolling under the runway.* Oslo lies inside both the Norway and the Sweden
pack (bbox bakes), and same-resolution tiles merge later-wins at runtime,
so Sweden's raw tiles showed under Norway's flattened surfaces. Two fixes:
(1) `flatten` now also applies the plateaus of every *other* pack's
airports whose runways touch this region's tiles (`_foreign_airports`,
never written back), so any pack that wins a tile carries the same
plateau — this also covers border airports inside the 5 km country
margins (Basel is in three packs); (2) Norway, Sweden and Finland are
rebaked as country-clipped packs like the rest (`build_world_terrain.py prune`
removes the old bbox tiles), so the only overlaps left are the margins.
*Ground plane = drawn surface (2026-09-20).* Wheels sat 8–10 cm into
runways and taxiways: JSBSim's ground was the terrain while the surfaces
are drawn a few centimetres above it. `Runways.surface_lift_at(ecef)`
answers the lift at a position from a per-airport 2D lookup built with
the mesh (runway rectangles, taxiway triangles in a 50 m grid, apron
polygons; airports within 8 km), and `node_3d._update_terrain_elevation`
adds it to the height pushed to JSBSim. Smoke: 16/34 → 8 cm, a taxiway
node → 10 cm, open field → 0. The A-10's remaining float is its own
`aircraft.cfg` model offset (2.21 m, marked as a guess there); the JSBSim
geometry gives CG − gear = 1.90 m for the A-10, 0.93 m for the PC-7.
*Slopes (ENGM grass, 2026-09-20): the aircraft stayed level on slopes and
was lifted or dropped from time to time.* JSBSim only received one terrain
height per frame at the CG and its default ground callback returns a flat
plane, so the gear never saw a slope and the plane stepped whenever that
height changed. `JSBGodot::set_ground_query(Callable)` installs a
`GodotGroundCallback` (FGGroundCallback) on the FDM's inertial model:
for every gear contact point JSBSim asks the host
(`node_3d._ground_height_geodetic`: Scenery3D elevation + drawn-surface
lift) for the terrain under that point and gets the slope normal from
finite differences 4 m north and east (NED → ECEF via the location's
Tl2ec); where the query has no data it falls back to the flat elevation
the host still pushes each frame. Contact locations must have the
planet ellipse set before geodetic values are read (as the default
callback does) — without it the gear model blew up. Re-installed after
every model load. `smoke_ground_steering` (taxi + yaw) passes.
*A-10 bouncing itself up on downhill grass (2026-09-20).* The bridge ran
`FDMExec->Run()` once per Godot physics tick (60 Hz) with JSBSim's default
1/120 s step: the FDM advanced at half real time and integrated the A-10's
stiff, heavily damped gear (50 000 lb/ft, 25 000 lb·s/ft per main) too
coarsely, so every ground crease pumped energy in. `_physics_process` now
sub-steps: `steps = round(delta / (1/240))`, `Setdt(delta / steps)`, and
runs JSBSim `steps` times per tick — real time, gear at 240 Hz, the
scenery ground callback queried per sub-step. `smoke_ground_steering`
and the main-scene smoke pass; `tests/probe_sim_rate.tscn` reports the
sim-time / wall-time ratio.
*One material for everything:* the first build used a ShaderMaterial per
runway plus one per taxiway/apron set (~10 000 materials) and the app died
at startup with SIGBUS in the Metal driver's `uniform_set_create`
(2026-09-20). The per-surface parameters (half width, length, displaced
thresholds, packed designator glyphs, kind·64 + stripes·2 + aiming) now
ride in the CUSTOM0/CUSTOM1 vertex attributes and a single shared
material renders all 3 376 airports (3 716 runways, 28 248 taxiways,
3 713 aprons; merge 0.2 s + build 0.5 s). `tests/probe_runway_shader.gd`
builds every region with the real renderer and is the guard for this
class of failure — headless runs never touch the GPU driver.

### 12.4 Steps

1. **Data** — `scripts/build_world_runways.py --all` (done 2026-09-20): one
   Overpass query per region bbox (cached under
   `scenery_in_world/osm_runways/`), OurAirports merge, `runways.json` per
   pack and `runways_report.txt` per region. Zurich: 16/34, 14/32, 10/28 and
   the grass strip, all from OSM, no flags.
2. **Bake** — terrain plateau under each runway (D18): done 2026-09-20,
   `build_world_terrain.py flatten` (`scripts/runway_plateau.py`, also part
   of `build`). The touched tiles are re-baked from SRTM first, so the step
   is idempotent (verified: a second run is byte-identical); then each
   strip (width + 12 m apron, 40 m overrun) is set to the runway plane and
   an 80 m smoothstep blend joins it to the terrain. The plane uses the
   OurAirports end elevations when both exist and each is within 15 m of
   the terrain there, else a least-squares line through the centreline
   terrain. `plateau_report.txt` per region lists plane, source, max cut and
   fill. Zurich 16/34: plane 423.7 → 423.1 m, SRTM along the centreline
   409.8–427.2 m (max fill 13.5 m); cross profile flat to ±42 m, terrain
   again at ±120 m. `bake --force` discards plateaus — run `flatten` again
   (`build` does).
   *Crossings (check flight LFSB / LSZH, 2026-09-20):* strips were applied
   one after another, so at a crossing the last plane overwrote the first
   and the terrain rose through the other runway's strip (a green band
   across it). Now all strips are rasterised jointly: inside a strip the
   terrain takes the lowest plane of every strip covering the pixel, and
   the blend zones are weight-averaged instead of compounded. Before that,
   `reconcile_crossings` makes crossing or touching runways of one airport
   agree in height at the crossing (the longer keeps its plane, the shorter
   is shifted; Zurich 10/28 moved −3.1 m onto 16/34), so the two surfaces
   meet instead of one floating above the other. The shifted plane is what
   `plane_elev_m` stores, so the mesh follows.
   *Small airfields:* OurAirports `small_airport` (2 937 in the nine
   regions) was outside the large/medium selection, so Schmerlat (LSPF)
   and Thun (LSZW) had no runways at all. They are now included in the
   runway data, the plateaus and the catalog as `grass_strip`, or
   `regional_airport` when a runway is paved.
3. **Runtime** — done 2026-09-20. `src/runways_lib.cpp` (C++ `RunwaysLib`) merges the enabled
   packs per airport in priority order (D16; disabled packs ignored) and
   converts entries to the catalog shape (thresholds = pavement end moved
   inward by the displaced distance, true heading from OurAirports else the
   end bearing). `src/runways.cpp` (C++ node `Runways`, rebuilt with the scenery
   sources) draws one strip mesh per runway in a WUN frame at the runway
   midpoint, every vertex projected exactly onto the ellipsoid at the plane
   height (`plane_elev_m`, written by the flatten stage, else `elev_m`, else
   terrain) + 5 cm; `shaders/runway.gdshader` draws the surface and
   markings in metres (edge lines, centreline dashes, threshold stripes by
   width, aiming bars ≥ 1800 m, designators from a 5×7 bitmap font,
   displaced-threshold bar; grass and gravel get no markings). A pack entry
   with `"model"` loads the glTF at the reference point and `"procedural":
   false` suppresses the strips (D17). Smokes: `tests/smoke_runways.tscn`
   (merge, override precedence, catalog, mesh on plane to 0.1 mm),
   `tests/probe_runway_shader.gd` (compiles the shader with the real
   renderer). Full scene: 608 runways from 441 airports.
4. **Catalog** — done with step 3: `node_3d.gd::_catalog_with_pack_runways`
   replaces the runways of every pack-defined airport in the bundled
   catalog and appends pack airports the catalog lacks, so the welcome
   screen lists Zurich 16/34, 14/32, 10/28 with real thresholds. The
   heading+length derivation remains only for airports no pack defines
   (the curated CH/BW entries until they become an override pack).

### 12.6 Airfield lighting (2026-09-20)

Agreed with gery: runway lights, taxiway centreline green, taxiway edges
blue, approach lights on lit runways of 1 800 m and more, lights on from
dusk and whenever the visibility is below 1 km (the JAR low-visibility
tables are not modelled).

**Data.** OSM maps individual fixtures as `aeroway=navigationaid` nodes
(`navigationaid=als|papi|vasi`, `light=runway_edge|runway_centre|
runway_threshold|touchdown_zone|taxiway_edge|taxiway_centre` and the
older spellings). `build_world_runways.py` fetches them per region bbox
(cache `scenery_in_world/osm_lights/`, Overpass query must be `out;` —
`out tags;` drops the coordinates) and stores them under the nearest
airport (reach 4 / 3 / 2 km for international / regional / grass) as
`lights: [{kind, lat, lon, colour?, dir?}]`, kinds `als txe txc rwe rwt
rwc tdz papi vasi`; `flatten` writes each fixture's `elev_m` from the
final terrain (so they sit on the plateau). Zurich: 3 996 fixtures
(1 202 taxiway edge, 796 taxiway centre, 625 approach, 519 centreline,
468 edge, 240 TDZ, 130 threshold, 4 PAPI). Coverage varies a lot between
airports, so the mapped fixtures are an *override per class*: a class
with at least 8 mapped fixtures replaces the generated one for that
airport, otherwise the generated layout is used. PAPI/VASI are always
generated (OSM maps one node per system and never the angles) and lit day
and night: the shader applies the night intensity only to modes 0 and 1
(gery, 2026-11-01: "the PAPI should always be visible"). The four units
sit 30' and 10' either side of the end's approach slope, which is the ILS
glide path of that runway end from the ILS catalog (`Runways::papi_glide_deg`,
LSZH 28 = 3.3 deg) and 3 deg elsewhere (gery, 2026-11-08: "on Rwy 28 the
lights are always white").

**Generated layout** (`src/runways.cpp`, one PRIMITIVE_POINTS mesh per
airport, one vertex per fixture, lights 35 cm above the surface):

| class | rule |
|---|---|
| runway edge | white every 60 m, 1 m outside the pavement; yellow in the last 600 m of the direction of travel; every lit runway |
| threshold / end | green across the threshold every 6 m seen from the approach, red from the runway; the pavement end before a displaced threshold red |
| centreline | ≥ 1 800 m: white every 30 m, alternating red/white in the last 900 m, red in the last 300 m |
| PAPI | four units on the pilot's left, 300 m past the threshold, 15 m from the edge, 9 m apart, angles 3.5 → 2.5° from the unit nearest the runway outward (two white / two red on a 3° slope) |
| approach | ≥ 1 800 m: centreline bars every 30 m out to 900 m (international, Calvert-style crossbars widening outward) or 420 m (others), only visible from the approach side |
| taxiway | green centreline every 15 m, blue edges every 30 m 1.5 m outside the pavement, none on runway pavement |

Each fixture carries two colours (seen from either side of its axis), a
mode (two-way / directed / PAPI) and the axis in the airport frame in
CUSTOM0..2; `shaders/airport_lights.gdshader` picks colour and
visibility per vertex, sizes the sprite by distance (3–12 px) and draws a
soft additive disc. The host sets `intensity` per frame:
`_airfield_light_intensity` = 1 when the visibility is below 1 km, else
`clamp((4° − sun elevation) / 8°)`, so lights fade in from sun elevation
4° down to −4°.

**Verified** with `tests/probe_runway_shader.gd` (PROBE_HDG / PROBE_PITCH /
PROBE_FOV / PROBE_NIGHT, PROBE_TARGET prints where a point lands in the
view): Zurich 34 approach shows the mapped Calvert bars, the green
threshold row, edge and centreline lights; PAPI shows 1 / 2 / 3 white
units at 2.7 / 3.0 / 3.4° to the PAPI (which sits 300 m past the
threshold, so a 3° slope over the threshold reads 2.5° there and all
red — as in reality). Gotcha found on the way: in the double-precision
build the model matrix is camera-relative, so a shader must not combine
`MODEL_MATRIX` with `CAMERA_POSITION_WORLD`; the camera position in model
space is `inverse(MODELVIEW_MATRIX) * (0,0,0,1)`. With the wrong
direction, approach lights showed from behind and hid from the
approach. Whole scene: 687 308 fixtures over the nine regions (117 868
mapped from OSM at 351 airports, the rest generated), built by distance
like the surfaces.

All nine regions fetched and flattened 2026-09-20 with
`scripts/build_world_lights.sh` (retries Overpass per region, then
`flatten`). **Open:** real airports' actual approach system types (ALSF-2, SALS, …) are not
modelled beyond the two lengths; runway lights of unlit (`lighted:
false`) runways are not drawn.

## 13. Cloud streaming of the base tier — proposal 2026-09-22

Trigger (gery, 2026-09-22): the Windows build runs; the goal is the **base
tier streamed from a web server**. The 1 m Swiss and BW sceneries stay what
D10 made them: opt-in add-ons, downloaded by hand as today's packs (they are
not streamed). Test hosting: `https://www.md80.ch/provpilot/scenery/`
(static ISP web space, limited in size and speed — enough for a test area).
Related observation: 15 fps at take-off in Dübendorf dropping to 2–3 fps in
flight with both add-ons installed; see §13.6.

### 13.1 What the base tier is (measured, `/Volumes/Data1/provpilot/build`)

| Region | Terrain tiles (34 px float32 `.raw`, 4.6 KB each) | Terrain | `airports/runways.json` |
|---|---|---|---|
| Switzerland | 47 337 | 370 MB | 11 MB |
| Austria | 91 037 | 711 MB | 6 MB |
| Denmark | 59 771 | 467 MB | 9 MB |
| Germany | 368 912 | 2.8 GB | 68 MB |
| France | 555 978 | 4.2 GB | 68 MB |
| Italy | 329 964 | 2.5 GB | 28 MB |
| Norway | 413 713 | 3.2 GB | 14 MB |
| Sweden | 498 938 | 3.8 GB | 16 MB |
| Finland | 381 898 | 2.9 GB | 9 MB |
| **all nine** | **2.75 M** | **≈ 21 GB** | 230 MB |

One file per square kilometre, no orthophoto, no buildings; runways,
taxiways, aprons and night lights are built at runtime from the region's
`runways.json`. The streamer reads `terrain/tile_E_N.raw` by name from the
ordered region roots; there are no mips in the base tier — the 8 × 8 km far
chunks composite the same 34 px files.

### 13.2 What a flight touches

Near and medium rings (≤ `load_radius` = 8 tiles): 17 × 17 = 289 tiles =
1.3 MB. At 100 kt the aircraft enters ~50 new tiles a minute = **230 KB /
min** — negligible. The problem is the horizon: far chunks out to 200 km
cover a 400 × 400 km square = 160 000 tiles = 740 MB and 160 000 requests
per Fly!, all from the same 34 px files. So the base tier streams only if it
gets a **far level**: one small heightmap per 8 × 8 km chunk (the 33 × 33
samples the chunk mesh actually uses, int16 = 2 KB) — the full 200 km
horizon is then 1 600 chunks ≈ 3.5 MB, and each chunk's 64 tiles are only
fetched when the chunk enters the medium ring.

### 13.3 Wire format

Same principle as the packs: the server hosts a directory tree, one file per
unit, on a static host; the client cache is a region root the streamer
already knows. Units:

- `far/chunk_E_N.bin` — 33 × 33 int16 centimetres (2 KB) per 8 km chunk,
  built once from the 64 tiles by the bake (`build_world_terrain.py far`).
  Runtime: `S3DTileManager` builds a far chunk from this file when present
  and falls back to compositing tiles when not (engine change, small).
- `chunks/tiles_E_N.zst` — the chunk's 64 tiles as int16 centimetres +
  zstd (≈ 60 KB instead of 300 KB); decoded on the client into the 64
  `terrain/tile_E_N.raw` files the streamer reads, so it stays untouched.
  Godot decompresses Zstd natively (`PackedByteArray.decompress`).
- `airports/runways.json` per region as today (6–68 MB, once per region,
  cached); the client fetches it when the region is enabled.
- `terrain/manifest.json` (scalars only, D14) and a `stream.json` per
  region: chunk grid, tile counts, sizes, sha256 of each chunk archive and a
  version stamp, so the cache can validate and the UI can show what is
  cached.

Whole strip hosted this way: ≈ 4 GB of chunk archives + 25 MB of far files +
230 MB of runways — the ISP space decides how much of it goes up; the
Switzerland base alone is ≈ 90 MB.

### 13.4 Client

1. **Cache-or-fetch** in front of the tile manager's file opens: the region
   root is a cache with a size cap (LRU by chunk, default a few GB — the
   whole strip fits). A miss on any tile of a chunk queues that chunk
   archive; the tiles stay "loading" until it has landed, like a slow disk.
2. **Fetch worker**: a thread with 4 keep-alive connections (`HTTPClient`),
   requests ordered by ring distance and heading (the chunk ahead of the
   nose first, then the far files of the whole horizon, which are tiny).
   Retries with backoff; a 404 is recorded as `no_data` (ocean, outside the
   region) so the streamer does not wait for it.
3. **Region manifest**: `scenery_manifest.json` gains a `stream` block per
   base region (base URL, chunk grid, `stream.json` URL, version). Base
   packs stay downloadable as ZIPs for offline installs; a streamed region
   and an installed one are the same on disk.
4. **Settings → Scenery**: per base region "install / stream / off", cache
   size, and "pre-cache an area" (box on the map → fetch its chunks now).
   The add-ons keep their manual install rows.

### 13.5 Test area on the ISP web space

The Switzerland base: 740 chunk archives (≈ 60 KB each, 45 MB) + 740 far
files (1.5 MB) + `runways.json` (11 MB) ≈ **60 MB**, uploaded as the same
tree the client caches. First measurement against the Windows install:
bytes and requests per minute, and stalls, on a Dübendorf–Schaffhausen
flight at 100 kt with an empty cache; then Austria and Denmark (1.2 GB
of raw tiles → ≈ 250 MB hosted) to test crossing a region border.

### 13.6 The fps drop in flight

Separate from streaming, and it is the add-ons, not the base tier: 15 fps on
the ground and 2–3 fps in flight with the CH and BW detail packs installed is
not a GPU limit (the ground view draws more), it is streaming work and
memory. Two pointers from this machine: the headless smokes report the
process at **8 GB** after a few minutes with detail scenery, and every km²
in flight brings five building sheets (1.8 MB) and a 1 MB raw heightmap
through the decode path. Before anything else, profile one flight (Godot
profiler + the tile manager's counters): building sheet loads per minute,
JPEG decodes per frame, heightmaps retained, vertex budget hits. The likely
fixes: merged buildings beyond the detail radius, dropping heightmaps of
distant tiles after meshing (the README says so; verify it holds with two
regions), and int16 heightmaps for the add-ons too.

### 13.7 First start — recommendation 2026-09-22

State: the installer is ≈ 235 MB (Windows zip / macOS dmg) with the
aircraft and the airport catalog but no scenery. Settings → Downloads still
mounts the C++ `SceneryDownloaderPanel`, but `scenery_manifest.json` on the server
returns 404 (the scenery directory 403), so the tab looks dead. The welcome
page already marks airports by installed base region (✓ / ✗ via
`AirportsLib.installed_regions`).

Options gery weighed: (a) a first-start screen to pick a region to
download; (b) keep the Downloads tab and give it a world map with
downloadable sections (X-Plane style); (c) pick an airport, and the flight
fetches what it needs. Recommendation: **(c) as the flow, (b) as the
management view, no gate screen** — because with §13 the "needed scenery"
for a flight is a few MB, and a gate that asks a new user to choose a
region before the first flight is the thing the 235 MB download avoids.

- **Ship the far level in the installer.** The far files of the whole
  strip are ≈ 25 MB (Europe ≈ 50 MB): the horizon and the globe are flyable
  offline from the first start, only the near chunks stream.
- **Runways per airport, not per region.** `airports/runways.json` is
  6–68 MB per region; the runtime builds each airport from its own entry, so
  the wire form is `airports/<ICAO>.json` (kilobytes), fetched with the
  airport's chunks. The catalog (names, positions, ICAOs) stays in the app.
- **Welcome page**: every airport of every region is listed; the marker
  becomes "cached / streams / add-on installed" instead of ✓ / ✗, and Fly!
  on an uncached airport starts the fetch during the intro zoom (the
  zoom already waits for loading, §"intro as loading view"). Offline with
  an empty cache: the airport is greyed with "needs a connection or a
  pre-cached area".
- **Settings → Downloads → "Scenery"**: cache size (default a few GB), used
  space per region with "clear", the base regions as rows ("stream / install
  pack / off") and, later, the map: the VFR map already draws borders from
  a GeoJSON; painting region polygons (Natural Earth admin-0, the same
  registry the bake uses) with status colours and a "pre-cache this box"
  drag is the X-Plane view, worth doing once the streaming works. The
  add-ons (CH, BW) keep their manual pack rows.
- **Pre-cache** = the same fetch queue run over a box now (a trip without a
  good link), stored in the same cache; ZIP base packs stay for offline
  installs.

### 13.8 Cache location — recommendation

No question at first start. The app already picks an install root
(`InstallConfig`: the executable's folder for a portable install, else
`~/ProvPilot`, else the user data dir) and a `scenery` folder under it; the
streaming cache is that same `scenery/<Region>` tree, so a streamed region
and an installed pack are indistinguishable on disk and to the Scenery
Library. The Scenery tab shows the path with **Move…** (re-points the root,
offers to copy what is cached) and the cache size; the first-start default
is what most users keep, and the ones who care find it in one place. A
prompt on first start would ask for a decision before the user knows what
the cache is for.

### 13.9 Scope of the base tier — what else it should carry

Terrain and airports are baked; the base tier is meant to be flyable
everywhere, so it needs the things that make the ground readable from the
air. Sizes below are per km² *on the wire* (compressed), with today's CH
detail data as the yardstick where a class exists there:

| Class | Source | Form | Wire size / km² | Note |
|---|---|---|---|---|
| terrain | SRTM (baked) | 34 px int16 zstd | ≈ 1 KB | done |
| **land cover colour** | ESA WorldCover 10 m (CC BY 4.0, global) or Corine (Europe) | 64 px colour tile per km (JPEG / palette PNG) | ≈ 3 KB | **the biggest visual win**: fields, forest, urban, rock, water tint instead of a slope ramp; also drives procedural trees (forest class) and the far level's colour (chunk mip, 1 KB) |
| water | OSM (`natural=water`, coastline) — §2.2 | polygons, simplified (lakes > 1 ha, rivers > 10 m), GLB per km where present | ≈ 3 KB averaged (30 KB where present) | detail CH is 186 KB / tile with every pond; the base wants the lakes you navigate by and the coast |
| roads | OSM motorway / trunk / primary / (secondary) | ribbon GLB per km where present | ≈ 3 KB averaged | detail CH roads are 631 KB / tile with every street; base = the roads visible from 3 000 ft; railways optional |
| cities | OSM `landuse=residential/industrial` polygons | flat patches with the urban colour + **procedural generic buildings at runtime** (density from the polygon, FlightGear-style), night lights as points | ≈ 1 KB | no building meshes in the base; the add-ons keep their real buildings |
| airports | OurAirports + OSM (baked) | per-airport JSON | ≈ KB per airport | done; split per ICAO for streaming (§13.7) |

Total ≈ **12 KB / km²** → 770 KB per 8 km chunk, ≈ 0.6 MB / min in
flight at 100 kt (fine), and for hosting: Switzerland base ≈ 0.6 GB, the
nine baked regions ≈ 33 GB, Europe ≈ 60–100 GB. The ISP web space holds a
test area; the full base tier wants object storage with a CDN in front
(Cloudflare R2 / Bunny / S3 — a few € per month at these sizes, egress is
the cost to watch). Everything above is per-chunk archive like the terrain,
so the streaming mechanics do not change; the bake gains one step per class
(`build_world_landcover.py`, `build_world_water.py`, `build_world_roads.py`,
`build_world_landuse.py`), each reusing the OSM tooling of the CH/BW packs
(`convert_osm_bw.py`) with a "base" simplification profile.

Suggested order: land cover first (one raster source, the largest effect,
also fixes the flat colour of the far level), then water (coastline + lakes),
then roads, then landuse + procedural buildings. Trees need no data of their
own once land cover is in.

**Roads and railways — implemented 2026-09-22** (gery: start with roads and
rails from OSM for the Swiss base, baked not draped, taxiway quality, rails
at their real gauge, Zürich as the starter box). `scripts/build_world_roads.py
--region switzerland-base --bbox 2660000 1230000 2710000 1275000 --corridor`
→ `<region>/roads/roads_E_N.glb` per 1 km tile + manifest (`"shader":
"roads"`, `elevation_baked`). Source: Geofabrik PBF (streamed with
pyosmium, 16 s for the box), classes motorway…tertiary (+links) and rail /
narrow_gauge / light_rail / tram (yards and sidings dropped). Heights in
two passes. Pass 1:
base terrain bilinear, Gaussian-smoothed along the way (roads ±60–120 m,
rails ±250 m), and the **corridor stage rewrites the 34 px terrain tiles**
so the ground under every road and track follows that profile — the
base-tier equivalent of the runway flatten (D18). The corridor reaches
half width + 45 m (the 31 m pixel spacing × √2): anything narrower leaves
grid cells with one untouched corner, and on a hillside the ground
interpolated across such a cell rises through the ribbon (found 2026-09-22:
4 % of vertices buried with a 14 m margin). The shelf that gives is the
price of a 30 m posting. Pass 2, after every corridor is written: each
ribbon re-samples the final ground at five points across its width and
takes the maximum with its own profile — corridors of parallel tracks, of a
road beside a railway and of crossings overlap, so no ribbon may trust its
own profile; bridges are lifted the same way. Result
over the box: no vertex below the ground, ballast 0.15 m and roads 0.20 m
above it. **Bridges and tunnels** (gery 2026-09-22, after spotting broken
railway lines and flat interchanges: "make a good guess" for the missing
heights, no structures yet): OSM has no heights, so a bridge deck is the
straight chord between the grounds at the ends of its chain (a viaduct
split into several OSM ways is joined by shared end nodes), lifted by one
rise until it clears every way it crosses by 5.5 m (2D segment
intersection against the other ways' profiles, approach ways excluded)
and the ground under it by 0.3 m; every surface way ending at a lifted
deck ramps up to it at 4 % (its embankment goes into the corridor first,
so a road passing under then cuts its own level through it). Tunnels stay
in the data: a chain shorter than 200 m is an underpass through an
embankment that a 30 m posting cannot dig, so it is drawn at grade and
the crossing way covers it; a longer one dives 8 m below the surface over
40 m behind each portal (an end shared with another tunnel way stays deep)
and is drawn there, hidden by the terrain, so the line only disappears
where a real tunnel does. Ribbons of a lower class sit 1 cm lower per
step (motorway → trunk → primary → secondary → tertiary, links 5 cm below
their parents, plus a 0–1.2 cm per-way jitter) so merges and at-grade
crossings draw cleanly with the higher class on top; rails' heads at
+0.32 m poke through a road at a level crossing, as they should. Zürich
box: 1 753 bridge ways lifted over 1 489 crossings, 2 883 ramps, 313 tunnel
ways below ground, 170 short ones at grade. Note the corridor bakes are
cumulative on the terrain tiles: a clean slate needs the terrain rebuilt
(`build_world_terrain.py`) before the roads bake. Geometry: road width from the lane count per carriageway
(`lanes`, class default 2; lane 2.9–3.75 m by class, 0.5 m verges) — OSM
maps motorways and trunks one way per carriageway, so a one-way way is
a single carriageway with a 2.5 m hard shoulder (kind 1), the rare two-way
motorway way gets both carriageways and a 3 m median (kind 5), links and
one-lane one-way streets get edge lines only; the first bake drew a
24 m dual carriageway on *each* OSM carriageway. Rails: a ballast bed per
track (4.2 m + 3.8 m per extra track) plus two **extruded rails** per track
(UIC 60 section: 150 mm foot, 72 mm head, 172 mm high) at the gauge (OSM
`gauge`, default 1435 mm; narrow gauge 1000 mm), so from any angle they
read as rails, not floating lines. Vertex data carries metres in the UVs
(across / along; UV2 = half width and the lane or track count) and the
surface kind in the colour alpha; `addons/scenery3d/shaders/roads.gdshader`
draws asphalt with low specular, lane dashes / edge lines / hard shoulder
per kind, gravel with 2.6 m sleepers under each track of a bed, and rails
with a polished head and rusty flanks — one material for every tile
(S3DRoadManager parses TEXCOORD_0/1 and picks the shader when the manifest
says so; legacy tiles keep the vertex-colour material). Zürich box: 1 832
tiles, 119 MB (≈ 53 KB / km², to be compressed per the streaming plan),
3 364 km of road, 1 126 km of rail, 1 877 terrain tiles re-baked, 38 s per
bake. Smoke `tests/smoke_base_roads.tscn`; renders with the real-size A-10
(`probe_aircraft_lights`, `PROBE_E/N/DIST/PITCH/YAW`) — note the C172 and
PC-7 were still drawn at 1/5 scale then (A-10 port §4; real size since
2026-09-27), which made the first renders look like giant slabs. Open: no physics surface for driving yet,
level crossings and junctions are plain overlaps (ramp ends show as
stair-steps where they merge), tram tracks in streets share the street
ribbon, no bridge or tunnel structures (decks hover, portals are plain
dives; details later per gery), colours are pale under the current
sky/ambient like everything else. Corrections by hand: the scenery editor
(`docs/SCENERY_EDITOR.md`, F8) edits a vector design that the bake folds
in with `--design <file>` (its junctions, tapers and replace polygons
included; the manifest lists the baked designs' sha256).

**swissTLM3D for Switzerland (2026-09-23).** gery asked for better road
data than OSM; swisstopo's landscape model has it: `build_world_roads.py
--source tlm --bbox …` reads `tlm_strassen_strasse` and
`tlm_oev_eisenbahn` from the LV95 GeoPackage
(`/Volumes/Data1/scenery_in_world/swisstlm3d/SWISSTLM3D_2026_LV95_LN02.gpkg`,
10.8 GB, release 2026-02, open data) with pyogrio. Every vertex carries
a surveyed height, so a way's profile is the data itself: no terrain
smoothing, no bridge deck or tunnel guessing. Structures come from
`kunstbaute` (Bruecke → elevated with piers, Tunnel → drawn at its true
axis, Unterfuehrung → at grade, the corridor digs the underpass), road
classes from `objektart` with their nominal widths (10m / 8m / 6m / 4m /
3m Strasse → primary … tertiary with `width`; Autobahn → one-way
motorway carriageways; Autostrasse → trunk; Ein-/Ausfahrt → motorway
links; Zufahrt / Verbindung → tertiary links; 1m / 2m Weg, Platz, Treppe
skipped), one-way from `richtungsgetrennt` and `kreisel`, railways with
gauge from `objektart`, `anzahl_spuren` tracks, trams and rails in
streets without ballast, closed lines and funiculars skipped. Way ids are
32-bit hashes of the swisstopo uuid (the `_WAYID` attribute and the
sidecars). Roads narrower than 5 m draw edge lines only. Zürich box test
(12 × 12 km): 15 701 ways in 1 s, 351 surveyed bridges, 575 piers; without
a corridor a third of the vertices float more than 1 m over the 30 m
terrain, so the TLM bake needs `--corridor`, which then reproduces the
real embankments. OSM stays the source for the other regions. Not
covered: paths, squares, the OSM-id deletions of a design made on OSM
tiles (they do not match TLM ids).

**Clean network (2026-09-23).** gery's check of the Zürich TLM box found
piers standing on the motorway, roads swallowed by the terrain and
patchy joins ("as clean as the taxiways"). Measured on the tiles: 1.3 %
of the road vertices were below the runtime surface although none was
below the *bilinear* terrain — `S3DTile` triangulates every pixel quad
along one diagonal, and on a stepped corridor edge the two surfaces
differ by up to a metre; and 10–30 % of the vertices floated more than
1 m because a corridor written later overwrote the ground under an
earlier way (a tertiary 30 m from a motorway cut floated 8 m). Rules
now in `build_world_roads.py`:
- `Terrain.sample(tri=True)` reproduces the runtime triangulation; the
  final-ground pass, piers, fans and embankments use it, and the pass
  also samples halfway between stations (a terrain vertex between two
  stations bulges above the straight ribbon). Result: 0.00 % buried.
- Where corridors overlap, the pixel takes the mean of the ways' heights
  weighted by 1 / (distance from each ribbon *edge* + 3 m)²
  (`Terrain.corridor_acc` / `resolve_corridors`): under a ribbon the
  ground is its own, between two ways the step is spread over the overlap
  instead of jumping at a jagged pixel boundary (nearest-edge ownership
  drew sawtooth cliffs beside the Wallisellen rail embankment; lowest-wins
  floated 43 % of the vertices in the dense LSMD network). The runtime
  patches (`s3d_terrain_patches.h`, corridors carry `half`) blend the same
  way. What remains is the shelf itself: a 5 m embankment becomes one
  31 m pixel of slope, lit like any slope — finer terrain near the
  network is the only cure (open, see the swissALTI3D idea).
- A 31 m pixel shared by two ways at different heights still lifts the
  farther one, so every on-ground ribbon that rides more than 0.3 m over
  the runtime surface gets an **embankment strip** (kind 16, the
  terrain's own flat green, up normals and the terrain material's
  response so only its outline shows) from just under its edge outward:
  the strip walks down a 1:1.5 slope line in 0.5 m steps, at most 15 m,
  until the ground meets it (interpolated, so the outer edge is smooth; a
  cut side ends it at once), so it never becomes a flat flap where the
  ground rises again; runs are padded by one station so the strip tapers
  out (`emit_skirts`; 6 800 km over the Zürich box, tiles 362 → 511 MB).
  Since 2026-10-25 the strip also stops at the edge of a neighbouring
  ribbon at or below the road (`skirt_neighbours`: it dips under that
  pavement instead of cutting across it), a side whose edge already lies
  inside another ribbon gets none, a lone station needs 1 m of height
  (a terrain pixel made spikes), and the shader draws it matte in the
  tone the ortho-textured ground renders (specular 0.5 had mirrored the
  sky into pale blue-grey slabs).
- Sidecars (`roads_E_N.ways.json`) carry the *untrimmed* way to its
  nodes, so the editor snaps to the junction, not to a cut-back ribbon
  end; a way is also listed in the tiles of both its end nodes.
- The **baked network has junctions**: way ends meeting at one LV95
  point are a node (OSM shares node ids; swissTLM3D splits every line at
  a junction, so the point is the key). Three or more road legs get the
  corner-fan pavement a design junction gets (`junction_fan`, shared
  with `Design`) and their ribbons are cut back to it, at the height of
  the highest leg end / fan corner; two legs of one kind (roads or
  rails, any width) meet on a **mitre** — both ribbons end on the
  bisector (`end_frames` in `MeshBuf.profile`, rails included), so
  curves made of many TLM segments no longer show wedge gaps.
- **Piers** are skipped where the deck is less than 1 m over the ground
  and where another way passes underneath more than 1 m lower
  (`StationIndex` of the stations near elevated ways, `way_under`), so a
  bridge over the A1 has piers on the verges only.
- swissTLM3D `Unterfuehrung` ways are drawn like tunnels (surveyed axis,
  no corridor): the ground at a crossing belongs to the way on top.
- `roads.gdshader` pulls every vertex 0.6 % of its distance towards the
  camera beyond 300 m (full from 2.5 km): the block-averaged coarser
  terrain LODs of the far rings no longer bury roads seen from altitude.
Verification: `scratchpad verify.py` style check of ribbons, fans and
strips against the triangulated terrain — LSMD test box after the
change: road ribbons median +0.24 m, 0.00 % buried, 11 % more than 0.5 m
up (was 21–39 %), 1 % more than 2 m up (was 5–22 %).

**Autobahn first (2026-09-23, afternoon).** gery: "the transitions are
just not usable — from scratch with only Autobahn elements, nice
transitions between roads". Found in the data: swissTLM3D has *no*
three-way vertex among the motorway ways — an Ein-/Ausfahrt ends on or
beside the Autobahn line (0–17 m from its axis) without sharing a vertex,
so node-based junctions never fired at a merge and the ramp ribbon just
overlapped the carriageway. The bake now builds motorway pavement the way
the taxiways are built:
- `--classes motorway,motorway_link` bakes only those (the rest stays for
  later; `--source tlm` now runs in `.venv` — pyogrio, geopandas and
  shapely 2.1 were installed there, osmium is optional).
- **Attachment**: a road way end within (own half + other half + 4 m) of
  another road way's axis at the same level (±1.5 m), not running against
  it (one-way vs one-way with opposite travel, i.e. the other carriageway,
  is skipped), joins that way at the projection point; the other way is
  split there into two legs (`gaps` on the way, chainage intervals the
  ribbon leaves out).
- **Patch**: every junction that is not two equal legs on a mitre gets a
  2D union of its legs' ribbons back to where they stop overlapping
  ((half_a + half_b) / sin angle for legs within 60°, at most 300 m; the
  corner cuts of `junction_fan` for sharp legs), plus the **gore taper**
  of a merging ramp (legs within 32°): the wedge between the ramp's inner
  edge and the carriageway edge over the last 80 m. Patches that overlap
  and share a way are one polygon; constrained Delaunay (shapely 2.1),
  heights from the nearest leg station, 1 cm over the ribbons, plain
  asphalt (kind 6). Ribbons are cut back to the patch, seamlessly (the
  slice and the piece share the segment, so the same square frame).
- **Markings are geometry** (`emit_markings`, kind 17), drawn over the
  *whole* way (through the patches): edge lines piecewise between
  stations, lane and centre lines as 6 m dashes every 12 m, 15 cm wide,
  3 cm over the pavement, offsets per pavement kind (one-way: hard
  shoulder right). At a junction the other legs' pavement is a *cutter*
  (prepared shapely polygon, side, mode): for a way that ends there it
  drops every line inside, and — widened by the 3.5 m shoulder zone,
  because an edge line sits that far inside its pavement edge — its edge
  line on the side of the carriageway it merges into (a merging lane has
  no line of its own there); for a through carriageway the same widened
  zone turns the edge line on the ramp's side into dashes, so the
  acceleration lane is separated from the through lanes the way it is
  painted, while the lane dashes and the far edge line run on. All road
  ribbons are plain (kind 6); the shader's per-kind markings are no
  longer used for baked roads. Debug: `PVP_DEBUG_NODE=E,N` prints the
  legs, cuts and cutters of the junctions within 60 m.
- **Merge length is measured**, not a formula: a leg's cut is where its
  ribbon stops overlapping the other leg's, plus a 40 m lane taper for a
  through carriageway (`overlap_len`); the gore fill runs only while the
  ramp's inner edge is within 5 m of the carriageway edge, and a taper
  triangle joins the ramp lane's outer edge to the carriageway edge beyond
  the ramp's start.
- Triangles of patches, markings and areas are wound like the ribbons
  (`MeshBuf._tri`): the double-sided material lit the patches as back
  faces, from below, which made them look like another material.
- Ribbons have no per-way jitter any more (nothing overlaps at one level).
The Zürich box was rebuilt from scratch for this: the box's terrain tiles
deleted and re-baked from SRTM, airports re-flattened, the roads dir
cleared, motorways only. Everything else (side roads, railways) comes
back class by class once the Autobahn looks right.

**Junction blocks (2026-09-23, evening).** gery, from the SwissTopo 3D
viewer: the raw data has nothing on the transitions themselves — the
acceleration lane beyond an entry's nose, the deceleration lane before an
exit's nose, the tapers, the gore — so those must be generic building
blocks, "like Transport Fever". First two blocks are in the bake
(`BLOCK_*` constants, the block builder in stage 4a):
- **Travel direction first.** swissTLM3D has no direction attribute and
  digitises only ~3/4 of its one-way lines in the driving direction
  (checked over the box: 111 of 141 Ausfahrt lines that touch the Autobahn
  start there, 112 of 149 Einfahrt lines end there). The bake orients
  one-way TLM ways itself: a carriageway by right-hand traffic (the
  nearest roughly parallel line of its class 6–45 m away, the opposite
  carriageway, must lie on its left, three samples vote), a ramp by its
  type (Ausfahrt starts at the Autobahn, Einfahrt ends there), and chains
  of connected one-way ways of one class agree by majority, which also
  orients ramp pieces that touch nothing (`flip_way`, 21 flips in the
  box). The editor's green arrows follow the stored order, so they are
  now the driving direction.
- **Entry / exit block.** A one-way link ending on (Einfahrt) or starting
  from (Ausfahrt) a one-way motorway carriageway at a shallow angle: the
  ramp's last / first 60 m are replaced by a Hermite curve into the centre
  of an extra lane beside the through lanes (lane width of the class, the
  carriageway's shoulder becomes part of it: the lane sits on the outer
  edge line, its own verge outside); the lane runs 200 m beyond the nose
  (entry) or 150 m before it (exit) and tapers 1:20 into the carriageway
  edge, stamped in (chainage, offset) along the carriageway *and its
  same-class one-way continuation* (`chain_walk`), so it bends with the
  road and crosses TLM segment boundaries. The strip is plain asphalt
  1 cm over the ribbons; its outer edge line and taper line are drawn,
  and the carriageway's own edge line turns dashed along it through a
  side cutter. The ramp is 5 cm lower than the motorway (class drop), so
  where its ribbon and lines run under the carriageway or the lane they
  vanish on their own. No union patch is built at such a node.
- Everything else (collector lanes of class Autobahn, ramp-to-ramp joins,
  forks) still gets the union patch. Next blocks: lane add / drop, fork /
  join of carriageways, collector lane; then roundabouts and T-junctions
  for the ordinary roads. The block library is meant to become data the
  scenery editor can place by hand.
Zürich box: 50 entry and 26 exit blocks, 258 union patches left.

**Blocks are drawings (2026-09-25).** gery drew an Autobahneinfahrt in
Blender ("not to scale, a draft") and asked whether the library would be
made that way; agreed: a block is a `.blend` of named flat quads in the
carriageway's frame (X from the outer pavement edge, Y along travel, 0 at
the nose), exported to JSON, stamped by the bake. `docs/ROAD_BLOCKS.md`
has the convention (roles `pave`, `line`, `dash`, `gore`, `cut.dash`,
`cut.drop`, `ramp.join`, `ref`), `scripts/road_blocks/make_blocks.py`
draws the first drafts from the norms (entry, exit, lane_add, lane_drop
in `scenery/blocks/src`), `export_blocks.py` writes `scenery/blocks/*.json`,
and the bake's `BlockFrame` / `stamp_block` bend every object along the
carriageway chain (polygons segmentised every 5 m, clipped to the chain,
CDT in world space, heights from the chain; lines and dashes sampled along
their axis; cut polygons become side cutters; the ramp is re-joined to
`ramp.join`). The hard-coded entry / exit strip is gone. Lane add / drop
blocks are chosen where two one-way ways of one class continue each other
with a lane more or less (none in the Zürich box's TLM data yet: TLM does
not carry lane counts, the class gives 2). Left-side ramps mirror X.

**Level network (2026-10-25).** gery: "still stuck … messy when seen up
close and therefore not drivable" (A51 at Kloten in the editor). Measured
on the tiles: the two carriageways ran 1.5 m apart in height 10 m apart,
the southbound one with a 16 % hump at an OSM split, and the median held
the higher one's skirt. Three causes, three passes in
`build_world_roads.py`:
- `smooth_chains` (stage 1b): the along-track smoothing runs once per
  chain of ways joined end to end at a node where nothing else ends, not
  per OSM way. A 70 m way smoothed alone over the 124 m window was the
  mean of the two or three pixels it touched; the next way's mean
  differed by a metre or two. Surveyed ways and design chains with height
  offsets end a chain.
- `couple_parallel` (stage 2b): parallel ways with less than 8 m between
  their ribbon edges (both carriageways, a ramp beside its motorway, a
  road along a railway) share a cross-section height: the rank-weighted
  mean (motorway 4 … link 1, railway 3, surveyed ×3) of their profiles,
  three passes with a 15 m smoothing after each; crossings and other
  levels (more than 6 m apart) do not count; bridges, tunnels and deck
  approaches keep theirs.
- the corridor `cap`: the pixels that carry a ribbon (under it or within
  three quarters of a pixel of its edge) never end up more than 0.1 m
  above its profile. The blend with a higher neighbour's corridor used to
  lift the ground into the lower ribbon and `ground_under` then raised
  the ribbon into the hump.
Zürich 7 × 7 km box: the carriageways now agree within 0.2 m, the hump is
a 2.5 % grade, embankment strips 52 → 48 km; the bake prints the chains,
the stations the coupling moved and the stations steeper than 6 % on
motorway / trunk / primary. The pale green triangles in gery's screenshot
are the editor's one-way arrows (`update_direction_arrows`), not the
tiles. Open: the gore pavement of a block renders lighter grey than the
ribbon; embankment strips on a 30 m posting remain flat planes; the fine
Swiss terrain (`--terrain-overlay`) would make most of them go away.

**Coherent network (2026-10-25, afternoon).** gery, driving the car at
Balsberg: "the roads are not coherent … circles have to be clean circles
with nice transitions … sections of the autobahn have gaps in heights and
roads to bridges are too steep … the circles would be higher up than the
autobahn in reality, the bridge connects the two circles; the height model
does not give that away, so a little imagination is needed". Measured on
the tiles (a step finder over 2 m cells: pavement vertices of two ways
0.35–4 m apart): 96 step cells in the Kloten box. Causes and passes, all
in `build_world_roads.py`:
- Approach ramps stopped at the first OSM way (`ramp_approaches` raised
  only the way touching the deck; a 29 m way under a 4 m deck left 2.7 m
  as one step at its far end). `propagate` now carries any profile change
  (ramp, cutting, node fix) on over the network: along the way, then into
  every way at its far node whatever the degree, fading at the grade,
  never into decks, tunnels, surveyed ways or rings. Fields from several
  sources merge per station by the deepest dip / highest lift
  (`_collect`), they never stack (two carriageways and the link between
  them under one bridge had handed their dips to each other: 7 m).
- Bridge chords started from the raw ground at the abutments, not from the
  approach profiles: a step at every bridge end. They start from the
  profile of the way through the abutment point (`approach_h`, by point,
  so a ring or an unsplit through road counts too).
- Smoothing is one-sided at a junction, so two secondaries meeting on a
  slope arrived 3 m apart. `reconcile_nodes`: one height per node, the
  rank-weighted mean of the ends (decks, tunnels, surveyed ways and rings
  fixed; ramps and cuttings weigh three times as much), the difference
  propagated at 4 %; a way ending alone on another way (a T on an unsplit
  road, a link ending beside the carriageway) takes that way's height. It
  runs before the decks (so ramps and cuttings start from consistent
  approaches) and again after the coupling.
- **Cut or lift** (`bridge_decks`): a deck lifted by `rise` needs
  rise / grade of approach on both sides. The approach can be the road
  beyond the abutment, or the deck itself beyond its last crossing (a
  flyover coming down onto the motorway it joins fades its lift inside
  the deck: `fade_a` / `fade_b`); a lower-rank deck never ramps a higher
  road, a ring is no approach. Where that is short (Balsberg: two
  roundabouts 56 m apart, the Werftstrasse bridge between them, free 0 of
  138 m) the deck takes the share its approaches can ramp and the crossed
  ways dip for the rest (`apply_dip`: a cutting at 4 % for roads, 2.5 %
  for rail, propagated along their network) — the A11 now runs 5 m under
  the roundabout square with the square at grade, as in the Google Earth
  view. A short tunnel drawn at grade (an underpass) dips the same way
  under what crosses it (`underpass_dips`). A crossing within 2 m of the
  deck's own abutment (the ring it ends on) does not count.
- **Roundabouts** (`fit_roundabouts`, stage 1): every junction=roundabout
  ring (one closed way or several joined) is fitted with a least-squares
  circle; if its nodes lie within 25 % of the radius it becomes a true
  circle: nodes moved onto it at their own angle, arcs sampled every 3 m,
  every leg ending on a ring node bent to meet the circle radially over
  its last 18 m or 1.5 r (`bend_leg`, a Hermite curve), the ring's profile
  one flat plane (fixed in the reconciliation, left out of the coupling,
  no ramp or cutting walks into it), its island a flat disc just under the
  pavement in the ground's colour and no embankment strip on the inner
  side. At a ring node only the fan's corner cuts apply: the merge and
  gore rules (a 43 m lane taper on the through legs, an overlap cut on a
  tangential leg) had eaten the whole ring into one patch. Zürich box: 13
  rings, 1 kept as drawn (an oval).
- Two quick fixes after gery's close look (11:26): a fitted ring is at
  least 6.5 m wide (`RING_MIN_WIDTH_M`; the OSM tag "one lane, one-way"
  gave a 4 m ring no car could circulate on), and **lane overlaps**: a
  lower-rank way running along a higher one with its ribbon more than 1 m
  inside the other's (a merge or collector lane OSM draws as its own way
  a few metres off the carriageway axis) keeps its pavement but drops its
  own lines inside the carriageway's pavement, and the carriageway's edge
  line on that side turns into dashes for the stretch (marking cutters,
  the mechanism the blocks use). Both ribbons had painted their edge
  lines there: the doubled bands in the red box.
Kloten tiles: step cells 96 → 26, the rest tunnels against portals and
decks against the ways under them. The bake prints "roundabouts … node
steps reconciled … cuttings N (M dips)"; `PVP_DEBUG_STEEP=1` prints every
deck decision, ramp and way steeper than 6 %, `PVP_DEBUG_WAY=id,id` traces
the profile changes of those ways. Open: the big Flughofstrasse ovals stay
as drawn; a T-junction on an unsplit road still gets a union patch, not a
fan; the terrain of a cutting is one 31 m pixel wide.

**Network model, slices 1 and 2 (2026-10-25, evening).** gery agreed to
the lane-based network model ("let's do both in that order"); the plan with
six slices is `docs/ROAD_NETWORK_PLAN.md`. Done the same evening, in
`build_world_roads.py`: slice 1, markings per chain (`chain_off` /
`chain_fwd` from `iter_chains(chain_links(through=True))`, chains that
continue through a junction along the straightest same-class pair of legs,
also used by the chain smoothing; dashes and the asphalt grain laid out in
chain chainage; one `ASPHALT` colour for every class, in the mesher too;
the shader's broad asphalt variation reduced so patch and block boundaries
are not seams); slice 2, dual carriageways (`pair_carriageways`: the two
one-way ways of a motorway paired station by station, the separation
smoothed and held at the two half widths plus 1.5 m, both moved half the
difference, ramps following their nodes; a median strip in the ground
colour from each inner edge to the midline, no skirt on that side). Also
that day: `RING_MIN_WIDTH_M` 6.5 m and the lane-overlap cutters (a
lower-rank way inside a carriageway drops its lines there, the
carriageway's edge line dashes), and the editor's pick keeps the baked
height (`adopt_baked_heights`). Slice 3, step 1 (same evening): entry and
exit blocks generated from the measured overlap of the link with the
carriageway (`make_merge_block`, join at the overlap start, the link cut
back to it); the drawings entry.json / exit.json are unused. A node whose
ends were moved by different amounts in `pair_carriageways` had split by
position and lost its mitre and blocks: the ends at a node now share one
move. Zürich box after all of it: entry 12, exit 14, lane add 19, lane
drop 22, 82 dual-carriageway ways, 21 km of median.

Order of work stays as above: far files + chunk archives in the bake,
cache-or-fetch + far-file support in the streamer, per-airport runway
files, then the welcome markers and the Scenery tab.

Nothing above is implemented yet. Proposed order: far files + chunk
archives in the bake (they are also a smaller base pack), then the
cache-or-fetch layer and far-file support in the streamer, then the
manifest and the UI.

## 14. Forests — analysis and plan 2026-11-08

gery: "the tree I used in the SwissTopo scenery drags down FPS radically. But
most even low spec computers running flight simulators get trees and forests
quite right, with good FPS. See if you can find a good way to integrate
forests in PVP."

### 14.1 What the measurements say

| Item | Today | Note |
|---|---|---|
| Tree mesh `Tree_lowpoly.glb` | **24 302 triangles**, 3.5 MB PNG, two opaque materials | a game tree is 300-1 500 near, 2-8 as a billboard |
| `tree_lod2.glb` | 22 179 triangles, four textures | not a LOD of the first, same cost |
| swissTLM3D placement | 1 tree / 25 m² of forest (`--forest-spacing 5`) | 76 M trees for CH, 9 161 tiles, 1.7 GB; median 7 000 trees per km² tile, max 25 867 |
| Runtime (`S3DVegetationManager`) | one MultiMesh per 1 km tile, load radius 1.5 km, density 0.5, no LOD, no visibility range, shadows off | 9-16 tiles × ~7 000 × 0.5 × 24 302 tris ≈ **0.7-1.2 billion triangles** in view |

That last line is the whole problem: a thousand times the budget of a
flight-sim forest, not a GPU weakness. X-Plane draws a forest tree as two
crossed quads (4 triangles) from a species atlas, FlightGear's random
vegetation as 2-3 quads, MSFS as a few cards with a top cap, all of them
thinned with distance and faded out between 5 and 15 km. Low-spec machines
cope because a forest of 100 000 trees is then 400 000-800 000 triangles,
alpha-tested (no sorting), one draw per tile.

### 14.2 Plan

1. **Tree cards instead of meshes.** The manager builds its own ArrayMesh:
   three vertical quads at 60° plus one horizontal canopy quad (8
   triangles). A species atlas (`trees/tree_atlas.png`, columns = species,
   rows = side view / top view) is rendered in **Blender headless** from
   detailed tree models (`scripts/trees/bake_tree_cards.py`: orthographic
   side and top renders with transparent film, 256 px per card, assembled
   into the atlas + `tree_atlas.json` with heights and crown ratios).
   Sources: the existing 24 k tree and BlenderKit free models (`cc_zero`
   / `royalty_free`: Pine Tree Double, Pine Tree Monterey, White Willow,
   Yellow Birch, …) — a rendered card is a derivative we may ship. Three or
   four species to start: spruce / fir, beech / oak, pine, birch.
2. **Shader `shaders/tree.gdshader`**: alpha scissor (no transparency
   sorting), two-sided, species column and a colour / scale variation from
   `INSTANCE_CUSTOM`, the top quad fades in as the view angle steepens so
   the cross is never seen from above, a dither fade at the far edge, a
   little wind sway. One shared material (Metal limit, §12.6).
3. **Thinning with distance, in the manager**: per tile two instances —
   *near* (every kept tree, `visibility_range` 0-2.5 km) and *far* (every
   fourth tree at 1.6× crown, 2.5-8 km, fade). Both built once from the
   same records, so a tile costs no more to load than today. Load radius
   becomes 8 km (the far ring is cheap: a 1 km² forest tile at 1 750
   trees × 8 triangles = 14 k). Settings > Graphics "Tree density" keeps
   its five steps and gains the far radius.
4. **Species and height from the data**: swissTLM3D gives height; species
   from the `Wald` / `Gebuesch` / `Gehoelz` classes and altitude (conifers
   above 1 200 m) at extraction. Other regions: `build_world_forests.py`
   fills OSM `landuse=forest` / `natural=wood` polygons at a base-tier
   spacing (1 tree / 60 m², conifer / broadleaf by `leaf_type`) into the
   same `vegetation_E_N.bin` files, per pack like the roads (§13.9).
5. **Measure, not guess**: `tests/probe_forest_fps.gd` loads the Zürich
   Oberland tiles (dense forest), renders 10 s from 1 000 ft and prints
   frame time, triangles and draw calls; target ≥ 60 fps on this machine
   with "High", and a screenshot from the cockpit, from 3 000 ft and from
   above (the top cap).

Order: 2 + 3 first with a placeholder atlas rendered from the existing tree
(the FPS fix), then the Blender bake with real species, then 4.

### 14.3 Built 2026-11-08 — steps 1-3 and the atlas bake

- `S3DVegetationManager` (scenery3d submodule) gained **card mode**
  (`card_mode`, `atlas_path`, `atlas_meta_path`, `near_range_m` 2 500,
  `far_range_m` 8 000, `far_fraction` 0.25, `far_scale` 1.6,
  `snap_to_terrain`): the 8-triangle card mesh is built in code, one shared
  `ShaderMaterial` on `addons/scenery3d/shaders/tree.gdshader`, species from
  `trees/tree_atlas.json`, conifers by altitude (`pick_species`: 25 % at
  700 m rising to 97 % at 1 350 m), per tile a near and a far
  `MultiMeshInstance3D` with visibility ranges, at most three tiles built per
  frame, the load radius follows the far range. `tile_stats(e, n)` exposes
  CPU-side counts for tests (the headless renderer keeps no instance data).
- **Terrain snapping.** The SwissTopo records carry a 1 m DEM ground height;
  on the SRTM base tier most trees sat metres under the surface and forests
  looked like scattered bushes. The host hands the manager Scenery3D's
  `S3DElevationDB`; every tree takes the drawn terrain's height, and a tile
  built before its terrain arrived is rebuilt when the database's epoch moves.
- **Atlas**: `scripts/trees/bake_tree_cards.py` (Blender 5.2 headless, Eevee,
  transparent film, orthographic side + top renders, numpy alpha dilation
  so mips never darken) from `scripts/trees/species.json` →
  `provpilot/trees/tree_atlas.png` (1024 × 512, VRAM compressed, mipmaps)
  + `tree_atlas.json`. First set: beech and oak from the two SwissTopo GLBs,
  spruce and pine as procedural cone stacks (placeholders until BlenderKit
  species are rendered).
- **Measured** (`tests/probe_forest_fps.gd`, C172 at 1 000 ft over the
  Tösstal, Tree density High = 0.75, real renderer, this M-series Mac):
  **99.8 fps**, 244 vegetation tiles active, 7.3 M primitives and 2 400
  draw calls per frame, 1.2 GB video memory. The old path put about a
  billion triangles in the same view. Top view shows canopy discs, no
  crosses; the chase view a closed canopy following the forest outlines.
- Tests: `tests/smoke_vegetation.tscn` (15 checks: mesh, species, near /
  far counts, ranges, shared material, altitude mix, heights, snapping,
  density). Settings > Graphics > Tree density keeps its five steps.

Test flight 2026-11-08 (gery): the trees swayed in calm air and the canopy
card read as a pale disc, "a foreign part". Fixes: the sway amplitude is the
weather's wind (`set_wind_speed_mps`, node_3d.gd polls the FDM's
`atmosphere/wind-mag-fps` twice a second; zero in calm air), the canopy
card's mean luminance is matched to the side card at bake time (0.9 ×) and
it fades in only on steep views (view elevation 0.5-0.8).

Tree line (gery: "I cannot check if the trees get smaller in the mountains
and if the tree border is respected"): the swissTLM3D forest polygons end at
the tree line by themselves (records taper from 1 800 m, none above 2 300 m;
the pastures above stay bare), but the extraction gave every tree the same
22 ± 4 m. The manager now scales the height by an altitude factor, 1.0 to
1 300 m falling to 0.45 at 2 300 m (about 11 m at 2 000 m), and the card
scales uniformly: the species' crown width is already in the square atlas
image, scaling the card to the record's crown radius squeezed every tree to
60 % of its drawn width. Probe over Engelberg towards the Titlis
(`probe_forest_fps.gd -- <out> <veg> 3 2674500 1186500 160 2500`): forest
to the tree line, small trees on the upper slopes, bare alps above.

Flicker (gery: "visible flickering of chunks of forest … a border line
behind the airplane"): two causes, both per tile. The engine's visibility
range swapped a whole tile from its near to its far set at 2.5 km, and a
tile whose terrain arrived after its trees was dropped and re-queued, a gap
of several frames. Now both sets are drawn within ± 400 m of the hand-over
(the ranges only cull beyond that plus the tile half-diagonal) and the
shader crossfades them per tree by complementary dither
(`lod_split_m` / `lod_band_m`, `INSTANCE_CUSTOM.w` marks the far set); a
re-snapped tile is rebuilt in place, the old nodes freed in the frame the
new ones land.

**Species from BlenderKit (2026-11-08).** `bake_tree_cards.py` resolves a
`blenderkit:<assetBaseId>` source itself: the search API gives the free
model's glTF download URL, a GET with a fresh `scene_uuid` (no account
needed for free assets, the addon does the same) returns the file URL,
cached in `<cache_root>/blenderkit_trees/`; model name, author and licence
go into `tree_atlas.json`. Three first picks failed in a way the bake now
warns about (card coverage < 4 % or luminance outside 0.1-0.7): a pine
whose needles were particle instances the glTF export dropped, a birch
whose main material lost its base-colour texture, a maple with a baked
texture that renders black. The atlas has seven species: beech and oak
(the SwissTopo GLBs), Maple tree, Birch Tree lowpoly, Tree Dougles Fir
(the spruce column), Grand Fir, Pine Tree (all royalty-free, authors in the
JSON). Conifers = species 4-6 in `pick_species`.

**The rectangle around the aircraft** (gery 2026-11-08: "trees pop up from
out of nowhere, especially on low passes"): the tile manager fed the
elevation database only for tiles inside the fine-LOD box
(`LOD_DISCARD_THRESHOLD`, to bound the 4 MB heightmaps of the detail
packs). Outside the box a forest tile stood on its record heights, often
under the SRTM surface, and as the box swept over it the tile snapped up:
buried trees appearing along a rectangle. `S3DElevationDB` now has a
**coarse layer**: every loaded terrain tile leaves a ≤ 65 px copy
(`load_coarse_tile`, 4 KB; 289 tiles ≈ 5 MB) for as long as it is loaded,
`get_elevation_grid_any` / `has_any_tile` read fine-else-coarse. The
vegetation snaps from that layer out to the terrain radius (8 tiles, the
forest's far range); roads and water keep draping on the fine layer only.
A tile waits for terrain only while its own terrain tile is absent
(`pending_tile_count`, 0 of 240 in the Tösstal probe).

### 14.4 Forests from OpenStreetMap and clear roads (2026-11-08, step 4)

gery: "continue with step 2 [the other regions]; generally try to keep roads
clear of trees, even inside forests." Per-tree files do not scale (CH is
1.7 GB; Germany would be 15 GB in a base pack), so the other regions get
**polygons, filled at load time**: `scripts/build_world_forests.py --region
<id>` streams the Geofabrik PBF (pyosmium, system python3) and writes per
1 km square `vegetation/forest_E_N.bin` (format FOR1: forest polygons from
`landuse=forest` / `natural=wood` with the leaf type → broadleaf / conifer /
mixed and a density of 1 tree / 36 m², plus **corridors** = road and rail
centre lines with a half width per class, motorway 16 m … track 3 m, rail
5 m) for the squares that have a terrain tile. A few KB per square:
Switzerland's corridors are 7 KB a square, Austria's forests ≈ 20 000
squares. `S3DVegetationManager` fills the polygons on its worker thread
(jittered grid at the spacing, deterministic per square, holes honoured,
`species_hint` from the leaf type) and drops every tree within a corridor's
half width + 1 m. The corridors of every forest file of a square also thin
the **surveyed** `vegetation_E_N.bin` trees of that square, across packs, so
the SwissTopo forests clear their roads too (`--roads-only` bake for the
Swiss base pack); where a surveyed tile exists the polygon fill stands
down (surveyed data wins). Tests: `tests/smoke_forest_tiles.tscn` (fill
count against the polygon area, no tree in a hole or a corridor, leaf type
→ species group, surveyed tile thinned by the corridor, forest file standing
down) and the Tösstal probe at 600 ft (roads clear through the Bauma forest).

Open:
SRTM measures the canopy in forests, so the ground under a forest is
already tree-height high — a later land-cover pass could lower it.

## See also

- [gerys_plan.md](gerys_plan.md) — the plan this subtask serves
- [SCENERY_ARCHITECTURE.md](SCENERY_ARCHITECTURE.md) / [WORLD3D.md](WORLD3D.md) — runtime & globe
- [SCENERY_DISTRIBUTION.md](SCENERY_DISTRIBUTION.md) / [SCENERY_LIBRARY.md](SCENERY_LIBRARY.md) — packs & library
- [TERRAIN_SETUP.md](TERRAIN_SETUP.md) — current CH/BW terrain provenance

