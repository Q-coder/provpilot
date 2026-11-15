# Crash detection

`CrashMonitor` (`src/crash_monitor.cpp`), driven from `node_3d.gd`
(`_update_crash`), since 2026-11-15. Settings > Realism > Crash Detection
(`crash_detection`, default on for new installs).

## What counts as a crash

| Case | Rule | Message |
|---|---|---|
| Hard touchdown | sink rate at the first contact above `touchdown_sink_mps` (wheels first, default 6 m/s) or `airframe_sink_mps` (anything else first, 3 m/s) | Hit the ground at N ft/min |
| Airframe on the ground | a non-wheel contact down at more than 30 kt with over 15 deg of bank or the nose more than 3 deg down | Wing / Nose struck the ground |
| Overturned | any contact with over 60 deg of bank, or pitch below -45 / above 60 deg | Overturned on the ground |
| Water | any contact over the sea for half a second | Came down on water |
| Overstress | load factor beyond 1.5 x `g_pos` / `g_neg` for 0.3 s in the air | Structural failure at N g |
| Overspeed | indicated speed above 1.15 x `vne_kt` for 3 s (0 = not checked) | Structural failure at N kt |

A tail strike on rotation and a level belly landing are not crashes. The
limits are per aircraft in `aircraft.cfg`:

```
[limits]
    g_pos   7.33     # limit loads; the airframe fails at 1.5 x
    g_neg   -3.0
    vne_kt  450
    touchdown_sink_mps 6.0
```

A-10 7.33 / -3.0 g, 450 kt; MD-80 2.5 / -1.0 g, 340 kt; C172 3.8 / -1.52 g,
163 kt; PC-7 6.0 / -3.0 g, 270 kt; ASK 21 6.5 / -4.0 g, 151 kt. Defaults
without a section: 3.8 / -1.52 g, no speed check.

## What happens

With the checkbox on: the flight is frozen and a dialog names the reason, with
**Restart** (the same flight again) and **Resume** (stay at the scene); see
docs/LOGBOOK.md. With it off the reason is only printed to the log and the
flight goes on (over water the aircraft sinks, docs/SEA.md).

The monitor is quiet for 6 s after a flight starts and while the spawn pins
the aircraft, in slew mode and for ground vehicles.

## The rebound of a hard touchdown (always on)

JSBSim's contact springs return what a real airframe absorbs by bending and
breaking: an aircraft flown into the ground came back up, sometimes faster
than it arrived. After a first contact with more than 3 m/s of sink the
upward speed is capped for 1.5 s at 30 % of that sink rate (at least 0.8 m/s;
`JSBGodot::limit_climb_speed`). Normal landings and touch-and-goes are below
the threshold and untouched.

Test: `probe_crash -- <id> <dive|g|land|drop> [detection 0|1]`.

## Open

- No damage model: nothing breaks off, gear does not collapse; a crash is the
  end of the flight or nothing.
- Collisions with buildings, trees and other objects are not detected (the
  FDM only knows the ground).
