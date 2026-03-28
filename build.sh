#!/bin/sh
# Raptor full rebuild script
# Usage: ./build.sh <platform> <br_output> [target...]
# Examples:
#   ./build.sh t31 /path/to/buildroot/output
#   ./build.sh t20 /path/to/buildroot/output rvd rsd
#   ./build.sh t31 /path/to/buildroot/output clean

set -e

platform="$1"
br_output="$2"
shift 2 2>/dev/null || true

case "$platform" in
    t20|T20) PLATFORM=T20 ;;
    t21|T21) PLATFORM=T21 ;;
    t23|T23) PLATFORM=T23 ;;
    t30|T30) PLATFORM=T30 ;;
    t31|T31) PLATFORM=T31 ;;
    t32|T32) PLATFORM=T32 ;;
    t40|T40) PLATFORM=T40 ;;
    t41|T41) PLATFORM=T41 ;;
    *)
        echo "Usage: $0 <platform> <br_output> [target...]"
        echo "Platforms: t20 t21 t23 t30 t31 t32 t40 t41"
        echo ""
        echo "  <br_output> is the buildroot output directory containing"
        echo "  host/ with the cross-compiler and sysroot."
        exit 1
        ;;
esac

if [ -z "$br_output" ] || [ ! -d "$br_output" ]; then
    echo "Error: buildroot output directory required"
    echo "Usage: $0 <platform> <br_output> [target...]"
    exit 1
fi

TOOLCHAIN="$br_output/host/bin"

# Auto-detect sysroot tuple
SYSROOT=""
for tuple in mipsel-buildroot-linux-uclibc mipsel-thingino-linux-uclibc; do
    if [ -d "$br_output/host/$tuple/sysroot" ]; then
        SYSROOT="$br_output/host/$tuple/sysroot"
        break
    fi
done

if [ ! -d "$TOOLCHAIN" ]; then
    echo "Toolchain not found: $TOOLCHAIN"
    exit 1
fi

if [ -z "$SYSROOT" ]; then
    echo "Sysroot not found in $br_output/host/"
    exit 1
fi

export PATH="$TOOLCHAIN:$PATH"

MAKE_ARGS="PLATFORM=$PLATFORM CROSS_COMPILE=mipsel-linux- SYSROOT=$SYSROOT AAC=1 OPUS=1"

# Auto-detect TLS support
if [ -f "$SYSROOT/usr/lib/libmbedtls.so" ] || [ -f "$SYSROOT/lib/libmbedtls.so" ]; then
    MAKE_ARGS="$MAKE_ARGS TLS=1"
fi

echo "Building for $PLATFORM"
echo "  Output:  $br_output"
echo "  Sysroot: $SYSROOT"

if [ $# -eq 0 ]; then
    make $MAKE_ARGS distclean
    make -j$(nproc) $MAKE_ARGS rvd rsd rad rhd rod ric rmr rmd raptorctl ringdump rac
    exec make $MAKE_ARGS build
else
    exec make -j$(nproc) $MAKE_ARGS "$@"
fi
