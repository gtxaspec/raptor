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

# Advanced encoder
check_contains "enc-set gop_mode" "ok" "$OUT/raptorctl" rvd enc-set 0 gop_mode 0
check_contains "enc-get gop_mode" "gop_mode" "$OUT/raptorctl" rvd enc-get 0 gop_mode
check_contains "enc-set color2grey" "ok" "$OUT/raptorctl" rvd enc-set 0 color2grey 1
check_contains "enc-get color2grey" "color2grey" "$OUT/raptorctl" rvd enc-get 0 color2grey

# ISP
check_contains "get-isp" "brightness" "$OUT/raptorctl" rvd get-isp
check_contains "get-wb" "mode" "$OUT/raptorctl" rvd get-wb
check_contains "get-exposure" "total_gain" "$OUT/raptorctl" rvd get-exposure
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

if [ "$KEEP" = "1" ]; then
    echo ""
    echo "=== Keeping daemons running (Ctrl-C to stop) ==="
    echo "  RHD: http://127.0.0.1:18080/"
    echo "  RSD: rtsp://127.0.0.1:15554/stream0"
    echo "  raptorctl: $OUT/raptorctl"
    wait
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
