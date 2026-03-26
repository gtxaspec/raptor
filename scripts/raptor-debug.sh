#!/bin/sh
# Raptor debug launch — start only the daemons you need
# Usage: raptor-debug.sh [daemons...]
# Example: raptor-debug.sh rvd        # just video
#          raptor-debug.sh rvd rsd     # video + rtsp
#          raptor-debug.sh             # all (default)

R=/mnt/nfs/projects/thingino/raptor/build
C=/tmp/raptor.conf

# Kill everything first
killall rvd rsd rad rhd rod ric rmr 2>/dev/null
sleep 2

# Copy config
cp /mnt/nfs/projects/thingino/raptor/config/raptor.conf $C

# Device-specific overrides
sed -i "s/fps = 25/fps = 30/g" $C
sed -i "s/# ao_enabled = false/ao_enabled = true/" $C
sed -i "s/level = info/level = debug/" $C
sed -i "s/enabled = false/enabled = true/" $C
sed -i "s/segment_minutes = 5/segment_minutes = 1/" $C
# codec override if needed
# sed -i "s/codec = aac/codec = l16/" $C

# Default: all daemons
DAEMONS="${@:-rvd rad rod rsd rhd ric}"

for d in $DAEMONS; do
    case $d in
        rvd) $R/rvd -c $C -f -d & sleep 3 ;;
        rad) $R/rad -c $C -f -d & sleep 1 ;;
        rod) $R/rod -c $C -f -d & sleep 1 ;;
        rsd) $R/rsd -c $C -f -d & sleep 1 ;;
        rhd) $R/rhd -c $C -f -d & sleep 1 ;;
        ric) $R/ric -c $C -f -d & sleep 1 ;;
        rmr) $R/rmr -c $C -f -d & sleep 1 ;;
        *)   echo "unknown daemon: $d" ;;
    esac
done

echo "running:"
ps | grep -E "rvd|rsd|rad|rhd|rod|ric|rmr" | grep -v grep
