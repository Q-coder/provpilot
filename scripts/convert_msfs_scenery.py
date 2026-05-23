#!/usr/bin/env python3
"""
convert_msfs_scenery.py — Convert MSFS 2020 scenery packages to Godot-compatible GLB.

Extracts glTF 2.0 models from BGL files, converts DDS textures to PNG,
strips MSFS-specific extensions, and produces clean GLB files that Godot
can import directly.

Usage:
    python3 scripts/convert_msfs_scenery.py <scenery_dir> [--output-dir <dir>]

Example:
    python3 scripts/convert_msfs_scenery.py scenery/custom/ps-airport-lspf-schmerlat

The script:
 1. Finds modelLib.BGL in the scenery package
 2. Extracts embedded glTF 2.0 binaries
 3. Resolves DDS texture references and converts to PNG
 4. Strips MSFS-specific glTF extensions (ASOBO_*, MSFT_texture_dds)
 5. Re-packs into clean GLB files with embedded textures
 6. Outputs a summary of all extracted models
"""

import argparse
import io
import json
import math
import os
import struct
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("Error: Pillow is required. Install with: pip install Pillow")
    sys.exit(1)


# ── glTF binary format constants ──────────────────────────────────────────

GLTF_MAGIC = b'glTF'
GLTF_VERSION = 2
CHUNK_TYPE_JSON = 0x4E4F534A   # 'JSON'
CHUNK_TYPE_BIN  = 0x004E4942   # 'BIN\0'

# MSFS extensions to strip
MSFS_EXTENSIONS = {
    'ASOBO_normal_map_convention',
    'ASOBO_macro_light',
    'ASOBO_asset_optimized',
    'ASOBO_material_windshield',
    'ASOBO_material_SSS',
    'ASOBO_material_anisotropic',
    'ASOBO_material_glass',
    'ASOBO_material_clear_coat',
    'ASOBO_material_fresnel_fade',
    'ASOBO_material_parallax_window',
    'ASOBO_material_fake_terrain',
    'ASOBO_material_invisible',
    'ASOBO_material_environment_occluder',
    'ASOBO_material_draw_order',
    'ASOBO_material_shadow_options',
    'ASOBO_material_day_night_switch',
    'ASOBO_material_UV_clamp',
    'ASOBO_material_detail_map',
    'ASOBO_material_blend_gbuffer',
    'ASOBO_material_pearlescent',
    'ASOBO_material_geo_decal',
    'ASOBO_tags',
    'MSFT_texture_dds',
}


def extract_glbs_from_bgl(bgl_path: Path) -> list[tuple[int, bytes]]:
    """Extract all glTF 2.0 binary chunks from a BGL file.

    Returns list of (offset, glb_bytes) tuples.
    """
    data = bgl_path.read_bytes()
    results = []
    start = 0

    while True:
        pos = data.find(GLTF_MAGIC, start)
        if pos == -1:
            break
        if pos + 12 > len(data):
            break

        version = struct.unpack_from('<I', data, pos + 4)[0]
        length = struct.unpack_from('<I', data, pos + 8)[0]

        if version == GLTF_VERSION and 100 < length and pos + length <= len(data):
            results.append((pos, data[pos:pos + length]))
            start = pos + length
        else:
            start = pos + 1

    return results


def parse_glb(glb_data: bytes) -> tuple[dict, bytes]:
    """Parse a GLB into (json_dict, binary_buffer)."""
    if glb_data[:4] != GLTF_MAGIC:
        raise ValueError("Not a GLB file")

    version = struct.unpack_from('<I', glb_data, 4)[0]
    total_length = struct.unpack_from('<I', glb_data, 8)[0]

    if version != 2:
        raise ValueError(f"Unsupported glTF version: {version}")

    offset = 12
    json_data = None
    bin_data = b''

    while offset < len(glb_data):
        chunk_length = struct.unpack_from('<I', glb_data, offset)[0]
        chunk_type = struct.unpack_from('<I', glb_data, offset + 4)[0]
        chunk_data = glb_data[offset + 8:offset + 8 + chunk_length]

        if chunk_type == CHUNK_TYPE_JSON:
            json_str = chunk_data.decode('utf-8').rstrip('\x00')
            json_data = json.loads(json_str)
        elif chunk_type == CHUNK_TYPE_BIN:
            bin_data = chunk_data

        offset += 8 + chunk_length
        # Align to 4 bytes
        if offset % 4 != 0:
            offset += 4 - (offset % 4)

    if json_data is None:
        raise ValueError("No JSON chunk found in GLB")

    return json_data, bin_data


