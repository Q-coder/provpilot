#!/usr/bin/env python3
"""Add FlightGear instruments, light lenses and a steering nose gear to a
Blender aircraft model and export it for ProvPilot (docs/INSTRUMENTS.md).

Run headless:
  /Applications/Blender.app/Contents/MacOS/Blender -b --python scripts/fg_panel_build.py -- \
      --blend provpilot/aircraft/c172p/model/C172P_1.blend \
      --panel provpilot/aircraft/c172p/model/panel.xml \
      --out provpilot/aircraft/c172p/model/C172P_1.glb [--save-blend out.blend]

The blend is the source of truth and is not modified; the glb and the
animations.json manifest next to it are the outputs. The panel manifest is a
FlightGear PropertyList of <model> entries (path + <offsets>, optional
<scale>) whose paths resolve in third_party/fgdata like the A-10 builder's.
The C172-specific edits (nose gear pivot, tail light lens) are keyed on the
Sketchfab object names and skipped when they are absent.
"""
import argparse
import json
import math
import os
import sys

import bpy
import mathutils
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fg_a10_build import Builder, godot_safe  # noqa: E402

# Blender frame of the Sketchfab C172: +X left, +Y aft, +Z up.
# FlightGear frame: x aft, y right, z up -> rotate +90 deg about Z.
PANEL_ROOT = "Panel_Root"
FG_TO_BLENDER_Z_DEG = 90.0

# C172 (Sketchfab) specifics
NOSE_GEAR_GROUPS = ["Cessna_Exterior_Body_MAT_0.019", "Cessna_Exterior_Body_MAT_0.021"]  # strut, wheel + fairing
NOSE_STEER_POINT = (0.0, -1.63, 0.50)     # a point on the vertical steering axis (Blender metres), through the tyre
RUDDER_MESH = "Cessna_Exterior_Body_MAT_0.008_Rudder"
TAIL_LIGHT_POS = (0.0, 6.60, 2.78)        # top corner of the rudder's trailing edge (trailing edge y 6.61 at z 2.72..2.81)
# yokes: the shaft's base cone (the last 3.5 cm before the panel) becomes its own
# clickable object; the yoke keeps the rest and gets a plain name
YOKES = [  # (source mesh, yoke name, cone name)
    ("Cessna_Interior_Steering_MAT_0_Steering_MAT_0", "Yoke_L", "Yoke_L_Cone"),
    ("Cessna_Interior_Steering_MAT_0.001_Steering_MAT_0", "Yoke_R", "Yoke_R_Cone"),
]
YOKE_CONE_Y = -0.752      # faces entirely forward of this (toward the panel) are the cone
TAIL_LIGHT_POS = TAIL_LIGHT_POS[:2] + (2.72,)   # gery 2026-09-22: a few cm lower still
# rocker switches: faces of the panel base standing proud of the plate inside
# a box (Blender metres); pivot at the box centre, they tilt about X.
# Row 1 under the glareshield: TAXI / LDG / PULSE; row 2: NAV / BCN / STROBE.
PANEL_BASE = "Static_Base_New_Mat_0_New_Mat_0"
ROCKER_HALF_X = 0.0085
ROCKER_PLATE_Y = -0.7835      # faces entirely aft of this (toward the pilot) are the rocker, not the plate
ROCKERS = [  # (name, x, z, half height)
    ("Switch_TaxiLt", 0.104, 1.446, 0.024),
    ("Switch_LandingLt", 0.088, 1.446, 0.024),
    ("Switch_NavLt", 0.104, 1.404, 0.022),
    ("Switch_BeaconLt", 0.088, 1.404, 0.022),
    ("Switch_StrobeLt", 0.072, 1.404, 0.022),
]
# the red split master rocker (Dynamic_Switch_001): left half ALT (larger x), right half BAT
MASTER_MESH = "Dynamic_Switch_001_New_Mat_0_New_Mat_0"
MASTER_SPLIT_X = 0.453
MASTER_PIVOT = (-0.770, 1.441)   # y, z of the rocker pivot line
SPINNER_GROUP = "Cessna_Exterior_Body_MAT_0.024"   # the spinner cone (already its own object in the blend)
SPINNER_AXIS_POINT = (0.0, -2.60, 1.5525)        # on the propeller axis (spinner bounds centre)
EXTERIOR_PARENT = "Cessna_Exterior"
# tyres to split out of the Sketchfab wheel objects (tyre + sidewalls + hub caps
# are loose parts centred on the axle; the fairing halves and brackets are not)
TYRES = [  # (source mesh object, new tyre object, parent object or None = NoseGear_Steer)
    ("Cessna_Exterior_Body_MAT_0.021_Body_Front_wheel", "NoseWheel_Tyre", None),
    ("Cessna_Exterior_Body_MAT_0.022_Body_MAT_0", "MainWheel_L_Tyre", EXTERIOR_PARENT),
    ("Cessna_Exterior_Body_MAT_0.023_Body_MAT_0", "MainWheel_R_Tyre", EXTERIOR_PARENT),
]


