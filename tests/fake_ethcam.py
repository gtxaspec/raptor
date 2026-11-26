#!/usr/bin/env python3
"""Fake Novatek EthCam rear module (the TX side), for testing rcd.

Speaks the module's protocol and copies the quirks the real one has:
  - commands on 8899: "<ip>:<port>/?custom=1&cmd=N&par=P\\0", XML replies
  - two-phase commands answer Status 2, then take the binary payload from
    ONE receive; a payload split across segments is rejected (-1)
  - data on 8887 (record) / 8888 (display) only after 9008 for that port
  - a data client that half-closes its socket is dropped
  - the first three frames after a stream start repeat the start timestamp
  - records: 16-byte header (u64 BE us timestamp, 20-bit LE size, GOP index,
    two byte-sum checksums, type 2); H.264 I frames carry length-prefixed
    SPS/PPS in front, H.265 I frames carry VPS/SPS/PPS inline

Faults, to prove rcd's loss counters fire: --skip N leaves out every Nth
frame (the timestamp still advances), --corrupt N breaks the checksum of
every Nth frame.

Usage: fake_ethcam.py --ip 127.0.0.12 --h264 clip.h264 [--h265 clip.hevc]
"""

import argparse
import socket
import struct
import sys
import threading
import time

CMD, REC, DISP = 8899, 8887, 8888
PAYLOAD = {9027: 12, 9032: 12, 9014: 44, 9015: 32}


def split_nals(data):
    out, i, n = [], 0, len(data)
    starts = []
    while True:
        j = data.find(b"\x00\x00\x01", i)
        if j < 0:
            break
        s = j - 1 if j > 0 and data[j - 1] == 0 else j
        starts.append((s, j + 3))
        i = j + 3
    for k, (s, p) in enumerate(starts):
        e = starts[k + 1][0] if k + 1 < len(starts) else n
        out.append(b"\x00\x00\x00\x01" + data[p:e].rstrip(b"\x00"))
    return out


def access_units(path, h265):
    """[(is_key, [nal...])] in decode order; parameter sets stay with the I frame"""
    aus, pending = [], []
    for nal in split_nals(open(path, "rb").read()):
        t = (nal[4] >> 1) & 0x3F if h265 else nal[4] & 0x1F
        vcl = t < 32 if h265 else 1 <= t <= 5
        key = t in (19, 20) if h265 else t == 5
        if not vcl:
            if (h265 and t in (32, 33, 34)) or (not h265 and t in (7, 8)):
                pending.append((t, nal))
            continue
        aus.append((key, pending + [(t, nal)]))
        pending = []
    return aus


def record(ts_us, gop_idx, nals, h265):
    pre = b""
    if not h265 and gop_idx == 1:
        sps = next(n for t, n in nals if t == 7)
        pps = next(n for t, n in nals if t == 8)
        pre = struct.pack("<H", len(sps)) + sps + struct.pack("<H", len(pps)) + pps
        body = b"".join(n for t, n in nals if t not in (7, 8))
    else:
        body = b"".join(n for t, n in nals)
    ca = sum(body[:64]) & 0xFF if len(body) > 64 else 0
    cb = sum(body[-64:]) & 0xFF if len(body) > 64 else 0
    ps = sum(len(n) for t, n in nals if t in (32, 33, 34)) & 0xFF if h265 and gop_idx == 1 else 0
    size = len(body)
    hdr = struct.pack(">Q", ts_us) + bytes(
        [size & 0xFF, (size >> 8) & 0xFF, (size >> 16) & 0x0F, gop_idx & 0xFF, ca, cb, ps, 2])
    return pre + hdr + body


