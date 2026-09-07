#!/usr/bin/env python3
"""List entries of a Godot 4 (v4) .pck archive (index only).

Header: magic u32, version u32, ver_major u32, ver_minor u32, ver_patch u32,
        flags u32, file_base u64, dir_offset u64, reserved 16*u32.
Directory (at dir_offset): count u32, then per entry:
        sl u32 (len incl. pad), path[sl], ofs u64, size u64, md5[16], flags u32.
"""
import struct, sys, collections

path = sys.argv[1]
buf = open(path, 'rb').read()
magic, ver, maj, minor, patch, flags = struct.unpack_from('<IIIIII', buf, 0)
print(f'magic={magic:#x} version={ver} godot={maj}.{minor}.{patch} flags={flags:#x}')
file_base, dir_offset = struct.unpack_from('<QQ', buf, 24)
off = dir_offset
count = struct.unpack_from('<I', buf, off)[0]
off += 4
print(f'files: {count}')
entries = []
for i in range(count):
    sl = struct.unpack_from('<I', buf, off)[0]
    off += 4
    name = buf[off:off + sl].rstrip(b'\x00').decode()
    off += sl
    o, s = struct.unpack_from('<QQ', buf, off)
    off += 16
    off += 16  # md5
    fl = struct.unpack_from('<I', buf, off)[0]
    off += 4
    entries.append((name, s, fl))

exts = collections.Counter(n.rsplit('.', 1)[-1] for n, _, _ in entries)
print('top extensions:', exts.most_common(10))
print()
pat = sys.argv[2] if len(sys.argv) > 2 else None
for n, s, fl in entries:
    if pat is not None:
        if pat.lower() in n.lower():
            print(f'{s:>12}  {n}')
    elif 'dylib' in n or '/faces/' in n or n.startswith('faces/') or n.endswith('.gdextension'):
        print(f'{s:>12}  {n}')