class PanelBuilder(Builder):
    def __init__(self, model_dir):
        super().__init__(model_dir, False)


def enable_ac3d():
    import addon_utils
    for mod in ("io_scene_ac3d", "bl_ext.user_default.io_scene_ac3d"):
        try:
            addon_utils.enable(mod, default_set=False)
        except Exception:
            continue
    if "import_ac3d" not in dir(bpy.ops.import_scene):
        raise SystemExit("AC3D importer add-on (io_scene_ac3d) is not available in this Blender")


def link_scene(o):
    bpy.context.scene.collection.objects.link(o)


def reparent_keep_world(o, new_parent):
    bpy.context.view_layer.update()   # matrix_world of freshly moved objects is stale until then
    old = o.matrix_world.copy()
    o.parent = new_parent
    o.matrix_parent_inverse = mathutils.Matrix.Identity(4)
    bpy.context.view_layer.update()
    o.matrix_world = old
    bpy.context.view_layer.update()


def build_panel(builder, panel_xml):
    root = bpy.data.objects.new(PANEL_ROOT, None)
    link_scene(root)
    root.empty_display_size = 0.05
    root.rotation_mode = 'XYZ'
    root.rotation_euler = (0.0, 0.0, math.radians(FG_TO_BLENDER_Z_DEG))
    t = ET.parse(panel_xml).getroot()
    rel_dir = os.path.dirname(panel_xml)
    n = 0
    for m in t.findall("model"):
        p = (m.findtext("path") or "").strip()
        xml = builder.resolve(p, rel_dir)
        if xml is None or not os.path.exists(xml):
            builder.missing.append({"model": "panel", "path": p})
            print("fg_panel_build: missing", p)
            continue
        e = builder.import_model(xml, builder.offsets_of(m), "Panel", root, 1)
        s = float((m.findtext("scale") or "1").strip())
        if e is not None and abs(s - 1.0) > 1e-6:
            e.scale = (s, s, s)
        n += 1
    # blanking plates: flat black discs facing forward (-x in the FlightGear
    # frame), placed like a sub-model
    nb = 0
    mat = bpy.data.materials.new("PanelBlank")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    if bsdf:
        bsdf.inputs["Base Color"].default_value = (0.02, 0.02, 0.02, 1.0)
        bsdf.inputs["Roughness"].default_value = 0.9
    for bl in t.findall("blank"):
        offs = builder.offsets_of(bl)
        r = float((bl.findtext("radius-m") or "0.04").strip())
        bpy.ops.mesh.primitive_circle_add(vertices=32, radius=r, fill_type='NGON', location=(0, 0, 0))
        disc = bpy.context.active_object
        disc.name = "PanelBlank_%02d" % nb
        disc.data.name = disc.name
        disc.data.materials.append(mat)
        # the circle lies in the local XY plane; turn it into the YZ plane (facing -x)
        disc.rotation_mode = 'XYZ'
        disc.rotation_euler = (0.0, math.radians(90.0), 0.0)
        holder = bpy.data.objects.new("Sub_blank_%02d" % nb, None)
        link_scene(holder)
        builder.place(holder, offs)
        holder.parent = root
        disc.parent = holder
        for c in list(disc.users_collection):
            c.objects.unlink(disc)
        builder.coll("Panel").objects.link(disc)
        nb += 1
    print("fg_panel_build: %d blanking plates" % nb)
    return root, n


