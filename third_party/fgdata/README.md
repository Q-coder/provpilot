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
