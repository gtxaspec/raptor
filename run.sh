#!/bin/sh
# Launch raptor daemons from NFS — run directly on device
# Usage: /mnt/nfs/projects/thingino/raptor/run.sh [config]
#
# If no config given, uses /etc/raptor.conf or generates a minimal one.

DIR="$(cd "$(dirname "$0")" && pwd)"
CONF="${1:-/etc/raptor.conf}"

log() { echo "[raptor] $*"; }

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
codec = opus
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
font_color = 0xFFFFFFFF
font_stroke = 1
time_enabled = true
time_format = %Y-%m-%d %H:%M:%S
uptime_enabled = true
text_enabled = true
text_string = Camera
stream0_time_pos = top_left
stream0_uptime_pos = top_right
stream0_text_pos = top_center
stream1_time_pos = top_left
stream1_uptime_pos = top_right
stream1_text_pos = top_center
[recording]
enabled = false
mode = both
stream = 0
audio = true
segment_minutes = 1
storage_path = /mnt/mmcblk0p1/raptor
max_storage_mb = 500
prebuffer_sec = 5
clip_length_sec = 60
clip_max_mb = 200
[webrtc]
enabled = true
udp_port = 8443
http_port = 8554
max_clients = 2
cert = /etc/ssl/certs/uhttpd.crt
key = /etc/ssl/private/uhttpd.key
[webtorrent]
enabled = false
[motion]
enabled = false
algorithm = move
sensitivity = 3
grid = 4x4
cooldown_sec = 10
poll_interval_ms = 500
record = true
record_post_sec = 30
[ircut]
enabled = false
[log]
level = info
target = syslog
EOF
    log "generated default config: $CONF"
fi

# Kill any running daemons and wait for them to exit
log "stopping running daemons..."
for d in rvd rad rod rsd rhd rmr rmd ric rwd; do
    pid=$(pidof $d 2>/dev/null)
    if [ -n "$pid" ]; then
        log "  killing $d (pid $pid)"
        kill $pid 2>/dev/null
    fi
done
# Wait for all to exit (HAL teardown can take several seconds)
for i in 1 2 3 4 5 6 7 8 9 10; do
    alive=0
    for d in rvd rad rod rsd rhd rmr rmd ric rwd; do
        pidof $d >/dev/null 2>&1 && alive=1 && break
    done
    [ "$alive" = 0 ] && break
    sleep 1
done
# Force-kill anything still alive
for d in rvd rad rod rsd rhd rmr rmd ric rwd; do
    pid=$(pidof $d 2>/dev/null)
    [ -n "$pid" ] && kill -9 $pid 2>/dev/null && log "  force-killed $d"
done

# Clean stale SHM
rm -f /dev/shm/rss_ring_* /dev/shm/rss_osd_* 2>/dev/null

log "config: $CONF"
log "binaries: $DIR"
echo ""

# RVD must start first (creates rings, encoder, ISP)
log "starting rvd (video pipeline)..."
$DIR/rvd/rvd -c "$CONF" -f -d >> /tmp/rvd.log 2>&1 &
sleep 4

# Audio producer
log "starting rad (audio)..."
$DIR/rad/rad -c "$CONF" -f -d >> /tmp/rad.log 2>&1 &
sleep 2

# OSD renderer
log "starting rod (OSD)..."
$DIR/rod/rod -c "$CONF" -f -d >> /tmp/rod.log 2>&1 &

# Stream consumers
log "starting rsd (RTSP)..."
$DIR/rsd/rsd -c "$CONF" -f -d >> /tmp/rsd.log 2>&1 &

log "starting rhd (HTTP/MJPEG)..."
$DIR/rhd/rhd -c "$CONF" -f -d >> /tmp/rhd.log 2>&1 &

log "starting rmr (recording)..."
$DIR/rmr/rmr -c "$CONF" -f -d >> /tmp/rmr.log 2>&1 &

log "starting rwd (WebRTC)..."
$DIR/rwd/rwd -c "$CONF" -f -d >> /tmp/rwd.log 2>&1 &
sleep 1

# Aux daemons
log "starting ric (IR-cut)..."
$DIR/ric/ric -c "$CONF" -f -d >> /tmp/ric.log 2>&1 &

log "starting rmd (motion)..."
$DIR/rmd/rmd -c "$CONF" -f -d >> /tmp/rmd.log 2>&1 &
sleep 2

echo ""
log "all daemons started:"
for d in rvd rad rod rsd rhd rmr rwd ric rmd; do
    pid=$(pidof $d 2>/dev/null)
    if [ -n "$pid" ]; then
        printf "  %-6s pid %-6s OK\n" "$d" "$pid"
    else
        printf "  %-6s %-13s FAILED\n" "$d" ""
    fi
done
echo ""
log "logs: /tmp/{rvd,rad,rod,rsd,rhd,rmr,rwd,ric,rmd}.log"
