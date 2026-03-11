# ProvPilot Scenery Development Plan — Clipmap Terrain

**Created**: March 2026
**Status**: In Progress (Phases 0–6 complete)
**Goal**: Eliminate far/near scenery overlap artifacts and achieve seamless terrain from cockpit to 300 km horizon.

---

## Background

### Problem

After fixing simulator pausing during floating-origin rebase, a new artifact appeared: **far scenery overlapping near scenery**. Land appears over lakes, phantom mountains show where they shouldn't. This is caused by a resolution mismatch between two independent terrain systems:

| System | Resolution | Coverage |
|--------|-----------|----------|
| Terrain3D (near) | 1m/pixel, 1024×1024 regions | ~17×17 km (load_radius=8) |
| DistantTerrain (far) | 256m/pixel, single mesh | All of Switzerland |

At 256m resolution, a single pixel averages mountains and valleys. When both systems overlap near the boundary (~7–8.5 km), the distant terrain's phantom geometry wins the depth test.

### MSFS 2020 Precedent ("Scenery Morphing")

MSFS solved this with two techniques:

1. **Geomorphing**: Vertices smoothly interpolate between LOD levels (no popping)
2. **Unified terrain pipeline**: Near and far terrain share the same clipmap structure at varying resolution

### Key Insight: Terrain3D Already Has a Clipmap

Terrain3D is a **Geomorphing Geometric Clipmap** (same technique as The Witcher 3). With `mesh_lods=8`, it provides 8 concentric LOD rings with smooth geomorphing transitions. The mesh infrastructure for continuous LOD is already solved internally.

**However**, Terrain3D has three hard limits that prevent it from covering the full horizon:

1. **Fixed pixel resolution** — All regions are 1024×1024 px. Can't use lower resolution for distant regions. A far-away LOD7 region (128m vertex spacing) still consumes a full 4 MB heightmap.
2. **32×32 region grid limit** — Max 32,768m per side. Flight sims need 100–300 km of visible horizon.
3. **Memory budget** — 289 regions × ~8 MB (height + color) ≈ 2.3 GB VRAM. Full 1024 regions would need ~8 GB.

---

## Architecture: Hybrid Clipmap

Keep Terrain3D for the inner ~8.5 km (where it excels), add custom clipmap rings beyond.

```
                  ~300 km horizon
  ┌─────────────────────────────────────────┐
  │       Ring 2: custom mesh, 256 m/vtx    │
  │  ┌───────────────────────────────────┐  │
  │  │    Ring 1: custom mesh, 64 m/vtx  │  │  ~120 km
  │  │  ┌─────────────────────────────┐  │  │
  │  │  │   Ring 0: custom, 16 m/vtx  │  │  │  ~50 km
  │  │  │  ┌───────────────────────┐  │  │  │
  │  │  │  │                       │  │  │  │
  │  │  │  │     TERRAIN 3D        │  │  │  │  ~8.5 km
  │  │  │  │  (8 LODs, geomorphing │  │  │  │  289 regions
  │  │  │  │   1m → 128m mesh)     │  │  │  │  Already a clipmap.
  │  │  │  │                       │  │  │  │
  │  │  │  └───────────────────────┘  │  │  │
  │  │  └─────────────────────────────┘  │  │
  │  └───────────────────────────────────┘  │
  └─────────────────────────────────────────┘
```

3 custom rings replace `DistantTerrain`. Each ring: ~30–50K vertices. Total: ~120K vertices (trivial for GPU).

### Comparison with Original (Full Clipmap) Plan

| Aspect | Full clipmap (original) | Hybrid (revised) |
|--------|------------------------|-------------------|
| Inner terrain | Custom clipmap 0–8.5 km | **Keep T3D as-is** (already a clipmap!) |
| Seam work | Match T3D outermost LOD | T3D LOD7 = 128m → Ring 0 at 16m aligns trivially |
| Custom rings | 4 (0–300 km) | **3** (8.5–300 km only) |
| Geomorphing shader | Write from scratch for all rings | Only for the 3 outer rings; T3D handles its own |
| Risk | Replaces T3D inner terrain | T3D untouched — zero risk to existing functionality |

---

## Phase 0 — Fix Immediate Overlap (quick fix)

**Goal**: Stop visible artifacts now while building the replacement.

Adjust `distant_terrain.gd` / `shaders/distant_terrain.gdshader` clip parameters:

