#!/usr/bin/env python3
"""Convert the layer's PPM dumps in a directory to PNG (stdlib only)."""
import os, struct, sys, zlib

def ppm2png(src, dst):
    data = open(src, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    raw = b"".join(b"\0" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    with open(dst, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))

for name in sorted(os.listdir(sys.argv[1])):
    if name.endswith(".ppm"):
        ppm2png(os.path.join(sys.argv[1], name), os.path.join(sys.argv[1], name[:-4] + ".png"))
