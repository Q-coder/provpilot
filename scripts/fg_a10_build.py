#!/usr/bin/env python3
"""Build the A-10 model for ProvPilot from the FlightGear package
(docs/A10_FLIGHTGEAR_PORT.md, subtask 1).

Runs INSIDE Blender (headless):

    /Applications/Blender.app/Contents/MacOS/Blender -b --python scripts/fg_a10_build.py -- \
        --src /Users/gery/Desktop/A-10 --out provpilot/aircraft/a10/model [--with-pilot] [--blend out.blend]

What it does:
  1. imports the airframe (Models/A10-004-015l3.ac) with the AC3D add-on,
  2. walks Models/A-10-model.xml and every nested <model> (cockpit, instruments,
     stick, pedals, ...) and imports each .ac under an Empty carrying the XML
     <offsets> (FlightGear frame: x aft, y right, z up, metres; heading about
     z, pitch about y, roll about x),
  3. renames every sub-model object "<tag>__<name>" (Godot-safe: no . : @ / " %)
     so instrument parts with the same AC3D name ("needle") stay distinct,
  4. exports a10_fg.glb (Y-up glTF, transforms NOT applied, textures embedded;
     no pilot, no light-effect volumes, no stores),
  5. writes animations.json: every FlightGear <animation> of the tree with its
     type, target node names, the raw XML as nested dicts, and the sub-model
     it belongs to, plus the clickable controls (pick/knob/slider) and the
     sub-model list. Values stay in the FlightGear frame; the runtime converts
     (glTF: x aft, y up, z left).
"""
import argparse
import json
import math
import os
import re
import sys
import xml.etree.ElementTree as ET

import bpy
import mathutils

SKIP_PATHS = ("Stores/loads.xml", "Generic/marker.xml", "Effects/landing-light.xml",
              "Effects/taxi-light.xml", "Trajectory-Marker", "lights-external.xml",
              "lights-internal.xml")
ANIM_TYPES = {"rotate", "translate", "spin", "select", "material", "textranslate", "range",
              "scale", "dist-scale", "billboard", "blend", "noshadow", "shader", "light",
              "pick", "knob", "slider", "timed", "alpha-test", "interaction", "flash"}
CONTROL_TYPES = {"pick", "knob", "slider"}
# ProvPilot-defined controls the FlightGear model lacks (it uses keys): the
# gear handle. Bindings use "provpilot.*" scripts mapped by fg_controls.gd.
EXTRA_CONTROLS = [
    {"model": "A-10-ld-gear-panel", "type": "pick", "objects": ["A-10-ld-gear-panel__ld-gear-handle"],
     "raw": {"type": "pick", "object-name": "ld-gear-handle",
             "action": {"button": "0", "binding": {"command": "nasal", "script": "provpilot.gear_toggle()"}}}},
]
# ProvPilot overrides of FlightGear records (applied to the raw XML dict):
# FlightGear steers the visual nose wheel from the rudder (20 deg max); the
# FDM's real steering angle (up to 60 deg with the A-10's NWS engaged) drives
# it here instead, sign as the rudder-based original (right pedal = -).
OVERRIDES = [
    {"match": {"model": "A-10-model", "type": "rotate", "property": "surface-positions/rudder-pos-norm", "object": "UC-NoseLow"},
     "set": {"property": "gear/gear[0]/steering-deg", "factor": "-1"}},
]
SAFE = re.compile(r'[.:@/"%]')


def godot_safe(name):
    return SAFE.sub("_", name)


def xml_to_dict(e):
    """Lossless-enough XML -> dict: text leaves, repeated tags -> lists."""
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
    if e.attrib:
        d["@"] = dict(e.attrib)
    return d


