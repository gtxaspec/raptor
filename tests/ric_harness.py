#!/usr/bin/env python3
"""Behavioral test harness for ric: scripted AE, observed GPIO, no hardware.

Runs INSIDE `unshare -rm` (test-ric.sh does that): this process is root in
its own user+mount namespace, so it can hide the host's /sys and /run behind
private tmpfs mounts. ric is then launched unmodified and everything it
touches is ours:

  - /var/run/rss/rvd.sock  -> a stub server here, speaking the real
    length-prefixed ctrl protocol. get-exposure answers from a scriptable
    dict (the "scene"); set-running-mode calls are recorded.
  - /sys/class/gpio/gpioN  -> pre-created files on a tmpfs. gpio_export()
    sees direction already present and takes the already-exported path, so
    every actuation is a plain file write we can watch with inotify.
  - /dev/ingenic_adc_aux_N -> only for the ADC scenario, via the LD_PRELOAD
    shim (ric_adc_shim.c) redirecting to a backing file.

Each scenario launches a fresh ric (clean baselines, clean warn-once flags),
drives the scene, and asserts on the GPIO event stream, the recorded
set-running-mode calls, ric's own ctrl socket, and its log.

Pulse-width note: inotify events carry no kernel timestamps; widths are
measured at drain time. That is reliable for the 100 ms pulses asserted
here, but a future ~10 ms pulse can drain as one batch -- assert event
counts and final state for those, not width.
"""
import json
import os
import re
import select
import socket
import struct
import subprocess
import sys
import threading
import time

RUN_DIR = "/var/run/rss"
GPIO_ROOT = "/sys/class/gpio"
IRCUT1, IRCUT2, IRLED, IRLED2 = 52, 53, 49, 50
PINS = (IRCUT1, IRCUT2, IRLED, IRLED2)
WORK = os.environ.get("RIC_TEST_WORK", "/tmp/ric-test")
RIC_BIN = os.environ.get("RIC_BIN", "asan-out/ric")
ADC_PRELOAD = os.environ.get("RIC_ADC_PRELOAD", "")
POLL_MS = 200

PASS = 0
FAIL = 0
FAILED = []


def result(ok, name, detail=""):
    global PASS, FAIL
    if ok:
        PASS += 1
        print("  PASS  %s" % name, flush=True)
    else:
        FAIL += 1
        FAILED.append(name)
        print("  FAIL  %s: %s" % (name, detail), flush=True)


# ── sandbox ──────────────────────────────────────────────────────────


def mount_tmpfs(target):
    subprocess.run(["mount", "-t", "tmpfs", "none", target], check=True)


def setup_sandbox():
    mount_tmpfs("/sys")
    os.makedirs(GPIO_ROOT)
    # gpio_export() checks access(gpioN/direction) and skips the kernel
    # export when it exists, so pre-created plain files are all it takes.
    for pin in PINS:
        d = "%s/gpio%d" % (GPIO_ROOT, pin)
        os.makedirs(d)
        open(d + "/direction", "w").close()
        with open(d + "/value", "w") as f:
            f.write("0")
    open(GPIO_ROOT + "/export", "w").close()
    mount_tmpfs("/run")
    os.makedirs(RUN_DIR, exist_ok=True)
    os.makedirs(WORK, exist_ok=True)
    # Writable /etc (overlay keeps ld.so.cache etc. intact) so the pin
    # discovery scenario controls /etc/thingino.json. Scenarios that set
    # their pins explicitly are unaffected: discovery fills only pins
    # still at -1, and this file defines no ir940.
    up, wk = WORK + "/etc-up", WORK + "/etc-wk"
    os.makedirs(up)
    os.makedirs(wk)
    subprocess.run(
        ["mount", "-t", "overlay", "overlay",
         "-o", "lowerdir=/etc,upperdir=%s,workdir=%s" % (up, wk), "/etc"],
        check=True,
    )
    with open("/etc/thingino.json", "w") as f:
        json.dump({"gpio": {"ircut": "%d %d" % (IRCUT1, IRCUT2), "ir850": IRLED}}, f)


# ── ctrl protocol (2-byte BE length + JSON, half-close after request) ─


def recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def send_msg(conn, payload):
    conn.sendall(struct.pack(">H", len(payload)) + payload)


def recv_msg(conn):
    hdr = recv_exact(conn, 2)
    if hdr is None:
        return None
    (n,) = struct.unpack(">H", hdr)
    return recv_exact(conn, n) if n else b""


def ctrl_cmd(sock_path, obj, timeout=5.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sock_path)
    send_msg(s, json.dumps(obj).encode())
    s.shutdown(socket.SHUT_WR)
    resp = recv_msg(s)
    s.close()
    return json.loads(resp) if resp else None


# ── stub rvd ─────────────────────────────────────────────────────────


class StubRvd:
    """Answers get-exposure from self.scene, records set-running-mode."""

    def __init__(self):
        self.scene = {}
        self.scene_queue = []
        self.modes = []  # (monotonic, "day"|"night")
        self.fps_calls = []  # (monotonic, int value) from set-sensor-fps
        self.fps_error = False  # answer set-sensor-fps with an error
        self.lock = threading.Lock()
        self.path = RUN_DIR + "/rvd.sock"
        self._bind()

    def _bind(self):
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(self.path)
        self.srv.listen(8)
        threading.Thread(target=self._loop, daemon=True).start()

    def pause(self):
        """Simulate rvd dying: close the listener and remove the socket
        so connects fail the way they do against a dead daemon."""
        srv, self.srv = self.srv, None
        srv.close()
        try:
            os.unlink(self.path)
        except OSError:
            pass

    def resume(self):
        """rvd comes back."""
        self._bind()

    def set_scene(self, **kw):
        with self.lock:
            self.scene = dict(kw)
            self.scene_queue = []

    def set_scene_sequence(self, scenes):
        """Return one scripted exposure per query, then hold the last."""
        with self.lock:
            self.scene_queue = [dict(sc) for sc in scenes]
            self.scene = dict(scenes[-1])

    def mark(self):
        with self.lock:
            return len(self.modes)

    def modes_since(self, mark):
        with self.lock:
            return [m for _, m in self.modes[mark:]]

    def fps_mark(self):
        with self.lock:
            return len(self.fps_calls)

    def fps_since(self, mark):
        with self.lock:
            return [v for _, v in self.fps_calls[mark:]]

    def _loop(self):
        srv = self.srv
        while True:
            try:
                conn, _ = srv.accept()
            except OSError:
                return  # paused: this listener is done
            try:
                req = recv_msg(conn)
                if req is None:
                    continue
                cmd = json.loads(req).get("cmd", "")
                if cmd == "get-exposure":
                    with self.lock:
                        if self.scene_queue:
                            sc = self.scene_queue.pop(0)
                        else:
                            sc = dict(self.scene)
                    resp = {
                        "total_gain": sc.get("gain", 0),
                        "exposure_us": sc.get("exposure_us", 10000),
                        "ae_luma": sc.get("luma", 0),
                        "ev": sc.get("ev", 0),
                        "wb_rgain": sc.get("rgain", 0),
                        "wb_bgain": sc.get("bgain", 0),
                    }
                elif cmd == "set-running-mode":
                    val = json.loads(req).get("value", "?")
                    with self.lock:
                        self.modes.append((time.monotonic(), val))
                    resp = {"status": "ok", "mode": val}
                elif cmd == "set-sensor-fps":
                    val = json.loads(req).get("value", -1)
                    with self.lock:
                        err = self.fps_error
                        if not err:
                            self.fps_calls.append((time.monotonic(), val))
                    if err:
                        resp = {"status": "error",
                                "reason": "not supported on this SoC"}
                    else:
                        resp = {"status": "ok", "fps_num": val or 25,
                                "fps_den": 1, "streams": []}
                elif cmd == "get-sensor-fps":
                    resp = {"status": "ok", "fps_num": 25, "fps_den": 1,
                            "base_fps_num": 25, "base_fps_den": 1}
                else:
                    resp = {"status": "ok"}
                send_msg(conn, json.dumps(resp).encode())
            except OSError:
                pass
            finally:
                conn.close()


# ── GPIO watcher (ctypes inotify on the fake sysfs) ──────────────────

IN_CLOSE_WRITE = 0x00000008


class GpioWatch:
    def __init__(self):
        import ctypes

        self.libc = ctypes.CDLL(None, use_errno=True)
        self.fd = self.libc.inotify_init1(0)
        if self.fd < 0:
            raise OSError("inotify_init1 failed")
        self.wd_pin = {}
        for pin in PINS:
            d = ("%s/gpio%d" % (GPIO_ROOT, pin)).encode()
            wd = self.libc.inotify_add_watch(self.fd, d, IN_CLOSE_WRITE)
            self.wd_pin[wd] = pin
        self.events = []  # (monotonic, pin, fname, content)
        self.lock = threading.Lock()
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        while True:
            r, _, _ = select.select([self.fd], [], [], 0.2)
            if not r:
                continue
            data = os.read(self.fd, 4096)
            t = time.monotonic()
            off = 0
            while off < len(data):
                wd, mask, cookie, nlen = struct.unpack_from("iIII", data, off)
                name = data[off + 16 : off + 16 + nlen].split(b"\0")[0].decode()
                off += 16 + nlen
                pin = self.wd_pin.get(wd)
                if pin is None or not name:
                    continue
                try:
                    content = open(
                        "%s/gpio%d/%s" % (GPIO_ROOT, pin, name)
                    ).read().strip()
                except OSError:
                    content = "?"
                with self.lock:
                    self.events.append((t, pin, name, content))

    def mark(self):
        with self.lock:
            return len(self.events)

    def since(self, mark, fname="value"):
        with self.lock:
            return [e for e in self.events[mark:] if e[2] == fname]


# ── ric lifecycle ────────────────────────────────────────────────────


class Ric:
    def __init__(self, name, conf_extra, env_extra=None, poll_ms=POLL_MS, mode="auto"):
        conf = "%s/%s.conf" % (WORK, name)
        with open(conf, "w") as f:
            f.write("[ircut]\nenabled = true\nmode = %s\n" % mode)
            f.write("poll_interval_ms = %d\n" % poll_ms)
            f.write(conf_extra)
            f.write("\n[log]\nlevel = debug\n")
        self.logpath = "%s/%s.log" % (WORK, name)
        self.log = open(self.logpath, "w")
        env = dict(os.environ)
        if env_extra:
            env.update(env_extra)
        self.proc = subprocess.Popen(
            [RIC_BIN, "-c", conf, "-f", "-d"],
            stdout=self.log,
            stderr=subprocess.STDOUT,
            env=env,
        )

    def wait_running(self, timeout=10.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if "ric running" in self.read_log():
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.05)
        return False

    def read_log(self):
        with open(self.logpath) as f:
            return f.read()

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.log.close()
        try:
            os.unlink(RUN_DIR + "/ric.sock")
        except OSError:
            pass


def wait_for(cond, timeout, step=0.05):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if cond():
            return True
        time.sleep(step)
    return False


def ric_status():
    try:
        return ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "status"})
    except OSError:
        return None


# ── assertion helpers ────────────────────────────────────────────────


def find_pulse(events, hi_pin, lo_pin):
    """Locate the dual-GPIO pulse in a value-event list: hi_pin goes 1
    while lo_pin holds 0, then both return 0. Returns the pulse width in
    seconds, or None -- also None if the opposite coil pin ever went high
    during the pulse (that would drive both coil ends)."""
    t_hi = t_end = None
    for t, pin, _, content in events:
        if pin == hi_pin and content == "1" and t_hi is None:
            t_hi = t
        elif t_hi is not None and t_end is None and pin == lo_pin and content == "1":
            return None
        elif pin == hi_pin and content == "0" and t_hi is not None:
            t_end = t
            break
    if t_hi is None or t_end is None:
        return None
    return t_end - t_hi


def wait_pulse(watch, gm, hi_pin, lo_pin, timeout=3.0):
    """Poll until the pulse is visible in the event stream (inotify drain
    lags the writes by scheduler noise), then return its width."""
    wait_for(lambda: find_pulse(watch.since(gm), hi_pin, lo_pin) is not None, timeout)
    return find_pulse(watch.since(gm), hi_pin, lo_pin)


def wait_last(watch, gm, pin, val, timeout=3.0):
    """Poll until pin's most recent value event since gm equals val."""
    return wait_for(lambda: last_value(watch.since(gm), pin) == val, timeout)


def last_value(events, pin):
    vals = [c for _, p, _, c in events if p == pin]
    return vals[-1] if vals else None


GPIO_CONF = (
    "gpio_ircut = %d\ngpio_ircut2 = %d\ngpio_irled = %d\n" % (IRCUT1, IRCUT2, IRLED)
)
LUMA_CONF = (
    GPIO_CONF
    + "trigger = luma\nnight_luma = 20\nnight_gain = 80000\n"
    + "day_gain_pct = 25\nhysteresis_sec = 2\npulse_ms = 100\n"
)


# ── scenarios ────────────────────────────────────────────────────────


