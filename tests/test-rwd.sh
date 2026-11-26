#!/bin/sh
# Security behavior suite for rwd against the real daemon: DTLS fingerprint
# pinning, session-slot eviction, CORS, WebTorrent offer authentication and
# the tracker/signaling race. Everything runs in a private user, mount and
# network namespace (own ports, /dev/shm, /run).
#
# Prerequisites: ./build-asan.sh (asan-out/rwd, asan-out/create_rings),
# openssl and python3.
#
# Usage: ./tests/test-rwd.sh

set -e

RAPTOR_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${RWD_OUT:-$RAPTOR_DIR/asan-out}"

for f in rwd create_rings; do
    if [ ! -x "$OUT/$f" ]; then
        echo "ERROR: $OUT/$f missing -- run ./build-asan.sh first"
        exit 1
    fi
done

if ! unshare -Urmn true 2>/dev/null; then
    echo "SKIP: unshare -Urmn unavailable (no unprivileged userns?)"
    exit 0
fi

WORK="${TMPDIR:-/tmp}/rwd-suite.$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

echo "=== rwd security suite ==="
RWD_OUT="$OUT" RWD_TEST_WORK="$WORK" \
    unshare -Urmn sh -c '
mount -t tmpfs none /dev/shm && mount -t tmpfs none /run && mkdir -p /run/rss &&
ip link set lo up && exec python3 "$1"' sh "$RAPTOR_DIR/tests/rwd_harness.py"