def build_glb(gltf_json: dict, bin_buffer: bytes) -> bytes:
    """Build a GLB binary from JSON dict and binary buffer."""
    # Pad binary to 4-byte alignment with zeros
    bin_padded = bytearray(bin_buffer)
    while len(bin_padded) % 4 != 0:
        bin_padded.append(0)

    # Update buffer byteLength to match padded BIN chunk (per GLB spec)
    buffers = gltf_json.get('buffers', [])
    if buffers:
        buffers[0]['byteLength'] = len(bin_padded)

    json_str = json.dumps(gltf_json, separators=(',', ':')).encode('utf-8')
    # Pad JSON to 4-byte alignment with spaces
    while len(json_str) % 4 != 0:
        json_str += b' '

    total_length = 12 + 8 + len(json_str) + 8 + len(bin_padded)

    out = io.BytesIO()
    # GLB header
    out.write(GLTF_MAGIC)
    out.write(struct.pack('<I', GLTF_VERSION))
    out.write(struct.pack('<I', total_length))
    # JSON chunk
    out.write(struct.pack('<I', len(json_str)))
    out.write(struct.pack('<I', CHUNK_TYPE_JSON))
    out.write(json_str)
    # BIN chunk
    out.write(struct.pack('<I', len(bin_padded)))
    out.write(struct.pack('<I', CHUNK_TYPE_BIN))
    out.write(bytes(bin_padded))

    return out.getvalue()


def load_dds_as_png_bytes(dds_path: Path, max_size: int = 1024) -> bytes | None:
    """Load a DDS file and return compressed image bytes (JPEG for RGB, PNG for RGBA).
    
    Downscales to max_size if larger.
    """
    try:
        img = Image.open(dds_path)
        # Downscale large textures
        if max(img.size) > max_size:
            ratio = max_size / max(img.size)
            new_size = (int(img.size[0] * ratio), int(img.size[1] * ratio))
            img = img.resize(new_size, Image.LANCZOS)
        
        buf = io.BytesIO()
        if img.mode == 'RGBA':
            # Check if alpha channel is actually used (not all 255)
            alpha = img.getchannel('A')
            if alpha.getextrema() == (255, 255):
                # All opaque — convert to RGB and save as JPEG
                img = img.convert('RGB')
                img.save(buf, format='JPEG', quality=85)
                return buf.getvalue()
            else:
                img.save(buf, format='PNG', optimize=True)
                return buf.getvalue()
        else:
            img = img.convert('RGB')
            img.save(buf, format='JPEG', quality=85)
            return buf.getvalue()
    except Exception as e:
        print(f"    WARNING: Failed to convert {dds_path.name}: {e}")
        return None


