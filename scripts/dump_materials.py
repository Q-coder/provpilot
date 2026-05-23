#!/usr/bin/env python3
"""Dump materials and check for issues in converted GLB."""
import json, struct, sys
from pathlib import Path

def parse_glb(data):
    offset = 12
    json_chunk = None
    while offset < len(data):
        chunk_len, chunk_type = struct.unpack_from('<II', data, offset)
        chunk_data = data[offset+8:offset+8+chunk_len]
        if chunk_type == 0x4E4F534A:
            text = chunk_data.rstrip(b'\x00 \t\n\r')
            json_chunk = json.loads(text)
        offset += 8 + chunk_len
    return json_chunk

glb = Path(sys.argv[1]).read_bytes()
gltf = parse_glb(glb)

# Materials
mats = gltf.get('materials', [])
print(f"Materials: {len(mats)}")
for i, mat in enumerate(mats):
    pbr = mat.get('pbrMetallicRoughness', {})
    base_color = pbr.get('baseColorFactor', [1,1,1,1])
    metallic = pbr.get('metallicFactor', 1.0)
    roughness = pbr.get('roughnessFactor', 1.0)
    base_tex = pbr.get('baseColorTexture')
    mr_tex = pbr.get('metallicRoughnessTexture')
    normal_tex = mat.get('normalTexture')
    emissive = mat.get('emissiveFactor', [0,0,0])
    alpha_mode = mat.get('alphaMode', 'OPAQUE')
    double_sided = mat.get('doubleSided', False)
    
    print(f"\n  mat[{i}]: '{mat.get('name', '')}' alpha={alpha_mode} double={double_sided}")
    print(f"    baseColor={base_color} metallic={metallic} roughness={roughness}")
    if base_tex:
        print(f"    baseColorTexture: {base_tex}")
    if mr_tex:
        print(f"    metallicRoughnessTexture: {mr_tex}")
    if normal_tex:
        print(f"    normalTexture: {normal_tex}")
    if any(e > 0 for e in emissive):
        print(f"    emissive: {emissive}")
    
    # Check for remaining extensions
    ext = mat.get('extensions', {})
    if ext:
        print(f"    EXTENSIONS: {list(ext.keys())}")

# Images
images = gltf.get('images', [])
print(f"\n\nImages: {len(images)}")
for i, img in enumerate(images):
    uri = img.get('uri', '')
    bv = img.get('bufferView')
    mime = img.get('mimeType', '')
    name = img.get('name', '')
    if uri:
        print(f"  img[{i}]: URI={uri} (UNRESOLVED!)")
    else:
        print(f"  img[{i}]: bv={bv} mime={mime} name={name}")

# Textures
textures = gltf.get('textures', [])
print(f"\nTextures: {len(textures)}")
for i, tex in enumerate(textures):
    src = tex.get('source')
    sampler = tex.get('sampler')
    print(f"  tex[{i}]: source=img[{src}] sampler={sampler}")

# Nodes with transforms
nodes = gltf.get('nodes', [])
print(f"\nNodes with transforms ({len(nodes)} total):")
for i, node in enumerate(nodes):
    t = node.get('translation')
    r = node.get('rotation')
    s = node.get('scale')
    m = node.get('matrix')
    mesh = node.get('mesh')
    if t or r or s or m:
        parts = []
        if t: parts.append(f"T={[round(v,2) for v in t]}")
        if r: parts.append(f"R={[round(v,3) for v in r]}")
        if s: parts.append(f"S={[round(v,3) for v in s]}")
        if m: parts.append(f"M={[round(v,2) for v in m]}")
        print(f"  node[{i}] '{node.get('name','')}' mesh={mesh}: {' '.join(parts)}")
