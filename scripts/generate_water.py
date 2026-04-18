#!/usr/bin/env python3
"""
Water body mesh generator for ProvPilot.

Reads lake and river polygons from swissTLM3D GeoPackage (tlm_bb_bodenbedeckung),
triangulates them, and exports as tiled GLB files with per-vertex colour.

Also reads narrow river/stream LineStrings from tlm_gewaesser_fliessgewaesser
and generates ribbon meshes (same approach as roads).

Usage:
  # Full Switzerland:
  python scripts/generate_water.py \
      --gpkg /path/to/SWISSTLM3D_2025.gpkg \
      --center-e 2682880 --center-n 1283072 \
      --output scenery/water

  # Test area (20 km radius):
  python scripts/generate_water.py \
      --gpkg /path/to/SWISSTLM3D_2025.gpkg \
      --center-e 2682880 --center-n 1283072 \
      --radius 20000 \
      --output scenery/water

Requirements:
  pip install fiona numpy tqdm shapely mapbox-earcut
"""

import argparse
import json
import math
import struct
import sys
from collections import defaultdict
from pathlib import Path

import fiona
import numpy as np
from tqdm import tqdm
from shapely.geometry import LineString, Polygon, MultiPolygon
from shapely.ops import unary_union
from shapely.strtree import STRtree

try:
    import mapbox_earcut
except ImportError:
    sys.exit("mapbox_earcut required: pip install mapbox-earcut")

try:
    from shapely.geometry import Polygon as ShapelyPolygon, box as shapely_box
    from shapely.ops import unary_union
except ImportError:
    sys.exit("shapely required: pip install shapely")


# ── Configuration ────────────────────────────────────────────────────────

TILE_SIZE = 4096  # metres

# Polygon layer and object types
POLYGON_LAYER = "tlm_bb_bodenbedeckung"
LAKE_TYPE = "Stehende Gewaesser"
RIVER_POLY_TYPE = "Fliessgewaesser"

# Stream LineString layer
STREAM_LAYER = "tlm_gewaesser_fliessgewaesser"

# Which stream types to include and their half-widths (metres)
STREAM_WIDTHS = {
    "Fliessgewaesser": 4.0,   # streams/rivers (8 m visible width)
}

# Only include surface-visible streams
STREAM_VERLAUF_INCLUDE = {"Oberirdisch", "Wasserfall", "Bruecke"}

# Vertex colours (RGB 0-1)
WATER_COLORS = {
    "lake":        (0.12, 0.28, 0.45),   # Deep blue
    "river_poly":  (0.15, 0.32, 0.42),   # Slightly lighter blue-green
    "stream":      (0.20, 0.38, 0.50),   # Light blue for narrow streams
}


# ── Polygon Triangulation ───────────────────────────────────────────────

def triangulate_polygon_3d(rings):
    """
    Triangulate a 3D polygon (outer ring + optional inner rings/holes).

    Args:
        rings: List of coordinate rings. First is outer, rest are holes.
               Each ring is [(e, n, z), ...].

    Returns:
        (vertices, triangles) — vertices as Nx3 (E,N,Z), triangles as Mx3 indices.
        Returns (empty, empty) on failure.
    """
    if not rings or len(rings[0]) < 3:
        return np.empty((0, 3)), np.empty((0, 3), dtype=np.uint32)

    # Flatten all rings into a single vertex array,
    # and record ring lengths for earcut
    all_verts = []
    ring_lengths = []
    for ring in rings:
        pts = [(c[0], c[1], c[2] if len(c) >= 3 else 0.0) for c in ring]
        # Remove closing vertex if it duplicates the first
        if len(pts) > 1 and pts[-1][0] == pts[0][0] and pts[-1][1] == pts[0][1]:
            pts = pts[:-1]
        if len(pts) < 3:
            continue
        ring_lengths.append(len(pts))
        all_verts.extend(pts)

    if not all_verts or not ring_lengths or ring_lengths[0] < 3:
        return np.empty((0, 3)), np.empty((0, 3), dtype=np.uint32)

    vertices = np.array(all_verts, dtype=np.float64)

    # Earcut needs Nx2 coords + ring end indices (uint32)
    coords_2d = vertices[:, :2].copy()  # shape (N, 2)

    # Ring end indices: cumulative sum of ring lengths
    ring_end_indices = np.cumsum(ring_lengths).astype(np.uint32)

    try:
        tri_indices = mapbox_earcut.triangulate_float64(coords_2d, ring_end_indices)
    except Exception:
        return np.empty((0, 3)), np.empty((0, 3), dtype=np.uint32)

    if len(tri_indices) == 0:
        return np.empty((0, 3)), np.empty((0, 3), dtype=np.uint32)

    triangles = np.array(tri_indices, dtype=np.uint32).reshape(-1, 3)
    return vertices.astype(np.float64), triangles


