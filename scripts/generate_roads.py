#!/usr/bin/env python3
"""
Road and railway mesh generator for ProvPilot.

Reads road/railway LineStrings from swissTLMRegio (or swissTLM3D) GeoPackage,
generates flat ribbon meshes as GLB files tiled on a grid, and writes a manifest.

Usage:
  # Test area around Schaffhausen:
  python scripts/generate_roads.py \
      --gpkg /path/to/swissTLMRegio_Product_LV95.gpkg \
      --center-e 2688000 --center-n 1283000 --radius 10000 \
      --output scenery/roads

  # Full Switzerland:
  python scripts/generate_roads.py \
      --gpkg /path/to/swissTLMRegio_Product_LV95.gpkg \
      --output scenery/roads

Requirements:
  pip install fiona numpy tqdm
"""

import argparse
import json
import math
import struct
import sys
from collections import defaultdict
from pathlib import Path

try:
    import fiona
    import numpy as np
    from tqdm import tqdm
except ImportError as e:
    print(f"Missing: {e}. Install: pip install fiona numpy tqdm")
    sys.exit(1)

# ── Configuration ────────────────────────────────────────────────────────

# Road widths (half-width in meters) by objval
# ── swissTLMRegio widths (objval field) ──
ROAD_WIDTHS_REGIO = {
    # Motorways
    "Autobahn":           7.0,
    "Autob_Ri":           5.0,
    "Autobahn_Rampe":     4.0,
    "Autob_Ri_Rampe":     4.0,
    # Expressways
    "Autostr":            5.0,
    "Autostr_Rampe":      3.5,
    # Main roads
    "HauptStrAB6":        3.5,
    "HauptStrAB4":        2.5,
    "HauptStrAB_Rampe":   2.5,
    # Connecting roads
    "VerbindStr6":        3.0,
    "VerbindStr4":        2.0,
    "VerbindStr_Rampe":   2.0,
    # Side roads
    "NebenStr6":          2.5,
    "NebenStr3":          1.5,
    "NebenStr_Rampe":     1.5,
    # Vehicle roads
    "Fahrstraes":         1.5,
}

RAILWAY_WIDTHS_REGIO = {
    "NS_Bahn":        2.0,
    "NS_BahnAuto":    2.5,
    "SS_Bahn":        1.5,
    "SS_BahnAuto":    2.0,
    "MS_Bahn":        1.5,
    "Standseilbahn":  1.0,
}

# ── swissTLM3D widths (objektart field) ──
ROAD_WIDTHS_TLM3D = {
    "Autobahn":       7.0,   # 6146 features
    "Autostrasse":    5.0,   # 1533
    "Einfahrt":       3.5,
    "Ausfahrt":       3.5,
    "Raststaette":    3.0,
    "Zufahrt":        2.5,
    "Dienstzufahrt":  2.0,
    "10m Strasse":    5.0,   # 16256
    "8m Strasse":     4.0,   # 25037
    "6m Strasse":     3.0,   # 127282
    "4m Strasse":     2.0,   # 423195
    "3m Strasse":     1.5,   # 398054
    "Verbindung":     2.0,   # 15959
    "Platz":          4.0,   # 3819
    "Autozug":        3.0,
    # Skip: "2m Weg", "1m Weg", "Markierte Spur", "Klettersteig",
    #        "1m Wegfragment", "2m Wegfragment", "Faehre", "Provisorium"
}

RAILWAY_WIDTHS_TLM3D = {
    "Normalspur":                2.0,   # 42767
    "Schmalspur":                1.5,   # 11906
    "Schmalspur mit Normalspur": 2.0,   # 231
    "Kleinbahn":                 1.0,   # 120
    # Skip Standseilbahn for now (in uebrige_bahn)
}

# Backwards compat alias
ROAD_WIDTHS = ROAD_WIDTHS_REGIO
RAILWAY_WIDTHS = RAILWAY_WIDTHS_REGIO

# Road type → vertex color (RGB floats) for shader differentiation in Godot
ROAD_COLORS = {
    "motorway":  (0.18, 0.18, 0.22),  # Dark blue-grey (motorways)
    "main":      (0.30, 0.30, 0.32),  # Medium grey (main roads)
    "secondary": (0.42, 0.42, 0.42),  # Light grey (secondary/connecting)
    "minor":     (0.52, 0.50, 0.48),  # Warm light grey (minor roads)
    "railway":   (0.30, 0.22, 0.15),  # Dark brown (railways)
}