def fix_alpha_materials(prefix_objs):
    """FlightGear draws a textured surface with its texture's alpha (the
    heading indicator's front glass, the turn coordinator's ball tube). The
    AC3D importer leaves the alpha unconnected, so the glTF export is opaque.
    Link it for every image that has transparent pixels."""
    done = set()
    n = 0
    for o in prefix_objs:
        if o.type != 'MESH':
            continue
        for mat in o.data.materials:
            if mat is None or mat.name in done or not mat.use_nodes:
                continue
            done.add(mat.name)
            nodes = mat.node_tree.nodes
            tex = next((nd for nd in nodes if nd.type == 'TEX_IMAGE' and nd.image is not None), None)
            bsdf = next((nd for nd in nodes if nd.type == 'BSDF_PRINCIPLED'), None)
            if tex is None or bsdf is None:
                continue
            img = tex.image
            if img.channels < 4:
                continue
            px = img.pixels[:]
            if min(px[3::4]) >= 0.999:
                continue
            mat.node_tree.links.new(tex.outputs['Alpha'], bsdf.inputs['Alpha'])
            if hasattr(mat, 'surface_render_method'):
                mat.surface_render_method = 'BLENDED'
            else:
                mat.blend_method = 'BLEND'
            mat.use_backface_culling = False
            n += 1
    print("fg_panel_build: %d textured materials use their alpha" % n)


def nose_gear_pivot():
    ext = bpy.data.objects.get(EXTERIOR_PARENT)
    groups = [bpy.data.objects.get(g) for g in NOSE_GEAR_GROUPS]
    if ext is None or any(g is None for g in groups):
        print("fg_panel_build: nose gear objects not found, steering pivot skipped")
        return None
    piv = bpy.data.objects.new("NoseGear_Steer", None)
    link_scene(piv)
    piv.empty_display_size = 0.1
    piv.location = NOSE_STEER_POINT
    reparent_keep_world(piv, ext)
    for g in groups:
        reparent_keep_world(g, piv)
    bpy.context.view_layer.update()
    print("fg_panel_build: nose gear pivot at %s (world), groups at %s" % (
        tuple(round(v, 3) for v in piv.matrix_world.translation),
        [tuple(round(v, 3) for v in g.matrix_world.translation) for g in groups]))
    return piv


def _islands(o):
    import bmesh
    bm = bmesh.new()
    bm.from_mesh(o.data)
    bm.verts.ensure_lookup_table()
    seen = set()
    out = []
    mw = o.matrix_world
    for v in bm.verts:
        if v.index in seen:
            continue
        stack = [v]
        comp = []
        while stack:
            x = stack.pop()
            if x.index in seen:
                continue
            seen.add(x.index)
            comp.append(x)
            for e in x.link_edges:
                for y in e.verts:
                    if y.index not in seen:
                        stack.append(y)
        pts = [mw @ x.co for x in comp]
        mn = mathutils.Vector([min(p[i] for p in pts) for i in range(3)])
        mx = mathutils.Vector([max(p[i] for p in pts) for i in range(3)])
        out.append({"verts": comp, "centre": (mn + mx) * 0.5})
    return bm, out


