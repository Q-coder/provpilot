# Radio navigation: VOR, DME and NDB

How ProVPilot receives ground-based radio navaids, where the station data
comes from, which instruments show it, and how to use it in the Cessna 172.
Added 2026-10-31.

## 1. Data source

`provpilot/navaids.json` is built from **OurAirports** `navaids.csv`
(https://ourairports.com/data/, public domain, updated daily), the same
source as the airport catalog. `scripts/build_world_navaids.py --all`
selects the stations of every region in `scripts/world_regions.json` by
ISO country (clipped to the region's bbox, like the airports) and keeps
VOR, VOR-DME, VORTAC, DME, TACAN, NDB and NDB-DME. Per station: ident, name,
type, frequency in kHz for every type (`114850` = 114.85 MHz, `375` =
375 kHz), WGS84 position, elevation, the paired DME frequency, the
declination the VOR's radials are slaved to (`slaved_variation_deg`, with
OurAirports' per-station `magnetic_variation_deg` as the fallback) and the
power class (LOW / MEDIUM / HIGH) that sets the nominal range. 901 stations
for the nine regions on 2026-10-31. Re-runs replace a region's entries;
`--refresh` re-downloads the CSV into `<cache_root>/ourairports/`.

Magnetic headings need the Earth's field: `GeoMagnetic` (`src/geo_magnetic.cpp`)
is the **World Magnetic Model WMM2025** (NOAA NCEI / BGS, public-domain
coefficients from `WMM.COF`, valid 2025.0-2030.0), a degree-12 spherical
harmonic synthesis as in the WMM technical report. It reproduces all 100
of NOAA's `WMM2025_TestValues.txt` to 0.01 deg in declination and
inclination and 2 nT per component (checked during development with a
standalone build of the same source; `tests/smoke_radio_nav.gd` keeps five
of the values).

## 2. Classes

| Class | File | Role |
|---|---|---|
| `NavaidsLib` | `src/navaids_lib.cpp` | loads the catalog once; `find_frequency(khz, kind_mask)`, `nearby(lat, lon, radius_nm, mask)`, `nearest`; Vincenty inverse on WGS84 (`distance_m`, `bearing_deg`, `inverse`), `angle_diff_deg` |
| `GeoMagnetic` | `src/geo_magnetic.cpp` | `declination_deg(lat, lon, alt_m, decimal_year)`, inclination, total intensity, `decimal_year_now()` |
| `RadioNav` | `src/radio_nav.cpp` | the receivers: NAV 1 (VOR + CDI) with its DME, ADF 1; `update(delta, lat, lon, alt_m, true_heading)`, `write_properties()` |
| `FGBridge` | `src/fg_bridge.cpp` | owns the `RadioNav`, feeds it the tuned frequencies, OBS and ADF card from the cockpit state, publishes the properties, emulates the KR 87 logic, makes every heading instrument magnetic |
| `FGControls` | `src/fg_controls.cpp` | new binding features the fgdata radios use: `<condition>` on bindings, `property-swap`, `mask` integer / decimal, `resolution`, knob `shift-repeat`, one wheel step per knob across its records |
| `VfrMap` | `src/vfr_map.cpp` | draws the navaids with ident and frequency |
| `FlightHud` | `src/flight_hud.cpp` | the NAV / ADF status line above the readouts |

Kinds are bit masks (`NavaidsLib.KIND_VOR`, `KIND_DME`, `KIND_NDB`,
`KIND_TACAN`): a VOR-DME is `KIND_VOR | KIND_DME`, a plain DME answers on
the VHF frequency of the paired channel but gives no azimuth (TRA
Trasadingen 114.30 is one; the CDI shows the NAV flag, the DME reads).

## 3. The receiver model (RadioNav)

Conventions follow FlightGear's `navradio` and `adf` instruments so fgdata's
gauges work unchanged. A `RadioNav` is one receiver: `receiver_index` 0 is
NAV 1 and writes `instrumentation/nav/...` plus the DME and the ADF; index
n > 0 writes `instrumentation/nav[n]/...` only. The bridge runs two (NAV 1
and NAV 2 since 2026-11-01, `get_radio_nav` / `get_radio_nav2`): NAV 2 takes
`instrumentation/nav[1]/frequencies/selected-mhz` and
`instrumentation/nav[1]/radials/selected-deg` from the cockpit state (the
MD-80's FGCP NAV 2 / CRS 2 knobs, the first officer's HSI reads it).

- **Station selection**: every station on the tuned frequency (kHz match)
  of the right kind; the one the aircraft is deepest inside the range of
  wins (frequencies are reused across Europe), re-evaluated every 2 s or
  on retune.
- **Range**: the service volume from the power class (VOR: terminal 25,
  low 40, high 130 nm; NDB: 25 / 50 / 75 nm), VHF additionally cut by the
  radio horizon `1.23 * sqrt(height above the station in ft) nm` (never
  below 8 nm). KLO on the field is received on the ground; a 25 nm VOR is
  not until the aircraft climbs (test: 450 m no, 2500 m yes). NDBs have no
  horizon. Inside the cone of confusion over a VOR (horizontal distance
  below 0.85 x height) the azimuth drops out, the DME keeps reading.
- **Radial**: the true bearing from the station to the aircraft minus the
  station's slaved declination. A VOR's radials are magnetic as the station
  was set up, not as today's field is; stations without a published value
  use the WMM at the station.
- **CDI**: course error `r = OBS - radial` normalised to +-180; beyond 90
  deg the needle shows the inbound course and the TO flag, else FROM;
  `heading-needle-deflection` is `r` clamped to +-10 deg (full scale),
  positive = fly right. First-order lag 0.4 s.
- **DME**: slant range in nm from the ground distance and the height
  difference; ground speed from the smoothed range rate; time to station.
- **ADF**: relative bearing to the NDB (0 = nose, 90 = right wing), needle
  lag 0.8 s on the shortest way round; parked at 90 deg without a signal
  (ANT behaviour). The compass card is a knob (`rotation-deg`).
- **Magnetic heading**: `environment/magnetic-variation-deg` is the WMM
  declination at the aircraft (recomputed after 0.05 deg of travel);
  the magnetic compass, the directional gyro and the A-10 HSI read
  `true - variation`. Zurich 2026: 3.65 deg E, so runway 16 (155 true)
  shows 151 on the card.

Properties published (nav[0] spellings normalise to `nav`):
`instrumentation/nav/{in-range, to-flag, from-flag, heading-needle-deflection[-norm],
radials/selected-deg, radials/actual-deg, radials/reciprocal-radial-deg,
heading-deg, nav-distance, nav-id, signal-quality-norm, frequencies/selected-mhz}`,
`instrumentation/dme/{in-range, indicated-distance-nm, indicated-ground-speed-kt, indicated-time-min}`,
`instrumentation/adf/{in-range, indicated-bearing-deg, rotation-deg, ident, frequencies/selected-khz}`,
`instrumentation/kr-87/...` (display logic), `orientation/heading-magnetic-deg`,
and the A-10's `sim/model/A-10/instrumentation/cdi/*` follow NAV 1.

## 4. Cockpit (Cessna 172)

Four fgdata models (GPL-2, vendored in `third_party/fgdata`) were added to
`aircraft/c172p/model/panel.xml` and built into the glb by
`scripts/fg_panel_build.py`:

| Instrument | fgdata model | Where |
|---|---|---|
| VOR 1 CDI (course ring, needle, TO / FROM / NAV flags) | `Instruments-3d/vor/vor.xml` | fourth hole of the top row |
| ADF (needle over a rotating card) | `Instruments-3d/adf/adf.xml` | fourth hole of the middle row |
| KX 165 NAV/COMM 1 | `Instruments-3d/kx165/kx165-1.xml` | first row of the painted radio stack |
| KR 87 ADF receiver | `Instruments-3d/kr87-adf/kr87.xml` | second row of the stack |

Knobs (right click / wheel up clockwise, left click / wheel down counter-clockwise; Shift = coarse):

- **OBS**: the VOR's course knob at the lower left of the gauge (fgdata's
  invisible click zones, `vor__click-L/R`); Shift turns 5 deg.
- **NAV frequency**: the right-hand knobs of the KX 165 tune the standby
  frequency, outer MHz (108-118) and inner 50 kHz; the swap button between
  the windows (`NavToggle`) flips it into use. COMM side the same (display
  only, 118-137 MHz, 25 kHz).
- **ADF frequency**: the KR 87's knobs step the standby by 1 kHz (inner)
  and 100 kHz (outer), 100-1300 kHz; **FRQ** swaps standby and active. ADF /
  BFO buttons switch the annunciators; the volume knob is the power switch.
- **ADF card**: wheel over the card face itself (the fgdata ADF has no knob
  mesh); Shift turns 5 deg.

Defaults at every flight start (`FGBridge::switch_defaults`): NAV 1 114.85
KLO Kloten with 116.90 WIL Willisau in standby, ADF 335 BER Bern with 375
GLA Gland in standby, OBS 0. The panel manifest's own `<params>` aliases
(the way fgdata parameterises its models) are resolved by the builder
(`resolve_aliases` in `scripts/fg_a10_build.py`).

The **VFR map** (N) draws VORs as a hexagon, NDBs as a dotted ring, lone
DME / TACAN as a square, with "ident frequency" up to the 20 km view. The
**HUD** (H, readouts mode) shows a line like
`NAV 114.85 KLO R340 TO 1.1 NM   ADF 335 ---`.

The A-10's HSI course deviation bar and TO / FROM flags follow NAV 1 (its
own nav radio panel has no frequency knobs wired yet).

