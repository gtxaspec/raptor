#!/usr/bin/env python3
"""Security behavior harness for rwd: DTLS pinning, session slots, CORS and
WebTorrent offer authentication, against the real daemon.

Runs inside the private user/mount/net namespace set up by test-rwd.sh, so
every port, /dev/shm and /run is ours. A fake WebSocket tracker (TLS) stands
in for the WebTorrent tracker; `openssl s_client` plays the DTLS peer behind
a UDP relay that owns the ICE source address.
"""
import hashlib
import hmac
import http.client as httpc
import json
import os
import re
import select
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time

OUT = os.environ.get("RWD_OUT", "asan-out")
WORK = os.environ.get("RWD_TEST_WORK", "/tmp/rwd-test")
UDP_PORT = 8443
HTTP_PORT = 8554
TRACKER_PORT = 9443

PASS = 0
FAILED = []


def check(name, ok, detail=""):
    global PASS
    if ok:
        PASS += 1
        print("  ok   " + name)
    else:
        FAILED.append(name)
        print("  FAIL " + name + (" -- " + detail if detail else ""))


def sh(*cmd):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def make_cert(name):
    cert = os.path.join(WORK, name + ".pem")
    key = os.path.join(WORK, name + ".key")
    sh("openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
       "-nodes", "-days", "2", "-subj", "/CN=" + name, "-keyout", key, "-out", cert)
    out = subprocess.run(["openssl", "x509", "-in", cert, "-noout", "-fingerprint", "-sha256"],
                         check=True, capture_output=True, text=True).stdout
    fp = "sha-256 " + out.split("=", 1)[1].strip()
    return cert, key, fp


ZERO_FP = "sha-256 " + ":".join(["00"] * 32)

OFFER = (
    "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0 1\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96\r\nc=IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:{ufrag}\r\na=ice-pwd:attackerpwdattackerpwd00\r\n"
    "a=fingerprint:{fp}\r\na=setup:actpass\r\na=mid:0\r\na=recvonly\r\na=rtcp-mux\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111 0\r\nc=IN IP4 0.0.0.0\r\na=mid:1\r\na=sendrecv\r\n"
    "a=rtcp-mux\r\na=rtpmap:111 opus/48000/2\r\n{cands}")


def offer(ufrag="atk1", fp=ZERO_FP, cands=""):
    return OFFER.format(ufrag=ufrag, fp=fp, cands=cands)


class Daemon:
    def __init__(self, name, maxc=2, auth="", extra=""):
        self.log = os.path.join(WORK, name + ".rwd.log")
        conf = os.path.join(WORK, name + ".conf")
        cert, key, _ = make_cert("rwd-" + name)
        with open(conf, "w") as f:
            f.write("[webrtc]\nenabled = true\nudp_port = %d\nhttp_port = %d\n"
                    "max_clients = %d\ncert = %s\nkey = %s\nlocal_ip = 127.0.0.1\n"
                    "https = false\n%s\n\n%s\n\n[log]\nlevel = debug\n"
                    % (UDP_PORT, HTTP_PORT, maxc, cert, key, auth, extra))
        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "abort_on_error=0:detect_leaks=1:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "print_stacktrace=1"
        self.rings = subprocess.Popen([os.path.join(OUT, "create_rings")],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
                                      env=env)
        time.sleep(1.0)
        self.proc = subprocess.Popen([os.path.join(OUT, "rwd"), "-c", conf, "-f", "-d"],
                                     stdout=open(self.log, "w"), stderr=subprocess.STDOUT,
                                     env=env)
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                socket.create_connection(("127.0.0.1", HTTP_PORT), timeout=0.5).close()
                break
            except OSError:
                time.sleep(0.2)
        else:
            self.stop()
            raise SystemExit("rwd did not start:\n" + self.logtext())
        time.sleep(0.5)

    def alive(self):
        return self.proc.poll() is None

    def logtext(self):
        with open(self.log, errors="replace") as f:
            return f.read()

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.rings.terminate()
        self.rings.wait()
        return self.proc.returncode

    def sanitizer_report(self):
        i = self.logtext().find("ERROR: AddressSanitizer")
        return None if i < 0 else self.logtext()[i:i + 1500]


def http_request(method, path, body=None, headers=None):
    c = httpc.HTTPConnection("127.0.0.1", HTTP_PORT, timeout=10)
    h = {"Content-Type": "application/sdp"}
    h.update(headers or {})
    c.request(method, path, body=body, headers=h)
    r = c.getresponse()
    data = r.read().decode(errors="replace")
    hdrs = {k.lower(): v for k, v in r.getheaders()}
    c.close()
    return r.status, hdrs, data