def _extract_faces(o, bm, faces, name, origin, parent, remove=True):
    """New object from [faces] of o (world coords relative to origin), parented
    under [parent] with its origin at [origin]; the faces leave o when remove."""
    import bmesh
    mw = o.matrix_world
    nb = bmesh.new()
    layer_uv = bm.loops.layers.uv.active
    nuv = nb.loops.layers.uv.new(layer_uv.name if layer_uv else "UVMap")
    vmap = {}
    for f in faces:
        nvs = []
        for v in f.verts:
            if v.index not in vmap:
                vmap[v.index] = nb.verts.new((mw @ v.co) - origin)
            nvs.append(vmap[v.index])
        try:
            nf = nb.faces.new(nvs)
        except ValueError:
            continue
        nf.material_index = f.material_index
        nf.smooth = f.smooth
        if layer_uv:
            for l_old, l_new in zip(f.loops, nf.loops):
                l_new[nuv].uv = l_old[layer_uv].uv
    me = bpy.data.meshes.new(name)
    nb.to_mesh(me)
    nb.free()
    for m in o.data.materials:
        me.materials.append(m)
    new = bpy.data.objects.new(name, me)
    link_scene(new)
    new.location = origin
    bpy.context.view_layer.update()
    if parent is not None:
        reparent_keep_world(new, parent)
    if remove:
        bmesh.ops.delete(bm, geom=list(faces), context='FACES')
    return new


def split_yokes():
    made = []
    for src_name, yoke_name, cone_name in YOKES:
        o = bpy.data.objects.get(src_name)
        if o is None:
            print("fg_panel_build: %s not found, yoke skipped" % src_name)
            continue
        bpy.context.view_layer.update()
        bm, _ = _islands(o)
        bm.faces.ensure_lookup_table()
        mw = o.matrix_world
        cone_faces = [f for f in bm.faces if all((mw @ v.co).y < YOKE_CONE_Y for v in f.verts)]
        pts = [mw @ v.co for f in cone_faces for v in f.verts]
        origin = mathutils.Vector((sum(p.x for p in pts) / len(pts), max(p.y for p in pts), sum(p.z for p in pts) / len(pts)))
        cone = _extract_faces(o, bm, cone_faces, cone_name, origin, o.parent)
        bm.to_mesh(o.data)
        bm.free()
        o.data.update()
        o.name = yoke_name
        o.data.name = yoke_name
        made.append({"yoke": yoke_name, "cone": cone_name, "cone_faces": len(cone_faces), "cone_origin": [round(v, 3) for v in origin]})
        print("fg_panel_build: %s split off %s (%d faces at %s)" % (yoke_name, cone_name, len(cone_faces), tuple(round(v, 3) for v in origin)))
    return made


# rockers split by hand in Blender (pivot at the switch centre): blend name -> manifest name
HAND_ROCKERS = {
    "Switch_ALT": "Switch_Alt", "SWitch_BAT": "Switch_Bat",
    "Switch_BCN_LT": "Switch_BeaconLt", "Switch_LDG": "Switch_LandingLt", "Switch_STROBE_LT": "Switch_StrobeLt",
    "Switch_Taxi": "Switch_TaxiLt", "Swtich_Nav_LT": "Switch_NavLt",
    "Switch_Pitot": "Switch_PitotHt", "Switch_Pulse": "Switch_LandingPulse",
}


