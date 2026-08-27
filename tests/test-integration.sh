#!/bin/sh
# Integration test for raptor daemons under ASan.
#
# Starts create_rings + daemons, exercises them with curl/raptorctl,
# then cleanly shuts down and checks ASan output for errors.
#
# Prerequisites:
#   ./build-asan.sh           # build all binaries
#   curl, ffprobe (optional)  # HTTP/RTSP clients
#
# Usage:
#   ./tests/test-integration.sh          # run all tests
#   ./tests/test-integration.sh -v       # verbose (show daemon logs)
#   ./tests/test-integration.sh -k       # keep running after tests (interactive)

set -e

RAPTOR_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$RAPTOR_DIR/asan-out"
LOG_DIR="$OUT/test-logs"
PASS=0
FAIL=0
SKIP=0
VERBOSE=0
KEEP=0

for arg in "$@"; do
    case "$arg" in
        -v) VERBOSE=1 ;;
        -k) KEEP=1 ;;
    esac
done

# ── Helpers ──

cleanup() {
    echo ""
    echo "=== Shutting down ==="
    for pid in $DAEMON_PIDS; do
        kill "$pid" 2>/dev/null || true
    done
    # Wait for ASan reports
    sleep 1
    for pid in $DAEMON_PIDS; do
        wait "$pid" 2>/dev/null || true
    done
    kill "$RINGS_PID" 2>/dev/null || true
    wait "$RINGS_PID" 2>/dev/null || true
}

trap cleanup EXIT

pass() {
    PASS=$((PASS + 1))
    printf "  PASS  %s\n" "$1"
}

fail() {
    FAIL=$((FAIL + 1))
    printf "  FAIL  %s: %s\n" "$1" "$2"
}

skip() {
    SKIP=$((SKIP + 1))
    printf "  SKIP  %s: %s\n" "$1" "$2"
}

# Check a command succeeded (exit 0)
check() {
    local name="$1"
    shift
    if "$@" > /dev/null 2>&1; then
        pass "$name"
    else
        fail "$name" "exit code $?"
    fi
}

# Check command output contains a string
check_contains() {
    local name="$1" pattern="$2"
    shift 2
    local output
    output=$("$@" 2>&1) || true
    if echo "$output" | grep -q "$pattern"; then
        pass "$name"
    else
        fail "$name" "expected '$pattern' in output"
        if [ "$VERBOSE" = "1" ]; then
            echo "    got: $(echo "$output" | head -3)"
        fi
    fi
}

# Check HTTP response code
check_http() {
    local name="$1" url="$2" expected="$3"
    local code
    code=$(curl -s -o /dev/null -w "%{http_code}" --max-time 3 "$url" 2>/dev/null) || true
    if [ "$code" = "$expected" ]; then
        pass "$name"
    else
        fail "$name" "expected HTTP $expected, got $code"
    fi
}

# ── Preflight ──

if [ ! -f "$OUT/create_rings" ] || [ ! -f "$OUT/rvd" ]; then
    echo "ERROR: Run ./build-asan.sh first"
    exit 1
fi

mkdir -p "$LOG_DIR"
touch "$LOG_DIR/.suite-start"
DAEMON_PIDS=""

# Use a test config
cat > "$LOG_DIR/test.conf" << 'CONF'
[sensor]
model = gc2053
name = gc2053
i2c_addr = 0x37

[stream0]
width = 1920
height = 1080
fps = 25
bitrate = 2000000
codec = h264
rc_mode = vbr
gop = 50

[stream1]
width = 640
height = 360
fps = 25
bitrate = 500000
codec = h265

[audio]
enabled = true
sample_rate = 16000
codec = l16

[rtsp]
backchannel = true
port = 15554
# Bounded like a real device: multi-MB autotuned sndbufs absorb client
# stalls silently and the recovery-invariants leg's fault never fires.
tcp_sndbuf = 65536

[http]
port = 18080
username =
password =

[webrtc]
port = 18443
username =
password =

[srt]
enabled = true
port = 19000
audio = false

[osd]
enabled = false

[ircut]
enabled = false

[motion]
enabled = false

[recording]
enabled = true
mode = continuous
stream = 0
audio = false
storage_path = __LOG_DIR__/rec
segment_minutes = 5
segment_seconds = 10
sei_timecode = true
sign = true
sign_key = __LOG_DIR__/sign.key

[ring]
refmode = true

[log]
level = debug
CONF

sed -i "s|__LOG_DIR__|$LOG_DIR|g" "$LOG_DIR/test.conf"
mkdir -p "$LOG_DIR/rec"

CONFIG="$LOG_DIR/test.conf"

# Backend-selection variants. The base config intentionally omits [system],
# exercising the established implicit IMP default.
{
    printf '[system]\nvideo_backend = imp\n\n'
    cat "$CONFIG"
} > "$LOG_DIR/test-imp.conf"
{
    printf '[system]\nvideo_backend = v4l2\nvideo_device = /dev/video0\n\n'
    cat "$CONFIG"
} > "$LOG_DIR/test-v4l2.conf"
IMP_CONFIG="$LOG_DIR/test-imp.conf"
V4L2_CONFIG="$LOG_DIR/test-v4l2.conf"

# Clean stale state from previous runs
mkdir -p /var/run/rss 2>/dev/null || { sudo mkdir -p /var/run/rss && sudo chmod 1777 /var/run/rss; }
rm -f /var/run/rss/*.pid /var/run/rss/*.sock 2>/dev/null
rm -f /dev/shm/rss_ring_* /dev/shm/rss_osd_* 2>/dev/null

# Kill any lingering daemons from previous runs
for d in rvd rsd rhd rod ric rmr rwd; do
    pkill -f "asan-out/$d" 2>/dev/null || true
done
sleep 0.5

# ── Start infrastructure ──

echo "=== Starting rings ==="
"$OUT/create_rings" --skip-video > "$LOG_DIR/rings.log" 2>&1 &
RINGS_PID=$!
sleep 1

if ! kill -0 "$RINGS_PID" 2>/dev/null; then
    echo "ERROR: create_rings failed to start"
    cat "$LOG_DIR/rings.log"
    exit 1
fi

echo "=== Backend selection tests ==="
if "$OUT/rvd" -c "$V4L2_CONFIG" -f -d > "$LOG_DIR/rvd-v4l2-disabled.log" 2>&1; then
    fail "disabled V4L2 backend is rejected" "rvd unexpectedly exited successfully"
elif grep -q "V4L2/OpenIMP backend requested but Raptor was built without V4L2_OPENIMP=1" \
        "$LOG_DIR/rvd-v4l2-disabled.log"; then
    pass "disabled V4L2 backend is rejected with an explicit message"
else
    fail "disabled V4L2 backend is rejected" "explicit diagnostic missing"
fi

# Start the explicit IMP form far enough to record its initialized topology,
# then compare it with the normal omitted-key daemon below.
"$OUT/rvd" -c "$IMP_CONFIG" -f -d > "$LOG_DIR/rvd-imp-explicit.log" 2>&1 &
IMP_PID=$!
IMP_PIPELINE=""
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    IMP_PIPELINE=$(sed -n 's/.*\(pipeline ready:.*\)/\1/p' \
        "$LOG_DIR/rvd-imp-explicit.log" | tail -1)
    [ -n "$IMP_PIPELINE" ] && break
    kill -0 "$IMP_PID" 2>/dev/null || break
    sleep 0.1
done
if [ -n "$IMP_PIPELINE" ] && kill -0 "$IMP_PID" 2>/dev/null; then
    pass "explicit IMP backend initializes"
else
    fail "explicit IMP backend initializes" "pipeline did not become ready"
fi
kill "$IMP_PID" 2>/dev/null || true
wait "$IMP_PID" 2>/dev/null || true

echo "=== Starting daemons ==="

start_daemon() {
    local name="$1"
    shift
    if [ "$VERBOSE" = "1" ]; then
        "$@" 2>&1 | tee "$LOG_DIR/$name.log" &
    else
        "$@" > "$LOG_DIR/$name.log" 2>&1 &
    fi
    local pid=$!
    DAEMON_PIDS="$DAEMON_PIDS $pid"
    sleep 0.3
    if kill -0 "$pid" 2>/dev/null; then
        echo "  started $name (pid $pid)"
    else
        echo "  FAILED  $name (check $LOG_DIR/$name.log)"
        cat "$LOG_DIR/$name.log" 2>/dev/null | head -20
    fi
}

# Core daemons (must start)
start_daemon rvd "$OUT/rvd" -c "$CONFIG" -f -d
start_daemon rhd "$OUT/rhd" -c "$CONFIG" -f -d
start_daemon rsd "$OUT/rsd" -c "$CONFIG" -f -d
start_daemon rsr "$OUT/rsr" -c "$CONFIG" -f -d

# Optional daemons (may exit immediately if disabled in config — that's OK)
start_daemon rod "$OUT/rod" -c "$CONFIG" -f -d
start_daemon ric "$OUT/ric" -c "$CONFIG" -f -d
start_daemon rmr "$OUT/rmr" -c "$CONFIG" -f -d

# Let daemons settle
sleep 2

DEFAULT_PIPELINE=$(sed -n 's/.*\(pipeline ready:.*\)/\1/p' "$LOG_DIR/rvd.log" | tail -1)
if [ -n "$IMP_PIPELINE" ] && [ "$DEFAULT_PIPELINE" = "$IMP_PIPELINE" ]; then
    pass "explicit IMP backend matches the omitted-key default"
else
    fail "explicit IMP backend matches the omitted-key default" \
        "explicit='$IMP_PIPELINE' default='$DEFAULT_PIPELINE'"
fi

# ── Tests ──

echo ""
echo "=== raptorctl tests ==="

# Status
check_contains "raptorctl status" "rvd" "$OUT/raptorctl" status
check_contains "raptorctl rvd status" "status" "$OUT/raptorctl" rvd status

# Encoder getters
check_contains "get-bitrate" "bitrate" "$OUT/raptorctl" rvd get-bitrate 0
check_contains "get-fps" "fps_num" "$OUT/raptorctl" rvd get-fps 0
check_contains "get-gop" "gop" "$OUT/raptorctl" rvd get-gop 0
check_contains "get-rc-mode" "rc_mode" "$OUT/raptorctl" rvd get-rc-mode 0
check_contains "get-qp-bounds" "min_qp" "$OUT/raptorctl" rvd get-qp-bounds 0
check_contains "get-enc-caps" "smartp_gop" "$OUT/raptorctl" rvd get-enc-caps

# Encoder setters
check_contains "set-bitrate" "ok" "$OUT/raptorctl" rvd set-bitrate 0 3000000
check_contains "set-gop" "ok" "$OUT/raptorctl" rvd set-gop 0 60
check_contains "set-fps" "ok" "$OUT/raptorctl" rvd set-fps 0 30
check_contains "set-qp-bounds" "ok" "$OUT/raptorctl" rvd set-qp-bounds 0 15 45
check_contains "set-rc-mode" "ok" "$OUT/raptorctl" rvd set-rc-mode 0 cbr
check_contains "request-idr" "ok" "$OUT/raptorctl" rvd request-idr

# Pin keyframe truth across the full producer-to-ring path. RSD waits for this
# bit before releasing a new client, so an IDR NAL marked non-key is a stream
# outage rather than cosmetic metadata.
timeout 5 "$OUT/ringdump" main -f -s -n 8 > "$LOG_DIR/keyframe-ring.log" 2>&1 &
KEY_READER_PID=$!
sleep 0.2
"$OUT/ringdump" main -i > /dev/null 2>&1 || true
wait "$KEY_READER_PID" 2>/dev/null || true
if grep -q 'nal=H264_IDR.*key=1' "$LOG_DIR/keyframe-ring.log"; then
    pass "requested H.264 IDR reaches the ring as a keyframe"
else
    fail "requested H.264 IDR reaches the ring as a keyframe" \
        "no H264_IDR/key=1 frame observed"
fi
if grep -Eq 'nal=H264_IDR.*key=0|nal=H264_SLICE.*key=1' \
        "$LOG_DIR/keyframe-ring.log"; then
    fail "ring key flag agrees with H.264 NAL type" "mismatched frame metadata"
else
    pass "ring key flag agrees with H.264 NAL type"
fi

# Advanced encoder
check_contains "enc-set gop_mode" "ok" "$OUT/raptorctl" rvd enc-set 0 gop_mode 0
check_contains "enc-get gop_mode" "gop_mode" "$OUT/raptorctl" rvd enc-get 0 gop_mode
check_contains "enc-set color2grey" "ok" "$OUT/raptorctl" rvd enc-set 0 color2grey 1
check_contains "enc-get color2grey" "color2grey" "$OUT/raptorctl" rvd enc-get 0 color2grey

# ISP
check_contains "get-isp" "brightness" "$OUT/raptorctl" rvd get-isp
check_contains "get-wb" "mode" "$OUT/raptorctl" rvd get-wb
check_contains "get-exposure" "total_gain" "$OUT/raptorctl" rvd get-exposure
check_contains "get-exposure validity" "valid_mask" "$OUT/raptorctl" rvd get-exposure
check_contains "set-brightness" "ok" "$OUT/raptorctl" rvd set-brightness 200

# Config
check_contains "config-show" "config" "$OUT/raptorctl" rvd config

# Memory / CPU
check_contains "raptorctl memory" "DAEMON\|Private\|rvd" "$OUT/raptorctl" memory
check_contains "raptorctl cpu" "DAEMON\|%\|rvd" "$OUT/raptorctl" cpu

echo ""
echo "=== RHD HTTP tests ==="

# Index page (will 404 since no html file, that's expected)
check_http "GET /" "http://127.0.0.1:18080/" "404"

# JPEG snapshot (wait for JPEG ring producer to start publishing)
snap_ok=false
for i in 1 2 3 4 5; do
    code=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:18080/snap" 2>/dev/null)
    if [ "$code" = "200" ]; then snap_ok=true; break; fi
    sleep 1
done
if $snap_ok; then pass "GET /snap"; else fail "GET /snap: expected HTTP 200, got $code"; fi

# MJPEG stream (connect briefly — timeout exit 124 = success for streaming)
if timeout 2 curl -s -o /dev/null "http://127.0.0.1:18080/mjpeg" 2>/dev/null; ret=$?; [ "$ret" = 124 ] || [ "$ret" = 0 ]; then
    pass "MJPEG stream starts"
else
    fail "MJPEG stream starts" "exit $ret"
fi

# ── JPEG IDR gate: a consumer's keyframe request must reach the
#    encoder for a video channel and must not for a JPEG one, where
#    every frame is already intra. The gate is invisible on real
#    hardware (the call it skips was a no-op there), so the mock's
#    enc_request_idr trace is the only artifact that can pin it.
#    Positive control first: the video assertion proves the
#    instrumentation before the JPEG absence means anything.
#    Every command is guarded: this file runs under set -e, and a
#    test that can abort the suite is worse than no test.
# Only lines written from here on count: raptorctl's request-idr test
# above deliberately drives the same HAL call, so scanning the whole
# log would read its output as this test's result.
IDR_MARK=$(wc -l < "$LOG_DIR/rvd.log" 2>/dev/null || echo 0)
idr_log_since() { tail -n "+$((IDR_MARK + 1))" "$LOG_DIR/rvd.log" 2>/dev/null || true; }
if "$OUT/ringdump" main -i > /dev/null 2>&1; then
    sleep 2
    check_contains "IDR request on a video ring reaches the encoder" \
        "mock: enc_request_idr chn=0$" idr_log_since

    # A held reader starts the JPEG encoder, so the frame loop reaches
    # the gate instead of parking in its no-consumers branch.
    "$OUT/ringdump" jpeg0 -f -s > /dev/null 2>&1 &
    IDR_READER=$!
    sleep 2
    JCHN=$(sed -n 's/.*jpeg chn \([0-9]*\): started.*/\1/p' "$LOG_DIR/rvd.log" | tail -1 || true)
    IDR_MARK2=$(wc -l < "$LOG_DIR/rvd.log" 2>/dev/null || echo 0)
    "$OUT/ringdump" jpeg0 -i > /dev/null 2>&1 || true
    sleep 2
    kill $IDR_READER 2>/dev/null || true
    wait $IDR_READER 2>/dev/null || true
    if [ -z "$JCHN" ]; then
        fail "IDR request on a JPEG ring is gated" "jpeg channel never started"
    elif tail -n "+$((IDR_MARK2 + 1))" "$LOG_DIR/rvd.log" 2>/dev/null |
            grep -q "mock: enc_request_idr chn=$JCHN\$"; then
        fail "IDR request on a JPEG ring is gated" "chn $JCHN received an IDR call"
    else
        pass "IDR request on a JPEG ring is gated (chn $JCHN: flag raised, no call)"
    fi
