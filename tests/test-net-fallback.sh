#!/bin/sh
# IPv6-first / IPv4-fallback behavior test for the listening daemons.
#
# Two passes over rvd+rsd+rhd+rwd+rsr with the mock HAL:
#   native   -- the build host has IPv6, so every listener must come up
#               dual-stack and answer on ::1 as well as 127.0.0.1
#   no-ipv6  -- LD_PRELOAD shim makes socket(AF_INET6) fail EAFNOSUPPORT
#               like a kernel built without IPv6; every listener must
#               fall back to IPv4, say so in its log, and still serve
#
# Prerequisites: ./build-asan.sh

set -e

RAPTOR_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$RAPTOR_DIR/asan-out"
LOG="$OUT/netfb-logs"
PASS=0
FAIL=0

for bin in rvd rsd rhd rwd rsr create_rings; do
    if [ ! -x "$OUT/$bin" ]; then
        echo "ERROR: $OUT/$bin missing -- run ./build-asan.sh first"
        exit 1
    fi
done

pass() { PASS=$((PASS + 1)); printf "  PASS  %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  FAIL  %s: %s\n" "$1" "$2"; }

check() { # check <name> <detail-on-fail> <cmd...>
    name="$1"; detail="$2"; shift 2
    # listeners come up asynchronously (rsr waits for rings); retry briefly
    n=0
    while [ "$n" -lt 20 ]; do
        if "$@" > /dev/null 2>&1; then pass "$name"; return 0; fi
        n=$((n + 1))
        sleep 0.5
    done
    fail "$name" "$detail"
}

check_now() { # no retry, for negative assertions
    name="$1"; detail="$2"; shift 2
    if "$@" > /dev/null 2>&1; then pass "$name"; else fail "$name" "$detail"; fi
}

# RTSP OPTIONS round-trip: argument 1 is the host to dial.
rtsp_options() {
    python3 - "$1" "$RTSP_PORT" <<'PY'
import socket, sys
host, port = sys.argv[1], int(sys.argv[2])
s = socket.create_connection((host, port), timeout=3)
s.settimeout(2)
s.sendall(b"OPTIONS rtsp://x/ RTSP/1.0\r\nCSeq: 1\r\n\r\n")
data = b""
try:
    while b"\r\n\r\n" not in data:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
except socket.timeout:
    pass
s.close()
sys.exit(0 if data.startswith(b"RTSP/1.0 200") else 1)
PY
}

RTSP_PORT=25554
HTTP_PORT=28080
WEBRTC_HTTP=28556
WEBRTC_UDP=28445
SRT_PORT=29000

mkdir -p "$LOG" "$LOG/rec"
cat > "$LOG/test.conf" << CONF
[sensor]
model = gc2053
name = gc2053

[stream0]
width = 1920
height = 1080
fps = 25
bitrate = 2000000
codec = h264
gop = 50

[audio]
enabled = true
sample_rate = 16000
codec = l16

[rtsp]
port = $RTSP_PORT

[http]
port = $HTTP_PORT

[webrtc]
enabled = true
udp_port = $WEBRTC_UDP
http_port = $WEBRTC_HTTP
cert = $LOG/netfb-cert.pem
key = $LOG/netfb-key.pem

[srt]
enabled = true
port = $SRT_PORT

[osd]
enabled = false

[ircut]
enabled = false

[motion]
enabled = false

[recording]
enabled = false

[ring]
refmode = true

[log]
level = debug
CONF

# rwd refuses to start without a DTLS cert; a throwaway pair will do.
if [ ! -f "$LOG/netfb-cert.pem" ]; then
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "$LOG/netfb-key.pem" -out "$LOG/netfb-cert.pem" \
        -days 30 -nodes -subj "/CN=netfb-test" > /dev/null 2>&1
fi

# The ASan runtime must stay first in the preload list.
cc -shared -fPIC -O2 -o "$OUT/net_noipv6_shim.so" \
    "$RAPTOR_DIR/tests/net_noipv6_shim.c" -ldl
ASAN_RT="$(ldd "$OUT/rsd" 2>/dev/null | awk '/libasan/{print $3; exit}')"
SHIM="$OUT/net_noipv6_shim.so"
[ -n "$ASAN_RT" ] && SHIM="$ASAN_RT $SHIM"

DAEMON_PIDS=""
RINGS_PID=""

