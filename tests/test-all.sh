#!/bin/bash
#
# test-all.sh -- Run the full raptor test suite
#
# Stages:
#   1. Build (ASAN or TSAN)
#   2. Sibling repo tests (raptor-ipc, raptor-common)
#   3. Unit tests (host x86, ASAN)
#   4. Integration tests (daemons + curl/ffprobe)
#   5. Leak/race detection (lifecycle soak)
#
# Usage:
#   ./tests/test-all.sh                   # quick pass (~2 min)
#   ./tests/test-all.sh --soak 300        # with 5-min leak soak
#   ./tests/test-all.sh --tsan            # TSAN instead of ASAN
#   ./tests/test-all.sh --tsan --soak 300 # full TSAN soak
#
# CI:
#   job asan:  ./tests/test-all.sh --soak 300
#   job tsan:  ./tests/test-all.sh --tsan --soak 300
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RAPTOR_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

SOAK=0
SAN_MODE="asan"
VERBOSE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --soak) SOAK="$2"; shift 2 ;;
        --tsan) SAN_MODE="tsan"; shift ;;
        --verbose|-v) VERBOSE="--verbose"; shift ;;
        -h|--help)
            echo "Usage: $0 [--soak <seconds>] [--tsan] [--verbose]"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

PASS=0
FAIL=0
TOTAL_START=$(date +%s)

stage_pass() {
    PASS=$((PASS + 1))
    echo ""
    echo "  >> PASS: $1"
    echo ""
}

stage_fail() {
    FAIL=$((FAIL + 1))
    echo ""
    echo "  >> FAIL: $1"
    echo ""
}

echo "========================================"
echo " raptor test suite ($SAN_MODE)"
echo "========================================"
echo ""

# ── Stage 1: Build ──

echo "=== Stage 1: Build ($SAN_MODE) ==="

if [ -f "$RAPTOR_DIR/asan-out/rsd" ] && [ -f "$RAPTOR_DIR/asan-out/create_rings" ]; then
    echo "  binaries exist, skipping build (delete asan-out/ to force)"
    stage_pass "build ($SAN_MODE) [cached]"
elif [ "$SAN_MODE" = "tsan" ]; then
    if (cd "$RAPTOR_DIR" && ./build-asan.sh tsan); then
        stage_pass "build (tsan)"
    else
        stage_fail "build (tsan)"
        echo "Build failed — cannot continue."
        exit 1
    fi
else
    if (cd "$RAPTOR_DIR" && ./build-asan.sh); then
        stage_pass "build (asan)"
    else
        stage_fail "build (asan)"
        echo "Build failed — cannot continue."
        exit 1
    fi
fi

# ── Stage 2: Sibling repo tests ──

echo "=== Stage 2: Sibling repo tests ==="

UNIT_SAN="address"
if [ "$SAN_MODE" = "tsan" ]; then
    UNIT_SAN="thread"
fi

for repo in raptor-ipc raptor-common; do
    REPO_DIR="$RAPTOR_DIR/../$repo"
    if [ -d "$REPO_DIR/tests" ]; then
        if (cd "$REPO_DIR/tests" && make clean > /dev/null 2>&1 && make tests SAN="$UNIT_SAN" > /dev/null 2>&1 && ./tests 2>/dev/null); then
            stage_pass "$repo tests ($UNIT_SAN)"
        else
            stage_fail "$repo tests ($UNIT_SAN)"
        fi
    else
        echo "  $repo/tests not found, skipping"
    fi
done

# ── Stage 3: Unit tests ──

echo "=== Stage 3: Unit tests ==="

if (cd "$RAPTOR_DIR/tests" && make clean > /dev/null 2>&1 && make tests SAN="$UNIT_SAN" > /dev/null 2>&1 && ./tests 2>/dev/null); then
    stage_pass "unit tests ($UNIT_SAN)"
else
    stage_fail "unit tests ($UNIT_SAN)"
fi

# ── Stage 4: Integration tests ──

echo "=== Stage 4: Integration tests ==="

if "$SCRIPT_DIR/test-integration.sh"; then
    stage_pass "integration tests"
else
    stage_fail "integration tests"
fi

# ── Stage 5: Leak / race detection ──

echo "=== Stage 5: Leak/race detection ==="

LEAK_ARGS=""
if [ "$SAN_MODE" = "tsan" ]; then
    LEAK_ARGS="--tsan"
fi
if [ "$SOAK" -gt 0 ]; then
    LEAK_ARGS="$LEAK_ARGS --duration $SOAK"
fi
if [ -n "$VERBOSE" ]; then
    LEAK_ARGS="$LEAK_ARGS $VERBOSE"
fi

if "$SCRIPT_DIR/test-leak.sh" $LEAK_ARGS; then
    stage_pass "leak/race check"
else
    stage_fail "leak/race check"
fi

# ── Summary ──

TOTAL_END=$(date +%s)
ELAPSED=$((TOTAL_END - TOTAL_START))

echo "========================================"
echo " Results: $PASS passed, $FAIL failed"
echo " Time:    ${ELAPSED}s"
echo " Mode:    $SAN_MODE"
if [ "$SOAK" -gt 0 ]; then
    echo " Soak:    ${SOAK}s"
fi
echo "========================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
