#!/bin/sh
# Behavioral test for ric: scripted AE via a stub rvd socket, GPIO
# actuation observed on a fake sysfs, ADC via an LD_PRELOAD shim.
# Needs no hardware and no privileges: everything runs inside an
# unprivileged user+mount namespace (see ric_harness.py).
#
# Prerequisites: ./build-asan.sh (asan-out/ric)
#
# Usage:
#   ./tests/test-ric.sh                # run the suite
#   RIC_SUITE_STRICT=1 ./tests/test-ric.sh   # count pending-PR#14 legs too

set -e

RAPTOR_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$RAPTOR_DIR/asan-out"

if [ ! -x "$OUT/ric" ]; then
    echo "ERROR: $OUT/ric missing -- run ./build-asan.sh first"
    exit 1
fi

if ! unshare -rm true 2>/dev/null; then
    echo "SKIP: unshare -rm unavailable (no unprivileged userns?)"
    exit 0
fi

# ADC shim: plain cc, no sanitizer -- it is preloaded after the ASan
# runtime, so interceptor chains still terminate here via RTLD_NEXT.
SHIM="$OUT/ric_adc_shim.so"
if cc -shared -fPIC -O2 -o "$SHIM" "$RAPTOR_DIR/tests/ric_adc_shim.c" -ldl; then
    ASAN_RT="$(ldd "$OUT/ric" 2>/dev/null | awk '/libasan/{print $3; exit}')"
    if [ -n "$ASAN_RT" ]; then
        RIC_ADC_PRELOAD="$ASAN_RT $SHIM"
    else
        RIC_ADC_PRELOAD="$SHIM"
    fi
else
    echo "WARN: ADC shim build failed; adc scenario will be skipped"
    RIC_ADC_PRELOAD=""
fi

# Unit-check the shim before trusting the suite to it: both read entries
# (plain and the fortified __read_chk that bypassed it in issue #18) and
# the rewind semantics, host-independently. A failing shim is a hard
# error, not a skip -- a suite running against a broken shim proves
# nothing about ric.
if [ -n "$RIC_ADC_PRELOAD" ]; then
    PROBE="$OUT/ric_shim_probe"
    if ! cc -O2 -o "$PROBE" "$RAPTOR_DIR/tests/ric_shim_probe.c"; then
        echo "ERROR: shim probe build failed"
        exit 1
    fi
    PROBE_BACKING="$(mktemp)"
    if ! RIC_ADC_BACKING="$PROBE_BACKING" LD_PRELOAD="$SHIM" "$PROBE"; then
        rm -f "$PROBE_BACKING" "$PROBE_BACKING.hits"
        echo "ERROR: adc shim unit check failed"
        exit 1
    fi
    rm -f "$PROBE_BACKING" "$PROBE_BACKING.hits"
fi

WORK="${TMPDIR:-/tmp}/ric-suite.$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

echo "=== ric behavior suite ==="
RIC_BIN="$OUT/ric" \
RIC_ADC_PRELOAD="$RIC_ADC_PRELOAD" \
RIC_TEST_WORK="$WORK" \
    unshare -rm python3 "$RAPTOR_DIR/tests/ric_harness.py"
