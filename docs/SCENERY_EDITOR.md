# Scenery editor (design / developer mode)

Status: subtasks 1–8 implemented 2026-09-22 and a pilot of 9; the open points
of each subtask are the plan. Owner: gery. Code (C++ GDExtension): `src/scenery_editor.cpp` (mode, camera,
cursor, tools, UI, view gizmo), `src/scenery_design.cpp` (document),
`src/scenery_design_mesher.cpp` (live meshes), tests
`tests/smoke_scenery_editor.tscn` and `tests/probe_scenery_editor.tscn`.

## Object tool (pack models)

`O Object` (key O) edits the models of the enabled object packs
(`<area>/objects/package.json`, see `docs/SCENERY_ARCHITECTURE.md`).

**Selection.** Click a model to select it (the pick is a ray against its
triangles, so a fence's long footprint never steals a click meant for the
table next to it); Shift-click adds or removes one; a click on empty ground
clears. The list on the right names every object of the loaded packs with
the selected ones highlighted; a click there selects, Shift / Cmd adds,
double-click frames the object; `F` frames the selection. Every selected
object is tinted orange (a translucent material overlay on its meshes, Blender-like without a post-process) and the active one also gets its bounding box in its own frame with a spoke out of its front face (+Z). The yellow terrain cursor fades while something is selected. All editor overlays (boxes, previews, design lines, node markers) are anchored at a double-precision origin and hold vertices relative to it: written in ECEF float coordinates they wobbled by up to half a metre as the camera moved (gery 2026-09-27).

**Gizmo.** A Blender-style gizmo drawn on screen at a constant size sits on
the selection's pivot; the tool column on the left switches its mode:

- **Move (G)**: red / green / blue arrows. Dragging an arrow moves the
  selection along that axis, dragging a model or the centre circle slides it
  in the pivot's ground plane.
- **Rotate (R)**: red / green / blue rings. The up ring turns the true
  heading, the other two tilt (saved as `pitch_deg` / `bank_deg`); a ring
  seen edge-on is faded and cannot be grabbed. With several objects selected
  they swing around the pivot and each turns on its own.
- **Scale (Y)**: red / green / blue cubes scale one axis each, the centre
  circle or a model scales all three; several objects also spread out from
  the pivot (saved as a number, or `[x, y, z]` when the axes differ).

A click never moves anything: drags start after 4 px. Holding **Shift**
while dragging makes the mouse count a tenth, for fine adjustments.

**Orientation and pivot** (the two menus at the top of the transform panel):
*Global* puts the gizmo on world east / north / up, *Local* on the active
model's own X / Y / Z (a glTF model facing north has +X pointing west), so a
building can be slid along its own front. *Median point* rotates and scales
a group about the middle of the selection, *Individual origins* about each
object's own pivot.

**Transform panel** (right side, like Blender's Item tab): the active
object's location (LV95 E / N and height above ground), rotation (heading,
pitch, bank), scale and dimensions in metres can be typed; a typed
dimension sets the scale of that axis, which is the easy way to shrink a
model to a known size. A click in a field takes the keyboard: type the
value, Enter applies it and hands the keys back to the editor (Escape or
moving the mouse off the panel does the same, applying what was typed).
Clicks and the mouse wheel over the panel stay in the panel; they never
select, drag or zoom the scene behind it (gery 2026-09-27: the editor
used to read keys before the GUI and wheel turns the panel left unhandled).
The **Pivot** buttons move the model's pivot to its
bounds centre, base centre, own origin or the terrain cursor without moving
the model (saved as `pivot` in the pack, the model-space point that sits at
the placement position).

Fine steps on the keyboard: `[` `]` height 0.5 m (Shift 5 m), `,` `.`
heading 1° (Shift 10°), `-` `=` scale 5 % (Shift 25 %), all applied to the
whole selection. `Ctrl+G` drops the lowest vertex of every selected model
onto the terrain, `Delete` removes the selection from its pack, Ctrl+Z
undoes one edit step (a group drag is one step). Ctrl+S (and leaving the
editor) writes the edited packs' package.json back, with the exclusion
circle of every object following it; that file is what a scenery pack
ships, so the edits are exported with the pack.

