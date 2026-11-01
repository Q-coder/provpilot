# Road network plan — from ribbons to lanes

Status 2026-10-25: proposal agreed with gery ("let's do both in that
order": the two quick fixes, then this). Nothing below is implemented yet
except where a slice says so.

## Why

`scripts/build_world_roads.py` draws one ribbon per OSM way along the OSM
centreline and repairs the overlaps afterwards (union patches, blocks,
tapers, lane-overlap cutters, cuttings, node heights). From the air that
matches the map. Up close it cannot be clean, because the ribbons are the
model: OSM centrelines are metres off, carry no lane geometry, split a
carriageway at every ramp, and every ribbon paints its own lines. gery's
standard is clean over accurate ([memory: roads-clean-over-accurate]):
circles are circles, a merge is a merge, no two pavements over each other.

## The model

A **network**: nodes and segments. A segment is a smooth curve (arcs or
clothoids fitted to the OSM points) with a **lane configuration** per
stretch: lanes per direction, shoulder, median, the lane that is added or
dropped and where. Markings, pavement edges, gores and junction pavement
are *derived* from the lanes, so nothing overlaps and nothing is painted
twice. Primitives:

- **Carriageway segment**: n lanes one way, or n + m two way with or
  without median. The two OSM one-way ways of a motorway become one
  dual-carriageway segment (one axis, two carriageways, a median strip).
- **Lane change**: a lane added or dropped over a taper length (the
  lane_add / lane_drop blocks become this).
- **Ramp attachment**: a one-lane segment joins or leaves a carriageway as
  an extra lane over a merge length, with a painted gore (the entry / exit
  blocks become this).
- **Roundabout**: ring radius and width, legs radial, splitter islands.
- **Junction**: legs meeting at a node, pavement from the lane edges with
  corner radii (the fan), stop lines.
- **Structures**: deck, cutting, tunnel portal, as now, on the network's
  heights (the height passes of 2026-10-25 stay: chain smoothing, node
  reconciliation, coupling, cut-or-lift).

OSM supplies topology, class, lane counts, one-way, bridge / tunnel /
layer and roundabout tags. swissTLM3D supplies true heights where present.
The scenery design (the editor) is a hand-made network in the same model,
so a picked OSM road and a designed road are one thing and one generator
draws both: the design mesher's code grows into that generator, and the
bake uses it through the same Python port (or the C++ generator is driven
from the bake; decide in slice 3 by measuring).

## Slices, each drivable when done

1. **Segments instead of ways** — done 2026-10-25 (first cut). A way
   knows its place along its chain (`chain_off`, `chain_fwd`; chains now
   continue through a junction along the straightest same-class pair of
   legs, `chain_links(through=True)`, which the chain smoothing uses too),
   and `emit_markings` lays the dashes out in chain chainage, so the
   rhythm runs through every OSM split and every tile piece. One asphalt
   colour for every class (`ASPHALT`, and the mesher's `class_colour`), so
   a class change along a road is not a seam; the class still sets the
   width, the lines and the z-order drop. Kept per way: the ribbon pieces
   (joined on mitres, invisible) and `_WAYID` for the editor's pick. Open
   from this slice: the edge line at a lane drop still steps where the
   block's own lines take over.
2. **Dual carriageways** — done 2026-10-25 (first cut, `pair_carriageways`,
   stage 1a). Every station of a one-way motorway / trunk way finds the
   opposite carriageway within 35 m; along the chain the separation is
   smoothed (sigma 40 m) and held at least at the two half widths plus a
   1.5 m median, and both carriageways move half the difference (fading
   out over 30 m at the ends of a paired stretch; a moved way end takes
   the ramps ending there along). OSM axes 9–13 m apart under two 11 m
   carriageways had the pavements overlapping. Each carriageway draws a
   flat median strip in the ground colour from its inner pavement edge to
   the midline, just under the pavement surface, and no embankment strip
   on that side. Kloten box: 27 ways paired, 589 stations moved, the A11
   now 12.5–14.4 m axis to axis with a 1.5 m gap between the pavements.
   Not done from the original plan: one shared axis (each carriageway
   keeps its own line, parallel by the smoothed separation), heights
   equal by construction (the coupling pass still does it), the median's
   width from reality (1.5 m everywhere; the real A11 has 2–3 m and a
   barrier).
3. **Lane model and merges** — started 2026-10-25 (step 1 done). Entry
   and exit blocks are generated from the data (`make_merge_block`): the
   bake measures how far the link already runs inside the carriageway's
   pavement (OSM often draws the acceleration lane as the link, ending
   where it merges; Balsberg: 127 m), puts the join there, cuts the link's
   ribbon back to it (`rejoin_ramp` with the measured join length), and
   lays the lane from the gore over max(overlap + 30 m, 200 m for a
   motorway, 150 m trunk) with a 75 / 60 m taper, dashed separation, outer
   line and painted nose. The drawings `entry.json` / `exit.json` are no
   longer used; `lane_add` / `lane_drop` still are. Found and fixed on the
   way: `pair_carriageways` moved the two ends at a node by different
   amounts, which split the node by position so mitres, lane blocks and
   entries were no longer found (the lane blocks had dropped from 8 / 10
   to 2 / 3 in the Kloten box); the ends meeting at a node now get the
   mean move. Still to do in this slice: lane add / drop generated by
   class (taper lengths, not the drawings); OSM lane counts per stretch
   driving the carriageway's lane dashes and width through a chain; a
   pavement edge that steps at a lane change without a two-leg node; the
   wide pavement left where a block's lane meets a junction patch.
4. **Junctions from lane edges.** Corner radii and stop lines at at-grade
   junctions; T junctions on unsplit roads get the fan, not a union patch.
   Acceptance: the Flughofstrasse crossings around the airport.
5. **Roundabouts complete.** Splitter islands, the ring's lane count from
   its width, entries flared. Acceptance: Balsberg and the Flughofstrasse
   ovals (fit ellipses or two-centre ovals).
6. **One generator.** The design mesher and the bake share the primitives;
   a picked road is edited as a segment with lanes, and the live preview
   equals the bake.

## Measurement

`PVP_DEBUG_STEEP=1` for grades and deck decisions, the step finder (2 m
cells with pavement of two ways 0.35–4 m apart), the buried check (pavement
under the drawn terrain), and driving the car (DriveMode) along the A11,
the rings and the Flughofstrasse crossings before and after every slice.
