#!/usr/bin/env python3
"""Validate the new GLB structure - check separate index bufferViews."""
import json, struct
from pathlib import Path

def parse_glb(data):
    o = 12
    j = None; b = b''
    while o < len(data):
        cl, ct = struct.unpack_from('<II', data, o)
        cd = data[o+8:o+8+cl]
        if ct == 0x4E4F534A: j = json.loads(cd.rstrip(b'\x00 '))
        elif ct == 0x004E4942: b = cd
        o += 8 + cl
    return j, b

g, b = parse_glb(Path('/tmp/provpilot_converted/Hangar_rechts.glb').read_bytes())

m0 = g['meshes'][0]
print(f"mesh[0] '{m0.get('name','')}' prims: {len(m0['primitives'])}")
for pi, p in enumerate(m0['primitives'][:5]):
    idx_acc = g['accessors'][p['indices']]
    idx_bv = g['bufferViews'][idx_acc['bufferView']]
    pos_acc_idx = p['attributes']['POSITION']
    pos_acc = g['accessors'][pos_acc_idx]
    print(f"  prim[{pi}]: idx byteOff={idx_acc.get('byteOffset',0)}, "
          f"bv={idx_acc['bufferView']} count={idx_acc['count']} comp={idx_acc['componentType']} "
          f"pos_acc={pos_acc_idx} vtx={pos_acc['count']}")

has_asobo = json.dumps(g).count('ASOBO')
print(f"\nRemaining ASOBO refs: {has_asobo}")
print(f"Asset: {g.get('asset', {})}")
print(f"extensionsUsed: {g.get('extensionsUsed', [])}")

# Verify each prim has UNIQUE index bufferView
idx_bvs = set()
all_unique = True
for p in m0['primitives']:
    bv = g['accessors'][p['indices']]['bufferView']
    if bv in idx_bvs:
        all_unique = False
    idx_bvs.add(bv)
print(f"\nAll index bufferViews unique: {all_unique}")
print(f"Total bufferViews: {len(g['bufferViews'])}, Accessors: {len(g['accessors'])}")

# Read actual index data to verify correctness
print("\n--- Index data verification ---")
for pi, p in enumerate(m0['primitives'][:3]):
    idx_acc = g['accessors'][p['indices']]
    idx_bv = g['bufferViews'][idx_acc['bufferView']]
    comp = idx_acc['componentType']
    fmt = {5121: 'B', 5123: 'H', 5125: 'I'}.get(comp, 'H')
    sz = struct.calcsize('<' + fmt)
    bv_off = idx_bv.get('byteOffset', 0)
    acc_off = idx_acc.get('byteOffset', 0)
    
    indices = []
    mn, mx = 99999, 0
    for i in range(idx_acc['count']):
        val = struct.unpack_from('<' + fmt, b, bv_off + acc_off + i * sz)[0]
        if i < 6: indices.append(val)
        mn = min(mn, val)
        mx = max(mx, val)
    
    vtx_count = g['accessors'][p['attributes']['POSITION']]['count']
    ok = "OK" if mx < vtx_count else "OUT OF RANGE!"
    print(f"  prim[{pi}]: count={idx_acc['count']} range=[{mn}..{mx}] vtx={vtx_count} "
          f"first6={indices} {ok}")
