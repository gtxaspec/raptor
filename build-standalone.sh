#!/bin/bash
#
# build-standalone.sh — Build raptor without buildroot
#
# Downloads toolchain and all dependencies, builds everything from source.
# First run takes a few minutes (downloads). Subsequent runs are fast.
#
# Usage: ./build-standalone.sh <platform> [options]
#   platform: t20, t21, t23, t30, t31, t32, t40, t41
#
# Options:
#   --no-tls       Disable TLS (no RTSPS/WebRTC)
#   --no-aac       Disable AAC codec
#   --no-opus      Disable Opus codec
#   --no-mp3       Disable MP3 codec
#   --clean        Clean all build artifacts
#   --deps-only    Only build dependencies, not raptor
#   --libc=TYPE    uclibc (default), musl, or glibc
#
# Environment:
#   CROSS_COMPILE  Override cross-compiler prefix (skip toolchain download)
#   JOBS           Override parallel job count (default: nproc)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEPS_DIR="$SCRIPT_DIR/.deps"
SYSROOT_DIR="$DEPS_DIR/sysroot"
LOG_DIR="$DEPS_DIR/logs"

# ── Check host dependencies ──

check_host_deps() {
    local missing=""
    for cmd in git cmake make gcc curl tar; do
        command -v "$cmd" >/dev/null 2>&1 || missing="$missing $cmd"
    done
    # autotools (for opus)
    for cmd in autoconf automake libtoolize; do
        command -v "$cmd" >/dev/null 2>&1 || missing="$missing $cmd"
    done
    # meson/ninja (for faac)
    command -v meson >/dev/null 2>&1 || missing="$missing meson"
    command -v ninja >/dev/null 2>&1 || missing="$missing ninja-build"

    if [ -n "$missing" ]; then
        echo "ERROR: Missing host tools:$missing"
        echo ""
        # Detect package manager and suggest install command
        if command -v apt-get >/dev/null 2>&1; then
            # Map tool names to package names
            local pkgs=""
            for tool in $missing; do
                case "$tool" in
                    libtoolize)  pkgs="$pkgs libtool" ;;
                    ninja-build) pkgs="$pkgs ninja-build" ;;
                    *)           pkgs="$pkgs $tool" ;;
                esac
            done
            echo "Install with:  sudo apt-get install$pkgs"
        elif command -v pacman >/dev/null 2>&1; then
            echo "Install with:  sudo pacman -S$missing"
        elif command -v dnf >/dev/null 2>&1; then
            local pkgs=""
            for tool in $missing; do
                case "$tool" in
                    libtoolize)  pkgs="$pkgs libtool" ;;
                    ninja-build) pkgs="$pkgs ninja-build" ;;
                    *)           pkgs="$pkgs $tool" ;;
                esac
            done
            echo "Install with:  sudo dnf install$pkgs"
        fi
        exit 1
    fi
}

check_host_deps

# Run a build command, show output only on failure
run() {
    local name="$1"; shift
    local log="$LOG_DIR/${name}.log"
    if "$@" >"$log" 2>&1; then
        return 0
    else
        echo "FAILED! Log:"
        tail -30 "$log"
        exit 1
    fi
}

# ── Pinned dependency versions ──

INGENIC_LIB_VERSION=HEAD
COMPY_VERSION=HEAD
RAPTOR_HAL_VERSION=HEAD
RAPTOR_IPC_VERSION=HEAD
RAPTOR_COMMON_VERSION=HEAD
MBEDTLS_VERSION=v3.6.5
OPUS_VERSION=1.5.2
FAAC_VERSION=6d9b02edd268bd2f3377a388ed77dde4f34556c8
HELIX_VERSION=05f2fb0045cc294b4e0d1a1a9747b89c22c1fea4
SCHRIFT_VERSION=24737d2922b23df4a5692014f5ba03da0c296112
MUSL_SHIM_VERSION=HEAD
UCLIBC_SHIM_VERSION=HEAD

# ── Parse arguments ──

PLATFORM=""
LIBC=uclibc
OPT_TLS=1
OPT_AAC=1
OPT_OPUS=1
OPT_MP3=1
OPT_CLEAN=0
OPT_CLEAN_ALL=0
OPT_DEPS_ONLY=0
OPT_STATIC=0
OPT_LOCAL=0
JOBS="${JOBS:-$(nproc)}"