def classify_road(objval):
    """Map objval/objektart to material class."""
    if objval in ("Autobahn", "Autob_Ri", "Autobahn_Rampe", "Autob_Ri_Rampe",
                  "Autostr", "Autostr_Rampe", "Autostrasse", "Einfahrt",
                  "Ausfahrt", "Raststaette"):
        return "motorway"
    if objval in ("HauptStrAB6", "HauptStrAB4", "HauptStrAB_Rampe",
                  "10m Strasse", "8m Strasse"):
        return "main"
    if objval in ("VerbindStr6", "VerbindStr4", "VerbindStr_Rampe",
                  "6m Strasse", "Platz", "Zufahrt", "Dienstzufahrt",
                  "Verbindung", "Autozug"):
        return "secondary"
    return "minor"


# Tile grid size for spatial partitioning (meters)
ROAD_TILE_SIZE = 4096

# Height offset above terrain to avoid z-fighting (meters)
Z_OFFSET = 0.0

# ── Stripe Configuration ────────────────────────────────────────────────

STRIPE_HALF_WIDTH = 0.12   # 24 cm total stripe width
STRIPE_Z_BUMP = 0.05       # 5 cm above road surface to avoid z-fighting
STRIPE_COLOR = (1.0, 1.0, 1.0)  # White
RAIL_HALF_WIDTH = 0.06     # 12 cm per rail
RAIL_GAUGE_HALF = 0.72     # ~1.44 m gauge (standard = 1.435 m)
RAIL_COLOR = (0.7, 0.7, 0.72)   # Silver/steel


# ── Mesh Generation ─────────────────────────────────────────────────────

def offset_linestring(coords, offset):
    """
    Offset a linestring perpendicular by `offset` meters.
    Positive = left (in the direction of travel).
    """
    n = len(coords)
    if n < 2:
        return coords

    pts = np.array(coords, dtype=np.float64)
    result = np.zeros_like(pts)

    for i in range(n):
        if i == 0:
            dx = pts[1, 0] - pts[0, 0]
            dy = pts[1, 1] - pts[0, 1]
        elif i == n - 1:
            dx = pts[-1, 0] - pts[-2, 0]
            dy = pts[-1, 1] - pts[-2, 1]
        else:
            dx = pts[i + 1, 0] - pts[i - 1, 0]
            dy = pts[i + 1, 1] - pts[i - 1, 1]

        length = math.sqrt(dx * dx + dy * dy)
        if length < 0.001:
            result[i] = pts[i]
        else:
            px = -dy / length * offset
            py = dx / length * offset
            result[i, 0] = pts[i, 0] + px
            result[i, 1] = pts[i, 1] + py
            result[i, 2] = pts[i, 2] if pts.shape[1] > 2 else 0

    return [tuple(r) for r in result]


def linestring_to_ribbon(coords, half_width, z_offset=Z_OFFSET):
    """
    Convert a 3D LineString to a flat ribbon mesh.
    
    Args:
        coords: List of (x, y, z) tuples in LV95
        half_width: Half the road width in meters
        z_offset: Height offset above the LineString Z
        
    Returns:
        vertices: Nx3 float32 array (in LV95 coordinates)
        triangles: Mx3 uint32 array (triangle indices)
    """
    n = len(coords)
    if n < 2:
        return np.empty((0, 3), dtype=np.float32), np.empty((0, 3), dtype=np.uint32)
    
    pts = np.array(coords, dtype=np.float64)
    
    # Compute direction vectors
    # For each point, compute the average direction of adjacent segments
    verts = np.zeros((n * 2, 3), dtype=np.float32)
    
    for i in range(n):
        # Position
        x, y, z = pts[i, 0], pts[i, 1], pts[i, 2] if pts.shape[1] > 2 else 0
        z += z_offset
        
        # Direction: average of previous and next segment
        if i == 0:
            dx = pts[1, 0] - pts[0, 0]
            dy = pts[1, 1] - pts[0, 1]
        elif i == n - 1:
            dx = pts[-1, 0] - pts[-2, 0]
            dy = pts[-1, 1] - pts[-2, 1]
        else:
            dx = pts[i+1, 0] - pts[i-1, 0]
            dy = pts[i+1, 1] - pts[i-1, 1]
        
        # Perpendicular (rotated 90°)
        length = math.sqrt(dx*dx + dy*dy)
        if length < 0.001:
            px, py = 0, 0
        else:
            px = -dy / length * half_width
            py = dx / length * half_width
        
        # Left and right vertices
        verts[i * 2]     = [x + px, y + py, z]  # left
        verts[i * 2 + 1] = [x - px, y - py, z]  # right
    
    # Generate triangles (2 per segment)
    n_tris = (n - 1) * 2
    tris = np.zeros((n_tris, 3), dtype=np.uint32)
    for i in range(n - 1):
        base = i * 2
        # Triangle 1: top-left, top-right, bottom-left
        tris[i * 2]     = [base, base + 1, base + 2]
        # Triangle 2: top-right, bottom-right, bottom-left
        tris[i * 2 + 1] = [base + 1, base + 3, base + 2]
    
    return verts, tris