def convert_glb(
    gltf_json: dict,
    bin_data: bytes,
    texture_dir: Path,
    model_index: int,
    max_texture_size: int = 1024,
) -> tuple[dict, bytes]:
    """Fix a MSFS glTF: resolve DDS textures, strip extensions, embed PNGs.

    Returns (cleaned_json, new_binary_buffer).
    """
    images = gltf_json.get('images', [])
    textures = gltf_json.get('textures', [])

    # ── 1. Fix texture source references ──────────────────────────
    # MSFS uses MSFT_texture_dds extension to point to DDS images.
    # Move the DDS source index to the standard 'source' field.
    for tex in textures:
        ext = tex.get('extensions', {})
        dds_ext = ext.get('MSFT_texture_dds', {})
        if 'source' in dds_ext:
            tex['source'] = dds_ext['source']
        # Remove the extension from this texture
        if 'MSFT_texture_dds' in ext:
            del ext['MSFT_texture_dds']
        if not ext:
            tex.pop('extensions', None)

    # ── 2. Resolve and convert DDS images to embedded PNGs ────────
    new_bin = bytearray(bin_data)
    buffer_views = gltf_json.get('bufferViews', [])
    buffers = gltf_json.get('buffers', [])

    for i, img in enumerate(images):
        uri = img.get('uri', '')
        if not uri:
            continue  # Already embedded in buffer

        # Resolve the DDS file path
        # URIs may reference .DDS files in the texture directory
        dds_path = texture_dir / uri
        if not dds_path.exists():
            # Try case-insensitive match
            dds_path = _find_file_icase(texture_dir, uri)

        if dds_path and dds_path.exists():
            png_data = load_dds_as_png_bytes(dds_path, max_texture_size)
            if png_data:
                # Add image to buffer and create a new bufferView
                # Align to 4 bytes
                while len(new_bin) % 4 != 0:
                    new_bin.append(0)

                bv_offset = len(new_bin)
                new_bin.extend(png_data)

                bv_index = len(buffer_views)
                buffer_views.append({
                    'buffer': 0,
                    'byteOffset': bv_offset,
                    'byteLength': len(png_data),
                })

                # Detect format from header bytes
                mime = 'image/png' if png_data[:4] == b'\x89PNG' else 'image/jpeg'

                # Update image to use embedded buffer
                img.pop('uri', None)
                img['bufferView'] = bv_index
                img['mimeType'] = mime
                continue

        # Check for roughness PNGs (these aren't DDS, they're plain PNGs
        # referenced directly)
        if uri.endswith('.PNG') or uri.endswith('.png'):
            png_path = texture_dir / uri
            if not png_path.exists():
                png_path = _find_file_icase(texture_dir, uri)
            if png_path and png_path.exists():
                png_data = png_path.read_bytes()
                while len(new_bin) % 4 != 0:
                    new_bin.append(0)
                bv_offset = len(new_bin)
                new_bin.extend(png_data)
                bv_index = len(buffer_views)
                buffer_views.append({
                    'buffer': 0,
                    'byteOffset': bv_offset,
                    'byteLength': len(png_data),
                })
                img.pop('uri', None)
                img['bufferView'] = bv_index
                img['mimeType'] = 'image/png'
                continue

        print(f"    WARNING: Image [{i}] texture not found: {uri}")

    # ── 3. Update buffer size ─────────────────────────────────────
    if buffers:
        buffers[0]['byteLength'] = len(new_bin)

    # ── 4. Strip MSFS extensions ──────────────────────────────────
    if 'extensionsUsed' in gltf_json:
        gltf_json['extensionsUsed'] = [
            e for e in gltf_json['extensionsUsed']
            if e not in MSFS_EXTENSIONS
        ]
        if not gltf_json['extensionsUsed']:
            del gltf_json['extensionsUsed']

    if 'extensionsRequired' in gltf_json:
        gltf_json['extensionsRequired'] = [
            e for e in gltf_json['extensionsRequired']
            if e not in MSFS_EXTENSIONS
        ]
        if not gltf_json['extensionsRequired']:
            del gltf_json['extensionsRequired']

    # Strip MSFS extensions from materials
    for mat in gltf_json.get('materials', []):
        ext = mat.get('extensions', {})
        for key in list(ext.keys()):
            if key in MSFS_EXTENSIONS:
                del ext[key]
        if not ext:
            mat.pop('extensions', None)

    # Strip from nodes
    for node in gltf_json.get('nodes', []):
        ext = node.get('extensions', {})
        for key in list(ext.keys()):
            if key in MSFS_EXTENSIONS:
                del ext[key]
        if not ext:
            node.pop('extensions', None)

    # Strip from root
    ext = gltf_json.get('extensions', {})
    for key in list(ext.keys()):
        if key in MSFS_EXTENSIONS:
            del ext[key]
    if not ext and 'extensions' in gltf_json:
        del gltf_json['extensions']

    # Strip from asset metadata (ASOBO_asset_optimized, etc.)
    asset_ext = gltf_json.get('asset', {}).get('extensions', {})
    for key in list(asset_ext.keys()):
        if key.startswith('ASOBO_') or key.startswith('MSFT_'):
            del asset_ext[key]
    if not asset_ext and 'extensions' in gltf_json.get('asset', {}):
        del gltf_json['asset']['extensions']

    # Strip remaining extras from primitives, meshes, accessors, images
    for mesh in gltf_json.get('meshes', []):
        mesh.pop('extras', None)
        for prim in mesh.get('primitives', []):
            # Keep extras until index splitting uses ASOBO_primitive
            pass
    for acc in gltf_json.get('accessors', []):
        acc.pop('extras', None)
    for img in gltf_json.get('images', []):
        img.pop('extras', None)

    # ── 5. Split shared index accessors using ASOBO_primitive submesh ranges ─
    # MSFS stores per-primitive StartIndex and PrimitiveCount in extras.
    # Without this, all primitives draw ALL indices from the shared accessor.
    new_bin = bytearray(new_bin)
    new_bin = _split_msfs_index_accessors(gltf_json, new_bin)

    # ── 6. De-interleave and convert MSFS packed vertex data ─────
    # MSFS uses a packed interleaved vertex format with half-floats
    # for texcoords/colors and signed bytes for normals/tangents.
    # Godot doesn't handle this well — de-interleave to standard floats.
    gltf_json, new_bin = _deinterleave_msfs_vertices(gltf_json, new_bin)

    # ── 7. Split shared vertex buffers so each primitive is standalone ─────
    # MSFS meshes share one vertex buffer across all primitives. Some engines
    # don't handle shared vertex accessors correctly. This step gives each
    # primitive its own vertex bufferViews with only the referenced vertices.
    gltf_json, new_bin = _split_shared_vertex_buffers(gltf_json, new_bin)

    return gltf_json, bytes(new_bin)


def _half_to_float(h: int) -> float:
    """Convert a 16-bit half-float (as unsigned int) to Python float."""
    sign = (h >> 15) & 1
    exp = (h >> 10) & 0x1F
    frac = h & 0x3FF
    if exp == 0:
        if frac == 0:
            return (-1)**sign * 0.0
        # Subnormal
        return (-1)**sign * (frac / 1024.0) * (2 ** -14)
    elif exp == 31:
        if frac == 0:
            return (-1)**sign * float('inf')
        return float('nan')
    return (-1)**sign * (1.0 + frac / 1024.0) * (2 ** (exp - 15))