# ── Stream Ribbon Mesh (reused from road generator) ─────────────────────

def catmull_rom_resample(pts, spacing):
    """
    Resample a 3D polyline along a centripetal Catmull-Rom spline at
    approximately `spacing` metres between output points. The spline passes
    through every input point, so topology is preserved; intermediate
    samples round off the corners that otherwise produce zigzag ribbons.

    Args:
        pts: (N, 3) float array of (E, N, Z) coordinates.
        spacing: target distance between output samples, in metres.

    Returns:
        (M, 3) float array of resampled coordinates (M >= N).
    """
    n = len(pts)
    if n < 3 or spacing <= 0.0:
        return pts

    # Centripetal Catmull-Rom: duplicate endpoints so every real segment
    # has a control point on either side.
    p = np.empty((n + 2, 3), dtype=np.float64)
    p[0] = pts[0] + (pts[0] - pts[1])
    p[1:-1] = pts
    p[-1] = pts[-1] + (pts[-1] - pts[-2])

    # Knot spacing t_i uses sqrt of chord length (alpha=0.5 = centripetal),
    # which avoids the cusps/loops of uniform Catmull-Rom on uneven data.
    def knot(p0, p1):
        d = np.linalg.norm(p1 - p0)
        return math.sqrt(d) if d > 1e-12 else 1e-6

    out = [pts[0]]
    for i in range(n - 1):
        p0, p1, p2, p3 = p[i], p[i + 1], p[i + 2], p[i + 3]
        t0 = 0.0
        t1 = t0 + knot(p0, p1)
        t2 = t1 + knot(p1, p2)
        t3 = t2 + knot(p2, p3)

        # Number of samples on this segment (excluding the starting point,
        # which was emitted by the previous segment).
        seg_len = np.linalg.norm(p2 - p1)
        steps = max(1, int(math.ceil(seg_len / spacing)))

        for s in range(1, steps + 1):
            u = s / steps
            t = t1 + (t2 - t1) * u
            # Barry-Goldman pyramid evaluation.
            a1 = (t1 - t) / (t1 - t0) * p0 + (t - t0) / (t1 - t0) * p1
            a2 = (t2 - t) / (t2 - t1) * p1 + (t - t1) / (t2 - t1) * p2
            a3 = (t3 - t) / (t3 - t2) * p2 + (t - t2) / (t3 - t2) * p3
            b1 = (t2 - t) / (t2 - t0) * a1 + (t - t0) / (t2 - t0) * a2
            b2 = (t3 - t) / (t3 - t1) * a2 + (t - t1) / (t3 - t1) * a3
            c = (t2 - t) / (t2 - t1) * b1 + (t - t1) / (t2 - t1) * b2
            out.append(c)

    return np.asarray(out, dtype=np.float64)


