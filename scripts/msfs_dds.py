#!/usr/bin/env python3
"""msfs_dds.py — decode the DDS textures of MSFS packages to RGBA and PNG.

Block-compressed formats decoded here (numpy, no Pillow needed):
  DXT1 (BC1), DXT3 (BC2, alpha ignored -> 4-bit), DXT5 (BC3), BC4 unsigned,
  BC5 unsigned / signed (ATI2, BC5U, BC5S: two-channel normal maps).
Anything else (BC7 via the DX10 header, uncompressed formats) is reported as
unsupported; the converter then writes a flat placeholder.

    python3 scripts/msfs_dds.py <file.dds> <out.png> [--normal-to-opengl]

`--normal-to-opengl` flips the green channel (MSFS ships DirectX-convention
normal maps, Godot expects OpenGL) and rebuilds blue for two-channel maps.
"""
import struct
import sys
import zlib
from typing import Optional, Tuple

import numpy as np

DDS_MAGIC = b"DDS "
DDPF_FOURCC = 0x4
DDPF_RGB = 0x40
DDPF_ALPHAPIXELS = 0x1


class DdsError(ValueError):
    pass


def _rgb565_to_rgb8(c: np.ndarray) -> np.ndarray:
    """(n,) uint16 -> (n, 3) uint8 with the usual bit replication."""
    r = ((c >> 11) & 0x1F).astype(np.uint16)
    g = ((c >> 5) & 0x3F).astype(np.uint16)
    b = (c & 0x1F).astype(np.uint16)
    r = (r << 3) | (r >> 2)
    g = (g << 2) | (g >> 4)
    b = (b << 3) | (b >> 2)
    return np.stack([r, g, b], axis=1).astype(np.uint8)


def _bc1_blocks(raw: np.ndarray, force_four: bool) -> Tuple[np.ndarray, np.ndarray]:
    """raw: (n, 8) uint8 -> rgb (n, 16, 3) uint8, alpha (n, 16) uint8."""
    n = raw.shape[0]
    c0 = raw[:, 0].astype(np.uint16) | (raw[:, 1].astype(np.uint16) << 8)
    c1 = raw[:, 2].astype(np.uint16) | (raw[:, 3].astype(np.uint16) << 8)
    p0 = _rgb565_to_rgb8(c0).astype(np.int32)
    p1 = _rgb565_to_rgb8(c1).astype(np.int32)
    four = (c0 > c1) | force_four
    p2 = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    palette = np.stack([p0, p1, p2, p3], axis=1).astype(np.uint8)  # (n, 4, 3)
    bits = raw[:, 4].astype(np.uint32) | (raw[:, 5].astype(np.uint32) << 8) | \
        (raw[:, 6].astype(np.uint32) << 16) | (raw[:, 7].astype(np.uint32) << 24)
    idx = np.stack([(bits >> (2 * i)) & 3 for i in range(16)], axis=1)  # (n, 16)
    rgb = palette[np.arange(n)[:, None], idx]  # (n, 16, 3)
    alpha = np.where((~four)[:, None] & (idx == 3), 0, 255).astype(np.uint8)
    return rgb, alpha


def _bc4_blocks(raw: np.ndarray, signed: bool) -> np.ndarray:
    """raw: (n, 8) uint8 -> (n, 16) float32 in 0..1 (unsigned) or -1..1 (signed)."""
    n = raw.shape[0]
    if signed:
        e0 = raw[:, 0].astype(np.int8).astype(np.float32)
        e1 = raw[:, 1].astype(np.int8).astype(np.float32)
        e0 = np.maximum(e0, -127.0) / 127.0
        e1 = np.maximum(e1, -127.0) / 127.0
        lo, hi = -1.0, 1.0
    else:
        e0 = raw[:, 0].astype(np.float32) / 255.0
        e1 = raw[:, 1].astype(np.float32) / 255.0
        lo, hi = 0.0, 1.0
    seven = e0 > e1
    pal = np.empty((n, 8), np.float32)
    pal[:, 0] = e0
    pal[:, 1] = e1
    for i in range(1, 7):  # 7-point ramp
        pal[:, i + 1] = ((7 - i) * e0 + i * e1) / 7.0
    five = np.empty((n, 8), np.float32)
    five[:, 0] = e0
    five[:, 1] = e1
    for i in range(1, 5):
        five[:, i + 1] = ((5 - i) * e0 + i * e1) / 5.0
    five[:, 6] = lo
    five[:, 7] = hi
    pal = np.where(seven[:, None], pal, five)
    bits = np.zeros(n, np.uint64)
    for i in range(6):
        bits |= raw[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    idx = np.stack([((bits >> np.uint64(3 * i)) & np.uint64(7)).astype(np.int64) for i in range(16)], axis=1)
    return pal[np.arange(n)[:, None], idx]


def _blocks_to_image(vals: np.ndarray, width: int, height: int) -> np.ndarray:
    """vals: (n_blocks, 16, C) -> (height, width, C), block rows are row-major."""
    bw, bh = (width + 3) // 4, (height + 3) // 4
    c = vals.shape[2]
    img = vals.reshape(bh, bw, 4, 4, c).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, c)
    return img[:height, :width]


