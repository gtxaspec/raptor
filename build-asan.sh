#!/bin/sh
# Build ALL raptor daemons for host (x86_64) with sanitizers.
#
# Automatically clones sibling repos (raptor-common, raptor-ipc, raptor-hal,
# compy) into asan-out/deps/ if not already present. No manual setup needed.
#
# RVD/RAD use a mock HAL (tests/mock_hal.c) that stubs IMP SDK calls.
# All other daemons build natively — no HAL dependency.
#
# Usage:
#   ./build-asan.sh              # build with ASan (memory safety)
#   ./build-asan.sh tsan         # build with TSan (thread safety)
#   ./build-asan.sh clean        # clean (keeps deps, use distclean to remove)
#   ./build-asan.sh distclean    # clean everything including cloned deps
#
# Test:
#   ./asan-out/create_rings &          # dummy SHM rings
#   ./asan-out/rvd -c config/raptor.conf -f -d &
#   ./tests/test-integration.sh        # automated test suite

set -e

RAPTOR_DIR="$(cd "$(dirname "$0")" && pwd)"

# Select sanitizer
SAN_MODE="asan"
if [ "$1" = "tsan" ]; then
    SAN_MODE="tsan"
    shift
fi

if [ "$SAN_MODE" = "tsan" ]; then
    SANITIZE="-fsanitize=thread -fno-omit-frame-pointer"
    SAN_LABEL="TSan"
else
    SANITIZE="-fsanitize=address,undefined -fno-omit-frame-pointer"
    SAN_LABEL="ASan"
fi

OUT="$RAPTOR_DIR/asan-out"
DEPS="$OUT/deps"

