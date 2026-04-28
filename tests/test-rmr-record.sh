#!/bin/bash
#
# test-rmr-record.sh -- On-device RMR recording validation
#
# Records two 1-minute motion clips on a test camera and validates
# the output fMP4 files with ffprobe on the host.
#
#   Clip 1: video + audio
#   Clip 2: video only
#
# Prerequisites:
#   - Device reachable via SSH (root@IP, key auth)
#   - Raptor built for the target platform (binaries in build/)
#   - SD card mounted at /mnt/mmcblk0p1 with >=100MB free
#   - NFS at /mnt/nfs on device (maps to $HOME on host)
#   - ffprobe on the build host
#
# Usage:
#   ./tests/test-rmr-record.sh <device-ip>
#   ./tests/test-rmr-record.sh <device-ip> --duration 30

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RAPTOR_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

DEVICE_IP=""
CLIP_DURATION=60

while [ $# -gt 0 ]; do
    case "$1" in
        --duration) CLIP_DURATION="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 <device-ip> [--duration <seconds>]"
            exit 0 ;;
        -*) echo "Unknown option: $1"; exit 1 ;;
        *) DEVICE_IP="$1"; shift ;;
    esac
done

[ -z "$DEVICE_IP" ] && { echo "Usage: $0 <device-ip> [--duration <seconds>]"; exit 1; }

# ── Paths ──

SSH="ssh -o StrictHostKeyChecking=no -o LogLevel=ERROR root@$DEVICE_IP"
DEVICE_BUILD="/mnt/nfs/projects/thingino/raptor/build"
DEVICE_SD="/mnt/mmcblk0p1/raptor-test"
HOST_TMP="$HOME/tmp/rmr-test"
DEVICE_NFS_TMP="/mnt/nfs/tmp/rmr-test"
RAPTORCTL="$DEVICE_BUILD/raptorctl"

PASS=0
FAIL=0
SKIP=0
RAPTOR_DAEMONS="rvd rad rsd rhd rod ric rmd rmr rwd rwc rsp rfs"

# ── Helpers ──

cleanup() {
    set +e
    echo ""
    echo "=== Cleanup ==="
    $SSH "killall $RAPTOR_DAEMONS 2>/dev/null" 2>/dev/null
    sleep 1
    $SSH "killall -9 $RAPTOR_DAEMONS 2>/dev/null" 2>/dev/null
    $SSH "rm -rf $DEVICE_SD" 2>/dev/null
    $SSH 'rm -f /dev/shm/rss_ring_* /dev/shm/rss_osd_* /var/run/rss/*.pid /var/run/rss/*.sock' 2>/dev/null
    rm -rf "$HOST_TMP"
    echo "    done"
    set -e
}
trap cleanup EXIT

