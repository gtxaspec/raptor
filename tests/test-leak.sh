#!/bin/bash
#
# test-leak.sh -- Memory leak and data race detection
#
# Exercises the daemon lifecycle (startup, client connect/disconnect,
# ring reconnect, shutdown) under sanitizers.
#
# Requires: ./build-asan.sh run first (or --tsan to auto-rebuild)
# Usage:    ./tests/test-leak.sh [--verbose] [--duration <seconds>] [--tsan]
#
# --tsan      Rebuild with ThreadSanitizer and check for data races
#             instead of memory leaks. Mutually exclusive with ASAN.
# --duration  Sustained soak with long-running + cycling clients.
#             Without it, runs a quick pass (~30s).
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RAPTOR_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT="$RAPTOR_DIR/asan-out"
LOG_DIR="$OUT/leak-logs"
VERBOSE=""
DURATION=0
SAN_MODE="asan"

while [ $# -gt 0 ]; do
    case "$1" in
        --verbose) VERBOSE="--verbose"; shift ;;
        --duration) DURATION="$2"; shift 2 ;;
        --tsan) SAN_MODE="tsan"; shift ;;
        *) echo "Usage: $0 [--verbose] [--duration <seconds>] [--tsan]"; exit 1 ;;
    esac
done

RTSP_PORT=15654
HTTP_PORT=18180
RWD_HTTP_PORT=18554
RWD_UDP_PORT=18443
SRT_PORT=19000

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

    # ── Scan logs for sanitizer errors ──
    echo ""
    echo "=== Sanitizer check ($SAN_MODE) ==="

    local found=0
    for log in "$LOG_DIR"/*.log "$LOG_DIR"/tsan.* "$LOG_DIR"/asan.*; do
        [ -f "$log" ] || continue
        local name
        name=$(basename "$log" .log)

        if [ "$SAN_MODE" = "tsan" ]; then
            if grep -q "WARNING: ThreadSanitizer" "$log"; then
                local count
                count=$(grep -c "WARNING: ThreadSanitizer" "$log")
                echo "  RACE  $name: $count data race(s)"
                if [ "$VERBOSE" = "--verbose" ]; then
                    grep -A10 "WARNING: ThreadSanitizer" "$log" | head -40
                fi
                found=1
            else
                echo "  CLEAN $name"
            fi
        else
            if grep -q "ERROR: LeakSanitizer" "$log"; then
                local summary
                summary=$(grep "SUMMARY: .*LeakSanitizer" "$log" | tail -1)
                echo "  LEAK  $name: $summary"
                if [ "$VERBOSE" = "--verbose" ]; then
                    grep -A5 "Direct leak\|Indirect leak" "$log" | head -30
                fi
                found=1
            elif grep -q "ERROR: AddressSanitizer" "$log"; then
                echo "  ASAN  $name: memory error detected"
                if [ "$VERBOSE" = "--verbose" ]; then
                    grep -A10 "ERROR: AddressSanitizer" "$log" | head -30
                fi
                found=1
            else
                echo "  CLEAN $name"
            fi
        fi
    done

    echo ""
    if [ "$found" -eq 1 ]; then
        echo "FAILED — $SAN_MODE errors detected (logs in $LOG_DIR/)"
        exit 1
    elif [ "$FAIL" -ne 0 ]; then
        echo "FAILED — $FAIL test phase(s) failed"
        exit 1
    else
        echo "PASSED — $SAN_MODE clean"
        exit 0
    fi
}

trap cleanup EXIT

# ── Preflight ──

if [ ! -f "$OUT/create_rings" ] || [ ! -f "$OUT/rsd" ]; then
    if [ "$SAN_MODE" = "tsan" ]; then
        echo "=== Building with ThreadSanitizer ==="
        (cd "$RAPTOR_DIR" && ./build-asan.sh tsan) || { echo "ERROR: tsan build failed"; exit 1; }
    else
        echo "ERROR: Run ./build-asan.sh first"
        exit 1
    fi
fi

for cmd in curl ffprobe; do
    if ! command -v "$cmd" > /dev/null 2>&1; then
        echo "ERROR: $cmd not found"
        exit 1
    fi
done

mkdir -p "$LOG_DIR"
rm -f "$LOG_DIR"/*.log "$LOG_DIR"/tsan.* "$LOG_DIR"/asan.*

if [ "$SAN_MODE" = "tsan" ]; then
    export TSAN_OPTIONS="exitcode=66:log_path=$LOG_DIR/tsan:history_size=4"
else
    export ASAN_OPTIONS="detect_leaks=1:exitcode=23:log_path=$LOG_DIR/asan"
fi

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
enabled = true
font = /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
time_format = %H:%M:%S

[ircut]
enabled = true
mode = day

[motion]
enabled = false

[recording]
enabled = false

[webrtc]
enabled = true
udp_port = $RWD_UDP_PORT
http_port = $RWD_HTTP_PORT
cert = $LOG_DIR/test.crt
key = $LOG_DIR/test.key
https = false
audio_mode = opus

[srt]
enabled = true
port = $SRT_PORT
max_clients = 4
audio = true

[log]
level = warn
CONF

# Generate self-signed cert for DTLS
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "$LOG_DIR/test.key" -out "$LOG_DIR/test.crt" \
    -days 1 -nodes -subj "/CN=test" 2>/dev/null

CONFIG="$LOG_DIR/leak-test.conf"

# ── Clean state ──

mkdir -p /var/run/rss 2>/dev/null || { sudo mkdir -p /var/run/rss && sudo chmod 1777 /var/run/rss; }
rm -f /var/run/rss/*.pid /var/run/rss/*.sock 2>/dev/null || true
rm -f /dev/shm/rss_ring_* /dev/shm/rss_osd_* 2>/dev/null || true

for d in rvd rsd rhd rod ric rmr rwd rsr create_rings; do
    pkill -f "asan-out/$d" 2>/dev/null || true
done
sleep 0.5

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
        cat "$LOG_DIR/$name.log" 2>/dev/null | head -20
        FAIL=$((FAIL + 1))
        return 1
    fi
    echo "  started $name (pid $pid)"
}

echo "=== Starting daemons ==="
start_daemon rvd "$OUT/rvd" -c "$CONFIG" -f
start_daemon rsd "$OUT/rsd" -c "$CONFIG" -f
start_daemon rad "$OUT/rad" -c "$CONFIG" -f
start_daemon rhd "$OUT/rhd" -c "$CONFIG" -f
start_daemon rod "$OUT/rod" -c "$CONFIG" -f
start_daemon ric "$OUT/ric" -c "$CONFIG" -f
start_daemon rwd "$OUT/rwd" -c "$CONFIG" -f
start_daemon rsr "$OUT/rsr" -c "$CONFIG" -f

# ── Start ring producer (after daemons so it owns the video rings) ──

echo "=== Starting ring producer ==="
"$OUT/create_rings" --no-audio > "$LOG_DIR/rings.log" 2>&1 &
RINGS_PID=$!
sleep 1

if ! kill -0 "$RINGS_PID" 2>/dev/null; then
    echo "ERROR: create_rings failed to start"
    cat "$LOG_DIR/rings.log"
    exit 1
fi
sleep 1

# ── WHIP helper: POST SDP offer, extract session, DELETE to teardown ──

WHIP_SDP="v=0
o=- 0 0 IN IP4 0.0.0.0
s=-
t=0 0
a=group:BUNDLE 0 1
m=video 9 UDP/TLS/RTP/SAVPF 96
c=IN IP4 0.0.0.0
a=mid:0
a=ice-ufrag:leak
a=ice-pwd:leaktestleaktestleaktest
a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99
a=setup:actpass
a=rtpmap:96 H264/90000
a=fmtp:96 packetization-mode=1;profile-level-id=42e01f
a=sendrecv
m=audio 9 UDP/TLS/RTP/SAVPF 111
c=IN IP4 0.0.0.0
a=mid:1
a=ice-ufrag:leak
a=ice-pwd:leaktestleaktestleaktest
a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99
a=setup:actpass
a=rtpmap:111 opus/48000/2
a=sendrecv"

whip_cycle() {
    local resp
    resp=$(curl -s -D - -o /dev/null --max-time 3 \
        -X POST "http://127.0.0.1:$RWD_HTTP_PORT/whip" \
        -H "Content-Type: application/sdp" \
        -d "$WHIP_SDP" 2>/dev/null)
    local loc
    loc=$(echo "$resp" | grep -i "^Location:" | tr -d '\r' | awk '{print $2}')
    if [ -n "$loc" ]; then
        sleep 0.2
        curl -s -o /dev/null --max-time 2 \
            -X DELETE "http://127.0.0.1:$RWD_HTTP_PORT$loc" 2>/dev/null || true
    fi
}

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

echo "  WHIP connect + delete..."
whip_cycle

echo "  phase 1 done"
sleep 1

# ── Phase 1b: RAD codec cycling ──

echo ""
echo "=== Phase 1b: RAD codec cycling ==="

for codec in opus aac pcmu pcma l16; do
    echo "  switching to $codec..."
    "$OUT/raptorctl" rad set-codec "$codec" > /dev/null 2>&1 || true
    sleep 0.5

    # Exercise audio consumers after codec switch
    timeout 2 curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/audio" 2>/dev/null || true
    timeout 2 ffprobe -v quiet -rtsp_transport tcp \
        "rtsp://127.0.0.1:$RTSP_PORT/stream0" > /dev/null 2>&1 || true
    echo "  $codec done"
done
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

    # WebRTC WHIP (create + teardown)
    whip_cycle

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

# Concurrent WHIP sessions
for i in $(seq 1 2); do
    whip_cycle &
    CPIDS="$CPIDS $!"
done

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
"$OUT/create_rings" --no-audio > "$LOG_DIR/rings-restart.log" 2>&1 &
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

echo "  post-reconnect WHIP..."
whip_cycle

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

        # WebRTC WHIP (create + teardown)
        whip_cycle

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
            "$OUT/create_rings" --no-audio > "$LOG_DIR/rings-soak.log" 2>&1 &
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