def scenario_startup_park(stub, watch):
    """A fresh start in a bright scene parks the filter in day position."""
    stub.set_scene(luma=120, gain=500, ev=4000)
    gm, mm = watch.mark(), stub.mark()
    ric = Ric("park", LUMA_CONF)
    ok = ric.wait_running()
    result(ok, "ric starts against stub rvd", "no 'ric running' in log")
    if ok:
        ok = wait_for(lambda: "day" in stub.modes_since(mm), 5)
        result(ok, "startup parks ISP to day", str(stub.modes_since(mm)))
        w = wait_pulse(watch, gm, IRCUT1, IRCUT2)
        result(
            w is not None and 0.05 < w < 0.3,
            "startup day pulse on ircut pins, opposite pin quiet",
            "width=%s events=%s" % (w, watch.since(gm)),
        )
        ok = wait_last(watch, gm, IRCUT1, "0") and wait_last(watch, gm, IRCUT2, "0")
        result(ok, "coil left de-energized after park", str(watch.since(gm)))
        dirs = watch.since(gm, "direction")
        result(
            all(c == "out" for _, _, _, c in dirs) and len(dirs) >= 3,
            "gpio directions set to out",
            str(dirs),
        )
    ric.stop()


def scenario_startup_ae_walk(stub, watch):
    """RVD is live before cold-start AE reaches a daylight exposure.
    Rising EV must not be counted as consecutive night samples."""
    samples = [
        {"luma": 4, "gain": 256, "ev": 45},
        {"luma": 5, "gain": 256, "ev": 50},
        {"luma": 6, "gain": 257, "ev": 56},
        {"luma": 8, "gain": 256, "ev": 63},
        {"luma": 11, "gain": 256, "ev": 71},
        {"luma": 14, "gain": 257, "ev": 80},
        {"luma": 17, "gain": 256, "ev": 90},
        {"luma": 19, "gain": 256, "ev": 101},
        {"luma": 21, "gain": 256, "ev": 113},
        {"luma": 30, "gain": 256, "ev": 140},
    ]
    stub.set_scene_sequence(samples)
    mm = stub.mark()
    ric = Ric("startup-ae-walk", LUMA_CONF)
    ok = ric.wait_running() and wait_for(
        lambda: "day AE ready" in ric.read_log(), 4
    )
    result(ok, "startup AE walk reaches a qualified reading", ric.read_log())
    modes = stub.modes_since(mm)
    result(
        "night" not in modes and ric_status().get("state") == "day",
        "startup AE walk does not cause a false night switch",
        "modes=%s log=%s" % (modes, ric.read_log()),
    )
    ric.stop()


def scenario_startup_dark(stub, watch):
    """The startup guard must still admit a genuinely dark, settled scene."""
    stub.set_scene(luma=5, gain=20000, ev=100000)
    mm = stub.mark()
    ric = Ric("startup-dark", LUMA_CONF)
    ok = ric.wait_running() and wait_for(
        lambda: "night" in stub.modes_since(mm), 4
    )
    result(ok, "settled dark startup still switches to night", ric.read_log())
    ric.stop()