pass() { PASS=$((PASS + 1)); printf "    PASS  %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "    FAIL  %s: %s\n" "$1" "$2"; }
skip() { SKIP=$((SKIP + 1)); printf "    SKIP  %s: %s\n" "$1" "$2"; }

check_range() {
    local name="$1" actual="$2" expected="$3" tolerance_pct="$4"
    if [ -z "$actual" ] || [ "$actual" = "null" ] || [ "$actual" = "N/A" ]; then
        fail "$name" "no value"; return
    fi
    local low high
    low=$(( expected * (100 - tolerance_pct) / 100 ))
    high=$(( expected * (100 + tolerance_pct) / 100 ))
    if [ "$actual" -ge "$low" ] 2>/dev/null && [ "$actual" -le "$high" ] 2>/dev/null; then
        pass "$name ($actual, expected $expected +/-${tolerance_pct}%)"
    else
        fail "$name" "got $actual, expected $expected +/-${tolerance_pct}% ($low-$high)"
    fi
}

write_config() {
    local audio_enabled="$1"
    cat > "$HOST_TMP/test.conf" << EOF
[sensor]

[stream0]
width = 1920
height = 1080
fps = $SENSOR_FPS
bitrate = 2000000
codec = h264
rc_mode = cbr
gop = $SENSOR_FPS

[audio]
enabled = $audio_enabled
sample_rate = 16000
codec = pcma

[ring]
refmode = true

[osd]
enabled = false

[ircut]
enabled = false

[motion]
enabled = false

[recording]
enabled = true
mode = motion
stream = 0
audio = $audio_enabled
storage_path = $DEVICE_SD
prebuffer_sec = 0
clip_length_sec = $((CLIP_DURATION + 60))

[log]
level = info
EOF
}

# Record a clip and copy it to host.
# $1 = clip name (clip1, clip2), $2 = audio enabled (true/false)
record_clip() {
    local name="$1" audio="$2"
    local conf_device="$DEVICE_NFS_TMP/test.conf"

    write_config "$audio"

    local audio_label="video + audio"
    [ "$audio" = "false" ] && audio_label="video only"
    echo "=== $name: $audio_label (${CLIP_DURATION}s) ==="

    # Clean SD from previous clip
    $SSH "rm -rf $DEVICE_SD/*" 2>/dev/null || true
    $SSH "mkdir -p $DEVICE_SD" 2>/dev/null

    $SSH "$DEVICE_BUILD/rmr -c $conf_device -d" 2>/dev/null
    sleep 2

    if ! $SSH 'pidof rmr > /dev/null 2>&1' 2>/dev/null; then
        fail "$name" "RMR failed to start"
        return
    fi
    pass "$name RMR running"

    echo "    recording ${CLIP_DURATION}s..."
    $SSH "timeout $((CLIP_DURATION + 30)) $RAPTORCTL test-motion $CLIP_DURATION" 2>/dev/null
    sleep 3

    $SSH 'killall rmr 2>/dev/null' 2>/dev/null || true
    sleep 2

    local clip_path
    clip_path=$($SSH "find $DEVICE_SD -name '*.mp4' -type f 2>/dev/null | head -1" 2>/dev/null || echo "")
    if [ -z "$clip_path" ]; then
        fail "$name" "no mp4 file found on SD card"
        return
    fi

    local clip_size
    clip_size=$($SSH "stat -c%s '$clip_path'" 2>/dev/null || echo "0")
    if [ "$clip_size" -lt 1024 ]; then
        fail "$name" "file too small (${clip_size} bytes)"
        return
    fi
    pass "$name recorded ($((clip_size / 1024))KB)"

    $SSH "cat '$clip_path'" > "$HOST_TMP/${name}.mp4" 2>/dev/null
    if [ -f "$HOST_TMP/${name}.mp4" ] && [ "$(stat -c%s "$HOST_TMP/${name}.mp4")" -gt 0 ]; then
        pass "$name copied to host"
    else
        fail "$name" "failed to pull file from device"
    fi
    echo ""
}

# Validate an fMP4 clip with ffprobe.
# $1 = host path, $2 = display name, $3 = expect audio (true/false)
validate_clip() {
    local clip="$1" name="$2" expect_audio="$3"

    if [ ! -f "$clip" ]; then
        fail "$name" "file not found on host"
        return
    fi

    # Container format
    local format
    format=$(ffprobe -v error -show_entries format=format_name -of csv=p=0 "$clip" 2>/dev/null || echo "")
    if echo "$format" | grep -q 'mov'; then
        pass "$name container ($format)"
    else
        fail "$name container" "got '$format', expected mov/mp4"
    fi

    # Duration
    local duration
    duration=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$clip" 2>/dev/null || echo "0")
    local dur_int=${duration%%.*}
    [ -z "$dur_int" ] && dur_int=0
    if [ "$dur_int" -ge $((CLIP_DURATION - 5)) ] && [ "$dur_int" -le $((CLIP_DURATION + 5)) ]; then
        pass "$name duration (${dur_int}s)"
    else
        fail "$name duration" "got ${dur_int}s, expected ${CLIP_DURATION} +/-5"
    fi

    # Video codec
    local video_codec
    video_codec=$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$clip" 2>/dev/null || echo "")
    if [ "$video_codec" = "h264" ] || [ "$video_codec" = "hevc" ]; then
        pass "$name video codec ($video_codec)"
    else
        fail "$name video codec" "got '$video_codec'"
    fi

    # Video resolution
    local width height
    width=$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$clip" 2>/dev/null || echo "0")
    height=$(ffprobe -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$clip" 2>/dev/null || echo "0")
    if [ "$width" = "1920" ] && [ "$height" = "1080" ]; then
        pass "$name resolution (${width}x${height})"
    else
        fail "$name resolution" "got ${width}x${height}, expected 1920x1080"
    fi

    # Frame count (slow for fMP4 — must count packets)
    local nb_frames
    nb_frames=$(ffprobe -v error -select_streams v:0 -count_frames \
        -show_entries stream=nb_read_frames -of csv=p=0 "$clip" 2>/dev/null || echo "0")
    local expected_frames=$((SENSOR_FPS * CLIP_DURATION))
    if [ -n "$nb_frames" ] && [ "$nb_frames" != "0" ] && [ "$nb_frames" != "N/A" ]; then
        check_range "$name frame count" "$nb_frames" "$expected_frames" 15
    else
        skip "$name frame count" "could not determine"
    fi

    # Audio stream
    local audio_codec
    audio_codec=$(ffprobe -v error -select_streams a:0 -show_entries stream=codec_name -of csv=p=0 "$clip" 2>/dev/null || echo "")
    if [ "$expect_audio" = "true" ]; then
        if [ -n "$audio_codec" ]; then
            pass "$name audio present ($audio_codec)"
        else
            fail "$name audio" "expected audio stream, none found"
        fi
    else
        if [ -z "$audio_codec" ]; then
            pass "$name no audio (expected)"
        else
            fail "$name audio" "found '$audio_codec', expected none"
        fi
    fi
}

# ══════════════════════════════════════════════════════════════
#  Main
# ══════════════════════════════════════════════════════════════

echo "========================================"
echo " RMR recording test: $DEVICE_IP"
echo " Clip duration: ${CLIP_DURATION}s"
echo "========================================"
echo ""

# ── Preflight ──

echo "=== Preflight ==="

if ! $SSH 'true' 2>/dev/null; then
    echo "ERROR: cannot SSH to root@$DEVICE_IP"; exit 1
fi
pass "SSH connectivity"

SD_AVAIL=$($SSH 'df -k /mnt/mmcblk0p1 2>/dev/null | awk "NR==2{print \$4}"' 2>/dev/null || echo "0")
SD_AVAIL_MB=$((SD_AVAIL / 1024))
if [ "$SD_AVAIL_MB" -lt 100 ]; then
    echo "ERROR: SD card has ${SD_AVAIL_MB}MB free (need 100MB)"; exit 1
fi
pass "SD card (${SD_AVAIL_MB}MB free)"

if ! $SSH "test -x $DEVICE_BUILD/rvd" 2>/dev/null; then
    echo "ERROR: $DEVICE_BUILD/rvd not found — check NFS mount and build"; exit 1
fi
pass "binaries accessible"

if ! command -v ffprobe > /dev/null 2>&1; then
    echo "ERROR: ffprobe not found on host"; exit 1
fi
pass "ffprobe on host"

SENSOR_FPS=$($SSH 'cat /sys/module/sensor_*/parameters/sensor_max_fps 2>/dev/null' 2>/dev/null || echo "")
[ -z "$SENSOR_FPS" ] || [ "$SENSOR_FPS" = "0" ] && \
    SENSOR_FPS=$($SSH 'cat /proc/jz/sensor/fps 2>/dev/null' 2>/dev/null || echo "25")
echo "    sensor fps: $SENSOR_FPS"

# Clean slate
$SSH "killall $RAPTOR_DAEMONS 2>/dev/null" 2>/dev/null || true
sleep 2
$SSH "killall -9 $RAPTOR_DAEMONS 2>/dev/null" 2>/dev/null || true
$SSH 'rm -f /dev/shm/rss_ring_* /dev/shm/rss_osd_* /var/run/rss/*.pid /var/run/rss/*.sock 2>/dev/null' 2>/dev/null || true
pass "clean slate"

mkdir -p "$HOST_TMP"
$SSH "mkdir -p $DEVICE_SD" 2>/dev/null
echo ""

# ── Start pipeline (RVD + RAD stay up for both clips) ──

echo "=== Pipeline ==="

write_config "true"
CONF_DEVICE="$DEVICE_NFS_TMP/test.conf"

$SSH "$DEVICE_BUILD/rvd -c $CONF_DEVICE -d" 2>/dev/null
sleep 4

if ! $SSH 'pidof rvd > /dev/null 2>&1' 2>/dev/null; then
    echo "ERROR: RVD failed to start"; exit 1
fi
pass "RVD running"

if ! $SSH 'test -e /dev/shm/rss_ring_main' 2>/dev/null; then
    echo "ERROR: video ring not created"; exit 1
fi
pass "video ring"

$SSH "LD_PRELOAD=/usr/lib/libimp-nodbg.so $DEVICE_BUILD/rad -c $CONF_DEVICE -d" 2>/dev/null
sleep 2

if ! $SSH 'pidof rad > /dev/null 2>&1' 2>/dev/null; then
    echo "ERROR: RAD failed to start"; exit 1
fi
pass "RAD running"

if ! $SSH 'test -e /dev/shm/rss_ring_audio' 2>/dev/null; then
    echo "ERROR: audio ring not created"; exit 1
fi
pass "audio ring"
echo ""

# ── Record clips ──

record_clip "clip1" "true"
record_clip "clip2" "false"

# ── Validate ──

echo "=== Validation ==="
echo ""

echo "--- clip1 (video + audio) ---"
validate_clip "$HOST_TMP/clip1.mp4" "clip1" "true"
echo ""

echo "--- clip2 (video only) ---"
validate_clip "$HOST_TMP/clip2.mp4" "clip2" "false"
echo ""

# ── Summary ──

echo "========================================"
echo " Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo " Device:  $DEVICE_IP"
echo "========================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
