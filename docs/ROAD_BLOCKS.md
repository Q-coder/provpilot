# Road junction blocks

The survey data (swissTLM3D, OSM) carries road axes, widths and heights,
but nothing of the shape of a junction: the acceleration lane beyond an
entry's nose, the deceleration lane before an exit, the tapers, the gore,
the widening where a lane is added. Those shapes are standard engineering,
so they are *blocks*: drawings of one situation each, in the frame of the
carriageway they attach to, which the bake finds in the data and stamps
along the real carriageway curve wherever the situation occurs. One
`entry` drawing serves every entry of the country; each stamp follows its
own carriageway's curve, width and slope.

Blocks are drawn in Blender (`scenery/blocks/src/*.blend`), exported to
JSON (`scenery/blocks/*.json`), and read by `scripts/build_world_roads.py`
(`--blocks DIR`, default `scenery/blocks`). Design record:
`docs/WORLD_SCENERY_DESIGN.md` §13.9 "Autobahn first" / "Junction blocks".

## Frame

- Metres. **X across** the road, 0 at the carriageway's outer pavement
  edge, positive outward (the ramp's side). The bake mirrors X for a ramp
  on the left. Inside the carriageway X is negative: a two-lane reference
  carriageway spans X = −11 … 0, its outer edge line lies at X = −3 (the
  0.5 m verge and the 2.5 m hard shoulder are inside the edge).
- **Y along** the road in the travel direction, 0 at the *nose* (where the
  ramp axis meets the carriageway) or at the lane change.
- Z is ignored; use it only to keep objects apart in the viewport.
- The carriageway's own pavement, lines and dashes are drawn by the way,
  not by the block: draw them as `ref.*` objects to see what you attach to.

## Roles (object name prefix)

| prefix | what the bake does with it |
|---|---|
| `ref.*` | nothing; reference geometry for drawing |
| `pave.*` | asphalt pavement polygon, 1 cm over the carriageway |
| `line.*` | a solid painted line: a thin quad, its long axis is the line, its short edge the width |
| `dash.*` | a dashed painted line (6 m every 12 m) along the thin quad's axis |
| `gore.*` | a painted nose: the polygon's outline and a hatch every 3 m |
| `cut.dash.*` | inside this polygon the carriageway's edge line on the block's side is drawn dashed |
| `cut.drop.*` | inside this polygon the carriageway's edge line on the block's side is not drawn |
| `ramp.join` | an empty: the ramp's axis is re-joined to this point tangentially along +Y (its last / first 60 m become a Hermite curve) |

Polygons are stamped point by point after segmentising their edges every
5 m, so a 200 m lane bends with the road. A block longer than the
carriageway chain it lands on (the box edge, a tunnel) is clipped in Y.

## Situations the bake recognises

| block | where |
|---|---|
| `entry` | a one-way link ending on a one-way motorway / trunk carriageway at a shallow angle: a TLM Einfahrt beside the unsplit carriageway line, or (OSM) at the node where the carriageway is split into an arriving and a departing way |
| `exit` | a one-way link starting from one (TLM Ausfahrt, or the OSM split node) |
| `lane_add` | two one-way ways of one class continuing each other, the second one lane wider: X = 0 is the narrow way's edge, the taper lies before the change |
| `lane_drop` | … the second one lane narrower: the taper lies after the change |

Everything else (collector lanes of class Autobahn, ramp joining ramp,
forks) still gets the generic union patch of the raw ribbons.

## Workflow

```
B=/Applications/Blender.app/Contents/MacOS/Blender
$B -b -P scripts/road_blocks/make_blocks.py -- scenery/blocks/src      # first drafts from the norms (overwrites!)
# edit scenery/blocks/src/<block>.blend in Blender
$B -b scenery/blocks/src/entry.blend -P scripts/road_blocks/export_blocks.py -- scenery/blocks
.venv/bin/python scripts/build_world_roads.py --region switzerland-base --source tlm --classes motorway,motorway_link --bbox … --corridor
```

The JSON is what the bake reads; the .blend is the source. Keep both in
git. The scenery editor will later place the same files by hand.
