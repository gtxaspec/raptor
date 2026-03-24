#!/bin/sh
# Build ALL raptor daemons for host (x86_64) with AddressSanitizer.
#
# RVD/RAD use a mock HAL (tests/mock_hal.c) that stubs IMP SDK calls.
# All other daemons build natively — no HAL dependency.
#
# Usage:
#   ./build-asan.sh          # build all
#   ./build-asan.sh clean    # clean
#
# Test:
#   ./asan-out/create_rings &          # dummy SHM rings
#   ./asan-out/rvd -c config/raptor.conf -f -d &
#   ./asan-out/rsd -c config/raptor.conf -f -d &
#   ./asan-out/rhd -c config/raptor.conf -f -d &
#   # exercise with curl/ffprobe, Ctrl-C — ASan prints leak report on exit

set -e

RAPTOR_DIR="$(cd "$(dirname "$0")" && pwd)"
HAL_DIR="$RAPTOR_DIR/../raptor-hal"
IPC_DIR="$RAPTOR_DIR/../raptor-ipc"
COMMON_DIR="$RAPTOR_DIR/../raptor-common"
COMPY_DIR="$RAPTOR_DIR/../compy"
COMPY_BUILD="$COMPY_DIR/build"
OUT="$RAPTOR_DIR/asan-out"

CC=gcc
SANITIZE="-fsanitize=address,undefined -fno-omit-frame-pointer"
CFLAGS="-Wall -Wextra -std=gnu11 -D_GNU_SOURCE -DPLATFORM_T31 -O1 -g $SANITIZE"
CFLAGS="$CFLAGS -I$IPC_DIR/include -I$COMMON_DIR/include"
LDFLAGS="$SANITIZE -lpthread -lrt"

# HAL include path (for RVD/RAD/ROD headers)
HAL_CFLAGS="-I$HAL_DIR/include"

# Compy include paths (for RSD)
COMPY_CFLAGS="-I$COMPY_DIR/include"
COMPY_CFLAGS="$COMPY_CFLAGS -I$COMPY_BUILD/_deps/slice99-src"
COMPY_CFLAGS="$COMPY_CFLAGS -I$COMPY_BUILD/_deps/datatype99-src"
COMPY_CFLAGS="$COMPY_CFLAGS -I$COMPY_BUILD/_deps/interface99-src"
COMPY_CFLAGS="$COMPY_CFLAGS -I$COMPY_BUILD/_deps/metalang99-src/include"

if [ "$1" = "clean" ]; then
    rm -rf "$OUT"
    echo "Cleaned asan-out/"
    exit 0
fi

mkdir -p "$OUT"

# ── Libraries ──

echo "=== raptor-common ==="
$CC $CFLAGS -c "$COMMON_DIR/src/rss_log.c" -o "$OUT/rss_log.o"
$CC $CFLAGS -c "$COMMON_DIR/src/rss_config.c" -o "$OUT/rss_config.o"
$CC $CFLAGS -c "$COMMON_DIR/src/rss_daemon.c" -o "$OUT/rss_daemon.o"
$CC $CFLAGS -c "$COMMON_DIR/src/rss_util.c" -o "$OUT/rss_util.o"
ar rcs "$OUT/librss_common.a" "$OUT"/rss_log.o "$OUT"/rss_config.o "$OUT"/rss_daemon.o "$OUT"/rss_util.o

echo "=== raptor-ipc ==="
$CC $CFLAGS -c "$IPC_DIR/src/rss_ring.c" -o "$OUT/rss_ring.o"
$CC $CFLAGS -c "$IPC_DIR/src/rss_osd_shm.c" -o "$OUT/rss_osd_shm.o"
$CC $CFLAGS -c "$IPC_DIR/src/rss_ctrl.c" -o "$OUT/rss_ctrl.o"
ar rcs "$OUT/librss_ipc.a" "$OUT/rss_ring.o" "$OUT/rss_osd_shm.o" "$OUT/rss_ctrl.o"

echo "=== mock_hal ==="
$CC $CFLAGS $HAL_CFLAGS -c "$RAPTOR_DIR/tests/mock_hal.c" -o "$OUT/mock_hal.o"
ar rcs "$OUT/libmock_hal.a" "$OUT/mock_hal.o"

LIBS="$OUT/librss_ipc.a $OUT/librss_common.a"
LIBS_HAL="$OUT/libmock_hal.a $LIBS"

# ── Non-HAL daemons ──

echo "=== RHD ==="
$CC $CFLAGS -c "$RAPTOR_DIR/rhd/rhd_main.c" -o "$OUT/rhd_main.o"
$CC -o "$OUT/rhd" "$OUT/rhd_main.o" $LIBS $LDFLAGS
echo "  -> rhd"

