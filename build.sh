#!/bin/sh
# Raptor full rebuild script
# Usage: ./build.sh [platform] [target...]
# Examples:
#   ./build.sh t31              # clean + rebuild everything for T31
#   ./build.sh t20 rvd rsd      # rebuild just rvd and rsd for T20
#   ./build.sh t31 clean        # clean only
#
# Supported platforms: t20, t21, t23, t30, t31

set -e

platform="$1"
shift 2>/dev/null || true

case "$platform" in
    t20|T20)
        PLATFORM=T20
        PROFILE="wyze_cam2_t20x_jxf23_rtl8189ftv-3.10.14-uclibc"
        ;;
    t21|T21)
        PLATFORM=T21
        PROFILE="wyze_cam2_t21x_jxf23_rtl8189ftv-3.10.14-uclibc"
        ;;
    t23|T23)
        PLATFORM=T23
        PROFILE="wyze_cam3_t23n_sc2336_atbm6031-3.10.14-uclibc"
        ;;
    t31|T31)
        PLATFORM=T31
        PROFILE="vanhua_z55i_t31x_gc4653_eth-3.10.14-uclibc"
        TUPLE="mipsel-buildroot-linux-uclibc"
        ;;
    *)
        echo "Usage: $0 <platform> [target...]"
        echo "Platforms: t20 t21 t23 t31"
        exit 1
        ;;
esac

BR_OUTPUT="$HOME/projects/thingino/thingino-firmware/output/master/$PROFILE"
TOOLCHAIN="$BR_OUTPUT/host/bin"
SYSROOT="$BR_OUTPUT/host/${TUPLE:-mipsel-thingino-linux-uclibc}/sysroot"

if [ ! -d "$TOOLCHAIN" ]; then
    echo "Toolchain not found: $TOOLCHAIN"
    echo "Build the firmware first for profile: $PROFILE"
    exit 1
fi

export PATH="$TOOLCHAIN:$PATH"

MAKE_ARGS="PLATFORM=$PLATFORM CROSS_COMPILE=mipsel-linux- SYSROOT=$SYSROOT"

echo "Building for $PLATFORM ($PROFILE)"

if [ $# -eq 0 ]; then
    make $MAKE_ARGS distclean
    exec make -j$(nproc) $MAKE_ARGS rvd rsd rad rhd rod ric rmr raptorctl ringdump
else
    exec make -j$(nproc) $MAKE_ARGS "$@"
fi