def linestring_to_ribbon(coords, half_width):
    """
    Convert a LineString to a ribbon mesh (two quads per segment).

    The input polyline is first resampled along a centripetal Catmull-Rom
    spline so that curvy streams render as smooth ribbons instead of the
    zigzag that results from connecting sparse TLM3D vertices directly.

    Returns:
        (vertices, triangles) — vertices Nx3 (E,N,Z), triangles Mx3 indices.
    """
    if len(coords) < 2:
        return np.empty((0, 3)), np.empty((0, 3), dtype=np.uint32)

    raw = np.array([(c[0], c[1], c[2] if len(c) >= 3 else 0.0) for c in coords],
                   dtype=np.float64)

    # Target a sample roughly every half-width: dense enough that a quad's
    # long edge is never much longer than its width, which is what keeps
    # bends smooth without exploding the triangle count.
    spacing = max(2.0, half_width)
    pts = catmull_rom_resample(raw, spacing)
    n = len(pts)
    if n < 2:
        return np.empty((0, 3)), np.empty((0, 3), dtype=np.uint32)

    # Direction vectors in E-N plane
    directions = np.diff(pts[:, :2], axis=0)
    lengths = np.linalg.norm(directions, axis=1, keepdims=True)
    lengths = np.where(lengths < 1e-10, 1.0, lengths)
    directions /= lengths

    # Per-vertex averaged tangent
    tangents = np.zeros((n, 2), dtype=np.float64)
    tangents[0] = directions[0]
    tangents[-1] = directions[-1]
    for i in range(1, n - 1):
        t = directions[i - 1] + directions[i]
        tl = np.linalg.norm(t)
        tangents[i] = t / tl if tl > 1e-10 else directions[i]

    # Perpendicular (normal in E-N plane)
    normals_2d = np.column_stack([-tangents[:, 1], tangents[:, 0]])

    # Left and right vertices
    left = pts.copy()
    right = pts.copy()
    left[:, 0] += normals_2d[:, 0] * half_width
    left[:, 1] += normals_2d[:, 1] * half_width
    right[:, 0] -= normals_2d[:, 0] * half_width
    right[:, 1] -= normals_2d[:, 1] * half_width

    # Interleave: left0, right0, left1, right1, ...
    vertices = np.empty((2 * n, 3), dtype=np.float64)
    vertices[0::2] = left
    vertices[1::2] = right

    # Two triangles per quad
    tris = []
    for i in range(n - 1):
        v0 = 2 * i
        v1 = v0 + 1
        v2 = v0 + 2
        v3 = v0 + 3
        tris.append([v0, v2, v1])
        tris.append([v1, v2, v3])

    triangles = np.array(tris, dtype=np.uint32)
    return vertices, triangles


# ── GLB Creation (same as road generator) ───────────────────────────────