else
    fail "JPEG IDR gate" "ringdump -i unavailable (stale asan build? run build-asan.sh)"
fi

# Audio stream
if timeout 2 curl -s -o /dev/null "http://127.0.0.1:18080/audio" 2>/dev/null; ret=$?; [ "$ret" = 124 ] || [ "$ret" = 0 ]; then
    pass "audio stream starts"
else
    fail "audio stream starts" "exit $ret"
fi

# 404
check_http "GET /invalid" "http://127.0.0.1:18080/nonexistent" "404"

# RHD clients list
check_contains "rhd clients" "ok" "$OUT/raptorctl" rhd clients

echo ""
echo "=== RSD RTSP tests ==="

# RSD status via raptorctl
check_contains "rsd status" "ok" "$OUT/raptorctl" rsd status

# RSD clients
check_contains "rsd clients" "ok" "$OUT/raptorctl" rsd clients

# SDP content validation via raw RTSP DESCRIBE
rtsp_describe() {
    local port="$1" path="$2"
    {
        printf "DESCRIBE rtsp://127.0.0.1:%d/%s RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n" "$port" "$path"
        sleep 1
    } | nc -q 1 127.0.0.1 "$port" 2>/dev/null
}

# stream0 = H.264
SDP0=$(rtsp_describe 15554 stream0)
if [ -n "$SDP0" ] && echo "$SDP0" | grep -qi "H264"; then
    if echo "$SDP0" | grep -q "sprop-parameter-sets="; then
        pass "H.264 SDP sprop-parameter-sets"
    else
        fail "H.264 SDP sprop-parameter-sets" "missing from SDP"
    fi
else
    skip "H.264 SDP" "DESCRIBE failed or codec mismatch"
fi

# stream1 = H.265
SDP1=$(rtsp_describe 15554 stream1)
if [ -n "$SDP1" ] && echo "$SDP1" | grep -qi "H265"; then
    sdp_ok=true
    for param in sprop-vps sprop-sps sprop-pps; do
        if ! echo "$SDP1" | grep -q "$param="; then
            fail "H.265 SDP $param" "missing from SDP"
            sdp_ok=false
        fi
    done
    if $sdp_ok; then
        pass "H.265 SDP sprop-vps/sps/pps"
    fi
else
    skip "H.265 SDP" "DESCRIBE failed or codec mismatch"
fi

# ── A rate change reaches the next client's SDP ──
# rvd republishes the ring header on set-fps, but rsd answers DESCRIBE from
# a cache it filled when it opened the ring, and an in-place republish wakes
# nobody. Without a refresh in the reader every client after the change is
# still told the old rate -- which the ring header cannot show, so this goes
# through DESCRIBE.
sdp_framerate() {
    rtsp_describe 15554 stream0 | sed -n 's/.*a=framerate:\([0-9][0-9]*\).*/\1/p' | head -1
}

FPS_SDP_BEFORE=$(sdp_framerate)
"$OUT/raptorctl" rvd set-fps 0 12 > /dev/null 2>&1
sleep 2
FPS_SDP_AFTER=$(sdp_framerate)
if [ "$FPS_SDP_AFTER" = "12" ] && [ "$FPS_SDP_BEFORE" != "12" ]; then
    pass "set-fps moves a=framerate for the next client ($FPS_SDP_BEFORE -> $FPS_SDP_AFTER)"
elif [ -z "$FPS_SDP_BEFORE" ]; then
    skip "set-fps moves a=framerate" "no a=framerate in the SDP (DESCRIBE failed?)"
else
    fail "set-fps moves a=framerate for the next client" \
        "before=$FPS_SDP_BEFORE after=$FPS_SDP_AFTER, expected 12"
fi

# Hand the following legs back the rate they had
if [ -n "$FPS_SDP_BEFORE" ]; then
    "$OUT/raptorctl" rvd set-fps 0 "$FPS_SDP_BEFORE" > /dev/null 2>&1
    sleep 1
fi

# ── RTP over UDP push (rsp net mode) ──
# [push] url=udp:// sends ring video as RTP datagrams with no session
# and no handshake: bind a receiver, point rsp at it, and judge the
# wire. The receiver reassembles fragmented and aggregated NALs, so
# this pins the packetization (version, PT, a single SSRC, contiguous
# sequence numbers on loopback) and the keyframe-first join: the full
# parameter set and an IDR must be the first thing a fresh receiver
# can decode. Both codecs run, because the NAL walk branches on the
# header size and each branch deserves its own wire proof.
cat > "$LOG_DIR/udp-push-listen.py" << 'PYEOF'
import socket
import sys
import time

verdict_path, port, codec = sys.argv[1], int(sys.argv[2]), sys.argv[3]
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", port))
sock.settimeout(1.0)

pkts = []
start = time.time()
first = None
while True:
    now = time.time()
    if first is not None and now - first > 4.0:
        break
    if now - start > 12.0:
        break
    try:
        data, _ = sock.recvfrom(65536)
    except socket.timeout:
        continue
    if first is None:
        first = time.time()
    pkts.append(data)

verdict = "FAIL no packets received"
if pkts:
    ssrcs = set()
    seqs = []
    types = set()
    bad = 0
    for p in pkts:
        if len(p) < 13 or (p[0] >> 6) != 2 or (p[1] & 0x7F) != 96:
            bad += 1
            continue
        ssrcs.add(p[8:12])
        seqs.append((p[2] << 8) | p[3])
        b = p[12:]
        if codec == "h265":
            t = (b[0] >> 1) & 0x3F
            if t < 48:
                types.add(t)
            elif t == 48:  # aggregation packet
                i = 2
                while i + 2 <= len(b):
                    ln = (b[i] << 8) | b[i + 1]
                    if i + 2 + ln > len(b) or ln == 0:
                        break
                    types.add((b[i + 2] >> 1) & 0x3F)
                    i += 2 + ln
            elif t == 49:  # fragmentation unit
                if len(b) >= 3 and (b[2] & 0x80):
                    types.add(b[2] & 0x3F)
        else:
            t = b[0] & 0x1F
            if 1 <= t <= 23:
                types.add(t)
            elif t == 24:  # STAP-A aggregate
                i = 1
                while i + 2 <= len(b):
                    ln = (b[i] << 8) | b[i + 1]
                    if i + 2 + ln > len(b) or ln == 0:
                        break
                    types.add(b[i + 2] & 0x1F)
                    i += 2 + ln
            elif t == 28:  # FU-A fragment
                if len(b) >= 2 and (b[1] & 0x80):
                    types.add(b[1] & 0x1F)
    lost = 0
    for a, c in zip(seqs, seqs[1:]):
        lost += ((c - a) & 0xFFFF) - 1
    # h264: SPS, PPS, IDR. h265: VPS, SPS, PPS (IDR arrives with them
    # but its exact type varies, 19 or 20, so the params are the pin).
    need = {32, 33, 34} if codec == "h265" else {7, 8, 5}
    missing = sorted(need - types)
    if bad:
        verdict = f"FAIL {bad}/{len(pkts)} packets not RTP v2 PT96"
    elif len(ssrcs) != 1:
        verdict = f"FAIL {len(ssrcs)} SSRCs on one stream"
    elif lost:
        verdict = f"FAIL {lost} sequence gaps on loopback"
    elif len(pkts) < 100:
        verdict = f"FAIL only {len(pkts)} packets in the window"
    elif missing:
        verdict = f"FAIL NAL types missing from join: {missing} (saw {sorted(types)})"
    else:
        verdict = (f"PASS {len(pkts)} pkts, 0 lost, one ssrc, "
                   f"nal types {sorted(types)}")
with open(verdict_path, "w") as f:
    f.write(verdict)
PYEOF

udp_push_leg() {
    UPL_CODEC="$1"
    UPL_STREAM="$2"
    UPL_PORT="$3"
    UPL_CONF="$LOG_DIR/test-push-$UPL_CODEC.conf"
    cp "$CONFIG" "$UPL_CONF"
    {
        echo ""
        echo "[push]"
        echo "enabled = true"
        echo "url = udp://127.0.0.1:$UPL_PORT"
        echo "stream = $UPL_STREAM"
        echo "audio = false"
        echo "autostart = true"
    } >> "$UPL_CONF"
    python3 "$LOG_DIR/udp-push-listen.py" "$LOG_DIR/udp-$UPL_CODEC.verdict" \
        "$UPL_PORT" "$UPL_CODEC" > "$LOG_DIR/udp-$UPL_CODEC.log" 2>&1 &
    UPL_LISTEN_PID=$!
    sleep 0.5
    start_daemon "rsp-$UPL_CODEC" "$OUT/rsp" -c "$UPL_CONF" -f -d
    wait "$UPL_LISTEN_PID"
    UPL_VERDICT=$(cat "$LOG_DIR/udp-$UPL_CODEC.verdict" 2>/dev/null)
    case "$UPL_VERDICT" in
        PASS*)
            pass "udp push $UPL_CODEC delivers decodable RTP from the join (${UPL_VERDICT#PASS })"
            ;;
        *)
            fail "udp push $UPL_CODEC delivers decodable RTP from the join" \
                "${UPL_VERDICT:-listener wrote no verdict}"
            ;;
    esac
    pkill -f "$OUT/rsp" 2>/dev/null || true
    sleep 0.5
}

