#!/usr/bin/env python3
"""Trigger an rp6fg frame dump while the right stick is held deflected (camera turning).
Usage: stick_dump.py /dev/input/eventN [threshold=0.5] [hold_s=0.3]"""
import fcntl, os, struct, sys, time

dev = sys.argv[1]
thr = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5
hold = float(sys.argv[3]) if len(sys.argv) > 3 else 0.3
ABS_RX, ABS_RY, EV_ABS = 3, 4, 3

def absinfo(fd, code):
    buf = bytearray(24)  # value, min, max, fuzz, flat, resolution
    fcntl.ioctl(fd, 0x80184540 + code, buf)  # EVIOCGABS(code)
    return struct.unpack("<6i", buf)

fd = os.open(dev, os.O_RDONLY)
info = {c: absinfo(fd, c) for c in (ABS_RX, ABS_RY)}
val = {c: info[c][0] for c in info}
norm = lambda c: (val[c] - (info[c][1] + info[c][2]) / 2) / ((info[c][2] - info[c][1]) / 2)
since = None
fmt = "llHHi"
size = struct.calcsize(fmt)
while True:
    _, _, typ, code, v = struct.unpack(fmt, os.read(fd, size))
    if typ != EV_ABS or code not in val:
        continue
    val[code] = v
    mag = max(abs(norm(ABS_RX)), abs(norm(ABS_RY)))
    now = time.monotonic()
    if mag >= thr:
        since = since or now
        if now - since >= hold:
            # Runs as root (the pad is root-only); the game must be able to unlink
            # the flag in sticky /dev/shm, so it has to belong to the invoking user.
            open("/dev/shm/rp6fg-dump-now", "w").close()
            if os.environ.get("SUDO_UID"):
                os.chown("/dev/shm/rp6fg-dump-now", int(os.environ["SUDO_UID"]), int(os.environ["SUDO_GID"]))
            print(f"dump triggered, stick {norm(ABS_RX):+.2f} {norm(ABS_RY):+.2f}", flush=True)
            break
    else:
        since = None
