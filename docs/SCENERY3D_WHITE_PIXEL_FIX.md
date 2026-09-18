# Scenery3D — White-Pixel Border Fix

## Problem

SWISSIMAGE orthophoto tiles encode non-Swiss territory (e.g. German land at
the CH/DE border) as solid white pixels (RGB 255, 255, 255). Up to 43 % of a
1024² border tile can be white. With first-hit-wins multi-source loading, those
white pixels show through as visible stripes even when a German DOP20 source
covering the same area is configured at a lower priority.

## Solution

When `skip_white_pixels` is enabled (default **true**) and two or more
orthophoto candidate paths are configured, the tile worker thread no longer
returns the first JPEG it finds. Instead it **decodes every candidate** and
**composites** them: any pixel that is still pure white in the running
composite is replaced by the corresponding pixel from the next-lower-priority
source. The composite is then handed to the main thread as raw RGB, skipping a
second JPEG decode.

## Implementation

All changes live in
[provpilot/addons/scenery3d/src/](../provpilot/addons/scenery3d/src/) and the
mirror clone at `/Users/gery/code/scenery3d/src/`.

### Data flow

1. **Worker thread** (`S3DTileManager::worker_func`, `s3d_tile_manager.cpp`)
   - Reads each candidate JPEG file into a `std::vector<uint8_t>`.
   - If `skip_white_pixels` is **off** or only one candidate exists → keeps the
     fast path: stores the raw bytes in `LoadResult::jpeg_bytes` and breaks.
   - Otherwise decodes via `Image::load_jpg_from_buffer` (proven thread-safe;
     already used by the chunk decoder), converts to `FORMAT_RGB8`, and merges
     into a running `composite_img`. White pixels (255, 255, 255) in the
     composite are overwritten by the candidate's pixel.
   - After each merge, samples the composite (stride 16 for ≥4096 px images)
     for any remaining white pixel — if none, breaks early.
   - Writes the final RGB byte buffer to `LoadResult::ortho_rgb` and the side
     length to `LoadResult::ortho_rgb_w`.

2. **Main thread** (`S3DTileManager::_process` result handler):
   - If `result.ortho_rgb` is non-empty → builds the `Image` directly via
     `Image::create_from_data(w, w, false, FORMAT_RGB8, …)`, generates mips,
     uploads. **No second JPEG decode.**
   - Otherwise falls back to the original path: `load_jpg_from_buffer` on
     `result.jpeg_bytes` (single-source / disabled case).

### New / modified properties

| Class            | Property                                    | Default |
|------------------|---------------------------------------------|---------|
| `S3DTileManager` | `bool skip_white_pixels`                    | `true`  |
| `Scenery3D`      | `bool skip_white_pixels` (forwards to mgr)  | `true`  |

Both are bound via `ClassDB::bind_method` + `ADD_PROPERTY` so they appear in
the Godot inspector and round-trip through `.tscn` / `.cfg`.

### New `LoadResult` fields

```cpp
// Pre-decoded composited RGB bytes (used when skip_white_pixels merges
// multiple sources in the worker). When non-empty, jpeg_bytes is ignored.
std::vector<uint8_t> ortho_rgb;
int ortho_rgb_w = 0;
```

### Key code locations

| File                                        | Symbol / region                          |
|---------------------------------------------|------------------------------------------|
| `s3d_tile_manager.h`                        | `skip_white_pixels` member, accessors, `LoadResult::ortho_rgb` |
| `s3d_tile_manager.cpp`                      | composite loop in `worker_func`          |
| `s3d_tile_manager.cpp`                      | `ortho_rgb` branch in `_process`         |
| `scenery3d.h` / `scenery3d.cpp`             | wrapper property, `_ready()` forwards to tile manager |

### GDScript wiring

`SettingsStore::apply_scenery_sources()` (`src/settings_store.cpp`, called from
`node_3d.gd::_apply_scenery_sources()`) reads the bool from settings and pushes
it to the live node:

```cpp
bool skip_white = bool(settings.get("skip_white_pixels", true));
if (scenery3d->has_method("set_skip_white_pixels"))
    scenery3d->call("set_skip_white_pixels", skip_white);
```

## Performance

- Composite mode adds **one extra `load_jpg_from_buffer` per candidate beyond
  the first**, on the worker thread (off the hot path of the renderer).
- The early-exit "any white pixel left?" check is O(N/16) on the composite —
  for a 1024² image that's ~65 k uint8 reads.
- Main thread saves one full JPEG decode in composite mode (decoded image is
  reused via `Image::create_from_data`).
- Single-source tiles and disabled `skip_white_pixels` are unchanged from the
  original first-hit-wins behaviour — zero overhead.

## Limitations

- **Far-distance chunk composite** (mip3, `chunk_ortho_jpegs` path) still does
  per-sub-tile first-hit-wins. White pixels can still appear at long range
  until the camera enters the per-tile mip0/2 ring. Promoting that path to
  white-aware compositing is a separate change.
- Composite skips candidates whose decoded width/height differs from the first
  decoded source. Mixing data with mismatched tile resolutions falls back to
  the first source.
- Pure-white detection is exact (`r==g==b==255`). Near-white pixels (e.g.
  254, 255, 255 from JPEG quantisation) are *not* treated as transparent. This
  is intentional — SWISSIMAGE's masked area is encoded as exact 255s.

## Build

```sh
cd /Users/gery/code/scenery3d
python3 -m SCons precision=double arch=arm64 -j8
cp demo/bin/libscenery3d.macos.template_debug.double.arm64.dylib \
   /Users/gery/code/provpilot/provpilot/bin/
codesign --force --sign - \
   /Users/gery/code/provpilot/provpilot/bin/libscenery3d.macos.template_debug.double.arm64.dylib
```

The `codesign` step is mandatory — without it macOS SIGKILLs the editor at
"Verifying GDExtensions".

## See Also

- [SCENERY_LIBRARY.md](SCENERY_LIBRARY.md) — UI and source-list management
- [SCENERY_ARCHITECTURE.md](SCENERY_ARCHITECTURE.md) — Scenery3D plugin overview