def rocker_switches():
    """Rockers split by hand in the blend (renamed for the manifest), else
    light rockers out of the panel base (face boxes) and the split master
    rocker out of its own mesh, each with its pivot as origin."""
    made = []
    hand = [(src, dst) for src, dst in HAND_ROCKERS.items() if bpy.data.objects.get(src) is not None]
    if hand:
        for src, dst in hand:
            o = bpy.data.objects[src]
            o.name = dst
            if o.data is not None:
                o.data.name = dst
            made.append(dst)
        print("fg_panel_build: %d rockers from the blend: %s" % (len(hand), ", ".join(d for _, d in hand)))
        return made
    base = bpy.data.objects.get(PANEL_BASE)
    if base is not None:
        bpy.context.view_layer.update()
        bm, _ = _islands(base)
        bm.faces.ensure_lookup_table()
        mw = base.matrix_world
        for name, x, z, hz in ROCKERS:
            faces = []
            for f in bm.faces:
                pts = [mw @ v.co for v in f.verts]
                c = sum(pts, mathutils.Vector()) / len(pts)
                # the rocker's faces: centred in the box, and at least one vertex
                # proud of the plate (the side walls reach down to it)
                if abs(c.x - x) <= ROCKER_HALF_X and abs(c.z - z) <= hz and max(p.y for p in pts) > ROCKER_PLATE_Y:
                    faces.append(f)
            if not faces:
                print("fg_panel_build: no rocker faces at x=%.3f z=%.3f" % (x, z))
                continue
            pts = [mw @ v.co for f in faces for v in f.verts]
            origin = mathutils.Vector((x, min(p.y for p in pts), z))
            _extract_faces(base, bm, faces, name, origin, base.parent)
            made.append(name)
            print("fg_panel_build: %s: %d faces, pivot %s" % (name, len(faces), tuple(round(v, 3) for v in origin)))
        bm.to_mesh(base.data)
        bm.free()
        base.data.update()
    master = bpy.data.objects.get(MASTER_MESH)
    if master is not None:
        bpy.context.view_layer.update()
        bm, _ = _islands(master)
        bm.faces.ensure_lookup_table()
        mw = master.matrix_world
        for name, pick in (("Switch_Alt", lambda p: p.x > MASTER_SPLIT_X), ("Switch_Bat", lambda p: p.x <= MASTER_SPLIT_X)):
            faces = [f for f in bm.faces if all(pick(mw @ v.co) for v in f.verts)]
            if not faces:
                continue
            xs = [(mw @ v.co).x for f in faces for v in f.verts]
            origin = mathutils.Vector((sum(xs) / len(xs), MASTER_PIVOT[0], MASTER_PIVOT[1]))
            _extract_faces(master, bm, faces, name, origin, master.parent, remove=False)
            made.append(name)
            print("fg_panel_build: %s: %d faces, pivot %s" % (name, len(faces), tuple(round(v, 3) for v in origin)))
        bm.free()
        # the halves replace the original mesh
        bpy.data.objects.remove(master)
    return made


def spinner_pivot():
    """Empty on the propeller axis carrying the spinner cone, so the cone can
    spin about its own axis (the Sketchfab object's origin is the model centre)."""
    ext = bpy.data.objects.get(EXTERIOR_PARENT)
    grp = bpy.data.objects.get(SPINNER_GROUP)
    if ext is None or grp is None:
        print("fg_panel_build: spinner objects not found, spinner pivot skipped")
        return None
    piv = bpy.data.objects.new("Spinner_Pivot", None)
    link_scene(piv)
    piv.empty_display_size = 0.1
    piv.location = SPINNER_AXIS_POINT
    reparent_keep_world(piv, ext)
    reparent_keep_world(grp, piv)
    bpy.context.view_layer.update()
    print("fg_panel_build: spinner pivot at %s (world)" % (tuple(round(v, 3) for v in piv.matrix_world.translation),))
    return piv


