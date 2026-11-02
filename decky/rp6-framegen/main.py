"""RP6 FrameGen (Decky backend): installs the rp6fg Vulkan layer and drives it live.

The layer (bin/librp6fg.so) is an implicit Vulkan layer enabled per game by the
launch option prefix RP6FG=1 (set by the frontend). While a game runs it maps
/dev/shm/rp6fg-live-<uid>; this backend writes the on/off switch and the
multiplier there and reads back the layer's once-a-second statistics.
"""
import asyncio
import hashlib
import json
import os
import pwd
import struct
import subprocess
import tempfile
import threading
import time

import decky

PLUGIN_DIR = os.path.dirname(os.path.realpath(__file__))
HOME = decky.DECKY_USER_HOME
USER = pwd.getpwnam(decky.DECKY_USER)
LIB_DIR = os.path.join(HOME, ".local/lib/rp6fg")
LIB = os.path.join(LIB_DIR, "librp6fg.so")
MANIFEST = os.path.join(HOME, ".local/share/vulkan/implicit_layer.d/rp6fg.json")
# /dev/shm is shared with Steam Runtime containers; /run/user/<uid> is not.
LIVE = f"/dev/shm/rp6fg-live-{USER.pw_uid}"

# Mirrors rp6fg::LiveControl (src/live.hpp).
LIVE_FMT = "<IIIIIIIIffffQII"
LIVE_SIZE = struct.calcsize(LIVE_FMT)
LIVE_MAGIC = 0x46365052
LIVE_VERSION = 2
OFF_ENABLED, OFF_MULT, OFF_ADAPTIVE, OFF_REFRESH = 8, 12, 56, 60
assert LIVE_SIZE == 64


def _digest(path):
    try:
        with open(path, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()
    except OSError:
        return None


def _own(path):
    os.chown(path, USER.pw_uid, USER.pw_gid)


def _write_atomic(path, data, mode):
    """Write a unique temp file next to the target and rename it over: concurrent
    writers never share a temp name, and games that have the old file mapped keep it."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".rp6fg-")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.chmod(tmp, mode)
        _own(tmp)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


def install_layer():
    src = os.path.join(PLUGIN_DIR, "bin", "librp6fg.so")
    if _digest(src) != _digest(LIB):
        with open(src, "rb") as f:
            _write_atomic(LIB, f.read(), 0o755)
    with open(os.path.join(PLUGIN_DIR, "bin", "rp6fg_layer.json.in")) as f:
        manifest = f.read().replace("@LIB@", LIB).encode()
    try:
        with open(MANIFEST, "rb") as f:
            same = f.read() == manifest
    except OSError:
        same = False
    if not same:
        _write_atomic(MANIFEST, manifest, 0o644)


def remove_layer():
    for path in (MANIFEST, LIB):
        try:
            os.remove(path)
        except OSError:
            pass


def _kscreen(*args):
    """kscreen-doctor in the user's Plasma session (the backend has no session environment)."""
    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=f"unix:path=/run/user/{USER.pw_uid}/bus",
               XDG_RUNTIME_DIR=f"/run/user/{USER.pw_uid}", WAYLAND_DISPLAY="wayland-0")
    return subprocess.run(["kscreen-doctor", *args], capture_output=True, text=True, timeout=5, env=env,
                          user=USER.pw_uid if os.geteuid() == 0 else None).stdout


def _panel():
    """(output name, current refresh, {refresh: mode id at the current size}) of the
    built-in panel (DSI), else the first enabled output. One kscreen-doctor call:
    it costs ~0.6 s of CPU, so callers reuse the result."""
    outs = [o for o in json.loads(_kscreen("-j"))["outputs"] if o.get("enabled")]
    outs.sort(key=lambda o: not o["name"].startswith("DSI"))
    for o in outs:
        cur = next((m for m in o["modes"] if m["id"] == o.get("currentModeId")), None)
        if cur:
            modes = {int(round(m["refreshRate"])): m["id"] for m in o["modes"] if m["size"] == cur["size"]}
            return o["name"], int(round(cur["refreshRate"])), modes
    raise RuntimeError("no enabled display")


def detect_refresh():
    """Refresh rate of the panel, or None if it cannot be read."""
    try:
        return _panel()[1]
    except Exception:
        return None


def set_refresh(hz):
    """Switch the panel to this refresh rate if needed; returns (before, now), None where unknown."""
    try:
        name, cur, modes = _panel()
    except Exception:
        decky.logger.exception("reading the display mode failed")
        return None, None
    if hz == cur or hz not in modes:
        return cur, cur
    try:
        _kscreen(f"output.{name}.mode.{modes[hz]}")
        decky.logger.info("display %s %d -> %d Hz", name, cur, hz)
        return cur, hz
    except Exception:
        decky.logger.exception("refresh switch to %s Hz failed", hz)
        return cur, cur