def whip(sdp, headers=None):
    st, hdrs, body = http_request("POST", "/whip", sdp, headers)
    if st != 201:
        return st, None
    return st, {
        "ufrag": re.search(r"a=ice-ufrag:(\S+)", body).group(1),
        "pwd": re.search(r"a=ice-pwd:(\S+)", body).group(1),
        "id": hdrs["location"].rsplit("/", 1)[1],
    }


def stun_request(local_ufrag, local_pwd, remote_ufrag="atk1"):
    txid = os.urandom(12)
    user = (local_ufrag + ":" + remote_ufrag).encode()
    attrs = struct.pack("!HH", 0x0006, len(user)) + user + b"\0" * ((4 - len(user) % 4) % 4)
    hdr = struct.pack("!HHI", 0x0001, len(attrs) + 24, 0x2112A442) + txid
    mi = hmac.new(local_pwd.encode(), hdr + attrs, hashlib.sha1).digest()
    attrs += struct.pack("!HH", 0x0008, 20) + mi
    return struct.pack("!HHI", 0x0001, len(attrs), 0x2112A442) + txid + attrs, txid


def stun_ping(sock, info, timeout=1.0):
    req, txid = stun_request(info["ufrag"], info["pwd"])
    sock.sendto(req, ("127.0.0.1", UDP_PORT))
    sock.settimeout(timeout)
    end = time.time() + timeout
    while time.time() < end:
        try:
            data, _ = sock.recvfrom(4096)
        except socket.timeout:
            return False
        if data[:2] == b"\x01\x01" and data[8:20] == txid:
            return True
    return False


def dtls_peer(info, cert=None, key=None, seconds=6, port=40001, until_rtp=False):
    """Run an openssl DTLS client behind a relay that owns the ICE address.
    Returns the number of RTP/SRTCP packets the camera sent afterwards."""
    cam = ("127.0.0.1", UDP_PORT)
    up = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    up.bind(("127.0.0.1", port))
    down = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    down.bind(("127.0.0.1", port + 1))
    if not stun_ping(up, info):
        up.close()
        down.close()
        return -1
    cmd = ["openssl", "s_client", "-dtls1_2", "-connect", "127.0.0.1:%d" % (port + 1),
           "-use_srtp", "SRTP_AES128_CM_SHA1_80", "-quiet"]
    if cert:
        cmd += ["-cert", cert, "-key", key]
    ossl = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    peer = None
    rtp = 0
    end = time.time() + seconds
    last_stun = time.time()
    while time.time() < end:
        r, _, _ = select.select([up, down], [], [], 0.2)
        for s in r:
            data, src = s.recvfrom(4096)
            if s is down:
                peer = src
                up.sendto(data, cam)
            elif 20 <= data[0] <= 25:
                if peer:
                    down.sendto(data, peer)
            elif 128 <= data[0] <= 191:
                rtp += 1
        if until_rtp and rtp:
            break
        if time.time() - last_stun > 2:
            up.sendto(stun_request(info["ufrag"], info["pwd"])[0], cam)
            last_stun = time.time()
    ossl.kill()
    ossl.wait()
    up.close()
    down.close()
    return rtp


# ── fake WebSocket tracker ──

def ws_frame(payload):
    n = len(payload)
    if n < 126:
        return bytes([0x81, n]) + payload
    return bytes([0x81, 126]) + struct.pack("!H", n) + payload


def ws_read(conn):
    def exact(n):
        d = b""
        while len(d) < n:
            c = conn.recv(n - len(d))
            if not c:
                raise OSError("closed")
            d += c
        return d
    hdr = exact(2)
    n = hdr[1] & 0x7F
    if n == 126:
        n = struct.unpack("!H", exact(2))[0]
    if hdr[1] & 0x80:
        mask = exact(4)
        data = bytearray(exact(n))
        for i in range(n):
            data[i] ^= mask[i % 4]
        return bytes(data)
    return exact(n)


