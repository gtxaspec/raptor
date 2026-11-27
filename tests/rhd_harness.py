#!/usr/bin/env python3
"""Client-handling harness for rhd, against the real daemon.

Runs inside the private user/mount/net namespace set up by test-rhd.sh.
Legs, in the order they run:
  https: a handshake that never finishes, a TLS client that stops reading
         a stream, one that stops reading a snapshot, and one that sends
         data while frames are being written to it
"""
import json
import os
import socket
import ssl
import subprocess
import sys
import threading
import time

OUT = os.environ.get("RHD_OUT", "asan-out")
WORK = os.environ.get("RHD_TEST_WORK", "/tmp/rhd-test")
RHD_BIN = os.environ.get("RHD_BIN", os.path.join(OUT, "rhd"))
PORT = 18080

PASS = 0
FAILED = []
ENV = dict(os.environ, ASAN_OPTIONS="abort_on_error=0:detect_leaks=1:halt_on_error=1",
           UBSAN_OPTIONS="print_stacktrace=1", TSAN_OPTIONS="halt_on_error=0")
SAN_MARKERS = ("ERROR: AddressSanitizer", "WARNING: ThreadSanitizer", "ERROR: LeakSanitizer",
               "runtime error:")


def check(name, ok, detail=""):
    global PASS
    if ok:
        PASS += 1
        print("  ok   " + name, flush=True)
    else:
        FAILED.append(name)
        print("  FAIL " + name + (" -- " + detail if detail else ""), flush=True)


def skip(name, why):
    print("  SKIP " + name + " -- " + why, flush=True)


def make_cert():
    cert = os.path.join(WORK, "rhd.pem")
    key = os.path.join(WORK, "rhd.key")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
                    "ec_paramgen_curve:prime256v1", "-nodes", "-days", "2", "-subj", "/CN=rhd",
                    "-keyout", key, "-out", cert], check=True, timeout=30,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return cert, key


class Rhd:
    def __init__(self, name, http_extra="", jpeg_kb=0):
        self.log = os.path.join(WORK, name + ".rhd.log")
        conf = os.path.join(WORK, name + ".conf")
        with open(conf, "w") as f:
            f.write("[log]\nlevel = debug\n\n[http]\nport = %d\nexif_timestamp = false\n"
                    "sign_snapshots = false\n%s\n" % (PORT, http_extra))
        rings = [os.path.join(OUT, "create_rings"), "--skip-video"]
        if jpeg_kb:
            rings += ["--jpeg-kb", str(jpeg_kb)]
        self.rings = subprocess.Popen(rings, stdout=subprocess.DEVNULL,
                                      stderr=subprocess.STDOUT, env=ENV)
        time.sleep(1.0)
        self.proc = subprocess.Popen([RHD_BIN, "-c", conf, "-f", "-d"],
                                     stdout=open(self.log, "w"), stderr=subprocess.STDOUT,
                                     env=ENV)
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=0.5).close()
                break
            except OSError:
                time.sleep(0.2)
        else:
            self.stop()
            raise SystemExit("rhd did not start:\n" + self.logtext())
        time.sleep(0.5)

    def logtext(self):
        with open(self.log, errors="replace") as f:
            return f.read()

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.rings.terminate()
        self.rings.wait()
        return self.proc.returncode

    def sanitizer_report(self):
        text = self.logtext()
        for m in SAN_MARKERS:
            i = text.find(m)
            if i >= 0:
                return text[max(0, i - 200):i + 1800]
        return None


def ctl(*args, timeout=5):
    """raptorctl rhd <args>; returns (parsed json or None, seconds taken)."""
    t0 = time.time()
    try:
        r = subprocess.run([os.path.join(OUT, "raptorctl"), "rhd", *args], capture_output=True,
                           timeout=timeout, env=ENV)
        out = r.stdout.decode(errors="replace")
    except subprocess.TimeoutExpired:
        return None, time.time() - t0
    try:
        return json.loads(out[out.index("{"):]), time.time() - t0
    except ValueError:
        return None, time.time() - t0


def client_count():
    r, _ = ctl("clients")
    return None if r is None else r.get("count")