def create_glb(vertices, normals, triangles, colors=None, name="water"):
    """Create a minimal glTF 2.0 JSON structure with optional vertex colors."""
    n_verts = len(vertices)
    n_tris = len(triangles)
    has_colors = colors is not None and len(colors) > 0

    # Buffer layout: indices | positions | normals | colors (optional)
    idx_bytes = n_tris * 3 * 4
    pos_bytes = n_verts * 3 * 4
    norm_bytes = n_verts * 3 * 4
    col_bytes = n_verts * 4 if has_colors else 0  # RGBA as unsigned bytes

    # Pad to 4-byte alignment
    idx_pad = (4 - idx_bytes % 4) % 4
    pos_pad = (4 - pos_bytes % 4) % 4
    norm_pad = (4 - norm_bytes % 4) % 4

    pos_offset = idx_bytes + idx_pad
    norm_offset = pos_offset + pos_bytes + pos_pad
    col_offset = norm_offset + norm_bytes + (norm_pad if has_colors else 0)
    total_bytes = col_offset + col_bytes if has_colors else norm_offset + norm_bytes

    # Compute bounds
    pos_min = vertices.min(axis=0).tolist()
    pos_max = vertices.max(axis=0).tolist()

    attributes = {"POSITION": 1, "NORMAL": 2}
    if has_colors:
        attributes["COLOR_0"] = 3

    accessors = [
        {
            "bufferView": 0,
            "componentType": 5125,
            "count": n_tris * 3,
            "type": "SCALAR",
        },
        {
            "bufferView": 1,
            "componentType": 5126,
            "count": n_verts,
            "type": "VEC3",
            "min": pos_min,
            "max": pos_max,
        },
        {
            "bufferView": 2,
            "componentType": 5126,
            "count": n_verts,
            "type": "VEC3",
        },
    ]

    buffer_views = [
        {"buffer": 0, "byteOffset": 0, "byteLength": idx_bytes, "target": 34963},
        {"buffer": 0, "byteOffset": pos_offset, "byteLength": pos_bytes, "target": 34962},
        {"buffer": 0, "byteOffset": norm_offset, "byteLength": norm_bytes, "target": 34962},
    ]

    if has_colors:
        accessors.append({
            "bufferView": 3,
            "componentType": 5121,
            "normalized": True,
            "count": n_verts,
            "type": "VEC4",
        })
        buffer_views.append({
            "buffer": 0, "byteOffset": col_offset, "byteLength": col_bytes,
            "target": 34962,
        })

    gltf = {
        "asset": {"version": "2.0", "generator": "ProvPilot water generator"},
        "scene": 0,
        "scenes": [{"name": name, "nodes": [0]}],
        "nodes": [{"name": name, "mesh": 0}],
        "meshes": [{
            "name": name,
            "primitives": [{
                "attributes": attributes,
                "indices": 0,
                "mode": 4,
            }]
        }],
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": total_bytes}],
    }

    return gltf, total_bytes


def write_glb(gltf_dict, buffer_data, output_path):
    """Write a binary GLB file."""
    json_str = json.dumps(gltf_dict, separators=(",", ":"))
    json_bytes = json_str.encode("utf-8")
    # Pad JSON to 4 bytes
    json_pad = (4 - len(json_bytes) % 4) % 4
    json_bytes += b" " * json_pad
    # Pad binary to 4 bytes
    bin_pad = (4 - len(buffer_data) % 4) % 4
    buffer_data_padded = buffer_data + b"\x00" * bin_pad

    total = 12 + 8 + len(json_bytes) + 8 + len(buffer_data_padded)

    with open(output_path, "wb") as f:
        # Header
        f.write(struct.pack("<4sII", b"glTF", 2, total))
        # JSON chunk
        f.write(struct.pack("<II", len(json_bytes), 0x4E4F534A))
        f.write(json_bytes)
        # BIN chunk
        f.write(struct.pack("<II", len(buffer_data_padded), 0x004E4942))
        f.write(buffer_data_padded)


# ── Coordinate Transforms ───────────────────────────────────────────────

def transform_to_godot(vertices, origin_e, origin_n):
    """
    Transform LV95 coordinates to Godot space.
      X = origin_e - swiss_e  (positive = West)
      Y = elevation (swiss_z)
      Z = swiss_n - origin_n  (positive = North)
    """
    result = np.zeros_like(vertices, dtype=np.float32)
    result[:, 0] = origin_e - vertices[:, 0]
    result[:, 1] = vertices[:, 2]
    result[:, 2] = vertices[:, 1] - origin_n
    return result


def get_tile_key(e, n, tile_size):
    """Get tile grid key for a coordinate."""
    te = int(math.floor(e / tile_size)) * tile_size
    tn = int(math.floor(n / tile_size)) * tile_size
    return (te, tn)


# ── Feature Reading ─────────────────────────────────────────────────────

def _coords_in_bbox(coords, bbox):
    """Check if any coordinate falls inside the bounding box."""
    min_e, min_n, max_e, max_n = bbox
    for c in coords:
        if min_e <= c[0] <= max_e and min_n <= c[1] <= max_n:
            return True
    return False


