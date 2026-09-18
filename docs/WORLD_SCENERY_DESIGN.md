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
under the loading screen; `_track_globe_rebase()` in `node_3d.gd::_process`
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
old `GLOBE_SHOW_ALT_M = 25 km` gate in sky_atmosphere.gd hid the coarse globe
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
mask frame 1. sky_atmosphere.gd derives the observer from the camera's
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
`Atmosphere` module (never updated since sky_atmosphere.gd took over) is
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
   current camera rides with the pinned aircraft.
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

**Implemented (live weather, METAR):** `provpilot/live_weather.gd`
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

## See also

- [gerys_plan.md](gerys_plan.md) — the plan this subtask serves
- [SCENERY_ARCHITECTURE.md](SCENERY_ARCHITECTURE.md) / [WORLD3D.md](WORLD3D.md) — runtime & globe
- [SCENERY_DISTRIBUTION.md](SCENERY_DISTRIBUTION.md) / [SCENERY_LIBRARY.md](SCENERY_LIBRARY.md) — packs & library
- [TERRAIN_SETUP.md](TERRAIN_SETUP.md) — current CH/BW terrain provenance