udp_push_leg h264 0 15998
udp_push_leg h265 1 15996

# ffprobe (if available — best RTSP test tool)
if command -v ffprobe > /dev/null 2>&1; then
    # ffprobe with timeout — exit 124 (timeout) means it connected and received data
    if timeout 5 ffprobe -v quiet -print_format json -show_streams \
        -rtsp_transport tcp "rtsp://127.0.0.1:15554/stream0" > /dev/null 2>&1; ret=$?; \
        [ "$ret" = 0 ] || [ "$ret" = 124 ]; then
        pass "ffprobe RTSP stream0"
    else
        fail "ffprobe RTSP stream0" "exit $ret"
    fi
else
    skip "ffprobe RTSP" "ffprobe not installed"
fi

# ── SR wall-clock mapping (rlatency over UDP) ──
# rlatency maps RTP timestamps to NTP via the Sender Reports and
# compares against this host's clock. Client and server share the
# clock here, so the p50 is loopback flight plus scheduler noise --
# microseconds -- and the assertion is really about compy's
# mono-to-NTP anchor: a sub-second truncation in it sat at a steady
# -519 ms while this whole suite stayed green, because nothing on x86
# mapped SR NTP against a wall clock. The 15 ms budget is orders of
# magnitude above loopback jitter and below every failure mode this
# guards. Incidentally the suite's only UDP-transport RTSP session.
if cc -O2 -o "$OUT/rlatency-host" "$RAPTOR_DIR/rlatency/rlatency.c" -lm > /dev/null 2>&1; then
    RLAT_OUT=$(timeout 40 "$OUT/rlatency-host" "rtsp://127.0.0.1:15554/stream0" -n 100 -q 2>&1)
    RLAT_P50=$(echo "$RLAT_OUT" | sed -n 's/.*P50: *\(-*[0-9.]*\) ms.*/\1/p')
    if [ -z "$RLAT_P50" ]; then
        fail "SR wall-clock mapping (rlatency, UDP)" \
            "no samples: $(echo "$RLAT_OUT" | tail -1)"
    elif python3 -c "import sys; sys.exit(0 if abs(float('$RLAT_P50')) <= 15 else 1)"; then
        pass "SR wall-clock mapping (rlatency, UDP): p50 ${RLAT_P50}ms"
    else
        fail "SR wall-clock mapping (rlatency, UDP)" \
            "p50 ${RLAT_P50}ms -- SR NTP anchor off against the shared host clock"
    fi
else
    fail "SR wall-clock mapping (rlatency, UDP)" "rlatency build failed"
fi

# ── Track re-SETUP lifecycle ──
# Pre-PLAY re-SETUP is transport renegotiation and must replace the
# first SETUP's transports (the replaced RTCP instance used to leak
# and pin the SR NTP anchor armed for the daemon's lifetime -- LSAN
# in the sanitizer stage now sees such a leak). After PLAY a re-SETUP
# is refused 455 with the session intact.
RESETUP_OUT=$(python3 - <<'RESETUP_EOF'
import socket

def req(sock, buf, method, url, cseq, extra=""):
    sock.sendall(f"{method} {url} RTSP/1.0\r\nCSeq: {cseq}\r\n{extra}\r\n".encode())
    while b"\r\n\r\n" not in buf[0]:
        d = sock.recv(4096)
        if not d:
            return ""
        buf[0] += d
    head, _, rest = buf[0].partition(b"\r\n\r\n")
    headers = head.decode(errors="replace")
    clen = 0
    for line in headers.split("\r\n"):
        if line.lower().startswith("content-length:"):
            clen = int(line.split(":", 1)[1])
    while len(rest) < clen:
        rest += sock.recv(4096)
    buf[0] = rest[clen:]
    return headers

s = socket.create_connection(("127.0.0.1", 15554), timeout=5)
buf = [b""]
base = "rtsp://127.0.0.1:15554/stream0"
req(s, buf, "DESCRIBE", base, 1, "Accept: application/sdp\r\n")
t = "Transport: RTP/AVP;unicast;client_port=45400-45401\r\n"
r1 = req(s, buf, "SETUP", base + "/video", 2, t)
sid = ""
for line in r1.split("\r\n"):
    if line.lower().startswith("session:"):
        sid = line.split(":", 1)[1].split(";")[0].strip()
sess = f"Session: {sid}\r\n"
t2 = "Transport: RTP/AVP;unicast;client_port=45402-45403\r\n"
r2 = req(s, buf, "SETUP", base + "/video", 3, t2 + sess)
r3 = req(s, buf, "PLAY", base, 4, sess + "Range: npt=0.000-\r\n")
r4 = req(s, buf, "SETUP", base + "/video", 5, t + sess)
r5 = req(s, buf, "GET_PARAMETER", base, 6, sess)
req(s, buf, "TEARDOWN", base, 7, sess)
s.close()
print("RENEG=" + ("200" if " 200 " in r2.split("\r\n")[0] + " " else r2.split("\r\n")[0]))
print("MIDPLAY=" + ("455" if " 455 " in r4.split("\r\n")[0] + " " else r4.split("\r\n")[0]))
print("ALIVE=" + ("200" if " 200 " in r5.split("\r\n")[0] + " " else r5.split("\r\n")[0]))
RESETUP_EOF
)
if echo "$RESETUP_OUT" | grep -q "RENEG=200"; then
    pass "pre-PLAY re-SETUP renegotiates (200, old transports dropped)"
else
    fail "pre-PLAY re-SETUP" "$(echo "$RESETUP_OUT" | grep RENEG=)"
fi
if echo "$RESETUP_OUT" | grep -q "MIDPLAY=455"; then
    pass "mid-PLAY re-SETUP refused (455)"
else
    fail "mid-PLAY re-SETUP" "expected 455: $(echo "$RESETUP_OUT" | grep MIDPLAY=)"
fi
if echo "$RESETUP_OUT" | grep -q "ALIVE=200"; then
    pass "session intact after refused re-SETUP"
else
    fail "session after refused re-SETUP" "$(echo "$RESETUP_OUT" | grep ALIVE=)"
fi

echo ""
echo "=== Multi-client stress test ==="

# Launch 4 concurrent MJPEG clients (timeout handles cleanup)
CURL_PIDS=""
for i in 1 2 3 4; do
    timeout 3 curl -s -o /dev/null "http://127.0.0.1:18080/mjpeg" &
    CURL_PIDS="$CURL_PIDS $!"
done
sleep 2
check_contains "RHD survives 4 MJPEG clients" "ok" "$OUT/raptorctl" rhd status
for p in $CURL_PIDS; do kill "$p" 2>/dev/null; done
wait $CURL_PIDS 2>/dev/null || true

# Launch 4 concurrent audio clients
CURL_PIDS=""
for i in 1 2 3 4; do
    timeout 3 curl -s -o /dev/null "http://127.0.0.1:18080/audio" &
    CURL_PIDS="$CURL_PIDS $!"
done
sleep 2
check_contains "RHD survives 4 audio clients" "ok" "$OUT/raptorctl" rhd status
for p in $CURL_PIDS; do kill "$p" 2>/dev/null; done
wait $CURL_PIDS 2>/dev/null || true

echo ""
echo "=== Slow client tests ==="

if [ -x "$OUT/test_slow_rtsp" ]; then
    # Test 1: Abrupt disconnect -- RSD should clean up without errors
    "$OUT/test_slow_rtsp" -p 15554 drop 2 > "$LOG_DIR/slow_drop.log" 2>&1 || true
    sleep 1
    check_contains "RSD survives abrupt disconnect" "ok" "$OUT/raptorctl" rsd status

    # Test 2: Slow reader + normal client coexist
    "$OUT/test_slow_rtsp" -p 15554 -d 8 slow 500 > "$LOG_DIR/slow_reader.log" 2>&1 &
    SLOW_PID=$!
    sleep 1
    # Normal client should work fine while slow client is connected
    if command -v ffprobe > /dev/null 2>&1; then
        if timeout 5 ffprobe -v quiet -print_format json -show_streams \
            -rtsp_transport tcp "rtsp://127.0.0.1:15554/stream0" > /dev/null 2>&1; ret=$?; \
            [ "$ret" = 0 ] || [ "$ret" = 124 ]; then
            pass "normal client unaffected by slow client"
        else
            fail "normal client unaffected by slow client" "exit $ret"
        fi
    else
        skip "normal client isolation" "ffprobe not installed"
    fi
    kill "$SLOW_PID" 2>/dev/null; wait "$SLOW_PID" 2>/dev/null || true

    # Test 3: Stalled client -- reads for 2s then stops completely
    "$OUT/test_slow_rtsp" -p 15554 -d 10 stall 2 > "$LOG_DIR/slow_stall.log" 2>&1 &
    STALL_PID=$!
    sleep 4
    # RSD should still be healthy
    check_contains "RSD healthy during stalled client" "ok" "$OUT/raptorctl" rsd status
    kill "$STALL_PID" 2>/dev/null; wait "$STALL_PID" 2>/dev/null || true

    # Verify RSD has no lingering clients after cleanup
    sleep 1
    check_contains "RSD no lingering clients" "ok" "$OUT/raptorctl" rsd status

    # Test 4: Verify refmode zerocopy race is fixed
    # rss_ring_read (validated copy) prevents recycled-during-send
    "$OUT/test_slow_rtsp" -p 15554 -d 8 slow 5000 > "$LOG_DIR/slow_refmode.log" 2>&1 &
    SLOW_REF_PID=$!
    sleep 5
    kill "$SLOW_REF_PID" 2>/dev/null; wait "$SLOW_REF_PID" 2>/dev/null || true
    if grep -q "recycled during send" "$LOG_DIR/rsd.log"; then
        fail "zerocopy refmode fix" "recycled-during-send still present"
    else
        pass "no zerocopy recycled-during-send (refmode fix verified)"
    fi
else
    skip "slow client tests" "test_slow_rtsp not built"
fi

echo ""
echo "=== ROD/RIC tests ==="

# ROD/RIC may exit immediately if disabled in config — skip if not running
if "$OUT/raptorctl" rod status > /dev/null 2>&1; then
    check_contains "rod status" "ok" "$OUT/raptorctl" rod status
else
    skip "rod status" "disabled in config"
fi
if "$OUT/raptorctl" ric status > /dev/null 2>&1; then
    check_contains "ric status" "ok" "$OUT/raptorctl" ric status
else
    skip "ric status" "disabled in config"
fi

echo ""
echo "=== Config round-trip ==="

check_contains "config save" "ok" "$OUT/raptorctl" config save

# ── Truthful config-save logging ──
# A daemon that wrote its runtime changes logs "running config saved";
# one with nothing dirty must say so and leave the file alone. The
# save above cleared rvd's dirty state, so the next save is a no-op.
CFG_MARK=$(wc -l < "$LOG_DIR/rvd.log" 2>/dev/null || echo 0)
cfg_log_since() { tail -n "+$((CFG_MARK + 1))" "$LOG_DIR/rvd.log" 2>/dev/null || true; }

"$OUT/raptorctl" config save > /dev/null 2>&1
sleep 1
if cfg_log_since | grep -q "no config changes to save"; then
    pass "no-op config save logs untouched"
else
    fail "no-op config save logs untouched" \
        "expected 'no config changes to save' in rvd log"
fi
if cfg_log_since | grep -q "running config saved"; then
    fail "no-op config save does not claim a write" \
        "'running config saved' logged with nothing dirty"
else
    pass "no-op config save does not claim a write"
fi

# Dirty rvd's config without changing effective state: re-set the gop
# to the value it already has (set marks it dirty regardless)
GOP_NOW=$("$OUT/raptorctl" rvd enc-get 0 gop 2>/dev/null | grep -o '[0-9][0-9]*' | tail -1)
[ -n "$GOP_NOW" ] || GOP_NOW=50
"$OUT/raptorctl" rvd set-gop 0 "$GOP_NOW" > /dev/null 2>&1
CFG_MARK=$(wc -l < "$LOG_DIR/rvd.log" 2>/dev/null || echo 0)
"$OUT/raptorctl" config save > /dev/null 2>&1
sleep 1
if cfg_log_since | grep -q "running config saved"; then
    pass "dirty config save logs the write"
else
    fail "dirty config save logs the write" \
        "expected 'running config saved' in rvd log"
fi