def read_polygon_features(gpkg_path, bbox=None):
    """
    Read lake and river polygon features from tlm_bb_bodenbedeckung.

    Returns list of (rings, water_type) where:
        rings: list of coordinate rings [(e,n,z), ...]
        water_type: "lake" or "river_poly"
    """
    features = []
    target_types = {LAKE_TYPE: "lake", RIVER_POLY_TYPE: "river_poly"}

    with fiona.open(gpkg_path, layer=POLYGON_LAYER) as src:
        total = len(src)
        for feat in tqdm(src, total=total, desc="Reading water polygons"):
            art = feat["properties"].get("objektart", "")
            if art not in target_types:
                continue

            geom = feat["geometry"]
            if geom is None or geom["type"] != "Polygon":
                continue

            rings = geom["coordinates"]
            if not rings or len(rings[0]) < 3:
                continue

            # Bbox filter
            if bbox is not None:
                if not _coords_in_bbox(rings[0], bbox):
                    continue

            water_type = target_types[art]
            features.append((rings, water_type))

    return features


def read_stream_features(gpkg_path, bbox=None):
    """
    Read narrow river/stream LineStrings from tlm_gewaesser_fliessgewaesser.

    Returns list of (coords, half_width, water_type).
    """
    features = []

    with fiona.open(gpkg_path, layer=STREAM_LAYER) as src:
        total = len(src)
        for feat in tqdm(src, total=total, desc="Reading streams"):
            art = feat["properties"].get("objektart", "")
            if art not in STREAM_WIDTHS:
                continue

            verlauf = feat["properties"].get("verlauf", "")
            if verlauf not in STREAM_VERLAUF_INCLUDE:
                continue

            geom = feat["geometry"]
            if geom is None or geom["type"] != "LineString":
                continue

            coords = geom["coordinates"]
            if len(coords) < 2:
                continue

            if bbox is not None:
                if not _coords_in_bbox(coords, bbox):
                    continue

            half_width = STREAM_WIDTHS[art]
            features.append((coords, half_width, "stream"))

    return features


# ── Tile Binning ─────────────────────────────────────────────────────────

def bin_polygons_to_tiles(features, tile_size):
    """
    Bin polygon features to tiles. Large polygons are clipped to tile boundaries.

    Args:
        features: List of (rings, water_type)
        tile_size: Tile grid size in metres.

    Returns:
        dict: {tile_key: [(clipped_rings, water_type), ...]}
    """
    tiles = defaultdict(list)

    for rings, water_type in tqdm(features, desc="Binning polygons to tiles"):
        outer = rings[0]
        holes = rings[1:] if len(rings) > 1 else []

        # Build shapely polygon for intersection
        try:
            outer_coords = [(c[0], c[1]) for c in outer]
            hole_coords = [[(c[0], c[1]) for c in h] for h in holes]
            poly = ShapelyPolygon(outer_coords, hole_coords)
            if not poly.is_valid:
                poly = poly.buffer(0)
            if poly.is_empty:
                continue
        except Exception:
            continue

        # Build Z lookup from all original 3D vertices for nearest-neighbor
        # interpolation after clipping (Shapely discards Z).
        orig_coords_3d = []
        for ring in rings:
            for c in ring:
                orig_coords_3d.append((c[0], c[1], c[2] if len(c) >= 3 else 0.0))
        orig_en = np.array([(c[0], c[1]) for c in orig_coords_3d], dtype=np.float64)
        orig_z  = np.array([c[2] for c in orig_coords_3d], dtype=np.float64)

        # Determine all tiles this polygon overlaps
        minx, miny, maxx, maxy = poly.bounds
        min_te = int(math.floor(minx / tile_size)) * tile_size
        min_tn = int(math.floor(miny / tile_size)) * tile_size
        max_te = int(math.floor(maxx / tile_size)) * tile_size
        max_tn = int(math.floor(maxy / tile_size)) * tile_size

        for te in range(min_te, max_te + tile_size, tile_size):
            for tn in range(min_tn, max_tn + tile_size, tile_size):
                tile_key = (te, tn)
                tile_box = shapely_box(te, tn, te + tile_size, tn + tile_size)

                try:
                    clipped = poly.intersection(tile_box)
                except Exception:
                    continue

                if clipped.is_empty:
                    continue

                # Extract polygon(s) from clipped result
                clipped_polys = []
                if clipped.geom_type == "Polygon":
                    clipped_polys = [clipped]
                elif clipped.geom_type == "MultiPolygon":
                    clipped_polys = list(clipped.geoms)
                elif clipped.geom_type == "GeometryCollection":
                    clipped_polys = [g for g in clipped.geoms
                                     if g.geom_type == "Polygon"]

                for cp in clipped_polys:
                    if cp.is_empty or cp.area < 1.0:  # skip tiny slivers
                        continue
                    # Reconstruct 3D rings with nearest-neighbor Z
                    clipped_rings = []
                    for ring_2d in [list(cp.exterior.coords)] + \
                                   [list(h.coords) for h in cp.interiors]:
                        ring_3d = []
                        for c in ring_2d:
                            # Find nearest original vertex and use its Z
                            dists = (orig_en[:, 0] - c[0])**2 + \
                                    (orig_en[:, 1] - c[1])**2
                            z = float(orig_z[np.argmin(dists)])
                            ring_3d.append((c[0], c[1], z))
                        clipped_rings.append(ring_3d)

                    tiles[tile_key].append((clipped_rings, water_type))

    return dict(tiles)


