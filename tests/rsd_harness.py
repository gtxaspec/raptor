#!/usr/bin/env python3
"""Security behavior harness for rsd against the real daemon.

Runs inside the private user/mount/net namespace set up by test-rsd.sh, so
every port, /dev/shm and /run is ours. create_rings supplies the video and
audio rings; each leg starts its own rsd with the config it needs.
"""
import os
import re
import select
import signal
import socket
import subprocess
import sys
import time

OUT = os.environ.get("RSD_OUT", "asan-out")
WORK = os.environ.get("RSD_TEST_WORK", "/tmp/rsd-test")
PORT = 15554

PASS = 0
FAILED = []
LOGS = []


def check(name, ok, detail=""):
    global PASS
    if ok:
        PASS += 1
        print("  ok   " + name)
    else:
        FAILED.append(name)
        print("  FAIL " + name + (" -- " + detail if detail else ""))


class Lab:
    """One rsd instance with its own config, on top of the shared rings."""

    def __init__(self, name, rtsp_extra=""):
        self.name = name
        self.conf = os.path.join(WORK, name + ".conf")
        self.log = os.path.join(WORK, name + ".log")
        self.asan = os.path.join(WORK, name + ".asan")
        LOGS.append(self.log)
        with open(self.conf, "w") as f:
            f.write("[rtsp]\nport = %d\n%s\n[log]\nlevel = debug\n" % (PORT, rtsp_extra))
        env = dict(os.environ,
                   ASAN_OPTIONS="abort_on_error=0:detect_leaks=1:log_path=" + self.asan,
                   TSAN_OPTIONS="log_path=" + self.asan,
                   UBSAN_OPTIONS="print_stacktrace=1")
        self.rsd = subprocess.Popen([os.path.join(OUT, "rsd"), "-c", self.conf, "-f", "-d"],
                                    stdout=open(self.log, "w"), stderr=subprocess.STDOUT, env=env)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.1)
        time.sleep(0.5)

    def alive(self):
        return self.rsd.poll() is None

    def log_text(self):
        with open(self.log, errors="replace") as f:
            return f.read()

    def finish(self):
        """Stop rsd; the sanitizers and a clean exit are checks of every leg."""
        alive = self.alive()
        clean = True
        if alive:
            self.rsd.send_signal(signal.SIGTERM)
            try:
                self.rsd.wait(10)
            except subprocess.TimeoutExpired:
                clean = False
                self.rsd.kill()
                self.rsd.wait()
        reports = [f for f in os.listdir(WORK) if f.startswith(self.name + ".asan")]
        check(self.name + ": rsd alive at the end", alive)
        check(self.name + ": rsd exits on SIGTERM", clean)
        check(self.name + ": no sanitizer report", not reports,
              ", ".join(reports))


class Rtsp:
    def __init__(self, rcvbuf=None, timeout=3):
        self.s = socket.socket()
        if rcvbuf:
            self.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.s.settimeout(timeout)
        self.s.connect(("127.0.0.1", PORT))
        self.cseq = 0
        self.buf = b""

    def close(self):
        self.s.close()

    def send(self, method, uri, extra=""):
        self.cseq += 1
        if not uri.startswith("rtsp://") and uri != "*":
            uri = "rtsp://127.0.0.1:%d%s" % (PORT, uri)
        self.s.sendall(("%s %s RTSP/1.0\r\nCSeq: %d\r\n%s\r\n" %
                        (method, uri, self.cseq, extra)).encode())

    def reply(self, timeout=3):
        """Next RTSP response, skipping interleaved frames. None on timeout or close."""
        end = time.time() + timeout
        while time.time() < end:
            while len(self.buf) >= 4 and self.buf[:1] == b"$":
                n = 4 + int.from_bytes(self.buf[2:4], "big")
                if len(self.buf) < n:
                    break
                self.buf = self.buf[n:]
            if self.buf[:5] == b"RTSP/" and b"\r\n\r\n" in self.buf:
                head, _, rest = self.buf.partition(b"\r\n\r\n")
                m = re.search(rb"Content-Length: (\d+)", head)
                need = int(m.group(1)) if m else 0
                if len(rest) >= need:
                    self.buf = rest[need:]
                    return head.decode(errors="replace") + "\r\n\r\n" + rest[:need].decode(errors="replace")
            try:
                self.s.settimeout(max(0.05, end - time.time()))
                d = self.s.recv(65536)
            except (socket.timeout, OSError):
                return None
            if not d:
                return None
            self.buf += d
        return None

    def req(self, method, uri, extra="", timeout=3):
        self.send(method, uri, extra)
        return self.reply(timeout)

    def media_frames(self, seconds):
        """RTP packets seen on interleaved channel 0 within the time (stops at the first)."""
        end = time.time() + seconds
        n = 0
        while time.time() < end and not n:
            while len(self.buf) >= 4 and self.buf[:1] == b"$":
                ln = 4 + int.from_bytes(self.buf[2:4], "big")
                if len(self.buf) < ln:
                    break
                if self.buf[1] == 0:
                    n += 1
                self.buf = self.buf[ln:]
            if n:
                break
            try:
                self.s.settimeout(max(0.05, end - time.time()))
                d = self.s.recv(65536)
            except (socket.timeout, OSError):
                break
            if not d:
                break
            self.buf += d
        return n

    def closed_within(self, seconds):
        """True once the server has ended the connection (EOF or reset)."""
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([self.s], [], [], 0.2)
            if r:
                try:
                    if not self.s.recv(65536):
                        return True
                except OSError:
                    return True
        return False