## 4b. ILS (added 2026-10-31, same day)

**Data**: `scripts/build_world_ils.py` writes `provpilot/ils.json`. Real
localizers, glide slopes and ILS DMEs come from FlightGear's
`Navaids/nav.dat.gz` (X-Plane 810 format, data cycle 2013.10 by Robin Peel,
GPL-2: the last freely redistributable worldwide set; OurAirports has no
ILS). Rows are matched to the runway ends of our airport catalog, with the
scenery packs' `airports/runways.json` (found through the scenery areas of
`provpilot.cfg`) replacing the catalog runways where present, as RunwaysLib
does at runtime. 437 real ILS matched (2026-10-31); a localizer whose
nav.dat course differs more than 8 deg from the runway is an offset approach
(Innsbruck LOC/DME East, Lugano) and keeps the nav.dat course and position
(`offset: true`, 10 such). The catalog holds **real ILS only** (437 entries,
gery 2026-11-01: an approach that does not exist in the real world must not
exist here; the 492 invented ones of the first build are gone). `--synthetic`
would add one per paved catalog runway of 1500 m or more without a real ILS
(`source: synthetic`, 3.0 deg slope, one channel per runway chosen to be
unused within 150 nm, ident `I` + the last two ICAO letters); the map and the
HUD would mark those with an asterisk (`ILS*`).

