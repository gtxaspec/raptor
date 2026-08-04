#!/bin/sh
# Behavioral test for rac beep: argument validation, then a real beep
# published to the speaker ring, captured with ringdump and verified
# sample-exact (count, peak, frequency via zero crossings, ramps).
# Needs no rad and no hardware: the ao-flush/ao-drain control sends
# are tolerant of a missing daemon.
#
# Prerequisites: ./build-asan.sh (asan-out/rac, asan-out/ringdump)
#
# Usage:
#   ./tests/test-rac.sh

set -e

RAPTOR_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$RAPTOR_DIR/asan-out"
WORK="${TMPDIR:-/tmp}/rac-test.$$"

for bin in rac ringdump; do
    if [ ! -x "$OUT/$bin" ]; then
        echo "ERROR: $OUT/$bin missing -- run ./build-asan.sh first"
        exit 1
    fi
done

if [ -e /dev/shm/rss_ring_speaker ]; then
    echo "ERROR: /dev/shm/rss_ring_speaker already exists -- another producer running?"
    exit 1
fi

mkdir -p "$WORK"
PASS=0
FAIL=0

ok() {
    PASS=$((PASS + 1))
    echo "  ok    $1"
}

bad() {
    FAIL=$((FAIL + 1))
    echo "  FAIL  $1"
}

check_rejects() {
    desc="$1"
    shift
    if "$OUT/rac" beep "$@" >/dev/null 2>&1; then
        bad "$desc (accepted, expected reject)"
    else
        ok "$desc"
    fi
}

echo "=== rac beep: argument validation ==="
check_rejects "rejects frequency below 20 Hz" -f 5
check_rejects "rejects frequency above Nyquist" -f 9000 -r 16000
check_rejects "rejects duration below 10 ms" -d 5
check_rejects "rejects duration above 30 s" -d 40000
check_rejects "rejects invalid sample rate" -r 0

echo "=== rac beep: ring capture (1 kHz, 1 s, 16 kHz) ==="
# 1 s at 16 kHz = 50 x 20 ms chunks = 16000 samples = 32000 bytes.
"$OUT/rac" beep -f 1000 -d 1000 -r 16000 >"$WORK/rac.log" 2>&1 &
RAC_PID=$!

# The ring appears before the first publish (rac waits up to 200 ms for
# a reader); the 16-slot ring then holds 320 ms of audio, so attaching
# within this poll loop can never miss frames.
i=0
while [ ! -e /dev/shm/rss_ring_speaker ] && [ $i -lt 100 ]; do
    i=$((i + 1))
    sleep 0.01
done
if [ ! -e /dev/shm/rss_ring_speaker ]; then
    bad "speaker ring never appeared"
    kill $RAC_PID 2>/dev/null || true
    wait $RAC_PID 2>/dev/null || true
    rm -rf "$WORK"
    echo "rac suite: $PASS passed, $((FAIL)) failed"
    exit 1
fi
ok "speaker ring created"

timeout 15 "$OUT/ringdump" speaker -d -n 50 >"$WORK/beep.raw" 2>"$WORK/ringdump.log" || true

if wait $RAC_PID; then
    ok "rac beep exited 0"
else
    bad "rac beep exited nonzero (see $WORK/rac.log)"
fi

if [ -e /dev/shm/rss_ring_speaker ]; then
    bad "speaker ring not destroyed on exit"
else
    ok "speaker ring destroyed on exit"
fi

if python3 - "$WORK/beep.raw" <<'EOF'
import struct
import sys

data = open(sys.argv[1], "rb").read()
samples = struct.unpack("<%dh" % (len(data) // 2), data)
n = len(samples)
failures = []

# Sample-exact count: 1 s at 16 kHz.
if n != 16000:
    failures.append("sample count %d != 16000" % n)

# Peak amplitude: 0.35 FS with 16 samples/cycle lands within 2%% of
# the crest, so [0.30, 0.40] catches silence, clipping, and a wrong
# amplitude without being phase-sensitive.
peak = max(abs(s) for s in samples) / 32767.0
if not 0.30 <= peak <= 0.40:
    failures.append("peak %.3f outside [0.30, 0.40]" % peak)

# Frequency: 1 kHz for 1 s = 1000 cycles = ~2000 zero crossings.
zc = sum(1 for a, b in zip(samples, samples[1:]) if (a < 0) != (b < 0))
if not 1980 <= zc <= 2020:
    failures.append("zero crossings %d outside [1980, 2020]" % zc)

# DC offset: a pure sine averages to ~0.
dc = sum(samples) / n / 32767.0
if abs(dc) > 0.01:
    failures.append("DC offset %.4f" % dc)

# Ramps: the first and last 1 ms sit low on the 5 ms raised-cosine
# fade (amp factor <= 0.09), the middle runs at full amplitude.
edge = 16  # 1 ms at 16 kHz
head = max(abs(s) for s in samples[:edge]) / 32767.0
tail = max(abs(s) for s in samples[-edge:]) / 32767.0
mid = max(abs(s) for s in samples[n // 4 : 3 * n // 4]) / 32767.0
if head > 0.25 * mid:
    failures.append("no attack ramp (head %.3f vs mid %.3f)" % (head, mid))
if tail > 0.25 * mid:
    failures.append("no release ramp (tail %.3f vs mid %.3f)" % (tail, mid))

for f in failures:
    print("  FAIL  %s" % f)
if not failures:
    print("  ok    payload: 16000 samples, peak %.3f, %d crossings, ramps present" % (peak, zc))
sys.exit(1 if failures else 0)
EOF
then
    PASS=$((PASS + 1))
else
    FAIL=$((FAIL + 1))
fi

if grep -q "beeped" "$WORK/rac.log"; then
    ok "duration report present"
else
    bad "duration report missing (see $WORK/rac.log)"
fi

rm -rf "$WORK"
echo "rac suite: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
