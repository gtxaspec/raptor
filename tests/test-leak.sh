#!/bin/bash
#
# test-leak.sh -- Leak detection via LeakSanitizer
#
# Exercises the daemon lifecycle (startup, client connect/disconnect,
# ring reconnect, shutdown) and fails on any memory leak.
#
# Requires: ./build-asan.sh run first
# Usage:    ./tests/test-leak.sh [--verbose] [--duration <seconds>]
#
# Without --duration, runs a quick pass (~30s).
# With --duration, loops connect/disconnect cycles for that long,
# plus a ring reconnect every 60s.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RAPTOR_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT="$RAPTOR_DIR/asan-out"
LOG_DIR="$OUT/leak-logs"
VERBOSE=""
DURATION=0

while [ $# -gt 0 ]; do
    case "$1" in
        --verbose) VERBOSE="--verbose"; shift ;;
        --duration) DURATION="$2"; shift 2 ;;
        *) echo "Usage: $0 [--verbose] [--duration <seconds>]"; exit 1 ;;
    esac
done

RTSP_PORT=15654
HTTP_PORT=18180

DAEMON_PIDS=""
RINGS_PID=""
FAIL=0

# ── Cleanup ──

cleanup() {
    echo ""
    echo "=== Shutdown ==="

    # SIGTERM daemons — triggers LeakSanitizer at exit
    for pid in $DAEMON_PIDS; do
        kill "$pid" 2>/dev/null || true
    done
    sleep 2
    for pid in $DAEMON_PIDS; do
        wait "$pid" 2>/dev/null || true
    done
    kill "$RINGS_PID" 2>/dev/null || true
    wait "$RINGS_PID" 2>/dev/null || true

    # ── Scan logs for leaks ──
    echo ""
    echo "=== Leak check ==="

    local leaked=0
    for log in "$LOG_DIR"/*.log; do
        [ -f "$log" ] || continue
        local name
        name=$(basename "$log" .log)

        if grep -q "ERROR: LeakSanitizer" "$log"; then
            local summary
            summary=$(grep "SUMMARY: .*LeakSanitizer" "$log" | tail -1)
            echo "  LEAK  $name: $summary"
            if [ "$VERBOSE" = "--verbose" ]; then
                grep -A5 "Direct leak\|Indirect leak" "$log" | head -30
            fi
            leaked=1
        elif grep -q "ERROR: AddressSanitizer" "$log"; then
            echo "  ASAN  $name: memory error detected"
            leaked=1
        else
            echo "  CLEAN $name"
        fi
    done

    echo ""
    if [ "$leaked" -eq 1 ]; then
        echo "FAILED — leaks detected (logs in $LOG_DIR/)"
        exit 1
    elif [ "$FAIL" -ne 0 ]; then
        echo "FAILED — $FAIL test phase(s) failed"
        exit 1
    else
        echo "PASSED — no leaks"
        exit 0
    fi
}

trap cleanup EXIT

# ── Preflight ──

if [ ! -f "$OUT/create_rings" ] || [ ! -f "$OUT/rsd" ]; then
    echo "ERROR: Run ./build-asan.sh first"
    exit 1
fi

for cmd in curl ffprobe; do
    if ! command -v "$cmd" > /dev/null 2>&1; then
        echo "ERROR: $cmd not found"
        exit 1
    fi
done

mkdir -p "$LOG_DIR"
rm -f "$LOG_DIR"/*.log

# Force LeakSanitizer on, abort on leak so exit code is nonzero
export ASAN_OPTIONS="detect_leaks=1:exitcode=23:log_path=$LOG_DIR/asan"

# ── Config ──

cat > "$LOG_DIR/leak-test.conf" << CONF
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
port = $RTSP_PORT

[http]
port = $HTTP_PORT
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
level = warn
CONF

CONFIG="$LOG_DIR/leak-test.conf"

# ── Clean state ──

mkdir -p /var/run/rss 2>/dev/null || sudo mkdir -p /var/run/rss
rm -f /var/run/rss/*.pid /var/run/rss/*.sock 2>/dev/null || true
rm -f /dev/shm/rss_ring_* /dev/shm/rss_osd_* 2>/dev/null || true

for d in rvd rsd rhd rod ric rmr rwd create_rings; do
    pkill -f "asan-out/$d" 2>/dev/null || true
done
sleep 0.5

# ── Start ring producer ──

echo "=== Starting ring producer ==="
"$OUT/create_rings" > "$LOG_DIR/rings.log" 2>&1 &
RINGS_PID=$!
sleep 1

if ! kill -0 "$RINGS_PID" 2>/dev/null; then
    echo "ERROR: create_rings failed to start"
    cat "$LOG_DIR/rings.log"
    exit 1
fi

# ── Start daemons ──

start_daemon() {
    local name="$1"
    shift
    "$@" > "$LOG_DIR/$name.log" 2>&1 &
    local pid=$!
    DAEMON_PIDS="$DAEMON_PIDS $pid"
    sleep 0.3
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "ERROR: $name failed to start (check $LOG_DIR/$name.log)"
        FAIL=$((FAIL + 1))
        return 1
    fi
    echo "  started $name (pid $pid)"
}

echo "=== Starting daemons ==="
start_daemon rvd "$OUT/rvd" -c "$CONFIG" -f
start_daemon rsd "$OUT/rsd" -c "$CONFIG" -f
start_daemon rhd "$OUT/rhd" -c "$CONFIG" -f
sleep 1

# ── Phase 1: Single client connect/disconnect ──

echo ""
echo "=== Phase 1: single client lifecycle ==="

echo "  RTSP connect + disconnect..."
timeout 3 ffprobe -v quiet -rtsp_transport tcp \
    "rtsp://127.0.0.1:$RTSP_PORT/stream0" > /dev/null 2>&1 || true
sleep 0.5

echo "  HTTP snapshot..."
curl -s -o /dev/null --max-time 3 "http://127.0.0.1:$HTTP_PORT/snap" || true

echo "  MJPEG stream (2s)..."
timeout 2 curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/mjpeg" 2>/dev/null || true

echo "  audio stream (2s)..."
timeout 2 curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/audio" 2>/dev/null || true

echo "  phase 1 done"
sleep 1

# ── Phase 2: Rapid connect/disconnect (amplify per-client leaks) ──

echo ""
echo "=== Phase 2: rapid connect/disconnect (5 cycles) ==="

for i in $(seq 1 5); do
    # RTSP
    timeout 2 ffprobe -v quiet -rtsp_transport tcp \
        "rtsp://127.0.0.1:$RTSP_PORT/stream0" > /dev/null 2>&1 || true

    # HTTP snapshot
    curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$HTTP_PORT/snap" || true

    # MJPEG (brief)
    timeout 1 curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/mjpeg" 2>/dev/null || true

    echo "  cycle $i/5 done"
done
sleep 1

# ── Phase 3: Concurrent clients ──

echo ""
echo "=== Phase 3: concurrent clients ==="

CPIDS=""
for i in $(seq 1 4); do
    timeout 3 curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/mjpeg" 2>/dev/null &
    CPIDS="$CPIDS $!"
done
timeout 3 ffprobe -v quiet -rtsp_transport tcp \
    "rtsp://127.0.0.1:$RTSP_PORT/stream0" > /dev/null 2>&1 &
CPIDS="$CPIDS $!"

for pid in $CPIDS; do
    wait "$pid" 2>/dev/null || true
done
echo "  concurrent clients done"
sleep 1

# ── Phase 4: Ring reconnect ──

echo ""
echo "=== Phase 4: ring reconnect ==="

echo "  killing ring producer..."
kill "$RINGS_PID" 2>/dev/null || true
wait "$RINGS_PID" 2>/dev/null || true
sleep 1

echo "  restarting ring producer..."
"$OUT/create_rings" > "$LOG_DIR/rings-restart.log" 2>&1 &
RINGS_PID=$!
sleep 2

if ! kill -0 "$RINGS_PID" 2>/dev/null; then
    echo "  WARN: create_rings restart failed"
    FAIL=$((FAIL + 1))
fi

echo "  post-reconnect RTSP probe..."
timeout 3 ffprobe -v quiet -rtsp_transport tcp \
    "rtsp://127.0.0.1:$RTSP_PORT/stream0" > /dev/null 2>&1 || true

echo "  post-reconnect HTTP snapshot..."
curl -s -o /dev/null --max-time 3 "http://127.0.0.1:$HTTP_PORT/snap" || true

echo "  ring reconnect done"
sleep 1

# ── Phase 5: Sustained soak (if --duration given) ──

if [ "$DURATION" -gt 0 ]; then
    echo ""
    echo "=== Phase 5: soak test (${DURATION}s) ==="

    # Long-running background clients — stay connected for the entire
    # soak to catch per-frame leaks (RTP packetizer, ring reader, frame
    # copy). Short-lived cycling clients run alongside these.
    SOAK_PIDS=""

    echo "  starting long-running RTSP client (TCP, full duration)..."
    timeout $((DURATION + 10)) ffmpeg -v quiet -rtsp_transport tcp \
        -i "rtsp://127.0.0.1:$RTSP_PORT/stream0" \
        -t "$DURATION" -f null - > /dev/null 2>&1 &
    SOAK_PIDS="$SOAK_PIDS $!"

    echo "  starting long-running MJPEG client (full duration)..."
    timeout $((DURATION + 10)) curl -s -o /dev/null \
        "http://127.0.0.1:$HTTP_PORT/mjpeg" 2>/dev/null &
    SOAK_PIDS="$SOAK_PIDS $!"

    sleep 1

    START_TIME=$(date +%s)
    END_TIME=$((START_TIME + DURATION))
    CYCLE=0
    LAST_RECONNECT=$START_TIME

    while [ "$(date +%s)" -lt "$END_TIME" ]; do
        CYCLE=$((CYCLE + 1))
        REMAINING=$((END_TIME - $(date +%s)))

        # RTSP stream (5s read via TCP, exercises full session lifecycle)
        timeout 5 ffmpeg -v quiet -rtsp_transport tcp \
            -i "rtsp://127.0.0.1:$RTSP_PORT/stream0" \
            -t 3 -f null - > /dev/null 2>&1 || true

        # HTTP snapshot
        curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$HTTP_PORT/snap" || true

        # MJPEG stream (2s)
        timeout 2 curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/mjpeg" 2>/dev/null || true

        # Audio stream (1s)
        timeout 1 curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/audio" 2>/dev/null || true

        # Concurrent burst every 5 cycles
        if [ $((CYCLE % 5)) -eq 0 ]; then
            CPIDS=""
            for i in $(seq 1 3); do
                timeout 3 curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/mjpeg" 2>/dev/null &
                CPIDS="$CPIDS $!"
            done
            timeout 5 ffmpeg -v quiet -rtsp_transport tcp \
                -i "rtsp://127.0.0.1:$RTSP_PORT/stream0" \
                -t 3 -f null - > /dev/null 2>&1 &
            CPIDS="$CPIDS $!"
            timeout 5 ffmpeg -v quiet -rtsp_transport udp \
                -i "rtsp://127.0.0.1:$RTSP_PORT/stream0" \
                -t 3 -f null - > /dev/null 2>&1 &
            CPIDS="$CPIDS $!"
            for pid in $CPIDS; do
                wait "$pid" 2>/dev/null || true
            done
        fi

        # Ring reconnect every 60s
        NOW=$(date +%s)
        if [ $((NOW - LAST_RECONNECT)) -ge 60 ]; then
            echo "  cycle $CYCLE (${REMAINING}s left) — ring reconnect"
            kill "$RINGS_PID" 2>/dev/null || true
            wait "$RINGS_PID" 2>/dev/null || true
            sleep 1
            "$OUT/create_rings" > "$LOG_DIR/rings-soak.log" 2>&1 &
            RINGS_PID=$!
            sleep 2
            LAST_RECONNECT=$NOW
        elif [ $((CYCLE % 20)) -eq 0 ]; then
            echo "  cycle $CYCLE (${REMAINING}s left)"
        fi
    done

    # Kill long-running clients before daemon shutdown so the daemons
    # process the disconnect (exercises client teardown path)
    echo "  stopping long-running clients..."
    for pid in $SOAK_PIDS; do
        kill "$pid" 2>/dev/null || true
    done
    for pid in $SOAK_PIDS; do
        wait "$pid" 2>/dev/null || true
    done
    sleep 1

    echo "  soak done: $CYCLE cycles in ${DURATION}s"
fi

# ── Shutdown (handled by cleanup trap) ──
