#!/bin/sh
# Security behavior suite for rsd against the real daemon: stalled players,
# endpoint aliases, RTSPS, request framing and connection slots. Everything
# runs in a private user, mount and network namespace (own ports,
# /dev/shm, /run).
#
# Prerequisites: ./build-asan.sh (asan-out/rsd, asan-out/create_rings),
# openssl and python3.
#
# Usage: ./tests/test-rsd.sh [leg ...]

set -e

RAPTOR_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${RSD_OUT:-$RAPTOR_DIR/asan-out}"

for f in rsd create_rings; do
    if [ ! -x "$OUT/$f" ]; then
        echo "ERROR: $OUT/$f missing -- run ./build-asan.sh first"
        exit 1
    fi
done

if ! unshare -Urmn true 2>/dev/null; then
    echo "SKIP: unshare -Urmn unavailable (no unprivileged userns?)"
    exit 0
fi

WORK="${TMPDIR:-/tmp}/rsd-suite.$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

echo "=== rsd security suite ==="
RSD_OUT="$OUT" RSD_TEST_WORK="$WORK" \
    unshare -Urmn sh -c '
mount -t tmpfs none /dev/shm && mount -t tmpfs none /run && mkdir -p /run/rss &&
ip link set lo up && exec python3 "$@"' sh "$RAPTOR_DIR/tests/rsd_harness.py" "$@"
