# fgdata subset

Generic FlightGear models the aircraft packages reference but do not ship
(`/Aircraft/Instruments-3d/...`), copied from the FlightGear base package
(fgdata, https://gitlab.com/flightgear/fgdata, branch `next`). Licensed under
the GNU GPL version 2 (see COPYING), like the FlightGear A-10 package.

`scripts/fg_a10_build.py` resolves absolute `Aircraft/...` model paths here
first, then in a local FlightGear install. Add further instruments by copying
their folder (`.ac`, `.xml`, textures) from fgdata.

| Folder | Used by |
|---|---|
| `Aircraft/Instruments-3d/altimeter` | A-10 cockpit altimeter (`Models/Cockpit/altimeter.xml`) |
| `Aircraft/Instruments-3d/vor` | Cessna 172 VOR 1 course deviation indicator (`panel.xml`) |
| `Aircraft/Instruments-3d/adf` | Cessna 172 ADF indicator |
| `Aircraft/Instruments-3d/kx165` | Cessna 172 KX 165 NAV/COMM 1 radio (nav frequency tuning) |
| `Aircraft/Instruments-3d/kr87-adf` | Cessna 172 KR 87 ADF receiver (`kr87.xml`; the KI 227 indicator files come with the folder) |
| `Aircraft/Instruments-3d/asi300` | PC-7 airspeed indicator (300 kt scale) |
| `Sounds/wind.wav` | ASK 21 wind sounds (`Sounds/ask21jsb-sound.xml`); `scripts/fg_a10_sounds.py` resolves generic `Sounds/` paths here |