def scenario_day_switch_ae_walk(stub, watch):
    """Entering day mode restarts AE. Rising EV after a valid dawn decision
    must not fail post-switch verification or debounce straight back to night."""
    stub.set_scene(luma=120, gain=500, ev=4000)
    ric = Ric("day-switch-ae-walk", LUMA_CONF)
    if not ric.wait_running():
        result(False, "day-switch AE walk: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)

    mm = stub.mark()
    stub.set_scene(luma=5, gain=20000, ev=100000)
    if not wait_for(lambda: "night" in stub.modes_since(mm), 4):
        result(False, "day-switch AE walk: night entry", ric.read_log())
        ric.stop()
        return

    # Let the IR-lit night reading become the baseline, then present dawn
    # below the ratio threshold. The mode switch resets AE to a string of
    # underexposed frames before daylight luma returns.
    time.sleep((3 + 2) * POLL_MS / 1000.0)
    mm = stub.mark()
    stub.set_scene(luma=50, gain=4000, ev=20000)
    if not wait_for(lambda: "day" in stub.modes_since(mm), 4):
        result(False, "day-switch AE walk: day entry", ric.read_log())
        ric.stop()
        return

    dm = stub.mark()
    stub.set_scene_sequence([
        {"luma": 4, "gain": 256, "ev": 45},
        {"luma": 5, "gain": 256, "ev": 50},
        {"luma": 6, "gain": 257, "ev": 56},
        {"luma": 8, "gain": 256, "ev": 63},
        {"luma": 11, "gain": 256, "ev": 71},
        {"luma": 14, "gain": 257, "ev": 80},
        {"luma": 17, "gain": 256, "ev": 90},
        {"luma": 19, "gain": 256, "ev": 101},
        {"luma": 21, "gain": 256, "ev": 113},
        {"luma": 30, "gain": 256, "ev": 140},
    ])
    ok = wait_for(lambda: "day AE ready" in ric.read_log(), 4)
    result(ok, "day-switch AE walk reaches a qualified reading", ric.read_log())
    modes = stub.modes_since(dm)
    result(
        "night" not in modes and ric_status().get("state") == "day",
        "day-switch AE walk does not reject a valid dawn",
        "modes=%s log=%s" % (modes, ric.read_log()),
    )
    ric.stop()


def scenario_dusk_dawn(stub, watch):
    """Luma drop switches to night; gain ratio against the sampled night
    baseline brings it back to day. The full happy path."""
    stub.set_scene(luma=120, gain=500, ev=4000)
    ric = Ric("duskdawn", LUMA_CONF)
    if not ric.wait_running():
        result(False, "dusk/dawn: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)

    gm, mm = watch.mark(), stub.mark()
    stub.set_scene(luma=5, gain=20000, ev=100000)  # night falls, gain climbs
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
    result(ok, "dusk: luma below threshold switches to night", ric.read_log()[-300:])
    w = wait_pulse(watch, gm, IRCUT2, IRCUT1)
    result(
        w is not None and 0.05 < w < 0.3,
        "dusk: night pulse (ircut2 high, ircut1 quiet) with ~100ms width",
        "width=%s" % w,
    )
    result(wait_last(watch, gm, IRLED, "1"), "dusk: IR LED on", str(watch.since(gm)))

    # cooldown (3 polls) must elapse so the night gain baseline exists
    time.sleep((3 + 1) * POLL_MS / 1000.0)
    gm, mm = watch.mark(), stub.mark()
    stub.set_scene(luma=5, gain=4000, ev=100000)  # dawn: gain under 25% of 20000
    ok = wait_for(lambda: "day" in stub.modes_since(mm), 4)
    result(ok, "dawn: gain ratio switches back to day", ric.read_log()[-300:])
    w = wait_pulse(watch, gm, IRCUT1, IRCUT2)
    result(
        w is not None and 0.05 < w < 0.3,
        "dawn: day pulse reversed polarity (ircut1 high, ircut2 quiet)",
        "width=%s" % w,
    )
    result(wait_last(watch, gm, IRLED, "0"), "dawn: IR LED off", str(watch.since(gm)))
    ok = wait_last(watch, gm, IRCUT1, "0") and wait_last(watch, gm, IRCUT2, "0")
    result(ok, "dawn: coil de-energized after pulse", str(watch.since(gm)))
    ric.stop()


def scenario_hysteresis(stub, watch):
    """A dip shorter than hysteresis must not switch; a sustained one must."""
    conf = LUMA_CONF.replace("hysteresis_sec = 2", "hysteresis_sec = 5")
    stub.set_scene(luma=120, gain=500, ev=4000)
    ric = Ric("hyst", conf)
    if not ric.wait_running():
        result(False, "hysteresis: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)

    mm = stub.mark()
    stub.set_scene(luma=5, gain=500, ev=4000)  # dark...
    time.sleep(2 * POLL_MS / 1000.0)  # ...for ~2 of 5 required polls
    stub.set_scene(luma=120, gain=500, ev=4000)  # headlights pass, scene back
    time.sleep(1.5)
    result(
        stub.modes_since(mm) == [],
        "short dip does not switch (hysteresis)",
        str(stub.modes_since(mm)),
    )
    stub.set_scene(luma=5, gain=500, ev=4000)
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
    result(ok, "sustained dip switches after hysteresis", str(stub.modes_since(mm)))
    ric.stop()


def scenario_cooldown(stub, watch):
    """Day-worthy data during the post-switch cooldown must not flap the
    mode back; the baseline is sampled from the scene at cooldown end."""
    stub.set_scene(luma=120, gain=500, ev=4000)
    ric = Ric("cooldown", LUMA_CONF)
    if not ric.wait_running():
        result(False, "cooldown: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)

    mm = stub.mark()
    stub.set_scene(luma=5, gain=20000, ev=100000)
    if not wait_for(lambda: "night" in stub.modes_since(mm), 4):
        result(False, "cooldown: night entry", str(stub.modes_since(mm)))
        ric.stop()
        return
    # Immediately look day-worthy. Within the cooldown window nothing may
    # move; afterwards the baseline samples this low gain (clamped to 10%
    # of the detection gain), and the scene still sits above the derived
    # day trigger: still nothing may move.
    mm2 = stub.mark()
    stub.set_scene(luma=5, gain=1000, ev=100000)
    time.sleep((3 + 2 + 2) * POLL_MS / 1000.0)
    result(
        "day" not in stub.modes_since(mm2),
        "no flap during cooldown; baseline from settled scene",
        str(stub.modes_since(mm2)),
    )
    ric.stop()


def scenario_covered_lens_baseline(stub, watch):
    """The covered-lens test every user runs, modeled with IR-dependent
    optics: covering goes dark -> night; the IR LED bounces off the
    cover so the scene reads bright (gain 459 vs 45025 at detection).
    The settled bounce IS the baseline now (no 10%-of-trigger clamp:
    a starlight sensor legitimately settles below 1% of its trigger),
    so a covered lens holds night with no day attempt, no probe and
    no IR blinking. Uncovering into a bright room reads the same
    brightness (377 vs 459 -- level cannot separate them), lands as a
    sustained dip below the settled baseline, and the probe resolves
    it with the emitter off: real ambient light, verified day."""
    stub.set_scene(luma=120, gain=400, ev=500)
    ric = Ric("covered", LUMA_CONF)
    if not ric.wait_running():
        result(False, "covered-lens: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)

    mm = stub.mark()
    stub.set_scene(luma=17, gain=45025, ev=60000)  # covered, dark
    if not wait_for(lambda: "night" in stub.modes_since(mm), 4):
        result(False, "covered-lens: night entry", str(stub.modes_since(mm)))
        ric.stop()
        return
    # IR on, reflecting off the cover: bright to every metric, and the
    # settled reading becomes the honest night baseline.
    stub.set_scene(luma=50, gain=459, ev=2600)
    time.sleep(10 * POLL_MS / 1000.0)

    gm, mm2 = watch.mark(), stub.mark()
    time.sleep(25 * POLL_MS / 1000.0)
    led_vals = [c for _, p, _, c in watch.since(gm) if p == IRLED]
    result("day" not in stub.modes_since(mm2) and "0" not in led_vals,
           "covered-lens: covered scene holds night, no day attempt, no blink",
           "modes=%s leds=%s" % (stub.modes_since(mm2), led_vals))
    result("day verification failed" not in ric.read_log(),
           "covered-lens: no verify churn while covered", ric.read_log()[-200:])

    # Uncover into a bright room: same brightness level as the bounce,
    # but it dips below the settled baseline -> probe -> honest luma.
    gm, mm3 = watch.mark(), stub.mark()
    stub.set_scene(luma=50, gain=377, ev=1900)
    ok = wait_for(lambda: last_value(watch.since(gm), IRLED) == "0", 4)
    result(ok, "covered-lens: uncover dips the baseline and probes",
           str(watch.since(gm)))
    stub.set_scene(luma=90, gain=300, ev=2000)  # true ambient, no IR
    ok = wait_for(lambda: "day" in stub.modes_since(mm3), 5)
    result(ok, "covered-lens: probe verifies real day and sticks",
           ric.read_log()[-300:])
    result(last_value(watch.since(gm), IRLED) == "0",
           "covered-lens: IR stays off in verified day", str(watch.since(gm)))

    # Re-cover: dark again, normal dusk returns night.
    mm4 = stub.mark()
    stub.set_scene(luma=8, gain=44002, ev=242354)
    ok = wait_for(lambda: "night" in stub.modes_since(mm4), 5)
    result(ok, "covered-lens: re-cover returns to night", str(stub.modes_since(mm4)))
    ric.stop()


def scenario_baseline_refresh(stub, watch):
    """A poisoned (or stale) baseline recovers once the scene shows its
    true dark reading: the running max raises the baseline, so a later
    moderate brightening crosses the honest trigger. Without the
    refresh, gain 3000 sits above any clamped-from-poison trigger and
    day never comes."""
    stub.set_scene(luma=120, gain=400, ev=500)
    ric = Ric("baserefresh", LUMA_CONF)
    if not ric.wait_running():
        result(False, "baseline-refresh: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)

    mm = stub.mark()
    stub.set_scene(luma=17, gain=45025, ev=60000)
    if not wait_for(lambda: "night" in stub.modes_since(mm), 4):
        result(False, "baseline-refresh: night entry", str(stub.modes_since(mm)))
        ric.stop()
        return
    stub.set_scene(luma=50, gain=459, ev=2600)  # reflection poisons the sample
    time.sleep((3 + 1) * POLL_MS / 1000.0)

    # Cover removed in a genuinely dark room: the real IR-lit night
    # reading appears and the running max adopts it.
    stub.set_scene(luma=30, gain=20000, ev=80000)
    time.sleep(3 * POLL_MS / 1000.0)

    mm2 = stub.mark()
    stub.set_scene(luma=45, gain=3000, ev=3000)  # dawn, moderate light
    ok = wait_for(lambda: "day" in stub.modes_since(mm2), 6)
    result(ok, "baseline-refresh: running max restores an honest day trigger",
           str(stub.modes_since(mm2)) + " " + ric.read_log()[-200:])
    ric.stop()


def scenario_probe_dawn(stub, watch):
    """T20-class compressed gain: a lit scene under IR floors total_gain
    far above the day ratio (Wyze V2 bench: night baseline 1300 with IR,
    lights-on 1024 = 79%, ratio floor 325 unreachable). The dip below
    probe_gain_pct of the baseline lifts the IR LEDs; true ambient luma
    then decides day, and the LEDs stay off through the switch."""
    stub.set_scene(luma=70, gain=4500, ev=1200000)
    ric = Ric("probedawn", LUMA_CONF)
    if not ric.wait_running():
        result(False, "probe dawn: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)

    mm = stub.mark()
    stub.set_scene(luma=6, gain=8192, ev=50000000)  # dark, gain at ceiling
    if not wait_for(lambda: "night" in stub.modes_since(mm), 4):
        result(False, "probe dawn: night entry", str(stub.modes_since(mm)))
        ric.stop()
        return
    # IR bounce lights the scene: AE re-locks luma, gain settles low
    stub.set_scene(luma=72, gain=1300, ev=370000)
    time.sleep((3 + 2) * POLL_MS / 1000.0)  # cooldown -> baseline 1300

    gm, mm = watch.mark(), stub.mark()
    # lab lights on: gain dips to 79% of baseline, luma still AE-locked
    stub.set_scene(luma=73, gain=1024, ev=290000)
    ok = wait_for(lambda: last_value(watch.since(gm), IRLED) == "0", 3)
    result(ok, "probe dawn: gain dip lifts IR for an ambient sample",
           str(watch.since(gm)))
    # without IR the scene reads bright
    stub.set_scene(luma=95, gain=2500, ev=800000)
    ok = wait_for(lambda: "day" in stub.modes_since(mm), 4)
    result(ok, "probe dawn: ambient luma switches to day",
           ric.read_log()[-300:])
    result(last_value(watch.since(gm), IRLED) == "0",
           "probe dawn: IR stays off through the day switch",
           str(watch.since(gm)))
    ric.stop()


def scenario_probe_dark_restore(stub, watch):
    """A probe that finds real darkness restores the IR LEDs, stays in
    night mode, and holds off instead of blinking every poll."""
    stub.set_scene(luma=70, gain=4500, ev=1200000)
    ric = Ric("probedark", LUMA_CONF)
    if not ric.wait_running():
        result(False, "probe dark: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)

    mm = stub.mark()
    stub.set_scene(luma=6, gain=8192, ev=50000000)
    if not wait_for(lambda: "night" in stub.modes_since(mm), 4):
        result(False, "probe dark: night entry", str(stub.modes_since(mm)))
        ric.stop()
        return
    stub.set_scene(luma=72, gain=1300, ev=370000)
    time.sleep((3 + 2) * POLL_MS / 1000.0)  # cooldown -> baseline 1300

    gm = watch.mark()
    # IR bounce shifted (camera re-aimed): gain dips, room still dark
    stub.set_scene(luma=72, gain=1024, ev=300000)
    ok = wait_for(lambda: last_value(watch.since(gm), IRLED) == "0", 3)
    result(ok, "probe dark: dip lifts IR", str(watch.since(gm)))

    rm = watch.mark()
    stub.set_scene(luma=4, gain=8192, ev=50000000)  # truly dark without IR
    ok = wait_for(lambda: last_value(watch.since(rm), IRLED) == "1", 3)
    result(ok, "probe dark: darkness restores IR", str(watch.since(rm)))

    # IR-lit scene returns brighter than before; the resampled baseline
    # (cooldown after restore) makes 1024 a fresh dip, but the holdoff
    # must keep the LEDs from blinking again.
    stub.set_scene(luma=72, gain=1300, ev=370000)
    time.sleep((3 + 1) * POLL_MS / 1000.0)
    rm2 = watch.mark()
    stub.set_scene(luma=72, gain=1024, ev=300000)
    time.sleep(8 * POLL_MS / 1000.0)
    result(last_value(watch.since(rm2), IRLED) != "0",
           "probe dark: holdoff suppresses an immediate re-probe",
           str(watch.since(rm2)))
    st = ric_status()
    result(st.get("state") == "night", "probe dark: still night", str(st))
    ric.stop()


def scenario_probe_slow_ae(stub, watch):
    """gc2053-class AE settles over many seconds after the IR lights the
    scene: the night baseline must wait for the walk to stand still
    instead of adopting a mid-swing value 15x above the settled gain,
    and the walk itself must not read as a probe-worthy dip (Wyze V3:
    baseline 4502 sampled mid-walk, settled 306, IR blinking at every
    probe holdoff all night). A real dip below the settled baseline
    must still probe."""
    stub.set_scene(luma=70, gain=4500, ev=1200000)
    ric = Ric("probeslowae", LUMA_CONF)
    if not ric.wait_running():
        result(False, "slow-AE: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)

    gm, mm = watch.mark(), stub.mark()
    stub.set_scene(luma=6, gain=45000, ev=50000000)  # dark, gain at ceiling
    if not wait_for(lambda: "night" in stub.modes_since(mm), 4):
        result(False, "slow-AE: night entry", str(stub.modes_since(mm)))
        ric.stop()
        return
    # AE walks down over many polls once IR lights the scene, then holds
    for g in (20000, 9000, 4500, 1500, 700, 306):
        stub.set_scene(luma=72, gain=g, ev=300000)
        time.sleep(2 * POLL_MS / 1000.0)
    time.sleep(26 * POLL_MS / 1000.0)
    led_vals = [c for _, p, _, c in watch.since(gm) if p == IRLED and c in ("0", "1")]
    result("0" not in led_vals,
           "slow-AE: neither the walk nor the settled gain false-probes",
           str(watch.since(gm)))
    st = ric_status()
    if not st or st.get("state") != "night":
        result(False, "slow-AE: still night after the walk", str(st))
        ric.stop()
        return
    result(True, "slow-AE: still night after the walk")

    # lights on: a real dip below the settled baseline still probes;
    # react to the lifted LEDs with the true bright ambient
    gm = watch.mark()
    stub.set_scene(luma=72, gain=240, ev=250000)
    ok = wait_for(lambda: last_value(watch.since(gm), IRLED) == "0", 5)
    result(ok, "slow-AE: a real dip below the settled baseline probes",
           str(watch.since(gm)))
    if ok:
        stub.set_scene(luma=95, gain=200, ev=200000)
        ok = wait_for(lambda: "day" in stub.modes_since(mm), 5)
        result(ok, "slow-AE: the probe completes into day",
               ric.read_log()[-200:])
    ric.stop()


def scenario_probe_recheck(stub, watch):
    """IR wash can hide ambient light entirely: on a Wyze V3 the IR-lit
    scene reads EV 637 lit vs ~700 dark -- a 9% perturbation no dip
    threshold can catch. The periodic recheck probes anyway after
    probe_recheck_sec without a dip, bounding dawn latency on
    wash-dominated scenes; a dark recheck restores, re-arms, and fires
    again an interval later."""
    conf = LUMA_CONF + "probe_recheck_sec = 2\n"
    stub.set_scene(luma=70, gain=4500, ev=1200000)
    ric = Ric("procheck", conf)
    if not ric.wait_running():
        result(False, "recheck: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)

    mm = stub.mark()
    stub.set_scene(luma=6, gain=8192, ev=50000000)
    if not wait_for(lambda: "night" in stub.modes_since(mm), 4):
        result(False, "recheck: night entry", str(stub.modes_since(mm)))
        ric.stop()
        return
    # IR-lit scene, rock stable: no dip ever
    stub.set_scene(luma=72, gain=1300, ev=370000)
    time.sleep((3 + 3) * POLL_MS / 1000.0)

    # a recheck probe must fire within ~2s even with nothing dipping
    gm = watch.mark()
    ok = wait_for(lambda: last_value(watch.since(gm), IRLED) == "0", 4)
    result(ok, "recheck: stable night still probes on the interval",
           str(watch.since(gm)))
    # the room is genuinely dark without IR: restore and hold night
    stub.set_scene(luma=4, gain=8192, ev=50000000)
    ok = wait_for(lambda: last_value(watch.since(gm), IRLED) == "1", 4)
    result(ok, "recheck: dark recheck restores IR", str(watch.since(gm)))
    st = ric_status()
    result(st is not None and st.get("state") == "night",
           "recheck: still night", str(st))

    # back to the stable IR-lit scene; the interval must re-arm and a
    # second recheck fire (holdoff applies to failed DIP probes, the
    # recheck cadence is its own clock)
    stub.set_scene(luma=72, gain=1300, ev=370000)
    time.sleep(3 * POLL_MS / 1000.0)
    gm2 = watch.mark()
    ok = wait_for(lambda: last_value(watch.since(gm2), IRLED) == "0", 6)
    result(ok, "recheck: the interval re-arms after a dark recheck",
           str(watch.since(gm2)))
    stub.set_scene(luma=95, gain=2500, ev=800000)  # honest bright ambient
    ok = wait_for(lambda: "day" in stub.modes_since(mm), 5)
    result(ok, "recheck: a lit recheck completes into day",
           ric.read_log()[-200:])
    ric.stop()


def scenario_recheck_rearm(stub, watch):
    """Shortening probe_recheck_sec at runtime must re-arm the running
    countdown: the bench sets a short interval mid-night to bound its
    dawn legs, and a countdown still holding the old ten-minute value
    would ignore it until the next night entry."""
    conf = LUMA_CONF + "probe_recheck_sec = 3600\n"
    stub.set_scene(luma=70, gain=4500, ev=1200000)
    ric = Ric("rearm", conf)
    if not ric.wait_running():
        result(False, "recheck-rearm: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)
    mm = stub.mark()
    stub.set_scene(luma=6, gain=8192, ev=50000000)
    if not wait_for(lambda: "night" in stub.modes_since(mm), 4):
        result(False, "recheck-rearm: night entry", str(stub.modes_since(mm)))
        ric.stop()
        return
    stub.set_scene(luma=72, gain=1300, ev=370000)
    time.sleep((3 + 3) * POLL_MS / 1000.0)

    gm = watch.mark()
    r = ctrl_cmd(RUN_DIR + "/ric.sock",
                 {"cmd": "set-threshold", "key": "probe_recheck_sec", "value": 1})
    ok = r is not None and r.get("status") == "ok"
    probed = wait_for(lambda: last_value(watch.since(gm), IRLED) == "0", 4)
    result(ok and probed,
           "recheck-rearm: shortening the interval re-arms the countdown",
           "resp=%s events=%s" % (r, watch.since(gm)))
    ric.stop()


def scenario_noir_luma_dawn(stub, watch):
    """With no IR bank enabled nothing pollutes luma at night, so dawn
    may ride luma directly even when the gain ratio cannot fire: a
    compressed-gain sensor sits at 56% of its pinned night baseline in
    a lit room, far above the 25% ratio floor."""
    conf = LUMA_CONF + "ir850 = false\nir940 = false\n"
    stub.set_scene(luma=70, gain=4500, ev=1200000)
    ric = Ric("noirdawn", conf)
    if not ric.wait_running():
        result(False, "no-IR dawn: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)

    gm, mm = watch.mark(), stub.mark()
    stub.set_scene(luma=6, gain=8192, ev=50000000)
    if not wait_for(lambda: "night" in stub.modes_since(mm), 4):
        result(False, "no-IR dawn: night entry", str(stub.modes_since(mm)))
        ric.stop()
        return
    result(last_value(watch.since(gm), IRLED) != "1",
           "no-IR dawn: disabled banks stay dark at night",
           str(watch.since(gm)))
    time.sleep((3 + 2) * POLL_MS / 1000.0)  # cooldown -> baseline 8192

    mm2 = stub.mark()
    stub.set_scene(luma=70, gain=4564, ev=1280000)  # lit: 56% of baseline
    ok = wait_for(lambda: "day" in stub.modes_since(mm2), 4)
    result(ok, "no-IR dawn: luma alone returns to day",
           ric.read_log()[-300:])
    ric.stop()


def scenario_ctrl(stub, watch):
    """Forced modes actuate regardless of the scene; auto resumes."""
    stub.set_scene(luma=120, gain=500, ev=4000)
    ric = Ric("ctrl", LUMA_CONF)
    if not ric.wait_running():
        result(False, "ctrl: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)

    gm, mm = watch.mark(), stub.mark()
    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "mode", "value": "night"})
    ok = r is not None and r.get("state") == "night"
    result(ok, "ctrl: forced night despite bright scene", str(r))
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 3)
    result(ok, "ctrl: forced night reaches rvd", str(stub.modes_since(mm)))
    result(wait_last(watch, gm, IRLED, "1"), "ctrl: forced night lights IR",
           str(watch.since(gm)))

    # Back to auto: bright scene should recover to day via the poll loop
    # only after a night baseline exists; force day instead, then auto.
    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "mode", "value": "day"})
    result(r is not None and r.get("state") == "day", "ctrl: forced day", str(r))
    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "mode", "value": "auto"})
    result(r is not None and r.get("mode") == "auto", "ctrl: back to auto", str(r))
    s = ric_status()
    result(
        s is not None and s.get("status") == "ok" and "exposure" in s,
        "ctrl: status carries exposure block",
        str(s),
    )
    ric.stop()


def scenario_single_gpio(stub, watch):
    """gpio_ircut2 = -1 selects level semantics: no pulse, plain levels."""
    conf = LUMA_CONF.replace(
        "gpio_ircut2 = %d" % IRCUT2, "gpio_ircut2 = -1"
    )
    stub.set_scene(luma=120, gain=500, ev=4000)
    ric = Ric("single", conf)
    if not ric.wait_running():
        result(False, "single-gpio: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)

    gm, mm = watch.mark(), stub.mark()
    stub.set_scene(luma=5, gain=20000, ev=100000)
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
    result(ok, "single-gpio: night switch", str(stub.modes_since(mm)))
    ok = wait_last(watch, gm, IRCUT1, "0")
    result(
        ok and last_value(watch.since(gm), IRCUT2) is None,
        "single-gpio: level drive, second pin untouched",
        str(watch.since(gm)),
    )
    ric.stop()


def scenario_zero_exposure(stub, watch):
    """All-zero exposure (HAL without readback) must hold the mode and warn
    once -- not read silence as darkness. [expected behavior per PR #14]"""
    stub.set_scene(luma=0, gain=0, ev=0)
    ric = Ric("zero", LUMA_CONF)
    if not ric.wait_running():
        result(False, "zero-exposure: ric start", "no 'ric running'")
        ric.stop()
        return
    mm = stub.mark()
    time.sleep(2.5)
    modes = stub.modes_since(mm)
    result(
        "night" not in modes,
        "zero exposure holds mode (no phantom night)",
        str(modes),
    )
    warns = len(re.findall(r"WARN.*no exposure data", ric.read_log()))
    result(warns == 1, "zero exposure warns exactly once", "warns=%d" % warns)
    # Manual override must still work on such platforms.
    gm = watch.mark()
    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "mode", "value": "night"})
    ok = r is not None and r.get("state") == "night"
    ev_ok = wait_for(lambda: last_value(watch.since(gm), IRLED) == "1", 3)
    result(ok and ev_ok, "zero exposure: manual override still actuates", str(r))
    ric.stop()


def scenario_partial_fields(stub, watch):
    """Gain-only and luma-only platforms must run on what they have.
    [expected behavior per PR #14]"""
    # gain-only: enter night on gain, return to day on the baseline ratio
    stub.set_scene(luma=0, gain=100000, ev=0)
    ric = Ric("gainonly", LUMA_CONF)
    if not ric.wait_running():
        result(False, "gain-only: ric start", "no 'ric running'")
        ric.stop()
        return
    mm = stub.mark()
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
    result(ok, "gain-only: night via gain threshold", str(stub.modes_since(mm)))
    time.sleep((3 + 1) * POLL_MS / 1000.0)  # cooldown: baseline = 100000
    stub.set_scene(luma=0, gain=10000, ev=0)
    ok = wait_for(lambda: "day" in stub.modes_since(mm), 4)
    result(ok, "gain-only: day via baseline ratio", str(stub.modes_since(mm)))
    ric.stop()

    # luma-only, active IR bank: the inverse-luma day test would oscillate
    # (IR lifts luma, day cuts the LEDs, dark again, night, ...), so the
    # mode must hold night even with luma back over the threshold.
    stub.set_scene(luma=5, gain=0, ev=0)
    ric = Ric("lumaonly", LUMA_CONF)  # gpio_irled set, ir850 defaults true
    if not ric.wait_running():
        result(False, "luma-only: ric start", "no 'ric running'")
        ric.stop()
        return
    mm = stub.mark()
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
    result(ok, "luma-only: night via luma", str(stub.modes_since(mm)))
    time.sleep((3 + 1) * POLL_MS / 1000.0)
    stub.set_scene(luma=100, gain=0, ev=0)  # what the lit IR does to luma
    time.sleep(2.0)
    result(
        "day" not in stub.modes_since(mm),
        "luma-only: active IR bank holds night (no oscillation)",
        str(stub.modes_since(mm)),
    )
    ric.stop()

    # luma-only, no active IR bank: luma is trustworthy at night, so the
    # inverse test must bring the camera back out.
    stub.set_scene(luma=5, gain=0, ev=0)
    ric = Ric("lumanoir", LUMA_CONF + "ir850 = false\n")
    if not ric.wait_running():
        result(False, "luma-only-noir: ric start", "no 'ric running'")
        ric.stop()
        return
    mm = stub.mark()
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
    result(ok, "luma-only: night via luma (no IR bank)", str(stub.modes_since(mm)))
    time.sleep((3 + 1) * POLL_MS / 1000.0)
    stub.set_scene(luma=100, gain=0, ev=0)
    ok = wait_for(lambda: "day" in stub.modes_since(mm), 4)
    result(ok, "luma-only: day via inverse luma (no active IR bank)",
           str(stub.modes_since(mm)))
    ric.stop()


def scenario_adc(stub, watch):
    """The ADC trigger must work even when the exposure readback reports
    nothing at all -- that combination is exactly the photoresistor board
    on a HAL without isp_get_exposure."""
    if not ADC_PRELOAD:
        print("  SKIP  adc: shim not built", flush=True)
        return
    backing = WORK + "/adc-value"
    hits = backing + ".hits"
    try:
        os.unlink(hits)
    except OSError:
        pass
    with open(backing, "wb") as f:
        f.write(struct.pack("<i", 700))  # bright side of adc_day=600
    conf = GPIO_CONF + (
        "trigger = adc\nadc_channel = 0\nadc_night = 200\n"
        "adc_day = 600\nhysteresis_sec = 2\n"
    )
    stub.set_scene(luma=0, gain=0, ev=0)  # nothing from the ISP, ever
    ric = Ric(
        "adc",
        conf,
        env_extra={"LD_PRELOAD": ADC_PRELOAD, "RIC_ADC_BACKING": backing},
    )
    if not ric.wait_running():
        result(False, "adc: ric start", ric.read_log()[-300:])
        ric.stop()
        return
    if "falling back to luma" in ric.read_log():
        # The shim never engaged at open time, so ric could not start
        # its ADC and demoted itself. (A read-level bypass looks
        # different -- start succeeds and polls return nothing -- and
        # the known case of that, _FORTIFY_SOURCE emitting __read_chk,
        # is intercepted by the shim since issue #18.) Coverage is lost
        # either way; strict mode refuses to lose it silently.
        ric.stop()
        if os.environ.get("RIC_SUITE_STRICT", "") == "1":
            result(False, "adc: shim provides the device", ric.read_log()[-300:])
        else:
            print("  SKIP  adc: preload shim inert on this host", flush=True)
        return

    # The shim appends to <backing>.hits on every read it serves. If
    # polls are running but no hits appear, reads are bypassing the
    # shim -- the exact shape of issue #18 -- and this leg names it
    # instead of letting the transition legs fail three layers up.
    ok = wait_for(
        lambda: os.path.exists(hits) and os.path.getsize(hits) > 0, 4
    )
    result(ok, "adc: shim read interception engaged (hit counter)",
           "polls running but no reads reached the shim (issue #18 shape)")
    if not ok:
        ric.stop()
        return
    mm = stub.mark()
    with open(backing, "wb") as f:
        f.write(struct.pack("<i", 100))  # dark
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
    result(
        ok,
        "adc: photoresistor drives night with zero exposure data",
        str(stub.modes_since(mm)),
    )
    with open(backing, "wb") as f:
        f.write(struct.pack("<i", 700))  # bright again
    ok = wait_for(lambda: "day" in stub.modes_since(mm), 4)
    result(
        ok,
        "adc: photoresistor drives day with zero exposure data",
        str(stub.modes_since(mm)),
    )
    ric.stop()


def scenario_adc_dead(stub, watch):
    """A photoresistor that opens and then stops answering must say so.
    A device that will not open at all is already reported, and ric
    demotes itself to the luma trigger; what was uncovered is the one
    that reads fine and then goes quiet, freezing day/night with a
    clean log. Reads are made to come up short by truncating the
    backing file, which is what a dead channel looks like from ric."""
    if not ADC_PRELOAD:
        print("  SKIP  adc-dead: shim not built", flush=True)
        return
    backing = WORK + "/adc-dead-value"
    hits = backing + ".hits"
    try:
        os.unlink(hits)
    except OSError:
        pass
    with open(backing, "wb") as f:
        f.write(struct.pack("<i", 700))  # bright side of adc_day=600
    conf = GPIO_CONF + (
        "trigger = adc\nadc_channel = 0\nadc_night = 200\n"
        "adc_day = 600\nhysteresis_sec = 2\n"
    )
    stub.set_scene(luma=0, gain=0, ev=0)
    ric = Ric(
        "adc-dead",
        conf,
        env_extra={"LD_PRELOAD": ADC_PRELOAD, "RIC_ADC_BACKING": backing},
    )
    if not ric.wait_running():
        result(False, "adc-dead: ric start", ric.read_log()[-300:])
        ric.stop()
        return
    if "falling back to luma" in ric.read_log():
        # Same inert-shim case scenario_adc documents; skip, or fail
        # under strict mode, rather than lose the coverage quietly.
        ric.stop()
        if os.environ.get("RIC_SUITE_STRICT", "") == "1":
            result(False, "adc-dead: shim provides the device", ric.read_log()[-300:])
        else:
            print("  SKIP  adc-dead: preload shim inert on this host", flush=True)
        return
    if not wait_for(lambda: os.path.exists(hits) and os.path.getsize(hits) > 0, 4):
        result(False, "adc-dead: shim read interception engaged",
               "polls running but no reads reached the shim (issue #18 shape)")
        ric.stop()
        return

    # A brief gap is not a fault. Truncating the backing file fails every
    # read, but for well under the sustained threshold -- the hit counter
    # proves ric really polled across it, so this is not vacuous.
    before = os.path.getsize(hits)
    with open(backing, "wb"):
        pass
    time.sleep(0.6)
    polled = os.path.getsize(hits) - before
    with open(backing, "wb") as f:
        f.write(struct.pack("<i", 700))
    log = ric.read_log()
    result(
        polled >= 2 and "read nothing" not in log and "reading again" not in log,
        "adc-dead: a brief gap in readings stays quiet",
        "polls across the gap=%d, log=%s" % (polled, log[-200:]),
    )

    # A sustained one is: day/night is stuck and nothing else will say so.
    mm = stub.mark()
    with open(backing, "wb"):
        pass
    ok = wait_for(lambda: "read nothing" in ric.read_log(), 8)
    result(ok, "adc-dead: a sustained outage is reported", ric.read_log()[-300:])
    if ok:
        # Rate-limited: the repeat is a minute out, so several more
        # seconds of failing polls must not add a second line.
        time.sleep(3)
        n = len(re.findall(r"read nothing", ric.read_log()))
        result(n == 1, "adc-dead: the outage warning is rate-limited",
               "%d warnings across ~5s of failed polls" % n)

    # Recovery is reported, and the poll path drives modes again rather
    # than merely logging.
    with open(backing, "wb") as f:
        f.write(struct.pack("<i", 100))  # dark
    ok = wait_for(lambda: "reading again" in ric.read_log(), 5)
    result(ok, "adc-dead: recovery is reported", ric.read_log()[-300:])
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 6)
    result(ok, "adc-dead: readings resume driving day/night",
           str(stub.modes_since(mm)))
    ric.stop()


def scenario_pulse_width(stub, watch):
    """pulse_ms drives the dual-GPIO coil pulse (default 10, the value
    the fleet's ircut script has always used; thingino-firmware #1380).
    100ms must measure as roughly 100ms; 10ms must measure clearly
    shorter. Drain-time stamps cannot resolve 10ms exactly, so the
    short assertion is an upper bound."""
    for ms, lo, hi in ((100, 0.05, 0.3), (10, 0.0, 0.05)):
        conf = LUMA_CONF.replace("pulse_ms = 100", "pulse_ms = %d" % ms)
        stub.set_scene(luma=120, gain=500, ev=4000)
        gm = watch.mark()
        ric = Ric("pulse%d" % ms, conf)
        if not ric.wait_running():
            result(False, "pulse_ms=%d: ric start" % ms, "no 'ric running'")
            ric.stop()
            continue
        w = wait_pulse(watch, gm, IRCUT1, IRCUT2)  # startup day park
        result(w is not None and lo <= w < hi,
               "pulse_ms=%d: coil pulse within expected bounds" % ms,
               "width=%s" % w)
        ric.stop()


def scenario_gain_trigger(stub, watch):
    """Legacy gain trigger: fixed absolute thresholds, no baseline."""
    conf = GPIO_CONF + (
        "trigger = gain\nnight_threshold = 40000\nday_threshold = 25000\n"
        "hysteresis_sec = 2\n"
    )
    stub.set_scene(luma=0, gain=10000, ev=0)
    ric = Ric("gain", conf)
    if not ric.wait_running():
        result(False, "gain-trigger: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)
    mm = stub.mark()
    stub.set_scene(luma=0, gain=50000, ev=0)
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
    result(ok, "gain-trigger: night above night_threshold", str(stub.modes_since(mm)))
    time.sleep((3 + 1) * POLL_MS / 1000.0)  # cooldown before day is evaluated
    stub.set_scene(luma=0, gain=10000, ev=0)
    ok = wait_for(lambda: "day" in stub.modes_since(mm), 4)
    result(ok, "gain-trigger: day below day_threshold", str(stub.modes_since(mm)))

    # the legacy thresholds retune live as well
    mm = stub.mark()
    stub.set_scene(luma=0, gain=50000, ev=0)
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
    time.sleep((3 + 1) * POLL_MS / 1000.0)
    ctrl_cmd(RUN_DIR + "/ric.sock",
             {"cmd": "set-threshold", "key": "day_threshold", "value": 60000})
    ok = ok and wait_for(lambda: "day" in stub.modes_since(mm), 4)
    result(ok, "gain-trigger: day_threshold retunes live",
           str(stub.modes_since(mm)))
    ric.stop()


def scenario_photo_night_boot(stub, watch):
    """A camera that powers up in darkness must reach night mode at
    production speed: with no AWB baseline neither spectral path can
    confirm, so the EV-only boot path (stock behavior) must fire on
    the ev_night counter alone -- not minutes later on the drift
    fallback. Calibration itself must wait for day mode: on
    short-throw scenes the IR LEDs pull EV below ev_day, and a
    baseline learned under IR poisons every deviation check."""
    conf = GPIO_CONF + "trigger = photo\n"
    stub.set_scene(luma=5, gain=30000, ev=200000, rgain=170, bgain=150)
    ric = Ric("photoboot", conf, poll_ms=10)
    if not ric.wait_running():
        result(False, "photo boot: ric start", "no 'ric running'")
        ric.stop()
        return
    mm = stub.mark()
    # settle(7) + NIGHT_EV_TRIGGER(23) = 30 polls; at 10ms that is
    # 0.3s. 3s is generous headroom and far below the ~2000 polls the
    # drift fallback would need -- a pass proves the fast path fired.
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 3)
    result(ok, "photo boot: dark boot reaches night on EV alone, fast",
           ric.read_log()[-300:])
    if not ok:
        ric.stop()
        return
    log = ric.read_log()
    result("photo AWB calibrated" not in log,
           "photo boot: no AWB baseline learned in the dark", log[-200:])

    # Bright scene: day must come back uncalibrated via the ratio path,
    # and only then may calibration sample (day mode, IR off).
    mm2 = stub.mark()
    stub.set_scene(luma=100, gain=500, ev=3000, rgain=140, bgain=140)
    ok = wait_for(lambda: "day" in stub.modes_since(mm2), 10)
    result(ok, "photo boot: uncalibrated day return via ratio path",
           ric.read_log()[-300:])
    ok = wait_for(lambda: "photo AWB calibrated" in ric.read_log(), 6)
    log = ric.read_log()
    day_pos = log.rfind("switched to DAY")
    cal_pos = log.rfind("photo AWB calibrated")
    result(ok and day_pos >= 0 and cal_pos > day_pos,
           "photo boot: calibration waits for day mode",
           "day@%d cal@%d" % (day_pos, cal_pos))
    ric.stop()


def scenario_photo(stub, watch):
    """Photo trigger: AWB auto-calibration from bright samples, night via
    EV + R-gain spectral shift (path 1), day via the fixed-EV drift
    detector. The day-approach ratio path is the other way into day and
    has its own scenario below; here ev stays above photo_ev_day, so the
    ratio path's ring never accumulates and drift is the only route."""
    conf = GPIO_CONF + "trigger = photo\n"
    stub.set_scene(luma=100, gain=500, ev=3000, rgain=140, bgain=140)
    ric = Ric("photo", conf, poll_ms=100)
    if not ric.wait_running():
        result(False, "photo: ric start", "no 'ric running'")
        ric.stop()
        return
    ok = wait_for(lambda: "photo AWB calibrated" in ric.read_log(), 6)
    result(ok, "photo: AWB baseline auto-calibrates from bright scene",
           ric.read_log()[-300:])
    if not ok:
        ric.stop()
        return

    mm = stub.mark()
    # dark (ev between ev_night and ev_deep) with an IR-shifted R gain
    stub.set_scene(luma=10, gain=20000, ev=80000, rgain=170, bgain=140)
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 10)
    result(ok, "photo: night via EV level + R-gain deviation",
           str(stub.modes_since(mm)))
    result("photo night trigger" in ric.read_log(),
           "photo: night came from the spectral path", ric.read_log()[-300:])

    # hold stable so the fixed-EV ring locks a reference, then brighten
    time.sleep(1.2)
    stub.set_scene(luma=60, gain=2000, ev=30000, rgain=150, bgain=140)
    ok = wait_for(lambda: "day" in stub.modes_since(mm), 8)
    result(ok, "photo: day via fixed-EV drift", str(stub.modes_since(mm)))
    ric.stop()


def scenario_photo_day_ratio(stub, watch):
    """The day-approach ratio path must be able to fire at all.

    day_ref_ev was declared and read but never assigned, so ref was
    always 0, the `ref <= 0` early return ran on every poll, and
    everything below it -- including its ric_set_mode(DAY) -- was dead.
    Day recovery came only from the fixed-EV drift detector, which is a
    separate path running in this same phase and would mask the defect
    in a mode-only assertion. So assert on the ratio path's own log
    line rather than on the mode.

    photo_ev_day is raised so the ring can accumulate on a step down
    small enough not to hand the win to the drift detector."""
    conf = (GPIO_CONF + "trigger = photo\nphoto_ev_day = 90000\n"
            + "hysteresis_sec = 0\n")
    stub.set_scene(luma=100, gain=500, ev=3000, rgain=140, bgain=140)
    ric = Ric("photodayratio", conf, poll_ms=100)
    if not ric.wait_running():
        result(False, "photo-day-ratio: ric start", "no 'ric running'")
        ric.stop()
        return
    if not wait_for(lambda: "photo AWB calibrated" in ric.read_log(), 6):
        result(False, "photo-day-ratio: AWB calibration", ric.read_log()[-300:])
        ric.stop()
        return

    mm = stub.mark()
    stub.set_scene(luma=10, gain=20000, ev=100000, rgain=170, bgain=140)
    if not wait_for(lambda: "night" in stub.modes_since(mm), 10):
        result(False, "photo-day-ratio: reaches night", str(stub.modes_since(mm)))
        ric.stop()
        return

    # Brighten to just under photo_ev_day and hold it there.
    stub.set_scene(luma=60, gain=2000, ev=85000, rgain=150, bgain=140)
    ok = wait_for(lambda: "photo day approach" in ric.read_log()
                  or "photo day trigger" in ric.read_log(), 15)
    result(ok, "photo: the day-approach ratio path can fire",
           ric.read_log()[-400:])
    ric.stop()


def scenario_photo_no_ev(stub, watch):
    """A platform that never reports ev cannot run the photo state machine;
    it must fall back to the luma trigger instead of idling silently.
    [expected behavior per PR #14]"""
    conf = GPIO_CONF + "trigger = photo\nnight_luma = 20\nhysteresis_sec = 2\n"
    stub.set_scene(luma=5, gain=0, ev=0)
    ric = Ric("photonoev", conf, poll_ms=POLL_MS)
    if not ric.wait_running():
        result(False, "photo-no-ev: ric start", "no 'ric running'")
        ric.stop()
        return
    mm = stub.mark()
    ok = wait_for(lambda: "falling back to the luma trigger" in ric.read_log(), 3)
    result(ok, "photo: no ev falls back to luma trigger", ric.read_log()[-200:])
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
    result(ok, "photo: fallback luma path reaches night", str(stub.modes_since(mm)))
    ric.stop()


def scenario_startup_forced(stub, watch):
    """mode=night|day in the config parks accordingly at startup and
    never polls transitions, whatever the scene says."""
    stub.set_scene(luma=120, gain=500, ev=4000)  # bright...
    gm, mm = watch.mark(), stub.mark()
    ric = Ric("forcedn", LUMA_CONF, mode="night")
    if not ric.wait_running():
        result(False, "forced-night: ric start", "no 'ric running'")
        ric.stop()
        return
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
    ev_ok = wait_for(lambda: last_value(watch.since(gm), IRLED) == "1", 3)
    result(ok and ev_ok, "config mode=night parks night in a bright scene",
           "%s irled=%s" % (stub.modes_since(mm), last_value(watch.since(gm), IRLED)))
    time.sleep(1.0)
    result("day" not in stub.modes_since(mm),
           "config mode=night never auto-switches", str(stub.modes_since(mm)))
    ric.stop()

    stub.set_scene(luma=2, gain=90000, ev=200000)  # ...and pitch dark
    mm = stub.mark()
    ric = Ric("forcedd", LUMA_CONF, mode="day")
    if not ric.wait_running():
        result(False, "forced-day: ric start", "no 'ric running'")
        ric.stop()
        return
    ok = wait_for(lambda: "day" in stub.modes_since(mm), 4)
    result(ok, "config mode=day parks day in a dark scene", str(stub.modes_since(mm)))
    time.sleep(1.0)
    result("night" not in stub.modes_since(mm),
           "config mode=day never auto-switches", str(stub.modes_since(mm)))
    ric.stop()


def scenario_ir_combos(stub, watch):
    """ir850 disabled + ir940 enabled: night must light only the 940nm
    pin and leave the 850nm pin untouched."""
    conf = LUMA_CONF + (
        "gpio_irled2 = %d\nir850 = false\nir940 = true\n" % IRLED2
    )
    stub.set_scene(luma=120, gain=500, ev=4000)
    ric = Ric("ircombo", conf)
    if not ric.wait_running():
        result(False, "ir-combos: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)
    gm, mm = watch.mark(), stub.mark()
    stub.set_scene(luma=5, gain=20000, ev=100000)
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
    result(ok, "ir-combos: night switch", str(stub.modes_since(mm)))
    result(wait_last(watch, gm, IRLED2, "1"), "ir-combos: ir940 pin lit",
           str(watch.since(gm)))
    result(last_value(watch.since(gm), IRLED) is None,
           "ir-combos: disabled ir850 pin untouched", str(watch.since(gm)))
    ric.stop()

    # both LED banks enabled: night lights the pair, day drops the pair
    conf = LUMA_CONF + ("gpio_irled2 = %d\nir850 = true\nir940 = true\n" % IRLED2)
    stub.set_scene(luma=120, gain=500, ev=4000)
    ric = Ric("irboth", conf)
    if not ric.wait_running():
        result(False, "ir-both: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)
    gm, mm = watch.mark(), stub.mark()
    stub.set_scene(luma=5, gain=20000, ev=100000)
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
    both = wait_last(watch, gm, IRLED, "1") and wait_last(watch, gm, IRLED2, "1")
    result(ok and both, "ir-both: night lights both LED pins", str(watch.since(gm)))
    time.sleep((3 + 1) * POLL_MS / 1000.0)
    gm = watch.mark()
    stub.set_scene(luma=5, gain=4000, ev=100000)
    ok = wait_for(lambda: "day" in stub.modes_since(mm), 4)
    both = wait_last(watch, gm, IRLED, "0") and wait_last(watch, gm, IRLED2, "0")
    result(ok and both, "ir-both: day drops both LED pins", str(watch.since(gm)))
    ric.stop()


def scenario_ir940_only_board(stub, watch):
    """A board whose ONLY IR bank is 940nm, running as shipped: pins from
    discovery, every [ircut] flag at its default. The only bank a board
    has must light at night without anyone editing a config -- found in
    the field on a wuuk y0510 that sat pitch black because ir940
    defaults to off. An explicit ir940 = false must still win."""
    orig = open("/etc/thingino.json").read()
    with open("/etc/thingino.json", "w") as f:
        json.dump({"gpio": {"ircut": "%d %d" % (IRCUT1, IRCUT2),
                            "ir940": IRLED2}}, f)
    try:
        conf = "trigger = luma\nnight_luma = 20\nhysteresis_sec = 2\n"
        stub.set_scene(luma=120, gain=500, ev=4000)
        ric = Ric("ir940only", conf)
        if not ric.wait_running():
            result(False, "ir940-only: ric start", "no 'ric running'")
            ric.stop()
            return
        time.sleep(0.5)
        gm, mm = watch.mark(), stub.mark()
        stub.set_scene(luma=5, gain=20000, ev=100000)
        ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
        result(ok, "ir940-only: night switch", str(stub.modes_since(mm)))
        result(wait_last(watch, gm, IRLED2, "1"),
               "ir940-only: the only IR bank lights by default",
               "gpio%d never went high -- 940-only board is blind at night"
               % IRLED2)
        ric.stop()

        # the user's explicit no must still be a no
        stub.set_scene(luma=120, gain=500, ev=4000)
        ric = Ric("ir940off", conf + "ir940 = false\n")
        if not ric.wait_running():
            result(False, "ir940-only: explicit-off ric start", "no 'ric running'")
            ric.stop()
            return
        time.sleep(0.5)
        gm, mm = watch.mark(), stub.mark()
        stub.set_scene(luma=5, gain=20000, ev=100000)
        ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
        time.sleep(0.5)
        result(ok and last_value(watch.since(gm), IRLED2) is None,
               "ir940-only: explicit ir940=false stays dark",
               str(watch.since(gm)))
        ric.stop()
    finally:
        with open("/etc/thingino.json", "w") as f:
            f.write(orig)


def scenario_mode_reassert(stub, watch):
    """Forcing a mode is an explicit hardware assertion, not a
    state-machine hint: after bench verbs perturb the rails (ircut
    night, ir850 on) while the daemon believes day, `mode day` must
    still cut the LEDs and re-pulse the filter -- found on a Wyze V3
    as day mode with lit IR LEDs, because a matching current_mode
    made the force a silent no-op."""
    stub.set_scene(luma=120, gain=500, ev=4000)
    ric = Ric("modereassert", LUMA_CONF)
    if not ric.wait_running():
        result(False, "mode-reassert: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)

    gm = watch.mark()
    ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "ircut", "value": "night"})
    ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "ir850", "value": "on"})
    if not wait_last(watch, gm, IRLED, "1"):
        result(False, "mode-reassert: bench perturbation", str(watch.since(gm)))
        ric.stop()
        return

    gm = watch.mark()
    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "mode", "value": "day"})
    ok = r is not None and r.get("state") == "day"
    led = wait_last(watch, gm, IRLED, "0")
    result(ok and led, "mode-reassert: forced day cuts perturbed IR",
           "resp=%s events=%s" % (r, watch.since(gm)))
    w = wait_pulse(watch, gm, IRCUT1, IRCUT2)
    result(w is not None,
           "mode-reassert: forced day re-pulses the filter day-ward",
           str(watch.since(gm)))

    # Symmetric: perturb day-ward while forced night, re-assert night
    ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "mode", "value": "night"})
    time.sleep(0.3)
    gm = watch.mark()
    ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "ir850", "value": "off"})
    ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "ircut", "value": "day"})
    time.sleep(0.3)
    gm = watch.mark()
    ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "mode", "value": "night"})
    ok = wait_last(watch, gm, IRLED, "1")
    result(ok, "mode-reassert: forced night relights perturbed IR",
           str(watch.since(gm)))
    ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "mode", "value": "auto"})
    ric.stop()


