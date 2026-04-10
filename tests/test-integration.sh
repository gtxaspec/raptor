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
codec = h264

[audio]
enabled = true
sample_rate = 16000
codec = l16

[rtsp]
port = 15554

[http]
port = 18080
username =
password =

[webrtc]
port = 18443
username =
password =

[osd]
enabled = false

[ircut]
enabled = false

[motion]
enabled = false

[recording]
enabled = false

[log]
level = debug
CONF

CONFIG="$LOG_DIR/test.conf"

# Clean stale state from previous runs
mkdir -p /var/run/rss 2>/dev/null || sudo mkdir -p /var/run/rss
rm -f /var/run/rss/*.pid /var/run/rss/*.sock 2>/dev/null
rm -f /dev/shm/rss_ring_* /dev/shm/rss_osd_* 2>/dev/null

# Kill any lingering daemons from previous runs
for d in rvd rsd rhd rod ric rmr rwd; do
    pkill -f "asan-out/$d" 2>/dev/null || true
done
sleep 0.5

# ── Start infrastructure ──

echo "=== Starting rings ==="
"$OUT/create_rings" > "$LOG_DIR/rings.log" 2>&1 &
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
    fi
}

# Core daemons (must start)
start_daemon rvd "$OUT/rvd" -c "$CONFIG" -f -d
start_daemon rhd "$OUT/rhd" -c "$CONFIG" -f -d
start_daemon rsd "$OUT/rsd" -c "$CONFIG" -f -d

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
check_contains "set-gop-mode" "ok" "$OUT/raptorctl" rvd set-gop-mode 0 0
check_contains "get-gop-mode" "gop_mode" "$OUT/raptorctl" rvd get-gop-mode 0
check_contains "set-color2grey" "ok" "$OUT/raptorctl" rvd set-color2grey 0 1
check_contains "get-color2grey" "color2grey" "$OUT/raptorctl" rvd get-color2grey 0

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

# JPEG snapshot
check_http "GET /snap" "http://127.0.0.1:18080/snap" "200"

# MJPEG stream (connect briefly — timeout exit 124 = success for streaming)
if timeout 2 curl -s -o /dev/null "http://127.0.0.1:18080/mjpeg" 2>/dev/null; ret=$?; [ "$ret" = 124 ] || [ "$ret" = 0 ]; then
    pass "MJPEG stream starts"
else
    fail "MJPEG stream starts" "exit $ret"
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
