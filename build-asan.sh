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
$CC $CFLAGS -c "$COMMON_DIR/src/rss_ctrl.c" -o "$OUT/rss_ctrl_common.o"
$CC $CFLAGS -c "$COMMON_DIR/src/rss_http.c" -o "$OUT/rss_http.o"
# Build info stub
cat > "$OUT/rss_build_info.c" << 'BUILDEOF'
const char *rss_build_hash = "asan";
const char *rss_build_time = "asan-build";
BUILDEOF
$CC $CFLAGS -c "$OUT/rss_build_info.c" -o "$OUT/rss_build_info.o"
ar rcs "$OUT/librss_common.a" "$OUT"/rss_log.o "$OUT"/rss_config.o "$OUT"/rss_daemon.o "$OUT"/rss_util.o "$OUT"/rss_ctrl_common.o "$OUT"/rss_http.o "$OUT"/rss_build_info.o

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
$CC $CFLAGS -c "$RAPTOR_DIR/rhd/rhd_http.c" -o "$OUT/rhd_http.o"
$CC -o "$OUT/rhd" "$OUT/rhd_main.o" "$OUT/rhd_http.o" $LIBS $LDFLAGS
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

echo "=== RMD ==="
$CC $CFLAGS -c "$RAPTOR_DIR/rmd/rmd_main.c" -o "$OUT/rmd_main.o"
$CC $CFLAGS -c "$RAPTOR_DIR/rmd/rmd_actions.c" -o "$OUT/rmd_actions.o"
$CC -o "$OUT/rmd" "$OUT/rmd_main.o" "$OUT/rmd_actions.o" $LIBS $LDFLAGS
echo "  -> rmd"

echo "=== ROD ==="
$CC $CFLAGS -c "$RAPTOR_DIR/rod/rod_main.c" -o "$OUT/rod_main.o"
$CC $CFLAGS -c "$RAPTOR_DIR/rod/rod_render.c" -o "$OUT/rod_render.o"
$CC -o "$OUT/rod" "$OUT/rod_main.o" "$OUT/rod_render.o" $LIBS -lschrift -lm $LDFLAGS
echo "  -> rod"

echo "=== RMR ==="
$CC $CFLAGS -c "$RAPTOR_DIR/rmr/rmr_main.c" -o "$OUT/rmr_main.o"
$CC $CFLAGS -c "$RAPTOR_DIR/rmr/rmr_mux.c" -o "$OUT/rmr_mux.o"
$CC $CFLAGS -c "$RAPTOR_DIR/rmr/rmr_nal.c" -o "$OUT/rmr_nal.o"
$CC $CFLAGS -c "$RAPTOR_DIR/rmr/rmr_prebuf.c" -o "$OUT/rmr_prebuf.o"
$CC $CFLAGS -c "$RAPTOR_DIR/rmr/rmr_storage.c" -o "$OUT/rmr_storage.o"
$CC -o "$OUT/rmr" "$OUT"/rmr_main.o "$OUT"/rmr_mux.o "$OUT"/rmr_nal.o "$OUT"/rmr_prebuf.o "$OUT"/rmr_storage.o $LIBS $LDFLAGS
echo "  -> rmr"

echo "=== raptorctl ==="
$CC $CFLAGS -c "$RAPTOR_DIR/raptorctl/raptorctl.c" -o "$OUT/raptorctl.o"
$CC $CFLAGS -c "$RAPTOR_DIR/raptorctl/raptorctl_info.c" -o "$OUT/raptorctl_info.o"
$CC -o "$OUT/raptorctl" "$OUT/raptorctl.o" "$OUT/raptorctl_info.o" $LIBS $LDFLAGS
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
$CC $CFLAGS $HAL_CFLAGS -c "$RAPTOR_DIR/rvd/rvd_ctrl.c" -o "$OUT/rvd_ctrl.o"
$CC $CFLAGS $HAL_CFLAGS -c "$RAPTOR_DIR/rvd/rvd_osd.c" -o "$OUT/rvd_osd.o"
$CC $CFLAGS $HAL_CFLAGS -c "$RAPTOR_DIR/rvd/rvd_ivs.c" -o "$OUT/rvd_ivs.o"
$CC -o "$OUT/rvd" "$OUT"/rvd_main.o "$OUT"/rvd_pipeline.o "$OUT"/rvd_frame_loop.o "$OUT"/rvd_ctrl.o "$OUT"/rvd_osd.o "$OUT"/rvd_ivs.o $LIBS_HAL $LDFLAGS
echo "  -> rvd"

echo "=== RAD (mock HAL) ==="
for f in rad_main.c rad_codec.c rad_codec_g711.c rad_codec_l16.c rad_codec_aac.c rad_codec_opus.c; do
  $CC $CFLAGS $HAL_CFLAGS -c "$RAPTOR_DIR/rad/$f" -o "$OUT/${f%.c}.o"
done
$CC -o "$OUT/rad" "$OUT"/rad_main.o "$OUT"/rad_codec.o "$OUT"/rad_codec_g711.o \
    "$OUT"/rad_codec_l16.o "$OUT"/rad_codec_aac.o "$OUT"/rad_codec_opus.o \
    $LIBS_HAL $LDFLAGS
echo "  -> rad"

# ── Test helpers ──

echo "=== rac ==="
$CC $CFLAGS -c "$RAPTOR_DIR/rac/rac.c" -o "$OUT/rac.o"
$CC $CFLAGS -c "$RAPTOR_DIR/rac/rac_record.c" -o "$OUT/rac_record.o"
$CC $CFLAGS -c "$RAPTOR_DIR/rac/rac_play.c" -o "$OUT/rac_play.o"
$CC -o "$OUT/rac" "$OUT/rac.o" "$OUT/rac_record.o" "$OUT/rac_play.o" $LIBS $LDFLAGS
echo "  -> rac"

echo "=== create_rings ==="
$CC $CFLAGS -c "$RAPTOR_DIR/tests/create_rings.c" -o "$OUT/create_rings.o"
$CC -o "$OUT/create_rings" "$OUT/create_rings.o" $LIBS $LDFLAGS
echo "  -> create_rings"

echo "=== RWC ==="
$CC $CFLAGS -c "$RAPTOR_DIR/rwc/rwc_main.c" -o "$OUT/rwc_main.o"
$CC -o "$OUT/rwc" "$OUT/rwc_main.o" $LIBS $LDFLAGS
echo "  -> rwc"

# RWD is skipped in ASAN builds — requires mbedTLS with DTLS-SRTP support
# (cross-build only via build.sh with TLS=1).
echo "=== RWD ==="
echo "  (skipped — requires mbedTLS DTLS-SRTP, use build.sh for cross-build)"

echo ""
echo "Done. All binaries in asan-out/"
ls -1 "$OUT"/rvd "$OUT"/rsd "$OUT"/rad "$OUT"/rhd "$OUT"/rod "$OUT"/ric "$OUT"/rmd "$OUT"/rmr "$OUT"/rwc "$OUT"/raptorctl "$OUT"/ringdump "$OUT"/rac "$OUT"/create_rings 2>/dev/null | while read f; do echo "  $(basename $f)"; done
