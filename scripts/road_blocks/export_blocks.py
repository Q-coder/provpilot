"""Export a road block .blend to the JSON the bake stamps (docs/ROAD_BLOCKS.md).

Run headless:  /Applications/Blender.app/Contents/MacOS/Blender -b scenery/blocks/src/entry.blend -P scripts/road_blocks/export_blocks.py -- scenery/blocks

Objects are read by their name's role prefix; polygons in world XY metres.
"""
import json
import sys
from pathlib import Path

import bpy

OUT = Path(sys.argv[sys.argv.index("--") + 1]) if "--" in sys.argv else Path("scenery/blocks")
OUT.mkdir(parents=True, exist_ok=True)
ROLES = ("pave", "line", "dash", "gore", "cut", "ramp", "ref")


def polygon_xy(ob):
    mw = ob.matrix_world
    m = ob.data
    if m.polygons:
        order = list(m.polygons[0].vertices)
    else:
        order = list(range(len(m.vertices)))
    return [[round((mw @ m.vertices[i].co).x, 3), round((mw @ m.vertices[i].co).y, 3)] for i in order]


def centreline(poly):
    """A thin quad's axis: midpoints of its two shortest edges, and its width."""
    n = len(poly)
    edges = []
    for i in range(n):
        a, b = poly[i], poly[(i + 1) % n]
        edges.append((((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5, i))
    edges.sort()
    (w0, i0), (w1, i1) = edges[0], edges[1]
    mid = lambda i: [(poly[i][0] + poly[(i + 1) % n][0]) / 2, (poly[i][1] + poly[(i + 1) % n][1]) / 2]
    a, b = mid(i0), mid(i1)
    if a[1] > b[1]:
        a, b = b, a
    return [[round(a[0], 3), round(a[1], 3)], [round(b[0], 3), round(b[1], 3)]], round((w0 + w1) / 2, 3)


block = {"name": Path(bpy.data.filepath).stem, "units": "m", "frame": "x from the carriageway's outer edge (+ outward), y along travel (0 at the nose / change)",
         "objects": []}
for ob in bpy.data.objects:
    role = ob.name.split(".")[0]
    if role not in ROLES:
        continue
    rec = {"name": ob.name, "role": role}
    if role == "ramp":
        rec["point"] = [round(ob.matrix_world.translation.x, 3), round(ob.matrix_world.translation.y, 3)]
        rec["direction"] = [0.0, 1.0]
    elif ob.type == "MESH":
        poly = polygon_xy(ob)
        rec["polygon"] = poly
        if role in ("line", "dash"):
            rec["axis"], rec["width"] = centreline(poly)
        if role == "cut":
            rec["mode"] = ob.name.split(".")[1] if "." in ob.name else "dash"
    else:
        continue
    block["objects"].append(rec)
ys = [p[1] for o in block["objects"] if "polygon" in o for p in o["polygon"]]
block["extent"] = [round(min(ys), 1), round(max(ys), 1)] if ys else [0.0, 0.0]
out = OUT / (block["name"] + ".json")
json.dump(block, open(out, "w"), indent=1)
print("wrote", out, len(block["objects"]), "objects")