echo "=== RSD ==="
$CC $CFLAGS $COMPY_CFLAGS -c "$RAPTOR_DIR/rsd/rsd_main.c" -o "$OUT/rsd_main.o"
$CC $CFLAGS $COMPY_CFLAGS -c "$RAPTOR_DIR/rsd/rsd_server.c" -o "$OUT/rsd_server.o"
$CC $CFLAGS $COMPY_CFLAGS -c "$RAPTOR_DIR/rsd/rsd_session.c" -o "$OUT/rsd_session.o"
$CC $CFLAGS $COMPY_CFLAGS -c "$RAPTOR_DIR/rsd/rsd_ring_reader.c" -o "$OUT/rsd_ring_reader.o"
$CC -o "$OUT/rsd" "$OUT"/rsd_main.o "$OUT"/rsd_server.o "$OUT"/rsd_session.o "$OUT"/rsd_ring_reader.o $LIBS "$COMPY_BUILD/libcompy.a" $LDFLAGS
echo "  -> rsd"

echo "=== RIC ==="
$CC $CFLAGS -c "$RAPTOR_DIR/ric/ric_main.c" -o "$OUT/ric_main.o"
$CC $CFLAGS -c "$RAPTOR_DIR/ric/ric_daynight.c" -o "$OUT/ric_daynight.o"
$CC -o "$OUT/ric" "$OUT/ric_main.o" "$OUT/ric_daynight.o" $LIBS $LDFLAGS
echo "  -> ric"

echo "=== ROD ==="
$CC $CFLAGS -c "$RAPTOR_DIR/rod/rod_main.c" -o "$OUT/rod_main.o"
$CC $CFLAGS -c "$RAPTOR_DIR/rod/rod_render.c" -o "$OUT/rod_render.o"
$CC -o "$OUT/rod" "$OUT/rod_main.o" "$OUT/rod_render.o" $LIBS -lschrift -lm $LDFLAGS
echo "  -> rod"

echo "=== RMR ==="
$CC $CFLAGS -c "$RAPTOR_DIR/rmr/rmr_main.c" -o "$OUT/rmr_main.o"
$CC $CFLAGS -c "$RAPTOR_DIR/rmr/rmr_mux.c" -o "$OUT/rmr_mux.o"
$CC $CFLAGS -c "$RAPTOR_DIR/rmr/rmr_nal.c" -o "$OUT/rmr_nal.o"
$CC $CFLAGS -c "$RAPTOR_DIR/rmr/rmr_storage.c" -o "$OUT/rmr_storage.o"
$CC -o "$OUT/rmr" "$OUT"/rmr_main.o "$OUT"/rmr_mux.o "$OUT"/rmr_nal.o "$OUT"/rmr_storage.o $LIBS $LDFLAGS
echo "  -> rmr"

echo "=== raptorctl ==="
$CC $CFLAGS -c "$RAPTOR_DIR/raptorctl/raptorctl.c" -o "$OUT/raptorctl.o"
$CC -o "$OUT/raptorctl" "$OUT/raptorctl.o" $LIBS $LDFLAGS
echo "  -> raptorctl"

echo "=== ringdump ==="
$CC $CFLAGS -c "$RAPTOR_DIR/ringdump/ringdump.c" -o "$OUT/ringdump.o"
$CC -o "$OUT/ringdump" "$OUT/ringdump.o" $LIBS $LDFLAGS
echo "  -> ringdump"

# ── HAL daemons (mock) ──

echo "=== RVD (mock HAL) ==="
$CC $CFLAGS $HAL_CFLAGS -c "$RAPTOR_DIR/rvd/rvd_main.c" -o "$OUT/rvd_main.o"
$CC $CFLAGS $HAL_CFLAGS -c "$RAPTOR_DIR/rvd/rvd_pipeline.c" -o "$OUT/rvd_pipeline.o"
$CC $CFLAGS $HAL_CFLAGS -c "$RAPTOR_DIR/rvd/rvd_frame_loop.c" -o "$OUT/rvd_frame_loop.o"
$CC $CFLAGS $HAL_CFLAGS -c "$RAPTOR_DIR/rvd/rvd_osd.c" -o "$OUT/rvd_osd.o"
$CC -o "$OUT/rvd" "$OUT"/rvd_main.o "$OUT"/rvd_pipeline.o "$OUT"/rvd_frame_loop.o "$OUT"/rvd_osd.o $LIBS_HAL $LDFLAGS
echo "  -> rvd"

echo "=== RAD (mock HAL) ==="
$CC $CFLAGS $HAL_CFLAGS -c "$RAPTOR_DIR/rad/rad_main.c" -o "$OUT/rad_main.o"
$CC -o "$OUT/rad" "$OUT/rad_main.o" $LIBS_HAL $LDFLAGS
echo "  -> rad"

# ── Test helpers ──

echo "=== create_rings ==="
$CC $CFLAGS -c "$RAPTOR_DIR/tests/create_rings.c" -o "$OUT/create_rings.o"
$CC -o "$OUT/create_rings" "$OUT/create_rings.o" $LIBS $LDFLAGS
echo "  -> create_rings"

echo ""
echo "Done. All 8 binaries in asan-out/"
ls -1 "$OUT"/rvd "$OUT"/rsd "$OUT"/rad "$OUT"/rhd "$OUT"/rod "$OUT"/ric "$OUT"/raptorctl "$OUT"/ringdump "$OUT"/create_rings 2>/dev/null | while read f; do echo "  $(basename $f)"; done