class Module:
    def __init__(self, args):
        self.ip = args.ip
        self.fps = args.fps
        self.info_fps = args.info_fps or args.fps
        self.info_w, self.info_h = (int(v) for v in args.info_size.split("x"))
        self.skip = args.skip
        self.corrupt = args.corrupt
        self.delay = args.delay
        self.clips = {False: access_units(args.h264, False)}
        if args.h265:
            self.clips[True] = access_units(args.h265, True)
        self.h265 = False
        self.lock = threading.Lock()
        self.clients = {REC: None, DISP: None}
        self.running = {REC: False, DISP: False}
        self.force_i = {REC: False, DISP: False}
        self.t0 = time.monotonic() - 7.0  # the module booted a while ago
        self.stats = {"cmd": 0, "rejected": 0}

    def reply(self, port, body):
        return ("%s:%d<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n%s" % (self.ip, port, body)).encode()

    def fn(self, port, cmd, status):
        return self.reply(port, "<Function>\n<Cmd>%d</Cmd>\n<Status>%d</Status>\n</Function>" % (cmd, status))

    def lst(self, port, cmd, items):
        it = "".join("<ITEM>%d</ITEM>\n" % v for v in items)
        return self.reply(port, "<LIST>\n<Cmd>%d</Cmd>\n%s</LIST>\n" % (cmd, it))

    def handle(self, port, cmd, par, payload):
        codec = 3 if self.h265 else 2
        if cmd == 9027:
            return self.fn(port, cmd, 76)
        if cmd == 9032:
            vcodec = struct.unpack("<I", payload[:4])[0]
            if vcodec == 1 and True in self.clips:
                self.h265 = True
            elif vcodec == 0:
                self.h265 = False
            return self.reply(port, "<Function>\n<Cmd>%d</Cmd>\n<Status>0</Status>\n<Value>12</Value>\n</Function>" % cmd)
        if cmd in (9014, 9015, 9001):
            return self.fn(port, cmd, 1)
        if cmd == 9025:
            return self.reply(port, "<Function>\n<Cmd>9025</Cmd>\n<Status>0</Status>\n<String>2019-12-12-Ver1.00</String>\n</Function>")
        if cmd == 9012:
            return self.lst(port, cmd, [self.info_w, self.info_h, self.info_fps, 2048000, 0, 30, codec, 2, 2, 8] + [0] * 23)
        if cmd == 9017:
            return self.lst(port, cmd, [0, 0, 153600, self.info_fps, self.info_w, self.info_h, 1800, codec])
        if cmd == 9008:
            with self.lock:
                self.running[port] = True
            return self.fn(port, cmd, 1)
        if cmd == 9009:
            with self.lock:
                for p in (REC, DISP):
                    if par == 1 or p == port:
                        self.running[p] = False
            return self.fn(port, cmd, 1)
        if cmd == 9016:
            self.force_i[port] = True
            return self.fn(port, cmd, 1)
        return self.fn(port, cmd, -256)

    def serve_cmd(self, conn):
        buf = b""
        while True:
            data = conn.recv(4096)
            if not data:
                break
            buf += data
            while b"\x00" in buf:
                req, buf = buf.split(b"\x00", 1)
                text = req.decode(errors="replace")
                try:
                    head, q = text.split("/?", 1)
                    port = int(head.split(":")[1])
                    kv = dict(x.split("=", 1) for x in q.split("&"))
                    cmd, par = int(kv["cmd"]), int(kv["par"])
                except (ValueError, KeyError, IndexError):
                    continue
                self.stats["cmd"] += 1
                payload = b""
                if cmd in PAYLOAD:
                    conn.sendall(self.fn(port, cmd, 2))
                    payload = buf if buf else conn.recv(4096)
                    buf = b""
                    if len(payload) != PAYLOAD[cmd]:
                        self.stats["rejected"] += 1
                        conn.sendall(self.fn(port, cmd, -1))
                        continue
                conn.sendall(self.handle(port, cmd, par, payload))
        conn.close()

    def serve_data(self, port, conn):
        with self.lock:
            self.clients[port] = conn
        try:
            while conn.recv(4096):
                pass
        except OSError:
            pass
        with self.lock:
            if self.clients[port] is conn:
                self.clients[port] = None
        conn.close()

    def streamer(self, port):
        idx, gop, start_ts, sent = 0, 0, None, 0
        period = 1.0 / self.fps
        nxt = time.monotonic()
        while True:
            nxt += period
            time.sleep(max(0.0, nxt - time.monotonic()))
            with self.lock:
                conn = self.clients[port] if self.running[port] else None
            if conn is None:
                idx, gop, start_ts = 0, 0, None
                continue
            aus = self.clips[self.h265]
            if self.force_i[port]:
                self.force_i[port] = False
                idx = 0
            key, nals = aus[idx % len(aus)]
            if idx % len(aus) == 0:
                key = True
            gop = 1 if key else gop + 1
            ts = int((time.monotonic() - self.t0) * 1e6)
            if start_ts is None:
                if self.delay:
                    time.sleep(self.delay)
                    self.delay = 0
                    nxt = time.monotonic()
                    ts = int((nxt - self.t0) * 1e6)
                start_ts, sent = ts, 0
            if sent < 3:
                ts = start_ts
            sent += 1
            n = idx + 1
            idx += 1
            if self.skip and n % self.skip == 0 and not key:
                continue
            rec = bytearray(record(ts, gop, nals, self.h265))
            if self.corrupt and n % self.corrupt == 0 and len(rec) > 200:
                rec[-1] ^= 0xFF
            try:
                conn.sendall(rec)
            except OSError:
                pass

    def listen(self, port, handler):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((self.ip, port))
        s.listen(4)

        def loop():
            while True:
                c, _ = s.accept()
                threading.Thread(target=handler, args=(c,), daemon=True).start()

        threading.Thread(target=loop, daemon=True).start()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default="127.0.0.12")
    ap.add_argument("--h264", required=True)
    ap.add_argument("--h265")
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--info-fps", type=int, default=0,
                    help="frame rate the status replies claim (default: --fps)")
    ap.add_argument("--info-size", default="320x240",
                    help="WxH the status replies claim")
    ap.add_argument("--skip", type=int, default=0)
    ap.add_argument("--corrupt", type=int, default=0)
    ap.add_argument("--delay", type=float, default=0,
                    help="seconds before the first frame of the first session")
    args = ap.parse_args()
    m = Module(args)
    m.listen(CMD, m.serve_cmd)
    m.listen(REC, lambda c: m.serve_data(REC, c))
    m.listen(DISP, lambda c: m.serve_data(DISP, c))
    for p in (REC, DISP):
        threading.Thread(target=m.streamer, args=(p,), daemon=True).start()
    print("fake ethcam on %s" % args.ip, flush=True)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