def scenario_manual_gpio(stub, watch):
    """raptorctl-level manual hardware control: ircut, ir850 and ir940
    each drive their piece alone -- no ISP call, no mode change. The
    LED commands ignore the enable flags (manual intent beats the
    automatic gating; lighting a disabled bank is the bench move), and
    the next automatic transition reasserts over manual state for the
    banks auto manages, while a manually-lit disabled bank stays lit."""
    conf = LUMA_CONF + ("gpio_irled2 = %d\nir940 = false\n" % IRLED2)
    stub.set_scene(luma=120, gain=500, ev=4000)
    ric = Ric("manualgpio", conf)
    if not ric.wait_running():
        result(False, "manual-gpio: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)

    gm, mm = watch.mark(), stub.mark()
    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "ircut", "value": "night"})
    ok = r is not None and r.get("status") == "ok" and r.get("mode") == "auto"
    w = wait_pulse(watch, gm, IRCUT2, IRCUT1)
    result(ok and w is not None,
           "manual ircut: filter pulses alone", "resp=%s width=%s" % (r, w))
    time.sleep(0.3)
    result(stub.modes_since(mm) == [],
           "manual ircut: no ISP call, no mode change", str(stub.modes_since(mm)))

    gm = watch.mark()
    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "ir850", "value": "on"})
    ok = r is not None and r.get("status") == "ok" and wait_last(watch, gm, IRLED, "1")
    result(ok, "manual ir850: bank lights alone", str(r))

    gm = watch.mark()
    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "ir940", "value": "on"})
    ok = r is not None and r.get("status") == "ok" and wait_last(watch, gm, IRLED2, "1")
    result(ok, "manual ir940: disabled bank still lights (manual beats flag)",
           str(r))

    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "ircut", "value": "sideways"})
    r2 = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "ir850", "value": "maybe"})
    result(r is not None and r.get("status") == "error"
           and r2 is not None and r2.get("status") == "error",
           "manual control: bad values rejected", "%s %s" % (r, r2))

    # auto reasserts what it manages; the disabled bank keeps manual state
    ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "mode", "value": "night"})
    time.sleep(0.3)
    gm = watch.mark()
    ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "mode", "value": "day"})
    ok = wait_last(watch, gm, IRLED, "0")
    result(ok and last_value(watch.since(gm), IRLED2) is None,
           "manual control: auto reasserts managed banks, disabled bank keeps manual state",
           str(watch.since(gm)))
    ric.stop()

    # a bank with no pin errors instead of pretending
    stub.set_scene(luma=120, gain=500, ev=4000)
    ric = Ric("manualnopin", LUMA_CONF)  # no gpio_irled2
    if not ric.wait_running():
        result(False, "manual-gpio: second ric start", "no 'ric running'")
        ric.stop()
        return
    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "ir940", "value": "on"})
    result(r is not None and r.get("status") == "error",
           "manual ir940: missing pin is an error", str(r))
    ric.stop()


