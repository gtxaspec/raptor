#!/bin/bash
#
# test-all.sh -- Run the full raptor test suite
#
# Stages:
#   1. Build (ASAN or TSAN)
#   2. Sibling repo tests (raptor-ipc, raptor-common)
#   3. Unit tests (host x86, ASAN)
#   4. Integration tests (daemons + curl/ffprobe), then the ric
#      behavior suite (stub rvd + fake sysfs GPIO, test-ric.sh),
#      the rac beep suite, the pre-auth parser fuzzers, and the
#      net-fallback suite
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

# ── Stage 0: Hand-written JSON gate ──
#
# Structured formats are built by serializers, never string assembly:
# a "%s" into a JSON literal is one edit away from injection, and the
# safety of today's literal requires provenance reasoning no reviewer
# should have to repeat. Production C carries exactly two documented
# exemptions -- raptorctl_help.c prints example -j syntax (display
# text, not construction) and raptor-ipc's transport error frame
# (rss_ctrl.c, a dependency-free layer emitting a constant shape with
# one integer). Anything else is a failure, not a style note.

echo "=== Stage 0: Hand-written JSON gate ==="
JSON_HITS=$(/usr/bin/grep -rn '{\\"' --include='*.c' --include='*.h' \
    "$RAPTOR_DIR" "$RAPTOR_DIR/../raptor-common" "$RAPTOR_DIR/../raptor-ipc" \
    "$RAPTOR_DIR/../raptor-hal" \
    2>/dev/null |
    grep -v '/tests/\|/fuzz/\|/\.deps/\|/build/\|/asan-out/' |
    grep -v 'raptorctl_help\.c\|raptor-ipc/src/rss_ctrl\.c' || true)
if [ -n "$JSON_HITS" ]; then
    echo "$JSON_HITS"
    stage_fail "hand-written JSON gate"
    echo "Build JSON with cJSON (rss_ctrl_cmd*/rss_ctrl_resp_*), never by hand."
    exit 1
else
    stage_pass "hand-written JSON gate"
fi

# ── Stage 1: Build ──

echo "=== Stage 1: Build ($SAN_MODE) ==="

# Reuse the tree only when it is genuinely the tree this run wants:
# the right sanitizer, and newer than every source that feeds it. The
# old check was bare file existence, so `--tsan` after an asan build
# printed "[cached]" and ran asan binaries under a tsan banner, and
# editing a daemon then running the suite tested the previous build.
BUILD_CACHED=0
if [ -f "$RAPTOR_DIR/asan-out/.build-ok" ] && [ -f "$RAPTOR_DIR/asan-out/rsd" ] &&
   [ -f "$RAPTOR_DIR/asan-out/create_rings" ]; then
    # build-asan.sh already stamps asan-out/.sanitizer (ASan/TSan) so it
    # can clean dep libs across a switch; read that rather than writing
    # a second stamp in a different vocabulary.
    WANT_SAN=ASan
    [ "$SAN_MODE" = "tsan" ] && WANT_SAN=TSan
    HAVE_SAN=$(cat "$RAPTOR_DIR/asan-out/.sanitizer" 2>/dev/null || echo unknown)
    if [ "$HAVE_SAN" != "$WANT_SAN" ]; then
        echo "  asan-out/ was built with '$HAVE_SAN', this run wants '$WANT_SAN' — rebuilding"
    else
        # Generated sources are excluded by name (build-asan.sh writes
        # rss_build_info.c, tests/Makefile seds sdp_parse.c out of
        # rwd_sdp.c) -- both are rewritten every build and would pin the
        # verdict to "stale" forever. Missing a future generated file
        # only costs a rebuild: this check fails safe.
        # -newer against the binary, first hit wins. No pipe into head:
        # this script runs under `set -o pipefail`, and head closing the
        # pipe early would abort the suite rather than answer the
        # question.
        NEWER=$(find "$RAPTOR_DIR" "$RAPTOR_DIR/../raptor-ipc" "$RAPTOR_DIR/../raptor-common" \
            \( -name '*.c' -o -name '*.h' \) -newer "$RAPTOR_DIR/asan-out/rsd" \
            -not -path '*/asan-out/*' -not -path '*/.git/*' \
            -not -name 'rss_build_info.c' -not -name 'sdp_parse.c' \
            -print -quit 2>/dev/null || true)
        if [ -n "$NEWER" ]; then
            echo "  $(basename "$NEWER") is newer than the build — rebuilding"
        else
            echo "  binaries current for $SAN_MODE, skipping build"
            stage_pass "build ($SAN_MODE) [cached]"
            BUILD_CACHED=1
        fi
    fi
fi

if [ "$BUILD_CACHED" = 1 ]; then
    :
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

if "$SCRIPT_DIR/test-ric.sh"; then
    stage_pass "ric behavior suite"
else
    stage_fail "ric behavior suite"
fi

if "$SCRIPT_DIR/test-rac.sh"; then
    stage_pass "rac beep suite"
else
    stage_fail "rac beep suite"
fi

if "$SCRIPT_DIR/test-fuzz.sh"; then
    stage_pass "fuzz (pre-auth parsers)"
else
    stage_fail "fuzz (pre-auth parsers)"
fi

if "$SCRIPT_DIR/test-net-fallback.sh"; then
    stage_pass "net fallback (IPv6-first, IPv4 fallback)"
else
    stage_fail "net fallback (IPv6-first, IPv4 fallback)"
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
