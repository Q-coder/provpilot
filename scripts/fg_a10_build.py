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
import copy
import os
import re
import shutil
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


def godot_node_name(blender_name):
    """The name Godot's glTF import gives a Blender object: the duplicate
    suffix ".001" arrives as "_001" (everything else of ours is already safe)."""
    return re.sub(r"\.(\d{3})$", r"_\1", blender_name)


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


def resolve_aliases(root):
    """FlightGear <params> aliases: an element with alias="../../params/x"
    takes the text (and children) of the file's <params><x> entry. fgdata's
    generic radios and the VOR gauge parameterise their property paths this
    way; resolving them here keeps xml_to_dict and the runtime alias-free.
    Returns the number of aliases resolved."""
    params = root.find("params")
    n = 0
    for el in root.iter():
        a = el.attrib.pop("alias", None)
        if a is None:
            continue
        name = a.rsplit("params/", 1)[-1] if "params/" in a else None
        tgt = params.find(name) if (params is not None and name) else None
        if tgt is None:
            print("fg_a10_build: unresolved alias", a)
            continue
        el.text = tgt.text
        for c in list(tgt):
            el.append(copy.deepcopy(c))
        n += 1
    return n


def resolve_includes(root, base_dir, depth=0):
    """SimGear PropertyList includes: <x include="file.xml" omit-node="y"/>
    merges the file's children into x (omit-node: into x's parent in x's
    place). The MD-80 models are assembled this way (MD-82.xml includes
    MD-80-main.xml). Includes are relative to the including file."""
    if depth > 6:
        return
    for el in list(root.iter()):
        inc = el.attrib.pop("include", None)
        if inc is None:
            continue
        path = os.path.normpath(os.path.join(base_dir, inc))
        if not os.path.exists(path):
            print("fg_a10_build: include missing", path)
            continue
        sub = ET.parse(path).getroot()
        resolve_includes(sub, os.path.dirname(path), depth + 1)
        omit = el.attrib.pop("omit-node", "").lower() in ("y", "yes", "true", "1")
        if omit:
            parent = None
            for cand in root.iter():
                if el in list(cand):
                    parent = cand
                    break
            if parent is None:
                continue
            idx = list(parent).index(el)
            parent.remove(el)
            for i, c in enumerate(list(sub)):
                parent.insert(idx + i, c)
        else:
            for c in list(sub):
                el.append(c)


def collect_set_aliases(set_xml):
    """Property aliases of a FlightGear -set file: every element with an
    alias="..." attribute defines <its property path> -> <target path>
    (the MD-80 routes its control-surface positions through
    rotors/main/blade[n]/... and sim/multiplay/generic/float[n] this way).
    Paths carry the n="i" indices as [i]; index 0 is left out like the runtime
    does. Returns {path: target}."""
    try:
        root = ET.parse(set_xml).getroot()
    except Exception as e:
        print("fg_a10_build: cannot parse", set_xml, e)
        return {}
    resolve_includes(root, os.path.dirname(set_xml))
    out = {}
    def walk(el, path):
        for c in el:
            n = c.attrib.get("n")
            seg = c.tag + ("[%s]" % n if n not in (None, "0") else "")
            p = path + "/" + seg if path else seg
            if "alias" in c.attrib:
                out[p] = c.attrib["alias"].strip()
            walk(c, p)
    walk(root, "")
    return out