class Builder:
    def __init__(self, src, with_pilot):
        self.src = src
        self.with_pilot = with_pilot
        # Generic FlightGear models (Aircraft/Instruments-3d/...): the repo's
        # vendored subset first (third_party/fgdata, GPL-2), then a local
        # FlightGear install.
        repo_fgdata = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "third_party", "fgdata"))
        self.fgdata = [repo_fgdata, "/Applications/FlightGear.app/Contents/Resources/data", os.path.expanduser("~/fgdata")]
        self.animations = []
        self.controls = []
        self.submodels = []
        self.missing = []
        self.skipped = []
        self.counts = {}

    # ── paths ──────────────────────────────────────────────────────────
    def resolve(self, p, rel_dir):
        if p.startswith("Aircraft/A-10/"):
            return os.path.join(self.src, p[len("Aircraft/A-10/"):])
        if p.startswith("/") or p.startswith("Aircraft/"):
            for fg in self.fgdata:
                c = os.path.join(fg, p.lstrip("/"))
                if os.path.exists(c):
                    return c
            return None
        return os.path.join(rel_dir, p)

    @staticmethod
    def offsets_of(node):
        d = {"x-m": 0.0, "y-m": 0.0, "z-m": 0.0, "heading-deg": 0.0, "pitch-deg": 0.0, "roll-deg": 0.0}
        o = node.find("offsets") if node is not None else None
        if o is not None:
            for c in o:
                if c.tag in d and c.text:
                    d[c.tag] = float(c.text)
        return d

    # ── scene helpers ──────────────────────────────────────────────────
    @staticmethod
    def coll(name):
        c = bpy.data.collections.get(name)
        if c is None:
            c = bpy.data.collections.new(name)
            bpy.context.scene.collection.children.link(c)
        return c

    def place(self, empty, offs):
        # FlightGear frame == Blender frame here: X aft, Y right, Z up.
        # heading positive = nose right (about -Z), pitch positive = nose up
        # (about +Y), roll positive = right wing down (about -X); Blender XYZ
        # euler = Rz * Ry * Rx, i.e. roll, then pitch, then heading.
        empty.location = (offs["x-m"], offs["y-m"], offs["z-m"])
        empty.rotation_mode = 'XYZ'
        empty.rotation_euler = (math.radians(-offs["roll-deg"]), math.radians(offs["pitch-deg"]),
                                math.radians(-offs["heading-deg"]))

    def import_ac(self, acp, group, prefix):
        before = set(bpy.data.objects.keys())
        bpy.ops.import_scene.import_ac3d(filepath=acp, collection_name=group)
        new = [o for o in bpy.data.objects if o.name not in before]
        # the importer also links into the active collection: keep one link
        for o in new:
            for c in list(o.users_collection):
                if c.name != group:
                    c.objects.unlink(o)
            if group not in [c.name for c in o.users_collection]:
                self.coll(group).objects.link(o)
        # unique, Godot-safe names
        for o in new:
            o.name = godot_safe((prefix + "__" if prefix else "") + o.name)
            if o.data is not None and hasattr(o.data, "name"):
                o.data.name = o.name
        return new

    # ── animations ─────────────────────────────────────────────────────
    def resolve_axis_object(self, rec, prefix, frame_root):
        """FlightGear knobs/sliders may take their axis from a helper object
        (a two-vertex line): axis = v1 -> v2, centre = midpoint, in the
        sub-model's file frame. The glb drops edge-only meshes, so resolve it
        here into the two-point axis form the animator understands."""
        raw = rec["raw"]
        axis = raw.get("axis")
        if not isinstance(axis, dict) or "object-name" not in axis:
            return
        helper = bpy.data.objects.get(godot_safe((prefix + "__" if prefix else "") + axis["object-name"]))
        if helper is None or helper.type != 'MESH' or len(helper.data.vertices) < 2:
            rec["axis_unresolved"] = axis["object-name"]
            return
        inv = frame_root.matrix_world.inverted() if frame_root is not None else mathutils.Matrix.Identity(4)
        pts = [inv @ (helper.matrix_world @ v.co) for v in helper.data.vertices]
        # the two vertices farthest apart define the line
        best = (0.0, pts[0], pts[-1])
        for i in range(len(pts)):
            for j in range(i + 1, len(pts)):
                d = (pts[i] - pts[j]).length
                if d > best[0]:
                    best = (d, pts[i], pts[j])
        p1, p2 = best[1], best[2]
        raw["axis"] = {"x1-m": f"{p1.x:.5f}", "y1-m": f"{p1.y:.5f}", "z1-m": f"{p1.z:.5f}",
                       "x2-m": f"{p2.x:.5f}", "y2-m": f"{p2.y:.5f}", "z2-m": f"{p2.z:.5f}"}
        if "center" not in raw:
            mid = (p1 + p2) * 0.5
            raw["center"] = {"x-m": f"{mid.x:.5f}", "y-m": f"{mid.y:.5f}", "z-m": f"{mid.z:.5f}"}
        rec["axis_from_object"] = axis["object-name"]

    def collect_animations(self, root_elem, tag, prefix, frame_root=None):
        # <object-name> may name another animation of this file (its <name>):
        # FlightGear then nests that animation's transform group inside this
        # one. Expand to the named animation's objects; the referencing
        # animation is emitted first so it stays outermost in the runtime's
        # chain (fgdata's attitude indicator rolls its PitchTranslation).
        named = {}
        for a in root_elem.findall("animation"):
            nm = (a.findtext("name") or "").strip()
            if nm:
                named[nm] = [godot_safe((prefix + "__" if prefix else "") + (o.text or "").strip()) for o in a.findall("object-name")]
        records = []
        for a in root_elem.findall("animation"):
            ty = (a.findtext("type") or "").strip()
            objs = []
            refs = []
            for o in a.findall("object-name"):
                on = (o.text or "").strip()
                if on in named and on != (a.findtext("name") or "").strip():
                    objs.extend(x for x in named[on] if x not in objs)
                    refs.append(on)
                else:
                    objs.append(godot_safe((prefix + "__" if prefix else "") + on))
            rec = {"type": ty or "none", "model": tag, "objects": objs, "raw": xml_to_dict(a)}
            if refs:
                rec["nests"] = refs
            records.append((a, rec))
        # referencing animations before the ones they nest
        order = []
        for a, rec in records:
            pos = len(order)
            for i, (_, other) in enumerate(order):
                if (other["raw"].get("name") or "") in rec.get("nests", []):
                    pos = min(pos, i)
            order.insert(pos, (a, rec))
        for a, rec in order:
            ty = rec["type"] if rec["type"] != "none" else ""
            objs = rec["objects"]
            self.resolve_axis_object(rec, prefix, frame_root)
            for ov in OVERRIDES:
                mt = ov["match"]
                if (mt.get("model", tag) == tag and mt.get("type", ty) == ty and mt.get("property", rec["raw"].get("property")) == rec["raw"].get("property")
                        and (mt.get("object") is None or mt["object"] in objs)):
                    rec["raw"].update(ov["set"])
                    rec["override"] = ov["set"]
            self.counts[ty or "none"] = self.counts.get(ty or "none", 0) + 1
            if ty in CONTROL_TYPES:
                self.controls.append(rec)
            else:
                self.animations.append(rec)

    # ── recursive model import ─────────────────────────────────────────
    def import_model(self, xml_path, offs, group, parent_empty, depth=0):
        if depth > 4:
            return None
        t = ET.parse(xml_path).getroot()
        rel_dir = os.path.dirname(xml_path)
        tag = os.path.splitext(os.path.basename(xml_path))[0]
        prefix = tag if depth > 0 else ""
        ac = (t.findtext("path") or "").strip()
        acp = self.resolve(ac, rel_dir) if ac else None
        e = bpy.data.objects.new(godot_safe("Sub_" + tag) if depth > 0 else "A10_Root", None)
        self.coll(group).objects.link(e)
        e.empty_display_size = 0.1
        self.place(e, offs)
        if parent_empty is not None:
            e.parent = parent_empty
        own = self.offsets_of(t)  # a model file may carry its own <offsets>
        inner = e
        if any(abs(v) > 1e-9 for v in own.values()):
            inner = bpy.data.objects.new(godot_safe("Sub_" + tag + "_own"), None)
            self.coll(group).objects.link(inner)
            self.place(inner, own)
            inner.parent = e
        n_objs = 0
        if acp and os.path.exists(acp):
            new = self.import_ac(acp, group, prefix)
            for o in new:
                if o.parent is None:
                    o.parent = inner
            n_objs = len(new)
        elif ac:
            self.missing.append({"model": tag, "path": ac})
        self.submodels.append({"tag": tag, "node": e.name, "xml": os.path.relpath(xml_path, self.src),
                               "ac": ac, "objects": n_objs, "offsets": offs, "prefix": prefix})
        self.collect_animations(t, tag, prefix, inner)
        for m in t.findall("model"):
            p = (m.findtext("path") or "").strip()
            if any(s in p for s in SKIP_PATHS) or (not self.with_pilot and "pilot" in p):
                self.skipped.append(p)
                continue
            sub = self.resolve(p, rel_dir)
            if sub is None or not os.path.exists(sub):
                self.missing.append({"model": tag, "path": p})
                continue
            child_group = "Cockpit" if "Cockpit/" in p else ("Pilot" if "pilot" in p else group)
            self.import_model(sub, self.offsets_of(m), child_group, e, depth + 1)
        return e

    # ── textures the cockpit references from another folder ───────────
    def fix_textures(self):
        index = {}
        for dp, _, fns in os.walk(os.path.join(self.src, "Models")):
            for fn in fns:
                index.setdefault(fn, os.path.join(dp, fn))
        fixed = []
        for img in bpy.data.images:
            if img.name in ("Render Result", "Viewer Node"):
                continue
            p = bpy.path.abspath(img.filepath) if img.filepath else ""
            if p and not os.path.exists(p):
                alt = index.get(os.path.basename(p))
                if alt:
                    img.filepath = alt
                    img.reload()
                    fixed.append(os.path.basename(p))
        return fixed

    # ── run ─────────────────────────────────────────────────────────────
    def build(self, out_dir, blend_path):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        # the factory reset drops the user's add-ons; the AC3D importer is one
        import addon_utils
        for mod in ("io_scene_ac3d", "bl_ext.user_default.io_scene_ac3d"):
            try:
                addon_utils.enable(mod, default_set=False)
            except Exception:
                continue
        if "import_ac3d" not in dir(bpy.ops.import_scene):
            raise SystemExit("AC3D importer add-on (io_scene_ac3d) is not available in this Blender")
        root_xml = os.path.join(self.src, "Models/A-10-model.xml")
        root = self.import_model(root_xml, self.offsets_of(None), "Airframe", None, 0)
        fixed = self.fix_textures()
        bpy.context.view_layer.update()
        # span check on the wingtip nav lights
        def centre(o):
            return sum((o.matrix_world @ mathutils.Vector(c) for c in o.bound_box), mathutils.Vector()) / 8.0
        l = bpy.data.objects.get("L-NavLight")
        r = bpy.data.objects.get("R-NavLight")
        span = (centre(l) - centre(r)).length if l and r else None
        os.makedirs(out_dir, exist_ok=True)
        glb = os.path.join(out_dir, "a10_fg.glb")
        bpy.ops.export_scene.gltf(filepath=glb, export_format='GLB', export_apply=False, export_yup=True,
                                  export_animations=False, export_image_format='AUTO',
                                  export_materials='EXPORT', export_extras=False,
                                  export_lights=False, export_cameras=False)
        for extra in EXTRA_CONTROLS:
            self.controls.append(dict(extra))
        # which targets exist in the exported scene (FlightGear ignores names the
        # AC3D files do not contain; the runtime and probes can be strict)
        present = set(bpy.data.objects.keys())
        unresolved = []
        for rec in self.animations + self.controls:
            miss = [o for o in rec["objects"] if o not in present and not any(k.startswith(o + ".") for k in present)]
            rec["unresolved"] = miss
            unresolved.extend(miss)
        manifest = {
            "unresolved_targets": sorted(set(unresolved)),
            "source": "FlightGear A-10 (github.com/l0k1/A-10, GPL-2.0), built by scripts/fg_a10_build.py",
            "frame": "FlightGear model frame: x aft, y right, z up, metres. glTF/Godot: x aft, y up, z left (z_godot = -y_fg, y_godot = z_fg).",
            "root": root.name,
            "span_m": span,
            "submodels": self.submodels,
            "animations": self.animations,
            "controls": self.controls,
            "counts": self.counts,
            "missing": self.missing,
            "skipped": self.skipped,
            "textures_relinked": fixed,
        }
        with open(os.path.join(out_dir, "animations.json"), "w") as f:
            json.dump(manifest, f, indent=1)
        if blend_path:
            bpy.ops.wm.save_as_mainfile(filepath=blend_path)
        print("fg_a10_build: %d objects, span %.2f m, %d animations, %d controls, %d sub-models, missing %d -> %s (%.1f MB)" % (
            len(bpy.data.objects), span or 0.0, len(self.animations), len(self.controls), len(self.submodels),
            len(self.missing), glb, os.path.getsize(glb) / 1e6))


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--with-pilot", action="store_true")
    ap.add_argument("--blend", default="")
    args = ap.parse_args(argv)
    Builder(args.src, args.with_pilot).build(args.out, args.blend)


if __name__ == "__main__":
    main()