**Corrections** (2026-11-08, gery: "I don't get an ILS for LSZH Rwy 14 on
111.75"): the 2013 cycle is stale. Zurich moved ILS 14 (IKL) from 108.30 to
111.75 and Lugano its LOC 01 (ILU) from 111.50 to 108.90, and a comparison
with a current X-Plane 12 `earth_nav.dat` (Navigraph cycle 2406, the user's
own installation, `--compare <path>` prints the report and writes nothing)
shows 36 frequency and 15 ident changes among our 437, 111 entries that no
longer exist (79 at airports without any ILS today, many of them nav.dat's
back-course duplicates, 32 at airports whose other ILS remain) and 95 ILS
in our regions that we lack. That data is not redistributable, so
`scripts/ils_updates.json` holds the corrections as **facts per (icao,
rwy)** — a new `freq_khz` and/or `ident`, or `remove: true` — each with a
`source` line; `build_world_ils.py` applies them after the runway match and
marks the entry `updated`. The two Swiss changes are verified against the
AIP charts; the other 49 come from the comparison and are marked "AIP check
pending". Removals and additions are left for a free current source
(none exists; OpenStreetMap's `aeroway=navigationaid` is a candidate).

**Geometry** is not taken from nav.dat: `NavaidsLib::resolve_ils()` builds
it from the runway the sim draws and spawns on, so the approach ends on the
runway. The welcome screen hands its airport list (catalog with the packs'
runways merged in) to `NavaidsLib::set_airports()`; for each ILS the landing
threshold and far end (LV95) give the course, the localizer antenna 300 m
beyond the far end, the glide slope and DME 300 m past the threshold at the
airport elevation, and the sector half width `atan(107 m / (length + 300 m))`
clamped to 1.5 .. 3 deg (full scale 107 m abeam the threshold, ICAO Annex
10). ILS whose runway is not in the active list (a pack not installed) stay
unresolved and silent.

`smoke_radio_nav` also flies the LSZH 28 final on 109.75 (IZW: both
needles centred at 3 nm) and then retunes 110.75 at the same spot, which is
ILS 34 (IZS): needle pegged, glide slope flagged — the picture a wrong
channel gives (gery, 2026-11-01). Runway 10/28 only exists in the Swiss base
pack, so the test merges that pack's runways like the welcome screen does and
skips when the volume is not mounted.

**Receiver** (`RadioNav::update_ils`): on an ILS channel (108.10 .. 111.95
odd tenths) the ILS wins unless only a VOR / DME is receivable. Both ends of
a runway on one channel: the receiver takes the end whose approach side the
aircraft is on. Localizer deflection = course error / half width x 10,
TO on the approach side, reverse sensing on the back course like a real
localizer; `nav-loc` = 1 and the OBS ring has no effect. Glide slope =
(angle - elevation angle from the GS site) x 5, 0.7 deg full scale,
positive = fly up, flagged beyond 15 nm (ICAO guarantees 10; crews intercept at 12-13, gery 2026-11-01), on the back course or outside three
sector widths. DME = slant range to the GS site (about 0.2 nm over the
threshold). Reception: 25 nm cut by the radio horizon. `signal-quality-norm` is 1
within range and fades over the last tenth of it (2026-11-01; it used to be
`1 - d / range`, and the MD-80's GS flag trips below 0.99).

The Cessna's VOR gauge shows the glide slope needle and GS flag; the HUD
line reads `ILS 110.50 IZH RWY 16 LOC +0.3 GS -0.1 4.8 NM`. The VFR map
draws a feather per ILS (apex at the threshold, 7 km long, left half
shaded; faint for synthetic) with `ILS 110.50 16` up to the 20 km view.

Not modelled: marker beacons (nav.dat has them; no indicator in the panel
yet), localizer back-course approaches as published procedures, CAT
differences.

## 5. Tests

- `tests/smoke_radio_nav.gd` (SceneTree script, 2 s): ILS IZH LSZH 16 from
  5 nm out (both needles centred, TO, DME, needle sense left of course and
  below the slope, back course FROM without glide slope); WMM2025 against five
  NOAA test values, the catalog (KLO on 114.85, TRA as DME only, BER on
  335), Vincenty Bern-Zurich 96 137 m, receiver geometry 10 nm north of
  KLO (radial, FROM / TO, needle sign on both sides of the course, DME
  slant range, horizon at 25 nm, 200 nm out), ADF relative bearing on two
  headings.
- `tests/smoke_c172_panel.tscn`: the four models present, KLO received on
  the ground at LSZH with the DME at 1.1 nm, 110.50 tunes the ILS IZH, the compass magnetic (3.65 E),
  OBS Shift+wheel 5 deg, KR 87 kHz knob and FRQ swap.
- `tests/smoke_a10_cockpit.tscn`: HSI card at the magnetic heading.
- `tests/smoke_vfr_map.tscn`: KLO drawn in view (the zoom-key checks of
  that test fail since the keyboard-mapping change of 2026-10-11, before
  this work).

## 6. Open points

- Marker beacons (outer / middle / inner) are in nav.dat but not modelled;
  the Cessna panel has no marker lights.
- The real-ILS list is the 2013 cycle; ILS added or removed since are not
  reflected (a hand override file would be the fix, as for the navaids).
- The Toggle_Avionics_Power switch of the Cessna model is not wired; the
  radios run from the main bus (ALT / BAT).
- Audio idents (Morse) are not played.
- NAV 2 has no DME of its own (the MD-80's DME 2 readout) and no ADF 2.
- The PC-7 got the same gauges and radios on 2026-10-31 (KX 165 and KR 87
  on the right-hand console, docs/INSTRUMENTS.md §5).