# ── GLB Writer ──────────────────────────────────────────────────────────

def create_glb(vertices, normals, triangles, colors=None, name="roads"):
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
        # 0: indices
        {
            "bufferView": 0,
            "componentType": 5125,  # UNSIGNED_INT
            "count": n_tris * 3,
            "type": "SCALAR",
        },
        # 1: positions
        {
            "bufferView": 1,
            "componentType": 5126,  # FLOAT
            "count": n_verts,
            "type": "VEC3",
            "min": pos_min,
            "max": pos_max,
        },
        # 2: normals
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
            "componentType": 5121,  # UNSIGNED_BYTE
            "normalized": True,
            "count": n_verts,
            "type": "VEC4",
        })
        buffer_views.append({
            "buffer": 0, "byteOffset": col_offset, "byteLength": col_bytes,
            "target": 34962,
        })
    
    gltf = {
        "asset": {"version": "2.0", "generator": "ProvPilot road generator"},
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


def create_glb_buffer(vertices, normals, triangles, total_bytes):
    """Create the binary buffer for GLB."""
    buf = bytearray(total_bytes)
    
    # Indices
    idx_flat = triangles.flatten().astype(np.uint32)
    idx_bytes = idx_flat.tobytes()
    buf[:len(idx_bytes)] = idx_bytes
    
    # Positions
    gltf_json, _ = create_glb(vertices, normals, triangles)
    pos_offset = gltf_json["bufferViews"][1]["byteOffset"]
    pos_data = vertices.astype(np.float32).tobytes()
    buf[pos_offset:pos_offset + len(pos_data)] = pos_data
    
    # Normals
    norm_offset = gltf_json["bufferViews"][2]["byteOffset"]
    norm_data = normals.astype(np.float32).tobytes()
    buf[norm_offset:norm_offset + len(norm_data)] = norm_data
    
    return bytes(buf)


def write_glb(gltf_json, binary_buffer, output_path):
    """Write a complete GLB file."""
    json_str = json.dumps(gltf_json, separators=(",", ":"))
    json_bytes = json_str.encode("utf-8")
    # Pad JSON to 4-byte alignment
    json_pad = (4 - len(json_bytes) % 4) % 4
    json_bytes += b" " * json_pad
    
    # GLB header
    total_length = 12 + 8 + len(json_bytes) + 8 + len(binary_buffer)
    
    with open(output_path, "wb") as f:
        # Header
        f.write(b"glTF")
        f.write(struct.pack("<I", 2))           # version
        f.write(struct.pack("<I", total_length)) # total length
        
        # JSON chunk
        f.write(struct.pack("<I", len(json_bytes)))
        f.write(struct.pack("<I", 0x4E4F534A))  # "JSON"
        f.write(json_bytes)
        
        # Binary chunk
        f.write(struct.pack("<I", len(binary_buffer)))
        f.write(struct.pack("<I", 0x004E4942))  # "BIN\0"
        f.write(binary_buffer)


def compute_normals(vertices, triangles):
    """Compute per-vertex normals (all face up for flat road strips)."""
    normals = np.zeros_like(vertices, dtype=np.float32)
    normals[:, 2] = 1.0  # Z-up in LV95 space (will be transformed to Y-up in Godot)
    return normals


# ── Coordinate Transform ────────────────────────────────────────────────

def transform_to_godot(vertices, origin_e, origin_n):
    """
    Transform LV95 coordinates to Godot space.
    
    Godot coordinate system:
      X = origin_e - swiss_e  (positive = West)
      Y = elevation (swiss_z)
      Z = swiss_n - origin_n  (positive = North)
    """
    result = np.zeros_like(vertices, dtype=np.float32)
    result[:, 0] = origin_e - vertices[:, 0]   # X = West
    result[:, 1] = vertices[:, 2]               # Y = elevation
    result[:, 2] = vertices[:, 1] - origin_n    # Z = North
    return result


def compute_normals_godot(vertices, triangles):
    """Compute normals in Godot space (Y-up)."""
    normals = np.zeros_like(vertices, dtype=np.float32)
    normals[:, 1] = 1.0  # Y-up
    return normals


# ── Main Pipeline ────────────────────────────────────────────────────────

def get_tile_key(e, n, tile_size=ROAD_TILE_SIZE):
    """Get the tile key for a given LV95 coordinate."""
    te = int(e // tile_size) * tile_size
    tn = int(n // tile_size) * tile_size
    return (te, tn)


def _coords_in_bbox(coords, bbox):
    """Check if any coordinate falls within the bounding box."""
    w, s, e, n = bbox
    for c in coords:
        if w <= c[0] <= e and s <= c[1] <= n:
            return True
    return False


def read_features(gpkg_path, layer_name, bbox=None, skip_tunnels=True,
                  skip_footpaths=True, width_map=None, type_field=None,
                  tunnel_field=None, tunnel_values=None):
    """
    Read features from a GeoPackage layer.
    
    Args:
        gpkg_path: Path to GeoPackage
        layer_name: Layer name to read
        bbox: Optional (west, south, east, north) bounding box in LV95
        skip_tunnels: Skip tunnel sections
        skip_footpaths: Skip footpaths/hiking trails
        width_map: Dict mapping type value to half-width
        type_field: Field name for the road/rail type (auto-detected)
        tunnel_field: Field name for tunnel/bridge info (auto-detected)
        tunnel_values: Set of values that indicate a tunnel
        
    Returns:
        List of (coords, half_width, material_class) tuples
    """
    if width_map is None:
        width_map = ROAD_WIDTHS
    
    # Auto-detect field names from schema
    with fiona.open(gpkg_path, layer=layer_name) as src:
        fields = list(src.schema["properties"].keys())
        
        if type_field is None:
            if "objval" in fields:
                type_field = "objval"           # swissTLMRegio
            elif "objektart" in fields:
                type_field = "objektart"         # swissTLM3D
            else:
                type_field = "objval"
        
        if tunnel_field is None:
            if "construct" in fields:
                tunnel_field = "construct"       # swissTLMRegio
            elif "kunstbaute" in fields:
                tunnel_field = "kunstbaute"      # swissTLM3D
            else:
                tunnel_field = "construct"
        
        if tunnel_values is None:
            tunnel_values = {"Tunnel"}
    
    # Skip values (pedestrian paths etc.)
    skip_types = {
        "Fussweg", "Klettersteig", "Markierte Spur", "Faehre", "Provisorium",
        "1m Weg", "2m Weg", "1m Wegfragment", "2m Wegfragment",
        "Luftseilbahn", "Transportseil", "Skilift", "Sesselbahn",
        "Gondelbahn", "Foerderband", "Lift",
    }
    
    is_railway = "rail" in layer_name.lower() or "bahn" in layer_name.lower()
    
    features = []
    
    with fiona.open(gpkg_path, layer=layer_name) as src:
        total = len(src)
        
        for rec in tqdm(src, total=total, desc=f"Reading {layer_name}"):
            props = rec["properties"]
            geom = rec["geometry"]
            
            if geom is None:
                continue
            
            objval = props.get(type_field, "")
            
            # Skip footpaths and irrelevant types
            if skip_footpaths and objval in skip_types:
                continue
            
            # Skip tunnels (not visible)
            tunnel_val = props.get(tunnel_field, "")
            if skip_tunnels and tunnel_val in tunnel_values:
                continue
            
            # Get width
            half_width = width_map.get(objval)
            if half_width is None:
                continue
            
            # Determine material class
            if is_railway:
                mat_class = "railway"
            else:
                mat_class = classify_road(objval)
            
            # Extract coordinates
            if geom["type"] == "LineString":
                coords = geom["coordinates"]
                # Ensure 3D
                if len(coords[0]) < 3:
                    coords = [(c[0], c[1], 0) for c in coords]
            elif geom["type"] == "MultiLineString":
                for line_coords in geom["coordinates"]:
                    if len(line_coords[0]) < 3:
                        line_coords = [(c[0], c[1], 0) for c in line_coords]
                    
                    if bbox and not _coords_in_bbox(line_coords, bbox):
                        continue
                    
                    features.append((line_coords, half_width, mat_class))
                continue
            else:
                continue
            
            # Apply bounding box filter
            if bbox and not _coords_in_bbox(coords, bbox):
                continue
            
            features.append((coords, half_width, mat_class))
    
    return features


def bin_features_to_tiles(features, tile_size=ROAD_TILE_SIZE):
    """
    Spatially bin features into tiles.
    
    Each feature is assigned to the tile containing its first coordinate.
    Long features that span multiple tiles are clipped at tile boundaries.
    
    Returns:
        Dict[tile_key] -> list of (coords, half_width, mat_class)
    """
    tiles = defaultdict(list)
    
    for coords, half_width, mat_class in features:
        # Assign based on midpoint of the segment
        if len(coords) >= 2:
            mid_idx = len(coords) // 2
            mid_e = coords[mid_idx][0]
            mid_n = coords[mid_idx][1]
        else:
            mid_e = coords[0][0]
            mid_n = coords[0][1]
        
        key = get_tile_key(mid_e, mid_n, tile_size)
        tiles[key].append((coords, half_width, mat_class))
    
    return tiles


def generate_tile_glb(features, tile_key, origin_e, origin_n, output_dir):
    """
    Generate a GLB file for a single road tile.
    
    Args:
        features: List of (coords, half_width, mat_class) for this tile
        tile_key: (easting, northing) of tile origin
        origin_e, origin_n: Godot coordinate origin
        output_dir: Output directory for GLB files
        
    Returns:
        dict with tile metadata or None
    """
    all_vertices = []
    all_triangles = []
    all_colors = []
    vertex_offset = 0
    segment_count = 0
    
    for coords, half_width, mat_class in features:
        verts, tris = linestring_to_ribbon(coords, half_width)
        if len(verts) == 0:
            continue
        
        tris = tris + vertex_offset
        all_vertices.append(verts)
        all_triangles.append(tris)
        
        # Per-vertex color from road classification
        rgb = ROAD_COLORS.get(mat_class, (0.5, 0.5, 0.5))
        seg_colors = np.full((len(verts), 4), 255, dtype=np.uint8)
        seg_colors[:, 0] = int(rgb[0] * 255)
        seg_colors[:, 1] = int(rgb[1] * 255)
        seg_colors[:, 2] = int(rgb[2] * 255)
        all_colors.append(seg_colors)
        
        vertex_offset += len(verts)
        segment_count += 1
        
        # ── White stripes for roads ──
        stripe_ribbons = []
        
        if mat_class == "motorway":
            # Edge stripes on both sides + center median line
            edge_off = half_width - STRIPE_HALF_WIDTH
            stripe_ribbons.append(offset_linestring(coords, edge_off))
            stripe_ribbons.append(offset_linestring(coords, -edge_off))
            stripe_ribbons.append(coords)  # center line
        elif mat_class == "main":
            # Edge stripes on both sides
            edge_off = half_width - STRIPE_HALF_WIDTH
            stripe_ribbons.append(offset_linestring(coords, edge_off))
            stripe_ribbons.append(offset_linestring(coords, -edge_off))
        elif mat_class == "secondary":
            # Single edge stripe on each side
            edge_off = half_width - STRIPE_HALF_WIDTH
            stripe_ribbons.append(offset_linestring(coords, edge_off))
            stripe_ribbons.append(offset_linestring(coords, -edge_off))
        
        for stripe_coords in stripe_ribbons:
            s_verts, s_tris = linestring_to_ribbon(
                stripe_coords, STRIPE_HALF_WIDTH,
                z_offset=Z_OFFSET + STRIPE_Z_BUMP)
            if len(s_verts) == 0:
                continue
            s_tris = s_tris + vertex_offset
            all_vertices.append(s_verts)
            all_triangles.append(s_tris)
            s_colors = np.full((len(s_verts), 4), 255, dtype=np.uint8)
            all_colors.append(s_colors)
            vertex_offset += len(s_verts)
            segment_count += 1
        
        # ── Rail lines for railways ──
        if mat_class == "railway":
            rail_rgb = RAIL_COLOR
            for sign in (+1, -1):
                rail_coords = offset_linestring(coords, sign * RAIL_GAUGE_HALF)
                r_verts, r_tris = linestring_to_ribbon(
                    rail_coords, RAIL_HALF_WIDTH,
                    z_offset=Z_OFFSET + STRIPE_Z_BUMP)
                if len(r_verts) == 0:
                    continue
                r_tris = r_tris + vertex_offset
                all_vertices.append(r_verts)
                all_triangles.append(r_tris)
                r_colors = np.full((len(r_verts), 4), 255, dtype=np.uint8)
                r_colors[:, 0] = int(rail_rgb[0] * 255)
                r_colors[:, 1] = int(rail_rgb[1] * 255)
                r_colors[:, 2] = int(rail_rgb[2] * 255)
                all_colors.append(r_colors)
                vertex_offset += len(r_verts)
                segment_count += 1
    
    if segment_count == 0:
        return None
    
    # Merge all geometry
    vertices = np.vstack(all_vertices)
    triangles = np.vstack(all_triangles)
    colors = np.vstack(all_colors)
    
    # Transform to Godot coordinates
    godot_verts = transform_to_godot(vertices, origin_e, origin_n)
    normals = compute_normals_godot(godot_verts, triangles)
    
    # Create GLB
    tile_e, tile_n = tile_key
    tile_name = f"roads_{tile_e}_{tile_n}"
    glb_name = f"{tile_name}.glb"
    
    gltf, total_bytes = create_glb(godot_verts, normals, triangles,
                                    colors=colors, name=tile_name)
    
    # Build buffer directly
    buf = bytearray(total_bytes)
    
    # Indices
    idx_flat = triangles.flatten().astype(np.uint32)
    buf[:len(idx_flat) * 4] = idx_flat.tobytes()
    
    # Positions
    pos_offset = gltf["bufferViews"][1]["byteOffset"]
    pos_data = godot_verts.astype(np.float32).tobytes()
    buf[pos_offset:pos_offset + len(pos_data)] = pos_data
    
    # Normals
    norm_offset = gltf["bufferViews"][2]["byteOffset"]
    norm_data = normals.astype(np.float32).tobytes()
    buf[norm_offset:norm_offset + len(norm_data)] = norm_data
    
    # Colors
    if len(gltf["bufferViews"]) > 3:
        col_offset = gltf["bufferViews"][3]["byteOffset"]
        col_data = colors.astype(np.uint8).tobytes()
        buf[col_offset:col_offset + len(col_data)] = col_data
    
    output_dir.mkdir(parents=True, exist_ok=True)
    output_path = output_dir / glb_name
    write_glb(gltf, bytes(buf), output_path)
    
    # Compute center
    center_e = int(round(vertices[:, 0].mean()))
    center_n = int(round(vertices[:, 1].mean()))
    
    return {
        "file": glb_name,
        "tile_e": tile_e,
        "tile_n": tile_n,
        "center_e": center_e,
        "center_n": center_n,
        "segments": segment_count,
        "vertices": len(vertices),
    }


def detect_source(gpkg_path):
    """Auto-detect whether GeoPackage is swissTLMRegio or swissTLM3D."""
    layers = fiona.listlayers(str(gpkg_path))
    if "tlm_strassen_strasse" in layers:
        return "tlm3d"
    elif "tlmregio_transportation_road" in layers:
        return "regio"
    else:
        raise ValueError(f"Unknown GeoPackage format. Layers: {layers[:10]}")


def run_road_pipeline(gpkg_path, output_dir, origin_e, origin_n,
                      bbox=None, tile_size=ROAD_TILE_SIZE):
    """
    Full road pipeline: read → bin → generate GLB tiles → manifest.
    
    Args:
        gpkg_path: Path to GeoPackage file
        output_dir: Output directory for road GLB files
        origin_e, origin_n: Godot coordinate origin (TERRAIN_CENTER)
        bbox: Optional (west, south, east, north) bounding box
        tile_size: Tile size in meters
    """
    gpkg_path = Path(gpkg_path)
    output_dir = Path(output_dir)
    
    # Auto-detect source type
    source = detect_source(gpkg_path)
    
    if source == "tlm3d":
        road_layer = "tlm_strassen_strasse"
        rail_layer = "tlm_oev_eisenbahn"
        road_widths = ROAD_WIDTHS_TLM3D
        rail_widths = RAILWAY_WIDTHS_TLM3D
        source_label = "swissTLM3D"
    else:
        road_layer = "tlmregio_transportation_road"
        rail_layer = "tlmregio_transportation_railway"
        road_widths = ROAD_WIDTHS_REGIO
        rail_widths = RAILWAY_WIDTHS_REGIO
        source_label = "swissTLMRegio"
    
    print(f"═══════════════════════════════════════════════════════════")
    print(f"ROAD PIPELINE")
    print(f"═══════════════════════════════════════════════════════════")
    print(f"  Source:    {gpkg_path.name} ({source_label})")
    print(f"  Output:    {output_dir}")
    print(f"  Origin:    E={origin_e} N={origin_n}")
    print(f"  Tile size: {tile_size}m")
    if bbox:
        print(f"  BBox:      W={bbox[0]} S={bbox[1]} E={bbox[2]} N={bbox[3]}")
    print()
    
    # ── Step 1: Read features ──────────────────────────────────────
    print("Step 1: Reading road features...")
    road_features = read_features(
        gpkg_path, road_layer,
        bbox=bbox, width_map=road_widths,
    )
    print(f"  → {len(road_features)} road segments")
    
    print("Step 1b: Reading railway features...")
    rail_features = read_features(
        gpkg_path, rail_layer,
        bbox=bbox, width_map=rail_widths,
        skip_footpaths=False,
    )
    print(f"  → {len(rail_features)} railway segments")
    
    all_features = road_features + rail_features
    print(f"  Total: {len(all_features)} features")
    print()
    
    # ── Step 2: Bin features into tiles ────────────────────────────
    print("Step 2: Spatial binning...")
    tiles = bin_features_to_tiles(all_features, tile_size)
    print(f"  → {len(tiles)} tiles")
    print()
    
    # ── Step 3: Generate GLB tiles ─────────────────────────────────
    print("Step 3: Generating GLB tiles...")
    manifest_tiles = {}
    
    for tile_key in tqdm(sorted(tiles.keys()), desc="Generating GLBs"):
        features = tiles[tile_key]
        result = generate_tile_glb(features, tile_key, origin_e, origin_n, output_dir)
        if result:
            tile_id = f"{tile_key[0]}_{tile_key[1]}"
            manifest_tiles[tile_id] = result
    
    print(f"  → {len(manifest_tiles)} GLB files written")
    print()
    
    # ── Step 4: Write manifest ─────────────────────────────────────
    print("Step 4: Writing manifest...")
    manifest = {
        "format": "glb",
        "source": source_label,
        "tile_size_m": tile_size,
        "conversion_origin_e": origin_e,
        "conversion_origin_n": origin_n,
        "tiles": manifest_tiles,
    }
    
    manifest_path = output_dir / "manifest.json"
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)
    
    # Summary
    total_segs = sum(t["segments"] for t in manifest_tiles.values())
    total_verts = sum(t["vertices"] for t in manifest_tiles.values())
    total_size = sum((output_dir / t["file"]).stat().st_size for t in manifest_tiles.values())
    
    print(f"\n  Summary:")
    print(f"    Tiles:     {len(manifest_tiles)}")
    print(f"    Segments:  {total_segs}")
    print(f"    Vertices:  {total_verts}")
    print(f"    Disk:      {total_size / (1024*1024):.1f} MB")
    print(f"    Manifest:  {manifest_path}")
    print()


# ── CLI ──────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Generate road meshes for ProvPilot")
    parser.add_argument("--gpkg", required=True, help="Path to GeoPackage file")
    parser.add_argument("--output", default="scenery/roads", help="Output directory")
    parser.add_argument("--center-e", type=float, default=2660000, help="Godot origin easting (LV95)")
    parser.add_argument("--center-n", type=float, default=1185000, help="Godot origin northing (LV95)")
    parser.add_argument("--radius", type=float, default=None, help="Radius in meters (optional bbox)")
    parser.add_argument("--tile-size", type=int, default=ROAD_TILE_SIZE, help="Tile size in meters")
    
    args = parser.parse_args()
    
    bbox = None
    if args.radius:
        bbox = (
            args.center_e - args.radius,
            args.center_n - args.radius,
            args.center_e + args.radius,
            args.center_n + args.radius,
        )
    
    run_road_pipeline(
        gpkg_path=args.gpkg,
        output_dir=Path(args.output),
        origin_e=args.center_e,
        origin_n=args.center_n,
        bbox=bbox,
        tile_size=args.tile_size,
    )


if __name__ == "__main__":
    main()