def scenario_ctrl_extras(stub, watch):
    """isp-mode changes only the ISP; set-threshold retunes live."""
    stub.set_scene(luma=120, gain=500, ev=4000)
    ric = Ric("ctrlx", LUMA_CONF)
    if not ric.wait_running():
        result(False, "ctrl-extras: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)

    gm, mm = watch.mark(), stub.mark()
    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "isp-mode", "value": "night"})
    ok = r is not None and r.get("isp_mode") == "night" and r.get("hw_state") == "day"
    result(ok, "isp-mode: ISP flips while hw_state stays", str(r))
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 3)
    result(ok, "isp-mode: reaches rvd", str(stub.modes_since(mm)))
    time.sleep(0.3)
    result(watch.since(gm) == [], "isp-mode: no GPIO movement",
           str(watch.since(gm)))
    ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "isp-mode", "value": "day"})

    # Live rethreshold: bright luma 120 becomes "night" once the
    # threshold moves above it.
    mm = stub.mark()
    r = ctrl_cmd(
        RUN_DIR + "/ric.sock",
        {"cmd": "set-threshold", "key": "night_luma", "value": 200},
    )
    result(r is not None and r.get("status") == "ok",
           "set-threshold: night_luma accepted", str(r))
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
    result(ok, "set-threshold: new threshold drives a transition",
           str(stub.modes_since(mm)))
    r = ctrl_cmd(
        RUN_DIR + "/ric.sock",
        {"cmd": "set-threshold", "key": "night_luma", "value": 999},
    )
    result(r is not None and r.get("status") == "error",
           "set-threshold: out-of-range rejected", str(r))
    r = ctrl_cmd(
        RUN_DIR + "/ric.sock",
        {"cmd": "set-threshold", "key": "bogus_key", "value": 1},
    )
    result(r is not None and r.get("status") == "error",
           "set-threshold: unknown key rejected", str(r))
    ric.stop()

    # night_gain and day_gain_pct retune live too: a bright-luma scene
    # goes night once its gain sits above a lowered night_gain, and the
    # baseline ratio releases it once day_gain_pct is raised over the
    # current gain fraction.
    stub.set_scene(luma=120, gain=50000, ev=4000)
    ric = Ric("ctrlx2", LUMA_CONF)
    if not ric.wait_running():
        result(False, "ctrl-extras: second ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)
    mm = stub.mark()
    ctrl_cmd(RUN_DIR + "/ric.sock",
             {"cmd": "set-threshold", "key": "night_gain", "value": 40000})
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
    result(ok, "set-threshold: night_gain drives the gain term live",
           str(stub.modes_since(mm)))
    time.sleep((3 + 1) * POLL_MS / 1000.0)  # cooldown: baseline = 50000
    # gain holds at 60% of baseline: below 25% never comes, 70% releases
    stub.set_scene(luma=120, gain=30000, ev=4000)
    time.sleep(1.0)
    still_night = "day" not in stub.modes_since(mm)
    ctrl_cmd(RUN_DIR + "/ric.sock",
             {"cmd": "set-threshold", "key": "day_gain_pct", "value": 70})
    ok = wait_for(lambda: "day" in stub.modes_since(mm), 4)
    result(still_night and ok,
           "set-threshold: day_gain_pct rescales the release ratio live",
           str(stub.modes_since(mm)))
    ric.stop()


def scenario_rvd_lost(stub, watch):
    """rvd dying mid-run must be said out loud. The poll's query fails,
    the branch returns, and day/night freezes exactly like the dead
    photoresistor of issue #18, one layer up -- at default log level
    nothing prints at all. A normal rvd restart takes a few seconds
    and must stay quiet; only a sustained outage is worth a line."""
    stub.set_scene(luma=120, gain=500, ev=4000)
    ric = Ric("rvdlost", LUMA_CONF)
    if not ric.wait_running():
        result(False, "rvd-lost: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)

    mm0 = stub.mark()
    stub.pause()
    time.sleep(3)  # a routine daemon restart is about this long
    stub.resume()
    time.sleep(1)
    log = ric.read_log()
    result("not answered" not in log,
           "rvd-lost: a restart-sized gap stays quiet", log[-200:])
    # Even a quiet gap can hide an rvd restart, and a restarted rvd
    # boots its ISP in day mode: recovery must re-assert the current
    # mode (idempotent here -- the camera is in day).
    ok = wait_for(lambda: "day" in stub.modes_since(mm0), 4)
    result(ok, "rvd-lost: even a quiet gap re-asserts the ISP mode",
           str(stub.modes_since(mm0)))
    mm = stub.mark()
    stub.set_scene(luma=5, gain=20000, ev=100000)
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
    result(ok, "rvd-lost: transitions run after the gap",
           str(stub.modes_since(mm)))

    mm2 = stub.mark()
    stub.pause()
    ok = wait_for(lambda: "not answered" in ric.read_log(), 16)
    result(ok, "rvd-lost: a sustained outage is reported",
           ric.read_log()[-300:])
    if ok:
        time.sleep(3)
        n = len(re.findall(r"not answered", ric.read_log()))
        result(n == 1, "rvd-lost: the outage warning is rate-limited",
               "%d warnings" % n)
    stub.resume()
    ok = wait_for(lambda: "answering again" in ric.read_log(), 6)
    result(ok, "rvd-lost: recovery is reported", ric.read_log()[-300:])
    # The camera sat in NIGHT through the outage; a restarted rvd would
    # be streaming day-mode colors under lit IR. The recovery must put
    # the ISP back where the filter and LEDs already are.
    ok = wait_for(lambda: "night" in stub.modes_since(mm2), 4)
    result(ok, "rvd-lost: recovery re-asserts the night ISP mode",
           str(stub.modes_since(mm2)))
    # The outage froze the post-switch cooldown (failed polls return
    # before it decrements), so it finishes only now -- let the night
    # baseline sample from the still-dark scene before brightening.
    time.sleep((3 + 2) * POLL_MS / 1000.0)
    mm = stub.mark()
    stub.set_scene(luma=120, gain=500, ev=4000)
    ok = wait_for(lambda: "day" in stub.modes_since(mm), 6)
    result(ok, "rvd-lost: polling drives day/night again",
           str(stub.modes_since(mm)))

    # Forced modes get the same treatment: the liveness probe and the
    # recovery re-assert cannot depend on AUTO being in charge -- a
    # forced-night camera whose rvd restarts is just as wrong.
    ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "mode", "value": "night"})
    time.sleep(0.5)
    mmf = stub.mark()
    stub.pause()
    time.sleep(3)
    stub.resume()
    ok = wait_for(lambda: "night" in stub.modes_since(mmf), 6)
    result(ok, "rvd-lost: a forced mode re-asserts after recovery too",
           str(stub.modes_since(mmf)))
    ric.stop()