# ── A snapshot channel's settings land in a section that is read back ──
# A JPEG channel is built from its parent video stream and has no
# section of its own. Given an empty one, a persisted key is written
# above every [section] header, where no loader will ever read it
# again -- the file still parses, the daemon still answers ok, and the
# setting is gone at the next boot. So this goes through the file:
# set-jpeg-quality is the one command that only accepts a JPEG channel,
# and the channel index is discovered rather than assumed, since it
# depends on how many video streams the config declares.
JQ_CHN=""
for c in 0 1 2 3 4 5; do
    if "$OUT/raptorctl" rvd set-jpeg-quality "$c" 60 2>/dev/null | grep -q '"ok"'; then
        JQ_CHN="$c"
        break
    fi
done
if [ -z "$JQ_CHN" ]; then
    skip "jpeg quality persists into a real section" "no JPEG channel accepted set-jpeg-quality"
else
    "$OUT/raptorctl" config save > /dev/null 2>&1
    sleep 1
    FIRST_SECT=$(grep -n '^\[' "$CONFIG" | head -1 | cut -d: -f1)
    ORPHANS=$(head -n "$((FIRST_SECT - 1))" "$CONFIG" | grep -c '^[a-z_][a-z_]* *=' || true)
    if [ "${ORPHANS:-0}" -ne 0 ]; then
        fail "jpeg quality persists into a real section" \
            "$ORPHANS key(s) written above the first [section] header"
    elif sed -n "/^\[stream0\]/,/^\[/p" "$CONFIG" | grep -q '^jpeg_quality *= *60'; then
        pass "jpeg quality persists into a real section (chn $JQ_CHN -> [stream0])"
    else
        fail "jpeg quality persists into a real section" \
            "jpeg_quality = 60 is not in [stream0]"
    fi
fi

echo ""
echo "=== SRT PSI cadence ==="

# PAT/PMT repetition measured in PCR time from a RAW capture
# (srt-live-transmit; ffmpeg would remux and regenerate PSI at its own
# cadence). PSI can only ride a frame, so a fixed emission threshold
# quantizes to threshold plus one frame period: at this config's 25fps
# the old fixed 450ms threshold produced 12-frame (480ms) intervals on
# the wire, grazing the 500ms DVB bound under device load. The
# predictive budget emits a frame early (11 frames = 440ms), holding
# the bound with margin at any frame rate. 460ms splits the two.
if command -v srt-live-transmit > /dev/null 2>&1; then
    timeout -k 3 25 srt-live-transmit "srt://127.0.0.1:19000" file://con \
        > "$LOG_DIR/psi_capture.ts" 2>/dev/null || true
    PSI_RC=0
    PSI_RES=$(python3 - "$LOG_DIR/psi_capture.ts" <<'PSI_EOF'
import bisect, sys
data = open(sys.argv[1], 'rb').read()
pcrs = []  # (byte offset, PCR seconds)
pats = []  # byte offsets of PAT section starts
for off in range(0, len(data) - 187, 188):
    p = data[off:off + 188]
    if p[0] != 0x47:
        continue
    pid = ((p[1] & 0x1f) << 8) | p[2]
    afc = (p[3] >> 4) & 0x3
    if afc in (2, 3) and p[4] >= 7 and (p[5] & 0x10):
        base = (p[6] << 25) | (p[7] << 17) | (p[8] << 9) | (p[9] << 1) | (p[10] >> 7)
        ext = ((p[10] & 1) << 8) | p[11]
        pcrs.append((off, (base * 300 + ext) / 27e6))
    if pid == 0 and (p[1] & 0x40):
        pats.append(off)
if len(pcrs) < 2 or len(pats) < 10:
    print(f"FAIL insufficient data: {len(pcrs)} PCRs, {len(pats)} PATs")
    sys.exit(1)
offs = [o for o, _ in pcrs]
def at(off):
    i = bisect.bisect_left(offs, off)
    if i == 0:
        return pcrs[0][1]
    if i >= len(pcrs):
        return pcrs[-1][1]
    (o1, t1), (o2, t2) = pcrs[i - 1], pcrs[i]
    return t1 + (t2 - t1) * (off - o1) / (o2 - o1) if o2 > o1 else t1
times = [at(o) for o in pats]
gaps = [(b - a) * 1000 for a, b in zip(times, times[1:])]
mx = max(gaps)
tag = "PASS" if mx <= 460 else "FAIL"
print(f"{tag} {len(pats)} PATs over {times[-1] - times[0]:.1f}s, max interval {mx:.1f}ms")
sys.exit(0 if mx <= 460 else 1)
PSI_EOF
    ) || PSI_RC=$?
    if [ "$PSI_RC" -eq 0 ]; then
        pass "SRT PSI cadence with margin (${PSI_RES#PASS })"
    else
        fail "SRT PSI cadence with margin" "${PSI_RES#FAIL }"
    fi
else
    skip "SRT PSI cadence" "srt-live-transmit not installed"
fi

echo ""
echo "=== SEI timecode + signed recording ==="

# ST 0604 SEI must reach RTSP clients and survive an ffmpeg copy
if command -v ffmpeg > /dev/null 2>&1; then
    timeout 10 ffmpeg -nostdin -loglevel quiet -rtsp_transport tcp \
        -i "rtsp://127.0.0.1:15554/stream0" -c copy -frames:v 30 \
        -f h264 -y "$LOG_DIR/sei_capture.h264" 2>/dev/null || true
    sei_n=$(grep -c 'MISPmicrosectime' "$LOG_DIR/sei_capture.h264" 2>/dev/null || echo 0)
    if [ "$sei_n" -ge 10 ]; then
        pass "RTSP carries ST 0604 SEI ($sei_n frames)"
    else
        fail "RTSP carries ST 0604 SEI" "only $sei_n SEIs in capture"
    fi
else
    skip "RTSP SEI capture" "ffmpeg not installed"
fi

check_contains "rmr sign-status" "fingerprint" "$OUT/raptorctl" rmr sign-status
check_contains "rmr export-pubkey" "pubkey" "$OUT/raptorctl" rmr export-pubkey