def bin_streams_to_tiles(features, tile_size):
    """
    Bin stream LineString features to tiles by first coordinate.
    """
    tiles = defaultdict(list)
    for coords, half_width, water_type in features:
        # Use midpoint for tile assignment
        mid = len(coords) // 2
        c = coords[mid]
        key = get_tile_key(c[0], c[1], tile_size)
        tiles[key].append((coords, half_width, water_type))
    return dict(tiles)


# ── Tile GLB Generation ─────────────────────────────────────────────────

def generate_tile_glb(poly_features, stream_features, tile_key,
                      origin_e, origin_n, output_dir):
    """
    Generate a single GLB tile from polygon and stream features.
    """
    all_vertices = []
    all_triangles = []
    all_colors = []
    vertex_offset = 0
    poly_count = 0
    stream_count = 0

    # Process polygon features (lakes, wide rivers)
    if poly_features:
        for rings, water_type in poly_features:
            verts, tris = triangulate_polygon_3d(rings)
            if len(verts) == 0:
                continue

            tris = tris + vertex_offset
            all_vertices.append(verts)
            all_triangles.append(tris)

            rgb = WATER_COLORS.get(water_type, (0.15, 0.30, 0.45))
            seg_colors = np.full((len(verts), 4), 255, dtype=np.uint8)
            seg_colors[:, 0] = int(rgb[0] * 255)
            seg_colors[:, 1] = int(rgb[1] * 255)
            seg_colors[:, 2] = int(rgb[2] * 255)
            all_colors.append(seg_colors)

            vertex_offset += len(verts)
            poly_count += 1

    # Process stream features (ribbon meshes)
    if stream_features:
        for coords, half_width, water_type in stream_features:
            verts, tris = linestring_to_ribbon(coords, half_width)
            if len(verts) == 0:
                continue

            tris = tris + vertex_offset
            all_vertices.append(verts)
            all_triangles.append(tris)

            rgb = WATER_COLORS.get(water_type, (0.20, 0.38, 0.50))
            seg_colors = np.full((len(verts), 4), 255, dtype=np.uint8)
            seg_colors[:, 0] = int(rgb[0] * 255)
            seg_colors[:, 1] = int(rgb[1] * 255)
            seg_colors[:, 2] = int(rgb[2] * 255)
            all_colors.append(seg_colors)

            vertex_offset += len(verts)
            stream_count += 1

    total_features = poly_count + stream_count
    if total_features == 0:
        return None

    # Merge
    vertices = np.vstack(all_vertices)
    triangles = np.vstack(all_triangles)
    colors = np.vstack(all_colors)

    # Transform to Godot
    godot_verts = transform_to_godot(vertices, origin_e, origin_n)

    # Normals: all Y-up
    normals = np.zeros_like(godot_verts, dtype=np.float32)
    normals[:, 1] = 1.0

    # Create GLB
    tile_e, tile_n = tile_key
    tile_name = f"water_{tile_e}_{tile_n}"
    glb_name = f"{tile_name}.glb"

    gltf, total_bytes = create_glb(godot_verts, normals, triangles,
                                    colors=colors, name=tile_name)

    # Build buffer
    buf = bytearray(total_bytes)

    idx_flat = triangles.flatten().astype(np.uint32)
    buf[:len(idx_flat) * 4] = idx_flat.tobytes()

    pos_off = gltf["bufferViews"][1]["byteOffset"]
    pos_data = godot_verts.astype(np.float32).tobytes()
    buf[pos_off:pos_off + len(pos_data)] = pos_data

    norm_off = gltf["bufferViews"][2]["byteOffset"]
    norm_data = normals.astype(np.float32).tobytes()
    buf[norm_off:norm_off + len(norm_data)] = norm_data

    if len(gltf["bufferViews"]) > 3:
        col_off = gltf["bufferViews"][3]["byteOffset"]
        col_data = colors.astype(np.uint8).tobytes()
        buf[col_off:col_off + len(col_data)] = col_data

    output_dir.mkdir(parents=True, exist_ok=True)
    output_path = output_dir / glb_name
    write_glb(gltf, bytes(buf), output_path)

    # Compute centre from original Swiss coordinates
    center_e = int(round(vertices[:, 0].mean()))
    center_n = int(round(vertices[:, 1].mean()))

    return {
        "file": glb_name,
        "tile_e": tile_e,
        "tile_n": tile_n,
        "center_e": center_e,
        "center_n": center_n,
        "polygons": poly_count,
        "streams": stream_count,
        "vertices": len(vertices),
    }


