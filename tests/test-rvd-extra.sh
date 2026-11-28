#!/bin/sh
# Stress for rvd's extra video streams: rvd with [stream2] enabled and the OSD
# thread running, a reader holding the extra ring open, while stream-restart,
# set-resolution, set-codec, osd-restart and privacy are sent at it in a loop.
# Extra streams are detached and attached from the control thread while the OSD
# thread walks streams[]; run it against the TSan build (./build-asan.sh tsan)
# as well as the ASan one. Runs in a private user, mount and network namespace.
#
# Usage: ./tests/test-rvd-extra.sh [rounds]

set -e

RAPTOR_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${RVD_OUT:-$RAPTOR_DIR/asan-out}"
ROUNDS="${1:-20}"

for f in rvd raptorctl ringdump; do
    if [ ! -x "$OUT/$f" ]; then
        echo "ERROR: $OUT/$f missing -- run ./build-asan.sh first"
        exit 1
    fi
done

if ! unshare -Urmn true 2>/dev/null; then
    echo "SKIP: unshare -Urmn unavailable (no unprivileged userns?)"
    exit 0
fi

if [ -z "$RVD_EXTRA_INNER" ]; then
    RVD_EXTRA_INNER=1 RVD_OUT="$OUT" exec unshare -Urmn sh -c '
mount -t tmpfs none /dev/shm && mount -t tmpfs none /run && mkdir -p /run/rss &&
exec sh "$0" "$@"' "$0" "$ROUNDS"
fi

WORK="${TMPDIR:-/tmp}/rvd-extra.$$"
mkdir -p "$WORK"
PIDS=""
cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null || true; done
    rm -rf "$WORK"
}
trap cleanup EXIT

cat > "$WORK/rvd.conf" <<EOF
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
gop = 50

[stream1]
width = 640
height = 360
fps = 25
bitrate = 500000
codec = h264

[stream2]
enabled = true
ring = aux
width = 1280
height = 720
bitrate = 1500000
linger = 1
osd_enabled = true

[osd]
enabled = true

[ring]
refmode = true

[log]
level = debug
EOF

ASAN_OPTIONS="abort_on_error=0:detect_leaks=1:log_path=$WORK/san" \
TSAN_OPTIONS="log_path=$WORK/san" \
    "$OUT/rvd" -c "$WORK/rvd.conf" -f -d > "$WORK/rvd.log" 2>&1 &
RVD=$!
PIDS="$RVD"

for _ in $(seq 1 100); do
    [ -e /dev/shm/rss_ring_aux ] && break
    sleep 0.1
done

PASS=0
FAIL=0
check() {
    name=$1
    shift
    if "$@"; then
        PASS=$((PASS + 1))
        echo "  ok   $name"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL $name"
    fi
}
ctl() { timeout 20 "$OUT/raptorctl" rvd "$@" > /dev/null 2>&1 || true; }

echo "=== rvd extra streams under stress ($ROUNDS rounds) ==="
check "aux attached" grep -q "extra \[stream2\]: attached" "$WORK/rvd.log"

# a reader keeps the extra stream's encoder running
"$OUT/ringdump" aux -f -n 1000000 > /dev/null 2>&1 &
PIDS="$PIDS $!"

i=0
while [ "$i" -lt "$ROUNDS" ]; do
    i=$((i + 1))
    ctl stream-restart 0
    ctl set-resolution 4 960 540
    ctl set-resolution 0 1280 720
    ctl stream-restart 4
    ctl set-codec 4 h265
    ctl osd-restart
    ctl privacy on
    ctl set-resolution 4 1280 720
    ctl set-codec 4 h264
    ctl privacy off
    ctl set-resolution 0 1920 1080
    ctl stream-restart 1
    kill -0 "$RVD" 2>/dev/null || break
done

check "rvd survives $ROUNDS rounds" kill -0 "$RVD"
check "aux attached again after the last round" \
    test "$(grep -c "extra \[stream2\]: attached" "$WORK/rvd.log")" -ge 2

kill "$RVD" 2>/dev/null || true
wait "$RVD" 2>/dev/null || true
PIDS=""
check "no sanitizer report" sh -c "ls '$WORK'/san* >/dev/null 2>&1 && exit 1 || exit 0"
if ls "$WORK"/san* >/dev/null 2>&1; then
    cat "$WORK"/san* | head -80
fi

echo
echo "$PASS checks passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