usage() {
    echo "Usage: $0 <platform> [options]"
    echo "  platform: t20, t21, t23, t30, t31, t32, t40, t41"
    echo ""
    echo "Options:"
    echo "  --no-tls       Disable TLS/WebRTC"
    echo "  --no-aac       Disable AAC codec"
    echo "  --no-opus      Disable Opus codec"
    echo "  --no-mp3       Disable MP3 codec"
    echo "  --static       Statically link optional deps (fewer .so files needed)"
    echo "  --clean        Clean build artifacts (keep downloaded deps)"
    echo "  --clean-all    Remove everything (.deps/ + build/)"
    echo "  --deps-only    Only build dependencies"
    echo "  --local        Use sibling repos instead of cloning (for development)"
    echo "  --libc=TYPE    uclibc (default), musl, or glibc"
    exit 1
}

for arg in "$@"; do
    case "$arg" in
        t10|t20|t21|t23|t30|t31|t32|t40|t41) PLATFORM="$arg" ;;
        --no-tls)    OPT_TLS=0 ;;
        --no-aac)    OPT_AAC=0 ;;
        --no-opus)   OPT_OPUS=0 ;;
        --no-mp3)    OPT_MP3=0 ;;
        --static)    OPT_STATIC=1 ;;
        --local)     OPT_LOCAL=1 ;;
        --clean)     OPT_CLEAN=1 ;;
        --clean-all) OPT_CLEAN_ALL=1 ;;
        --deps-only) OPT_DEPS_ONLY=1 ;;
        --libc=*)    LIBC="${arg#--libc=}" ;;
        -h|--help)   usage ;;
        *)           echo "Unknown option: $arg"; usage ;;
    esac
done

[ -z "$PLATFORM" ] && usage

PLATFORM_UPPER=$(echo "$PLATFORM" | tr a-z A-Z)

# ── Clean ──

if [ "$OPT_CLEAN_ALL" = 1 ] || [ "$OPT_CLEAN" = 1 ]; then
    # Clean raptor daemons first
    make clean 2>/dev/null || true

    if [ "$OPT_CLEAN_ALL" = 1 ]; then
        echo "Removing all deps and build artifacts..."
        rm -rf "$DEPS_DIR" "$SCRIPT_DIR/build"
    else
        echo "Cleaning build artifacts (keeping downloaded deps)..."
        rm -rf "$SYSROOT_DIR" "$LOG_DIR" "$SCRIPT_DIR/build"
        for d in raptor-hal raptor-ipc raptor-common; do
            [ -d "$DEPS_DIR/$d" ] && make -C "$DEPS_DIR/$d" clean 2>/dev/null || true
        done
        rm -rf "$DEPS_DIR/compy/build-cross" "$DEPS_DIR/mbedtls/build-cross"
        rm -rf "$DEPS_DIR/faac/build-cross" "$DEPS_DIR/opus/config.status"
        rm -f "$DEPS_DIR/ESP8266Audio/src/libhelix-aac/"*.o
        rm -f "$DEPS_DIR/ESP8266Audio/src/libhelix-mp3/"*.o
        rm -f "$DEPS_DIR/libschrift/schrift.o" "$DEPS_DIR/libschrift/libschrift.so" "$DEPS_DIR/libschrift/libschrift.a"
        rm -f "$DEPS_DIR/uclibc_shim.o" "$DEPS_DIR/musl_shim.o"
    fi
    echo "Done."
    exit 0
fi

# ── SDK version map ──

case "$PLATFORM" in
    t10|t20) SDK_VERSION=3.12.0; GCC_VER=4.7.2 ;;
    t21)     SDK_VERSION=1.0.33; GCC_VER=4.7.2 ;;
    t23)     SDK_VERSION=1.1.0;  GCC_VER=5.4.0 ;;
    t30)     SDK_VERSION=1.0.5;  GCC_VER=4.7.2 ;;
    t31)     SDK_VERSION=1.1.6;  GCC_VER=5.4.0 ;;
    t32)     SDK_VERSION=1.0.6;  GCC_VER=5.4.0 ;;
    t40)     SDK_VERSION=1.2.0;  GCC_VER=7.2.0 ;;
    t41)     SDK_VERSION=1.2.0;  GCC_VER=7.2.0 ;;
esac

# ── Toolchain ──