Models load on a worker thread (parsing and scene generation both), so a
50 MB model no longer stalls the frame when it comes into range.

## 1. Why

The OSM bake (design §13.9) gets roads and railways *roughly* right and
then hits the limits of the data: no heights, junctions and interchanges
as ribbon overlaps, 30 m terrain under 4 m tracks. gery, looking at the
Zürich Nord interchange (2026-09-22): "we have to approach this
differently. PVP needs a scenery design / developer mode … independent of
any aircraft, works like the slew mode. It has a cursor and tools for
scenery building … with the possibility to save changes to the scenery."
The editor is that mode: a person places and corrects scenery elements in
the running simulator, with the terrain and the OSM base visible, and the
result is a small vector file that ships with a pack and that the bake
folds in. Scope of the first tools: roads (straights, curves, crossings,
circles, lane expansion / reduction) and railways (single tracks,
switches).

## 2. Principles

- **Independent of the aircraft.** F8 freezes the flight model like slew,
  takes its own free camera and gives it back on exit; the aircraft is
  neither moved nor needed.
- **Vector source, not meshes.** The editor edits a *design*: a graph of
  nodes (LV95 metres) and typed edges. Meshes are derived, live in the
  sim and at bake time, by the same rules as the OSM ribbons, so designed
  and baked roads look identical.