| Parameter | Current | New | Reason |
|-----------|---------|-----|--------|
| near_clip | 7000 | 9500 | Clear T3D's 8.2 km edge + margin |
| near_fade_width | 6000 | 4000 | Tighter fade (starting further out) |
| height_blend_range | 8000 | 6000 | Faster height transition |
| flatten_depth | 500 | 800 | Push flat zone deeper below camera |

**Files**: `distant_terrain.gd`, `shaders/distant_terrain.gdshader`

---

## Phase 1 — Multi-Resolution Heightmap Pyramid (offline tool)

**New script**: `scripts/generate_terrain_mipmap.py`

Reads the 42,400 source tiles (1m) and generates lower-resolution tile pyramids:

| Level | Resolution | Tile size (px) | File size/tile | Ring |
|-------|-----------|----------------|----------------|------|
| 0 | 1 m/px | 1024×1024 | 4 MB | Terrain3D (existing) |
| 2 | 16 m/px | 64×64 | 16 KB | Ring 0 |
| 3 | 64 m/px | 16×16 | 1 KB | Ring 1 |
| 4 | 256 m/px | 4×4 | 64 B | Ring 2 |

Level 1 (4m) is skipped — T3D covers that range with its internal LOD.

**Downsampling uses median** to preserve flat surfaces (lakes, valley floors). Matching orthophoto mipmaps also generated (JPEG, progressive downsample).

Output: `scenery/terrain/mip2/`, `scenery/terrain/mip3/`, `scenery/terrain/mip4/` with per-level manifests.

---

## Phase 2 — Clipmap Ring Mesh Generator

**New file**: `provpilot/terrain_clipmap.gd`

3 concentric ring meshes as fixed-topology `ArrayMesh`:

| Ring | Inner radius | Outer radius | Resolution | Approx vertices |
|------|-------------|-------------|-----------|-----------------|
| 0 | 8.5 km | 50 km | 16 m/vtx | ~50K |
| 1 | 50 km | 120 km | 64 m/vtx | ~40K |
| 2 | 120 km | 300 km | 256 m/vtx | ~30K |

Each ring mesh:

- Vertices store **grid UV** — shader computes world position from heightmap texture
- Inner hole matches next-inner ring (or T3D boundary for Ring 0)
- **L-shaped fill pieces** stitch between rings at resolution boundaries
- Pre-generated once at startup (or saved as `.res` for instant load)
- Meshes don't change shape — only heightmap texture data updates

---

## Phase 3 — Clipmap Shader with Geomorphing

**New shader**: `provpilot/shaders/terrain_clipmap.gdshader`

**Vertex shader**:

1. Read grid UV from vertex attribute
2. Compute world XZ from ring offset + grid UV × resolution
3. Sample heightmap texture → "fine" height
4. Sample parent (coarser) heightmap → "coarse" height
5. Compute morph factor from distance to ring boundary
6. `final_height = mix(fine_height, coarse_height, morph_factor)`
7. Apply floating-origin offset

**Fragment shader**:

1. Sample orthophoto color texture
2. Apply atmospheric haze (distance-based, matches existing environment fog)
3. Compute normal from height derivatives (central differences)
4. Basic diffuse lighting from sun direction

**Geomorphing**: Vertices that exist at both fine and coarse levels have matching heights (no morph). Vertices only at the finer level interpolate toward coarse positions as `morph_factor` ramps via `smoothstep` near the outer boundary. Eliminates all popping.

---

## Phase 4 — Terrain3D ↔ Ring 0 Seam ✅ IMPLEMENTED

Ring 0's inner edge must blend with Terrain3D's outer edge.

**Approach**:

1. Ring 0 samples from the **same 1m heightmap tiles** T3D uses at the boundary zone
2. Ring 0's inner-edge vertex spacing = T3D's LOD7 (~128m) — trivial alignment since Ring 0 at 16m has a vertex every 8th position that aligns
3. **Transition band** (1–2 km wide) inside Ring 0 blends height, color, and normals
4. **Depth bias**: Ring 0 renders slightly behind T3D so T3D always wins the depth test in overlap
5. At 8.5 km the seam is in atmospheric haze territory — small mismatches are invisible

---

## Phase 5 — Clipmap Streaming Manager ✅ IMPLEMENTED

**New file**: `provpilot/clipmap_streamer.gd`

Manages heightmap + orthophoto texture lifecycle for clipmap rings:

1. **Centralised texture ownership** — `ClipmapStreamer` (extends RefCounted) owns the per-ring `Image` + `ImageTexture` pairs. `terrain_clipmap.gd` delegates all texture creation and updates to the streamer.
2. **Budget-limited I/O** — at most 8 tiles per frame (`MAX_TILES_PER_FRAME`), inner rings prioritised.
3. **Floating origin rebase** — on every rebase, all ring textures are rebuilt centered on the new position and mesh positions reset to zero. Full rebuild takes <1 ms per ring.
4. **Toroidal texture infrastructure** — strip-blit and toroidal-offset logic is implemented and retained for future use, but currently unused. Each ring's texture covers ±16–262 km while rebase fires at 5 km, so incremental scrolling provides no benefit.
5. **Velocity pre-fetch** — implemented but disabled (same reason: textures are far larger than the rebase zone).
6. **Shader support** — `toroidal_offset` uniform (vec2) added to `terrain_clipmap.gdshader`. Textures use `repeat_enable`. UV wrapped via `fract(uv + toroidal_offset)`. Currently offset stays at zero.
7. **Seam heightmap** — Phase 4 seam texture rebuilt on every rebase via streamer.

Texture mechanics:

- Each ring's heightmap: 2048×2048 `ImageTexture` (FORMAT_RF)
- Full re-upload via `ImageTexture.update(image)` takes <1 ms
- `build_all()` for initial load, `rebase()` for every floating-origin shift

---

## Phase 6 — Integration & DistantTerrain Removal ✅ IMPLEMENTED

1. ~~Remove `distant_terrain.gd` and `shaders/distant_terrain.gdshader`~~ ✅ Deleted
2. ~~Replace `DistantTerrain` node in `node_3d.tscn` with `TerrainClipmap` node~~ ✅ Removed from scene; TerrainClipmap is sole distant terrain
3. ~~Update `_perform_origin_rebase()` in `node_3d.gd` to call `clipmap.rebase_center()`~~ ✅ Already done in Phase 5; dead DistantTerrain references cleaned up
4. ~~Verify orthophoto rendering across all rings~~ ✅ Confirmed in flight testing
5. ~~Tune atmospheric parameters (haze, fog)~~ ✅ Clipmap shader handles fog/haze natively

**Removed files**: `distant_terrain.gd`, `distant_terrain.gd.uid`, `shaders/distant_terrain.gdshader`, `shaders/distant_terrain.gdshader.uid`
**Cleaned up**: Loading progress no longer tracks DistantTerrain chunks; startup progress reweighted to terrain 60% + scenery 40%.

---

## Phase 7 — Polish & Tune

- Fine-tune geomorph transition widths
- Adjust ring boundaries and resolutions based on visual testing
- Profile GPU cost; if Ring 0 (16m) is too dense, try 32m
- Test edge cases: high altitude, low altitude, fast flight, rebase during banking
- Optional: render-distance slider in settings

---

## Implementation Order

```
Phase 0 ──→ (usable sim while building clipmap)

Phase 1 ──→ Phase 2 ──→ Phase 3 ──┬──→ Phase 4 ──┬──→ Phase 6 ──→ Phase 7
                                   └──→ Phase 5 ──┘
```

Phase 4 (seam) and Phase 5 (streaming) are independent and can be developed in parallel.

---

## Risk Mitigation

| Risk | Mitigation |
|------|-----------|
| T3D boundary seam visible | Phase 0 quick-fix stays as fallback; increase overlap zone in Phase 4 |
| Godot lacks partial texture update | Full texture update at 1024² is <1 ms; acceptable |
| Too many draw calls | 3 rings = 3 draw calls; trivial |
| Heightmap pyramid disk size | Levels 2–4 combined ≈ 0.5% of Level 0 size; negligible |
| Geomorphing visible as "jelly" | Widen morph zone; test at cockpit viewing angles (5–15° down) |
| Streaming can't keep up with fast flight | Pre-fetch based on velocity; outer rings change slowly |

---

## What This Eliminates

- **Phantom mountains/lakes**: No more resolution cliff between two independent systems
- **Scenery morphing/popping**: Geomorphing smoothly blends vertex heights
- **Rebase stutter gaps**: Clipmap ring textures shift in-place; no progressive loading gap
- **Hard terrain cutoff**: Seamless terrain from cockpit to 300 km horizon
- **DistantTerrain system**: Entirely replaced (single mesh, separate shader, overlap hacks)