TOOLCHAIN_DIR="$DEPS_DIR/toolchain"

setup_toolchain() {
    if [ -n "$CROSS_COMPILE" ] && command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
        # Resolve to absolute path so cmake/meson can find it
        local gcc_path
        gcc_path=$(command -v "${CROSS_COMPILE}gcc")
        CROSS_COMPILE="${gcc_path%gcc}"
        echo "Using existing toolchain: ${CROSS_COMPILE}gcc"
        return
    fi

    ARCH=$(uname -m)
    case "$ARCH" in
        x86_64)  HOST_ARCH=x86_64 ;;
        aarch64) HOST_ARCH=aarch64 ;;
        *)       echo "Unsupported host: $ARCH"; exit 1 ;;
    esac

    # xburst1 = mips32r1, xburst2 = mips32r2
    case "$PLATFORM" in
        t40|t41) SOC_ARCH=xburst2 ;;
        *)       SOC_ARCH=xburst1 ;;
    esac

    TOOLCHAIN_NAME="thingino-toolchain-${HOST_ARCH}_${SOC_ARCH}_${LIBC}_gcc15-linux-mipsel"
    TOOLCHAIN_URL="https://github.com/themactep/thingino-firmware/releases/download/toolchain-${HOST_ARCH}/${TOOLCHAIN_NAME}.tar.gz"

    if [ ! -d "$TOOLCHAIN_DIR" ] || [ ! -f "$TOOLCHAIN_DIR/.version_${TOOLCHAIN_NAME}" ]; then
        echo "Downloading toolchain: $TOOLCHAIN_NAME"
        mkdir -p "$TOOLCHAIN_DIR"
        curl -fSL "$TOOLCHAIN_URL" | tar xz -C "$TOOLCHAIN_DIR" --strip-components=1
        touch "$TOOLCHAIN_DIR/.version_${TOOLCHAIN_NAME}"
    fi

    export PATH="$TOOLCHAIN_DIR/bin:$PATH"

    # Auto-detect cross-compile prefix
    for prefix in mipsel-linux- mipsel-thingino-linux-uclibc- mipsel-thingino-linux-musl- mipsel-thingino-linux-gnu-; do
        if command -v "${prefix}gcc" >/dev/null 2>&1; then
            CROSS_COMPILE="$prefix"
            break
        fi
    done

    if [ -z "$CROSS_COMPILE" ]; then
        echo "ERROR: No cross-compiler found in toolchain"
        exit 1
    fi

    echo "Toolchain: ${CROSS_COMPILE}gcc ($(${CROSS_COMPILE}gcc -dumpversion))"
}

# ── Helper: clone or update a git repo ──

clone_repo() {
    local name="$1" url="$2" version="$3" submodules="${4:-}"
    local dir="$DEPS_DIR/$name"

    # --local: symlink from sibling directory instead of cloning
    if [ "$OPT_LOCAL" = 1 ]; then
        local sibling="$SCRIPT_DIR/../$name"
        if [ -d "$sibling" ]; then
            if [ -L "$dir" ]; then
                return  # already linked
            elif [ -d "$dir" ]; then
                rm -rf "$dir"  # remove cloned copy, use local
            fi
            ln -sf "$(cd "$sibling" && pwd)" "$dir"
            echo "Using local $name → $sibling"
            return
        else
            echo "WARNING: --local but $sibling not found, cloning instead"
        fi
    fi

    if [ ! -d "$dir/.git" ]; then
        echo "Cloning $name..."
        if [ "$version" = "HEAD" ]; then
            git clone --quiet --depth 1 "$url" "$dir"
        else
            git clone --quiet "$url" "$dir"
        fi
    fi

    if [ "$version" = "HEAD" ]; then
        echo "Updating $name (latest)..."
        git -C "$dir" fetch --quiet --depth 1 origin
        git -C "$dir" reset --quiet --hard origin/main 2>/dev/null || \
        git -C "$dir" reset --quiet --hard origin/master
    else
        local current
        current=$(git -C "$dir" rev-parse HEAD 2>/dev/null || echo "")
        if [ "$current" != "$version" ]; then
            echo "Checking out $name @ ${version:0:10}..."
            git -C "$dir" fetch --quiet origin
            git -C "$dir" checkout --quiet "$version"
        fi
    fi

    if [ "$submodules" = "submodules" ]; then
        git -C "$dir" submodule update --init --quiet --depth 1
    fi
}

