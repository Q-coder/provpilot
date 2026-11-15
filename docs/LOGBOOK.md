# Logbook, black box, pause

Since 2026-11-15.

## Logbook

`FlightLog` (`src/flight_log.cpp`) records every flight and writes it when the
next flight is requested or the window closes; flights under 30 s are not
kept. One pilot for now ("Pilot"); profiles come later.

- `logbook.json` next to provpilot.cfg: `{schema, pilot, flights: [...]}`,
  newest first, at most 2000. Per flight: id (start time, unix), pilot,
  aircraft, start / end time, from / to (the airport within 5 km by its ICAO
  code, else the coordinates) with lat / lon, duration, time in the air,
  distance flown, highest altitude, fastest indicated and ground speed, largest
  climb and descent rate, load factor range, take-offs, landings, hardest
  touchdown, result ("Completed" or "Crash: reason").
- `logbook/<id>.json`: the black box, a sample every 2 s (`t_s, lat, lon,
  alt_ft, ias_kt, vs_fpm, g`); beyond 5400 samples the track is kept at half
  the rate.

Settings > Logbook (`LogbookPanel`, `src/logbook_panel.cpp`): totals, the
flights as a table (the running flight first), and **Black Box** (or a double
click): the flight's figures and its altitude / speed profile over time.

Test: `probe_logbook_shot -- <out_dir>`; `probe_crash` prints the entry count.

## Pause

`pause` in the key catalog, **P** (the fine throttle-up key moved from P to
U). Freezes the flight model and mutes the sound; the scene keeps drawing,
"PAUSED" stands at the top. P again goes on. Not available over the menus,
while a flight loads or while the crash dialog is up.

## Crash dialog

With crash detection on (docs/CRASH_DETECTION.md) a crash freezes the flight
and opens a dialog with the reason: **Restart** begins the same flight again,
**Resume** closes the dialog and stays at the scene (the monitor is quiet for
6 s, then armed again). The logbook entry of the flight says "Crash: reason"
either way.

## Open

- Pilot profiles; a map of the track in the black box; export.
- A flight that ends by switching to the welcome screen without a new Fly!
  stays open until the next flight or the exit.