class Builder:
    def __init__(self, src, with_pilot, prefix="Aircraft/A-10/", root_xml="Models/A-10-model.xml", glb_name="a10_fg.glb",
                 skip_paths=None, source_note="FlightGear A-10 (github.com/l0k1/A-10, GPL-2.0)", set_xml=""):
        self.set_xml = set_xml
        self.src = src
        self.with_pilot = with_pilot
        self.prefix = prefix
        self.root_xml = root_xml
        self.glb_name = glb_name
        self.skip_paths = tuple(skip_paths) if skip_paths else SKIP_PATHS
        self.source_note = source_note
        self.is_a10 = prefix.startswith("Aircraft/A-10")
        # Generic FlightGear models (Aircraft/Instruments-3d/...): the repo's
        # vendored subset first (third_party/fgdata, GPL-2), then a local
        # FlightGear install.
        repo_fgdata = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "third_party", "fgdata"))
        self.fgdata = [repo_fgdata, "/Applications/FlightGear.app/Contents/Resources/data", os.path.expanduser("~/fgdata")]
        self.animations = []
        self.controls = []
        self.texts = []   # FlightGear <text> elements (3D text): an empty per text, the runtime adds the label
        self.fonts = {}   # font file name -> source path (None when not found)
        self.submodels = []
        self.missing = []
        self.skipped = []
        self.counts = {}

    # ── paths ──────────────────────────────────────────────────────────
    def resolve(self, p, rel_dir):
        if p.startswith(self.prefix):
            return os.path.join(self.src, p[len(self.prefix):])
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

    @staticmethod
    def patch_ac3d_crease():
        """Make the AC3D add-on honour each object's crease angle.

        io_scene_ac3d calls Mesh.set_sharp_from_angle(crease) BEFORE
        Mesh.from_pydata(), i.e. on an empty mesh (left over from the
        Blender < 4.1 auto-smooth days), so no edge is ever marked sharp and
        every smooth-shaded face blends its normals across rib caps, panel
        ends and control-surface edges: wing segments shade dark on one side
        and bright on the other. Wrap the object creation and re-apply the
        object's own crease (the .ac "crease" line, OSG's 61 deg default)
        once the geometry exists."""
        # the operator imports its module lazily on first use: load it now
        import importlib
        mod = None
        for pkg in [n for n in sys.modules if n == "io_scene_ac3d" or n.endswith(".io_scene_ac3d")]:
            try:
                mod = importlib.import_module(pkg + ".import_ac3d")
                break
            except Exception:
                continue
        if mod is None or not hasattr(mod, "AcObj"):
            print("WARNING: io_scene_ac3d.import_ac3d not found; crease angles not applied")
            return
        if getattr(mod.AcObj, "_pvp_crease_patched", False):
            return
        orig = mod.AcObj.create_blender_object

        def create_blender_object(ac, *args, **kwargs):
            result = orig(ac, *args, **kwargs)
            o = ac.bl_obj
            if o is not None and o.type == "MESH" and len(o.data.polygons):
                o.data.set_sharp_from_angle(angle=math.radians(float(ac.crease)))
            return result

        mod.AcObj.create_blender_object = create_blender_object
        mod.AcObj._pvp_crease_patched = True

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
        # unique, Godot-safe names; remember the .ac object name -> the name
        # this import's object ends up with (a second import of the same file
        # gets Blender's ".001", Godot's "_001": the instance's own part)
        self.last_import_names = {}
        for o in new:
            orig = o.name
            o.name = godot_safe((prefix + "__" if prefix else "") + o.name)
            if o.data is not None and hasattr(o.data, "name"):
                o.data.name = o.name
            self.last_import_names.setdefault(orig, godot_node_name(o.name))
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

    def collect_animations(self, root_elem, tag, prefix, frame_root=None, sub_by_name=None, local_names=None):
        # <object-name> may name another animation of this file (its <name>):
        # FlightGear then nests that animation's transform group inside this
        # one. Expand to the named animation's objects; the referencing
        # animation is emitted first so it stays outermost in the runtime's
        # chain (fgdata's attitude indicator rolls its PitchTranslation).
        local_names = local_names or {}
        def part(on):
            # the object(s) of THIS import (exact), else the prefixed name (stem match)
            v = local_names.get(on, godot_safe((prefix + "__" if prefix else "") + on))
            return list(v) if isinstance(v, list) else [v]
        named = {}
        for a in root_elem.findall("animation"):
            nm = (a.findtext("name") or "").strip()
            if nm:
                named[nm] = [x for o in a.findall("object-name") for x in part((o.text or "").strip())]
        records = []
        sub_by_name = sub_by_name or {}
        for a in root_elem.findall("animation"):
            ty = (a.findtext("type") or "").strip()
            objs = []
            refs = []
            exact = False
            for o in a.findall("object-name"):
                on = (o.text or "").strip()
                if on in named and on != (a.findtext("name") or "").strip():
                    objs.extend(x for x in named[on] if x not in objs)
                    refs.append(on)
                elif on in sub_by_name:
                    # a <model><name> of this file: FlightGear animates the whole
                    # sub-model (the MD-80 hides its EDP/SDP or the round engine
                    # gauges that way) -> its placement empty, by exact name
                    objs.append(sub_by_name[on])
                    exact = True
                elif on in local_names:
                    # this import's own part: a file used twice (one altimeter per
                    # seat) must not animate the other copy's needle as well; a
                    # name several <text>s share lists every copy
                    v = local_names[on]
                    objs.extend(v if isinstance(v, list) else [v])
                    exact = True
                else:
                    objs.append(godot_safe((prefix + "__" if prefix else "") + on))
            rec = {"type": ty or "none", "model": tag, "objects": objs, "raw": xml_to_dict(a)}
            if exact:
                rec["exact"] = True
            if frame_root is not None:
                # the very sub-model instance this file was imported as: a file
                # used several times (one altimeter per seat) shares its tag,
                # and centres must not resolve in another copy's frame
                rec["frame"] = godot_node_name(frame_root.name)
            if refs:
                rec["nests"] = refs
            records.append((a, rec))
        # <effect><inherits-from>Effects/model-transparent</inherits-from>: FlightGear
        # alpha-blends these objects (instrument shadows, glass, the yaw string)
        for ef in root_elem.findall("effect"):
            inh = (ef.findtext("inherits-from") or "").strip()
            if "transparent" in inh or "glass" in inh:
                objs = [x for o in ef.findall("object-name") for x in part((o.text or "").strip())]
                if objs:
                    rec = {"type": "material", "model": tag, "objects": objs, "raw": {"type": "material", "transparent": True, "effect": inh}}
                    if any(o in local_names.values() or any(isinstance(v, list) and o in v for v in local_names.values()) for o in objs):
                        rec["exact"] = True
                    records.append((ef, rec))
                    self.counts["material"] = self.counts.get("material", 0) + 1
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
            for ov in (OVERRIDES if self.is_a10 else []):
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

    # ── <text>: FlightGear 3D text (osgText) ───────────────────────────
    def font_source(self, font):
        """The aircraft's Fonts/<font>, else fgdata's; remembered for build()."""
        base = os.path.basename(font)
        if base in self.fonts:
            return
        cands = [os.path.join(self.src, "Fonts", font)] + [os.path.join(fg, "Fonts", font) for fg in self.fgdata]
        self.fonts[base] = next((c for c in cands if os.path.exists(c)), None)

    def import_texts(self, t, tag, prefix, group, inner, local_names):
        """A <text> is a value or literal drawn at <offsets> in this file's frame
        (number-value with a printf format, text-value, literal; a font and a
        character size in metres; its <name> is an object other animations may
        select). An empty placed like a sub-model marks the spot; the runtime
        hangs a Label3D on it (FGAnimator, "texts" in the manifest)."""
        names = [(tx.findtext("name") or "").strip() for tx in t.findall("text")]
        for tx in t.findall("text"):
            nm = (tx.findtext("name") or "").strip()
            if not nm:
                continue
            te = bpy.data.objects.new(godot_safe((prefix + "__" if prefix else "") + nm), None)
            self.coll(group).objects.link(te)
            te.empty_display_size = 0.01
            self.place(te, self.offsets_of(tx))
            te.parent = inner
            raw = xml_to_dict(tx)
            font = (tx.findtext("font") or "").strip()
            if font:
                self.font_source(font)
                raw["font"] = os.path.basename(font)
            rec = {"model": tag, "node": godot_node_name(te.name), "frame": godot_node_name(inner.name), "raw": raw}
            prop = raw.get("property") if isinstance(raw.get("property"), str) else ""
            if prop.endswith("-fmt"):
                # Octal450's Nasal keeps "<prop>-fmt" as the knob's working copy
                # and rounds it into <prop>; without that Nasal the base property
                # is the truth (the MD-80 radio heads and FGCP windows)
                rec["property_fg"] = prop
                raw["property"] = prop[:-4]
            self.texts.append(rec)
            # the name resolves exactly to this import's empty; a name several
            # texts share (the MD-80's "Digits.Test", one per window) lists
            # every copy, so one select reaches them all even in an exact record
            if nm not in local_names:
                local_names[nm] = godot_node_name(te.name) if names.count(nm) == 1 else [godot_node_name(te.name)]
            elif isinstance(local_names[nm], list):
                local_names[nm].append(godot_node_name(te.name))
            self.counts["text"] = self.counts.get("text", 0) + 1

    # ── recursive model import ─────────────────────────────────────────
    def import_model(self, xml_path, offs, group, parent_empty, depth=0):
        if depth > 4:
            return None
        t = ET.parse(xml_path).getroot()
        resolve_includes(t, os.path.dirname(xml_path))
        resolve_aliases(t)
        rel_dir = os.path.dirname(xml_path)
        tag = os.path.splitext(os.path.basename(xml_path))[0]
        prefix = tag if depth > 0 else ""
        ac = (t.findtext("path") or "").strip()
        acp = self.resolve(ac, rel_dir) if ac else None
        e = bpy.data.objects.new(godot_safe("Sub_" + tag) if depth > 0 else godot_safe(tag + "_Root"), None)
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
        local_names = {}
        if acp and os.path.exists(acp):
            new = self.import_ac(acp, group, prefix)
            local_names = dict(self.last_import_names)
            for o in new:
                if o.parent is None:
                    o.parent = inner
            n_objs = len(new)
        elif ac:
            self.missing.append({"model": tag, "path": ac})
        # "node" is the frame the file's coordinates live in (animation
        # centres and axes are relative to it): the own-offset empty when the
        # file carries <offsets>, else the placement empty
        self.submodels.append({"tag": tag, "node": godot_node_name(inner.name), "placement": godot_node_name(e.name), "xml": os.path.relpath(xml_path, self.src),
                               "ac": ac, "objects": n_objs, "offsets": offs, "own_offsets": own, "prefix": prefix})
        self.import_texts(t, tag, prefix, group, inner, local_names)
        sub_by_name = {}
        for m in t.findall("model"):
            p = (m.findtext("path") or "").strip()
            cond = m.find("condition")
            sub_name = (m.findtext("name") or "").strip()
            if any(s in p for s in self.skip_paths) or (not self.with_pilot and "pilot" in p.lower()):
                self.skipped.append(p)
                continue
            sub = self.resolve(p, rel_dir)
            if sub is None or not os.path.exists(sub):
                self.missing.append({"model": tag, "path": p})
                continue
            child_group = "Cockpit" if "Cockpit/" in p else ("Pilot" if "pilot" in p else group)
            # a model file's own <offsets> move its mesh AND its nested
            # sub-models (SimGear puts them under the same transform): the
            # MD-80's airframe file shifts everything 22.6 m nose-ward
            parent_for_children = inner
            if sub.lower().endswith(".ac"):
                # a <model> may point straight at a mesh (placards, chocks):
                # place it like a sub-model without an XML of its own
                ac_tag = os.path.splitext(os.path.basename(sub))[0]
                holder = bpy.data.objects.new(godot_safe("Sub_" + ac_tag), None)
                self.coll(child_group).objects.link(holder)
                holder.empty_display_size = 0.1
                self.place(holder, self.offsets_of(m))
                holder.parent = parent_for_children
                new = self.import_ac(sub, child_group, ac_tag)
                for o in new:
                    if o.parent is None:
                        o.parent = holder
                self.submodels.append({"tag": ac_tag, "node": godot_node_name(holder.name), "xml": "", "ac": p, "objects": len(new),
                                       "offsets": self.offsets_of(m), "prefix": ac_tag})
                if cond is not None:
                    self.model_condition(tag, holder.name, cond)
                if sub_name:
                    sub_by_name[sub_name] = godot_node_name(holder.name)
                continue
            child = self.import_model(sub, self.offsets_of(m), child_group, parent_for_children, depth + 1)
            if child is None:
                continue
            if cond is not None:
                self.model_condition(tag, child.name, cond)
            if sub_name:
                sub_by_name[sub_name] = godot_node_name(child.name)
        self.collect_animations(t, tag, prefix, inner, sub_by_name, local_names)
        return e

    def model_condition(self, tag, node_name, cond):
        """<model><condition>: FlightGear shows the sub-model only while the
        condition holds (the MD-80 picks EFIS or analog instruments per
        option). Emitted as a select animation on the placement empty."""
        node_name = godot_node_name(node_name)
        rec = {"type": "select", "model": tag, "objects": [node_name], "exact": True,
               "raw": {"type": "select", "object-name": node_name, "condition": xml_to_dict(cond)}}
        self.counts["select"] = self.counts.get("select", 0) + 1
        self.animations.append(rec)

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
        self.patch_ac3d_crease()
        root_xml = os.path.join(self.src, self.root_xml)
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
        glb = os.path.join(out_dir, self.glb_name)
        bpy.ops.export_scene.gltf(filepath=glb, export_format='GLB', export_apply=False, export_yup=True,
                                  export_animations=False, export_image_format='AUTO',
                                  export_materials='EXPORT', export_extras=False,
                                  export_lights=False, export_cameras=False)
        for extra in (EXTRA_CONTROLS if self.is_a10 else []):
            self.controls.append(dict(extra))
        # which targets exist in the exported scene (FlightGear ignores names the
        # AC3D files do not contain; the runtime and probes can be strict)
        present = set(bpy.data.objects.keys())
        unresolved = []
        for rec in self.animations + self.controls:
            miss = [o for o in rec["objects"] if o not in present and not any(k.startswith(o + ".") for k in present)]
            rec["unresolved"] = miss
            unresolved.extend(miss)
        fonts = {}
        for base, src in self.fonts.items():
            if src is None:
                print("fg_a10_build: font not found:", base)
                continue
            os.makedirs(os.path.join(out_dir, "fonts"), exist_ok=True)
            shutil.copyfile(src, os.path.join(out_dir, "fonts", base))
            fonts[base] = "fonts/" + base
        manifest = {
            "unresolved_targets": sorted(set(unresolved)),
            "source": self.source_note + ", built by scripts/fg_a10_build.py",
            "frame": "FlightGear model frame: x aft, y right, z up, metres. glTF/Godot: x aft, y up, z left (z_godot = -y_fg, y_godot = z_fg).",
            "root": root.name,
            "span_m": span,
            "submodels": self.submodels,
            "animations": self.animations,
            "controls": self.controls,
            # FlightGear <text> elements: {model, node (the empty), frame, raw{type, format, property, text, font, character-size, alignment, axis-alignment}}
            "texts": self.texts,
            "fonts": fonts,
            "counts": self.counts,
            "missing": self.missing,
            "skipped": self.skipped,
            "textures_relinked": fixed,
            # property aliases of the -set file (property -> target), applied by the bridge
            "aliases": collect_set_aliases(self.set_xml) if self.set_xml else {},
        }
        with open(os.path.join(out_dir, "animations.json"), "w") as f:
            json.dump(manifest, f, indent=1)
        if blend_path:
            bpy.ops.wm.save_as_mainfile(filepath=blend_path)
        print("fg_a10_build: %d objects, span %.2f m, %d animations, %d controls, %d texts, %d sub-models, missing %d -> %s (%.1f MB)" % (
            len(bpy.data.objects), span or 0.0, len(self.animations), len(self.controls), len(self.texts), len(self.submodels),
            len(self.missing), glb, os.path.getsize(glb) / 1e6))


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--with-pilot", action="store_true")
    ap.add_argument("--blend", default="")
    ap.add_argument("--prefix", default="Aircraft/A-10/", help="FlightGear path prefix of the package (Aircraft/<name>/)")
    ap.add_argument("--root", default="Models/A-10-model.xml", help="root model XML relative to --src")
    ap.add_argument("--glb-name", default="a10_fg.glb")
    ap.add_argument("--skip", action="append", default=None, help="sub-model path fragments to skip (repeatable); default: the A-10 list")
    ap.add_argument("--source-note", default="FlightGear A-10 (github.com/l0k1/A-10, GPL-2.0)")
    ap.add_argument("--set-xml", default="", help="the aircraft's -set / main XML: its property aliases go into the manifest")
    args = ap.parse_args(argv)
    Builder(args.src, args.with_pilot, prefix=args.prefix, root_xml=args.root, glb_name=args.glb_name,
            skip_paths=args.skip, source_note=args.source_note, set_xml=args.set_xml).build(args.out, args.blend)


if __name__ == "__main__":
    main()