def _split_msfs_index_accessors(gltf_json: dict, bin_data: bytearray) -> bytearray:
    """Split shared index accessors into per-primitive accessors using ASOBO_primitive extras.

    MSFS uses ASOBO_primitive extras to store per-primitive submesh ranges:
      - StartIndex: starting index offset in the shared index accessor (default 0)
      - PrimitiveCount: number of triangles for this primitive

    Without this, all primitives sharing an index accessor draw ALL indices,
    causing the same geometry to be rendered multiple times with different materials.

    This function creates a NEW bufferView and accessor for each primitive,
    copying its index sub-range into a dedicated slice of the binary buffer.
    This avoids relying on accessor byteOffset within shared bufferViews,
    which some engines handle incorrectly.
    """
    accessors = gltf_json.get('accessors', [])
    buffer_views = gltf_json.get('bufferViews', [])
    meshes = gltf_json.get('meshes', [])

    for mesh in meshes:
        for prim in mesh.get('primitives', []):
            asobo = prim.get('extras', {}).get('ASOBO_primitive', {})
            prim_count = asobo.get('PrimitiveCount')
            if prim_count is None:
                continue

            start_index = asobo.get('StartIndex', 0)
            index_count = prim_count * 3  # 3 indices per triangle

            old_acc_idx = prim.get('indices')
            if old_acc_idx is None:
                continue

            old_acc = accessors[old_acc_idx]
            old_bv = buffer_views[old_acc['bufferView']]
            comp_type = old_acc['componentType']
            comp_size = _COMP_SIZE.get(comp_type, 2)

            # Calculate source byte range in the original buffer
            src_bv_offset = old_bv.get('byteOffset', 0)
            src_acc_offset = old_acc.get('byteOffset', 0)
            src_start = src_bv_offset + src_acc_offset + start_index * comp_size
            src_length = index_count * comp_size

            # Copy this primitive's index data to a new location in the buffer
            while len(bin_data) % 4 != 0:
                bin_data.append(0)
            new_bv_offset = len(bin_data)
            bin_data.extend(bin_data[src_start:src_start + src_length])

            # Create a new bufferView for this primitive's indices
            new_bv_idx = len(buffer_views)
            buffer_views.append({
                'buffer': 0,
                'byteOffset': new_bv_offset,
                'byteLength': src_length,
                'target': 34963,  # ELEMENT_ARRAY_BUFFER
            })

            # Create a new accessor with byteOffset=0 (dedicated bufferView)
            new_acc = {
                'bufferView': new_bv_idx,
                'byteOffset': 0,
                'componentType': comp_type,
                'count': index_count,
                'type': 'SCALAR',
            }

            new_acc_idx = len(accessors)
            accessors.append(new_acc)
            prim['indices'] = new_acc_idx

            # Clean up the extras now that we've used them
            del prim['extras']['ASOBO_primitive']
            if not prim['extras']:
                del prim['extras']

    return bin_data


# Component type sizes and struct format chars
_COMP_SIZE = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}
_COMP_FMT  = {5120: 'b', 5121: 'B', 5122: 'h', 5123: 'H', 5125: 'I', 5126: 'f'}
_TYPE_N    = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT2': 4, 'MAT3': 9, 'MAT4': 16}