- **Saved in the library, loaded every flight.** The design file lives at
  `<install root>/scenery/Design/design/scenery_design.json` (the install
  root of `provpilot.cfg`, where the weather profiles live too; override
  with `PVP_DESIGN_FILE`). Never derived from the scenery areas: their
  order changes between sessions, which lost a design on 2026-09-22 (it
  went to the Norway pack's parent). At every flight start it is meshed
  once the terrain around is in, so edits are part of the scenery, not of
  the editing session.
- **Replaced ways vanish without a bake.** Every baked vertex carries its
  OSM way id (`_WAYID`, uint32). The editor hands the road manager the set
  of replaced ways (picked or deleted ids, plus every sidecar way with a
  point inside a replace polygon); `S3DRoadManager.set_hidden_ways`
  reloads the tiles and drops those ways' triangles as each tile is
  built. So a pick or a delete shows at once, in the editor and in normal
  flight (the design is applied at every start), and the bake only makes
  it permanent in the streamed tiles (option 2 of the "alternatives to
  baking" discussion, 2026-09-23; terrain patches at tile load are the
  second step, done the same day).
- **Terrain follows the design without a bake.** The tile loader takes
  the design's plateaus and corridors (`S3DTileManager.set_terrain_patches`)
  and writes them into every close tile's heights as it is decoded, before
  the mesh and the elevation query see them — the bake's `--corridor`
  rules at load time: plateaus at their height, the smoothed ground profile
  plus node offsets under every road and track run that is not elevated,
  reach = half width + 45 m. The editor rebuilds the patches on every
  change; the close tiles reload (about a second) and the live meshes
  re-sample the patched ground. The patches are computed only in the
  editor, where the terrain around the edits is loaded, and kept in the
  design file (`patches`); a flight or an Edit Mode session applies that
  cache **before its first tile loads**, so nothing ever reloads under a
  placed aircraft (gery 2026-09-23: the Cessna on LSMD's runway was thrown
  into the air and landed on its roof when the tiles reloaded a few
  seconds after placement). Should a reload still happen while an
  aircraft is on the ground — an edit in the F8 editor, say — the
  teleport pin holds the aircraft until the terrain has settled. Far chunks stay unpatched
  until they come close. **Airport plateaus are protected**: runway strips
  (+60 m), taxiways (+25 m) and aprons of every known airport are handed
  over as protection shapes, and neither a live corridor nor the bake's
  `--corridor` changes a pixel inside them — gery's A-10 dropped onto
  Dübendorf's runway on 2026-09-23 after a design road's corridor had cut
  the plateau the runway plane sits on; the plateaus were restored with
  `build_world_terrain.py flatten` and the roads re-baked with protection.
  A designer's Flatten plateau still wins (it is explicit).
- **Baked ways can be picked.** Every bake writes `roads_E_N.ways.json`
  next to each tile: the source ways (id, class, lanes, one-way, gauge,
  bridge / tunnel flags, LV95 polyline every 20 m). The editor reads the
  sidecars of the tiles around the cursor, so a baked OSM road can be
  picked into the design and replaced by id.
- **The bake consumes it.** `build_world_roads.py --design <file>` adds
  the design's ways to the OSM set with the corridor, class and lift
  rules, bakes its junction pavements and lane tapers, cuts OSM ways
  inside its *replace* polygons, and records the file's sha256 in the
  roads manifest; the runtime sees that signature and keeps the live
  meshes off until the design is edited again, so the base tier on the
  server carries the corrections and the live mesher only shows what is
  not baked yet.
- **Same units everywhere.** Metres, LV95 for the Swiss packs (a design
  file names its CRS; other grids come with the first non-Swiss pack).

## 3. The mode (implemented)

Two ways in: **F8** during a flight (the flight model freezes, the
aircraft stays), or the welcome screen's **Edit Mode** button (gery
2026-09-22: "loads right to the scenery edit mode without loading an
aircraft and without the zoom in routine; it loads the area of the
selected airport"). Edit Mode parks the aircraft hidden at the airport
(the sun observer and the map need a position), keeps the flight model in
slew, switches scenery streaming on around the editor camera 300 m over
the airport reference point looking along the runway, and starts no intro
zoom; leaving the editor (F8 or Esc) returns to the welcome screen, from
where Fly! starts a normal flight.

| Key / mouse | Action |
|---|---|
| F8 | enter / leave the editor (also Esc when no line is open) |
| W A S D, Q E | move horizontally / down, up; Shift = 4× faster; speed grows with height |
| PgUp, PgDn | climb, descend (editor only; in flight they zoom the camera) |
| right mouse held | look around |
| Space | view straight down, heading kept; Shift+Space: straight down and north up. The point in the middle of the screen stays in the middle |
| axis gizmo (right edge) | Blender-style: X (east, red), Y (north, green), Z (up, blue) as the camera sees them, hollow circles for the opposites. Click Z for the top view, −Z for a level view, X / −X / Y / −Y for a side view from the east / west / north / south; drag anywhere on it to orbit around the point in the middle of the screen |
| mouse wheel | closer / farther along the view |
| 1 2 3 4 5 6 7 8 9 | tool: Select (drag a node), Road, Rail, Delete, Circle, Switch, Replace, Flatten, Area |
| Pick tool (0) | click a baked OSM road or track (a design road under the cursor is named instead, with a pointer to Select / Delete): it becomes design nodes and edges with the way's class, lanes and gauge (ends snap to existing design nodes); the original disappears from the tiles at once and its OSM id goes to `replace_ids` so the bake drops it for good. The panel names the way under the cursor. Needs the tiles' `roads_E_N.ways.json` sidecars (bakes from 2026-09-22 on) |
| Area tool | clicks add the corners of a patch, Enter closes it: grass, forest floor, field (flat on the highest ground under it), water (flat on the lowest), or a building (roof at the panel height, walls to the ground); Delete on a corner removes it |
| R | Select with a road selected: reverse its direction (the whole run of equal segments). Direction: a design road flows in the order its points were placed, first click to last; a baked road flows in its OSM way direction (the arrows on the OSM map). One-way roads show cyan (design) or green (baked) arrows every 40 m within 1.5 km of the camera; the panel prints the flow heading of the road under the cursor and, while drawing, the heading the next segment will flow |
| [ ] | lower / raise by 0.5 m, { } by 5 m (read as typed characters, and Option+5 / 6 / 8 / 9 count as [ ] { } whatever character the Mac reports, so a Swiss or German Mac keyboard works; Shift+[ ] on keyboards that have the keys). Object tool: the selected objects' height. Select: the selected node's height offset — a deck, an embankment or a cut, interpolated along the edges; with a selected edge (click a design road away from its nodes) both its ends move; Flatten while drawing: the plateau height |
| Flatten tool | clicks add the corners of a plateau, the height starts at the ground of the first corner ([ ] adjusts it), Enter closes it (blue outline at the target height with drop lines down to the ground at the corners). The close terrain tiles reload with the plateau at once; the bake writes it into the tiles for good. A click on a corner of a finished plateau (Flatten or Select tool) selects it and [ ] { } move its height in place, the terrain following; Delete on a corner removes the whole plateau. To sink the world terrain under a photogrammetry tile: draw the plateau around the tile and lower it below the tile's lowest ground |
| Replace tool | clicks add the corners of an area, Enter closes it (red outline): the bake cuts every OSM road and track inside it, so the design's own roads take their place; Delete on a corner removes the area |
| Switch tool | click a track (the toe of the turnout), then the end of the diverging track: the track is split there and the diverging track leaves as the arc tangent to it (radius shown; below 150 m it is refused, 190 m is the tightest standard turnout); continue it with the Rail tool |
| C | curved: each new Road / Rail segment continues the last one as a tangent arc (the panel shows the radius) |
| Circle tool | click the centre, then a point on the ring (a second click on the centre takes the panel radius): a one-way ring of eight arcs, counter-clockwise, i.e. a roundabout for right-hand traffic |
| left click | place the next point of a line (snaps to a design node on screen, to the end of a baked road within 12 m so the two join exactly, or splits a crossed design line into a junction), pick a node (Select), remove a line (Delete) |
| Enter, right click | finish the line |
| Esc | cancel the open line, else drop the selection, else leave the editor (the host ignores that same press, so the welcome screen does not open on top — a 2026-09-23 fix after it did and swallowed the keys) |
| Ctrl+Z, Ctrl+S | undo, save |
| Bake area (Ctrl+B) | saves, then runs `build_world_roads.py` for the 7 × 7 km of tiles around the camera with this design in the background (`--corridor` when the design has roads, tracks or plateaus), and reloads the road tiles when it finishes: picked or deleted OSM roads leave the tiles, designed roads and plateaus become part of them. The panel shows the running time and the bake's last log line (log in the user data dir). Needs Python with the bake's libraries on this machine |

While the editor is on the air is clear: no fog whatever the weather
says, and the cloud layers are hidden (gery 2026-09-22: "the edit mode
should be free of any haze"); both return on exit.

Picking is done on screen (gery 2026-09-23: nodes above the ground and
nodes seen from far away could not be selected): a node or an edge is
picked when it lies within 14 pixels of the mouse, whatever its height,
and only then by a 6 m ground distance. Every design node shows as a
yellow sphere (white when selected) scaled with distance, and design
roads are drawn with a blue tint while the editor is on so they stand
apart from the baked ones. Delete on a baked road that is not in the
design hides it in the tiles at once and lists its OSM id, which the
next bake drops for good.

A point placed on the end of a baked road (12 m snap) joins it: the line
takes that road's class, lanes, direction and width, and when the road is
one-way the line is turned to flow with it — into the road when it joins
the road's start, out of it when it joins the road's end (gery 2026-09-23:
a 3-lane one-way motorway drawn towards the end of its continuation had
its hard shoulder on the wrong side). Typed panel values count as soon as
they are typed, Enter is not needed (a typed "3" that was lost on focus
change produced the "single lane" of the same report). Picking a road,
selecting a design road, or selecting one of its nodes
loads its class, lanes, direction and width into the panel, so a segment
drawn from there continues it with the same settings (gery 2026-09-23:
the extension of a picked motorway "won't fit"). The panel's fields drop
keyboard focus as soon as the mouse leaves the panel, so WASD never types
into the lanes field.

The cursor is the mouse ray marched against the terrain (the scenery
elevation query, so it works on every pack and on the coarse globe); the
yellow marker shows the hit, the panel its LV95 coordinates and height.
The panel also holds the road class, lane count and one-way flag for the
Road tool and the gauge for the Rail tool, the node / edge / mesh counts,
the design path and whether it is unsaved.

## 3b. Tutorial: fixing a road crossing (a bridge over the motorway)

The OSM bake draws every road on the ground, so where a road crosses the
motorway on a bridge the two overlap at grade (the picture of 2026-09-22).
Nothing baked can be selected directly: the tiles are meshes. The Pick
tool turns the baked way into design geometry you can edit.

1. Welcome screen → **Edit Mode** with the nearest airport selected, or
   F8 in flight. Fly to the crossing (WASD, wheel; Space for the top
   view, drag the gizmo to orbit). The air is clear in the editor.
2. Press **0** (Pick). Move the cursor onto the crossing road: the panel
   shows what is under it, e.g. `secondary, 2 lanes (OSM way 123)`. Click.
   The way becomes nodes and edges (blue when picked: it draws just above
   the original) and its id is listed for the bake. If the road is split
   into several OSM ways, pick each piece; their ends join.
3. Press **1** (Select). Click the node nearest the motorway's centre;
   press **]** a few times (0.5 m each, Shift for 5 m) until the panel
   shows an offset of about 6 m. Do the same for the next node on each
   side with smaller offsets, or leave them at 0: the deck ramps down to
   the ground between nodes. Piers and parapets appear as soon as a
   stretch is more than 0.3 m up. A node too far from the crossing? Drag
   it with Select, or place extra nodes by picking the road at that point
   with the Road tool (a click on an edge splits it).
4. If the OSM geometry is wrong (a kink, a missing curve), delete an edge
   with **4** and redraw it with **2** and *curved* on, snapping to the
   existing nodes; or draw a Replace area (**7**) over the mess and redraw
   everything inside it.
5. **Ctrl+S** (also automatic on leaving). The design is live in the sim
   from now on, over the baked tiles.
6. Press **Bake area** (Ctrl+B). The tiles around the camera are re-baked
   with the design in the background (about half a minute) and reload:
   the picked OSM ways are gone, the design's roads take their place with
   the deck lifted and the piers baked, and the editor reports the design
   as baked and switches its live meshes off. The same bake for a whole
   region is the command line
   `scripts/build_world_roads.py --region switzerland-base --bbox … --corridor --design <install root>/scenery/Design/design/scenery_design.json`
   (the publishing step for the streamed base tier).

## 4. Design document (implemented)

```json
{"schema": 1, "crs": "EPSG:2056", "next_id": 12,
 "nodes": [[1, 2683017.0, 1258547.0], [2, 2683417.0, 1258547.0]],
 "edges": [{"id": 3, "a": 1, "b": 2, "kind": "road", "cls": "secondary",
            "lanes": 2, "oneway": false, "gauge": 0, "tracks": 0}]}
```

- An edge may carry an explicit `width` (metres; 0 = from the lanes). A
  picked OSM way brings its `width` tag along (the sidecars carry it), so
  the design copy is as wide as the baked original; the panel has a width
  field for drawing.
- A node is a point, optionally with a height offset `h` (metres above
  the ground, negative below): a deck, an embankment or a cut; offsets are
  interpolated along the edges by arc length and junction pavements take
  their node's offset. `flatten` lists plateaus (`poly`, `h` metres ASL)
  for the terrain.
- An edge joins two nodes and carries the attributes
  of the OSM bake: `kind` road / rail, `cls` (OSM highway or railway
  class), `lanes`, `oneway`, `gauge` (mm), `tracks`.
- A junction or crossing is a node with three or more edges. Placing a
  point on an existing edge splits it there, so lines connect instead of
  overlapping.
- An edge may carry `via: [e, n]`: it is then the circular arc through
  that point (sampled every 10 m, at least four segments). Splitting an
  arc gives two arcs of the same circle; nearest-edge tests follow the
  arc, not the chord.
- A *chain* is a run of edges with equal attributes through nodes of
  degree two; the mesher extrudes a chain in one piece (mitred corners,
  continuous UVs; a ring closes). Attributes changing at a node (lanes,
  class) start a new chain.
- Undo is a snapshot stack of the document (100 steps); save writes the
  JSON, load restores it; ids are stable so a pack can diff designs.

## 5. Live meshes (implemented)

`SceneryDesignMesher` builds one `MeshInstance3D` per chain in a local ENU
frame at the chain's first station (the same `W3DCoords` frame the runway
meshes use): stations every 10 m, the ground as the highest terrain of
the centre and both edges (no smoothing, so nothing sinks), lift 0.20 m
for roads with the per-class drops and 0.15 m for ballast, widths from
the lane count exactly as `road_geometry()` in the bake, rails as the
extruded UIC 60 section at the gauge, and the base-tier roads shader on
every mesh. The heights come from the precise scenery terrain only (never
the coarse globe: meshes built on it hovered until the tiles arrived and
then dropped, gery 2026-09-23); a chain whose terrain is not streamed in
yet is left out and built when the streamer next settles, in the editor
and in flight alike. The design is re-meshed on every edit.

