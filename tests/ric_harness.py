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
        self.modes = []  # (monotonic, "day"|"night")
        self.lock = threading.Lock()
        path = RUN_DIR + "/rvd.sock"
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(path)
        self.srv.listen(8)
        threading.Thread(target=self._loop, daemon=True).start()

    def set_scene(self, **kw):
        with self.lock:
            self.scene = dict(kw)

    def mark(self):
        with self.lock:
            return len(self.modes)

    def modes_since(self, mark):
        with self.lock:
            return [m for _, m in self.modes[mark:]]

    def _loop(self):
        while True:
            conn, _ = self.srv.accept()
            try:
                req = recv_msg(conn)
                if req is None:
                    continue
                cmd = json.loads(req).get("cmd", "")
                if cmd == "get-exposure":
                    with self.lock:
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
    + "day_gain_pct = 25\nhysteresis_sec = 2\n"
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
    # move, and afterwards the baseline was sampled from THIS low gain, so
    # day now needs < 25% of it: still nothing may move.
    mm2 = stub.mark()
    stub.set_scene(luma=5, gain=1000, ev=100000)
    time.sleep((3 + 2 + 2) * POLL_MS / 1000.0)
    result(
        "day" not in stub.modes_since(mm2),
        "no flap during cooldown; baseline from settled scene",
        str(stub.modes_since(mm2)),
    )
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
    ric.stop()


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
        scenario_dusk_dawn,
        scenario_hysteresis,
        scenario_cooldown,
        scenario_ctrl,
        scenario_ctrl_extras,
        scenario_single_gpio,
        scenario_ir_combos,
        scenario_startup_forced,
        scenario_gain_trigger,
        scenario_photo,
        scenario_photo_day_ratio,
        scenario_photo_no_ev,
        scenario_zero_exposure,
        scenario_partial_fields,
        scenario_adc,
        scenario_disabled,
        scenario_json_discovery,
    ]
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
