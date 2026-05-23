#!/usr/bin/env python3
"""Validate GLB files for accessor/bufferView size mismatches."""

import json
import struct
import sys
from pathlib import Path

COMP_SIZE = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}
TYPE_N = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT2': 4, 'MAT3': 9, 'MAT4': 16}

def validate_glb(glb_path):
    data = glb_path.read_bytes()
    magic, version, length = struct.unpack_from('<III', data, 0)
    json_len, json_type = struct.unpack_from('<II', data, 12)
    json_str = data[20:20+json_len].decode('utf-8').rstrip()
    gltf = json.loads(json_str)

    bin_offset = 20 + json_len
    while bin_offset % 4:
        bin_offset += 1
    bin_len, bin_type = struct.unpack_from('<II', data, bin_offset)
    bin_start = bin_offset + 8
    bin_data = data[bin_start:bin_start+bin_len]

    accessors = gltf.get('accessors', [])
    bvs = gltf.get('bufferViews', [])
    bufs = gltf.get('buffers', [])

    errors = []

    # Check buffer size vs actual binary
    if bufs:
        declared = bufs[0]['byteLength']
        if declared != len(bin_data):
            errors.append(f'  buffer[0].byteLength={declared} != actual bin_size={len(bin_data)}')

    for i, acc in enumerate(accessors):
        bv_idx = acc.get('bufferView')
        if bv_idx is None:
            continue
        if bv_idx >= len(bvs):
            errors.append(f'  acc[{i}]: bufferView={bv_idx} out of range (only {len(bvs)} bvs)')
            continue
        bv = bvs[bv_idx]

        comp_size = COMP_SIZE.get(acc['componentType'], 4)
        n_comps = TYPE_N.get(acc['type'], 1)
        count = acc['count']
        acc_offset = acc.get('byteOffset', 0)
        stride = bv.get('byteStride', comp_size * n_comps)

        if count > 0:
            required = (count - 1) * stride + comp_size * n_comps + acc_offset
        else:
            required = 0
        bv_length = bv['byteLength']
        bv_offset = bv.get('byteOffset', 0)

        if required > bv_length:
            errors.append(
                f'  acc[{i}] type={acc["type"]} comp={acc["componentType"]} '
                f'count={count} bv={bv_idx} acc_off={acc_offset}: '
                f'needs {required} but bv has {bv_length} (bv_off={bv_offset}, stride={stride})'
            )

        if bv_offset + bv_length > len(bin_data):
            errors.append(
                f'  acc[{i}] bv[{bv_idx}]: bv_offset({bv_offset})+bv_length({bv_length})'
                f'={bv_offset+bv_length} > bin_size({len(bin_data)})'
            )

    return errors


if __name__ == '__main__':
    glb_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('.')
    if glb_dir.is_file():
        paths = [glb_dir]
    else:
        paths = sorted(glb_dir.glob('*.glb'))

    total_errors = 0
    for p in paths:
        errors = validate_glb(p)
        if errors:
            print(f'ERRORS in {p.name}:')
            for e in errors:
                print(e)
            total_errors += len(errors)
        else:
            print(f'OK: {p.name}')

    if total_errors:
        print(f'\n{total_errors} errors found')
    else:
        print(f'\nAll {len(paths)} files OK')
