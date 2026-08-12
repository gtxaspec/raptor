#!/usr/bin/env python3
"""RTP recovery invariants: both tracks must stay monotonic THROUGH
disturbances, not merely resume after them.

Connects over RTSP/TCP interleaved (small SO_RCVBUF on purpose), plays
video+audio, then validates while the harness disturbs the stream:

  phase 1  clean capture
  phase 2  stall: stop reading the socket so the server's send queue
           overflows (drop-video-keep-audio policy under test)
  phase 3  resume; the harness then restarts the ring producer (rvd)
  phase 4  post-restart capture

Asserts, per track, across ALL phases: RTP timestamps never move
backwards (mod-2^32), gaps are forward-only; and the first video slice
after each disturbance window starts a keyframe (SPS/PPS/IDR), never an
orphan P. This is the seam a field bug (PR #27) shipped through: the
fault-injecting legs asserted only coarse recovery while the
timestamp-analysis legs only ever saw undisturbed streams.
"""

import re
import socket
import struct
import sys
import time

URL = sys.argv[1]
STALL_AT = float(sys.argv[2])       # seconds into capture
STALL_SECS = float(sys.argv[3])
TOTAL_SECS = float(sys.argv[4])

m = re.match(r"rtsp://([^:/]+):(\d+)(/\S*)", URL)
HOST, PORT, PATH = m.group(1), int(m.group(2)), m.group(3)

cseq = 0


def req(sock, method, url, extra=""):
    global cseq
    cseq += 1
    msg = f"{method} {url} RTSP/1.0\r\nCSeq: {cseq}\r\n{extra}\r\n"
    sock.sendall(msg.encode())
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError(f"{method}: connection closed")
        data += chunk
    head, _, rest = data.partition(b"\r\n\r\n")
    head = head.decode(errors="replace")
    clen = 0
    for line in head.split("\r\n"):
        if line.lower().startswith("content-length:"):
            clen = int(line.split(":", 1)[1])
    while len(rest) < clen:
        rest += sock.recv(4096)
    body = rest[:clen]
    leftover = rest[clen:]
    return head, body, leftover


sock = socket.create_connection((HOST, PORT), timeout=10)
# A small receive buffer makes the stall bite quickly: the server's
# writes block as soon as we stop draining.
sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16384)

head, sdp, pending = req(sock, "DESCRIBE", URL, "Accept: application/sdp\r\n")
if "200 OK" not in head.split("\r\n")[0]:
    print(f"FAIL describe: {head.splitlines()[0]}")
    sys.exit(1)

controls = {}  # media type -> control url
media = None
for line in sdp.decode(errors="replace").splitlines():
    if line.startswith("m="):
        media = line.split("=")[1].split()[0]
    elif line.startswith("a=control:") and media:
        c = line.split(":", 1)[1].strip()
        if not c.startswith("rtsp://"):
            c = URL.rstrip("/") + "/" + c.lstrip("/")
        controls.setdefault(media, c)

if "video" not in controls or "audio" not in controls:
    print(f"FAIL sdp tracks: have {sorted(controls)} (need video+audio)")
    sys.exit(1)

session = ""
chan = {}
for i, mtype in enumerate(("video", "audio")):
    extra = f"Transport: RTP/AVP/TCP;unicast;interleaved={i * 2}-{i * 2 + 1}\r\n"
    if session:
        extra += f"Session: {session}\r\n"
    head, _, more = req(sock, "SETUP", controls[mtype], extra)
    pending += more
    if "200 OK" not in head.split("\r\n")[0]:
        print(f"FAIL setup {mtype}: {head.splitlines()[0]}")
        sys.exit(1)
    for line in head.split("\r\n"):
        if line.lower().startswith("session:"):
            session = line.split(":", 1)[1].split(";")[0].strip()
    chan[i * 2] = mtype

head, _, more = req(sock, "PLAY", URL, f"Session: {session}\r\nRange: npt=0-\r\n")
pending += more
if "200 OK" not in head.split("\r\n")[0]:
    print(f"FAIL play: {head.splitlines()[0]}")
    sys.exit(1)