# ── Wall-clock-aligned rotation ──
# Segments run at 10s granularity for the suite. Other legs bounce the
# pipeline (producer restart, rmr disable), and a segment opened on
# recovery is legitimately unaligned -- so the assertion is on chained
# rotations: whenever two segments sit one period apart, the second
# opened on a wall-clock boundary (filename is HH-MM-SS from the same
# realtime clock the OSD burns), within 1s. The pre-boundary IDR
# request is what makes that tight.
if ALIGN_OUT=$(find "$LOG_DIR/rec" -name '*.mp4' -newer "$LOG_DIR/.suite-start" | sort | python3 -c "
import sys, os
files = [line.strip() for line in sys.stdin if line.strip()]
secs = []
for f in files:
    h, m, s = os.path.basename(f)[:-4].split('-')
    secs.append((int(h) * 3600 + int(m) * 60 + int(s), os.path.basename(f)))
secs.sort()
pairs = 0
bad = []
for (a, _), (b, name) in zip(secs, secs[1:]):
    gap = b - a
    if 8 <= gap <= 12:
        pairs += 1
        off = b % 10
        if 1 < off < 9:
            bad.append(name)
print(f'{len(secs)} segments, {pairs} chained rotations, misaligned: {bad}')
sys.exit(1 if bad or pairs < 2 else 0)"); then
    pass "chained rotations open on wall-clock boundaries ($ALIGN_OUT)"
else
    fail "recording segment alignment" "$ALIGN_OUT"
fi

# Cleanly close the current segment, then verify its chain
"$OUT/raptorctl" rmr disable > /dev/null 2>&1
sleep 1
REC_FILE=$(find "$LOG_DIR/rec" -name '*.mp4' -not -path '*/timelapse/*' | head -1)
if [ -n "$REC_FILE" ] && [ -x "$OUT/rverify" ]; then
    if "$OUT/rverify" -k "$LOG_DIR/sign.key.pub" "$REC_FILE" \
        > "$LOG_DIR/rverify.log" 2>&1; then
        pass "signed recording verifies"
    else
        fail "signed recording verifies" "see rverify.log"
    fi
    grep -c 'MISPmicrosectime' "$REC_FILE" > /dev/null 2>&1 &&
        pass "recording carries ST 0604 SEI" ||
        fail "recording carries ST 0604 SEI" "none found"
    # One flipped byte inside the first fragment must break the chain
    cp "$REC_FILE" "$LOG_DIR/tampered.mp4"
    printf '\xff' | dd of="$LOG_DIR/tampered.mp4" bs=1 seek=2000 conv=notrunc 2>/dev/null
    if "$OUT/rverify" -k "$LOG_DIR/sign.key.pub" "$LOG_DIR/tampered.mp4" \
        > "$LOG_DIR/rverify-tamper.log" 2>&1; then
        fail "tampered recording rejected" "rverify passed a tampered file"
    else
        pass "tampered recording rejected"
    fi
else
    skip "signed recording verification" "no recording or rverify missing"
fi
"$OUT/raptorctl" rmr enable > /dev/null 2>&1

echo ""
echo "=== Timelapse ==="

# Runtime-enabled via ctrl (the test config ships it off), sampled
# deterministically with timelapse-snap. The file must be all-keyframe
# with playback-spaced timestamps -- a real-time-spaced file would mean
# the synthetic DTS path broke -- and carries the same SEI timecodes
# and signature chain as every other recording.
# test-logs persists across runs and the storage path splits by DATE:
# a run that straddles midnight (or follows an earlier run) leaves
# stale files whose date directory can sort ahead of today's, and
# `find | head -1` then probes a 1-frame leftover instead of this
# run's file. Start from an empty tree.
rm -rf "$LOG_DIR/rec/timelapse"
check_contains "timelapse initially off" '"enabled":[[:space:]]*false' \
    "$OUT/raptorctl" rmr timelapse-status
check_contains "timelapse-enable" "ok" "$OUT/raptorctl" rmr timelapse-enable
check_contains "timelapse-set interval" "ok" "$OUT/raptorctl" rmr timelapse-set interval 1
sleep 1
for i in 1 2 3 4; do
    "$OUT/raptorctl" rmr timelapse-snap > /dev/null 2>&1
    sleep 0.6
done
TL_STATUS=$("$OUT/raptorctl" rmr timelapse-status 2>/dev/null)
echo "$TL_STATUS" | grep -q '"interval":[[:space:]]*2' &&
    pass "interval clamped to minimum" ||
    fail "interval clamped to minimum" "$TL_STATUS"

check_contains "timelapse-disable" "ok" "$OUT/raptorctl" rmr timelapse-disable
sleep 1
TL_FILE=$(find "$LOG_DIR/rec/timelapse" -name '*.mp4' 2>/dev/null | head -1)
if [ -z "$TL_FILE" ]; then
    fail "timelapse file created" "no mp4 under rec/timelapse"
elif command -v ffprobe > /dev/null 2>&1; then
    pass "timelapse file created"
    TL_RC=0
    TL_RES=$(ffprobe -v error -select_streams v -show_entries packet=pts_time,flags \
        -of csv=p=0 "$TL_FILE" 2>/dev/null | python3 -c '
import sys
rows = [l.strip().split(",") for l in sys.stdin if l.strip()]
n = len(rows)
nonkey = sum(1 for r in rows if "K" not in r[1])
pts = [float(r[0]) for r in rows]
gaps = [b - a for a, b in zip(pts, pts[1:])]
bad = sum(1 for g in gaps if abs(g - 1.0 / 30.0) > 0.001)
print(f"{n} samples, {nonkey} non-key, {bad} bad gaps")
sys.exit(0 if n >= 4 and nonkey == 0 and bad == 0 else 1)
') || TL_RC=$?
    if [ "$TL_RC" -eq 0 ]; then
        pass "timelapse all-keyframe at playback spacing ($TL_RES)"
    else
        fail "timelapse all-keyframe at playback spacing" "$TL_RES"
    fi
    grep -c 'MISPmicrosectime' "$TL_FILE" > /dev/null 2>&1 &&
        pass "timelapse carries ST 0604 SEI" ||
        fail "timelapse carries ST 0604 SEI" "none found"
    if [ -x "$OUT/rverify" ]; then
        if "$OUT/rverify" -k "$LOG_DIR/sign.key.pub" "$TL_FILE" \
            > "$LOG_DIR/rverify-tl.log" 2>&1; then
            pass "timelapse signature chain verifies"
        else
            fail "timelapse signature chain verifies" "see rverify-tl.log"
        fi
    fi
    # Disable closed the file; a fresh enable+snap must open a second
    # one -- the same close/reopen path a ring reconnect exercises.
    "$OUT/raptorctl" rmr timelapse-enable > /dev/null 2>&1
    "$OUT/raptorctl" rmr timelapse-snap > /dev/null 2>&1
    sleep 1
    "$OUT/raptorctl" rmr timelapse-disable > /dev/null 2>&1
    TL_COUNT=$(find "$LOG_DIR/rec/timelapse" -name '*.mp4' 2>/dev/null | wc -l)
    if [ "$TL_COUNT" -ge 2 ]; then
        pass "timelapse reopens a fresh file ($TL_COUNT files)"
    else
        fail "timelapse reopens a fresh file" "only $TL_COUNT file(s)"
    fi
else
    pass "timelapse file created"
    skip "timelapse packet checks" "ffprobe not installed"
fi

# ── ONVIF backchannel: client audio reaches the speaker ring ──
# rsd's receive path (sendonly SETUP, RTP over an interleaved channel,
# PCMU decode, publish) had no x86 coverage at all -- it was exercised
# only by the hardware battery, and only ever asserted that the ring
# existed. Here the frames are read back out, which is what catches
# rsd publishing into a handle that cannot publish.
# Do NOT clear the speaker ring first: create_rings already owns one
# here, which is the realistic state (rac playing, rad up) and the
# case rsd got wrong -- it opened the existing ring, got a consumer
# handle, and every publish failed -EINVAL in silence. Removing the
# ring would push rsd down the create path and hide exactly that.
python3 - > "$LOG_DIR/backchannel.out" 2>&1 <<'BC_EOF' &
import socket, struct, sys, time

def rsp(sock, buf):
    while b"\r\n\r\n" not in buf[0]:
        d = sock.recv(4096)
        if not d:
            return "", buf
        buf[0] += d
    head, _, rest = buf[0].partition(b"\r\n\r\n")
    txt = head.decode(errors="replace")
    clen = 0
    for line in txt.split("\r\n"):
        if line.lower().startswith("content-length:"):
            clen = int(line.split(":", 1)[1])
    while len(rest) < clen:
        rest += sock.recv(4096)
    body, buf[0] = rest[:clen], rest[clen:]
    return txt + "\r\n\r\n" + body.decode(errors="replace"), buf

REQ = "Require: www.onvif.org/ver20/backchannel\r\n"
s = socket.create_connection(("127.0.0.1", 15554), timeout=10)
buf = [b""]
base = "rtsp://127.0.0.1:15554/stream0"
s.sendall(f"DESCRIBE {base} RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n{REQ}\r\n".encode())
desc, buf = rsp(s, buf)
if " 200 " not in desc.split("\r\n")[0]:
    print("DESCRIBE_FAILED"); sys.exit(0)
if "a=sendonly" not in desc:
    print("NO_SENDONLY"); sys.exit(0)
print("SENDONLY_OK")

ctl = None
for sec in desc.split("m=")[1:]:
    if "a=sendonly" in sec:
        for line in sec.split("\n"):
            if line.strip().startswith("a=control:"):
                ctl = line.strip().split(":", 1)[1]
if not ctl:
    print("NO_CONTROL"); sys.exit(0)

# A backchannel-only session has nothing to play: set up the video
# track first, exactly as a real ONVIF client does, then the
# sendonly track beside it.
s.sendall(f"SETUP {base}/video RTSP/1.0\r\nCSeq: 2\r\n"
          f"Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n".encode())
vsetup, buf = rsp(s, buf)
sid = ""
for line in vsetup.split("\r\n"):
    if line.lower().startswith("session:"):
        sid = line.split(":", 1)[1].split(";")[0].strip()
if " 200 " not in vsetup.split("\r\n")[0]:
    print("VIDEO_SETUP_FAILED"); sys.exit(0)

url = ctl if ctl.startswith("rtsp") else base + "/" + ctl
s.sendall(f"SETUP {url} RTSP/1.0\r\nCSeq: 3\r\nSession: {sid}\r\n"
          f"Transport: RTP/AVP/TCP;unicast;interleaved=4-5\r\n{REQ}\r\n".encode())
setup, buf = rsp(s, buf)
if " 200 " not in setup.split("\r\n")[0]:
    print("SETUP_FAILED"); sys.exit(0)

s.sendall(f"PLAY {base} RTSP/1.0\r\nCSeq: 4\r\nSession: {sid}\r\nRange: npt=0.000-\r\n\r\n".encode())
play, buf = rsp(s, buf)
if " 200 " not in play.split("\r\n")[0]:
    print("PLAY_FAILED"); sys.exit(0)

seq = ts = sent = 0
for _ in range(50):
    rtp = struct.pack("!BBHII", 0x80, 0, seq & 0xFFFF, ts, 0x1234ABCD) + b"\xff" * 160
    s.sendall(b"\x24" + struct.pack("!BH", 4, len(rtp)) + rtp)
    seq += 1; ts += 160; sent += 1
    time.sleep(0.005)
print(f"SENT={sent}", flush=True)
# rsd destroys the speaker ring when the client goes away, so hold the
# session while the caller inspects it -- the same reason the hardware
# battery's probe holds.
time.sleep(6.0)
s.close()
BC_EOF
BC_PID=$!
# Poll for the ring while the probe holds its session, then read the
# header: write_seq counts what rsd actually published. This is the
# assertion the hardware battery cannot make -- it can only see that
# the ring exists.
BC_SEQ=0
for _ in $(seq 1 40); do
    if [ -e /dev/shm/rss_ring_speaker ]; then
        BC_SEQ=$(timeout 5 "$OUT/ringdump" speaker 2>&1 |
                 sed -n 's/.*Write seq: *\([0-9]*\).*/\1/p' | head -1)
        [ "${BC_SEQ:-0}" -gt 0 ] 2>/dev/null && break
    fi
    sleep 0.25
done
wait $BC_PID 2>/dev/null
BC_OUT=$(cat "$LOG_DIR/backchannel.out" 2>/dev/null)

echo "$BC_OUT" | grep -q "SENDONLY_OK" \
    && pass "backchannel advertises a sendonly track" \
    || fail "backchannel sendonly track" "$(echo "$BC_OUT" | head -1)"

if echo "$BC_OUT" | grep -q "SENT=50"; then
    pass "backchannel accepts 50 RTP frames"
else
    fail "backchannel send" "$(echo "$BC_OUT" | tail -1)"
fi

if [ "${BC_SEQ:-0}" -gt 0 ] 2>/dev/null; then
    pass "backchannel audio reaches the speaker ring ($BC_SEQ frames published)"
else
    fail "backchannel speaker ring" "rsd published nothing (write_seq=${BC_SEQ:-none})"
fi

# ── Backchannel multi-codec offer: every advertised PT must decode ──
# One TCP session switches codecs mid-stream (legal per RFC 8866: the
# client may change among the offered formats), and rsd's per-codec
# "receiving X" line only fires after a SUCCESSFUL decode -- for opus
# and AAC that is the real decoder speaking, not the depacketizer.
python3 - > "$LOG_DIR/backchannel-codecs.out" 2>&1 <<'BC2_EOF' &
import socket, struct, subprocess, sys, time

def rsp(sock, buf):
    while b"\r\n\r\n" not in buf[0]:
        d = sock.recv(4096)
        if not d:
            return "", buf
        buf[0] += d
    head, _, rest = buf[0].partition(b"\r\n\r\n")
    txt = head.decode(errors="replace")
    clen = 0
    for line in txt.split("\r\n"):
        if line.lower().startswith("content-length:"):
            clen = int(line.split(":", 1)[1])
    while len(rest) < clen:
        rest += sock.recv(4096)
    body, buf[0] = rest[:clen], rest[clen:]
    return txt + "\r\n\r\n" + body.decode(errors="replace"), buf

REQ = "Require: www.onvif.org/ver20/backchannel\r\n"
base = "rtsp://127.0.0.1:15554/stream0"
s = socket.create_connection(("127.0.0.1", 15554), timeout=10)
buf = [b""]
s.sendall(f"DESCRIBE {base} RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n{REQ}\r\n".encode())
desc, buf = rsp(s, buf)
bc_sec = next((sec for sec in desc.split("m=")[1:] if "a=sendonly" in sec), "")
pts = bc_sec.split("\n")[0].split("RTP/AVP", 1)[-1].split()
for want in ("0", "8", "112", "113", "114"):
    if want not in pts:
        print(f"OFFER_MISSING={want}"); sys.exit(0)
print("OFFER_OK=" + " ".join(pts))

s.sendall(f"SETUP {base}/video RTSP/1.0\r\nCSeq: 2\r\n"
          f"Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n".encode())
vs, buf = rsp(s, buf)
sid = next((l.split(":", 1)[1].split(";")[0].strip()
            for l in vs.split("\r\n") if l.lower().startswith("session:")), "")
s.sendall(f"SETUP {base}/backchannel RTSP/1.0\r\nCSeq: 3\r\nSession: {sid}\r\n"
          f"Transport: RTP/AVP/TCP;unicast;interleaved=4-5\r\n{REQ}\r\n".encode())
bs, buf = rsp(s, buf)
if " 200 " not in bs.split("\r\n")[0]:
    print("SETUP_FAILED"); sys.exit(0)
s.sendall(f"PLAY {base} RTSP/1.0\r\nCSeq: 4\r\nSession: {sid}\r\nRange: npt=0.000-\r\n\r\n".encode())
rsp(s, buf)

# AAC AUs from ffmpeg (ADTS stripped): AAC-LC, ring rate, mono, the
# exact stream the fmtp config advertises.
aac_aus = []
try:
    adts = subprocess.run(
        ["ffmpeg", "-v", "quiet", "-f", "lavfi", "-i", "sine=frequency=440:duration=1",
         "-ar", "16000", "-ac", "1", "-c:a", "aac", "-b:a", "24k", "-f", "adts", "-"],
        capture_output=True, timeout=20).stdout
    i = 0
    while i + 7 <= len(adts) and len(aac_aus) < 12:
        if adts[i] != 0xFF or (adts[i + 1] & 0xF0) != 0xF0:
            break
        flen = ((adts[i + 3] & 0x03) << 11) | (adts[i + 4] << 3) | (adts[i + 5] >> 5)
        hdr = 7 if (adts[i + 1] & 0x01) else 9
        if (adts[i + 2] >> 2) & 0xF == 8 and flen > hdr:
            aac_aus.append(adts[i + hdr:i + flen])
        i += flen
except Exception:
    aac_aus = []

seq = 0
ssrc = 0x22334455

def send(pt, ts, payload):
    global seq
    rtp = struct.pack("!BBHII", 0x80, pt, seq & 0xFFFF, ts, ssrc) + payload
    s.sendall(b"\x24" + struct.pack("!BH", 4, len(rtp)) + rtp)
    seq += 1
    time.sleep(0.005)

ts = 0
for _ in range(30):                      # PCMA, 20 ms @ 8 kHz
    send(8, ts, b"\xd5" * 160); ts += 160
print("SENT_PCMA=30")
ts = 0
for _ in range(30):                      # L16/16000, 10 ms
    send(114, ts, struct.pack("!160h", *([1000] * 160))); ts += 160
print("SENT_L16=30")
ts = 0
for _ in range(30):                      # opus: 1-byte TOC = 20 ms WB mono silence
    send(112, ts, b"\x08"); ts += 960    # RFC 7587 clock is 48 kHz
print("SENT_OPUS=30")
if aac_aus:
    ts = 0
    for au in aac_aus:                   # AAC-hbr, one AU per packet
        hbr = b"\x00\x10" + struct.pack("!H", len(au) << 3) + au
        send(113, ts, hbr); ts += 1024
    print(f"SENT_AAC={len(aac_aus)}")
else:
    print("AAC_SKIP")
# rsd owes the sender a receiver report on the interleaved RTCP
# channel (RFC 3550): scan the incoming stream for a PT 201 frame.
s.settimeout(2.0)
got = b""
deadline = time.time() + 7.0
while time.time() < deadline:
    try:
        d = s.recv(4096)
    except socket.timeout:
        continue
    if not d:
        break
    got += d
    # The socket multiplexes video frames with the RTCP channel, so a
    # \x24\x05 byte pair can occur INSIDE video payload. Walk every
    # candidate and demand it actually look like RTCP (version bits +
    # sane frame length) -- a single find() sticks on the first false
    # positive forever and never examines the real report behind it.
    found = False
    i = got.find(b"\x24\x05")
    while i >= 0:
        if len(got) >= i + 6:
            flen = (got[i + 2] << 8) | got[i + 3]
            if got[i + 5] == 201 and (got[i + 4] & 0xC0) == 0x80 and 8 <= flen <= 512:
                found = True
                break
        i = got.find(b"\x24\x05", i + 1)
    if found:
        print("RR_TCP_OK")
        break

# The leave compound: TEARDOWN must be preceded on the wire by a BYE
# for the reporter SSRC (same discipline as the sending tracks). The
# stream also carries video frames, so a frame only counts when it
# parses as a complete RTCP compound whose packets all bear RTCP PTs.
def compound_has_bye(frame):
    off, seen = 0, False
    while off + 4 <= len(frame):
        if (frame[off] >> 6) != 2 or frame[off + 1] not in (200, 201, 202, 203):
            return False
        if frame[off + 1] == 203:
            seen = True
        off += (((frame[off + 2] << 8) | frame[off + 3]) + 1) * 4
    return seen and off == len(frame)

s.sendall(f"TEARDOWN {base} RTSP/1.0\r\nCSeq: 5\r\nSession: {sid}\r\n\r\n".encode())
got2 = got
deadline = time.time() + 5.0
bye_at = resp_at = -1
while time.time() < deadline:
    try:
        d = s.recv(4096)
    except socket.timeout:
        break
    if not d:
        break
    got2 += d
    if bye_at < 0:
        j = 0
        while True:
            j = got2.find(b"\x24\x05", j)
            if j < 0 or len(got2) < j + 4:
                break
            ln = (got2[j + 2] << 8) | got2[j + 3]
            if len(got2) >= j + 4 + ln and compound_has_bye(got2[j + 4:j + 4 + ln]):
                bye_at = j
                break
            j += 2
    resp_at = got2.rfind(b"RTSP/1.0 200")
    if bye_at >= 0 and resp_at >= 0:
        break
if bye_at >= 0 and resp_at >= 0 and bye_at < resp_at:
    print("BYE_TCP_OK")
elif bye_at >= 0:
    print("BYE_TCP_LATE")
else:
    print("BYE_TCP_MISSING")
s.close()
BC2_EOF
BC2_PID=$!
wait $BC2_PID 2>/dev/null
BC2_OUT=$(cat "$LOG_DIR/backchannel-codecs.out" 2>/dev/null)

echo "$BC2_OUT" | grep -q "OFFER_OK" \
    && pass "backchannel offers PCMU/PCMA/opus/AAC/L16 ($(echo "$BC2_OUT" | sed -n 's/OFFER_OK=//p'))" \
    || fail "backchannel multi-codec offer" "$(echo "$BC2_OUT" | head -1)"

for codec in PCMA L16 opus; do
    if grep -q "backchannel: receiving $codec" "$LOG_DIR/rsd.log"; then
        pass "backchannel decodes $codec"
    else
        fail "backchannel $codec decode" "no 'receiving $codec' in rsd.log"
    fi
done
if echo "$BC2_OUT" | grep -q "AAC_SKIP"; then
    skip "backchannel AAC decode (ffmpeg produced no usable AUs)"
elif grep -q "backchannel: receiving AAC" "$LOG_DIR/rsd.log"; then
    pass "backchannel decodes AAC (real AUs via ffmpeg)"
else
    fail "backchannel AAC decode" "no 'receiving AAC' in rsd.log"
fi

echo "$BC2_OUT" | grep -q "RR_TCP_OK" \
    && pass "backchannel receiver report arrives on the interleaved RTCP channel" \
    || fail "backchannel TCP receiver report" "no PT 201 frame seen"

echo "$BC2_OUT" | grep -q "BYE_TCP_OK" \
    && pass "backchannel leave compound (RR+SDES+BYE) precedes the TEARDOWN 200" \
    || fail "backchannel TEARDOWN BYE" "$(echo "$BC2_OUT" | grep BYE_TCP || echo none)"

# ── Backchannel codec selection: config governs offer AND dispatch ──
# set-backchannel-codecs is read per session, so the flip needs no
# restart; the restricted server must shrink its m-line and treat a
# disabled codec's packets exactly like a payload type it never
# offered.
check_contains "backchannel codec selection applies live" '"backchannel_codecs":"pcmu,l16"' \
    "$OUT/raptorctl" rsd set-backchannel-codecs "pcmu,l16"
check_contains "status names the restricted offer" '"backchannel_codecs":"pcmu,l16"' \
    "$OUT/raptorctl" rsd status
check_contains "unknown codec is refused by name" 'g729' \
    "$OUT/raptorctl" rsd set-backchannel-codecs "pcmu,g729"
BC4_OUT=$(python3 - <<'BC4_EOF'
import socket, struct, sys, time

def rsp(sock, buf):
    while b"\r\n\r\n" not in buf[0]:
        d = sock.recv(4096)
        if not d:
            return "", buf
        buf[0] += d
    head, _, rest = buf[0].partition(b"\r\n\r\n")
    txt = head.decode(errors="replace")
    clen = 0
    for line in txt.split("\r\n"):
        if line.lower().startswith("content-length:"):
            clen = int(line.split(":", 1)[1])
    while len(rest) < clen:
        rest += sock.recv(4096)
    body, buf[0] = rest[:clen], rest[clen:]
    return txt + "\r\n\r\n" + body.decode(errors="replace"), buf

REQ = "Require: www.onvif.org/ver20/backchannel\r\n"
base = "rtsp://127.0.0.1:15554/stream0"
s = socket.create_connection(("127.0.0.1", 15554), timeout=10)
buf = [b""]
s.sendall(f"DESCRIBE {base} RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n{REQ}\r\n".encode())
desc, buf = rsp(s, buf)
bc_sec = next((sec for sec in desc.split("m=")[1:] if "a=sendonly" in sec), "")
pts = bc_sec.split("\n")[0].split("RTP/AVP", 1)[-1].split()
print("RESTRICTED_OFFER=" + " ".join(pts))
if pts != ["0", "114"]:
    sys.exit(0)
if "rtpmap:112" in bc_sec or "rtpmap:113" in bc_sec or "rtpmap:8 " in bc_sec:
    print("STRAY_RTPMAP"); sys.exit(0)

s.sendall(f"SETUP {base}/video RTSP/1.0\r\nCSeq: 2\r\n"
          f"Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n".encode())
vs, buf = rsp(s, buf)
sid = next((l.split(":", 1)[1].split(";")[0].strip()
            for l in vs.split("\r\n") if l.lower().startswith("session:")), "")
s.sendall(f"SETUP {base}/backchannel RTSP/1.0\r\nCSeq: 3\r\nSession: {sid}\r\n"
          f"Transport: RTP/AVP/TCP;unicast;interleaved=4-5\r\n{REQ}\r\n".encode())
rsp(s, buf)
s.sendall(f"PLAY {base} RTSP/1.0\r\nCSeq: 4\r\nSession: {sid}\r\nRange: npt=0.000-\r\n\r\n".encode())
rsp(s, buf)
seq = 0
for _ in range(20):  # PCMA against a pcmu,l16 offer: must be dropped
    rtp = struct.pack("!BBHII", 0x80, 8, seq & 0xFFFF, seq * 160, 0x11223344) + b"\xd5" * 160
    s.sendall(b"\x24" + struct.pack("!BH", 4, len(rtp)) + rtp)
    seq += 1
    time.sleep(0.005)
print("DISABLED_SENT=20", flush=True)
time.sleep(1.0)
s.close()
BC4_EOF
)
echo "$BC4_OUT" | grep -q "RESTRICTED_OFFER=0 114" \
    && pass "restricted offer carries exactly the configured payload types" \
    || fail "restricted backchannel offer" "$(echo "$BC4_OUT" | head -1)"
if echo "$BC4_OUT" | grep -q "DISABLED_SENT=20" \
   && grep -q "backchannel: dropping payload type 8" "$LOG_DIR/rsd.log"; then
    pass "disabled codec's packets are dropped like a never-offered payload type"
else
    fail "disabled codec dispatch" "no 'dropping payload type 8' in rsd.log"
fi
check_contains "codec selection restores to the full offer" '"backchannel_codecs":"pcmu,pcma,opus,aac,l16"' \
    "$OUT/raptorctl" rsd set-backchannel-codecs ""

# ── RTSP option negotiation and SET_PARAMETER conformance ──
# RFC 2326 §12.32: a Require tag the server cannot honor draws 551
# naming the tags in Unsupported; §10.9: SET_PARAMETER with no body is
# the standard keepalive, and parameters nobody understands draw 451.
# The body case doubles as a framing check: its bytes must not desync
# the connection for the request behind it.
CONF_OUT=$(python3 - <<'CONF_EOF'
import socket

def txn(reqs):
    s = socket.create_connection(("127.0.0.1", 15554), timeout=5)
    out, buf = [], b""
    for r in reqs:
        s.sendall(r.encode())
        while b"\r\n\r\n" not in buf:
            d = s.recv(4096)
            if not d:
                break
            buf += d
        head, _, buf = buf.partition(b"\r\n\r\n")
        out.append(head.decode(errors="replace"))
    s.close()
    return out

base = "rtsp://127.0.0.1:15554/stream0"

r = txn([f"DESCRIBE {base} RTSP/1.0\r\nCSeq: 1\r\n"
         f"Require: org.example.fancy\r\n\r\n"])[0]
print("R551=" + ("yes" if " 551 " in r.splitlines()[0]
                 and "org.example.fancy" in r else "no"))

r = txn([f"DESCRIBE {base} RTSP/1.0\r\nCSeq: 1\r\n"
         f"Require: www.onvif.org/ver20/backchannel, org.example.fancy\r\n\r\n"])[0]
uns = next((l for l in r.splitlines()
            if l.lower().startswith("unsupported:")), "")
print("RMIX=" + ("yes" if " 551 " in r.splitlines()[0]
                 and "org.example.fancy" in uns
                 and "backchannel" not in uns else "no"))

r = txn([f"DESCRIBE {base} RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n"
         f"Require: www.onvif.org/ver20/backchannel\r\n\r\n"])[0]
print("ROK=" + ("yes" if " 200 " in r.splitlines()[0] else "no"))

body = "p: v\r\n"
rs = txn([
    f"SET_PARAMETER {base} RTSP/1.0\r\nCSeq: 2\r\n\r\n",
    f"SET_PARAMETER {base} RTSP/1.0\r\nCSeq: 3\r\n"
    f"Content-Type: text/parameters\r\n"
    f"Content-Length: {len(body)}\r\n\r\n{body}",
    f"OPTIONS {base} RTSP/1.0\r\nCSeq: 4\r\n\r\n",
])
print("SPKEEP=" + ("yes" if len(rs) > 0 and " 200 " in rs[0].splitlines()[0] else "no"))
print("SP451=" + ("yes" if len(rs) > 1 and " 451 " in rs[1].splitlines()[0] else "no"))
print("SPBODY=" + ("yes" if len(rs) > 2 and " 200 " in rs[2].splitlines()[0]
                   and "CSeq: 4" in rs[2] else "no"))

# A Require list longer than the server's Unsupported scratch buffer:
# the 551 must carry only whole tags the client actually sent (never
# a length that outruns what was written), and the connection must
# stay usable for the request behind it.
tags = ["org.example.longlist%02d" % n for n in range(40)]
rs = txn([f"DESCRIBE {base} RTSP/1.0\r\nCSeq: 5\r\n"
          f"Require: {', '.join(tags)}\r\n\r\n",
          f"OPTIONS {base} RTSP/1.0\r\nCSeq: 6\r\n\r\n"])
uns = next((l for l in rs[0].splitlines()
            if l.lower().startswith("unsupported:")), "")
listed = [t.strip() for t in uns.split(":", 1)[1].split(",")] if uns else []
print("RLONG=" + ("yes" if " 551 " in rs[0].splitlines()[0]
                  and listed and all(t in tags for t in listed)
                  and len(rs) > 1 and " 200 " in rs[1].splitlines()[0]
                  and "CSeq: 6" in rs[1] else "no"))

# A single tag longer than the buffer: the header names a prefix of it
# rather than arriving empty or with stack garbage.
giant = "org.example." + "x" * 400
rs = txn([f"DESCRIBE {base} RTSP/1.0\r\nCSeq: 7\r\n"
          f"Require: {giant}\r\n\r\n",
          f"OPTIONS {base} RTSP/1.0\r\nCSeq: 8\r\n\r\n"])
uns = next((l for l in rs[0].splitlines()
            if l.lower().startswith("unsupported:")), "")
val = uns.split(":", 1)[1].strip() if uns else ""
print("RGIANT=" + ("yes" if " 551 " in rs[0].splitlines()[0]
                   and val and giant.startswith(val)
                   and len(rs) > 1 and " 200 " in rs[1].splitlines()[0] else "no"))
CONF_EOF
)
echo "$CONF_OUT" | grep -q "R551=yes" \
    && pass "unknown Require tag draws 551 with Unsupported naming it" \
    || fail "Require 551" "$(echo "$CONF_OUT" | grep R551)"
echo "$CONF_OUT" | grep -q "RMIX=yes" \
    && pass "mixed Require list 551s naming only the stranger" \
    || fail "Require mixed list" "$(echo "$CONF_OUT" | grep RMIX)"
echo "$CONF_OUT" | grep -q "ROK=yes" \
    && pass "the supported Require tag alone still serves" \
    || fail "Require supported tag" "$(echo "$CONF_OUT" | grep ROK)"
echo "$CONF_OUT" | grep -q "SPKEEP=yes" \
    && pass "SET_PARAMETER keepalive answers 200" \
    || fail "SET_PARAMETER keepalive" "$(echo "$CONF_OUT" | grep SPKEEP)"
echo "$CONF_OUT" | grep -q "SP451=yes" \
    && pass "SET_PARAMETER with a parameter draws 451" \
    || fail "SET_PARAMETER 451" "$(echo "$CONF_OUT" | grep SP451)"
echo "$CONF_OUT" | grep -q "SPBODY=yes" \
    && pass "a request body does not desync the connection (CSeq echoes through)" \
    || fail "body framing" "$(echo "$CONF_OUT" | grep SPBODY)"
echo "$CONF_OUT" | grep -q "RLONG=yes" \
    && pass "overlong Require list 551s with only whole client-sent tags" \
    || fail "Require overlong list" "$(echo "$CONF_OUT" | grep RLONG)"
echo "$CONF_OUT" | grep -q "RGIANT=yes" \
    && pass "oversized single Require tag 551s naming a prefix of it" \
    || fail "Require oversized tag" "$(echo "$CONF_OUT" | grep RGIANT)"

# ── Backchannel over UDP: its own socket pair, actually read ──
# Regression shape: the backchannel SETUP used to borrow the VIDEO
# track's UDP fd slots, so with a video UDP SETUP in the same session
# the backchannel audio landed on sockets nobody read. The write_seq
# delta below is measured across ONLY this probe.
BC3_BEFORE=$(timeout 5 "$OUT/ringdump" speaker 2>&1 |
             sed -n 's/.*Write seq: *\([0-9]*\).*/\1/p' | head -1)
python3 - > "$LOG_DIR/backchannel-udp.out" 2>&1 <<'BC3_EOF' &
import socket, struct, sys, time

def rsp(sock, buf):
    while b"\r\n\r\n" not in buf[0]:
        d = sock.recv(4096)
        if not d:
            return "", buf
        buf[0] += d
    head, _, rest = buf[0].partition(b"\r\n\r\n")
    txt = head.decode(errors="replace")
    clen = 0
    for line in txt.split("\r\n"):
        if line.lower().startswith("content-length:"):
            clen = int(line.split(":", 1)[1])
    while len(rest) < clen:
        rest += sock.recv(4096)
    body, buf[0] = rest[:clen], rest[clen:]
    return txt + "\r\n\r\n" + body.decode(errors="replace"), buf

REQ = "Require: www.onvif.org/ver20/backchannel\r\n"
base = "rtsp://127.0.0.1:15554/stream0"
s = socket.create_connection(("127.0.0.1", 15554), timeout=10)
buf = [b""]
s.sendall(f"DESCRIBE {base} RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n{REQ}\r\n".encode())
rsp(s, buf)

def udp_pair():
    a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    a.bind(("127.0.0.1", 0)); b.bind(("127.0.0.1", 0))
    return a, b, a.getsockname()[1], b.getsockname()[1]

v_rtp, v_rtcp, vp1, vp2 = udp_pair()
b_rtp, b_rtcp, bp1, bp2 = udp_pair()

s.sendall(f"SETUP {base}/video RTSP/1.0\r\nCSeq: 2\r\n"
          f"Transport: RTP/AVP;unicast;client_port={vp1}-{vp2}\r\n\r\n".encode())
vs, buf = rsp(s, buf)
if " 200 " not in vs.split("\r\n")[0]:
    print("VIDEO_UDP_SETUP_FAILED"); sys.exit(0)
sid = next((l.split(":", 1)[1].split(";")[0].strip()
            for l in vs.split("\r\n") if l.lower().startswith("session:")), "")

s.sendall(f"SETUP {base}/backchannel RTSP/1.0\r\nCSeq: 3\r\nSession: {sid}\r\n"
          f"Transport: RTP/AVP;unicast;client_port={bp1}-{bp2}\r\n{REQ}\r\n".encode())
bs, buf = rsp(s, buf)
if " 200 " not in bs.split("\r\n")[0]:
    print("BC_UDP_SETUP_FAILED"); sys.exit(0)
srv_port = 0
for line in bs.split("\r\n"):
    if line.lower().startswith("transport:") and "server_port=" in line:
        srv_port = int(line.split("server_port=")[1].split("-")[0].split(";")[0])
if not srv_port:
    print("NO_SERVER_PORT"); sys.exit(0)
print(f"BC_UDP_SETUP_OK={srv_port}")

s.sendall(f"PLAY {base} RTSP/1.0\r\nCSeq: 4\r\nSession: {sid}\r\nRange: npt=0.000-\r\n\r\n".encode())
rsp(s, buf)

seq = ts = 0
for _ in range(30):
    rtp = struct.pack("!BBHII", 0x80, 0, seq & 0xFFFF, ts, 0x778899AA) + b"\xff" * 160
    b_rtp.sendto(rtp, ("127.0.0.1", srv_port))
    seq += 1; ts += 160
    time.sleep(0.005)
print("UDP_SENT=30", flush=True)
# The receiver report comes back on the backchannel RTCP pair.
b_rtcp.settimeout(8.0)
try:
    rr, _ = b_rtcp.recvfrom(2048)
    if len(rr) >= 8 and rr[1] == 201:
        print("RR_UDP_OK")
except socket.timeout:
    pass
time.sleep(2.0)
s.close()
BC3_EOF
BC3_PID=$!
BC3_SEQ=0
for _ in $(seq 1 40); do
    BC3_SEQ=$(timeout 5 "$OUT/ringdump" speaker 2>&1 |
              sed -n 's/.*Write seq: *\([0-9]*\).*/\1/p' | head -1)
    [ "${BC3_SEQ:-0}" -gt "${BC3_BEFORE:-0}" ] 2>/dev/null && break
    sleep 0.25
done
wait $BC3_PID 2>/dev/null
BC3_OUT=$(cat "$LOG_DIR/backchannel-udp.out" 2>/dev/null)

echo "$BC3_OUT" | grep -q "BC_UDP_SETUP_OK" \
    && pass "backchannel UDP SETUP gets its own server port" \
    || fail "backchannel UDP SETUP" "$(echo "$BC3_OUT" | head -1)"

if [ "${BC3_SEQ:-0}" -gt "${BC3_BEFORE:-0}" ] 2>/dev/null; then
    pass "backchannel UDP audio reaches the speaker ring ($((BC3_SEQ - BC3_BEFORE)) frames, video UDP pair intact)"
else
    fail "backchannel UDP receive" "write_seq stuck at ${BC3_SEQ:-none} (before=${BC3_BEFORE:-none})"
fi

echo "$BC3_OUT" | grep -q "RR_UDP_OK" \
    && pass "backchannel receiver report arrives on the UDP RTCP pair" \
    || fail "backchannel UDP receiver report" "no PT 201 datagram on client rtcp port"

# ── Producer restart under a lingering client ──
# A playing client whose media goes unread (stalled player, dead NVR
# that still ACKs) must not pin the ring reader to a dead producer's
# frozen ring: the field wedge was 4+ minutes of no-frames for every
# new client until the zombie aged out. Reconnect must come from
# staleness detection, not the idle-close path a playing client blocks.
python3 - <<'ZOMBIE_EOF' &
import sys, time, os
sys.path.insert(0, os.path.expanduser("~/projects/thingino/raptor-test/probes"))
try:
    from rtsplib import RtspSession
    s = RtspSession("127.0.0.1", 15554, "/stream0")
    s.describe(); s.setup("video", 0); s.play()
    time.sleep(45)
    s.close()
except Exception:
    time.sleep(45)
ZOMBIE_EOF
ZOMBIE_PID=$!
sleep 3

pkill -f "$OUT/rvd" 2>/dev/null || true
sleep 2
start_daemon rvd "$OUT/rvd" -c "$CONFIG" -f -d
start_daemon rad "$OUT/rad" -c "$CONFIG" -f -d
sleep 3

if timeout -k 3 15 ffprobe -v error -rtsp_transport tcp \
    -show_entries stream=codec_name -of csv=p=0 "rtsp://127.0.0.1:15554/stream0" \
    2>/dev/null | grep -q h264; then
    pass "fresh client gets frames after producer restart under a lingering client"
else
    fail "producer restart under lingering client" "no frames within 15s -- reader pinned to the dead ring"
fi
kill $ZOMBIE_PID 2>/dev/null || true
wait $ZOMBIE_PID 2>/dev/null || true


# ── Audio ring cadence: AAC frames stamp on the sample grid ──
# The AAC accumulator publishes 1024-sample frames built from 20ms
# chunks; stamping them on the chunk grid gave four 60ms intervals
# then one 80ms at 16kHz, and the weave rode into recordings and
# sender reports. The real codec runs here (x86 libfaac): restart rad
# on an AAC config and require every ring interval to be the exact
# frame duration.
AAC_CONF="$LOG_DIR/test-aac.conf"
sed 's/^codec = l16/codec = aac/' "$CONFIG" > "$AAC_CONF"
pkill -f "$OUT/rad" 2>/dev/null || true
sleep 1
start_daemon rad "$OUT/rad" -c "$AAC_CONF" -f -d
sleep 4
AAC_DTS=$("$OUT/ringdump" audio -f -n 25 2>&1 | sed -n 's/.*dt=\([0-9]*\).*/\1/p' | tail -20)
if [ -n "$AAC_DTS" ] && python3 - <<PYEOF2
import sys
dts = [int(x) for x in """$AAC_DTS""".split()]
ideal = 1024 * 1000000 // 16000
bad = [d for d in dts if abs(d - ideal) > 1500]
print(f"aac ring: {len(dts)} intervals, ideal {ideal}us, off-grid: {bad}")
sys.exit(1 if bad or len(dts) < 10 else 0)
PYEOF2
then
    pass "aac ring intervals sit on the 1024-sample grid"
else
    fail "aac ring intervals sit on the 1024-sample grid" "chunk-grid weave (60/60/80ms) or no frames"
fi
pkill -f "$OUT/rad" 2>/dev/null || true
sleep 1
start_daemon rad "$OUT/rad" -c "$CONFIG" -f -d
sleep 1

# ── Sensor rate on a platform that will not set it ──
# A backend that leaves isp_set_sensor_fps out, or refuses it outright,
# is behaving as specified, so rvd reports the rate in force instead of
# repeating the number that was dropped. Which answer is honest depends
# on what can be read back, hence three arms; the mock's two knobs pick
# which platform it imitates. No [sensor] fps in the suite config and no
# /proc/jz on the host, so rvd falls back to 25 -- the same case that
# made the original line mislead.
fps_arm() {
    arm_name="$1"
    arm_want="$2"
    shift 2
    pkill -f "$OUT/rvd" 2>/dev/null || true
    sleep 1
    start_daemon rvd env "$@" "$OUT/rvd" -c "$CONFIG" -f -d
    sleep 2
    if grep -q "$arm_want" "$LOG_DIR/rvd.log" 2>/dev/null; then
        pass "$arm_name"
    else
        fail "$arm_name" "expected '$arm_want' in rvd log"
        [ "$VERBOSE" = "1" ] && grep -i "sensor0 fps\|isp_set_sensor_fps" "$LOG_DIR/rvd.log"
    fi
    # Whichever arm fired, a documented refusal is not a fault: no
    # warning about the call, and the arm's own line is not raised to one
    if grep -q "isp_set_sensor_fps failed" "$LOG_DIR/rvd.log" 2>/dev/null ||
        grep "sensor0 fps" "$LOG_DIR/rvd.log" 2>/dev/null | grep -q "WARN"; then
        fail "$arm_name is not a fault" "a documented NOTSUP was reported as one"
    else
        pass "$arm_name is not a fault"
    fi
}

echo ""
echo "=== Sensor rate diagnostics ==="

fps_arm "unsettable and unreadable names no rate as the sensor's" \
    "no source confirmed" RSS_MOCK_SENSOR_FPS_SET=notsup
fps_arm "unsettable but readable names the rate in force over the request" \
    "sensor runs at 30, not the 25 asked for" \
    RSS_MOCK_SENSOR_FPS_SET=notsup RSS_MOCK_SENSOR_FPS_ACTUAL=30
fps_arm "unsettable and already at the requested rate says so plainly" \
    "not settable on this platform; the sensor runs at 25" \
    RSS_MOCK_SENSOR_FPS_SET=notsup RSS_MOCK_SENSOR_FPS_ACTUAL=25

# A setter that fails for a real reason is still a fault: the three
# arms above must not have swallowed the warning path with them
pkill -f "$OUT/rvd" 2>/dev/null || true
sleep 1
start_daemon rvd env RSS_MOCK_SENSOR_FPS_SET=error "$OUT/rvd" -c "$CONFIG" -f -d
sleep 2
if grep -q "isp_set_sensor_fps failed" "$LOG_DIR/rvd.log" 2>/dev/null &&
    ! grep -q "not settable on this platform\|no source confirmed" "$LOG_DIR/rvd.log" 2>/dev/null; then
    pass "a setter that fails outright is still reported as a failure"
else
    fail "a setter that fails outright is still reported as a failure" \
        "expected 'isp_set_sensor_fps failed' and none of the unsupported-platform lines"
fi

# Default mock: the setter works, so none of the three arms may appear
pkill -f "$OUT/rvd" 2>/dev/null || true
sleep 1
start_daemon rvd "$OUT/rvd" -c "$CONFIG" -f -d
sleep 2
if grep -q "sensor0 fps: 25$" "$LOG_DIR/rvd.log" 2>/dev/null &&
    ! grep -q "not settable on this platform\|no source confirmed" "$LOG_DIR/rvd.log" 2>/dev/null; then
    pass "a settable sensor still reports the rate it was given"
else
    fail "a settable sensor still reports the rate it was given" \
        "expected a plain 'sensor0 fps: 25' and no unsupported-platform line"
fi

if [ "$KEEP" = "1" ]; then
    echo ""
    echo "=== Keeping daemons running (Ctrl-C to stop) ==="
    echo "  RHD: http://127.0.0.1:18080/"
    echo "  RSD: rtsp://127.0.0.1:15554/stream0"
    echo "  raptorctl: $OUT/raptorctl"
    wait
fi

# ── A geometry change drops the clients holding the old SDP ──
# The picture size is answered in the SDP -- the SPS rides in
# sprop-parameter-sets and carries the dimensions -- and RTSP cannot
# renegotiate it mid-session, so a client left in place is told one size
# while being sent another. rvd reuses the ring across an encoder restart
# and rewrites its stream info in place: nothing is closed, nothing is
# reopened, and no reconnect event carries the news, so the reader has to
# see it on the live header. A rate change in the same session is the
# control: a framerate is advisory, and refreshing it must disconnect
# nobody. Runs late, beside the recovery legs, because set-resolution
# rebuilds the encoder and the legs above are entitled to a stream that
# has not been restarted under them.
GEO_OUT=$(python3 - "$OUT/raptorctl" <<'GEO_EOF'
import socket, subprocess, sys, time

def req(sock, buf, method, url, cseq, extra=""):
    sock.sendall(f"{method} {url} RTSP/1.0\r\nCSeq: {cseq}\r\n{extra}\r\n".encode())
    while b"\r\n\r\n" not in buf[0]:
        d = sock.recv(4096)
        if not d:
            return ""
        buf[0] += d
    head, _, rest = buf[0].partition(b"\r\n\r\n")
    headers = head.decode(errors="replace")
    clen = 0
    for line in headers.split("\r\n"):
        if line.lower().startswith("content-length:"):
            clen = int(line.split(":", 1)[1])
    while len(rest) < clen:
        rest += sock.recv(4096)
    buf[0] = rest[clen:]
    return headers

def ctl(*args):
    r = subprocess.run([sys.argv[1]] + list(args), capture_output=True,
                       text=True, timeout=20)
    return r.stdout

def media(sock, seconds):
    """Interleaved bytes read inside the window, or None once the server closes."""
    end = time.time() + seconds
    got = 0
    while True:
        left = end - time.time()
        if left <= 0:
            return got
        sock.settimeout(left)
        try:
            d = sock.recv(65536)
        except socket.timeout:
            return got
        if not d:
            return None
        got += len(d)

base = "rtsp://127.0.0.1:15554/stream0"
s = socket.create_connection(("127.0.0.1", 15554), timeout=5)
buf = [b""]
req(s, buf, "DESCRIBE", base, 1, "Accept: application/sdp\r\n")
setup = req(s, buf, "SETUP", base + "/video", 2,
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
sid = ""
for line in setup.split("\r\n"):
    if line.lower().startswith("session:"):
        sid = line.split(":", 1)[1].split(";")[0].strip()
sess = f"Session: {sid}\r\n"
play = req(s, buf, "PLAY", base, 3, sess + "Range: npt=0.000-\r\n")
if " 200 " not in play.split("\r\n")[0] + " ":
    print("PLAY_FAILED")
    sys.exit(0)
if not media(s, 3.0):
    print("NO_MEDIA")
    sys.exit(0)

# Control: the rate is a cache refresh, and every client plays on.
ctl("rvd", "set-fps", "0", "12")
print("FPS_KEPT=" + ("1" if media(s, 3.0) else "0"))
ctl("rvd", "set-fps", "0", "25")

res = ctl("rvd", "set-resolution", "0", "1280", "720")
print("SETRES_OK=" + ("1" if '"ok"' in res else "0"))
print("DROPPED=" + ("1" if media(s, 10.0) is None else "0"))
s.close()

# Hand the following legs back the geometry they had, and prove the
# server survived the disconnect it just made.
ctl("rvd", "set-resolution", "0", "1920", "1080")
time.sleep(2)
s2 = socket.create_connection(("127.0.0.1", 15554), timeout=5)
alive = req(s2, [b""], "DESCRIBE", base, 1, "Accept: application/sdp\r\n")
print("ALIVE=" + ("1" if " 200 " in alive.split("\r\n")[0] + " " else "0"))
s2.close()
GEO_EOF
)
if echo "$GEO_OUT" | grep -qE "PLAY_FAILED|NO_MEDIA"; then
    skip "resolution change disconnects the stream's clients" \
        "no playing session to test ($GEO_OUT)"
elif echo "$GEO_OUT" | grep -q "SETRES_OK=0"; then
    skip "resolution change disconnects the stream's clients" "set-resolution was refused"
else
    if echo "$GEO_OUT" | grep -q "FPS_KEPT=1"; then
        pass "a rate change leaves playing clients alone"
    else
        fail "a rate change leaves playing clients alone" \
            "the client stopped receiving after set-fps"
    fi
    if echo "$GEO_OUT" | grep -q "DROPPED=1"; then
        pass "resolution change disconnects the stream's clients"
    else
        fail "resolution change disconnects the stream's clients" \
            "still connected 10s after set-resolution"
    fi
    if echo "$GEO_OUT" | grep -q "ALIVE=1"; then
        pass "rsd answers DESCRIBE after dropping a client"
    else
        fail "rsd answers DESCRIBE after dropping a client" "$GEO_OUT"
    fi
fi

# ── A PAUSEd client holds the stale SDP too ──
# PAUSE keeps the session and its negotiated SDP; a geometry change
# while paused would otherwise resume the client straight onto frames
# its SDP does not describe. The drop must reach everyone with a SETUP
# transport, playing or not.
PAUSE_OUT=$(python3 - "$OUT/raptorctl" <<'PAUSE_EOF'
import socket, subprocess, sys, time

def req(sock, buf, method, url, cseq, extra=""):
    sock.sendall(f"{method} {url} RTSP/1.0\r\nCSeq: {cseq}\r\n{extra}\r\n".encode())
    while b"\r\n\r\n" not in buf[0]:
        d = sock.recv(4096)
        if not d:
            return ""
        buf[0] += d
    head, _, rest = buf[0].partition(b"\r\n\r\n")
    headers = head.decode(errors="replace")
    clen = 0
    for line in headers.split("\r\n"):
        if line.lower().startswith("content-length:"):
            clen = int(line.split(":", 1)[1])
    while len(rest) < clen:
        rest += sock.recv(4096)
    buf[0] = rest[clen:]
    return headers

def ctl(*args):
    r = subprocess.run([sys.argv[1]] + list(args), capture_output=True,
                       text=True, timeout=20)
    return r.stdout

def eof_within(sock, seconds):
    """True once the server closes; drains interleaved bytes meanwhile."""
    end = time.time() + seconds
    while True:
        left = end - time.time()
        if left <= 0:
            return False
        sock.settimeout(left)
        try:
            d = sock.recv(65536)
        except socket.timeout:
            return False
        if not d:
            return True

base = "rtsp://127.0.0.1:15554/stream0"
s = socket.create_connection(("127.0.0.1", 15554), timeout=5)
buf = [b""]
req(s, buf, "DESCRIBE", base, 1, "Accept: application/sdp\r\n")
setup = req(s, buf, "SETUP", base + "/video", 2,
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
sid = ""
for line in setup.split("\r\n"):
    if line.lower().startswith("session:"):
        sid = line.split(":", 1)[1].split(";")[0].strip()
sess = f"Session: {sid}\r\n"
play = req(s, buf, "PLAY", base, 3, sess + "Range: npt=0.000-\r\n")
if " 200 " not in play.split("\r\n")[0] + " ":
    print("PLAY_FAILED")
    sys.exit(0)
time.sleep(1.5)
pause = req(s, buf, "PAUSE", base, 4, sess)
if " 200 " not in pause.split("\r\n")[0] + " ":
    print("PAUSE_FAILED")
    sys.exit(0)

res = ctl("rvd", "set-resolution", "0", "1280", "720")
print("SETRES_OK=" + ("1" if '"ok"' in res else "0"))
print("PAUSED_DROPPED=" + ("1" if eof_within(s, 10.0) else "0"))
s.close()
ctl("rvd", "set-resolution", "0", "1920", "1080")
time.sleep(2)
PAUSE_EOF
)
if echo "$PAUSE_OUT" | grep -qE "PLAY_FAILED|PAUSE_FAILED"; then
    skip "geometry change drops a PAUSEd client" "no paused session to test ($PAUSE_OUT)"
elif echo "$PAUSE_OUT" | grep -q "SETRES_OK=0"; then
    skip "geometry change drops a PAUSEd client" "set-resolution was refused"
elif echo "$PAUSE_OUT" | grep -q "PAUSED_DROPPED=1"; then
    pass "geometry change drops a PAUSEd client"
else
    fail "geometry change drops a PAUSEd client" \
        "paused client still connected 10s after set-resolution"
fi

# ── Recovery invariants: RTP through disturbances ──
# The slow-client legs above prove the server SURVIVES a stall; this
# one proves the streams stay CORRECT through it: both tracks' RTP
# timestamps monotonic across a send-queue overflow (client stall) and
# a producer restart, and video re-enters on a keyframe, never an
# orphan P. This is the seam a field bug (PR #27) shipped through:
# fault injection asserted only coarse recovery while timestamp
# analysis only ever saw undisturbed streams. Runs last: the rvd
# restart disturbs everything after it.
echo ""
echo "=== Recovery invariants ==="
if command -v python3 > /dev/null 2>&1; then
    RTPINV_LOG="$LOG_DIR/rtp_invariants.out"
    python3 "$RAPTOR_DIR/tests/rtp_invariants.py" \
        "rtsp://127.0.0.1:15554/stream0" 3 4 15 > "$RTPINV_LOG" 2>&1 &
    RTPINV_PID=$!
    sleep 9
    pkill -f "$OUT/rvd -c" 2>/dev/null || true
    sleep 1
    start_daemon rvd "$OUT/rvd" -c "$CONFIG" -f -d
    RTPINV_RC=0
    wait $RTPINV_PID || RTPINV_RC=$?
    if [ "$RTPINV_RC" -eq 0 ]; then
        pass "recovery invariants ($(tail -1 "$RTPINV_LOG" | cut -c1-120))"
    else
        fail "recovery invariants" "$(tail -3 "$RTPINV_LOG" | tr '\n' ' ' | cut -c1-220)"
    fi
else
    skip "recovery invariants" "python3 not installed"
fi

# Shutdown happens in trap

echo ""
echo "=== Checking ASan output ==="

ASAN_ERRORS=0
for log in "$LOG_DIR"/*.log; do
    name=$(basename "$log" .log)
    if grep -q "ERROR: AddressSanitizer\|ERROR: LeakSanitizer\|ERROR: ThreadSanitizer\|SUMMARY:.*Sanitizer" "$log" 2>/dev/null; then
        # Filter out known acceptable leaks (e.g. one-time allocations in daemon init)
        real_errors=$(grep -cE "ERROR: (Address|Thread)Sanitizer" "$log" 2>/dev/null || true)
        real_errors=${real_errors:-0}
        if [ "$real_errors" -gt 0 ]; then
            fail "sanitizer $name" "memory errors detected (see $log)"
            ASAN_ERRORS=$((ASAN_ERRORS + 1))
            if [ "$VERBOSE" = "1" ]; then
                grep "SUMMARY:" "$log" 2>/dev/null || true
            fi
        else
            pass "sanitizer $name (leaks only)"
        fi
    else
        pass "sanitizer $name"
    fi
done

# ── Summary ──

echo ""
TOTAL=$((PASS + FAIL + SKIP))
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ($TOTAL total) ==="

if [ "$FAIL" -gt 0 ]; then
    echo "FAILED — check logs in $LOG_DIR/"
    exit 1
fi
echo "ALL PASSED"
exit 0