# ── Main Pipeline ────────────────────────────────────────────────────────

def run_water_pipeline(gpkg_path, output_dir, center_e, center_n,
                       tile_size=TILE_SIZE, radius=None):
    """Run the full water generation pipeline."""

    output_dir = Path(output_dir)

    # Bounding box
    bbox = None
    if radius:
        bbox = (center_e - radius, center_n - radius,
                center_e + radius, center_n + radius)
        print(f"Bounding box: E=[{bbox[0]}, {bbox[2]}], N=[{bbox[1]}, {bbox[3]}]")

    origin_e = float(center_e)
    origin_n = float(center_n)

    # Step 1: Read polygon features
    print("\nStep 1a: Reading water polygon features...")
    poly_features = read_polygon_features(gpkg_path, bbox=bbox)
    n_lakes = sum(1 for _, t in poly_features if t == "lake")
    n_river_polys = sum(1 for _, t in poly_features if t == "river_poly")
    print(f"  → {n_lakes} lake polygons, {n_river_polys} river polygons")

    # Step 1b: Read stream features
    print("\nStep 1b: Reading stream features...")
    stream_features = read_stream_features(gpkg_path, bbox=bbox)
    print(f"  → {len(stream_features)} stream segments")

    # Step 1c: Drop stream centerlines that fall inside a river polygon.
    # Otherwise the darker ribbon renders on top of the lighter river surface
    # and shows up as a dark "median" line down wide rivers.
    river_polys = []
    for rings, water_type in poly_features:
        if water_type != "river_poly":
            continue
        try:
            poly = Polygon(
                [(c[0], c[1]) for c in rings[0]],
                [[(c[0], c[1]) for c in ring] for ring in rings[1:]],
            )
            if poly.is_valid and not poly.is_empty:
                river_polys.append(poly)
        except Exception:
            continue

    if river_polys and stream_features:
        tree = STRtree(river_polys)
        kept = []
        dropped = 0
        for coords, half_width, water_type in tqdm(stream_features,
                                                    desc="Filtering streams"):
            line = LineString([(c[0], c[1]) for c in coords])
            # Candidates whose bbox intersects the line's bbox.
            cand_idx = tree.query(line)
            if len(cand_idx) == 0:
                kept.append((coords, half_width, water_type))
                continue
            inside_len = 0.0
            line_len = line.length
            for i in cand_idx:
                poly = river_polys[int(i)]
                if not line.intersects(poly):
                    continue
                inside_len += line.intersection(poly).length
                if inside_len >= 0.5 * line_len:
                    break
            if inside_len >= 0.5 * line_len:
                dropped += 1
            else:
                kept.append((coords, half_width, water_type))
        stream_features = kept
        print(f"  → dropped {dropped} stream segments inside river polygons, "
              f"{len(stream_features)} remaining")

    total_features = len(poly_features) + len(stream_features)
    print(f"  Total: {total_features} features")

    if total_features == 0:
        print("No water features found. Done.")
        return

    # Step 2: Spatial binning
    print("\nStep 2: Spatial binning...")
    poly_tiles = bin_polygons_to_tiles(poly_features, tile_size)
    stream_tiles = bin_streams_to_tiles(stream_features, tile_size)

    # Merge tile keys
    all_tile_keys = set(poly_tiles.keys()) | set(stream_tiles.keys())
    print(f"  → {len(all_tile_keys)} tiles")

    # Step 3: Generate GLBs
    print("\nStep 3: Generating GLB tiles...")
    manifest_tiles = {}
    total_verts = 0
    total_bytes = 0

    for tile_key in tqdm(sorted(all_tile_keys), desc="Generating GLBs"):
        pf = poly_tiles.get(tile_key, [])
        sf = stream_tiles.get(tile_key, [])

        result = generate_tile_glb(pf, sf, tile_key,
                                    origin_e, origin_n, output_dir)
        if result is None:
            continue

        tile_id = f"{tile_key[0]}_{tile_key[1]}"
        manifest_tiles[tile_id] = result
        total_verts += result["vertices"]
        glb_path = output_dir / result["file"]
        total_bytes += glb_path.stat().st_size

    print(f"  → {len(manifest_tiles)} GLB files written")

    # Step 4: Write manifest
    print("\nStep 4: Writing manifest...")
    manifest = {
        "format": "glb",
        "source": "swissTLM3D",
        "tile_size_m": tile_size,
        "conversion_origin_e": origin_e,
        "conversion_origin_n": origin_n,
        "tiles": manifest_tiles,
    }

    manifest_path = output_dir / "manifest.json"
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    print(f"\n  Summary:")
    print(f"    Tiles:     {len(manifest_tiles)}")
    print(f"    Lakes:     {n_lakes}")
    print(f"    River polys: {n_river_polys}")
    print(f"    Streams:   {len(stream_features)}")
    print(f"    Vertices:  {total_verts:,}")
    print(f"    Disk:      {total_bytes / 1e6:.1f} MB")
    print(f"    Manifest:  {manifest_path}")


# ── CLI ──────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Generate water body meshes for ProvPilot")
    parser.add_argument("--gpkg", required=True,
                        help="Path to swissTLM3D GeoPackage")
    parser.add_argument("--output", required=True,
                        help="Output directory for GLB tiles")
    parser.add_argument("--center-e", type=float, default=2682880.0,
                        help="Terrain center easting (LV95)")
    parser.add_argument("--center-n", type=float, default=1283072.0,
                        help="Terrain center northing (LV95)")
    parser.add_argument("--tile-size", type=int, default=TILE_SIZE,
                        help=f"Tile grid size in metres (default: {TILE_SIZE})")
    parser.add_argument("--radius", type=float, default=None,
                        help="Optional radius filter (metres from centre)")

    args = parser.parse_args()

    run_water_pipeline(
        gpkg_path=args.gpkg,
        output_dir=args.output,
        center_e=args.center_e,
        center_n=args.center_n,
        tile_size=args.tile_size,
        radius=args.radius,
    )


if __name__ == "__main__":
    main()