def _live_open():
    """Open the control block, (re)initialising it when missing or from another version."""
    # O_NOFOLLOW: /dev/shm is world-writable. fallocate, not truncate: a sparse
    # tmpfs page could SIGBUS the game that maps it if /dev/shm fills up.
    fd = os.open(LIVE, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o644)
    head = os.pread(fd, 8, 0)
    if len(head) < 8 or struct.unpack("<II", head) != (LIVE_MAGIC, LIVE_VERSION):
        os.posix_fallocate(fd, 0, 4096)
        os.pwrite(fd, struct.pack("<IIII", LIVE_MAGIC, LIVE_VERSION, 1, 2), 0)
        os.pwrite(fd, struct.pack("<II", 1, 120), OFF_ADAPTIVE)
        _own(LIVE)
    return fd


def read_live():
    fd = _live_open()
    try:
        raw = os.pread(fd, LIVE_SIZE, 0)
    finally:
        os.close(fd)
    (magic, version, enabled, mult, appid, width, height, active_mult,
     real_fps, out_fps, cpu_us, submit_us, heartbeat, adaptive, refresh) = struct.unpack(LIVE_FMT, raw)
    # The layer stamps CLOCK_MONOTONIC once a second while a game presents.
    running = heartbeat and time.monotonic_ns() - heartbeat < 3_000_000_000
    return {
        "enabled": bool(enabled),
        "mult": mult if 2 <= mult <= 4 else 2,
        "adaptive": bool(adaptive),
        "refresh": refresh,
        "running": bool(running),
        "appid": appid if running else 0,
        "width": width, "height": height,
        "active_mult": active_mult if running else 0,
        "real_fps": round(real_fps, 1) if running else 0,
        "out_fps": round(out_fps, 1) if running else 0,
        "cpu_us": round(cpu_us, 1) if running else 0,
        "submit_us": round(submit_us, 1) if running else 0,
    }


def write_live(offset, value):
    fd = _live_open()
    try:
        os.pwrite(fd, struct.pack("<I", int(value)), offset)
    finally:
        os.close(fd)


# ---- per-game profiles ------------------------------------------------------------
# Like RP6 Gyro: the running game is the "AppId=<n>" argument of Steam's
# "SteamLaunch" reaper process (works for every library, internal or SD card).
# Its profile picks the display refresh the generator aims for (60 or 120 Hz),
# adaptive mode and the (max) multiplier. Profile 0 holds the defaults every
# game inherits. Leaving the game restores the refresh rate from before it,
# unless the user changed it meanwhile; the pending restore is kept on disk so a
# crash or reload mid-game does not leave the panel at the game's rate.
PROFILES = os.path.join(HOME, ".config/rp6fg/profiles.json")
RESTORE = os.path.join(HOME, ".config/rp6fg/restore.json")
BUILTIN = {"target_hz": 120, "adaptive": True, "mult": 2}
LOCK = threading.Lock()  # profile file, live block writes and display switching


def running_appid():
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                args = f.read(4096).split(b"\0")
        except OSError:
            continue
        if any(a.endswith(b"reaper") for a in args) and b"SteamLaunch" in args:
            for a in args:
                if a.startswith(b"AppId=") and a[6:].isdigit():
                    return int(a[6:])
    return 0


def _clean(key, value):
    """Validated profile value, or None."""
    if key == "target_hz":
        try:
            return 60 if int(value) == 60 else 120
        except (TypeError, ValueError):
            return None
    if key == "mult":
        try:
            return max(2, min(4, int(value)))
        except (TypeError, ValueError):
            return None
    if key == "adaptive":
        if isinstance(value, str):
            return value.strip().lower() in ("1", "true", "yes", "on")
        return bool(value) if isinstance(value, (bool, int)) else None
    return None


def load_profiles():
    """{appid: {key: value}} with anything malformed dropped."""
    try:
        with open(PROFILES) as f:
            data = json.load(f)
    except (OSError, ValueError):
        return {}
    out = {}
    for k, v in (data.items() if isinstance(data, dict) else []):
        if not (isinstance(k, str) and k.isdigit() and isinstance(v, dict)):
            continue
        entry = {key: _clean(key, v[key]) for key in BUILTIN if key in v}
        out[k] = {key: val for key, val in entry.items() if val is not None}
    return out


def profile(appid):
    data = load_profiles()
    p = dict(BUILTIN)
    p.update(data.get("0", {}))
    p.update(data.get(str(appid), {}))
    return p


