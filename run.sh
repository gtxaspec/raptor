#!/bin/sh
# Launch raptor daemons from NFS — run directly on device
# Usage: /mnt/nfs/projects/thingino/raptor/run.sh [config]
#
# If no config given, uses /etc/raptor.conf or generates a minimal one.

DIR="$(cd "$(dirname "$0")" && pwd)"
CONF="${1:-/etc/raptor.conf}"

if [ ! -f "$CONF" ]; then
    CONF="/tmp/raptor.conf"
    cat > "$CONF" << 'EOF'
[sensor]
[stream0]
fps = 25
codec = h264
profile = 2
bitrate = 2000000
rc_mode = vbr
gop = 50
[stream1]
enabled = true
width = 640
height = 360
fps = 25
codec = h264
profile = 0
bitrate = 500000
rc_mode = cbr
gop = 50
[ring]
main_slots = 32
main_data_mb = 0
sub_slots = 32
sub_data_mb = 0
[audio]
enabled = true
sample_rate = 16000
codec = aac
volume = 80
gain = 25
[rtsp]
enabled = true
port = 554
[http]
enabled = true
port = 8080
[osd]
enabled = true
font = /usr/share/fonts/default.ttf
font_size = 24
timestamp = true
timestamp_fmt = %Y-%m-%d %H:%M:%S
label = Camera
[recording]
enabled = true
mode = both
stream = 0
audio = true
segment_minutes = 1
storage_path = /mnt/mmcblk0p1/raptor
max_storage_mb = 500
prebuffer_sec = 5
clip_length_sec = 60
clip_max_mb = 200
[motion]
enabled = true
algorithm = move
sensitivity = 3
grid = 4x4
cooldown_sec = 10
poll_interval_ms = 500
record = true
record_post_sec = 30
[ircut]
enabled = false
EOF
    echo "Generated default config: $CONF"
fi

# Kill any running daemons
killall rvd rad rod rsd rhd rmr rmd ric 2>/dev/null
sleep 1

echo "Starting raptor from $DIR"
echo "Config: $CONF"

# RVD must start first
$DIR/rvd/rvd -c "$CONF" -f -d >> /tmp/rvd.log 2>&1 &
sleep 4

# Producers
$DIR/rad/rad -c "$CONF" -f -d >> /tmp/rad.log 2>&1 &
sleep 2
$DIR/rod/rod -c "$CONF" -f -d >> /tmp/rod.log 2>&1 &

# Consumers
$DIR/rsd/rsd -c "$CONF" -f -d >> /tmp/rsd.log 2>&1 &
$DIR/rhd/rhd -c "$CONF" -f -d >> /tmp/rhd.log 2>&1 &
$DIR/rmr/rmr -c "$CONF" -f -d >> /tmp/rmr.log 2>&1 &
sleep 1
$DIR/ric/ric -c "$CONF" -f -d >> /tmp/ric.log 2>&1 &
$DIR/rmd/rmd -c "$CONF" -f -d >> /tmp/rmd.log 2>&1 &
sleep 2

echo ""
echo "Running daemons:"
ps | grep "$DIR" | grep -v grep | awk '{print $5}' | sed "s|$DIR/||;s|/.*||" | sort
echo ""
echo "Logs in /tmp/{rvd,rad,rod,rsd,rhd,rmr,ric,rmd}.log"
