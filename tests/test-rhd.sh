#!/bin/sh
# Behavior suite for rhd's client handling against the real daemon: TLS
# handshakes and writes that must not hold the only thread, a TLS stream
# client that sends data while frames go out, and connections that never
# ask for anything. Runs in a private user, mount and network namespace
# (own ports, /dev/shm, /run).
#
# Prerequisites: ./build-asan.sh (asan-out/rhd, create_rings, raptorctl),
# openssl and python3. Under a TSan build the stream-client leg is what
# reports a data race on the TLS connection.
#
# Usage: ./tests/test-rhd.sh

set -e

RAPTOR_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${RHD_OUT:-$RAPTOR_DIR/asan-out}"

for f in rhd create_rings raptorctl; do
    if [ ! -x "$OUT/$f" ]; then
        echo "ERROR: $OUT/$f missing -- run ./build-asan.sh first"
        exit 1
    fi
done

if ! unshare -Urmn true 2>/dev/null; then
    echo "SKIP: unshare -Urmn unavailable (no unprivileged userns?)"
    exit 0
fi

WORK="${TMPDIR:-/tmp}/rhd-suite.$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

echo "=== rhd client handling suite ==="
RHD_OUT="$OUT" RHD_TEST_WORK="$WORK" \
    unshare -Urmn sh -c '
mount -t tmpfs none /dev/shm && mount -t tmpfs none /run && mkdir -p /run/rss &&
ip link set lo up && exec timeout 300 python3 "$1"' sh "$RAPTOR_DIR/tests/rhd_harness.py"