# ── Build functions ──

CC="${CROSS_COMPILE}gcc"
AR="${CROSS_COMPILE}ar"

build_ingenic_lib() {
    local src="$DEPS_DIR/ingenic-lib"
    # SDK libs are built for glibc or uclibc only — musl uses the uclibc builds
    local sdk_libc="$LIBC"
    [ "$sdk_libc" = "musl" ] && sdk_libc=uclibc
    local libdir="$src/$PLATFORM_UPPER/lib/$SDK_VERSION/$sdk_libc/$GCC_VER"
    # Some platforms don't have a GCC version subdirectory
    [ ! -d "$libdir" ] && libdir="$src/$PLATFORM_UPPER/lib/$SDK_VERSION/$sdk_libc"

    if [ ! -d "$libdir" ]; then
        echo "ERROR: Ingenic SDK libs not found at $libdir"
        echo "Available versions:"
        ls -d "$src/$PLATFORM_UPPER/lib/"*/*/* 2>/dev/null || echo "  (none for $PLATFORM_UPPER)"
        exit 1
    fi

    echo "Installing Ingenic SDK libs from $SDK_VERSION/$sdk_libc/$GCC_VER"
    cp -f "$libdir"/*.so "$SYSROOT_DIR/usr/lib/"

    # libalog: T40/T41 use their own, others use T31 1.1.6 uclibc
    case "$PLATFORM" in
        t40|t41)
            cp -f "$libdir/libalog.so" "$SYSROOT_DIR/usr/lib/" ;;
        *)
            if [ "$sdk_libc" = "uclibc" ]; then
                local alog="$src/T31/lib/1.1.6/uclibc/$GCC_VER/libalog.so"
            else
                local alog="$libdir/libalog.so"
            fi
            [ -f "$alog" ] && cp -f "$alog" "$SYSROOT_DIR/usr/lib/"
            ;;
    esac
}

build_libc_shim() {
    # Build both .a (preferred — statically linked into HAL daemons via
    # --whole-archive + --export-dynamic) and .so (fallback for musl where
    # executable→.so interposition doesn't work).
    local shimname="" shimsrc=""
    case "$LIBC" in
        uclibc)
            shimname=uclibcshim
            clone_repo ingenic-uclibc https://github.com/gtxaspec/ingenic-uclibc "$UCLIBC_SHIM_VERSION"
            shimsrc="$DEPS_DIR/ingenic-uclibc/uclibc_shim.c"
            ;;
        musl)
            shimname=muslshim
            clone_repo ingenic-musl https://github.com/gtxaspec/ingenic-musl "$MUSL_SHIM_VERSION"
            shimsrc="$DEPS_DIR/ingenic-musl/musl_shim.c"
            ;;
        glibc) return ;;
    esac

    [ -f "$SYSROOT_DIR/usr/lib/lib${shimname}.a" ] && return
    echo "Building $LIBC shim..."
    ${CROSS_COMPILE}gcc -fPIC -c -o "$DEPS_DIR/${shimname}.o" "$shimsrc"
    ${CROSS_COMPILE}ar rcs "$SYSROOT_DIR/usr/lib/lib${shimname}.a" "$DEPS_DIR/${shimname}.o"
    ${CROSS_COMPILE}gcc -fPIC -shared -o "$SYSROOT_DIR/usr/lib/lib${shimname}.so" "$shimsrc"
}

build_mbedtls() {
    local src="$DEPS_DIR/mbedtls"
    local builddir="$src/build-cross"
    if [ "$OPT_STATIC" = 1 ]; then
        [ -f "$SYSROOT_DIR/usr/lib/libmbedtls.a" ] && return
    else
        [ -f "$SYSROOT_DIR/usr/lib/libmbedtls.so" ] && return
    fi

    echo "Building mbedtls..."
    rm -rf "$builddir"
    mkdir -p "$builddir"
    cd "$builddir"
    # Enable DTLS-SRTP for WebRTC (disabled by default in upstream mbedtls)
    local config_h="$src/include/mbedtls/mbedtls_config.h"
    if ! grep -q "MBEDTLS_SSL_DTLS_SRTP" "$config_h" 2>/dev/null || \
       grep -q "^//#define MBEDTLS_SSL_DTLS_SRTP" "$config_h" 2>/dev/null; then
        sed -i 's|^//#define MBEDTLS_SSL_DTLS_SRTP|#define MBEDTLS_SSL_DTLS_SRTP|' "$config_h" 2>/dev/null || \
        echo "#define MBEDTLS_SSL_DTLS_SRTP" >> "$config_h"
    fi
    local shared=ON static=OFF
    if [ "$OPT_STATIC" = 1 ]; then shared=OFF; static=ON; fi
    run mbedtls-cmake cmake .. \
        -DCMAKE_C_COMPILER="${CROSS_COMPILE}gcc" \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_INSTALL_PREFIX="$SYSROOT_DIR/usr" \
        -DUSE_SHARED_MBEDTLS_LIBRARY="$shared" \
        -DUSE_STATIC_MBEDTLS_LIBRARY="$static" \
        -DENABLE_PROGRAMS=OFF \
        -DENABLE_TESTING=OFF \
        -DCMAKE_C_FLAGS="-Os"
    run mbedtls-build make -j"$JOBS"
    run mbedtls-install make install
    cd "$SCRIPT_DIR"
}

build_opus() {
    local src="$DEPS_DIR/opus"
    if [ "$OPT_STATIC" = 1 ]; then
        [ -f "$SYSROOT_DIR/usr/lib/libopus.a" ] && return
    else
        [ -f "$SYSROOT_DIR/usr/lib/libopus.so" ] && return
    fi

    echo "Building opus..."
    cd "$src"
    # Use system autotools, not toolchain's (which has broken perl paths)
    if [ ! -f configure ]; then
        local save_path="$PATH"
        export PATH="/usr/bin:/usr/local/bin:$PATH"
        run opus-autogen ./autogen.sh
        export PATH="$save_path"
    fi
    local shared_flag="--disable-static --enable-shared"
    [ "$OPT_STATIC" = 1 ] && shared_flag="--enable-static --disable-shared"
    run opus-configure ./configure --host=mipsel-linux --prefix="$SYSROOT_DIR/usr" \
        $shared_flag --disable-doc --disable-extra-programs \
        CC="${CROSS_COMPILE}gcc"
    run opus-build make -j"$JOBS"
    run opus-install make install
    cd "$SCRIPT_DIR"
}

build_faac() {
    local src="$DEPS_DIR/faac"
    if [ "$OPT_STATIC" = 1 ]; then
        [ -f "$SYSROOT_DIR/usr/lib/libfaac.a" ] && return
    else
        [ -f "$SYSROOT_DIR/usr/lib/libfaac.so" ] && return
    fi

    echo "Building faac..."
    cd "$src"
    # Write meson cross file
    local crossfile="$DEPS_DIR/meson-cross.txt"
    cat > "$crossfile" <<MESON
[binaries]
c = '${CROSS_COMPILE}gcc'
ar = '${CROSS_COMPILE}ar'
strip = '${CROSS_COMPILE}strip'
[host_machine]
system = 'linux'
cpu_family = 'mips'
cpu = 'mips32'
endian = 'little'
MESON
    local libtype=shared
    [ "$OPT_STATIC" = 1 ] && libtype=static
    rm -rf build-cross
    run faac-setup meson setup build-cross --cross-file "$crossfile" \
        --prefix="$SYSROOT_DIR/usr" \
        -Dfloating-point=single -Dmax-channels=2 \
        -Ddefault_library="$libtype"
    run faac-build ninja -C build-cross -j"$JOBS"
    run faac-install ninja -C build-cross install
    cd "$SCRIPT_DIR"
}

setup_helix_stubs() {
    local stubdir="$DEPS_DIR/helix-stubs"
    [ -f "$stubdir/pgmspace.h" ] && return
    mkdir -p "$stubdir"
    cat > "$stubdir/Arduino.h" <<'H'
#ifndef ARDUINO_H
#define ARDUINO_H
#include <stdint.h>
#include "pgmspace.h"
#endif
H
    cat > "$stubdir/pgmspace.h" <<'H'
#ifndef PGMSPACE_H
#define PGMSPACE_H
#include <stdint.h>
#include <string.h>
#define PROGMEM
#define PGM_P const char *
#define pgm_read_byte(x) (*(const uint8_t *)(x))
#define pgm_read_word(x) (*(const uint16_t *)(x))
#define pgm_read_dword(x) (*(const uint32_t *)(x))
#define memcpy_P memcpy
#endif
H
}

build_helix_aac() {
    local src="$DEPS_DIR/ESP8266Audio/src/libhelix-aac"
    local ext=so; [ "$OPT_STATIC" = 1 ] && ext=a
    [ -f "$SYSROOT_DIR/usr/lib/libhelix-aac.$ext" ] && return

    echo "Building libhelix-aac..."
    setup_helix_stubs
    local stubdir="$DEPS_DIR/helix-stubs"
    local objs=""
    for f in "$src"/*.c; do
        ${CROSS_COMPILE}gcc -Os -fPIC -DUSE_DEFAULT_STDLIB -I"$stubdir" -I"$src" -c "$f" -o "${f%.c}.o"
        objs="$objs ${f%.c}.o"
    done
    if [ "$OPT_STATIC" = 1 ]; then
        ${CROSS_COMPILE}ar rcs "$SYSROOT_DIR/usr/lib/libhelix-aac.a" $objs
    else
        ${CROSS_COMPILE}gcc -shared -o "$SYSROOT_DIR/usr/lib/libhelix-aac.so" $objs
    fi
    cp -f "$src"/*.h "$SYSROOT_DIR/usr/include/"
}

build_helix_mp3() {
    local src="$DEPS_DIR/ESP8266Audio/src/libhelix-mp3"
    local ext=so; [ "$OPT_STATIC" = 1 ] && ext=a
    [ -f "$SYSROOT_DIR/usr/lib/libhelix-mp3.$ext" ] && return

    echo "Building libhelix-mp3..."
    setup_helix_stubs
    local stubdir="$DEPS_DIR/helix-stubs"
    local objs=""
    for f in "$src"/*.c; do
        ${CROSS_COMPILE}gcc -Os -fPIC -DUSE_DEFAULT_STDLIB -DARDUINO -I"$stubdir" -I"$src" -c "$f" -o "${f%.c}.o"
        objs="$objs ${f%.c}.o"
    done
    if [ "$OPT_STATIC" = 1 ]; then
        ${CROSS_COMPILE}ar rcs "$SYSROOT_DIR/usr/lib/libhelix-mp3.a" $objs
    else
        ${CROSS_COMPILE}gcc -shared -o "$SYSROOT_DIR/usr/lib/libhelix-mp3.so" $objs
    fi
    cp -f "$src"/*.h "$SYSROOT_DIR/usr/include/"
}

build_schrift() {
    local src="$DEPS_DIR/libschrift"
    local ext=so; [ "$OPT_STATIC" = 1 ] && ext=a
    [ -f "$SYSROOT_DIR/usr/lib/libschrift.$ext" ] && return

    echo "Building libschrift..."
    cd "$src"
    ${CROSS_COMPILE}gcc -Os -std=c99 -fPIC -c schrift.c -o schrift.o
    if [ "$OPT_STATIC" = 1 ]; then
        ${CROSS_COMPILE}ar rcs libschrift.a schrift.o
        cp -f libschrift.a "$SYSROOT_DIR/usr/lib/"
    else
        ${CROSS_COMPILE}gcc -shared -o libschrift.so schrift.o
        cp -f libschrift.so "$SYSROOT_DIR/usr/lib/"
    fi
    cp -f schrift.h "$SYSROOT_DIR/usr/include/"
    cd "$SCRIPT_DIR"
}

build_raptor_common() {
    local src="$DEPS_DIR/raptor-common"
    [ -f "$SYSROOT_DIR/usr/lib/librss_common.a" ] && return

    echo "Building raptor-common..."
    make -C "$src" clean 2>/dev/null || true
    run raptor-common make -C "$src" CC="${CROSS_COMPILE}gcc" AR="${CROSS_COMPILE}ar" -j"$JOBS"
    cp -f "$src/librss_common.a" "$SYSROOT_DIR/usr/lib/"
    cp -f "$src/include/rss_common.h" "$SYSROOT_DIR/usr/include/"
    cp -f "$src/include/rss_net.h" "$SYSROOT_DIR/usr/include/"
}

build_raptor_ipc() {
    local src="$DEPS_DIR/raptor-ipc"
    [ -f "$SYSROOT_DIR/usr/lib/librss_ipc.a" ] && return

    echo "Building raptor-ipc..."
    make -C "$src" clean 2>/dev/null || true
    run raptor-ipc make -C "$src" CC="${CROSS_COMPILE}gcc" AR="${CROSS_COMPILE}ar" -j"$JOBS"
    cp -f "$src/librss_ipc.a" "$SYSROOT_DIR/usr/lib/"
    cp -f "$src/include/rss_ipc.h" "$SYSROOT_DIR/usr/include/"
}

build_raptor_hal() {
    local src="$DEPS_DIR/raptor-hal"
    [ -f "$SYSROOT_DIR/usr/lib/libraptor_hal_video.a" ] && return

    echo "Building raptor-hal ($PLATFORM_UPPER)..."
    make -C "$src" clean 2>/dev/null || true
    run raptor-hal make -C "$src" \
        PLATFORM="$PLATFORM_UPPER" \
        CROSS_COMPILE="$CROSS_COMPILE" \
        INGENIC_HEADERS="$src/ingenic-headers" \
        -j"$JOBS"
    cp -f "$src/libraptor_hal_video.a" "$SYSROOT_DIR/usr/lib/"
    cp -f "$src/libraptor_hal_audio.a" "$SYSROOT_DIR/usr/lib/"
    cp -f "$src/include/raptor_hal.h" "$SYSROOT_DIR/usr/include/"
}

build_compy() {
    local src="$DEPS_DIR/compy"
    local builddir="$src/build-cross"
    [ -f "$SYSROOT_DIR/usr/lib/libcompy.a" ] && return

    echo "Building compy..."
    rm -rf "$builddir"
    mkdir -p "$builddir"
    cd "$builddir"

    local tls_opt=OFF
    [ "$OPT_TLS" = 1 ] && tls_opt=ON

    run compy-cmake cmake .. \
        -DCMAKE_C_COMPILER="${CROSS_COMPILE}gcc" \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCOMPY_SHARED=OFF \
        -DCOMPY_TLS_MBEDTLS="$tls_opt" \
        -DCMAKE_C_FLAGS="-I$SYSROOT_DIR/usr/include" \
        -DCMAKE_PREFIX_PATH="$SYSROOT_DIR/usr"
    run compy-build make -j"$JOBS"

    cp -f libcompy.a "$SYSROOT_DIR/usr/lib/"
    mkdir -p "$SYSROOT_DIR/usr/include/compy"
    cp -f "$src/include/compy.h" "$SYSROOT_DIR/usr/include/"
    cp -a "$src/include/compy/"* "$SYSROOT_DIR/usr/include/compy/"
    # Header-only transitive deps
    for dep in slice99 datatype99 interface99; do
        [ -d "$builddir/_deps/${dep}-src" ] && \
            cp -f "$builddir/_deps/${dep}-src/"*.h "$SYSROOT_DIR/usr/include/"
    done
    [ -d "$builddir/_deps/metalang99-src/include" ] && \
        cp -f "$builddir/_deps/metalang99-src/include/metalang99.h" "$SYSROOT_DIR/usr/include/" && \
        cp -a "$builddir/_deps/metalang99-src/include/metalang99" "$SYSROOT_DIR/usr/include/"

    cd "$SCRIPT_DIR"
}

# ── Main ──

echo "=== Raptor standalone build ==="
echo "Platform:  $PLATFORM_UPPER (SDK $SDK_VERSION)"
echo "Features:  TLS=$OPT_TLS AAC=$OPT_AAC OPUS=$OPT_OPUS MP3=$OPT_MP3 STATIC=$OPT_STATIC LOCAL=$OPT_LOCAL"
echo "Deps dir:  $DEPS_DIR"
echo ""

# Set up toolchain
setup_toolchain

# Create sysroot and log dir
mkdir -p "$SYSROOT_DIR/usr/lib" "$SYSROOT_DIR/usr/include" "$SYSROOT_DIR/lib" "$LOG_DIR"

# Clone all repos
clone_repo ingenic-lib  https://github.com/gtxaspec/ingenic-lib     "$INGENIC_LIB_VERSION"
clone_repo raptor-hal   https://github.com/gtxaspec/raptor-hal       "$RAPTOR_HAL_VERSION"  submodules
clone_repo raptor-ipc   https://github.com/gtxaspec/raptor-ipc       "$RAPTOR_IPC_VERSION"
clone_repo raptor-common https://github.com/gtxaspec/raptor-common   "$RAPTOR_COMMON_VERSION"
clone_repo compy        https://github.com/gtxaspec/compy            "$COMPY_VERSION"
clone_repo libschrift   https://github.com/tomolt/libschrift         "$SCHRIFT_VERSION"

[ "$OPT_TLS" = 1 ] && clone_repo mbedtls https://github.com/Mbed-TLS/mbedtls "$MBEDTLS_VERSION" submodules
[ "$OPT_OPUS" = 1 ] && clone_repo opus https://github.com/xiph/opus "v$OPUS_VERSION"
[ "$OPT_AAC" = 1 ] && clone_repo faac https://github.com/knik0/faac "$FAAC_VERSION"
if [ "$OPT_AAC" = 1 ] || [ "$OPT_MP3" = 1 ]; then
    clone_repo ESP8266Audio https://github.com/earlephilhower/ESP8266Audio "$HELIX_VERSION"
fi

# Build deps in order
build_ingenic_lib
build_libc_shim
[ "$OPT_TLS" = 1 ]  && build_mbedtls
[ "$OPT_OPUS" = 1 ] && build_opus
[ "$OPT_AAC" = 1 ]  && build_faac
[ "$OPT_AAC" = 1 ]  && build_helix_aac
[ "$OPT_MP3" = 1 ]  && build_helix_mp3
build_schrift
build_raptor_common
build_raptor_ipc
build_raptor_hal
build_compy

echo ""
echo "All dependencies built."

if [ "$OPT_DEPS_ONLY" = 1 ]; then
    echo "Deps installed to: $SYSROOT_DIR"
    exit 0
fi

# Build raptor
echo ""
echo "Building raptor daemons..."

COMPY_CFLAGS="-I$SYSROOT_DIR/usr/include"
[ "$OPT_TLS" = 1 ] && COMPY_CFLAGS="$COMPY_CFLAGS -DCOMPY_HAS_TLS"

TARGETS="rvd rsd rad rhd rod ric rmr rmd raptorctl ringdump rac"
[ "$OPT_TLS" = 1 ] && TARGETS="$TARGETS rwd"

make -j"$JOBS" \
    PLATFORM="$PLATFORM_UPPER" \
    CROSS_COMPILE="$CROSS_COMPILE" \
    SYSROOT="$SYSROOT_DIR" \
    HAL_DIR=".deps/raptor-hal" \
    IPC_DIR=".deps/raptor-ipc" \
    COMMON_DIR=".deps/raptor-common" \
    COMPY_DIR=".deps/compy" \
    LIB_HAL_VIDEO="$SYSROOT_DIR/usr/lib/libraptor_hal_video.a" \
    LIB_HAL_AUDIO="$SYSROOT_DIR/usr/lib/libraptor_hal_audio.a" \
    LIB_IPC="$SYSROOT_DIR/usr/lib/librss_ipc.a" \
    LIB_IPC_FILE="$SYSROOT_DIR/usr/lib/librss_ipc.a" \
    LIB_COMMON="$SYSROOT_DIR/usr/lib/librss_common.a" \
    LIB_COMMON_FILE="$SYSROOT_DIR/usr/lib/librss_common.a" \
    LIB_COMPY="$SYSROOT_DIR/usr/lib/libcompy.a" \
    LIB_COMPY_FILE="$SYSROOT_DIR/usr/lib/libcompy.a" \
    COMPY_CFLAGS="$COMPY_CFLAGS" \
    EXTRA_CFLAGS="-I$SYSROOT_DIR/usr/include" \
    ${OPT_TLS:+TLS=1 WEBTORRENT=1} \
    ${OPT_AAC:+AAC=1} \
    ${OPT_OPUS:+OPUS=1} \
    ${OPT_MP3:+MP3=1} \
    $TARGETS

# Collect binaries
mkdir -p "$SCRIPT_DIR/build"
for d in rvd rsd rad rhd rod ric rmr rmd rwd raptorctl ringdump rac; do
    [ -f "$SCRIPT_DIR/$d/$d" ] && cp -f "$SCRIPT_DIR/$d/$d" "$SCRIPT_DIR/build/"
done

echo ""
echo "=== Build complete ==="
ls -lh "$SCRIPT_DIR/build/"