def read_dds(data: bytes) -> Tuple[str, int, int, bytes]:
    """(format, width, height, level-0 payload) of a DDS byte string."""
    if data[:4] != DDS_MAGIC:
        raise DdsError("not a DDS file")
    height, width = struct.unpack_from("<II", data, 12)
    pf_flags = struct.unpack_from("<I", data, 80)[0]
    fourcc = data[84:88]
    rgb_bits = struct.unpack_from("<I", data, 88)[0]
    off = 128
    if pf_flags & DDPF_FOURCC:
        if fourcc == b"DX10":
            dxgi = struct.unpack_from("<I", data, 128)[0]
            off = 148
            fmt = {70: "BC1", 71: "BC1", 72: "BC1", 73: "BC2", 74: "BC2", 75: "BC2",
                   76: "BC3", 77: "BC3", 78: "BC3", 79: "BC4", 80: "BC4", 81: "BC4S",
                   82: "BC5", 83: "BC5", 84: "BC5S", 94: "BC6H", 95: "BC6H", 96: "BC6H",
                   97: "BC7", 98: "BC7", 99: "BC7"}.get(dxgi, "DXGI%d" % dxgi)
        else:
            fmt = {b"DXT1": "BC1", b"DXT2": "BC2", b"DXT3": "BC2", b"DXT4": "BC3", b"DXT5": "BC3",
                   b"ATI1": "BC4", b"BC4U": "BC4", b"BC4S": "BC4S", b"ATI2": "BC5", b"BC5U": "BC5",
                   b"BC5S": "BC5S"}.get(fourcc, fourcc.decode("latin1"))
    elif pf_flags & DDPF_RGB:
        fmt = "RGBA8" if (pf_flags & DDPF_ALPHAPIXELS and rgb_bits == 32) else "RGB%d" % rgb_bits
    else:
        fmt = "unknown"
    return fmt, width, height, data[off:]


