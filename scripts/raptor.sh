#!/bin/sh
# Raptor development launch script
# Copy to /root/raptor.sh on the device, or run directly from NFS
R=/mnt/nfs/projects/thingino/raptor/build
C=/tmp/raptor.conf

killall rvd rsd rad rhd rod ric rmr 2>/dev/null
sleep 2
cp /mnt/nfs/projects/thingino/raptor/config/raptor.conf $C

# Device-specific overrides
sed -i "s/fps = 25/fps = 30/g" $C
sed -i "s/# ao_enabled = false/ao_enabled = true/" $C

$R/rvd -c $C -f -d &
sleep 3
$R/rad -c $C -f -d &
sleep 1
$R/rod -c $C -f -d &
sleep 1
$R/rsd -c $C -f -d &
sleep 1
$R/rhd -c $C -f -d &
sleep 1
$R/ric -c $C -f -d &
sleep 1
$R/rmr -c $C -f -d &
sleep 1

echo "raptor running"
ps | grep -E "rvd|rsd|rad|rhd|rod|ric|rmr" | grep -v grep