def split_tyres(nose_pivot):
    """Separate each tyre (with sidewalls and hub caps) from its wheel object
    into a new object whose origin is the axle centre, so it can spin about
    its local X. The tyre body is the loose part with the largest height
    among the narrow parts; every loose part centred on its axle within 3 cm
    and taller than 10 cm belongs to the tyre."""
    import bmesh
    made = []
    for src_name, tyre_name, parent_name in TYRES:
        o = bpy.data.objects.get(src_name)
        if o is None:
            print("fg_panel_build: %s not found, tyre skipped" % src_name)
            continue
        bpy.context.view_layer.update()
        mw = o.matrix_world.copy()
        bm = bmesh.new()
        bm.from_mesh(o.data)
        bm.verts.ensure_lookup_table()
        bm.faces.ensure_lookup_table()
        seen = set()
        islands = []
        for v in bm.verts:
            if v.index in seen:
                continue
            stack = [v]
            comp = []
            while stack:
                x = stack.pop()
                if x.index in seen:
                    continue
                seen.add(x.index)
                comp.append(x)
                for e in x.link_edges:
                    for y in e.verts:
                        if y.index not in seen:
                            stack.append(y)
            pts = [mw @ x.co for x in comp]
            mn = mathutils.Vector([min(p[i] for p in pts) for i in range(3)])
            mx = mathutils.Vector([max(p[i] for p in pts) for i in range(3)])
            islands.append({"verts": comp, "min": mn, "max": mx, "centre": (mn + mx) * 0.5, "w": mx.x - mn.x, "h": mx.z - mn.z})
        body = max((i for i in islands if i["w"] < 0.2), key=lambda i: i["h"], default=None)
        if body is None:
            print("fg_panel_build: no tyre body in %s" % src_name)
            continue
        axle = body["centre"]
        radius = body["h"] * 0.5
        # sidewalls and hub caps sit within a few cm of the tyre body along the
        # axle; the strut brackets further inboard do not
        tyre_islands = [i for i in islands if (i["centre"] - axle).yz.length < 0.03 and i["h"] > 0.10
                        and i["min"].x > body["min"].x - 0.04 and i["max"].x < body["max"].x + 0.04]
        tyre_faces = {f for i in tyre_islands for x in i["verts"] for f in x.link_faces}
        # new mesh from the tyre faces, in axle-centred world coordinates
        nb = bmesh.new()
        layer_uv = bm.loops.layers.uv.active
        nuv = nb.loops.layers.uv.new(layer_uv.name if layer_uv else "UVMap")
        vmap = {}
        for f in tyre_faces:
            nvs = []
            for v in f.verts:
                if v.index not in vmap:
                    vmap[v.index] = nb.verts.new((mw @ v.co) - axle)
                nvs.append(vmap[v.index])
            try:
                nf = nb.faces.new(nvs)
            except ValueError:
                continue
            nf.material_index = f.material_index
            nf.smooth = f.smooth
            if layer_uv:
                for l_old, l_new in zip(f.loops, nf.loops):
                    l_new[nuv].uv = l_old[layer_uv].uv
        me = bpy.data.meshes.new(tyre_name)
        nb.to_mesh(me)
        nb.free()
        for m in o.data.materials:
            me.materials.append(m)
        tyre = bpy.data.objects.new(tyre_name, me)
        link_scene(tyre)
        tyre.location = axle
        bpy.context.view_layer.update()
        parent = nose_pivot if parent_name is None else bpy.data.objects.get(parent_name)
        if parent is not None:
            reparent_keep_world(tyre, parent)
        # drop the tyre faces from the source object
        bmesh.ops.delete(bm, geom=list(tyre_faces), context='FACES')
        bm.to_mesh(o.data)
        bm.free()
        o.data.update()
        made.append({"tyre": tyre_name, "axle": [round(v, 3) for v in axle], "radius_m": round(radius, 3), "faces": len(tyre_faces), "parts": len(tyre_islands)})
        print("fg_panel_build: %s from %s: axle %s, radius %.3f m, %d faces in %d parts" % (tyre_name, src_name, tuple(round(v, 3) for v in axle), radius, len(tyre_faces), len(tyre_islands)))
    return made


