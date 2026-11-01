#!/usr/bin/env python3
"""Export a FlightGear aircraft's liveries for ProvPilot.

FlightGear liveries are PropertyList files (Models/Liveries/<variant>/<id>.xml)
that set texture properties (sim/model/livery/texture, .../fairings/texture,
...) which `material` animations with <texture-prop> read; the -set file
seeds a few more of the same kind (the engine-variant N1 / N2 gauge faces).
ProvPilot's FGAnimator swaps the albedo texture of the animation's objects
when such a property changes; the file name is resolved below the aircraft's
texture root (aircraft.cfg [model] textures_dir, default model/liveries) in
the directory of the animation's own model XML relative to Models/, exactly
as FlightGear resolves it relative to that XML.

This script copies the referenced images into that layout and writes one
catalog per variant, model/liveries/<variant>.json:

  {"variant": "MD-82", "defaults": {prop: file}, "liveries": [{"id", "name", "textures": {prop: path}}]}

Run (plain python, PIL not needed):
  python3 scripts/fg_liveries.py --src aircraft/MD-80 --out provpilot/aircraft/md80/model/liveries \
      --variant MD-81 --variant MD-82 --variant MD-83 --variant MD-87
then rsync the folder to the other aircraft copies (docs/AIRCRAFT_CONFIG.md).
"""
import argparse
import glob
import json
import os
import shutil
import sys
import xml.etree.ElementTree as ET


def texture_props(root_el, base=""):
    """All <texture> leaves below sim/model as {"sim/model/<x>/texture": value}."""
    out = {}
    sim = root_el.find("sim")
    model = sim.find("model") if sim is not None else None
    if model is None:
        return out
    for group in model:
        tex = group.find("texture")
        if tex is not None and tex.text and tex.text.strip():
            out["sim/model/%s/texture" % group.tag] = tex.text.strip()
    return out


def livery_name(root_el, fallback):
    n = root_el.find("sim/model/livery/name")
    return n.text.strip() if n is not None and n.text else fallback


def find_in_models(models_dir, name):
    """A bare file name: the file of that name below Models/ (the gauge faces live in Models/Instruments)."""
    hits = glob.glob(os.path.join(models_dir, "**", name), recursive=True)
    hits = [h for h in hits if os.path.isfile(h)]
    return hits[0] if hits else None


def copy_rel(models_dir, out_dir, rel, copied):
    src = os.path.join(models_dir, rel)
    if not os.path.isfile(src):
        return False
    dst = os.path.join(out_dir, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if dst not in copied:
        shutil.copyfile(src, dst)
        copied.add(dst)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="the FlightGear aircraft package")
    ap.add_argument("--out", required=True, help="the texture root to write (model/liveries)")
    ap.add_argument("--variant", action="append", required=True, help="Liveries/<variant> folder and <variant>-set.xml (repeatable)")
    args = ap.parse_args()
    models_dir = os.path.join(args.src, "Models")
    os.makedirs(args.out, exist_ok=True)
    copied = set()
    missing = []
    for variant in args.variant:
        catalog = {"variant": variant, "source": "FlightGear %s, Models/Liveries/%s" % (os.path.basename(os.path.abspath(args.src)), variant), "defaults": {}, "liveries": []}
        set_xml = os.path.join(args.src, variant + "-set.xml")
        if os.path.isfile(set_xml):
            for prop, value in texture_props(ET.parse(set_xml).getroot()).items():
                catalog["defaults"][prop] = value
                src = find_in_models(models_dir, value) if "/" not in value else os.path.join(models_dir, value)
                if src and os.path.isfile(src):
                    rel = os.path.relpath(src, models_dir)
                    copy_rel(models_dir, args.out, rel, copied)
                else:
                    missing.append("%s: %s" % (variant, value))
        liv_dir = os.path.join(models_dir, "Liveries", variant)
        for xml in sorted(glob.glob(os.path.join(liv_dir, "*.xml"))):
            lid = os.path.splitext(os.path.basename(xml))[0]
            try:
                root_el = ET.parse(xml).getroot()
            except ET.ParseError as e:
                print("fg_liveries: cannot parse", xml, e, file=sys.stderr)
                continue
            textures = texture_props(root_el)
            if not textures:
                continue
            ok = True
            for prop, rel in textures.items():
                if not copy_rel(models_dir, args.out, rel, copied):
                    missing.append("%s/%s: %s" % (variant, lid, rel))
                    ok = False
            if ok:
                catalog["liveries"].append({"id": lid, "name": livery_name(root_el, lid), "textures": textures})
        catalog["liveries"].sort(key=lambda l: l["name"].lower())
        with open(os.path.join(args.out, variant + ".json"), "w") as f:
            json.dump(catalog, f, indent=1)
        print("fg_liveries: %s: %d liveries, %d default textures" % (variant, len(catalog["liveries"]), len(catalog["defaults"])))
    # The PNGs are read as files at runtime (Image.load_from_file), never as
    # imported textures: a keep-mode .import per image tells the editor to leave
    # them alone and the exporter to pack them as they are (a .gdignore would hide
    # the folder from the export altogether). The editor fills in the uid.
    res_prefix = "res://" + os.path.relpath(args.out, os.path.join(os.path.dirname(__file__), "..", "provpilot")).replace(os.sep, "/")
    for png in copied:
        rel = os.path.relpath(png, args.out).replace(os.sep, "/")
        with open(png + ".import", "w") as f:
            f.write('[remap]\n\nimporter="keep"\ntype=""\n\n[deps]\n\nsource_file="%s/%s"\ndest_files=[]\n' % (res_prefix, rel))
    gdignore = os.path.join(args.out, ".gdignore")
    if os.path.exists(gdignore):
        os.remove(gdignore)
    total = sum(os.path.getsize(p) for p in copied)
    print("fg_liveries: %d files, %.1f MB -> %s" % (len(copied), total / 1e6, args.out))
    for m in missing:
        print("fg_liveries: missing texture", m)


if __name__ == "__main__":
    main()
