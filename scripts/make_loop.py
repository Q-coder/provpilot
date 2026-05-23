#!/usr/bin/env python3
"""Create a seamlessly loopable OGG from an MP3 engine sound.

Standard loop-crossfade technique:
  - Decode MP3 to raw PCM
  - Trim MP3 encoder padding (~25ms from start)
  - Overlay the tail onto the head with a crossfade so end→start is seamless
  - Cut the tail (now baked into the head)
  - Encode to OGG Vorbis
"""

import array
import os
import subprocess
import sys

SRC = "sounds/c172_engine_inflight.mp3"
OUT = "provpilot/sounds/c172_engine_inflight.ogg"
SR = 44100
CHANNELS = 2
XFADE_SEC = 0.2   # 200ms crossfade
TRIM_START = 0.025  # 25ms MP3 padding to trim

def main():
    base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src = os.path.join(base, SRC)
    out = os.path.join(base, OUT)

    if not os.path.isfile(src):
        print(f"ERROR: Source not found: {src}")
        sys.exit(1)

    # Decode MP3 → raw PCM float32 interleaved
    proc = subprocess.run([
        "ffmpeg", "-y", "-i", src,
        "-f", "f32le", "-acodec", "pcm_f32le",
        "-ac", str(CHANNELS), "-ar", str(SR), "-"
    ], capture_output=True)
    if proc.returncode != 0:
        print("ffmpeg decode failed:", proc.stderr.decode())
        sys.exit(1)

    samples = array.array("f", proc.stdout)
    n_frames = len(samples) // CHANNELS
    print(f"Decoded: {n_frames} frames ({n_frames/SR:.2f}s)")

    # Trim MP3 padding from start
    trim_frames = int(TRIM_START * SR)
    samples = samples[trim_frames * CHANNELS:]
    n_frames = len(samples) // CHANNELS
    print(f"After trim: {n_frames} frames ({n_frames/SR:.2f}s)")

    # Crossfade length in frames
    xfade = int(XFADE_SEC * SR)

    # Standard loop crossfade:
    # Overlay the LAST xfade frames onto the FIRST xfade frames,
    # with tail fading out and head fading in.
    # Then truncate the last xfade frames (they're now baked into the start).
    for i in range(xfade):
        t = i / float(xfade)  # 0 at start → 1 at end of crossfade
        for ch in range(CHANNELS):
            head_idx = i * CHANNELS + ch
            tail_idx = (n_frames - xfade + i) * CHANNELS + ch
            head_val = samples[head_idx]
            tail_val = samples[tail_idx]
            # Blend: tail fades out (1-t), head fades in (t)
            samples[head_idx] = tail_val * (1.0 - t) + head_val * t

    # Truncate: remove the last xfade frames
    final_frames = n_frames - xfade
    final = samples[:final_frames * CHANNELS]
    print(f"Loop: {final_frames} frames ({final_frames/SR:.2f}s)")

    # Write raw PCM to temp file
    raw_path = "/tmp/engine_loop_raw.pcm"
    with open(raw_path, "wb") as f:
        f.write(final.tobytes())

    # Encode to OGG Vorbis
    proc2 = subprocess.run([
        "ffmpeg", "-y",
        "-f", "f32le", "-ar", str(SR), "-ac", str(CHANNELS), "-i", raw_path,
        "-c:a", "vorbis", "-strict", "-2", "-q:a", "6",
        out
    ], capture_output=True)
    os.unlink(raw_path)

    if proc2.returncode != 0:
        print("ffmpeg encode failed:", proc2.stderr.decode())
        sys.exit(1)

    size = os.path.getsize(out)
    print(f"Output: {out} ({size} bytes)")
    print("Done — seamless loop ready!")

if __name__ == "__main__":
    main()
