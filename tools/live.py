#!/usr/bin/env python3
"""Read or change rp6fg's live control block (/dev/shm/rp6fg-live-<uid>).
Usage: live.py [enabled=0|1] [mult=2..4] [adaptive=0|1] [refresh=Hz]"""
import os, struct, sys
PATH = "/dev/shm/rp6fg-live-%d" % os.getuid()
if not os.path.exists(PATH):
    sys.exit("no live block yet (no game with RP6FG=1 has created a swapchain)")
with open(PATH, "r+b") as f:
    for arg in sys.argv[1:]:
        k, v = arg.split("=")
        f.seek({"enabled": 8, "mult": 12, "adaptive": 56, "refresh": 60}[k]); f.write(struct.pack("<I", int(v)))
    f.seek(0)
    magic, ver, en, mult, appid, w, h, am, rf, of, cpu, sub, hb, ad, hz = struct.unpack("<IIIIIIIIffffQII", f.read(64))
    print(f"v{ver} enabled={en} mult={mult} adaptive={ad} {hz}Hz | app={appid} {w}x{h} active=x{am} real={rf:.1f} out={of:.1f} fps cpu={cpu:.1f}us submit={sub:.1f}us")