def _deinterleave_msfs_vertices(gltf_json: dict, bin_data: bytearray) -> tuple[dict, bytearray]:
    """De-interleave MSFS packed vertex buffers into standard separate-attribute buffers.

    MSFS packs all vertex attributes into a single interleaved buffer with stride=36:
      offset  0: POSITION   VEC3 FLOAT  (12 bytes)
      offset 12: TANGENT    VEC4 BYTE   (4 bytes)  — signed normalized
      offset 16: NORMAL     VEC4 BYTE   (4 bytes)  — signed normalized (should be VEC3)
      offset 20: TEXCOORD_0 VEC2 SHORT  (4 bytes)  — actually half-floats
      offset 24: TEXCOORD_1 VEC2 SHORT  (4 bytes)  — actually half-floats
      offset 28: COLOR_0    VEC4 USHORT (8 bytes)  — actually half-floats

    This function:
    1. Reads the packed data using the stride layout
    2. Converts BYTE normals/tangents to FLOAT (with normalization)
    3. Converts SHORT/USHORT texcoords/colors from half-float to FLOAT
    4. Writes each attribute into its own separate bufferView
    5. Updates accessors to point to standard FLOAT data
    """
    accessors = gltf_json.get('accessors', [])
    buffer_views = gltf_json.get('bufferViews', [])
    meshes = gltf_json.get('meshes', [])
    buffers = gltf_json.get('buffers', [])

    # Find interleaved buffer views (those with byteStride set)
    interleaved_bvs = set()
    for i, bv in enumerate(buffer_views):
        if bv.get('byteStride'):
            interleaved_bvs.add(i)

    if not interleaved_bvs:
        return gltf_json, bin_data

    # Build accessor → attribute name mapping
    acc_usage = {}
    for mesh in meshes:
        for prim in mesh.get('primitives', []):
            for attr_name, acc_idx in prim.get('attributes', {}).items():
                acc_usage.setdefault(acc_idx, set()).add(attr_name)

    new_bin = bytearray(bin_data)

    # Process each interleaved buffer view
    for bv_idx in sorted(interleaved_bvs):
        bv = buffer_views[bv_idx]
        stride = bv['byteStride']
        bv_offset = bv.get('byteOffset', 0)
        bv_length = bv['byteLength']

        # Find all accessors using this buffer view
        bv_accessors = []
        for acc_idx, acc in enumerate(accessors):
            if acc.get('bufferView') == bv_idx:
                bv_accessors.append(acc_idx)

        if not bv_accessors:
            continue

        # Read and convert each accessor's data
        for acc_idx in bv_accessors:
            acc = accessors[acc_idx]
            vertex_count = acc['count']
            comp_type = acc['componentType']
            acc_type = acc['type']
            acc_offset = acc.get('byteOffset', 0)
            n_comps = _TYPE_N.get(acc_type, 1)
            comp_size = _COMP_SIZE.get(comp_type, 4)
            fmt_char = _COMP_FMT.get(comp_type, 'f')
            attr_names = acc_usage.get(acc_idx, set())

            # Determine output type
            is_position = 'POSITION' in attr_names
            is_normal = 'NORMAL' in attr_names
            is_tangent = 'TANGENT' in attr_names
            is_texcoord = any(n.startswith('TEXCOORD') for n in attr_names)
            is_color = 'COLOR_0' in attr_names

            # Read all vertex data for this accessor
            float_data = []
            for v in range(vertex_count):
                base = bv_offset + v * stride + acc_offset
                raw_values = struct.unpack_from(f'<{n_comps}{fmt_char}', new_bin, base)

                if comp_type == 5126:  # Already FLOAT
                    float_data.extend(raw_values)
                elif comp_type in (5120, 5121):  # BYTE/UBYTE — normalized
                    if comp_type == 5120:  # signed
                        float_data.extend(max(val / 127.0, -1.0) for val in raw_values)
                    else:
                        float_data.extend(val / 255.0 for val in raw_values)
                elif comp_type in (5122, 5123):  # SHORT/USHORT
                    if is_texcoord or is_color:
                        # Half-float encoding: raw bits are IEEE 754 half-precision
                        for rv in raw_values:
                            # Convert signed short to unsigned 16-bit for half-float decode
                            if comp_type == 5122:  # signed short
                                h = rv & 0xFFFF
                            else:
                                h = rv
                            float_data.append(_half_to_float(h))
                    else:
                        # Standard normalized
                        if comp_type == 5122:
                            float_data.extend(max(val / 32767.0, -1.0) for val in raw_values)
                        else:
                            float_data.extend(val / 65535.0 for val in raw_values)

            # Fix NORMAL: if it was VEC4, only keep first 3 components
            out_n_comps = n_comps
            if is_normal and acc_type == 'VEC4':
                # Strip 4th component
                fixed = []
                for i in range(0, len(float_data), 4):
                    fixed.extend(float_data[i:i+3])
                float_data = fixed
                out_n_comps = 3

            # Write as FLOAT data into a new bufferView
            while len(new_bin) % 4 != 0:
                new_bin.append(0)
            new_bv_offset = len(new_bin)
            float_bytes = struct.pack(f'<{len(float_data)}f', *float_data)
            new_bin.extend(float_bytes)

            # Create new bufferView
            new_bv_idx = len(buffer_views)
            # Set target for vertex attributes vs indices
            target = 34962  # ARRAY_BUFFER for vertex data
            buffer_views.append({
                'buffer': 0,
                'byteOffset': new_bv_offset,
                'byteLength': len(float_bytes),
                'target': target,
            })

            # Update accessor
            acc['bufferView'] = new_bv_idx
            acc['byteOffset'] = 0
            acc['componentType'] = 5126  # FLOAT
            acc.pop('normalized', None)
            if is_normal and acc_type == 'VEC4':
                acc['type'] = 'VEC3'

            # Update min/max for POSITION
            if is_position:
                mins = [float('inf')] * out_n_comps
                maxs = [float('-inf')] * out_n_comps
                for i in range(vertex_count):
                    for c in range(out_n_comps):
                        val = float_data[i * out_n_comps + c]
                        mins[c] = min(mins[c], val)
                        maxs[c] = max(maxs[c], val)
                acc['min'] = mins
                acc['max'] = maxs

        # Mark the old interleaved bufferView as unused (zero length)
        # We can't remove it as indices reference buffer views by index,
        # but we can shrink it
        # Actually, leave it as-is since index accessors may reference other bvs

    # Update buffer size
    if buffers:
        buffers[0]['byteLength'] = len(new_bin)

    return gltf_json, new_bin