def scenario_night_fps(stub, watch):
    """Optional night sensor rate. Policy lives in ric: entering night
    sends rvd set-sensor-fps <night_fps>, entering day restores with 0,
    startup restores too (heals a ric restart mid-night), the recovery
    re-assert re-applies after an rvd restart, the runtime knob takes
    effect in the regime it governs, and a backend that answers with an
    error parks the feature for the run instead of hammering it."""
    conf = LUMA_CONF + "night_fps = 12\n"
    stub.set_scene(luma=120, gain=500, ev=4000)
    fm = stub.fps_mark()
    ric = Ric("nightfps", conf)
    if not ric.wait_running():
        result(False, "night-fps: ric start", "no 'ric running'")
        ric.stop()
        return
    # Startup forces day, which restores the base rate: a ric that
    # died mid-night must not leave the sensor slow forever.
    ok = wait_for(lambda: 0 in stub.fps_since(fm), 5)
    result(ok, "night-fps: startup restores the base rate",
           str(stub.fps_since(fm)))

    fm = stub.fps_mark()
    stub.set_scene(luma=5, gain=20000, ev=100000)
    ok = wait_for(lambda: 12 in stub.fps_since(fm), 6)
    result(ok, "night-fps: night entry applies the configured rate",
           str(stub.fps_since(fm)))

    # Wait out cooldown + settle so the baseline lands before dawn.
    time.sleep((3 + 2) * POLL_MS / 1000.0)
    fm = stub.fps_mark()
    stub.set_scene(luma=120, gain=500, ev=4000)
    ok = wait_for(lambda: 0 in stub.fps_since(fm), 8)
    result(ok, "night-fps: day return restores the base rate",
           str(stub.fps_since(fm)))

    # Back to night, then an rvd restart: the recovery re-assert must
    # re-apply the night rate -- a restarted rvd is at its boot rate.
    fm = stub.fps_mark()
    stub.set_scene(luma=5, gain=20000, ev=100000)
    if not wait_for(lambda: 12 in stub.fps_since(fm), 6):
        result(False, "night-fps: night re-entry", str(stub.fps_since(fm)))
        ric.stop()
        return
    fm = stub.fps_mark()
    stub.pause()
    time.sleep(3)
    stub.resume()
    ok = wait_for(lambda: 12 in stub.fps_since(fm), 6)
    result(ok, "night-fps: recovery re-asserts the night rate",
           str(stub.fps_since(fm)))

    # Runtime knob, active regime: a new rate applies now; switching it
    # off mid-night restores the base rate before the knob forgets.
    fm = stub.fps_mark()
    ctrl_cmd(RUN_DIR + "/ric.sock",
             {"cmd": "set-threshold", "key": "night_fps", "value": 15})
    ok = wait_for(lambda: 15 in stub.fps_since(fm), 4)
    result(ok, "night-fps: runtime change applies mid-night",
           str(stub.fps_since(fm)))
    fm = stub.fps_mark()
    ctrl_cmd(RUN_DIR + "/ric.sock",
             {"cmd": "set-threshold", "key": "night_fps", "value": 0})
    ok = wait_for(lambda: 0 in stub.fps_since(fm), 4)
    result(ok, "night-fps: switching off mid-night restores the base",
           str(stub.fps_since(fm)))
    # Off means off: the next transition moves no sensor rate.
    time.sleep((3 + 2) * POLL_MS / 1000.0)
    fm, mm = stub.fps_mark(), stub.mark()
    stub.set_scene(luma=120, gain=500, ev=4000)
    wait_for(lambda: "day" in stub.modes_since(mm), 8)
    time.sleep(0.5)
    result(stub.fps_since(fm) == [],
           "night-fps: disabled sends no rate commands",
           str(stub.fps_since(fm)))

    # A backend that cannot do rates: the error answer parks the
    # feature for the run, warned once, and stops the traffic.
    stub.fps_error = True
    ctrl_cmd(RUN_DIR + "/ric.sock",
             {"cmd": "set-threshold", "key": "night_fps", "value": 12})
    stub.set_scene(luma=5, gain=20000, ev=100000)
    ok = wait_for(lambda: "night_fps disabled" in ric.read_log(), 8)
    result(ok, "night-fps: an unsupported backend parks the feature",
           ric.read_log()[-300:])
    stub.fps_error = False
    fm = stub.fps_mark()
    time.sleep((3 + 2) * POLL_MS / 1000.0)
    stub.set_scene(luma=120, gain=500, ev=4000)
    time.sleep(2)
    result(stub.fps_since(fm) == [],
           "night-fps: parked means no further attempts",
           str(stub.fps_since(fm)))
    ric.stop()