class Tracker:
    """Accepts one camera at a time; `send(msg)` queues a frame for it and
    answers received from the camera land in `answers`."""

    def __init__(self, cert, key):
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(cert, key)
        self.ls = socket.socket()
        self.ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.ls.bind(("127.0.0.1", TRACKER_PORT))
        self.ls.listen(4)
        self.ls.settimeout(0.5)
        self.stop_ev = threading.Event()
        self.conn = None
        self.connected = threading.Event()
        self.accepted = 0
        self.announce = None
        self.answers = []
        self.lock = threading.Lock()
        self.t = threading.Thread(target=self.run, daemon=True)
        self.t.start()

    def run(self):
        while not self.stop_ev.is_set():
            try:
                raw, _ = self.ls.accept()
            except socket.timeout:
                continue
            self.accepted += 1
            try:
                conn = self.ctx.wrap_socket(raw, server_side=True)
                req = b""
                while b"\r\n\r\n" not in req:
                    req += conn.recv(1024)
                conn.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n")
                self.announce = json.loads(ws_read(conn))
                self.conn = conn
                self.connected.set()
                conn.settimeout(0.5)
                while not self.stop_ev.is_set():
                    try:
                        msg = ws_read(conn)
                    except socket.timeout:
                        continue
                    with self.lock:
                        self.answers.append(json.loads(msg))
            except (OSError, ssl.SSLError, ValueError):
                pass
            self.connected.clear()

    def send(self, obj):
        with self.lock:
            self.conn.sendall(ws_frame(json.dumps(obj).encode()))

    def wait_answers(self, n, timeout):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                if len(self.answers) >= n:
                    return True
            time.sleep(0.1)
        return False

    def close(self):
        self.stop_ev.set()
        self.t.join(5)
        self.ls.close()


def auth_tag(key, direction, offer_id, sdp):
    msg = (direction + "\n" + offer_id + "\n" + sdp).encode()
    return hmac.new(key.encode(), msg, hashlib.sha256).hexdigest()


def wt_offer(key, offer_id, sdp, auth="good"):
    o = {"type": "offer", "sdp": sdp}
    if auth == "good":
        o["auth"] = auth_tag(key, "offer", offer_id, sdp)
    elif auth == "wrong":
        o["auth"] = auth_tag("not-the-key", "offer", offer_id, sdp)
    return {"action": "announce", "peer_id": "P" * 20, "offer_id": offer_id, "offer": o}


def wt_extra(tls_verify="false", key="viewerkey-123"):
    return ("[webtorrent]\nenabled = true\ntracker = wss://127.0.0.1:%d\n"
            "stun_server = 127.0.0.1\nstun_port = 3479\ntls_verify = %s\nshare_key = %s\n"
            % (TRACKER_PORT, tls_verify, key))