def raw_conn(rcvbuf=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if rcvbuf:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    s.settimeout(5)
    s.connect(("127.0.0.1", PORT))
    return s


def tls_conn(rcvbuf=None):
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx.wrap_socket(raw_conn(rcvbuf), server_hostname="rhd")


def tls_get(path, timeout=5):
    """One whole GET /snap over TLS; returns the status line, or the error."""
    try:
        s = tls_conn()
        s.settimeout(timeout)
        s.sendall(("GET %s HTTP/1.0\r\n\r\n" % path).encode())
        d = b""
        while b"\r\n" not in d:
            c = s.recv(4096)
            if not c:
                break
            d += c
        s.close()
        return d.split(b"\r\n")[0].decode(errors="replace")
    except (OSError, ssl.SSLError) as e:
        return "error: %s" % e


def count_frames(stamps, stop, s):
    try:
        while not stop.is_set():
            d = s.recv(65536)
            if not d:
                break
            stamps.append((time.time(), d.count(b"--raptorframe")))
    except (OSError, ssl.SSLError):
        pass


def frames_between(stamps, t0, t1):
    return sum(n for t, n in list(stamps) if t0 <= t <= t1)


def wait_for(fn, timeout, step=0.25):
    end = time.time() + timeout
    while time.time() < end:
        if fn():
            return True
        time.sleep(step)
    return fn()


def https_legs():
    print("-- https", flush=True)
    cert, key = make_cert()
    d = Rhd("https", "https = true\ncert = %s\nkey = %s\n" % (cert, key), jpeg_kb=100)

    # A ClientHello that never completes must not stop anyone else
    victim = tls_conn()
    victim.settimeout(5)
    victim.sendall(b"GET /mjpeg HTTP/1.0\r\n\r\n")
    stamps, stop = [], threading.Event()
    th = threading.Thread(target=count_frames, args=(stamps, stop, victim), daemon=True)
    th.start()
    time.sleep(2)
    hello = b"\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03"
    attackers = [raw_conn() for _ in range(3)]
    t0 = time.time()
    for i, b in enumerate(hello):
        for a in attackers:
            try:
                a.sendall(bytes([b]))
            except OSError:
                pass
        if i == 3:
            _, took = ctl("status")
            check("status answers while 3 handshakes dribble (%.1fs)" % took, took < 2.0)
            line = tls_get("/snap", timeout=4)
            check("a real TLS client is served while 3 handshakes dribble (%s)" % line,
                  line.startswith("HTTP/1.1 200"))
        time.sleep(0.8)
    t1 = time.time()
    got = frames_between(stamps, t0 + 0.5, t1)
    check("MJPEG stream keeps flowing while handshakes dribble (%d frames in %.1fs)"
          % (got, t1 - t0), got >= 3)
    for a in attackers:
        a.close()
    stop.set()
    victim.close()
    th.join(5)

    # A stream client that stops reading is dropped, not waited for
    slow = tls_conn(rcvbuf=2048)
    slow.sendall(b"GET /mjpeg HTTP/1.0\r\n\r\n")
    check("stalled TLS stream client is dropped", wait_for(lambda: client_count() == 0, 25))
    slow.close()

    # So is one that stops reading a snapshot, and the loop runs meanwhile
    try:
        with open("/proc/sys/net/ipv4/tcp_wmem", "w") as f:
            f.write("4096 4096 16384")
        wmem = True
    except OSError:
        wmem = False
    if wmem:
        snap = tls_conn(rcvbuf=2048)
        snap.sendall(b"GET /snap HTTP/1.0\r\n\r\n")
        time.sleep(0.7)
        _, took = ctl("status")
        check("status answers while a snapshot client does not read (%.1fs)" % took, took < 1.0)
        check("snapshot client that does not read is dropped",
              wait_for(lambda: client_count() == 0, 10))
        snap.close()
    else:
        skip("snapshot client that does not read", "tcp_wmem not writable")

    # Data from a streaming client while its frames are written
    talker = tls_conn()
    talker.settimeout(5)
    talker.sendall(b"GET /mjpeg HTTP/1.0\r\n\r\n")
    stamps, stop = [], threading.Event()
    th = threading.Thread(target=count_frames, args=(stamps, stop, talker), daemon=True)
    th.start()
    t0 = time.time()
    try:
        while time.time() - t0 < 4:
            talker.sendall(b"x" * 64)
            time.sleep(0.02)
    except (OSError, ssl.SSLError):
        pass
    stop.set()
    talker.close()
    th.join(5)
    time.sleep(0.5)

    rc = d.stop()
    rep = d.sanitizer_report()
    check("no sanitizer report from the https daemon", rep is None, rep or "")
    check("https daemon exits cleanly (rc=%s)" % rc, rc == 0)


def main():
    os.makedirs(WORK, exist_ok=True)
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    if which in ("all", "https"):
        https_legs()
    print("%d passed, %d failed" % (PASS, len(FAILED)))
    for n in FAILED:
        print("  FAILED: " + n)
    sys.exit(1 if FAILED else 0)


if __name__ == "__main__":
    main()