def status(r):
    return r.split("\r\n")[0] if r else "NO REPLY"


def ok(r):
    return bool(r) and r.startswith("RTSP/1.0 200")


def session_id(r):
    m = re.search(r"Session: *([0-9]+)", r or "")
    return m.group(1) if m else None


def play_tcp(c, path="/stream0", rcvbuf_ok=True):
    """DESCRIBE, SETUP (TCP interleaved) and PLAY a video-only session."""
    c.req("DESCRIBE", path, "Accept: application/sdp\r\n")
    r = c.req("SETUP", path + "/video", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
    sid = session_id(r)
    return r, c.req("PLAY", path, "Session: %s\r\n" % sid)


def leg_stalled_player():
    print("== stalled TCP player")
    lab = Lab("stall", "tcp_sndbuf = 4096\nsend_timeout = 5\n")
    a = Rtsp(rcvbuf=2048)
    b = Rtsp()
    _, r = play_tcp(a)
    check("stall: player starts", ok(r), status(r))
    check("stall: bystander answered before the stall", ok(b.req("OPTIONS", "*")))
    time.sleep(3)  # A reads nothing; its send thread parks in write()
    a.send("OPTIONS", "*")  # the control plane now has a request from the stalled client
    time.sleep(0.5)
    t0 = time.time()
    r = b.req("OPTIONS", "*", timeout=1.5)
    check("stall: existing client answered while a player is stalled", ok(r), status(r))
    try:
        n = Rtsp(timeout=2)
        r = n.req("OPTIONS", "*", timeout=1.5)
        n.close()
    except OSError as e:
        r = None
    check("stall: new client answered while a player is stalled", ok(r), status(r))
    time.sleep(6)  # past send_timeout; reading earlier would count as progress
    check("stall: the stalled player is dropped", a.closed_within(10))
    check("stall: the drop is logged", "not reading" in lab.log_text())
    check("stall: bystander still answered after the drop", ok(b.req("OPTIONS", "*")))
    lab.finish()


def leg_alias_gate():
    print("== endpoint alias gate")
    lab = Lab("alias", "endpoint_main = camA\n")
    c = Rtsp()
    check("alias: DESCRIBE of the default path is refused",
          status(c.req("DESCRIBE", "/stream0", "Accept: application/sdp\r\n")).startswith("RTSP/1.0 404"))
    c.close()

    c = Rtsp()
    r = c.req("SETUP", "/stream0/video", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
    check("alias: SETUP without DESCRIBE cannot use the default path",
          status(r).startswith("RTSP/1.0 404"), status(r))
    r = c.req("SETUP", "/stream0/trackID=0", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
    check("alias: SETUP of a trackID control cannot use the default path either",
          status(r).startswith("RTSP/1.0 404"), status(r))
    r = c.req("PLAY", "/stream0")
    check("alias: PLAY without a session starts nothing", not ok(r), status(r))
    check("alias: no media reaches the bypassing client", not c.media_frames(1.5))
    c.close()

    c = Rtsp()
    r = c.req("DESCRIBE", "/camA", "Accept: application/sdp\r\n")
    check("alias: DESCRIBE of the alias works", ok(r), status(r))
    r = c.req("SETUP", "/camA/video", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
    check("alias: SETUP of the alias works", ok(r), status(r))
    sid = session_id(r)
    r = c.req("SETUP", "/stream0/video", "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\nSession: %s\r\n" % sid)
    check("alias: a later SETUP naming the default path is refused", status(r).startswith("RTSP/1.0 404"), status(r))
    r = c.req("PLAY", "/stream0", "Session: %s\r\n" % sid)
    check("alias: PLAY naming the default path is refused", status(r).startswith("RTSP/1.0 404"), status(r))
    r = c.req("PLAY", "/camA", "Session: %s\r\n" % sid)
    check("alias: PLAY of the alias works", ok(r), status(r))
    check("alias: media flows on the alias", c.media_frames(3))
    c.close()
    lab.finish()


def leg_uri_resolution():
    print("== stream resolution of request URIs")
    lab = Lab("resolve")
    c = Rtsp()
    r = c.req("DESCRIBE", "/stream1/", "Accept: application/sdp\r\n")
    check("resolve: a Content-Base style trailing slash names its own stream",
          ok(r) and "b=AS:1000" in r, status(r))
    c.close()

    c = Rtsp()
    r = c.req("SETUP", "/nothing/here/video", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
    check("resolve: SETUP of an unknown path is refused", status(r).startswith("RTSP/1.0 404"), status(r))
    r = c.req("SETUP", "/stream0/video", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
    check("resolve: SETUP without DESCRIBE works on a default path", ok(r), status(r))
    c.close()
    c = Rtsp()
    r = c.req("SETUP", "/stream0/trackID=0", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
    check("resolve: a trackID control resolves to its stream", ok(r), status(r))
    r = c.req("SETUP", "/stream0/trackID=1", "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\nSession: %s\r\n" % session_id(r))
    check("resolve: and so does a second track of the same stream", ok(r), status(r))
    c.close()
    c = Rtsp()
    r = c.req("SETUP", "/stream0/video", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
    sid = session_id(r)
    r = c.req("SETUP", "/stream1/video", "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\nSession: %s\r\n" % sid)
    check("resolve: a later SETUP naming another stream is refused", status(r).startswith("RTSP/1.0 404"), status(r))
    r = c.req("PLAY", "/stream1", "Session: %s\r\n" % sid)
    check("resolve: PLAY naming another stream is refused", status(r).startswith("RTSP/1.0 404"), status(r))
    r = c.req("PLAY", "/stream0/", "Session: %s\r\n" % sid)
    check("resolve: PLAY on the trailing-slash form works", ok(r), status(r))
    check("resolve: media flows", c.media_frames(3))
    c.close()
    lab.finish()


def leg_userinfo():
    print("== credentials in the request URI")
    lab = Lab("userinfo")
    c = Rtsp()
    base = "rtsp://admin:pa55@127.0.0.1:%d/stream0" % PORT
    c.req("DESCRIBE", base, "Accept: application/sdp\r\n")
    r = c.req("SETUP", base + "/video", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
    r = c.req("PLAY", base, "Session: %s\r\n" % session_id(r))
    m = re.search(r"RTP-Info: *(.*)", r or "")
    info = m.group(1).strip() if m else ""
    check("userinfo: PLAY answered", ok(r), status(r))
    check("userinfo: RTP-Info url carries no credentials",
          bool(info) and "@" not in info and "pa55" not in info, info)
    check("userinfo: RTP-Info url is the plain URL",
          info.startswith("url=rtsp://127.0.0.1:%d/stream0/video;" % PORT), info)
    c.close()
    lab.finish()


def main():
    os.makedirs(WORK, exist_ok=True)
    rings = subprocess.Popen([os.path.join(OUT, "create_rings")],
                             stdout=open(os.path.join(WORK, "rings.log"), "w"),
                             stderr=subprocess.STDOUT)
    time.sleep(1.5)
    legs = [leg_stalled_player, leg_alias_gate, leg_uri_resolution, leg_userinfo]
    only = sys.argv[1:]
    try:
        for leg in legs:
            if not only or leg.__name__ in only:
                leg()
    finally:
        rings.kill()
        rings.wait()
    print("\n%d checks passed, %d failed" % (PASS, len(FAILED)))
    for f in FAILED:
        print("  FAILED: " + f)
    if FAILED:
        for log in LOGS:
            print("--- tail of " + os.path.basename(log))
            with open(log, errors="replace") as f:
                print("".join(f.readlines()[-30:]))
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