if [ "$1" = "clean" ]; then
    rm -f "$OUT"/*.o "$OUT"/*.a "$OUT"/rss_build_info.c
    rm -f "$OUT"/rvd "$OUT"/rsd "$OUT"/rad "$OUT"/rhd "$OUT"/rod "$OUT"/ric
    rm -f "$OUT"/rmd "$OUT"/rmr "$OUT"/rwc "$OUT"/rwd "$OUT"/rsp
    rm -f "$OUT"/raptorctl "$OUT"/ringdump "$OUT"/rac "$OUT"/create_rings
    rm -rf "$OUT"/mbedtls-build "$OUT"/mbedtls-install "$OUT"/compy-build
    echo "Cleaned asan-out/ (deps kept — use distclean to remove)"
    exit 0
fi

if [ "$1" = "distclean" ]; then
    rm -rf "$OUT"
    echo "Cleaned asan-out/ including deps"
    exit 0
fi

echo "Building with $SAN_LABEL ($SANITIZE)"

mkdir -p "$OUT" "$DEPS"

# Always clean raptor .o files — no dependency tracking, stale objects
# cause silent bugs. Dep libs (mbedtls, compy, schrift) are cached.
rm -f "$OUT"/*.o "$OUT"/rss_build_info.c

# Auto-clean dep libs when switching between ASAN and TSAN —
# incompatible instrumentation causes link errors.
STAMP="$OUT/.sanitizer"
PREV_SAN=""
[ -f "$STAMP" ] && PREV_SAN=$(cat "$STAMP")
if [ -n "$PREV_SAN" ] && [ "$PREV_SAN" != "$SAN_LABEL" ]; then
    echo "  sanitizer changed ($PREV_SAN -> $SAN_LABEL), cleaning dep libs"
    rm -rf "$OUT"/mbedtls-build "$OUT"/mbedtls-install "$OUT"/compy-build
    rm -f "$OUT"/schrift.o "$OUT"/*.a
fi
echo "$SAN_LABEL" > "$STAMP"

# ── Clone/update dependencies ──

clone_or_pull() {
    local name="$1" url="$2" dir="$DEPS/$name"
    if [ -d "$dir/.git" ]; then
        echo "  update  $name"
        git -C "$dir" pull --ff-only -q 2>/dev/null || true
    else
        echo "  clone   $name"
        git clone --depth 1 -q "$url" "$dir"
    fi
}

# Use sibling repos if they exist, otherwise clone into deps/
find_or_clone() {
    local name="$1" url="$2" varname="$3"
    local sibling="$RAPTOR_DIR/../$name"
    if [ -d "$sibling" ]; then
        eval "$varname=\"$sibling\""
    else
        clone_or_pull "$name" "$url"
        eval "$varname=\"$DEPS/$name\""
    fi
}

echo "=== Dependencies ==="
find_or_clone raptor-common https://github.com/gtxaspec/raptor-common.git COMMON_DIR
find_or_clone raptor-ipc    https://github.com/gtxaspec/raptor-ipc.git    IPC_DIR
find_or_clone raptor-hal    https://github.com/gtxaspec/raptor-hal.git    HAL_DIR
find_or_clone compy         https://github.com/gtxaspec/compy.git         COMPY_DIR
find_or_clone libschrift    https://github.com/tomolt/libschrift.git      SCHRIFT_DIR
find_or_clone faac          https://github.com/knik0/faac.git             FAAC_DIR

# Build mbedTLS from source with DTLS-SRTP enabled
MBEDTLS_VER="3.6.6"
MBEDTLS_DIR="$DEPS/mbedtls-$MBEDTLS_VER"
MBEDTLS_BUILD="$OUT/mbedtls-build"
MBEDTLS_PREFIX="$OUT/mbedtls-install"
if [ ! -f "$MBEDTLS_PREFIX/lib/libmbedtls.a" ]; then
    if [ ! -d "$MBEDTLS_DIR" ]; then
        echo "  fetch   mbedtls-$MBEDTLS_VER"
        curl -sL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VER/mbedtls-$MBEDTLS_VER.tar.bz2" \
            | tar xj -C "$DEPS"
    fi
    echo "=== mbedTLS $MBEDTLS_VER (cmake) ==="
    # Enable DTLS-SRTP in the source config (idempotent)
    sed -i 's|//#define MBEDTLS_SSL_DTLS_SRTP|#define MBEDTLS_SSL_DTLS_SRTP|' \
        "$MBEDTLS_DIR/include/mbedtls/mbedtls_config.h"
    sed -i 's|//#define MBEDTLS_SSL_PROTO_DTLS|#define MBEDTLS_SSL_PROTO_DTLS|' \
        "$MBEDTLS_DIR/include/mbedtls/mbedtls_config.h"
    mkdir -p "$MBEDTLS_BUILD"
    cmake -S "$MBEDTLS_DIR" -B "$MBEDTLS_BUILD" \
        -DCMAKE_C_FLAGS="$SANITIZE -O1 -g" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_INSTALL_PREFIX="$MBEDTLS_PREFIX" \
        -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
        > /dev/null 2>&1
    cmake --build "$MBEDTLS_BUILD" -j"$(nproc)" > /dev/null 2>&1
    cmake --install "$MBEDTLS_BUILD" > /dev/null 2>&1
    echo "  -> libmbedtls.a (DTLS-SRTP enabled)"
fi
MBEDTLS_CFLAGS="-I$MBEDTLS_PREFIX/include"
MBEDTLS_LIBS="$MBEDTLS_PREFIX/lib/libmbedtls.a $MBEDTLS_PREFIX/lib/libmbedx509.a $MBEDTLS_PREFIX/lib/libmbedcrypto.a"

# Build compy for x86 with TLS
COMPY_BUILD="$OUT/compy-build"
if [ ! -f "$COMPY_BUILD/libcompy.a" ]; then
    echo "=== compy (cmake + mbedTLS) ==="
    mkdir -p "$COMPY_BUILD"
    cmake -S "$COMPY_DIR" -B "$COMPY_BUILD" \
        -DCMAKE_C_FLAGS="$SANITIZE -O1 -g $MBEDTLS_CFLAGS" \
        -DCMAKE_BUILD_TYPE=Debug -DCOMPY_SHARED=OFF \
        -DCOMPY_TLS_MBEDTLS=ON \
        -DCMAKE_PREFIX_PATH="$MBEDTLS_PREFIX" \
        > /dev/null 2>&1
    cmake --build "$COMPY_BUILD" -j"$(nproc)" > /dev/null 2>&1
    echo "  -> libcompy.a (with TLS)"
fi

# Compy include paths (fetched by cmake)
COMPY_CFLAGS="-I$COMPY_DIR/include -DCOMPY_HAS_TLS"
COMPY_CFLAGS="$COMPY_CFLAGS -I$COMPY_BUILD/_deps/slice99-src"
COMPY_CFLAGS="$COMPY_CFLAGS -I$COMPY_BUILD/_deps/datatype99-src"
COMPY_CFLAGS="$COMPY_CFLAGS -I$COMPY_BUILD/_deps/interface99-src"
COMPY_CFLAGS="$COMPY_CFLAGS -I$COMPY_BUILD/_deps/metalang99-src/include"

# ── Compiler setup ──

CC=gcc
CFLAGS="-Wall -Wextra -Werror -std=gnu11 -D_GNU_SOURCE -DPLATFORM_T31 -O1 -g $SANITIZE"
CFLAGS="$CFLAGS -I$IPC_DIR/include -I$COMMON_DIR/include $MBEDTLS_CFLAGS"
LDFLAGS="$SANITIZE -lpthread -lrt -lm"

HAL_CFLAGS="-I$HAL_DIR/include"
TLS_CFLAGS="-DRSS_HAS_TLS"

# ── Libraries ──

echo "=== raptor-common ==="
$CC $CFLAGS -c "$COMMON_DIR/src/rss_log.c" -o "$OUT/rss_log.o"
$CC $CFLAGS -c "$COMMON_DIR/src/rss_config.c" -o "$OUT/rss_config.o"
$CC $CFLAGS -c "$COMMON_DIR/src/rss_daemon.c" -o "$OUT/rss_daemon.o"
$CC $CFLAGS -c "$COMMON_DIR/src/rss_util.c" -o "$OUT/rss_util.o"
$CC $CFLAGS -c "$COMMON_DIR/src/rss_ctrl_cmds.c" -o "$OUT/rss_ctrl_common.o"
$CC $CFLAGS -c "$COMMON_DIR/src/rss_http.c" -o "$OUT/rss_http.o"
$CC $CFLAGS -c "$COMMON_DIR/src/cJSON.c" -o "$OUT/cJSON.o"
cat > "$OUT/rss_build_info.c" << 'BUILDEOF'
const char *rss_build_hash = "asan";
const char *rss_build_time = "asan-build";
const char *rss_build_platform = "x86_64";
BUILDEOF
$CC $CFLAGS -c "$OUT/rss_build_info.c" -o "$OUT/rss_build_info.o"
ar rcs "$OUT/librss_common.a" "$OUT"/rss_log.o "$OUT"/rss_config.o "$OUT"/rss_daemon.o "$OUT"/rss_util.o "$OUT"/rss_ctrl_common.o "$OUT"/rss_http.o "$OUT"/cJSON.o

echo "=== raptor-ipc ==="
$CC $CFLAGS -c "$IPC_DIR/src/rss_ring.c" -o "$OUT/rss_ring.o"
$CC $CFLAGS -c "$IPC_DIR/src/rss_osd_shm.c" -o "$OUT/rss_osd_shm.o"
$CC $CFLAGS -c "$IPC_DIR/src/rss_ctrl.c" -o "$OUT/rss_ctrl.o"
$CC $CFLAGS -c "$IPC_DIR/src/rss_ipc_log.c" -o "$OUT/rss_ipc_log.o"
ar rcs "$OUT/librss_ipc.a" "$OUT/rss_ring.o" "$OUT/rss_osd_shm.o" "$OUT/rss_ctrl.o" "$OUT/rss_ipc_log.o"

echo "=== mock_hal ==="
$CC $CFLAGS $HAL_CFLAGS -c "$RAPTOR_DIR/tests/mock_hal.c" -o "$OUT/mock_hal.o"
ar rcs "$OUT/libmock_hal.a" "$OUT/mock_hal.o"

echo "=== rss_tls ==="
$CC $CFLAGS $TLS_CFLAGS -c "$COMMON_DIR/src/rss_tls.c" -o "$OUT/rss_tls.o"

LIBS="$OUT/librss_ipc.a $OUT/librss_common.a $OUT/rss_build_info.o"
LIBS_HAL="$OUT/libmock_hal.a $LIBS"
LIBS_TLS="$OUT/rss_tls.o $MBEDTLS_LIBS"

# ── Non-HAL daemons ──

echo "=== RHD ==="
$CC $CFLAGS $TLS_CFLAGS -c "$RAPTOR_DIR/rhd/rhd_main.c" -o "$OUT/rhd_main.o"
$CC $CFLAGS $TLS_CFLAGS -c "$RAPTOR_DIR/rhd/rhd_http.c" -o "$OUT/rhd_http.o"
$CC $CFLAGS -c "$RAPTOR_DIR/rhd/rhd_audio.c" -o "$OUT/rhd_audio.o"
$CC -o "$OUT/rhd" "$OUT/rhd_main.o" "$OUT/rhd_http.o" "$OUT/rhd_audio.o" $LIBS $LIBS_TLS $LDFLAGS
echo "  -> rhd"

echo "=== RSD ==="
$CC $CFLAGS $COMPY_CFLAGS -c "$RAPTOR_DIR/rsd/rsd_main.c" -o "$OUT/rsd_main.o"
$CC $CFLAGS $COMPY_CFLAGS -c "$RAPTOR_DIR/rsd/rsd_server.c" -o "$OUT/rsd_server.o"
$CC $CFLAGS $COMPY_CFLAGS -c "$RAPTOR_DIR/rsd/rsd_session.c" -o "$OUT/rsd_session.o"
$CC $CFLAGS $COMPY_CFLAGS -c "$RAPTOR_DIR/rsd/rsd_ring_reader.c" -o "$OUT/rsd_ring_reader.o"
$CC -o "$OUT/rsd" "$OUT"/rsd_main.o "$OUT"/rsd_server.o "$OUT"/rsd_session.o "$OUT"/rsd_ring_reader.o $LIBS "$COMPY_BUILD/libcompy.a" $MBEDTLS_LIBS $LDFLAGS
echo "  -> rsd"

echo "=== RIC ==="
$CC $CFLAGS -c "$RAPTOR_DIR/ric/ric_main.c" -o "$OUT/ric_main.o"
$CC $CFLAGS -c "$RAPTOR_DIR/ric/ric_daynight.c" -o "$OUT/ric_daynight.o"
$CC $CFLAGS -c "$RAPTOR_DIR/ric/ric_photo.c" -o "$OUT/ric_photo.o"
$CC -o "$OUT/ric" "$OUT/ric_main.o" "$OUT/ric_daynight.o" "$OUT/ric_photo.o" $LIBS $LDFLAGS
echo "  -> ric"

echo "=== RMD ==="
$CC $CFLAGS -c "$RAPTOR_DIR/rmd/rmd_main.c" -o "$OUT/rmd_main.o"
$CC $CFLAGS -c "$RAPTOR_DIR/rmd/rmd_actions.c" -o "$OUT/rmd_actions.o"
$CC -o "$OUT/rmd" "$OUT/rmd_main.o" "$OUT/rmd_actions.o" $LIBS $LDFLAGS
echo "  -> rmd"

echo "=== ROD ==="
# Compile libschrift's single .c directly into rod — avoids depending on
# a system-wide install (CI runs on fresh ubuntu-latest with nothing in
# /usr/lib). Sanitizer flags apply to schrift.c too, which is actually
# desirable: it exercises the font path under ASan/TSan.
#
# libschrift carries a static reallocarray() polyfill for older libcs;
# glibc 2.26+ exports reallocarray too, which creates a static-vs-
# non-static redeclaration conflict. Patch the polyfill's forward
# decl and definition to use a different name so both can coexist.
# Idempotent: skips the sed if already renamed.
if ! grep -q 'schrift_reallocarray' "$SCHRIFT_DIR/schrift.c"; then
    sed -i 's|\breallocarray\b|schrift_reallocarray|g' "$SCHRIFT_DIR/schrift.c"
fi
if [ ! -f "$OUT/schrift.o" ]; then
    # -w silences upstream warnings in libschrift (calloc-transposed-args,
    # _POSIX_C_SOURCE redefine). Our own code still builds with -Wall.
    $CC $CFLAGS -w -c "$SCHRIFT_DIR/schrift.c" -o "$OUT/schrift.o"
fi
for f in rod_main rod_ctrl rod_elem rod_config rod_template rod_render rod_receipt; do
    $CC $CFLAGS -I"$SCHRIFT_DIR" -c "$RAPTOR_DIR/rod/$f.c" -o "$OUT/$f.o"
done
$CC -o "$OUT/rod" "$OUT"/rod_main.o "$OUT"/rod_ctrl.o "$OUT"/rod_elem.o "$OUT"/rod_config.o \
    "$OUT"/rod_template.o "$OUT"/rod_render.o "$OUT"/rod_receipt.o "$OUT/schrift.o" $LIBS -lm $LDFLAGS
echo "  -> rod"

echo "=== RMR ==="
$CC $CFLAGS -c "$RAPTOR_DIR/rmr/rmr_main.c" -o "$OUT/rmr_main.o"
$CC $CFLAGS -c "$RAPTOR_DIR/rmr/rmr_mux.c" -o "$OUT/rmr_mux.o"
$CC $CFLAGS -c "$RAPTOR_DIR/rmr/rmr_nal.c" -o "$OUT/rmr_nal.o"
$CC $CFLAGS -c "$RAPTOR_DIR/rmr/rmr_prebuf.c" -o "$OUT/rmr_prebuf.o"
$CC $CFLAGS -c "$RAPTOR_DIR/rmr/rmr_storage.c" -o "$OUT/rmr_storage.o"
$CC -o "$OUT/rmr" "$OUT"/rmr_main.o "$OUT"/rmr_mux.o "$OUT"/rmr_nal.o "$OUT"/rmr_prebuf.o "$OUT"/rmr_storage.o $LIBS $LDFLAGS
echo "  -> rmr"

echo "=== RSP ==="
RSP_CFLAGS="$TLS_CFLAGS -I$RAPTOR_DIR/rmr"
$CC $CFLAGS $RSP_CFLAGS -c "$RAPTOR_DIR/rsp/rsp_main.c" -o "$OUT/rsp_main.o"
$CC $CFLAGS $RSP_CFLAGS -c "$RAPTOR_DIR/rsp/rsp_rtmp.c" -o "$OUT/rsp_rtmp.o"
$CC $CFLAGS $RSP_CFLAGS -c "$RAPTOR_DIR/rsp/rsp_audio.c" -o "$OUT/rsp_audio.o"
$CC -o "$OUT/rsp" "$OUT"/rsp_main.o "$OUT"/rsp_rtmp.o "$OUT"/rsp_audio.o "$OUT"/rmr_nal.o $LIBS $LIBS_TLS $LDFLAGS
echo "  -> rsp"

echo "=== raptorctl ==="
for f in raptorctl raptorctl_dispatch raptorctl_config raptorctl_ipc raptorctl_help raptorctl_info; do
    $CC $CFLAGS -c "$RAPTOR_DIR/raptorctl/$f.c" -o "$OUT/$f.o"
done
$CC -o "$OUT/raptorctl" "$OUT"/raptorctl.o "$OUT"/raptorctl_dispatch.o "$OUT"/raptorctl_config.o \
    "$OUT"/raptorctl_ipc.o "$OUT"/raptorctl_help.o "$OUT"/raptorctl_info.o $LIBS $LDFLAGS
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
# Build libfaac from the clone — not packaged in Debian/Ubuntu due to
# licensing. Needs a minimal config.h (upstream's Makefile/autotools
# build chain is bypassed; mirrors thingino-firmware's faac.mk).
FAAC_BUILD="$OUT/faac-build"
if [ ! -f "$FAAC_BUILD/libfaac.a" ]; then
    echo "=== libfaac (from clone) ==="
    mkdir -p "$FAAC_BUILD"
    printf '%s\n' \
        '#define PACKAGE "faac"' \
        '#define PACKAGE_VERSION "1.40.0"' \
        '#define HAVE_GETOPT_H 1' \
        '#define HAVE_STDINT_H 1' \
        '#define HAVE_SYS_TIME_H 1' \
        '#define HAVE_SYS_TYPES_H 1' \
        '#define HAVE_STRCASECMP 1' \
        '#define FAAC_PRECISION_SINGLE 1' \
        '#define MAX_CHANNELS 2' \
        > "$FAAC_BUILD/config.h"
    FAAC_SRCS="bitstream.c blockswitch.c channels.c cpu_compute.c fft.c \
               filtbank.c frame.c huff2.c huffdata.c quantize.c stereo.c \
               tns.c util.c"
    # -w silences upstream libfaac warnings (sign-compare, unused-parameter,
    # missing-field-initializers). Not our code to fix.
    for f in $FAAC_SRCS; do
        $CC $CFLAGS -w -DHAVE_CONFIG_H -I"$FAAC_BUILD" \
            -I"$FAAC_DIR" -I"$FAAC_DIR/include" -fPIC \
            -c "$FAAC_DIR/libfaac/$f" -o "$FAAC_BUILD/${f%.c}.o"
    done
    ar rcs "$FAAC_BUILD/libfaac.a" "$FAAC_BUILD"/*.o
    echo "  -> libfaac.a"
fi
FAAC_CFLAGS="-I$FAAC_DIR/include"
FAAC_LIBS="$FAAC_BUILD/libfaac.a"

for f in rad_main.c rad_codec.c rad_codec_g711.c rad_codec_l16.c rad_codec_aac.c rad_codec_opus.c; do
  $CC $CFLAGS $HAL_CFLAGS $FAAC_CFLAGS -c "$RAPTOR_DIR/rad/$f" -o "$OUT/${f%.c}.o"
done
$CC -o "$OUT/rad" "$OUT"/rad_main.o "$OUT"/rad_codec.o "$OUT"/rad_codec_g711.o \
    "$OUT"/rad_codec_l16.o "$OUT"/rad_codec_aac.o "$OUT"/rad_codec_opus.o \
    $LIBS_HAL $FAAC_LIBS -lopus $LDFLAGS
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

echo "=== RWD ==="
RWD_CFLAGS="$CFLAGS $COMPY_CFLAGS $TLS_CFLAGS -DMBEDTLS_ALLOW_PRIVATE_ACCESS -DRAPTOR_WEBTORRENT"
$CC $RWD_CFLAGS -c "$RAPTOR_DIR/rwd/rwd_main.c" -o "$OUT/rwd_main.o"
$CC $RWD_CFLAGS -c "$RAPTOR_DIR/rwd/rwd_signaling.c" -o "$OUT/rwd_signaling.o"
$CC $RWD_CFLAGS -c "$RAPTOR_DIR/rwd/rwd_sdp.c" -o "$OUT/rwd_sdp.o"
$CC $RWD_CFLAGS -c "$RAPTOR_DIR/rwd/rwd_ice.c" -o "$OUT/rwd_ice.o"
$CC $RWD_CFLAGS -c "$RAPTOR_DIR/rwd/rwd_dtls.c" -o "$OUT/rwd_dtls.o"
$CC $RWD_CFLAGS -c "$RAPTOR_DIR/rwd/rwd_media.c" -o "$OUT/rwd_media.o"
$CC $RWD_CFLAGS -c "$RAPTOR_DIR/rwd/rwd_webtorrent.c" -o "$OUT/rwd_webtorrent.o"
$CC -o "$OUT/rwd" "$OUT"/rwd_main.o "$OUT"/rwd_signaling.o "$OUT"/rwd_sdp.o \
    "$OUT"/rwd_ice.o "$OUT"/rwd_dtls.o "$OUT"/rwd_media.o "$OUT"/rwd_webtorrent.o \
    $LIBS "$COMPY_BUILD/libcompy.a" $LIBS_TLS -lopus $LDFLAGS
echo "  -> rwd"

echo ""
echo "Done. All binaries in asan-out/"
ls -1 "$OUT"/rvd "$OUT"/rsd "$OUT"/rad "$OUT"/rhd "$OUT"/rod "$OUT"/ric "$OUT"/rmd "$OUT"/rmr "$OUT"/rwd "$OUT"/rwc "$OUT"/rsp "$OUT"/raptorctl "$OUT"/ringdump "$OUT"/rac "$OUT"/create_rings 2>/dev/null | while read f; do echo "  $(basename $f)"; done