def scenario_photo_interference(stub, watch):
    """After a ratio-path day trigger the photo machine watches for a
    false day. A rise in ev (darker again) past the interference band
    must hand the mode back to night -- that was headlights. A further
    drop (genuinely brighter) must retire the watch back to
    night-detect without touching the mode."""
    conf = GPIO_CONF + "trigger = photo\nphoto_ev_day = 90000\n"
    for arm, drive_ev, expect_night in (
        ("false-day", 100000, True),
        ("genuine-bright", 60000, False),
    ):
        stub.set_scene(luma=100, gain=500, ev=3000, rgain=140, bgain=140)
        ric = Ric("photoint-%s" % arm, conf, poll_ms=100)
        if not ric.wait_running() or not wait_for(
            lambda: "photo AWB calibrated" in ric.read_log(), 6
        ):
            result(False, "interfere %s: bring-up" % arm, ric.read_log()[-200:])
            ric.stop()
            continue
        stub.set_scene(luma=10, gain=20000, ev=100000, rgain=170, bgain=140)
        if not wait_for(lambda: "photo night trigger" in ric.read_log(), 10):
            result(False, "interfere %s: night entry" % arm, ric.read_log()[-200:])
            ric.stop()
            continue
        stub.set_scene(luma=60, gain=2000, ev=85000, rgain=150, bgain=140)
        if not wait_for(lambda: "photo day trigger" in ric.read_log(), 15):
            result(False, "interfere %s: ratio day entry" % arm,
                   ric.read_log()[-200:])
            ric.stop()
            continue
        # the interference ring seeds from ~8 stable polls of the same ev
        time.sleep(1.2)
        mm = stub.mark()
        stub.set_scene(luma=60, gain=2000, ev=drive_ev, rgain=150, bgain=140)
        if expect_night:
            ok = wait_for(lambda: "night" in stub.modes_since(mm), 5)
            result(ok and "interfere: false day" in ric.read_log(),
                   "interfere: a false day is handed back to night",
                   ric.read_log()[-250:])
        else:
            ok = wait_for(lambda: "interfere: genuine bright" in ric.read_log(), 5)
            time.sleep(0.5)
            result(ok and "night" not in stub.modes_since(mm),
                   "interfere: genuine brightness retires the watch, mode holds",
                   str(stub.modes_since(mm)) + " " + ric.read_log()[-200:])
        ric.stop()


def scenario_default_topologies(stub, watch):
    """Boards as shipped, remaining shapes: a dual-bank board must light
    only its 850 by default (the 940 auto-enable must not overreach),
    and a single-pin ircut board discovered from a number-form json
    must level-drive its one pin."""
    orig = open("/etc/thingino.json").read()
    conf = "trigger = luma\nnight_luma = 20\nhysteresis_sec = 2\n"

    try:
        with open("/etc/thingino.json", "w") as f:
            json.dump({"gpio": {"ircut": "%d %d" % (IRCUT1, IRCUT2),
                                "ir850": IRLED, "ir940": IRLED2}}, f)
        stub.set_scene(luma=120, gain=500, ev=4000)
        ric = Ric("dualbank", conf)
        if not ric.wait_running():
            result(False, "dual-bank default: ric start", "no 'ric running'")
            ric.stop()
            return
        time.sleep(0.5)
        gm, mm = watch.mark(), stub.mark()
        stub.set_scene(luma=5, gain=20000, ev=100000)
        ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
        ok = ok and wait_last(watch, gm, IRLED, "1")
        result(ok and last_value(watch.since(gm), IRLED2) is None,
               "dual-bank default: 850 lights, 940 stays opt-in",
               str(watch.since(gm)))
        ric.stop()

        with open("/etc/thingino.json", "w") as f:
            json.dump({"gpio": {"ircut": IRCUT1, "ir850": IRLED}}, f)
        stub.set_scene(luma=120, gain=500, ev=4000)
        ric = Ric("singlepin", conf)
        if not ric.wait_running():
            result(False, "single-pin default: ric start", "no 'ric running'")
            ric.stop()
            return
        time.sleep(0.5)
        gm, mm = watch.mark(), stub.mark()
        stub.set_scene(luma=5, gain=20000, ev=100000)
        ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
        ok = ok and wait_last(watch, gm, IRCUT1, "0")
        result(ok and last_value(watch.since(gm), IRCUT2) is None,
               "single-pin default: level drive from a number-form json",
               str(watch.since(gm)))
        ric.stop()
    finally:
        with open("/etc/thingino.json", "w") as f:
            f.write(orig)


def scenario_gpio_failure(stub, watch):
    """A GPIO that stops accepting writes must cost a warning, not the
    daemon: ircut2's value becomes /dev/full (opens, every write fails
    ENOSPC) and the ir850 value becomes a directory (open itself
    fails). Both must be named in the log, the healthy pin must still
    actuate, and the transition must complete."""
    v2 = "%s/gpio%d/value" % (GPIO_ROOT, IRCUT2)
    vl = "%s/gpio%d/value" % (GPIO_ROOT, IRLED)
    os.rename(v2, v2 + ".save")
    os.symlink("/dev/full", v2)
    os.rename(vl, vl + ".save")
    os.mkdir(vl)
    try:
        stub.set_scene(luma=120, gain=500, ev=4000)
        ric = Ric("gpiofail", LUMA_CONF)
        if not ric.wait_running():
            result(False, "gpio-failure: ric start", "no 'ric running'")
            ric.stop()
            return
        time.sleep(0.5)
        gm, mm = watch.mark(), stub.mark()
        stub.set_scene(luma=5, gain=20000, ev=100000)
        ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
        result(ok, "gpio-failure: transition still completes",
               str(stub.modes_since(mm)))
        log = ric.read_log()
        result("gpio %d set: write failed" % IRCUT2 in log,
               "gpio-failure: a failing write is named", log[-250:])
        result("gpio %d set:" % IRLED in log,
               "gpio-failure: a pin that will not open is named", log[-250:])
        result(wait_last(watch, gm, IRCUT1, "0"),
               "gpio-failure: the healthy pin still actuates",
               str(watch.since(gm)))
        s = ric_status()
        result(s is not None and s.get("status") == "ok",
               "gpio-failure: daemon healthy after the storm", str(s))
        ric.stop()
    finally:
        os.unlink(v2)
        os.rename(v2 + ".save", v2)
        os.rmdir(vl)
        os.rename(vl + ".save", vl)


def scenario_disabled(stub, watch):
    """enabled=false must exit promptly and touch nothing."""
    gm = watch.mark()
    conf = "%s/disabled.conf" % WORK
    with open(conf, "w") as f:
        f.write("[ircut]\nenabled = false\n\n[log]\nlevel = debug\n")
    proc = subprocess.Popen(
        [RIC_BIN, "-c", conf, "-f", "-d"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    try:
        proc.wait(timeout=5)
        exited = True
    except subprocess.TimeoutExpired:
        exited = False
        proc.kill()
        proc.wait()
    result(exited, "enabled=false exits promptly", "still running after 5s")
    result(watch.since(gm) == [], "enabled=false touches no GPIO",
           str(watch.since(gm)))


def scenario_hostile_config(stub, watch):
    """Config-file values outside every documented range must clamp, not
    crash: poll_interval_ms = 0 divided two probe-path expressions
    before load_config clamped it, so a typo'd config took ric down
    with SIGFPE the first time a probe armed."""
    conf = (GPIO_CONF
            + "trigger = luma\nnight_luma = 300\nnight_gain = 80000\n"
            + "day_gain_pct = -5\nhysteresis_sec = 0\npulse_ms = 100\n"
            + "poll_interval_ms = 0\nprobe_gain_pct = 90\n"
            + "probe_holdoff_sec = 999999999\nprobe_recheck_sec = 999999999\n")
    stub.set_scene(luma=120, gain=500, ev=4000)
    ric = Ric("hostile", conf)
    if not ric.wait_running():
        result(False, "hostile config: ric start", "no 'ric running'")
        ric.stop()
        return
    result("clamped" in ric.read_log(),
           "hostile config: out-of-range keys named in the log",
           ric.read_log()[-300:])

    mm = stub.mark()
    stub.set_scene(luma=5, gain=90000, ev=200000)
    if not wait_for(lambda: "night" in stub.modes_since(mm), 8):
        result(False, "hostile config: reaches night", str(stub.modes_since(mm)))
        ric.stop()
        return
    # Baseline settles, then a dip arms the probe; pre-fix the probe's
    # holdoff arithmetic divided by the raw poll_interval_ms = 0.
    time.sleep(2.5)
    stub.set_scene(luma=5, gain=30000, ev=60000)
    ok = wait_for(lambda: "ambient probe" in ric.read_log(), 10)
    result(ok, "hostile config: probe arms", ric.read_log()[-200:])
    ok = wait_for(lambda: "probe found darkness" in ric.read_log(), 10)
    result(ok, "hostile config: probe completes without SIGFPE",
           ric.read_log()[-200:])
    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "status"})
    result(r is not None and r.get("status") == "ok",
           "hostile config: ric alive after the probe", str(r))
    ric.stop()


def scenario_ctrl_contract(stub, watch):
    """The ctrl surface validates at the boundary like every other
    daemon: a mode outside auto|day|night is an error (and never
    persisted), status is an explicit verb, and an unknown command
    is an error instead of a status alias."""
    stub.set_scene(luma=120, gain=500, ev=4000)
    ric = Ric("ctrlc", LUMA_CONF)
    if not ric.wait_running():
        result(False, "ctrl-contract: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.3)

    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "mode", "value": "banana"})
    ok = r is not None and r.get("status") == "error"
    result(ok, "ctrl-contract: mode banana rejected", str(r))
    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "mode"})
    ok = r is not None and r.get("mode") == "auto"
    result(ok, "ctrl-contract: mode unchanged after rejection", str(r))

    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "status"})
    ok = (r is not None and r.get("status") == "ok" and "mode" in r
          and "state" in r)
    result(ok, "ctrl-contract: explicit status carries mode and state", str(r))

    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "bogus-cmd"})
    ok = r is not None and r.get("status") == "error"
    result(ok, "ctrl-contract: unknown command is an error", str(r))
    ric.stop()


