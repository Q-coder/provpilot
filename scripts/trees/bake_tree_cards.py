"""Render the tree card atlas for S3DVegetationManager's card mode
(docs/WORLD_SCENERY_DESIGN.md §14).

Run headless:
  /Applications/Blender.app/Contents/MacOS/Blender -b -P scripts/trees/bake_tree_cards.py -- scripts/trees/species.json provpilot/trees

Per species: import the source (glb / blend, or `blenderkit:<assetBaseId>`:
the free model's glTF is fetched anonymously from BlenderKit's API into
<cache_root>/blenderkit_trees/, licence and author recorded in the atlas
JSON) or build a procedural conifer,
render an orthographic side view and a top view with a transparent film
(Eevee, card_px square each), and write the atlas PNG (columns = species,
upper row side views, lower row top views) plus tree_atlas.json with the
species list the runtime reads (name, conifer, height_m, crown_ratio).
"""
import json
import math
import os
import sys
import tempfile
from pathlib import Path

import urllib.parse
import urllib.request
import uuid

import bpy
import numpy as np
from mathutils import Vector

def _cache_root():
    """scripts/world_regions.json cache_root, like the other scenery scripts."""
    reg = Path(__file__).resolve().parent.parent / "world_regions.json"
    try:
        return Path(json.loads(reg.read_text()).get("cache_root", "/Volumes/Data1/scenery_in_world"))
    except Exception:
        return Path("/Volumes/Data1/scenery_in_world")


CACHE = _cache_root() / "blenderkit_trees"
BK_API = "https://www.blenderkit.com/api/v1"
UA = {"User-Agent": "provpilot bake_tree_cards.py"}


def bk_get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=180) as r:
        return r.read()


def fetch_blenderkit(asset_base_id):
    """The free model's glTF from BlenderKit (anonymous: free assets need no
    account, only a scene id). Returns (path, meta) with name, author, licence."""
    CACHE.mkdir(parents=True, exist_ok=True)
    meta_path = CACHE / f"{asset_base_id}.json"
    glb_path = CACHE / f"{asset_base_id}.glb"
    if glb_path.exists() and meta_path.exists():
        return glb_path, json.loads(meta_path.read_text())
    q = urllib.parse.quote(f"asset_base_id:{asset_base_id}")
    results = json.loads(bk_get(f"{BK_API}/search/?query={q}&page_size=5")).get("results", [])
    if not results:
        raise SystemExit(f"BlenderKit: no asset {asset_base_id}")
    r = results[0]
    files = {f["fileType"]: f["downloadUrl"] for f in r.get("files", []) if f.get("downloadUrl")}
    url = files.get("gltf") or files.get("gltf_godot")
    if not url:
        raise SystemExit(f"BlenderKit: {r['name']} has no glTF file ({sorted(files)})")
    info = json.loads(bk_get(url + "?scene_uuid=" + str(uuid.uuid4())))
    data = bk_get(info["filePath"])
    glb_path.write_bytes(data)
    meta = {"name": r.get("name"), "author": (r.get("author") or {}).get("fullName") or (r.get("author") or {}).get("firstName"),
            "license": r.get("license"), "asset_base_id": asset_base_id, "url": f"https://www.blenderkit.com/asset-gallery-detail/{asset_base_id}/",
            "face_count": (r.get("dictParameters") or {}).get("faceCount")}
    meta_path.write_text(json.dumps(meta, indent=1))
    print(f"  fetched {r['name']} ({meta['license']}, {len(data) // 1024} KB) -> {glb_path}")
    return glb_path, meta

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
SPEC = Path(argv[0]) if argv else Path("scripts/trees/species.json")
OUT = Path(argv[1]) if len(argv) > 1 else Path("provpilot/trees")
OUT.mkdir(parents=True, exist_ok=True)
spec = json.loads(SPEC.read_text())
PX = int(spec.get("card_px", 256))


def clear_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.film_transparent = True
    scene.render.resolution_x = PX
    scene.render.resolution_y = PX
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.view_settings.view_transform = "Standard"
    try:
        scene.eevee.taa_render_samples = 16
    except Exception:
        pass
    world = bpy.data.worlds.new("World")
    scene.world = world
    world.use_nodes = True
    bg = world.node_tree.nodes.get("Background")
    if bg:
        bg.inputs[0].default_value = (0.9, 0.9, 0.85, 1.0)   # neutral ambient: the card keeps its own colour
        bg.inputs[1].default_value = 0.6
    sun = bpy.data.objects.new("Sun", bpy.data.lights.new("Sun", "SUN"))
    sun.data.energy = 4.0
    sun.rotation_euler = (math.radians(40), 0.0, math.radians(-35))
    scene.collection.objects.link(sun)
    return scene