def _split_shared_vertex_buffers(gltf_json: dict, bin_data: bytearray) -> tuple[dict, bytearray]:
    """Split shared vertex accessors so each primitive gets its own vertex data.

    MSFS meshes with multiple primitives share the same vertex accessors.
    All primitives reference all vertices, but each only uses a subset via
    its index accessor. This causes problems because:
    1. Some engines import unused vertices and waste memory
    2. The index values reference into the full shared vertex array,
       but some engines may not handle this correctly

    This function:
    1. For each primitive, collects the actual vertices referenced by its indices
    2. Creates new vertex bufferViews/accessors with only those vertices
    3. Remaps the indices to reference the compacted vertex data
    """
    accessors = gltf_json.get('accessors', [])
    buffer_views = gltf_json.get('bufferViews', [])
    meshes = gltf_json.get('meshes', [])
    buffers = gltf_json.get('buffers', [])

    TYPE_N = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}

    for mesh in meshes:
        prims = mesh.get('primitives', [])
        if len(prims) <= 1:
            continue  # Single-primitive meshes don't need splitting

        # Check if primitives share vertex accessors
        first_pos = prims[0].get('attributes', {}).get('POSITION')
        if first_pos is None:
            continue
        shared = all(
            p.get('attributes', {}).get('POSITION') == first_pos
            for p in prims[1:]
        )
        if not shared:
            continue  # Already split

        for prim in prims:
            attrs = prim.get('attributes', {})
            idx_acc_idx = prim.get('indices')
            if idx_acc_idx is None:
                continue

            # Read all indices for this primitive
            idx_acc = accessors[idx_acc_idx]
            idx_bv = buffer_views[idx_acc['bufferView']]
            comp = idx_acc['componentType']
            fmt = {5121: 'B', 5123: 'H', 5125: 'I'}.get(comp, 'H')
            sz = struct.calcsize('<' + fmt)
            idx_bv_off = idx_bv.get('byteOffset', 0)
            idx_acc_off = idx_acc.get('byteOffset', 0)

            indices = []
            for i in range(idx_acc['count']):
                val = struct.unpack_from('<' + fmt, bin_data, idx_bv_off + idx_acc_off + i * sz)[0]
                indices.append(val)

            # Build mapping: old_vertex_index → new_vertex_index
            used_verts = sorted(set(indices))
            old_to_new = {old: new for new, old in enumerate(used_verts)}
            new_vertex_count = len(used_verts)

            # Remap indices and write new index buffer
            new_indices = [old_to_new[idx] for idx in indices]
            while len(bin_data) % 4 != 0:
                bin_data.append(0)

            # Use UINT16 if possible, UINT32 otherwise
            if new_vertex_count <= 65535:
                new_comp = 5123  # UNSIGNED_SHORT
                new_fmt = 'H'
                new_sz = 2
            else:
                new_comp = 5125  # UNSIGNED_INT
                new_fmt = 'I'
                new_sz = 4

            new_idx_offset = len(bin_data)
            new_idx_bytes = struct.pack(f'<{len(new_indices)}{new_fmt}', *new_indices)
            bin_data.extend(new_idx_bytes)
            # Pad to 4 bytes
            while len(bin_data) % 4 != 0:
                bin_data.append(0)

            new_idx_bv = len(buffer_views)
            buffer_views.append({
                'buffer': 0,
                'byteOffset': new_idx_offset,
                'byteLength': len(new_idx_bytes),
                'target': 34963,
            })
            new_idx_acc = len(accessors)
            accessors.append({
                'bufferView': new_idx_bv,
                'byteOffset': 0,
                'componentType': new_comp,
                'count': len(new_indices),
                'type': 'SCALAR',
            })
            prim['indices'] = new_idx_acc

            # For each vertex attribute, extract only the used vertices
            for attr_name, attr_acc_idx in list(attrs.items()):
                attr_acc = accessors[attr_acc_idx]
                attr_bv = buffer_views[attr_acc['bufferView']]
                attr_type = attr_acc['type']
                n_comps = TYPE_N.get(attr_type, 1)
                attr_comp = attr_acc['componentType']
                attr_comp_sz = _COMP_SIZE.get(attr_comp, 4)
                attr_fmt = _COMP_FMT.get(attr_comp, 'f')
                attr_bv_off = attr_bv.get('byteOffset', 0)
                attr_acc_off = attr_acc.get('byteOffset', 0)
                attr_stride = attr_bv.get('byteStride', n_comps * attr_comp_sz)

                # Read only referenced vertices
                vertex_data = []
                for old_idx in used_verts:
                    base = attr_bv_off + attr_acc_off + old_idx * attr_stride
                    vals = struct.unpack_from(f'<{n_comps}{attr_fmt}', bin_data, base)
                    vertex_data.extend(vals)

                # Write compacted vertex data
                while len(bin_data) % 4 != 0:
                    bin_data.append(0)
                new_attr_offset = len(bin_data)
                new_attr_bytes = struct.pack(
                    f'<{len(vertex_data)}{attr_fmt}', *vertex_data)
                bin_data.extend(new_attr_bytes)

                new_attr_bv = len(buffer_views)
                buffer_views.append({
                    'buffer': 0,
                    'byteOffset': new_attr_offset,
                    'byteLength': len(new_attr_bytes),
                    'target': 34962,
                })

                new_attr_acc = len(accessors)
                new_acc_entry = {
                    'bufferView': new_attr_bv,
                    'byteOffset': 0,
                    'componentType': attr_comp,
                    'count': new_vertex_count,
                    'type': attr_type,
                }
                # Preserve min/max for POSITION
                if 'min' in attr_acc:
                    # Recompute min/max from compacted data
                    mins = [float('inf')] * n_comps
                    maxs = [float('-inf')] * n_comps
                    for vi in range(new_vertex_count):
                        for c in range(n_comps):
                            val = vertex_data[vi * n_comps + c]
                            mins[c] = min(mins[c], val)
                            maxs[c] = max(maxs[c], val)
                    new_acc_entry['min'] = mins
                    new_acc_entry['max'] = maxs

                accessors.append(new_acc_entry)
                attrs[attr_name] = new_attr_acc

    if buffers:
        buffers[0]['byteLength'] = len(bin_data)

    return gltf_json, bin_data

