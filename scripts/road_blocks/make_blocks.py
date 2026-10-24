"""Draw the road junction block library (docs/ROAD_BLOCKS.md) as .blend files.

Run headless:  /Applications/Blender.app/Contents/MacOS/Blender -b -P scripts/road_blocks/make_blocks.py -- scenery/blocks/src
Then export:   /Applications/Blender.app/Contents/MacOS/Blender -b <file>.blend -P scripts/road_blocks/export_blocks.py -- scenery/blocks

Every block is drawn in metres in the frame of the carriageway it attaches
to: X across, 0 at the carriageway's outer pavement edge, positive outward
(the ramp's side); Y along, in the travel direction, 0 at the nose or the
change. Object names carry the role (see ROAD_BLOCKS.md). These are the
first drafts from the Swiss norms; edit them in Blender and re-export.
"""
import sys
from pathlib import Path

import bpy

OUT = Path(sys.argv[sys.argv.index("--") + 1]) if "--" in sys.argv else Path("scenery/blocks/src")
OUT.mkdir(parents=True, exist_ok=True)

# reference two-lane carriageway: 2 x 3.75 m lanes, 2.5 m hard shoulder, 0.5 m verges
LANE = 3.75
SHOULDER = 2.5
VERGE = 0.5
W_REF = 2 * LANE + SHOULDER + 2 * VERGE          # 11.0 m
EDGE_LINE = -(VERGE + SHOULDER)                  # the carriageway's outer edge line, 3.0 m inside the edge
LINE_W = 0.15
ACC, DEC, TAPER, NOSE = 200.0, 150.0, 75.0, 30.0


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    for c in list(bpy.data.collections):
        bpy.data.collections.remove(c)