def decode_dds(data: bytes) -> Tuple[np.ndarray, str]:
    """Level 0 of a DDS as (height, width, 4) uint8 RGBA. Two-channel formats
    come back with the raw X / Y in R / G (0..255), B = 128, A = 255."""
    fmt, width, height, payload = read_dds(data)
    bw, bh = (width + 3) // 4, (height + 3) // 4
    n = bw * bh
    if fmt == "BC1":
        raw = np.frombuffer(payload, np.uint8, n * 8).reshape(n, 8)
        rgb, a = _bc1_blocks(raw, False)
        rgba = np.concatenate([rgb, a[:, :, None]], axis=2)
    elif fmt == "BC2":
        raw = np.frombuffer(payload, np.uint8, n * 16).reshape(n, 16)
        rgb, _ = _bc1_blocks(raw[:, 8:], True)
        abits = raw[:, :8]
        a = np.stack([((abits[:, i // 2] >> (4 * (i % 2))) & 0xF) * 17 for i in range(16)], axis=1).astype(np.uint8)
        rgba = np.concatenate([rgb, a[:, :, None]], axis=2)
    elif fmt == "BC3":
        raw = np.frombuffer(payload, np.uint8, n * 16).reshape(n, 16)
        rgb, _ = _bc1_blocks(raw[:, 8:], True)
        a = np.clip(_bc4_blocks(raw[:, :8], False) * 255.0 + 0.5, 0, 255).astype(np.uint8)
        rgba = np.concatenate([rgb, a[:, :, None]], axis=2)
    elif fmt in ("BC4", "BC4S"):
        raw = np.frombuffer(payload, np.uint8, n * 8).reshape(n, 8)
        r = _bc4_blocks(raw, fmt == "BC4S")
        if fmt == "BC4S":
            r = r * 0.5 + 0.5
        r8 = np.clip(r * 255.0 + 0.5, 0, 255).astype(np.uint8)
        rgba = np.stack([r8, r8, r8, np.full_like(r8, 255)], axis=2)
    elif fmt in ("BC5", "BC5S"):
        raw = np.frombuffer(payload, np.uint8, n * 16).reshape(n, 16)
        signed = fmt == "BC5S"
        x = _bc4_blocks(raw[:, :8], signed)
        y = _bc4_blocks(raw[:, 8:], signed)
        if signed:
            x = x * 0.5 + 0.5
            y = y * 0.5 + 0.5
        x8 = np.clip(x * 255.0 + 0.5, 0, 255).astype(np.uint8)
        y8 = np.clip(y * 255.0 + 0.5, 0, 255).astype(np.uint8)
        rgba = np.stack([x8, y8, np.full_like(x8, 128), np.full_like(x8, 255)], axis=2)
    elif fmt == "RGBA8":
        img = np.frombuffer(payload, np.uint8, width * height * 4).reshape(height, width, 4)
        return np.ascontiguousarray(img[:, :, [2, 1, 0, 3]]), fmt  # BGRA on disk
    else:
        raise DdsError("unsupported DDS format %s (%dx%d)" % (fmt, width, height))
    return _blocks_to_image(rgba, width, height), fmt


def normal_map_to_opengl(rgba: np.ndarray, two_channel: bool) -> np.ndarray:
    """DirectX normal map -> OpenGL (flip G); two-channel maps get B rebuilt."""
    out = rgba.copy()
    out[:, :, 1] = 255 - out[:, :, 1]
    if two_channel:
        x = out[:, :, 0].astype(np.float32) / 127.5 - 1.0
        y = out[:, :, 1].astype(np.float32) / 127.5 - 1.0
        z = np.sqrt(np.clip(1.0 - x * x - y * y, 0.0, 1.0))
        out[:, :, 2] = np.clip(z * 127.5 + 127.5 + 0.5, 0, 255).astype(np.uint8)
    return out


def downscale(rgba: np.ndarray, max_size: int) -> np.ndarray:
    """Box-filter halving until both sides are <= max_size."""
    while rgba.shape[0] > max_size or rgba.shape[1] > max_size:
        h, w = rgba.shape[0] // 2 * 2, rgba.shape[1] // 2 * 2
        a = rgba[:h, :w].astype(np.uint16)
        rgba = ((a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2] + 2) // 4).astype(np.uint8)
    return rgba


def png_bytes(rgba: np.ndarray, keep_alpha: bool = True) -> bytes:
    """Encode (h, w, 4) uint8 as PNG (RGBA, or RGB when alpha is unused)."""
    h, w = rgba.shape[:2]
    if not keep_alpha or bool(np.all(rgba[:, :, 3] == 255)):
        pixels = np.ascontiguousarray(rgba[:, :, :3])
        color_type = 2
    else:
        pixels = np.ascontiguousarray(rgba)
        color_type = 6
    rows = np.concatenate([np.zeros((h, 1), np.uint8), pixels.reshape(h, -1)], axis=1)  # filter 0
    raw = zlib.compress(rows.tobytes(), 6)

    def chunk(tag: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, color_type, 0, 0, 0))
            + chunk(b"IDAT", raw) + chunk(b"IEND", b""))


def main(argv) -> int:
    if len(argv) < 3:
        print(__doc__)
        return 1
    data = open(argv[1], "rb").read()
    rgba, fmt = decode_dds(data)
    if "--normal-to-opengl" in argv:
        rgba = normal_map_to_opengl(rgba, fmt in ("BC5", "BC5S"))
    open(argv[2], "wb").write(png_bytes(rgba))
    print("%s: %s %dx%d -> %s" % (argv[1], fmt, rgba.shape[1], rgba.shape[0], argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