def tail_light_lens():
    rudder = bpy.data.objects.get(RUDDER_MESH)
    if rudder is None:
        print("fg_panel_build: rudder mesh not found, tail light lens skipped")
        return None
    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.02, segments=12, ring_count=8, location=TAIL_LIGHT_POS)
    lens = bpy.context.active_object
    lens.name = "TailLight_lens"
    lens.data.name = "TailLight_lens"
    mat = bpy.data.materials.new("TailLight_lens")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    if bsdf:
        bsdf.inputs["Base Color"].default_value = (1.0, 1.0, 1.0, 1.0)
        bsdf.inputs["Emission Color"].default_value = (1.0, 1.0, 1.0, 1.0)
        bsdf.inputs["Emission Strength"].default_value = 2.0
    lens.data.materials.append(mat)
    reparent_keep_world(lens, rudder)
    return lens


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--blend", required=True)
    ap.add_argument("--panel", required=True)
    ap.add_argument("--out", required=True, help="output .glb; animations.json is written next to it")
    ap.add_argument("--save-blend", default="")
    args = ap.parse_args(argv)

    bpy.ops.wm.open_mainfile(filepath=os.path.abspath(args.blend))
    enable_ac3d()
    model_dir = os.path.dirname(os.path.abspath(args.out))
    b = PanelBuilder(model_dir)
    root, n_instr = build_panel(b, os.path.abspath(args.panel))
    fix_alpha_materials([o for o in bpy.data.objects if "__" in o.name])
    yokes = split_yokes()
    switches = rocker_switches()
    # the manifest's own <animation> records animate model parts by their
    # Godot names (yokes, switches); their axes / centres are Blender
    # coordinates of this model (the runtime maps x,y,z -> x,z,-y into glTF)
    b.collect_animations(ET.parse(os.path.abspath(args.panel)).getroot(), "panel", "", None)
    # a manifest-level rotate without <center> turns about the part's own
    # origin (the pivot set in Blender), not the model origin
    for rec in b.animations:
        if rec["model"] == "panel" and rec["type"] == "rotate" and "center" not in rec["raw"] and len(rec["objects"]) == 1:
            o = bpy.data.objects.get(rec["objects"][0])
            if o is not None:
                bpy.context.view_layer.update()
                t = o.matrix_world.translation
                rec["raw"]["center"] = {"x-m": "%.5f" % t.x, "y-m": "%.5f" % t.y, "z-m": "%.5f" % t.z}
                rec["center_from_origin"] = True
    piv = nose_gear_pivot()
    tyres = split_tyres(piv)
    spin = spinner_pivot()
    lens = tail_light_lens()
    bpy.context.view_layer.update()

    out = os.path.abspath(args.out)
    bpy.ops.export_scene.gltf(filepath=out, export_format='GLB', export_apply=False, export_yup=True,
                              export_animations=False, export_image_format='AUTO',
                              export_materials='EXPORT', export_extras=False,
                              export_lights=False, export_cameras=False)
    present = set(bpy.data.objects.keys())
    unresolved = []
    for rec in b.animations + b.controls:
        miss = [o for o in rec["objects"] if o not in present and not any(k.startswith(o + ".") for k in present)]
        rec["unresolved"] = miss
        unresolved.extend(miss)
    manifest = {
        "unresolved_targets": sorted(set(unresolved)),
        "source": "ProvPilot panel manifest %s with FlightGear generic instruments (fgdata, GPL-2), built by scripts/fg_panel_build.py" % os.path.basename(args.panel),
        "frame": "FlightGear model frame under %s (rotated %.0f deg about Z into the model's Blender frame); glTF/Godot: y up." % (PANEL_ROOT, FG_TO_BLENDER_Z_DEG),
        "root": root.name,
        "submodels": b.submodels,
        "animations": b.animations,
        "controls": b.controls,
        "counts": b.counts,
        "missing": b.missing,
        "skipped": b.skipped,
        "extras": {"nose_gear_pivot": piv.name if piv else None, "tail_light_lens": lens.name if lens else None, "tyres": tyres,
                   "spinner_pivot": spin.name if spin else None, "yokes": yokes, "switches": switches},
    }
    with open(os.path.join(model_dir, "animations.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    if args.save_blend:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(args.save_blend))
    print("fg_panel_build: %d instruments, %d animations, %d controls, missing %d, unresolved %d -> %s (%.1f MB)" % (
        n_instr, len(b.animations), len(b.controls), len(b.missing), len(set(unresolved)), out, os.path.getsize(out) / 1e6))


if __name__ == "__main__":
    main()