def share_key(d, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        m = re.search(r"share URL: \S+#key=(\S+)", d.logtext())
        if m:
            return m.group(1)
        time.sleep(0.2)
    return None


# ── scenarios ──

def scenario_dtls():
    print("DTLS peer certificate is pinned to the offer fingerprint")
    good_cert, good_key, good_fp = make_cert("peer-good")
    other_cert, other_key, _ = make_cert("peer-other")

    d = Daemon("dtls-nocert")
    st, info = whip(offer(fp=good_fp))
    rtp = dtls_peer(info)
    log = d.logtext()
    check("no certificate: no SRTP stack", "SRTP stack ready" not in log)
    check("no certificate: refusal logged", "peer presented no certificate" in log)
    check("no certificate: no media", rtp == 0, "rtp=%s" % rtp)
    d.stop()

    d = Daemon("dtls-wrong")
    st, info = whip(offer(fp=good_fp))
    rtp = dtls_peer(info, other_cert, other_key)
    log = d.logtext()
    check("wrong certificate: no SRTP stack", "SRTP stack ready" not in log)
    check("wrong certificate: mismatch logged", "peer fingerprint mismatch" in log)
    check("wrong certificate: no media", rtp == 0, "rtp=%s" % rtp)
    d.stop()

    d = Daemon("dtls-right")
    st, info = whip(offer(fp=good_fp))
    rtp = dtls_peer(info, good_cert, good_key, seconds=12, until_rtp=True)
    log = d.logtext()
    check("matching certificate: fingerprint verified", "peer fingerprint verified" in log)
    check("matching certificate: SRTP stack up", "SRTP stack ready" in log)
    check("matching certificate: media flows", rtp > 0, "rtp=%s" % rtp)
    d.stop()


def scenario_wt_auth(cert, key):
    print("WebTorrent offers must carry the share key")
    tr = Tracker(cert, key)
    d = Daemon("wt-auth", extra=wt_extra())
    sk = share_key(d)
    check("share key announced", sk is not None)
    check("camera reached the tracker", tr.connected.wait(10))

    sdp = offer(ufrag="wtpeer")
    tr.send(wt_offer(sk, "s0_noauth", sdp, auth="none"))
    check("offer without auth gets no answer", not tr.wait_answers(1, 3))
    check("missing auth logged", "not authenticated with the share key" in d.logtext())
    tr.send(wt_offer(sk, "s0_wrong", sdp, auth="wrong"))
    check("offer with a wrong key gets no answer", not tr.wait_answers(1, 3))

    good = wt_offer(sk, "s0_good", sdp)
    tr.send(good)
    check("authenticated offer is answered", tr.wait_answers(1, 5))
    if tr.answers:
        ans = tr.answers[0]["answer"]
        check("answer proves the camera knows the key",
              ans.get("auth") == auth_tag(sk, "answer", "s0_good", ans["sdp"]))
    tr.send(good)
    check("replayed offer is not answered twice", not tr.wait_answers(2, 3))
    check("replay logged", "offer_id already used" in d.logtext())
    check("daemon healthy", d.alive() and d.sanitizer_report() is None, str(d.sanitizer_report()))
    d.stop()
    tr.close()


def scenario_wt_tls(cert, key):
    print("WebTorrent tracker TLS fails closed")
    tr = Tracker(cert, key)
    sh("mount", "-t", "tmpfs", "none", "/etc/ssl/certs")
    try:
        d = Daemon("wt-nocabundle", extra=wt_extra(tls_verify="true"))
        end = time.time() + 20
        while time.time() < end and "refusing unverified" not in d.logtext():
            time.sleep(0.3)
        log = d.logtext()
        check("missing CA bundle refuses the connection", "refusing unverified tracker TLS" in log)
        check("the tracker never got a TLS session or announce", tr.announce is None)
        d.stop()
    finally:
        sh("umount", "/etc/ssl/certs")
    tr.close()


def scenario_wt_key():
    print("WebTorrent share key length")
    d = Daemon("wt-shortkey", extra=wt_extra(key="abc1234"))
    time.sleep(2)
    log = d.logtext()
    check("7-character key rejected", "share_key must be at least 8 characters" in log)
    d.stop()


def scenario_wt_race(cert, key):
    print("WebTorrent offers racing WHIP posts")
    tr = Tracker(cert, key)
    d = Daemon("wt-race", maxc=1, extra=wt_extra())
    sk = share_key(d)
    tr.connected.wait(10)
    cands = "".join("a=candidate:%d 1 UDP 2122260223 127.0.0.1 %d typ host\r\n" % (i, 41000 + i)
                    for i in range(16))
    stop = threading.Event()

    def poster():
        body = offer(ufrag="whip").encode()
        req = (b"POST /whip HTTP/1.1\r\nHost: x\r\nContent-Type: application/sdp\r\n"
               b"Content-Length: %d\r\n\r\n" % len(body)) + body
        while not stop.is_set():
            try:
                s = socket.create_connection(("127.0.0.1", HTTP_PORT), timeout=3)
                s.sendall(req)
                s.recv(8192)
                s.close()
            except OSError:
                time.sleep(0.05)

    ths = [threading.Thread(target=poster, daemon=True) for _ in range(2)]
    for t in ths:
        t.start()
    n = 0
    end = time.time() + 25
    while time.time() < end and d.alive():
        n += 1
        oid = "s0_race%d" % n
        try:
            tr.send(wt_offer(sk, oid, offer(ufrag="wt%d" % n, cands=cands)))
        except OSError:
            break
        time.sleep(0.02)
    stop.set()
    for t in ths:
        t.join(5)
    alive = d.alive()
    d.stop()
    rep = d.sanitizer_report()
    check("no use-after-free under concurrent offers", alive and rep is None,
          (rep or "rwd died")[:600])
    tr.close()


def main():
    os.makedirs(WORK, exist_ok=True)
    cert, key, _ = make_cert("tracker")
    only = os.environ.get("RWD_ONLY", "").split(",")
    for sc in (scenario_dtls, scenario_wt_key,):
        if only == [""] or sc.__name__[9:] in only:
            sc()
    for sc in (scenario_wt_auth, scenario_wt_tls, scenario_wt_race,):
        if only == [""] or sc.__name__[9:] in only:
            sc(cert, key)
    print("%d passed, %d failed" % (PASS, len(FAILED)))
    for f in FAILED:
        print("  failed: " + f)
    sys.exit(1 if FAILED else 0)


if __name__ == "__main__":
    main()