def vcl_start_type(payload):
    """Return the NAL type if this packet begins a VCL slice (1-5), else
    None. Non-VCL NALs (SEI, SPS, PPS) legitimately precede the IDR and
    must not conclude the keyframe check. Handles single NAL, STAP-A
    (24) and FU-A (28, start bit only)."""
    if not payload:
        return None
    t = payload[0] & 0x1F
    if t == 28 and len(payload) >= 2:
        if payload[1] & 0x80:  # FU start
            ft = payload[1] & 0x1F
            return ft if 1 <= ft <= 5 else None
        return None
    if t == 24:  # STAP-A: first VCL among the aggregates
        off = 1
        while off + 2 < len(payload):
            sz = int.from_bytes(payload[off:off + 2], "big")
            off += 2
            if off >= len(payload):
                break
            at = payload[off] & 0x1F
            if 1 <= at <= 5:
                return at
            off += sz
        return None
    return t if 1 <= t <= 5 else None


buf = bytearray(pending)
start = time.monotonic()
stalled = resumed = False
last_ts = {}
backward = {m: 0 for m in ("video", "audio")}
counts = {m: 0 for m in ("video", "audio")}
events = []  # (time, label)
# Keyframe checks arm on DATA, not wall time: a client stall replays
# the kernel-buffered backlog first on resume, so packets read after
# the stall are pre-overflow frames the client legitimately owns. The
# recovery boundary is the video timestamp gap where the dropped
# frames were; every such discontinuity, whatever caused it, must
# resume on an IDR. 90kHz ticks: 30000 = 333ms >> one frame at 25fps.
GAP_TICKS = 30000
kf_pending = False
gap_count = 0
kf_fail = []
kf_ok = 0

sock.settimeout(0.5)
while True:
    now = time.monotonic() - start
    if now >= TOTAL_SECS:
        break
    if not stalled and now >= STALL_AT:
        stalled = True
        events.append((now, "stall"))
        time.sleep(STALL_SECS)
        events.append((time.monotonic() - start, "resume"))
        resumed = True
        continue
    try:
        chunk = sock.recv(65536)
        if not chunk:
            print("FAIL connection closed mid-capture")
            sys.exit(1)
        buf += chunk
    except socket.timeout:
        continue

    while True:
        if len(buf) < 4:
            break
        if buf[0] != 0x24:
            # RTSP keepalive/response bytes in-stream: resync to '$'
            nxt = buf.find(b"$")
            if nxt < 0:
                buf.clear()
                break
            del buf[:nxt]
            continue
        ch = buf[1]
        ln = struct.unpack(">H", buf[2:4])[0]
        if len(buf) < 4 + ln:
            break
        pkt = bytes(buf[4:4 + ln])
        del buf[:4 + ln]
        if ch not in chan or len(pkt) < 12:
            continue  # RTCP channels and runts
        mtype = chan[ch]
        ts = struct.unpack(">I", pkt[4:8])[0]
        counts[mtype] += 1
        if mtype in last_ts:
            delta = (ts - last_ts[mtype]) & 0xFFFFFFFF
            if delta != 0 and delta >= 0x80000000:
                backward[mtype] += 1
                print(f"BACKWARD {mtype} ts {last_ts[mtype]} -> {ts} "
                      f"at t={time.monotonic()-start:.2f}")
            elif mtype == "video" and delta > GAP_TICKS:
                gap_count += 1
                kf_pending = True
        last_ts[mtype] = ts
        if mtype == "video" and kf_pending:
            t = vcl_start_type(pkt[12:])
            if t is None:
                continue  # SEI/SPS/PPS before the IDR: keep waiting
            kf_pending = False
            if t == 5:
                kf_ok += 1
            else:
                kf_fail.append(t)

print(f"video={counts['video']} audio={counts['audio']} "
      f"backward_video={backward['video']} backward_audio={backward['audio']} "
      f"gaps={gap_count} idr_resumes={kf_ok} kf_fail={kf_fail} "
      f"events={[(round(t, 2), l) for t, l in events]}")

# gap_count >= 1 proves a disturbance actually landed in the stream:
# a stall the buffers absorbed would leave the leg vacuous.
ok = (counts["video"] > 50 and counts["audio"] > 20 and
      backward["video"] == 0 and backward["audio"] == 0 and
      gap_count >= 1 and kf_ok == gap_count and not kf_fail and resumed)
sys.exit(0 if ok else 1)
