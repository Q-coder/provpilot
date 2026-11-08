#!/usr/bin/env python3
"""
build_world_ils.py — ILS catalog for every runway of provpilot/airports.json.

Real ILS data comes from FlightGear's `Navaids/nav.dat.gz` (X-Plane 810
format, data cycle 2013.10 by Robin Peel, GPL-2 — the last freely
redistributable worldwide navaid set; ILS frequencies and idents are very
stable, and this is the same licence as the fgdata instruments we vendor).
Rows 4 / 5 (localizer), 6 (glide slope), 12 (ILS DME) are matched to the
runway ends of our airport catalog by ICAO and designator (L/R/C-tolerant;
an end whose nav.dat course points the other way is assigned to the opposite
end). Each entry keeps the nav.dat antenna positions for reference; the
runtime (NavaidsLib) builds the localizer and glide slope geometry from the
catalog runway itself, so the ILS is always aligned with the runway the
aircraft spawns and lands on. A localizer whose course deviates more than
8 deg from the runway is an offset approach (Innsbruck LOC/DME East): it is
flagged `offset` and keeps the nav.dat course and position.

Real ILS only by default (gery, 2026-11-01: an approach that does not exist
in the real world must not exist here). With --synthetic every paved catalog
runway of at least 1500 m without a real ILS gets an invented one (`source:
synthetic`, 3.0 deg glide slope, one ILS channel per runway shared by both
ends, chosen to be unused within 150 nm, ident I + letters of the ICAO code);
the map and the HUD mark those with an asterisk.

Usage:
    python scripts/build_world_ils.py [--offline] [--refresh] [--synthetic] [--verbose]
Cache: <cache_root>/fgdata/nav.dat.gz (scripts/world_regions.json cache_root).
"""

import argparse
import gzip
import json
import math
import re
import sys
import urllib.request
from datetime import date
from pathlib import Path

NAV_DAT_URL = "https://gitlab.com/flightgear/fgdata/-/raw/next/Navaids/nav.dat.gz"
REPO = Path(__file__).resolve().parent.parent
REGIONS_JSON = REPO / "scripts" / "world_regions.json"
AIRPORTS = REPO / "provpilot" / "airports.json"
NAVAIDS = REPO / "provpilot" / "navaids.json"
OUT = REPO / "provpilot" / "ils.json"
UPDATES = REPO / "scripts" / "ils_updates.json"   # AIP corrections on top of the 2013 nav.dat

FT_TO_M = 0.3048
NM_M = 1852.0
OFFSET_DEG = 8.0            # localizer course vs runway: beyond this it is an offset approach (Innsbruck, Lugano)
SYNTH_MIN_LENGTH_M = 1500
SYNTH_SEPARATION_NM = 150.0
# the 40 ILS localizer channels (108.10 .. 111.95 MHz, odd tenths and their +0.05)
ILS_CHANNELS_KHZ = [108100 + 200 * i + o for i in range(20) for o in (0, 50)]

COMMENT = ("ILS per runway end of provpilot/airports.json, built by scripts/build_world_ils.py. "
           "Real localizers / glide slopes / ILS DMEs from FlightGear nav.dat (GPL-2, cycle 2013.10); "
           "runtime geometry comes from the catalog runway (NavaidsLib) unless `offset` is true, "
           "when the nav.dat localizer course and position are used. `source` is 'nav.dat' or "
           "'synthetic' (paved runway >= 1500 m without real ILS, invented frequency and ident). "
           "freq_khz: 110500 = 110.50 MHz. Entries with `updated` carry a correction from "
           "scripts/ils_updates.json (frequency / ident changes since 2013, from the AIPs).")


def load_cache_root():
    with open(REGIONS_JSON) as f:
        return Path(json.load(f)["cache_root"])