def scenario_photo_threshold_order(stub, watch):
    """Inverted photo thresholds (ev_deep under ev_night) break the
    deep-count logic silently; the load must say so."""
    conf = (GPIO_CONF + "trigger = photo\nphoto_ev_night = 50000\n"
            + "photo_ev_deep = 10000\n")
    stub.set_scene(luma=100, gain=500, ev=3000, rgain=140, bgain=140)
    ric = Ric("photoorder", conf, poll_ms=100)
    if not ric.wait_running():
        result(False, "photo-order: ric start", "no 'ric running'")
        ric.stop()
        return
    ok = wait_for(lambda: "photo thresholds out of order" in ric.read_log(), 3)
    result(ok, "photo-order: inverted thresholds warned at load",
           ric.read_log()[-300:])
    ric.stop()


def scenario_json_discovery(stub, watch):
    """No pins in the config: they come from /etc/thingino.json (the
    sandbox owns that file via an overlay)."""
    conf = "trigger = luma\nnight_luma = 20\nnight_gain = 80000\nhysteresis_sec = 2\n"
    stub.set_scene(luma=120, gain=500, ev=4000)
    ric = Ric("discover", conf)
    if not ric.wait_running():
        result(False, "discovery: ric start", "no 'ric running'")
        ric.stop()
        return
    log = ric.read_log()
    ok = "GPIOs from /etc/thingino.json: ircut=%d ircut2=%d irled=%d" % (
        IRCUT1, IRCUT2, IRLED) in log
    result(ok, "discovery: pins resolved from thingino.json", log[:400])
    time.sleep(0.3)
    gm, mm = watch.mark(), stub.mark()
    stub.set_scene(luma=5, gain=20000, ev=100000)
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
    ok = ok and wait_last(watch, gm, IRCUT2, "0") and wait_last(watch, gm, IRLED, "1")
    result(ok, "discovery: discovered pins actuate", str(watch.since(gm)))
    ric.stop()


def scenario_active_low(stub, watch):
    """Inverted single-pin hardware (the tapo c100 class): logical
    drives must come out raw-inverted from the config flags and from
    the {pin, active_low} json object alike, and an inverted bank must
    never see a raw-low (lit) transient, park included."""
    conf_tail = ("trigger = luma\nnight_luma = 20\nnight_gain = 80000\n"
                 "day_gain_pct = 25\nhysteresis_sec = 2\n")
    conf = ("gpio_ircut = %d\ngpio_ircut_active_low = true\n"
            "gpio_irled = %d\ngpio_irled_active_low = true\n"
            % (IRCUT1, IRLED)) + conf_tail

    stub.set_scene(luma=120, gain=500, ev=4000)
    gm, mm = watch.mark(), stub.mark()
    ric = Ric("alowconf", conf)
    if not ric.wait_running():
        result(False, "active-low conf: ric start", "no 'ric running'")
        ric.stop()
        return
    ok = wait_for(lambda: "day" in stub.modes_since(mm), 5)
    ok = ok and wait_last(watch, gm, IRCUT1, "0") \
        and wait_last(watch, gm, IRLED, "1")
    led = [c for _, p, _, c in watch.since(gm) if p == IRLED]
    result(ok and led and led[0] == "1" and "0" not in led,
           "active-low conf: day is raw-inverted, bank never flashes",
           str(watch.since(gm)))
    gm, mm = watch.mark(), stub.mark()
    stub.set_scene(luma=5, gain=20000, ev=100000)
    ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
    ok = ok and wait_last(watch, gm, IRCUT1, "1") \
        and wait_last(watch, gm, IRLED, "0")
    result(ok, "active-low conf: night holds raw 1, bank lights on raw 0",
           str(watch.since(gm)))
    ric.stop()

    orig = open("/etc/thingino.json").read()
    try:
        with open("/etc/thingino.json", "w") as f:
            json.dump({"gpio": {
                "ircut": {"pin": IRCUT1, "active_low": True},
                "ir850": {"pin": IRLED, "active_low": True}}}, f)
        stub.set_scene(luma=120, gain=500, ev=4000)
        ric = Ric("alowjson", conf_tail)
        if not ric.wait_running():
            result(False, "active-low json: ric start", "no 'ric running'")
            ric.stop()
            return
        log = ric.read_log()
        result("ircut-active-low" in log and "irled-active-low" in log,
               "active-low json: discovery reports the inversion", log[:400])
        time.sleep(0.3)
        gm, mm = watch.mark(), stub.mark()
        stub.set_scene(luma=5, gain=20000, ev=100000)
        ok = wait_for(lambda: "night" in stub.modes_since(mm), 4)
        ok = ok and wait_last(watch, gm, IRCUT1, "1") \
            and wait_last(watch, gm, IRLED, "0")
        result(ok, "active-low json: object-form pins drive raw-inverted",
               str(watch.since(gm)))
        ric.stop()
    finally:
        with open("/etc/thingino.json", "w") as f:
            f.write(orig)


def scenario_live_bank_policy(stub, watch):
    """The ir850/ir940 enable flags are live policy: disabling a bank
    mid-night drops its LED immediately with the mode holding (the
    window-reflection case), enabling lights it back, and the flag
    round-trips through get-thresholds. Out-of-range values refuse."""
    stub.set_scene(luma=70, gain=4500, ev=1200000)
    ric = Ric("bankpol", LUMA_CONF)
    if not ric.wait_running():
        result(False, "bank-policy: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)
    mm = stub.mark()
    stub.set_scene(luma=6, gain=8192, ev=50000000)
    if not wait_for(lambda: "night" in stub.modes_since(mm), 4):
        result(False, "bank-policy: night entry", str(stub.modes_since(mm)))
        ric.stop()
        return
    gm = watch.mark()
    r = ctrl_cmd(RUN_DIR + "/ric.sock",
                 {"cmd": "set-threshold", "key": "ir850", "value": 0})
    off_now = wait_for(lambda: last_value(watch.since(gm), IRLED) == "0", 3)
    result(r is not None and r.get("status") == "ok" and off_now,
           "bank-policy: disabling ir850 mid-night drops the LED now",
           "resp=%s events=%s" % (r, watch.since(gm)))
    result("day" not in stub.modes_since(mm),
           "bank-policy: the mode holds through the policy change",
           str(stub.modes_since(mm)))

    gm2 = watch.mark()
    r = ctrl_cmd(RUN_DIR + "/ric.sock",
                 {"cmd": "set-threshold", "key": "ir850", "value": 1})
    on_now = wait_for(lambda: last_value(watch.since(gm2), IRLED) == "1", 3)
    result(r is not None and r.get("status") == "ok" and on_now,
           "bank-policy: re-enabling lights it back",
           "resp=%s events=%s" % (r, watch.since(gm2)))

    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "get-thresholds"})
    result(r is not None and r.get("ir850") is True and "pulse_ms" in r,
           "bank-policy: flags and pulse_ms round-trip through get-thresholds",
           str(r))
    r = ctrl_cmd(RUN_DIR + "/ric.sock",
                 {"cmd": "set-threshold", "key": "ir850", "value": 2})
    result(r is not None and r.get("status") != "ok",
           "bank-policy: a non-boolean value is refused", str(r))

    # ADC cross-validation and pulse_ms bounds, on the same instance.
    r = ctrl_cmd(RUN_DIR + "/ric.sock",
                 {"cmd": "set-threshold", "key": "adc_night", "value": 700})
    result(r is not None and r.get("status") != "ok",
           "bank-policy: adc_night above adc_day is refused", str(r))
    r1 = ctrl_cmd(RUN_DIR + "/ric.sock",
                  {"cmd": "set-threshold", "key": "adc_day", "value": 900})
    r2 = ctrl_cmd(RUN_DIR + "/ric.sock",
                  {"cmd": "set-threshold", "key": "adc_night", "value": 700})
    result(all(x is not None and x.get("status") == "ok" for x in (r1, r2)),
           "bank-policy: ordered adc updates land", "%s %s" % (r1, r2))
    r = ctrl_cmd(RUN_DIR + "/ric.sock",
                 {"cmd": "set-threshold", "key": "pulse_ms", "value": 0})
    result(r is not None and r.get("status") != "ok",
           "bank-policy: pulse_ms zero is refused", str(r))
    ric.stop()


def scenario_live_trigger_switch(stub, watch):
    """set-trigger swaps the judge without bouncing the regime: switch
    luma->gain mid-night, the mode holds, the machinery re-arms, and
    the GAIN trigger's own day condition then drives the return to day.
    Unknown names refuse; a same-trigger switch is a quiet ok."""
    conf = LUMA_CONF + "night_threshold = 40000\nday_threshold = 25000\n"
    stub.set_scene(luma=70, gain=4500, ev=1200000)
    ric = Ric("trigsw", conf)
    if not ric.wait_running():
        result(False, "trigger-switch: ric start", "no 'ric running'")
        ric.stop()
        return
    time.sleep(0.5)
    mm = stub.mark()
    # The dark scene must read dark to BOTH judges: luma 6 for the luma
    # trigger that enters night, gain above night_threshold so the gain
    # trigger holds night after the switch instead of instantly ruling
    # day (its model reads any gain under day_threshold as a lit scene).
    stub.set_scene(luma=6, gain=50000, ev=50000000)
    if not wait_for(lambda: "night" in stub.modes_since(mm), 4):
        result(False, "trigger-switch: night entry", str(stub.modes_since(mm)))
        ric.stop()
        return

    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "set-trigger", "value": "bogus"})
    result(r is not None and r.get("status") != "ok",
           "trigger-switch: unknown trigger is refused", str(r))

    mm2 = stub.mark()
    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "set-trigger", "value": "gain"})
    result(r is not None and r.get("status") == "ok",
           "trigger-switch: luma -> gain accepted", str(r))
    time.sleep(2 * POLL_MS / 1000.0)
    result("day" not in stub.modes_since(mm2),
           "trigger-switch: the regime survives the switch",
           str(stub.modes_since(mm2)))
    g = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "get-thresholds"})
    result(g is not None and g.get("trigger") == "gain",
           "trigger-switch: get-thresholds reports the new judge", str(g))

    r = ctrl_cmd(RUN_DIR + "/ric.sock", {"cmd": "set-trigger", "value": "gain"})
    result(r is not None and r.get("status") == "ok",
           "trigger-switch: same trigger is a quiet ok", str(r))

    # The new judge rules: gain below day_threshold ends the night. The
    # re-arm's cooldown can extend through the settle window (17 polls)
    # before evaluation resumes, then the hysteresis runs -- size the
    # wait for the worst case, not the happy path.
    time.sleep((3 + 1) * POLL_MS / 1000.0)
    mm3 = stub.mark()
    stub.set_scene(luma=70, gain=1300, ev=370000)
    ok = wait_for(lambda: "day" in stub.modes_since(mm3), 10)
    result(ok, "trigger-switch: the gain trigger drives the next transition",
           str(stub.modes_since(mm3)))
    ric.stop()


def main():
    setup_sandbox()
    stub = StubRvd()
    watch = GpioWatch()

    # Legs for behavior that is specified but not yet landed go here and
    # are reported as KNOWN-FAIL instead of failing the suite; strict mode
    # (RIC_SUITE_STRICT=1) counts them regardless. Empty since the PR #14
    # work landed -- a regression in those legs now fails like any other.
    known_fail = set()
    if os.environ.get("RIC_SUITE_STRICT", "") == "1":
        known_fail.clear()

    scenarios = [
        scenario_startup_park,
        scenario_startup_ae_walk,
        scenario_startup_dark,
        scenario_day_switch_ae_walk,
        scenario_dusk_dawn,
        scenario_hysteresis,
        scenario_cooldown,
        scenario_covered_lens_baseline,
        scenario_baseline_refresh,
        scenario_probe_dawn,
        scenario_probe_dark_restore,
        scenario_noir_luma_dawn,
        scenario_probe_slow_ae,
        scenario_probe_recheck,
        scenario_recheck_rearm,
        scenario_ctrl,
        scenario_ctrl_extras,
        scenario_manual_gpio,
        scenario_mode_reassert,
        scenario_single_gpio,
        scenario_ir_combos,
        scenario_ir940_only_board,
        scenario_startup_forced,
        scenario_pulse_width,
        scenario_gain_trigger,
        scenario_photo,
        scenario_photo_night_boot,
        scenario_photo_day_ratio,
        scenario_photo_interference,
        scenario_photo_no_ev,
        scenario_zero_exposure,
        scenario_partial_fields,
        scenario_adc,
        scenario_adc_dead,
        scenario_rvd_lost,
        scenario_night_fps,
        scenario_default_topologies,
        scenario_gpio_failure,
        scenario_disabled,
        scenario_json_discovery,
        scenario_active_low,
        scenario_hostile_config,
        scenario_ctrl_contract,
        scenario_photo_threshold_order,
        scenario_live_bank_policy,
        scenario_live_trigger_switch,
    ]
    only = os.environ.get("RIC_SCENARIO", "")
    if only:
        scenarios = [sc for sc in scenarios if only in sc.__name__]
    for sc in scenarios:
        print("== %s" % sc.__name__, flush=True)
        try:
            sc(stub, watch)
        except Exception as e:  # a scenario crash is a failure, not an abort
            result(False, sc.__name__, "exception: %r" % e)

    hard = [f for f in FAILED if f not in known_fail]
    soft = [f for f in FAILED if f in known_fail]
    print("", flush=True)
    for f in soft:
        print("  KNOWN-FAIL (pending PR #14): %s" % f, flush=True)
    print(
        "ric suite: %d pass, %d fail (%d known)" % (PASS, len(hard), len(soft)),
        flush=True,
    )
    sys.exit(1 if hard else 0)


if __name__ == "__main__":
    main()
