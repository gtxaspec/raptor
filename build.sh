#!/bin/sh
# Raptor full rebuild script
# Usage: ./build.sh <platform> <br_output> [target...]
# Examples:
#   ./build.sh t31 /path/to/buildroot/output
#   ./build.sh t20 /path/to/buildroot/output rvd rsd
#   ./build.sh t31 /path/to/buildroot/output clean
#   ./build.sh infinity6e /path/to/openipc-firmware/output rod

set -e

platform="$1"
br_output="$2"
if [ $# -ge 2 ]; then
    shift 2
fi

case "$platform" in
    t10|T10) PLATFORM=T10 ;;
    t20|T20) PLATFORM=T20 ;;
    t21|T21) PLATFORM=T21 ;;
    t23|T23) PLATFORM=T23 ;;
    t30|T30) PLATFORM=T30 ;;
    t31|T31) PLATFORM=T31 ;;
    t32|T32) PLATFORM=T32 ;;
    t33|T33) PLATFORM=T33 ;;
    t40|T40) PLATFORM=T40 ;;
    t41|T41) PLATFORM=T41 ;;
    a1|A1)   PLATFORM=A1 ;;
    infinity6e|INFINITY6E|ssc30kq|SSC30KQ) PLATFORM=INFINITY6E ;;
    *)
        echo "Usage: $0 <platform> <br_output> [target...]"
        echo "Platforms: t10 t20 t21 t23 t30 t31 t32 t33 t40 t41 a1 infinity6e"
        echo ""
        echo "  <br_output> is the buildroot output directory containing"
        echo "  host/ with the cross-compiler and sysroot."
        exit 1
        ;;
esac

# Everything above is Ingenic and mipsel; Infinity6E is SigmaStar and ARM.
# That splits both the sysroot tuple and the compiler prefix, so neither can
# stay hardcoded below.
case "$PLATFORM" in
    INFINITY6E)
        SYSROOT_TUPLES="arm-buildroot-linux-gnueabihf arm-openipc-linux-gnueabihf \
                        arm-thingino-linux-gnueabihf arm-buildroot-linux-musleabihf"
        # OpenIPC's Buildroot installs arm-openipc-*-gcc against an
        # arm-buildroot-* sysroot, so the prefix and the tuple are not the
        # same string and the prefix has to be looked for separately.
        CROSS_CANDIDATES="arm-openipc-linux-gnueabihf- arm-buildroot-linux-gnueabihf- \
                          arm-linux-gnueabihf-"
        CROSS_GLOB="arm"
        ;;
    *)
        SYSROOT_TUPLES="mipsel-buildroot-linux-uclibc mipsel-thingino-linux-uclibc \
                        mipsel-buildroot-linux-musl mipsel-thingino-linux-musl"
        CROSS_CANDIDATES="mipsel-linux-"
        CROSS_GLOB="mipsel"
        ;;
esac

if [ -z "$br_output" ] || [ ! -d "$br_output" ]; then
    echo "Error: buildroot output directory required"
    echo "Usage: $0 <platform> <br_output> [target...]"
    exit 1
fi

TOOLCHAIN="$br_output/host/bin"

# Auto-detect sysroot tuple (uclibc or musl on Ingenic, gnueabihf on ARM)
SYSROOT=""
for tuple in $SYSROOT_TUPLES; do
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
    # Unquoted on purpose: word-splitting collapses the line continuations
    # in the list above into single spaces.
    echo "  looked for:" $SYSROOT_TUPLES
    exit 1
fi

# Named candidates first so a working setup keeps the prefix it already used,
# then fall back to whatever <arch>*-gcc the toolchain actually installed.
CROSS_COMPILE=""
for c in $CROSS_CANDIDATES; do
    if [ -x "$TOOLCHAIN/${c}gcc" ]; then
        CROSS_COMPILE="$c"
        break
    fi
done
if [ -z "$CROSS_COMPILE" ]; then
    for gcc in "$TOOLCHAIN/$CROSS_GLOB"*-gcc; do
        [ -x "$gcc" ] || continue
        CROSS_COMPILE="$(basename "$gcc" gcc)"
        break
    done
fi

if [ -z "$CROSS_COMPILE" ]; then
    echo "No $CROSS_GLOB cross-compiler found in $TOOLCHAIN"
    echo "  looked for:" $CROSS_CANDIDATES
    exit 1
fi

export PATH="$TOOLCHAIN:$PATH"

MAKE_ARGS="PLATFORM=$PLATFORM CROSS_COMPILE=$CROSS_COMPILE SYSROOT=$SYSROOT AAC=1 OPUS=1 MP3=1"

# Auto-detect TLS support
if [ -f "$SYSROOT/usr/lib/libmbedtls.so" ] || [ -f "$SYSROOT/lib/libmbedtls.so" ]; then
    MAKE_ARGS="$MAKE_ARGS TLS=1 WEBTORRENT=1"
fi

echo "Building for $PLATFORM"
echo "  Output:  $br_output"
echo "  Sysroot: $SYSROOT"
echo "  Cross:   $CROSS_COMPILE"

if [ $# -eq 0 ]; then
    make $MAKE_ARGS distclean
    make -j$(nproc) $MAKE_ARGS rvd rsd rad rhd rod ric rmr rmd rwd raptorctl ringdump rac
    exec make $MAKE_ARGS build
else
    exec make -j$(nproc) $MAKE_ARGS "$@"
fi