Junctions: at every node with three or more road edges the mesher builds
a pavement polygon before the chains — legs sorted by bearing, the corner
of each neighbouring pair where the left edge line of one leg meets the
right edge line of the next (nearly parallel legs get a corner between
the edge lines), each leg cut back to its farthest corner (at least 1 m),
and the outline (right cut point, left cut point, corner, per leg) fanned
from the node as one plain-asphalt mesh (shader kind 6, UV = metres from
the node) at the highest leg class lift + 5 mm. The chains then start at
the cut, so no ribbon ends overlap inside the junction. Rails at a node
are ignored here (a level crossing stays an overlap). Lane changes: where
a chain ends at a degree-2 node whose other road edge is narrower, the
wider ribbon tapers to the narrower width over 30 m per lane (per-station
half widths in UV2, so the shader's lane dashes follow the taper).

## 6. Subtasks

1. **Mode, camera, cursor, panel** — done 2026-09-22. F8, free camera in
   ECEF with a heading / pitch model, terrain cursor by ray march + bisect,
   HUD panel, entry refused while the flight is still loading; welcome
   screen Edit Mode (`tests/smoke_edit_mode.tscn`); Ctrl+S / Ctrl+Z ahead
   of the GUI, no movement while a modifier is held, autosave on exit.
2. **Document and persistence** — done 2026-09-22. `SceneryDesign`
   (nodes, edges, split, chains, undo, JSON), library path, load at every
   flight start, save with Ctrl+S.
3. **Straight roads and single tracks, crossings** — done 2026-09-22.
   Road and Rail tools placing chains, snapping and splitting for
   junctions, Delete, Select-drag, live meshes (`SceneryDesignMesher`).
   Verified by `smoke_scenery_editor` (mode, camera, axes, cursor hit,
   split, chains, ring, mesh on the terrain, save / reload, undo, exit) and
   renders from `probe_scenery_editor` (design_demo: motorway pair, primary
   with a bend, rail crossing, one-way ring).
4. **Curves and circles** — done 2026-09-22. Arc edges (`via`), tangent
   arcs while drawing (C), the Circle tool (roundabout ring of eight
   arcs), arc-aware split and picking, chain polylines from arc samples;
   checked by `smoke_scenery_editor` (tangent, sampling, split, ring on
   the circle) and the `design_curves` renders. Not done: connecting the
   roads that touch a circle automatically (place the road's last point on
   the ring: it splits the ring arc and joins), splines.
5. **Junction surfaces and lane transitions** — done 2026-09-22 (see
   §5: corner-fan pavement per junction node, legs trimmed, tapers at
   lane changes; roads touching a ring join it through the arc split).
   Checked by `smoke_scenery_editor` (T-junction pavement, leg cut, first
   vertex at the pavement edge, taper to the one-lane width) and the
   `design_junctions` renders (4-way crossing, T, 2→4 lane change,
   roundabout with three roads). Not done: corner fillet radii, stop
   lines and give-way markings, a centre island for roundabouts, tapers
   for ramps merging into a motorway.
6. **Railway switches** — done 2026-09-22. Switch tool: the track is
   split at the toe (`SceneryDesign.add_switch`), the diverging track is
   the tangent arc to the clicked end (radius from the click, refused
   below 150 m), attributes copied from the track. The through track
   stays one line of two chains meeting flat at the switch node; the
   diverging rails start on the through rails and separate along the arc
   (the point blades), the rail sections cross where they must (the
   frog); each chain's ballast sits a few millimetres apart so shared bed
   does not z-fight. Checked by `smoke_scenery_editor` (refusal of a tight
   radius, switch node of degree 3, diverging direction = through tangent,
   attributes copied) and the `design_switches` renders (a passing loop:
   two turnouts, 300 m reverse curves, siding 9 m off the main line). Not
   done: fixed standard turnout geometry (1:9 / 1:12 / 1:14 with a straight
   switch panel), crossings (diamonds), catenary.
7. **Bake integration** — done 2026-09-22. `build_world_roads.py
   --design <file>` (repeatable): the `Design` class mirrors the editor
   (chains, arc sampling, junction corner fans, leg cuts, tapers) and
   turns the design into ways with synthetic node refs (10^15 + id) that
   go through the same stages as OSM ways (profiles, corridors, final
   ground); OSM ways are clipped to the outside of the design's replace
   polygons (a way through a polygon becomes its outside pieces);
   junction pavements are fanned into the tile of their node on the
   final ground (kind 6); the manifest gets `designs: [{path, sha256,
   built}]`. Runtime: `SceneryEditor.design_is_baked()` looks for the
   design file's sha256 in the manifests of the road manager's paths and
   keeps the live meshes off while it matches and the design is clean;
   the first edit brings them back. Checked by `smoke_scenery_editor`
   (replace polygon round trip, baked detection through a manifest, live
   meshes off / back on) and a test bake of `design_junctions` into a
   scratch roads directory rendered with `PROBE_ROADS_DIR`. Not done:
   the bake still needs the `--design` path by hand (no discovery of
   every enabled area's `Design/design/*.json`), Windows / server runs.
   Replacement by OSM id (`replace_ids`, filled by the Pick tool) was
   added on 2026-09-22 together with the way sidecars.
8. **Heights and terrain tools** — done 2026-09-22. Node height offsets
   (Select + [ ]), meshed live along the chains; the Flatten tool stores
   plateaus. Bake: a design chain is split into its offset runs — a piece
   whose offset exceeds 0.3 m is *elevated* (profile + offset, no
   corridor, so the ground stays under the deck), a negative offset is a
   cut the corridor digs — both relative to the way's smoothed profile,
   so on a bump a small cut can still end above the raw terrain; a chain
   is split into its elevated and ground runs at the bake; plateaus are
   written into the terrain tiles
   before any profile is sampled (stage 0, with `--corridor`, the flag
   that permits terrain writes). Checked by `smoke_scenery_editor` (deck
   offset lifts the ribbon 6 m, zero offsets drop, plateaus and offsets
   round-trip) and a test bake of `design_heights` against a scratch
   terrain copy (plateau pixels at the target height, deck vertices over
   the ground). Not done: piers, abutments or retaining walls for decks
   and cuts, blending at plateau edges (a plateau is a hard step, like the
   runway flatten), live terrain edits in the sim (the runtime terrain
   changes only after a bake).
9. **More elements** — pilot 2026-09-22. Bridge structures: every
   elevated run (stations more than 0.3 m up) gets concrete parapets
   along both edges and a pier every 25 m from the deck's underside to
   the ground, live and in the bake (shader kind 7; the bake skips piers
   under decks less than 1 m up and piers that would stand on another
   way, 2026-09-23). Areas (`areas` in
   the document): grass, forest floor and field patches flat on the
   highest ground under them, water flat on the lowest, buildings as a
   roof at their height with walls to the ground (kinds 10–15, ear-clipped
   in the bake, `Geometry2D.triangulate_polygon` live); the bake puts
   them into the roads tiles for now, until the base tier has its own
   land cover, water and building bakes. Checked by `smoke_scenery_editor`
   (piers under a deck, two area meshes, building roof + four walls, JSON
   round trip, clear air on / off) and the `design_elements` renders and
   bake. Not done: tunnel portals, abutments and retaining walls, trees
   on forest patches, roofs other than flat, water flow / shoreline
   blending, moving areas to their own base-tier bakes.

## 7. Open points

- The shader draws the OSM base and the design with the same material;
  a design road over an OSM road z-fights until the bake replaces the OSM
  way (subtask 7) — until then, delete-by-design is not possible in the
  live view.
- Mouse input reaches the editor through `_unhandled_input`; the cockpit
  click handling in `node_3d.gd` is skipped while the mode is on.
- Windows / joystick: the free camera is keyboard + mouse only for now;
  the slew axes could drive it later.
