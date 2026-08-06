#!/bin/bash
#
# test-fuzz.sh -- bounded libFuzzer run over the parsers that read
# untrusted network input before authentication:
#
#   fuzz_sdp        rwd's SDP offer parser (WHIP body)
#   fuzz_stun       rwd's STUN parser (raw UDP from anyone)
#   fuzz_http_auth  rhd/rwd Digest+Basic header parsing (pre-auth)
#
# A crash here is a remote DoS at best. The run is time-boxed so it
# can sit in test-all; give it longer with --seconds for a real hunt,
# and keep the corpus with --corpus to make successive runs cumulative.
#
# Needs clang (libFuzzer); skips cleanly without it, since the rest of
# the suite builds with gcc.
#
# Usage:
#   ./tests/test-fuzz.sh [--seconds <n>] [--corpus <dir>]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RAPTOR_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SECONDS_EACH=20
CORPUS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --seconds) SECONDS_EACH="$2"; shift 2 ;;
        --corpus) CORPUS="$2"; shift 2 ;;
        -h|--help) echo "Usage: $0 [--seconds <n>] [--corpus <dir>]"; exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

if ! command -v clang > /dev/null 2>&1; then
    echo "SKIP: clang not installed (libFuzzer harnesses need it)"
    exit 0
fi

cd "$RAPTOR_DIR/fuzz"
echo "=== Building fuzz harnesses ==="
if ! make > /tmp/fuzz-build.log 2>&1; then
    echo "FAIL: harness build (see /tmp/fuzz-build.log)"
    tail -5 /tmp/fuzz-build.log
    exit 1
fi
echo "  built: fuzz_sdp fuzz_stun fuzz_http_auth"

PASS=0
FAIL=0

for target in fuzz_sdp fuzz_stun fuzz_http_auth; do
    echo ""
    echo "=== $target (${SECONDS_EACH}s) ==="
    args=(-max_total_time="$SECONDS_EACH" -print_final_stats=1)
    if [ -n "$CORPUS" ]; then
        mkdir -p "$CORPUS/$target"
        args+=("$CORPUS/$target")
    fi
    log=/tmp/$target.log
    if timeout -k 5 $((SECONDS_EACH + 30)) "./$target" "${args[@]}" > "$log" 2>&1; then
        execs=$(grep -oE 'stat::number_of_executed_units: *[0-9]+' "$log" | grep -oE '[0-9]+$')
        echo "  PASS  no crashes in ${execs:-?} executions"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  crash or timeout (see $log)"
        grep -E "ERROR|SUMMARY|Test unit written" "$log" | head -4 | sed 's/^/        /'
        FAIL=$((FAIL + 1))
    fi
done

echo ""
echo "fuzz suite: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