def _find_file_icase(directory: Path, filename: str) -> Path | None:
    """Case-insensitive file search in a directory."""
    target = filename.lower()
    if directory.exists():
        for f in directory.iterdir():
            if f.name.lower() == target:
                return f
    return None


def get_model_name(gltf_json: dict) -> str:
    """Extract a human-readable name from the glTF metadata."""
    # Try scene name
    scenes = gltf_json.get('scenes', [])
    if scenes:
        name = scenes[0].get('name', '')
        if name and name != 'Scene':
            return name

    # Try first few meaningful node names
    nodes = gltf_json.get('nodes', [])
    names = []
    for n in nodes[:5]:
        nm = n.get('name', '')
        if nm and not nm.startswith('Cube') and not nm.startswith('Cylinder'):
            names.append(nm)
    if names:
        return '_'.join(names[:2])

    return ''


def main():
    parser = argparse.ArgumentParser(
        description='Convert MSFS 2020 scenery to Godot-compatible GLB files')
    parser.add_argument('scenery_dir', type=Path,
                        help='Path to the MSFS scenery package directory')
    parser.add_argument('--output-dir', '-o', type=Path, default=None,
                        help='Output directory (default: <scenery_dir>/converted)')
    parser.add_argument('--list-only', action='store_true',
                        help='Only list models without converting')
    parser.add_argument('--models', type=str, default=None,
                        help='Comma-separated model indices to convert (default: all)')
    parser.add_argument('--max-texture-size', type=int, default=1024,
                        help='Maximum texture dimension in pixels (default: 1024)')
    args = parser.parse_args()

    scenery_dir = args.scenery_dir.resolve()
    if not scenery_dir.exists():
        print(f"Error: {scenery_dir} does not exist")
        sys.exit(1)

    # Find the modelLib BGL
    bgl_path = None
    for p in scenery_dir.rglob('modelLib.BGL'):
        bgl_path = p
        break
    if not bgl_path:
        # Try case-insensitive
        for p in scenery_dir.rglob('*.BGL'):
            if 'modellib' in p.name.lower():
                bgl_path = p
                break

    if not bgl_path:
        print("Error: No modelLib.BGL found in the scenery package")
        sys.exit(1)

    print(f"Found modelLib: {bgl_path}")
    print(f"  Size: {bgl_path.stat().st_size:,} bytes")

    # Find texture directory
    texture_dir = bgl_path.parent / 'texture'
    if not texture_dir.exists():
        print(f"  WARNING: No texture directory at {texture_dir}")
        texture_dir = None

    # Also check MaterialLibs for additional textures
    matlib_textures = []
    matlib_dir = scenery_dir / 'MaterialLibs'
    if matlib_dir.exists():
        for tex_dir in matlib_dir.rglob('Textures'):
            matlib_textures.append(tex_dir)

    # Extract glTF models
    print(f"\nExtracting glTF models from BGL...")
    glbs = extract_glbs_from_bgl(bgl_path)
    print(f"  Found {len(glbs)} models\n")

    # Parse each and display info
    models = []
    for idx, (offset, glb_data) in enumerate(glbs):
        try:
            gltf_json, bin_data = parse_glb(glb_data)
            meshes = gltf_json.get('meshes', [])
            nodes = gltf_json.get('nodes', [])
            materials = gltf_json.get('materials', [])
            images = gltf_json.get('images', [])
            name = get_model_name(gltf_json)

            node_names = [n.get('name', '') for n in nodes if n.get('name')]

            models.append({
                'index': idx,
                'offset': offset,
                'size': len(glb_data),
                'name': name,
                'mesh_count': len(meshes),
                'material_count': len(materials),
                'image_count': len(images),
                'node_names': node_names,
                'gltf_json': gltf_json,
                'bin_data': bin_data,
            })

            print(f"  [{idx:2d}] {len(glb_data):>10,} bytes  "
                  f"{len(meshes):>3} meshes  {len(materials):>3} materials  "
                  f"{len(images):>3} textures")
            if node_names:
                display = ', '.join(node_names[:6])
                if len(node_names) > 6:
                    display += f'  ... (+{len(node_names) - 6} more)'
                print(f"       nodes: {display}")

        except Exception as e:
            print(f"  [{idx:2d}] ERROR: {e}")

    if args.list_only:
        return

    # Determine which models to convert
    if args.models:
        selected = [int(x.strip()) for x in args.models.split(',')]
    else:
        selected = list(range(len(models)))

    # Output directory
    output_dir = args.output_dir or scenery_dir / 'converted'
    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"\nConverting {len(selected)} models to {output_dir}...")

    for model in models:
        idx = model['index']
        if idx not in selected:
            continue

        print(f"\n  Converting model [{idx}] ({model['mesh_count']} meshes)...")

        gltf_json = model['gltf_json']
        bin_data = model['bin_data']

        # Convert textures and clean up
        cleaned_json, new_bin = convert_glb(
            gltf_json, bin_data,
            texture_dir or Path('.'),
            idx,
            max_texture_size=args.max_texture_size,
        )

        # If texture_dir didn't have all files, try MaterialLibs
        remaining_uris = [
            img.get('uri', '') for img in cleaned_json.get('images', [])
            if img.get('uri')
        ]
        if remaining_uris and matlib_textures:
            for mat_tex_dir in matlib_textures:
                cleaned_json, new_bin = convert_glb(
                    cleaned_json, new_bin, mat_tex_dir, idx,
                    max_texture_size=args.max_texture_size)

        # Remove unresolved image references (e.g. roughness maps not in package)
        for img in cleaned_json.get('images', []):
            if img.get('uri'):
                # Can't resolve — remove the URI so it's a stub
                uri = img.pop('uri')
                # Create a 1x1 white fallback image
                fallback = Image.new('RGB', (1, 1), (255, 255, 255))
                buf = io.BytesIO()
                fallback.save(buf, format='PNG')
                fb_data = buf.getvalue()
                while len(new_bin) % 4 != 0:
                    new_bin = bytearray(new_bin)
                    new_bin.append(0)
                new_bin = bytearray(new_bin)
                bv_offset = len(new_bin)
                new_bin.extend(fb_data)
                bv_index = len(cleaned_json.get('bufferViews', []))
                cleaned_json.setdefault('bufferViews', []).append({
                    'buffer': 0,
                    'byteOffset': bv_offset,
                    'byteLength': len(fb_data),
                })
                img['bufferView'] = bv_index
                img['mimeType'] = 'image/png'
                new_bin = bytes(new_bin)
                # Update buffer size
                if cleaned_json.get('buffers'):
                    cleaned_json['buffers'][0]['byteLength'] = len(new_bin)

        # Build clean GLB
        glb_out = build_glb(cleaned_json, new_bin)

        # Generate output filename
        node_names = model['node_names']
        if node_names:
            # Use first meaningful node name
            base = node_names[0].replace(' ', '_').replace('.', '_')
            base = ''.join(c for c in base if c.isalnum() or c == '_')
        else:
            base = f'model_{idx:02d}'
        out_path = output_dir / f'{base}.glb'

        # Avoid overwriting
        counter = 1
        while out_path.exists():
            out_path = output_dir / f'{base}_{counter}.glb'
            counter += 1

        out_path.write_bytes(glb_out)
        print(f"    -> {out_path.name} ({len(glb_out):,} bytes)")

        # Write .import file to disable Godot mesh compression (prevents garbled geometry)
        import_path = Path(str(out_path) + '.import')
        if not import_path.exists():
            import_path.write_text(
                '[params]\n\nmeshes/force_disable_compression=true\n'
            )

    print(f"\nDone! Converted models are in: {output_dir}")


if __name__ == '__main__':
    main()
