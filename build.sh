#!/bin/sh
# Raptor full rebuild script
# Usage: ./build.sh [target...]
# Examples:
#   ./build.sh              # clean + rebuild everything
#   ./build.sh rvd rsd      # rebuild just rvd and rsd
#   ./build.sh clean        # clean only

PROFILE="wyze_cam3_t31al_gc2053_atbm6031-3.10.14-uclibc"
BR_OUTPUT="$HOME/projects/thingino/thingino-firmware/output/master/$PROFILE"
TOOLCHAIN="$BR_OUTPUT/host/bin"
SYSROOT="$BR_OUTPUT/host/mipsel-thingino-linux-uclibc/sysroot"

export PATH="$TOOLCHAIN:$PATH"

MAKE_ARGS="PLATFORM=T31 CROSS_COMPILE=mipsel-linux- SYSROOT=$SYSROOT"

if [ $# -eq 0 ]; then
    # Full clean rebuild
    make $MAKE_ARGS distclean
    exec make -j$(nproc) $MAKE_ARGS rvd rsd rad rhd rod ric raptorctl ringdump
else
    exec make -j$(nproc) $MAKE_ARGS "$@"
fi