def save_profile(appid, key, value):
    """Store one key; a game's entry only holds what differs from inheriting."""
    data = load_profiles()
    entry = data.setdefault(str(appid), {})
    entry[key] = value
    _write_atomic(PROFILES, json.dumps(data, indent=1).encode(), 0o644)
    _own(os.path.dirname(PROFILES))


def _load_restore():
    try:
        with open(RESTORE) as f:
            r = json.load(f)
        return int(r["restore"]), int(r["set"])
    except (OSError, ValueError, KeyError, TypeError):
        return None


class Follower:
    """Applies the running game's profile; restores the display when it ends."""

    def __init__(self):
        self.appid = 0
        self.stopping = False

    def _apply_profile(self, appid, switch_display):
        p = profile(appid)
        # Generator settings first: the game creates its swapchain while we are
        # still talking to kscreen-doctor, and a later change would re-create it.
        write_live(OFF_MULT, p["mult"])
        write_live(OFF_ADAPTIVE, 1 if p["adaptive"] else 0)
        if switch_display:
            before, now = set_refresh(p["target_hz"])
            if now:
                write_live(OFF_REFRESH, now)
            if before and now and before != now and not _load_restore():
                _write_atomic(RESTORE, json.dumps({"restore": before, "set": now}).encode(), 0o644)
        decky.logger.info("game %d: %s", appid, p)

    def restore(self):
        """Put the panel back as it was before the game, unless the user changed it."""
        r = _load_restore()
        if not r:
            return
        before, ours = r
        cur = detect_refresh()
        if cur == ours:
            _, now = set_refresh(before)
            if now:
                write_live(OFF_REFRESH, now)
            if now != before:
                return  # keep the record and try again next time
        elif cur:
            write_live(OFF_REFRESH, cur)
        try:
            os.remove(RESTORE)
        except OSError:
            pass

    def tick(self):
        appid = running_appid()
        with LOCK:
            if self.stopping or appid == self.appid:
                return
            self.appid = appid
            if appid:
                self._apply_profile(appid, switch_display=True)
            else:
                self.restore()

    def profile_changed(self, appid, key):
        with LOCK:
            if not self.stopping and appid and appid == self.appid:
                self._apply_profile(appid, switch_display=key == "target_hz")


FOLLOWER = Follower()


def state(appid=None):
    pick = FOLLOWER.appid if appid is None else max(0, int(appid))
    return {"installed": os.path.exists(MANIFEST) and os.path.exists(LIB), "live": read_live(),
            "running_appid": FOLLOWER.appid, "profile_appid": pick, "profile": profile(pick),
            "refresh_modes": refresh_modes()}


_modes = None


def refresh_modes():
    """Refresh rates the panel offers at its resolution (cached once known)."""
    global _modes
    if _modes is None:
        try:
            _modes = sorted(_panel()[2])
        except Exception:
            return [60, 120]
    return _modes


def set_profile(appid, key, value):
    appid = int(appid)
    value = _clean(key, value)
    if appid < 0 or value is None:
        raise ValueError(f"bad profile setting {appid}/{key}={value!r}")
    with LOCK:
        save_profile(appid, key, value)
    FOLLOWER.profile_changed(appid, key)  # applies right away if that game is running
    return state(appid)


class Plugin:
    async def get_state(self, appid=None):
        return await asyncio.to_thread(state, appid)

    async def set_enabled(self, enabled, appid=None):
        await asyncio.to_thread(write_live, OFF_ENABLED, 1 if enabled else 0)
        return await asyncio.to_thread(state, appid)

    async def set_profile(self, appid, key, value):
        return await asyncio.to_thread(set_profile, appid, key, value)

    async def _follow(self):
        while True:
            try:
                await asyncio.to_thread(FOLLOWER.tick)
            except Exception:
                decky.logger.exception("game follower")
            await asyncio.sleep(1)

    async def _main(self):
        try:
            await asyncio.to_thread(install_layer)
            decky.logger.info("rp6fg layer installed at %s", LIB)
            hz = await asyncio.to_thread(detect_refresh)
            if hz:
                await asyncio.to_thread(write_live, OFF_REFRESH, hz)
            # A previous instance may have died mid-game: restore now if no game runs.
            if not await asyncio.to_thread(running_appid):
                await asyncio.to_thread(FOLLOWER.restore)
        except Exception:
            decky.logger.exception("rp6fg startup failed")
        self.follow_task = asyncio.get_event_loop().create_task(self._follow())

    async def _unload(self):
        task = getattr(self, "follow_task", None)
        if task:
            task.cancel()

        def stop():
            with LOCK:  # waits for an apply already running in a worker thread
                FOLLOWER.stopping = True
                FOLLOWER.restore()
        # Do not leave the panel at a game's refresh rate.
        await asyncio.to_thread(stop)

    async def _uninstall(self):
        await asyncio.to_thread(remove_layer)