def leaf_material(name, rgb):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (*rgb, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.9
    return m


def procedural(kind):
    """A conifer from stacked cones (spruce: narrow, many tiers; pine: bare
    trunk, loose crown). Good enough for a card seen from 300 m up."""
    objs = []
    h = 1.0
    trunk_mat = leaf_material("trunk", (0.25, 0.17, 0.1))
    if kind == "spruce":
        tiers, crown_w, start = 7, 0.36, 0.08
        green = leaf_material("spruce", (0.05, 0.14, 0.05))
    else:
        tiers, crown_w, start = 3, 0.5, 0.5
        green = leaf_material("pine", (0.1, 0.2, 0.07))
    bpy.ops.mesh.primitive_cylinder_add(radius=0.025, depth=h, location=(0, 0, h / 2))
    trunk = bpy.context.object
    trunk.data.materials.append(trunk_mat)
    objs.append(trunk)
    for i in range(tiers):
        f = i / max(tiers - 1, 1)
        z0 = start + (1.0 - start) * f * 0.85
        r = crown_w * 0.5 * (1.0 - 0.75 * f) + 0.03
        depth = (1.0 - start) / tiers * 1.8
        bpy.ops.mesh.primitive_cone_add(radius1=r, radius2=r * 0.15, depth=depth, vertices=10, location=(0, 0, z0 + depth / 2))
        cone = bpy.context.object
        cone.data.materials.append(green)
        objs.append(cone)
    return objs


def import_source(path):
    before = set(bpy.data.objects)
    if path.endswith(".glb") or path.endswith(".gltf"):
        bpy.ops.import_scene.gltf(filepath=path)
    elif path.endswith(".blend"):
        with bpy.data.libraries.load(path, link=False) as (src, dst):
            dst.objects = [n for n in src.objects]
        for o in dst.objects:
            if o is not None:
                bpy.context.scene.collection.objects.link(o)
    else:
        raise SystemExit("unknown source " + path)
    objs = [o for o in bpy.data.objects if o not in before and o.type == "MESH"]
    for o in objs:
        for slot in o.material_slots:
            m = slot.material
            if m is None or not m.use_nodes:
                continue
            bsdf = m.node_tree.nodes.get("Principled BSDF")
            if bsdf is None:
                continue
            tex = [n for n in m.node_tree.nodes if n.type == "TEX_IMAGE" and n.image]
            if not tex and ("leaf" in m.name.lower() or "foliage" in m.name.lower() or "crown" in m.name.lower()):
                # untextured foliage (the SwissTopo tree's SimpleProceduralLeaf): a leaf green
                for link in list(m.node_tree.links):
                    if link.to_socket == bsdf.inputs["Base Color"]:
                        m.node_tree.links.remove(link)
                bsdf.inputs["Base Color"].default_value = (0.13, 0.3, 0.09, 1.0)
            m.blend_method = "CLIP" if hasattr(m, "blend_method") else m.blend_method
            bsdf.inputs["Roughness"].default_value = 0.9
    return objs


def bounds(objs):
    lo = Vector((1e9, 1e9, 1e9))
    hi = Vector((-1e9, -1e9, -1e9))
    for o in objs:
        for c in o.bound_box:
            w = o.matrix_world @ Vector(c)
            lo = Vector((min(lo.x, w.x), min(lo.y, w.y), min(lo.z, w.z)))
            hi = Vector((max(hi.x, w.x), max(hi.y, w.y), max(hi.z, w.z)))
    return lo, hi


def render(scene, cam_loc, cam_rot, ortho, out_path):
    cam_data = bpy.data.cameras.new("cam")
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = ortho
    cam_data.clip_end = 1000.0
    cam = bpy.data.objects.new("cam", cam_data)
    cam.location = cam_loc
    cam.rotation_euler = cam_rot
    scene.collection.objects.link(cam)
    scene.camera = cam
    scene.render.filepath = str(out_path)
    bpy.ops.render.render(write_still=True)
    bpy.data.objects.remove(cam)


def load_rgba(path):
    img = bpy.data.images.load(str(path))
    w, h = img.size
    px = np.array(img.pixels[:], dtype=np.float32).reshape(h, w, 4)
    bpy.data.images.remove(img)
    return px[::-1]   # top row first


tmp = Path(tempfile.mkdtemp(prefix="tree_cards_"))
species = spec["species"]
atlas = np.zeros((2 * PX, PX * len(species), 4), dtype=np.float32)
meta = []
for col, sp in enumerate(species):
    scene = clear_scene()
    src = sp["source"]
    bk_meta = None
    if src.startswith("blenderkit:"):
        path, bk_meta = fetch_blenderkit(src.split(":", 1)[1])
        src = str(path)
    objs = procedural(src.split(":", 1)[1]) if src.startswith("procedural:") else import_source(src)
    lo, hi = bounds(objs)
    size = hi - lo
    cx, cy = (lo.x + hi.x) / 2, (lo.y + hi.y) / 2
    # the card is one crown width wide and one height tall: frame by the larger
    extent = max(size.z, size.x, size.y) * 1.04
    side_png = tmp / f"{sp['name']}_side.png"
    top_png = tmp / f"{sp['name']}_top.png"
    # side: from -Y looking +Y, the tree centred vertically in the card (base at the bottom)
    render(scene, (cx, lo.y - extent * 2, lo.z + extent / 2), (math.radians(90), 0, 0), extent, side_png)
    # top: straight down
    render(scene, (cx, cy, hi.z + extent * 2), (0, 0, 0), extent, top_png)
    side = load_rgba(side_png)
    top = load_rgba(top_png)
    # The canopy card must read as part of the same tree: match its mean
    # luminance to the side card's (the top view catches the full sun and came
    # out a pale disc over the forest, gery 2026-11-08), a touch darker since
    # the crown's top is seen against sunlit cards' edges.
    def mean_lum(img):
        a = img[..., 3] > 0.5
        rgb = img[..., :3][a]
        return float((rgb @ np.array([0.2126, 0.7152, 0.0722])).mean()) if len(rgb) else 1.0
    ls, lt = mean_lum(side), mean_lum(top)
    cover = float((side[..., 3] > 0.5).mean())
    if cover < 0.04 or ls < 0.1 or ls > 0.7:
        print(f"  WARNING {sp['name']}: side card covers {cover * 100:.1f} % at luminance {ls:.2f} - missing foliage or texture? check the model")
    if lt > 1e-3:
        top = top.copy()
        top[..., :3] = np.clip(top[..., :3] * (0.9 * ls / lt), 0.0, 1.0)
    atlas[0:PX, col * PX:(col + 1) * PX] = side
    atlas[PX:2 * PX, col * PX:(col + 1) * PX] = top
    print(f"  {sp['name']}: side luminance {ls:.3f}, top {lt:.3f} -> scaled to {0.9 * ls:.3f}")
    crown_ratio = round(float(max(size.x, size.y) / size.z), 3) if size.z > 0 else sp.get("crown_ratio", 0.6)
    entry = {"name": sp["name"], "conifer": bool(sp.get("conifer", False)),
             "height_m": sp.get("height_m", 20), "crown_ratio": crown_ratio,
             "source": os.path.basename(src)}
    if bk_meta:
        entry["model"] = bk_meta["name"]
        entry["author"] = bk_meta["author"]
        entry["license"] = bk_meta["license"]
        entry["url"] = bk_meta["url"]
    meta.append(entry)
    print(f"  {sp['name']}: {src} size {size.x:.1f} x {size.y:.1f} x {size.z:.1f} m, crown/height {crown_ratio}")

def dilate_rgb(img, passes=8):
    """Spread the colour of covered pixels into the transparent ones so the
    mip levels and the alpha-scissor edge never blend towards black."""
    rgb = img[..., :3].copy()
    a = img[..., 3] > 0.02
    for _ in range(passes):
        acc = np.zeros_like(rgb)
        cnt = np.zeros(a.shape, dtype=np.float32)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            sa = np.roll(a, (dy, dx), axis=(0, 1))
            sc = np.roll(rgb, (dy, dx), axis=(0, 1))
            acc += sc * sa[..., None]
            cnt += sa
        fill = (~a) & (cnt > 0)
        rgb[fill] = acc[fill] / cnt[fill][:, None]
        a = a | fill
    out = img.copy()
    out[..., :3] = rgb
    return out


atlas = dilate_rgb(atlas)
out_img = bpy.data.images.new("tree_atlas", width=atlas.shape[1], height=atlas.shape[0], alpha=True)
out_img.pixels[:] = atlas[::-1].reshape(-1).tolist()
out_img.filepath_raw = str(OUT / "tree_atlas.png")
out_img.file_format = "PNG"
out_img.save()
(OUT / "tree_atlas.json").write_text(json.dumps({"_comment": "Tree card atlas: columns = species, upper half side view, lower half top view (scripts/trees/bake_tree_cards.py).", "card_px": PX, "species": meta}, indent=1) + "\n")
print(f"atlas {atlas.shape[1]} x {atlas.shape[0]} -> {OUT / 'tree_atlas.png'}, {len(meta)} species")