def ensure_nav_dat(offline: bool, refresh: bool) -> Path:
    path = load_cache_root() / "fgdata" / "nav.dat.gz"
    if path.exists() and path.stat().st_size > 0 and not refresh:
        return path
    if offline:
        if path.exists():
            return path
        sys.exit(f"--offline but no cached {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    print(f"  downloading {NAV_DAT_URL}")
    req = urllib.request.Request(NAV_DAT_URL, headers={"User-Agent": "provpilot-build/1.0"})
    with urllib.request.urlopen(req, timeout=120) as r:
        path.write_bytes(r.read())
    return path


def parse_nav_dat(path: Path):
    loc, gs, dme = {}, {}, {}
    with gzip.open(path, "rt", encoding="latin-1") as f:
        for line in f:
            fields = line.split()
            if len(fields) < 10 or fields[0] not in ("4", "5", "6", "12"):
                continue
            try:
                lat, lon = float(fields[1]), float(fields[2])
                elev_m = float(fields[3]) * FT_TO_M
                freq = int(fields[4]) * 10          # 10830 -> 108300 kHz
                mult = float(fields[6])
            except ValueError:
                continue
            ident, icao, rwy = fields[7], fields[8], fields[9]
            key = (icao, rwy)
            if fields[0] in ("4", "5"):
                loc[key] = {"lat": lat, "lon": lon, "elev_m": elev_m, "freq": freq, "course": mult % 360.0,
                            "ident": ident, "cat": " ".join(fields[10:]), "loc_only": fields[0] == "5"}
            elif fields[0] == "6":
                gs[key] = {"lat": lat, "lon": lon, "elev_m": elev_m, "freq": freq,
                           "angle": math.floor(mult / 1000.0) / 100.0, "course": mult % 1000.0}
            else:
                dme[key] = {"lat": lat, "lon": lon, "elev_m": elev_m, "freq": freq, "bias_nm": mult}
    return loc, gs, dme


def ends_of(designator: str):
    return [p.strip() for p in designator.split("/") if p.strip()]


def strip_lrc(end: str) -> str:
    return re.sub(r"[LRC]$", "", end)


def end_heading(runway, idx):
    return runway["heading_deg"] % 360.0 if idx == 0 else (runway["heading_deg"] + 180.0) % 360.0


def angle_diff(a, b):
    return (a - b + 180.0) % 360.0 - 180.0


def find_runway_end(airport, rwy_name, course):
    """(runway, end index) for a nav.dat runway name; None when the airport
    has no such runway. When the named end points the other way (swapped
    designators between the datasets), the end facing the course is taken."""
    hit = None
    for r in airport["runways"]:
        e = ends_of(r["designator"])
        if rwy_name in e:
            hit = (r, e.index(rwy_name))
            break
    if hit is None:
        for r in airport["runways"]:
            e = [strip_lrc(x) for x in ends_of(r["designator"])]
            if strip_lrc(rwy_name) in e:
                hit = (r, e.index(strip_lrc(rwy_name)))
                break
    if hit is None:
        return None
    r, idx = hit
    if len(ends_of(r["designator"])) >= 2 and abs(angle_diff(course, end_heading(r, idx))) > 90.0:
        idx = 1 - idx
    return r, idx


def haversine_nm(lat1, lon1, lat2, lon2):
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dl = math.radians(lon2 - lon1)
    a = math.sin((p2 - p1) / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * 6371008.8 * math.asin(math.sqrt(a)) / NM_M


def synthetic_ident(icao: str, idx: int) -> str:
    letters = re.sub(r"[^A-Z0-9]", "", icao.upper())[-2:] or "XX"
    return "I" + (letters if idx == 0 else letters[::-1])


def pack_runway_files(extra):
    """airports/runways.json of every scenery area in provpilot.cfg plus --packs dirs."""
    roots = list(extra or [])
    cfg = REPO / "provpilot.cfg"
    if cfg.exists():
        roots += re.findall(r'"path":\s*"([^"]+)"', cfg.read_text())
    files = []
    for r in roots:
        for cand in (Path(r) / "airports" / "runways.json",):
            if cand.exists() and cand not in files:
                files.append(cand)
    return files


def apply_updates(entries, path: Path, verbose: bool):
    """scripts/ils_updates.json: per (icao, rwy) a new freq_khz / ident, or remove."""
    if not path.exists():
        return 0, 0
    with open(path) as f:
        ups = json.load(f)["updates"]
    by_key = {(e["icao"], e["rwy"]): e for e in entries}
    n_changed = n_removed = 0
    for u in ups:
        e = by_key.get((u["icao"], u["rwy"]))
        if e is None:
            print(f"  WARNING: ils_updates: no catalog ILS for {u['icao']} {u['rwy']}")
            continue
        if u.get("remove"):
            entries.remove(e)
            n_removed += 1
            continue
        before = f"{e['ident']} {e['freq_khz'] / 1000:.2f}"
        for k in ("freq_khz", "ident", "cat", "gs_deg"):
            if k in u:
                e[k] = u[k]
        e["updated"] = u.get("source", "ils_updates.json")
        n_changed += 1
        if verbose:
            print(f"  update {u['icao']} {u['rwy']}: {before} -> {e['ident']} {e['freq_khz'] / 1000:.2f}")
    return n_changed, n_removed


def compare_with_xplane(entries, path: Path):
    """Report mode: our catalog against a locally installed X-Plane earth_nav.dat
    (type 4 localizer rows). Nothing is copied: the report names the ILS whose
    frequency or ident differs, the ones that are gone and the ones we lack,
    for checking against the AIPs and adding to ils_updates.json."""
    xp = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.split()
            if len(p) < 11 or p[0] != "4":
                continue
            xp[(p[8], p[10])] = (int(p[4]) * 10, p[7])
    ours = {(e["icao"], e["rwy"]): e for e in entries}
    changed = [(k, e["ident"], e["freq_khz"], xp[k][1], xp[k][0]) for k, e in sorted(ours.items())
               if k in xp and (xp[k][0] != e["freq_khz"] or xp[k][1] != e["ident"])]
    gone = [k for k in sorted(ours) if k not in xp]
    prefixes = {e["icao"][:2] for e in entries}
    new = [k for k in sorted(xp) if k[0][:2] in prefixes and k not in ours]
    print(f"=== compare with {path}: {len(changed)} changed, {len(gone)} gone, {len(new)} not in our catalog ===")
    for k, i0, f0, i1, f1 in changed:
        print(f"  changed {k[0]} {k[1]}: {i0} {f0 / 1000:.2f} -> {i1} {f1 / 1000:.2f}")
    print("  gone:", ", ".join(f"{k[0]} {k[1]}" for k in gone))
    print("  new:", ", ".join(f"{k[0]} {k[1]} {xp[k][1]} {xp[k][0] / 1000:.2f}" for k in new))


def main() -> int:
    p = argparse.ArgumentParser(description="FlightGear nav.dat + catalog -> provpilot/ils.json")
    p.add_argument("--updates", default=str(UPDATES), help="AIP corrections applied after the match (default scripts/ils_updates.json)")
    p.add_argument("--no-updates", action="store_true", help="plain nav.dat, no corrections")
    p.add_argument("--compare", metavar="EARTH_NAV_DAT", help="report the differences to an X-Plane earth_nav.dat and exit (nothing written)")
    p.add_argument("--packs", action="append", default=[], help="extra scenery pack dir with airports/runways.json")
    p.add_argument("--offline", action="store_true")
    p.add_argument("--refresh", action="store_true")
    p.add_argument("--synthetic", action="store_true", help="also invent an ILS for paved runways >= 1500 m without one (default: real ILS only)")
    p.add_argument("--no-synthetic", action="store_true", help=argparse.SUPPRESS)  # the old default, kept for muscle memory
    p.add_argument("--verbose", action="store_true")
    args = p.parse_args()

    with open(AIRPORTS) as f:
        airports = json.load(f)["airports"]
    by_icao = {a["icao"]: a for a in airports}
    # scenery packs' airports/runways.json (OSM-surveyed ends with precise
    # headings) replace the catalog runways of the airports they list and add
    # airports the catalog lacks, as RunwaysLib does at runtime
    n_pack = 0
    for rfile in pack_runway_files(args.packs):
        with open(rfile) as f:
            pack = json.load(f)
        plist = pack.get("airports", [])
        plist = plist if isinstance(plist, list) else list(plist.values())
        for pa in plist:
            rws = []
            for r in pa.get("runways", []):
                ends = r.get("ends") or []
                des = r.get("designators") or [e.get("ident") for e in ends]
                if len(des) < 2 or len(ends) < 2:
                    continue
                rws.append({"designator": "/".join(des), "heading_deg": float(ends[0].get("heading_deg", 0.0)),
                            "length_m": float(r.get("length_m", 0.0)), "surface": r.get("surface", "asphalt")})
            if not rws or "icao" not in pa:
                continue
            a = by_icao.get(pa["icao"])
            if a is None:
                a = {"icao": pa["icao"], "name": pa.get("name", ""), "lat": pa.get("lat", 0.0), "lon": pa.get("lon", 0.0),
                     "elevation_m": pa.get("elevation_m", 0), "type": pa.get("type", "regional_airport"), "runways": []}
                airports.append(a)
                by_icao[a["icao"]] = a
            a["runways"] = rws
            n_pack += 1
    if n_pack:
        print(f"  {n_pack} airports take their runways from the scenery packs")
    vor_freqs = []
    if NAVAIDS.exists():
        with open(NAVAIDS) as f:
            vor_freqs = [(n["freq_khz"], n["lat"], n["lon"]) for n in json.load(f)["navaids"]]

    print(f"=== {date.today().isoformat()}: ILS catalog -> {OUT.name} ===")
    loc, gs, dme = parse_nav_dat(ensure_nav_dat(args.offline, args.refresh))
    entries = []
    taken = set()          # (icao, designator, end idx) with a real ILS
    no_airport = 0
    no_runway = []
    offsets = []
    for (icao, rwy), l in sorted(loc.items()):
        a = by_icao.get(icao)
        if a is None:
            no_airport += 1
            continue
        hit = find_runway_end(a, rwy, l["course"])
        if hit is None:
            no_runway.append(f"{icao} {rwy}")
            continue
        r, idx = hit
        end = ends_of(r["designator"])[idx] if len(ends_of(r["designator"])) > idx else rwy
        dev = angle_diff(l["course"], end_heading(r, idx))
        offset = abs(dev) > OFFSET_DEG
        if offset:
            offsets.append(f"{icao} {end} {dev:+.1f}")
        g = gs.get((icao, rwy))
        d = dme.get((icao, rwy))
        e = {
            "icao": icao, "designator": r["designator"], "rwy": end,
            "ident": l["ident"], "freq_khz": l["freq"], "cat": l["cat"],
            "course_true_deg": round(l["course"], 3),
            "gs_deg": g["angle"] if g else None,
            "loc": {"lat": l["lat"], "lon": l["lon"], "elev_m": round(l["elev_m"], 1)},
            "gs": {"lat": g["lat"], "lon": g["lon"], "elev_m": round(g["elev_m"], 1)} if g else None,
            "dme": {"lat": d["lat"], "lon": d["lon"], "elev_m": round(d["elev_m"], 1), "bias_nm": d["bias_nm"]} if d else None,
            "offset": offset,
            "course_dev_deg": round(dev, 2),
            "source": "nav.dat",
        }
        entries.append(e)
        taken.add((icao, r["designator"], idx))

    n_real = len(entries)
    if args.compare:
        compare_with_xplane(entries, Path(args.compare))
        return 0
    n_upd, n_rm = (0, 0) if args.no_updates else apply_updates(entries, Path(args.updates), args.verbose)
    if n_upd or n_rm:
        print(f"  {n_upd} corrections from {Path(args.updates).name} applied, {n_rm} ILS removed")
    n_synth = 0
    if args.synthetic and not args.no_synthetic:
        used = [(e["freq_khz"], e["loc"]["lat"], e["loc"]["lon"]) for e in entries] + vor_freqs
        for a in sorted(airports, key=lambda x: x["icao"]):
            for r in a["runways"]:
                e2 = ends_of(r["designator"])
                if len(e2) < 2 or r.get("surface") not in ("asphalt", "concrete") or r["length_m"] < SYNTH_MIN_LENGTH_M:
                    continue
                free_ends = [i for i in range(2) if (a["icao"], r["designator"], i) not in taken]
                if not free_ends:
                    continue
                # one channel per runway, unused within SYNTH_SEPARATION_NM
                chan = None
                for c in ILS_CHANNELS_KHZ:
                    if all(not (f == c and haversine_nm(a["lat"], a["lon"], la, lo) < SYNTH_SEPARATION_NM) for f, la, lo in used):
                        chan = c
                        break
                if chan is None:
                    continue
                used.append((chan, a["lat"], a["lon"]))
                for i in free_ends:
                    entries.append({
                        "icao": a["icao"], "designator": r["designator"], "rwy": e2[i],
                        "ident": synthetic_ident(a["icao"], i), "freq_khz": chan, "cat": "ILS-cat-I",
                        "course_true_deg": round(end_heading(r, i), 1),
                        "gs_deg": 3.0, "loc": None, "gs": None, "dme": None,
                        "offset": False, "course_dev_deg": 0.0, "source": "synthetic",
                    })
                    n_synth += 1

    entries.sort(key=lambda e: (e["icao"], e["rwy"]))
    with open(OUT, "w") as f:
        json.dump({"_comment": COMMENT, "source": f"FlightGear nav.dat cycle 2013.10{' + ils_updates.json' if n_upd or n_rm else ''}{' + synthetic' if n_synth else ''}, {date.today().isoformat()}",
                   "ils": entries}, f, indent=1)
        f.write("\n")
    airports_with = len({e["icao"] for e in entries})
    print(f"  {n_real} real ILS matched ({no_airport} nav.dat ILS at airports outside the catalog, "
          f"{len(no_runway)} with no such runway in the catalog), {len(offsets)} offset localizers, "
          f"{n_synth} synthetic; {airports_with} airports have an ILS")
    if args.verbose:
        print("  offset localizers:", ", ".join(offsets))
        print("  runway not in catalog:", ", ".join(no_runway))
    print(f"  written: {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