cleanup() {
    for pid in $DAEMON_PIDS; do kill "$pid" 2>/dev/null || true; done
    for pid in $DAEMON_PIDS; do wait "$pid" 2>/dev/null || true; done
    [ -n "$RINGS_PID" ] && kill "$RINGS_PID" 2>/dev/null || true
    [ -n "$RINGS_PID" ] && wait "$RINGS_PID" 2>/dev/null || true
    DAEMON_PIDS=""
    RINGS_PID=""
}
trap cleanup EXIT

start_stack() { # start_stack <suffix> [preload]
    sfx="$1"; preload="$2"
    mkdir -p /var/run/rss 2>/dev/null || true
    rm -f /var/run/rss/*.pid /var/run/rss/*.sock 2>/dev/null || true
    rm -f /dev/shm/rss_ring_* /dev/shm/rss_osd_* 2>/dev/null || true
    "$OUT/create_rings" --skip-video > "$LOG/rings-$sfx.log" 2>&1 &
    RINGS_PID=$!
    sleep 1
    for d in rvd rsd rhd rwd rsr; do
        if [ -n "$preload" ]; then
            env LD_PRELOAD="$preload" "$OUT/$d" -c "$LOG/test.conf" -f -d \
                > "$LOG/$d-$sfx.log" 2>&1 &
        else
            "$OUT/$d" -c "$LOG/test.conf" -f -d > "$LOG/$d-$sfx.log" 2>&1 &
        fi
        DAEMON_PIDS="$DAEMON_PIDS $!"
        sleep 0.3
    done
    sleep 2
}

echo "=== pass 1: native (host kernel has IPv6) ==="
start_stack native ""

check "rsd listens dual-stack (v6 wildcard :$RTSP_PORT)" "no v6 wildcard listener" \
    sh -c "ss -ltn | grep -qE '(\[::\]|\*):$RTSP_PORT '"
check "rsd answers RTSP over ::1" "OPTIONS via ::1 failed" rtsp_options ::1
check "rsd answers RTSP over 127.0.0.1" "OPTIONS via 127.0.0.1 failed" rtsp_options 127.0.0.1
check "rhd answers HTTP over ::1" "curl ::1 failed" \
    curl -s -o /dev/null --max-time 3 "http://[::1]:$HTTP_PORT/"
check "rwd signaling listens dual-stack" "no v6 wildcard listener" \
    sh -c "ss -ltn | grep -qE '(\[::\]|\*):$WEBRTC_HTTP '"
check "rsr SRT bound v6 wildcard UDP" "no v6 udp listener" \
    sh -c "ss -lun | grep -qE '(\[::\]|\*):$SRT_PORT '"

cleanup

echo "=== pass 2: simulated IPv6-less kernel (EAFNOSUPPORT shim) ==="
start_stack noipv6 "$SHIM"

check "rsd falls back and says so" "no fallback log line" \
    grep -q "kernel has no IPv6; listening on IPv4 only" "$LOG/rsd-noipv6.log"
check "rsd listens IPv4-only" "no 0.0.0.0 listener" \
    sh -c "ss -ltn | grep -q '0.0.0.0:$RTSP_PORT'"
check "rsd still answers RTSP over 127.0.0.1" "OPTIONS failed" rtsp_options 127.0.0.1
check_now "rsd refuses ::1 (nothing listens there)" "::1 unexpectedly answered" \
    sh -c "! (echo | timeout 2 python3 -c 'import socket;socket.create_connection((\"::1\",$RTSP_PORT),timeout=1)') 2>/dev/null"
check "rhd falls back via rss_listen_tcp and serves" "curl 127.0.0.1 failed" \
    curl -s -o /dev/null --max-time 3 "http://127.0.0.1:$HTTP_PORT/"
check "rwd media fallback logged" "no media fallback log" \
    grep -q "kernel has no IPv6; WebRTC media on IPv4 only" "$LOG/rwd-noipv6.log"
check "rwd signaling fallback logged" "no signaling fallback log" \
    grep -q "kernel has no IPv6; WebRTC signaling on IPv4 only" "$LOG/rwd-noipv6.log"
check "rwd signaling serves on 127.0.0.1" "connect failed" \
    sh -c "ss -ltn | grep -q '0.0.0.0:$WEBRTC_HTTP'"
check "rsr rebinds v4 and says so" "no SRT fallback log" \
    grep -q "kernel has no IPv6; SRT listening on IPv4 only" "$LOG/rsr-noipv6.log"
check "rsr SRT listening on v4 UDP" "no v4 udp listener" \
    sh -c "ss -lun | grep -q '0.0.0.0:$SRT_PORT'"

cleanup
trap - EXIT

echo ""
echo "net fallback: $PASS pass, $FAIL fail"
[ "$FAIL" -eq 0 ]
