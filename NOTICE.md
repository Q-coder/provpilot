# Third-party components and licences

ProVPilot's own source code (everything in this repository that is not listed
below) is released under the MIT License (`LICENSE`).

## Licence of the distributed program

`src/it_autoflight.cpp` / `.h` are a C++ port of the mode logic of
IT-AUTOFLIGHT from the FlightGear MD-80 by Octal450 (Joshua Davidson),
licensed under the GNU General Public License, version 2. They are compiled
into `libprovpilot`. The built library, and a ProVPilot release that contains
it, is therefore a combined work distributed under the **GNU GPL, version 2**
(text: `provpilot/aircraft/md80/LICENSE`). The MIT licence continues to apply
to ProVPilot's own files taken by themselves. The complete corresponding
source of every release is this repository at the release tag.

## Code

| Component | Licence |
|---|---|
| Godot Engine (double precision fork), godot-cpp | MIT |
| JSBSim | LGPL 2.1 |
| Scenery3D, world3d | MIT |
| IT-AUTOFLIGHT port (`src/it_autoflight.*`) | GPL-2.0 |

## Aircraft and models

| Item | Source | Licence |
|---|---|---|
| MD-81 / 82 / 83 / 87 | FlightGear MD-80, Octal450 | GPL-2.0 |
| A-10 | FlightGear A-10: Lee Elliott, Alexis Bory, David Bastien and contributors | GPL-2.0 |
| ASK 21 | FlightGear ASK21: Heiko Schulz, Thomas Polzer, Viktor Radnai, Benedikt Wolf, Gilberto Agostinho and contributors | GPL-2.0 or later |
| Instruments of the Cessna 172 and PC-7, `third_party/fgdata` | FlightGear base package | GPL-2.0 |
| Cessna 172 model | "Cessna172" by KOG_THORNS, Sketchfab | CC-BY-4.0 |
| Locomotive Re 4/4 | "Swiss Lok Re 4/4" by hotdoghans, Sketchfab | CC-BY-4.0 |
| Car | "Low poly car 6" by assetfactory, Sketchfab | Sketchfab Standard |

Each aircraft folder carries its licence text and credits (`LICENSE`,
`CREDITS.txt`, `AUTHORS.md`). The ported files are modified from the
originals (converted to glTF, rescaled, adapted); the conversion scripts are
in `scripts/`.

## Data

| Data | Source | Licence |
|---|---|---|
| Swiss terrain, orthophotos, buildings, roads, water | swisstopo | free geodata licence, source to be named |
| Baden-Württemberg terrain, orthophotos | LGL Baden-Württemberg | dl-de/by-2-0 |
| Coastline, roads, forests, airports, runways, taxiways | © OpenStreetMap contributors | ODbL 1.0 |
| World terrain | NASA SRTM | public domain |
| Globe elevation | NOAA ETOPO | public domain |
| Country outlines | Natural Earth | public domain |
| Airports, navaids | OurAirports | public domain |
| ILS data | X-Plane nav.dat (Robin Peel) | GPL |
| Magnetic model | NOAA / NCEI WMM2025 | public domain |
| METAR | aviationweather.gov | public domain |

## Trademarks

Aircraft, manufacturer, airline and railway names and liveries are trademarks
of their owners. They are used to identify what is simulated; ProVPilot is
not affiliated with or endorsed by any of them.
