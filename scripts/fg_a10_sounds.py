#!/usr/bin/env python3
"""Export the FlightGear A-10 sound configuration for ProvPilot
(docs/A10_FLIGHTGEAR_PORT.md, subtask 8): Sounds/A-10-sound.xml -> sounds.json
plus the referenced .wav files, for the FGSounds runtime (provpilot/fg_sounds.gd).

    python3 scripts/fg_a10_sounds.py --src /Users/gery/Desktop/A-10 --out provpilot/aircraft/a10/sounds [--out aircraft/a10/sounds]

Records keep the XML verbatim as nested dicts ({tag, name, mode, file, raw});
generic FlightGear sounds ($FG_ROOT/Sounds/*.wav) the package does not ship
are listed under "missing" and get no file.
"""
import argparse, json, os, shutil, sys
import xml.etree.ElementTree as ET


def xml_to_dict(e):
    kids = list(e)
    if not kids:
        return (e.text or "").strip()
    d = {}
    for k in kids:
        v = xml_to_dict(k)
        if k.tag in d:
            if not isinstance(d[k.tag], list):
                d[k.tag] = [d[k.tag]]
            d[k.tag].append(v)
        else:
            d[k.tag] = v
    return d


# The A-10's engine sounds test the LEFT generator switch as their "engine
# running" condition (FlightGear quirk): switching that generator off silences
# every engine. ProvPilot keys them on the engine's own N1 instead — the N1
# property the record's <volume>/<pitch> entries already read.
def override_engine_conditions(rec):
    if rec["tag"] != "engine":
        return
    raw = rec["raw"]
    n1 = "engines/engine/n1"
    for key in ("volume", "pitch"):
        entries = raw.get(key, [])
        entries = entries if isinstance(entries, list) else [entries]
        for e in entries:
            prop = (e.get("property") or "").strip().lstrip("/") if isinstance(e, dict) else ""
            if prop.startswith("engines/engine") and prop.endswith("/n1"):
                n1 = prop.replace("[0]", "")
    def walk(x):
        if isinstance(x, dict):
            for k, v in list(x.items()):
                if k == "property" and isinstance(v, str) and "controls/electric/engine" in v and v.endswith("generator"):
                    x[k] = n1
                    rec["override"] = "engine condition on %s" % n1
                else:
                    walk(v)
        elif isinstance(x, list):
            for v in x:
                walk(v)
    walk(raw.get("condition"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", action="append", required=True)
    args = ap.parse_args()
    base_xml = ET.parse(os.path.join(args.src, "A-10-base.xml")).getroot()
    sound_path = None
    for s in base_xml.iter("sound"):
        p = s.findtext("path")
        if p:
            sound_path = p
    if sound_path is None:
        sys.exit("no <sound><path> in A-10-base.xml")
    xml_file = os.path.join(args.src, sound_path.replace("Aircraft/A-10/", ""))
    fx = ET.parse(xml_file).getroot().find("fx")
    records, missing, files = [], [], {}
    for snd in fx:
        path = snd.findtext("path")
        if not path:
            continue
        rec = {"tag": snd.tag, "name": (snd.findtext("name") or snd.tag).strip(), "mode": (snd.findtext("mode") or "once").strip(),
               "path": path.strip(), "raw": xml_to_dict(snd)}
        override_engine_conditions(rec)
        local = os.path.join(args.src, path.replace("Aircraft/A-10/", "")) if path.startswith("Aircraft/A-10/") else None
        if local and os.path.exists(local):
            rel = os.path.relpath(local, os.path.join(args.src, "Sounds")).replace(os.sep, "_")
            files[rel] = local
            rec["file"] = rel
        else:
            rec["file"] = ""
            if path not in missing:
                missing.append(path)
        records.append(rec)
    for out in args.out:
        os.makedirs(out, exist_ok=True)
        for rel, local in files.items():
            shutil.copyfile(local, os.path.join(out, rel))
        with open(os.path.join(out, "sounds.json"), "w") as f:
            json.dump({"source": "FlightGear A-10 " + sound_path, "sounds": records, "missing": missing}, f, indent=1)
    print("fg_a10_sounds: %d sounds, %d files, %d missing generic files (%s) -> %s" % (len(records), len(files), len(missing), ", ".join(missing), ", ".join(args.out)))


if __name__ == "__main__":
    main()