def material(name, rgb):
    m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = next((n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
    if bsdf:
        bsdf.inputs["Base Color"].default_value = (*rgb, 1.0)
    m.diffuse_color = (*rgb, 1.0)
    return m


def quad(name, pts, z=0.0, mat=None):
    """A flat polygon from (x, y) points, in order."""
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([(x, y, z) for x, y in pts], [], [list(range(len(pts)))])
    mesh.update()
    ob = bpy.data.objects.new(name, mesh)
    if mat:
        ob.data.materials.append(mat)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def line(name, a, b, width=LINE_W, z=0.01, mat=None):
    """A thin quad from a to b: a painted line (solid) or its dashed axis."""
    (x0, y0), (x1, y1) = a, b
    dx, dy = x1 - x0, y1 - y0
    l = (dx * dx + dy * dy) ** 0.5 or 1.0
    nx, ny = -dy / l * width / 2, dx / l * width / 2
    return quad(name, [(x0 + nx, y0 + ny), (x0 - nx, y0 - ny), (x1 - nx, y1 - ny), (x1 + nx, y1 + ny)], z, mat)


def common(kind_lengths):
    asphalt = material("asphalt", (0.24, 0.24, 0.24))
    lane_m = material("lane", (0.31, 0.34, 0.39))
    paint = material("paint", (0.85, 0.85, 0.82))
    ref = material("reference", (0.18, 0.18, 0.2))
    return asphalt, lane_m, paint, ref


def entry_block(path):
    reset()
    asphalt, lane_m, paint, ref = common(None)
    y0, y1 = -NOSE - 20.0, ACC + TAPER + 10.0
    # reference carriageway (not stamped): edge at x = 0, axis at -W/2
    quad("ref.carriageway", [(-W_REF, y0), (0.0, y0), (0.0, y1), (-W_REF, y1)], -0.01, ref)
    line("ref.edge_line", (EDGE_LINE, y0), (EDGE_LINE, y1), mat=ref)
    line("ref.axis", (-W_REF / 2, y0), (-W_REF / 2, y1), width=0.05, mat=ref)
    # the ramp arrives at the nose: its axis joins the lane centre here, along +Y
    ramp = bpy.data.objects.new("ramp.join", None)
    ramp.empty_display_type = "SINGLE_ARROW"
    ramp.location = (EDGE_LINE + LANE / 2, 0.0, 0.0)
    bpy.context.scene.collection.objects.link(ramp)
    # acceleration lane: on the outer edge line, the shoulder becomes part of it, own verge outside
    x_in, x_out = EDGE_LINE, EDGE_LINE + LANE + VERGE
    quad("pave.lane", [(x_in - 0.2, -NOSE), (x_out, -NOSE), (x_out, ACC), (x_in - 0.2, ACC)], 0.0, lane_m)
    quad("pave.taper", [(x_in - 0.2, ACC), (x_out, ACC), (x_in - 0.2 + 0.2, ACC + TAPER), (x_in - 0.2, ACC + TAPER)], 0.0, lane_m)
    # painted lines: the lane's outer line, its taper line, the dashed separation
    line("line.lane_outer", (x_out - VERGE, 0.0), (x_out - VERGE, ACC), mat=paint)
    line("line.taper", (x_out - VERGE, ACC), (EDGE_LINE, ACC + TAPER), mat=paint)
    line("dash.separation", (EDGE_LINE, 0.0), (EDGE_LINE, ACC + TAPER), mat=paint)
    # gore: the painted nose between the ramp's inner edge and the carriageway edge line
    quad("gore.nose", [(EDGE_LINE, -NOSE), (EDGE_LINE + LANE * 0.9, -NOSE), (EDGE_LINE + 0.3, 0.0)], 0.005, paint)
    # the carriageway's own outer edge line: dashed along the lane and the taper
    quad("cut.dash.separation", [(EDGE_LINE - 1.0, 0.0), (x_out, 0.0), (x_out, ACC + TAPER + 3.0), (EDGE_LINE - 1.0, ACC + TAPER + 3.0)], -0.02, ref)
    bpy.ops.wm.save_as_mainfile(filepath=str(path))


def exit_block(path):
    reset()
    asphalt, lane_m, paint, ref = common(None)
    y0, y1 = -(DEC + TAPER + 10.0), NOSE + 20.0
    quad("ref.carriageway", [(-W_REF, y0), (0.0, y0), (0.0, y1), (-W_REF, y1)], -0.01, ref)
    line("ref.edge_line", (EDGE_LINE, y0), (EDGE_LINE, y1), mat=ref)
    line("ref.axis", (-W_REF / 2, y0), (-W_REF / 2, y1), width=0.05, mat=ref)
    ramp = bpy.data.objects.new("ramp.join", None)
    ramp.empty_display_type = "SINGLE_ARROW"
    ramp.location = (EDGE_LINE + LANE / 2, 0.0, 0.0)
    bpy.context.scene.collection.objects.link(ramp)
    x_in, x_out = EDGE_LINE, EDGE_LINE + LANE + VERGE
    quad("pave.taper", [(x_in - 0.2, -DEC - TAPER), (x_in, -DEC - TAPER), (x_out, -DEC), (x_in - 0.2, -DEC)], 0.0, lane_m)
    quad("pave.lane", [(x_in - 0.2, -DEC), (x_out, -DEC), (x_out, NOSE), (x_in - 0.2, NOSE)], 0.0, lane_m)
    line("line.taper", (EDGE_LINE, -DEC - TAPER), (x_out - VERGE, -DEC), mat=paint)
    line("line.lane_outer", (x_out - VERGE, -DEC), (x_out - VERGE, 0.0), mat=paint)
    line("dash.separation", (EDGE_LINE, -DEC - TAPER), (EDGE_LINE, 0.0), mat=paint)
    quad("gore.nose", [(EDGE_LINE + 0.3, 0.0), (EDGE_LINE + LANE * 0.9, NOSE), (EDGE_LINE, NOSE)], 0.005, paint)
    quad("cut.dash.separation", [(EDGE_LINE - 1.0, -DEC - TAPER - 3.0), (x_out, -DEC - TAPER - 3.0), (x_out, 0.0), (EDGE_LINE - 1.0, 0.0)], -0.02, ref)
    bpy.ops.wm.save_as_mainfile(filepath=str(path))


def lane_change_block(path, add):
    """A carriageway gaining (add) or losing (drop) its outer lane at Y = 0:
    the pavement steps by one lane there; the block tapers the step over
    TAPER m on the narrow side. X = 0 is the NARROW carriageway's edge."""
    reset()
    asphalt, lane_m, paint, ref = common(None)
    y0, y1 = -TAPER - 20.0, TAPER + 20.0
    quad("ref.carriageway", [(-W_REF, y0), (0.0, y0), (0.0, y1), (-W_REF, y1)], -0.01, ref)
    line("ref.edge_line", (EDGE_LINE, y0), (EDGE_LINE, y1), mat=ref)
    if add:
        # the wide part starts at Y = 0 and continues; widen over the TAPER before it
        quad("pave.taper", [(-0.2, -TAPER), (0.0, -TAPER), (LANE, 0.0), (-0.2, 0.0)], 0.0, lane_m)
        line("line.taper", (EDGE_LINE, -TAPER), (EDGE_LINE + LANE, 0.0), mat=paint)
        quad("cut.drop.edge", [(EDGE_LINE - 1.0, -TAPER), (LANE + 1.0, -TAPER), (LANE + 1.0, 0.0), (EDGE_LINE - 1.0, 0.0)], -0.02, ref)
    else:
        quad("pave.taper", [(-0.2, 0.0), (LANE, 0.0), (0.0, TAPER), (-0.2, TAPER)], 0.0, lane_m)
        line("line.taper", (EDGE_LINE + LANE, 0.0), (EDGE_LINE, TAPER), mat=paint)
        quad("cut.drop.edge", [(EDGE_LINE - 1.0, 0.0), (LANE + 1.0, 0.0), (LANE + 1.0, TAPER), (EDGE_LINE - 1.0, TAPER)], -0.02, ref)
    bpy.ops.wm.save_as_mainfile(filepath=str(path))


entry_block(OUT / "entry.blend")
exit_block(OUT / "exit.blend")
lane_change_block(OUT / "lane_add.blend", True)
lane_change_block(OUT / "lane_drop.blend", False)
print("blocks written to", OUT)
