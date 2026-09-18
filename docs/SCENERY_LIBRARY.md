# Scenery Library (FSX-style)

ProvPilot manages multiple scenery data sources through an FSX-style **Scenery
Library** in the Settings screen. Each entry is a *region directory* containing
one or more asset subfolders; entries are ranked by priority and can be toggled
on/off independently.

## UI

Open **Settings → Scenery Library**. The table has these columns:

| On | Pri | Name | Assets | Path |
|----|-----|------|--------|------|
| ✓  | 1   | Switzerland           | terrain, orthophoto, buildings, water, roads, vegetation, trees, airports | `/Users/gery/provpilot/scenery/Switzerland` |
| ✓  | 2   | Germany/Baden-Wuertemberg (terrain) | terrain | `/Users/gery/provpilot/scenery/Germany/Baden-Wuertemberg` |
| ✓  | 3   | Germany/Baden-Wuertemberg (ortho)   | orthophoto | `/Volumes/Data1/scenery/Germany/Baden-Wuertemberg` |

Buttons:

- **Scan for Scenery** — recursively searches the configured scan roots (default
  `/Users/gery/provpilot/scenery` and `/Volumes/Data1/scenery`) for directories
  containing any of the recognised asset subdirs. New entries are sorted by
  asset count descending, so a fully-stocked region (Switzerland: 8 assets)
  outranks partial regions.
- **Add Area…** — pick any directory manually.
- **Move Up / Move Down** — change priority within the table.
- **Remove** — delete the selected entry.

## Region Convention

Each entry's `path` is a region directory. Recognised asset subdirectories
(`SettingsStore::ASSET_SUBDIRS` in `src/settings_store.cpp`):

```
terrain/  orthophoto/  buildings/  water/  roads/  vegetation/  trees/  airports/
```

The same logical region can appear twice from different volumes — e.g.
Baden-Württemberg's terrain on the internal SSD and its orthophotos on
`/Volumes/Data1`. Both are listed separately and contribute their respective
assets when both are enabled.

## Persistence

Stored in `user://settings.cfg` under the `[scenery]` section:

```ini
[scenery]
scenery_areas = [{ "name": "...", "path": "...", "enabled": true, "priority": 1 }, ...]
scenery_scan_roots = PackedStringArray("/Users/gery/provpilot/scenery", "/Volumes/Data1/scenery")
skip_white_pixels = true
```

On macOS this resolves to
`~/Library/Application Support/Godot/app_userdata/ProVPilot/settings.cfg`.

## Download Scenery (in-app)

**Settings → Download Scenery** offers one-click installation of the official
ProvPilot scenery packs hosted at
`https://www.md80.ch/provpilot/scenery/`. The panel is implemented by
[provpilot/scenery_downloader.gd](../provpilot/scenery_downloader.gd) and
mounted as a section of `SettingsScreen`.

UI flow:

1. **Manifest URL** — defaults to
   `https://www.md80.ch/provpilot/scenery_manifest.json`. Hit **Refresh** to
   re-fetch.
2. **Install Root** — defaults to `InstallRoot.get_scenery_root()` if set,
   else `<install>/scenery`. **Browse…** picks any directory; the chosen root
   is *not* persisted by the panel itself — it's just the extraction target
   for this session.
3. Each region in the manifest shows one **base** row plus four **detail**
   rows (`nw/ne/sw/se` quadrants). Per pack you see size, install status, and
   a **Download / Update / Reinstall** button; installed packs also get a
   **Forget** button (drops state without deleting files).
4. After a successful install the panel calls `on_area_installed(path)`,
   which `SettingsScreen` wires to `_add_scenery_area()` — the new region
   region root shows up automatically in the Scenery Library above.

Pipeline per pack:

- HTTPRequest streams the zip to
  `<install_root>/.downloads/<pack_id>.zip` (threaded, no size limit).
- Size + sha256 are verified against the manifest entry (chunked HashingContext
  with per-frame yields so the UI stays responsive).
- ZIPReader extracts every entry into `<install_root>/`, yielding every 32
  files. The top-level region directory (e.g. `Switzerland/` or
  `Germany/Baden-Wuertemberg/`) is detected automatically by walking the
  single-child chain until an asset subdir is found.
- The zip is deleted on success.
- State is written into `settings["installed_scenery_packs"][pack_id]` =
  `{version, sha256, size, path, installed_at}`. The manifest URL doubles as
  the update check: a sha256 mismatch turns the row into "Update available".

The panel does *not* manage the **Scenery Library** ranking — that's the
user's job in the table above. Installs are additive; uninstalls are manual
(rm -rf the region directory + click **Forget**).

## Runtime Plumbing

`node_3d.gd::_apply_scenery_sources()` is called in `_ready()` and again on
settings close; it forwards to the C++ `SettingsStore::apply_scenery_sources()`
(`src/settings_store.cpp`), which:

1. Takes `get_enabled_scenery_areas()` — the priority-sorted enabled list.
2. For each area, probes `<path>/terrain`, `<path>/orthophoto`,
   `<path>/buildings`, `<path>/water`, `<path>/roads`, `<path>/vegetation` and
   collects the existing dirs into per-asset `PackedStringArray`s.
3. Assigns to the `Scenery3D` node:
   - `data_paths` (terrain)
   - `orthophoto_paths`
   - `water_paths`
   - `road_paths`
   - `buildings_path` (first only — single-path API)
4. Assigns `vegetation_paths` to the `Vegetation` (`S3DVegetationManager`) node.
5. Forwards `skip_white_pixels` via `scenery3d_node.set_skip_white_pixels()`.

> **Important — do not also set the singular `data_path` / `orthophoto_path`
> properties.** Their C++ setters call `data_paths.clear()` /
> `orthophoto_paths.clear()` first, which silently drops every path except the
> one being assigned. `_apply_scenery_sources()` only ever assigns the array
> properties.

## CH/DE Border White-Pixel Fix

SWISSIMAGE encodes non-Swiss territory (e.g. German land) as solid white
(255,255,255) — up to 43 % of a 1024² border tile. The `skip_white_pixels`
toggle (default **on**) tells the C++ tile worker to composite multiple
orthophoto sources at decode time: white pixels in higher-priority sources are
filled in from lower-priority ones.

See [SCENERY3D_WHITE_PIXEL_FIX.md](SCENERY3D_WHITE_PIXEL_FIX.md) for the C++
implementation.

## Adding a New Region

1. Drop the data into a region directory using the standard subdir layout, e.g.

   ```
   /Volumes/Data1/scenery/Austria/Vorarlberg/
   ├── terrain/
   ├── orthophoto/
   └── buildings/
   ```

2. Make sure the parent directory is in `scenery_scan_roots` (edit
   `settings.cfg` or use the config UI).
3. Open **Settings → Scenery Library**, click **Scan for Scenery**.
4. Adjust priority if needed and tick **On**.
5. Restart the level (or just close settings — `_apply_scenery_sources()` runs
   on close).

## See Also

- [SCENERY3D_WHITE_PIXEL_FIX.md](SCENERY3D_WHITE_PIXEL_FIX.md) — multi-source
  orthophoto compositing in C++
- [SCENERY_ARCHITECTURE.md](SCENERY_ARCHITECTURE.md) — Scenery3D plugin overview
- [TERRAIN_SETUP.md](TERRAIN_SETUP.md) — data file format and tile naming
