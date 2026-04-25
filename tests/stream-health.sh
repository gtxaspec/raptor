#!/bin/bash
#
# stream-health.sh — RTSP stream health test
#
# Captures RTSP streams for 30-60 seconds per audio codec and analyzes
# for timestamp monotonicity, frame timing, codec compliance, A/V sync,
# and sustained streaming correctness.
#
# Usage:
#   ./tests/stream-health.sh <device-ip>
#   ./tests/stream-health.sh <device-ip> --duration 60
#   ./tests/stream-health.sh <device-ip> --codec pcmu   # single codec
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RAPTOR_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

DOCKER_NFS_CONTAINER=""
NFS_PORT=12049
NFS_MOUNTD_PORT=12048

cleanup_on_exit() {
    set +e
    if [ -n "${DEVICE_IP:-}" ]; then
        ssh -o ConnectTimeout=3 -o StrictHostKeyChecking=no -o LogLevel=ERROR \
            "root@$DEVICE_IP" \
            'umount -f -l /tmp/raptor-test 2>/dev/null; killall rvd rad rsd rhd 2>/dev/null' \
            2>/dev/null
    fi
    if [ -n "$DOCKER_NFS_CONTAINER" ]; then
        docker rm -f "$DOCKER_NFS_CONTAINER" > /dev/null 2>&1
    fi
    # Keep logs, clean temp captures
    rm -f /tmp/raptor-stream-*.mkv 2>/dev/null
    set -e
}
trap cleanup_on_exit EXIT

DEVICE_IP=""
DURATION=60
CODEC_FILTER=""

while [ $# -gt 0 ]; do
    case "$1" in
        --duration) DURATION="$2"; shift 2 ;;
        --codec) CODEC_FILTER="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 <device-ip> [--duration <sec>] [--codec <codec>]"
            exit 0 ;;
        -*) echo "Unknown option: $1"; exit 1 ;;
        *) DEVICE_IP="$1"; shift ;;
    esac
done

[ -z "$DEVICE_IP" ] && { echo "Usage: $0 <device-ip> [--duration <sec>] [--codec <codec>]"; exit 1; }

DOCKER_NFS_CONTAINER="raptor-nfs-$(echo "$DEVICE_IP" | tr '.' '-')"
docker rm -f "$DOCKER_NFS_CONTAINER" > /dev/null 2>&1 || true

SSH="ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no -o LogLevel=ERROR root@$DEVICE_IP"
DEVICE_MNT="/tmp/raptor-test"
BUILD_DIR="$RAPTOR_DIR/build"
TEST_CONF="$RAPTOR_DIR/tests/stream-health.conf"
RTSP_PORT=554
PASS=0
FAIL=0
SKIP=0

# Log directory — keeps frame CSVs and error details for analysis
RUN_ID=$(date +%Y%m%d-%H%M%S)
LOG_DIR="/tmp/raptor-stream-health-${RUN_ID}"
mkdir -p "$LOG_DIR"

RAPTOR_DAEMONS="rvd rad rsd rhd rod ric rmd rmr rwd rwc rsp rfs"

# ── Helpers ──

pass() { PASS=$((PASS + 1)); printf "    PASS  %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "    FAIL  %s: %s\n" "$1" "$2"; }
skip() { SKIP=$((SKIP + 1)); printf "    SKIP  %s: %s\n" "$1" "$2"; }

# ── Preflight ──

echo "========================================"
echo " Stream health test: $DEVICE_IP"
echo " Duration: ${DURATION}s per codec"
echo "========================================"
echo ""

echo "=== Preflight ==="

# Use bundled ffmpeg/ffprobe if available, otherwise download
TOOLS_DIR="$SCRIPT_DIR/tools"
if [ -x "$TOOLS_DIR/ffmpeg" ] && [ -x "$TOOLS_DIR/ffprobe" ]; then
    FFMPEG="$TOOLS_DIR/ffmpeg"
    FFPROBE="$TOOLS_DIR/ffprobe"
else
    echo "    downloading ffmpeg static build..."
    mkdir -p "$TOOLS_DIR"
    curl -sL "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-linux64-gpl.tar.xz" \
        -o /tmp/ffmpeg-latest.tar.xz
    tar -xf /tmp/ffmpeg-latest.tar.xz -C /tmp/
    cp /tmp/ffmpeg-master-latest-linux64-gpl/bin/ffmpeg "$TOOLS_DIR/"
    cp /tmp/ffmpeg-master-latest-linux64-gpl/bin/ffprobe "$TOOLS_DIR/"
    rm -rf /tmp/ffmpeg-master-latest-linux64-gpl /tmp/ffmpeg-latest.tar.xz
    FFMPEG="$TOOLS_DIR/ffmpeg"
    FFPROBE="$TOOLS_DIR/ffprobe"
fi
echo "    ffmpeg: $($FFMPEG -version 2>/dev/null | head -1 | awk '{print $3}')"

if ! $SSH 'true' 2>/dev/null; then
    echo "ERROR: cannot SSH to root@$DEVICE_IP"; exit 1
fi
pass "SSH connectivity"

$SSH "killall $RAPTOR_DAEMONS 2>/dev/null" 2>/dev/null || true
sleep 1
$SSH "killall -9 $RAPTOR_DAEMONS 2>/dev/null" 2>/dev/null || true
$SSH 'rm -f /dev/shm/rss_ring_* /dev/shm/rss_osd_* /var/run/rss/*.pid /var/run/rss/*.sock 2>/dev/null' 2>/dev/null || true

HOST_IP=$($SSH 'echo $SSH_CONNECTION' 2>/dev/null | awk '{print $1}')
[ -z "$HOST_IP" ] && HOST_IP=$(ip route get "$DEVICE_IP" 2>/dev/null | grep -o 'src [0-9.]*' | awk '{print $2}')

if [ ! -f "$BUILD_DIR/rvd" ]; then
    echo "ERROR: build/rvd not found"; exit 1
fi

# NFS setup
if ! $SSH "test -d $DEVICE_MNT/build" 2>/dev/null; then
    if command -v docker > /dev/null 2>&1; then
        echo "    starting Docker NFS server..."
        docker run -d --rm --name "$DOCKER_NFS_CONTAINER" \
            --privileged \
            -v "$RAPTOR_DIR":/export/raptor:ro \
            -e NFS_EXPORT_0='/export/raptor *(ro,no_subtree_check,insecure,no_root_squash)' \
            -p ${NFS_PORT}:2049/tcp -p ${NFS_PORT}:2049/udp \
            -p ${NFS_MOUNTD_PORT}:32767/tcp -p ${NFS_MOUNTD_PORT}:32767/udp \
            erichough/nfs-server > /dev/null 2>&1
        sleep 3
        $SSH "mkdir -p $DEVICE_MNT" 2>/dev/null || true
        $SSH "mount -t nfs -o nolock,port=$NFS_PORT,mountport=$NFS_MOUNTD_PORT,nfsvers=3,tcp $HOST_IP:/export/raptor $DEVICE_MNT" 2>/dev/null
        if ! $SSH "test -x $DEVICE_MNT/build/rvd" 2>/dev/null; then
            echo "ERROR: Docker NFS mount failed"; exit 1
        fi
        pass "NFS mount (Docker)"
    else
        echo "ERROR: No NFS setup available"; exit 1
    fi
else
    pass "NFS mount"
fi

DEVICE_RAPTOR="$DEVICE_MNT"
RAPTORCTL="$DEVICE_RAPTOR/build/raptorctl"
CONF_ON_DEVICE="$DEVICE_RAPTOR/tests/stream-health.conf"

SENSOR=$($SSH 'sensor name 2>/dev/null || cat /proc/jz/sensor/name 2>/dev/null' 2>/dev/null || echo "unknown")
SENSOR_FPS=$($SSH 'cat /sys/module/sensor_*/parameters/sensor_max_fps 2>/dev/null' 2>/dev/null || echo "")
[ -z "$SENSOR_FPS" ] || [ "$SENSOR_FPS" = "0" ] && SENSOR_FPS=$($SSH 'sensor max_fps 2>/dev/null' 2>/dev/null || echo "25")
echo "    sensor: $SENSOR (${SENSOR_FPS}fps)"

if [ ! -x "$FFMPEG" ] || [ ! -x "$FFPROBE" ]; then
    echo "ERROR: ffmpeg/ffprobe not available"; exit 1
fi

echo ""

# ── Audio codec table ──

# codec_id  config_name  config_rate  expected_ffprobe_codec  expected_ffprobe_rate
CODECS="l16:l16:16000:pcm_s16be:16000
pcmu:pcmu:8000:pcm_mulaw:8000
pcma:pcma:8000:pcm_alaw:8000
aac:aac:16000:aac:16000
opus:opus:16000:opus:48000"

# ── Write base config ──

write_config() {
    local audio_codec="$1" audio_rate="$2"
    cat > "$TEST_CONF" << EOF
[sensor]
model = $SENSOR

[stream0]
width = 1920
height = 1080
fps = $SENSOR_FPS
bitrate = 2000000
codec = h264
rc_mode = cbr
gop = $SENSOR_FPS

[stream1]
width = 640
height = 360
fps = $SENSOR_FPS
bitrate = 500000
codec = h264
rc_mode = cbr

[audio]
enabled = true
sample_rate = $audio_rate
codec = $audio_codec

[rtsp]
port = $RTSP_PORT

[http]
port = 8080
username =
password =

[osd]
enabled = true
font_size = 24

[osd.timestamp]
type = text
template = %time%
position = top_left

[osd.uptime]
type = text
template = %uptime%
position = top_right

[osd.camera]
type = text
template = Camera
position = top_center

[osd.logo]
type = image
path = /usr/share/images/thingino_100x30.bgra
width = 100
height = 30
position = bottom_right

[ircut]
enabled = false

[motion]
enabled = false

[recording]
enabled = false

[ring]
refmode = true

[jpeg]
enabled = true

[log]
level = warn
EOF
}

# ── Start/restart daemons ──

start_all() {
    local audio_codec="$1" audio_rate="$2"

    $SSH "killall $RAPTOR_DAEMONS 2>/dev/null" 2>/dev/null || true
    sleep 2
    $SSH "killall -9 $RAPTOR_DAEMONS 2>/dev/null" 2>/dev/null || true
    $SSH 'rm -f /dev/shm/rss_ring_* /dev/shm/rss_osd_* /var/run/rss/*.pid /var/run/rss/*.sock 2>/dev/null' 2>/dev/null || true
    sleep 1

    write_config "$audio_codec" "$audio_rate"

    $SSH "$DEVICE_RAPTOR/build/rvd -c $CONF_ON_DEVICE -d" 2>/dev/null
    sleep 4
    $SSH "LD_PRELOAD=/usr/lib/libimp-nodbg.so $DEVICE_RAPTOR/build/rad -c $CONF_ON_DEVICE -d" 2>/dev/null
    sleep 2
    $SSH "$DEVICE_RAPTOR/build/rsd -c $CONF_ON_DEVICE -d" 2>/dev/null
    sleep 1
    $SSH "$DEVICE_RAPTOR/build/rhd -c $CONF_ON_DEVICE -d" 2>/dev/null
    sleep 1
    $SSH "$DEVICE_RAPTOR/build/rod -c $CONF_ON_DEVICE -d" 2>/dev/null
    sleep 2

    $SSH 'pidof rvd > /dev/null && pidof rad > /dev/null && pidof rsd > /dev/null' 2>/dev/null
}

# ── Capture and analyze ──

analyze_stream() {
    local codec_id="$1"
    local expect_acodec="$2"
    local expect_arate="$3"
    local prefix="[$codec_id]"
    local capture="/tmp/raptor-stream-${codec_id}.mkv"
    local frames="$LOG_DIR/frames-${codec_id}.csv"
    local errors="$LOG_DIR/errors-${codec_id}.txt"

    set +eo pipefail

    # Capture
    echo "    capturing ${DURATION}s RTSP (TCP)..."
    timeout $((DURATION + 10)) $FFMPEG -y -v quiet -rtsp_transport tcp \
        -i "rtsp://$DEVICE_IP:$RTSP_PORT/stream0" \
        -t "$DURATION" -c copy -f matroska "$capture" 2>/dev/null
    ret=$?

    if [ ! -f "$capture" ] || [ "$(stat -c%s "$capture" 2>/dev/null || echo 0)" -lt 10000 ]; then
        fail "$prefix capture" "no data ($FFMPEG exit $ret)"
        set -eo pipefail
        return
    fi
    pass "$prefix capture ($(du -h "$capture" | cut -f1))"

    # Extract frame data
    $FFPROBE -v quiet -print_format csv \
        -show_frames \
        -show_entries frame=media_type,key_frame,pts_time,pkt_dts_time,duration_time,pkt_size \
        "$capture" > "$frames" 2>/dev/null

    if [ ! -s "$frames" ]; then
        fail "$prefix ffprobe" "no frame data"
        set -eo pipefail
        return
    fi

    # Verify audio codec via show_streams
    local stream_info
    stream_info=$($FFPROBE -v quiet -print_format json -show_streams "$capture" 2>/dev/null || echo "{}")
    # Extract audio stream info — find the audio block in JSON
    local audio_block
    audio_block=$(echo "$stream_info" | python3 -c "
import sys,json
d=json.load(sys.stdin)
for s in d.get('streams',[]):
    if s.get('codec_type')=='audio':
        print('codec_name='+s.get('codec_name',''))
        print('sample_rate='+s.get('sample_rate',''))
        break
" 2>/dev/null || echo "")
    local actual_acodec
    actual_acodec=$(echo "$audio_block" | grep '^codec_name=' | cut -d= -f2 || echo "")
    local actual_arate
    actual_arate=$(echo "$audio_block" | grep '^sample_rate=' | cut -d= -f2 || echo "")

    if [ -n "$actual_acodec" ]; then
        if [ "$actual_acodec" = "$expect_acodec" ]; then
            pass "$prefix audio codec ($actual_acodec)"
        else
            fail "$prefix audio codec" "got $actual_acodec, expected $expect_acodec"
        fi
    else
        fail "$prefix audio codec" "no audio stream found"
    fi

    if [ -n "$actual_arate" ] && [ -n "$expect_arate" ]; then
        if [ "$actual_arate" = "$expect_arate" ]; then
            pass "$prefix audio sample rate ($actual_arate)"
        else
            fail "$prefix audio sample rate" "got $actual_arate, expected $expect_arate"
        fi
    fi

    # Analyze frames with awk
    local analysis
    analysis=$(awk -F',' '
    BEGIN {
        v_count=0; a_count=0; v_idr=0; v_zero=0; v_small_idr=0
        v_pts_bad=0; v_dts_bad=0; a_pts_bad=0
        v_gap=0; a_gap=0
        v_last_pts=""; v_last_dts=""
        a_last_pts=""
        v_first_pts=""; a_first_pts=""
        v_dur_bad=0; a_dur_bad=0
    }
    $1 == "frame" {
        media = $2
        key = $3
        pts = $4
        dts = $5
        dur = $6
        size = $7

        if (pts == "N/A" || pts == "") next

        if (media == "video") {
            v_count++
            if (key == 1) {
                v_idr++
                if (size+0 < 1000 && v_count > 1) v_small_idr++
            }
            if (size+0 == 0) v_zero++

            if (v_first_pts == "") v_first_pts = pts

            if (v_last_pts != "" && v_count > 2 && pts+0 <= v_last_pts+0) v_pts_bad++
            if (v_last_dts != "" && dts != "N/A" && dts+0 < v_last_dts+0) v_dts_bad++
            v_last_pts = pts

            if (dts != "N/A") v_last_dts = dts

            if (dur != "N/A" && dur+0 > 0) {
                expected_dur = 1.0 / '"$SENSOR_FPS"'
                if (dur+0 > expected_dur * 3) v_gap++
                if (dur+0 > expected_dur * 2 || dur+0 < expected_dur * 0.3) v_dur_bad++
            }
        }
        else if (media == "audio") {
            a_count++
            if (a_first_pts == "") a_first_pts = pts

            if (a_last_pts != "" && a_count > 2 && pts+0 <= a_last_pts+0) a_pts_bad++
            a_last_pts = pts

            if (dur != "N/A" && dur+0 > 0) {
                if (dur+0 > 0.1) a_gap++
                if (dur+0 > 0.05 || dur+0 < 0.005) a_dur_bad++
            }
        }
    }
    END {
        av_delta = "N/A"
        if (v_first_pts != "" && a_first_pts != "") {
            d = v_first_pts - a_first_pts
            if (d < 0) d = -d
            av_delta = sprintf("%.3f", d)
        }
        printf "v_count=%d v_idr=%d v_zero=%d v_small_idr=%d ", v_count, v_idr, v_zero, v_small_idr
        printf "v_pts_bad=%d v_dts_bad=%d v_gap=%d v_dur_bad=%d ", v_pts_bad, v_dts_bad, v_gap, v_dur_bad
        printf "a_count=%d a_pts_bad=%d a_gap=%d a_dur_bad=%d ", a_count, a_pts_bad, a_gap, a_dur_bad
        printf "av_delta=%s\n", av_delta
    }
    ' "$frames")

    # Parse results
    eval "$analysis"

    # Dump specific violations to error log
    awk -F',' '
    BEGIN { last_v_pts=""; last_a_pts=""; vc=0; ac=0 }
    $1 == "frame" && ($4 != "N/A" && $4 != "") {
        if ($2 == "video") {
            vc++
            if (last_v_pts != "" && vc > 2 && $4+0 <= last_v_pts+0)
                printf "VIDEO PTS INVERSION: frame %d pts=%s prev_pts=%s delta=%.6f\n", NR, $4, last_v_pts, $4-last_v_pts
            last_v_pts = $4
        }
        if ($2 == "audio") {
            ac++
            if (last_a_pts != "" && ac > 2 && $4+0 <= last_a_pts+0)
                printf "AUDIO PTS INVERSION: frame %d pts=%s prev_pts=%s delta=%.6f\n", NR, $4, last_a_pts, $4-last_a_pts
            last_a_pts = $4
        }
    }' "$frames" > "$errors" 2>/dev/null

    # Video checks
    if [ "$v_count" -gt 0 ]; then
        pass "$prefix video frames ($v_count)"
    else
        fail "$prefix video frames" "none"
    fi

    if [ "$v_pts_bad" -eq 0 ]; then
        pass "$prefix video PTS monotonic"
    else
        fail "$prefix video PTS" "$v_pts_bad non-monotonic"
    fi

    if [ "$v_dts_bad" -eq 0 ]; then
        pass "$prefix video DTS monotonic"
    else
        fail "$prefix video DTS" "$v_dts_bad non-monotonic"
    fi

    if [ "$v_idr" -gt 0 ]; then
        pass "$prefix video IDR present ($v_idr)"
    else
        fail "$prefix video IDR" "none in ${DURATION}s"
    fi

    if [ "$v_zero" -eq 0 ]; then
        pass "$prefix video no zero-size frames"
    else
        fail "$prefix video zero-size" "$v_zero frames"
    fi

    if [ "$v_small_idr" -eq 0 ]; then
        pass "$prefix video IDR size sane"
    else
        fail "$prefix video IDR size" "$v_small_idr undersized keyframes"
    fi

    if [ "$v_gap" -eq 0 ]; then
        pass "$prefix video no timing gaps"
    else
        fail "$prefix video gaps" "$v_gap frames with >3x expected duration"
    fi

    # Audio checks
    if [ "$a_count" -gt 100 ]; then
        pass "$prefix audio frames ($a_count)"
    else
        fail "$prefix audio frames" "only $a_count (expected >100)"
    fi

    if [ "$a_pts_bad" -eq 0 ]; then
        pass "$prefix audio PTS monotonic"
    else
        fail "$prefix audio PTS" "$a_pts_bad non-monotonic"
    fi

    if [ "$a_gap" -eq 0 ]; then
        pass "$prefix audio no timing gaps"
    else
        fail "$prefix audio gaps" "$a_gap frames with >100ms gap"
    fi

    # A/V sync
    if [ "$av_delta" != "N/A" ]; then
        av_ms=$(echo "$av_delta * 1000" | bc | cut -d. -f1)
        if [ "${av_ms:-9999}" -lt 500 ]; then
            pass "$prefix A/V sync (${av_delta}s delta)"
        else
            fail "$prefix A/V sync" "${av_delta}s delta (>500ms)"
        fi
    else
        skip "$prefix A/V sync" "missing timestamps"
    fi

    rm -f "$capture"
    set -eo pipefail
}

# ══════════════════════════════════════════════════════════════════
#  Main
# ══════════════════════════════════════════════════════════════════

echo ""

for line in $CODECS; do
    IFS=: read -r codec_id config_name config_rate expect_codec expect_rate <<< "$line"

    # Skip if --codec filter set
    if [ -n "$CODEC_FILTER" ] && [ "$CODEC_FILTER" != "$codec_id" ]; then
        continue
    fi

    echo "=== Audio codec: $codec_id (${config_rate}Hz) ==="

    if start_all "$config_name" "$config_rate"; then
        pass "[$codec_id] daemons started"
        sleep 1
        analyze_stream "$codec_id" "$expect_codec" "$expect_rate"
    else
        skip "[$codec_id] all" "daemons failed to start"
    fi

    echo ""
done

# ── MJPEG stream health ──

echo "=== MJPEG stream health (${DURATION}s) ==="
set +eo pipefail

MJPEG_DIR="$LOG_DIR/mjpeg"
mkdir -p "$MJPEG_DIR"
MJPEG_RAW="$MJPEG_DIR/raw.bin"
MJPEG_ERRORS="$MJPEG_DIR/errors.txt"

# Capture raw MJPEG multipart stream
timeout $((DURATION + 5)) curl -s -o "$MJPEG_RAW" --max-time "$DURATION" \
    "http://$DEVICE_IP:8080/mjpeg" 2>/dev/null || true

if [ ! -f "$MJPEG_RAW" ] || [ "$(stat -c%s "$MJPEG_RAW" 2>/dev/null || echo 0)" -lt 1000 ]; then
    fail "MJPEG capture" "no data"
else
    RAW_SIZE=$(du -h "$MJPEG_RAW" | cut -f1)
    pass "MJPEG capture ($RAW_SIZE)"

    # Split on JPEG SOI markers (FF D8) and analyze each frame
    # Count JPEG frames, verify SOI/EOI markers, check sizes
    MJPEG_ANALYSIS=$(python3 -c "
import sys

data = open('$MJPEG_RAW', 'rb').read()
soi = b'\xff\xd8'
eoi = b'\xff\xd9'

# Find all JPEG frames by SOI marker
frames = []
pos = 0
while True:
    start = data.find(soi, pos)
    if start == -1:
        break
    end = data.find(eoi, start + 2)
    if end == -1:
        break
    end += 2  # include EOI
    frames.append((start, end, end - start))
    pos = end

total = len(frames)
bad_soi = 0
bad_eoi = 0
zero_size = 0
min_size = 999999999
max_size = 0
sizes = []

for i, (start, end, size) in enumerate(frames):
    if data[start:start+2] != soi:
        bad_soi += 1
    if data[end-2:end] != eoi:
        bad_eoi += 1
    if size < 100:
        zero_size += 1
    if size < min_size:
        min_size = size
    if size > max_size:
        max_size = size
    sizes.append(size)

# Check timing consistency (frames should be ~1fps for JPEG idle mode,
# or higher if actively streaming)
print(f'total={total}')
print(f'bad_soi={bad_soi}')
print(f'bad_eoi={bad_eoi}')
print(f'zero_size={zero_size}')
print(f'min_size={min_size}')
print(f'max_size={max_size}')
print(f'avg_size={sum(sizes)//max(len(sizes),1)}')
" 2>/dev/null || echo "total=0")

    eval "$MJPEG_ANALYSIS"

    if [ "${total:-0}" -gt 5 ]; then
        pass "MJPEG frames ($total)"
    else
        fail "MJPEG frames" "only ${total:-0}"
    fi

    if [ "${bad_soi:-0}" -eq 0 ]; then
        pass "MJPEG SOI markers valid"
    else
        fail "MJPEG SOI" "$bad_soi frames missing SOI (FF D8)"
    fi

    if [ "${bad_eoi:-0}" -eq 0 ]; then
        pass "MJPEG EOI markers valid"
    else
        fail "MJPEG EOI" "$bad_eoi frames missing EOI (FF D9)"
    fi

    if [ "${zero_size:-0}" -eq 0 ]; then
        pass "MJPEG no undersized frames (min ${min_size:-0}B, max ${max_size:-0}B)"
    else
        fail "MJPEG frame sizes" "$zero_size frames < 100 bytes"
    fi

    rm -f "$MJPEG_RAW"
fi

set -eo pipefail
echo ""

# ── Multi-stream concurrent test ──

echo "=== Multi-stream concurrent (${DURATION}s) ==="
set +eo pipefail

MS_DIR="$LOG_DIR/multi"
mkdir -p "$MS_DIR"

# Capture stream0 + stream1 + MJPEG simultaneously
echo "    capturing stream0 + stream1 + MJPEG in parallel..."
timeout $((DURATION + 10)) $FFMPEG -y -v quiet -rtsp_transport tcp \
    -i "rtsp://$DEVICE_IP:$RTSP_PORT/stream0" \
    -t "$DURATION" -c copy -f matroska "$MS_DIR/stream0.mkv" 2>/dev/null &
PID_S0=$!
timeout $((DURATION + 10)) $FFMPEG -y -v quiet -rtsp_transport tcp \
    -i "rtsp://$DEVICE_IP:$RTSP_PORT/stream1" \
    -t "$DURATION" -c copy -f matroska "$MS_DIR/stream1.mkv" 2>/dev/null &
PID_S1=$!
timeout $((DURATION + 5)) curl -s -o "$MS_DIR/mjpeg.bin" --max-time "$DURATION" \
    "http://$DEVICE_IP:8080/mjpeg" 2>/dev/null &
PID_MJ=$!

wait $PID_S0 2>/dev/null
wait $PID_S1 2>/dev/null
wait $PID_MJ 2>/dev/null

# Analyze stream0
S0_SIZE=$(stat -c%s "$MS_DIR/stream0.mkv" 2>/dev/null || echo "0")
S1_SIZE=$(stat -c%s "$MS_DIR/stream1.mkv" 2>/dev/null || echo "0")
MJ_SIZE=$(stat -c%s "$MS_DIR/mjpeg.bin" 2>/dev/null || echo "0")

if [ "$S0_SIZE" -gt 10000 ]; then
    S0_FRAMES=$($FFPROBE -v quiet -print_format csv -show_frames \
        -show_entries frame=media_type "$MS_DIR/stream0.mkv" 2>/dev/null | grep -c 'video' || true)
    S0_FRAMES=$((S0_FRAMES + 0))
    S0_EXPECTED=$((DURATION * SENSOR_FPS * 9 / 10))
    if [ "$S0_FRAMES" -ge "$S0_EXPECTED" ]; then
        pass "multi stream0 ($S0_FRAMES video frames)"
    else
        fail "multi stream0" "only $S0_FRAMES frames (expected >=$S0_EXPECTED)"
    fi
else
    fail "multi stream0" "no data"
fi

if [ "$S1_SIZE" -gt 5000 ]; then
    S1_FRAMES=$($FFPROBE -v quiet -print_format csv -show_frames \
        -show_entries frame=media_type "$MS_DIR/stream1.mkv" 2>/dev/null | grep -c 'video' || true)
    S1_FRAMES=$((S1_FRAMES + 0))
    S1_EXPECTED=$((DURATION * SENSOR_FPS * 9 / 10))
    if [ "$S1_FRAMES" -ge "$S1_EXPECTED" ]; then
        pass "multi stream1 ($S1_FRAMES video frames)"
    else
        fail "multi stream1" "only $S1_FRAMES frames (expected >=$S1_EXPECTED)"
    fi
else
    fail "multi stream1" "no data"
fi

if [ "$MJ_SIZE" -gt 10000 ]; then
    MJ_FRAMES=$(python3 -c "
data = open('$MS_DIR/mjpeg.bin','rb').read()
print(data.count(b'\xff\xd8'))
" 2>/dev/null || echo "0")
    if [ "$MJ_FRAMES" -gt 3 ]; then
        pass "multi MJPEG ($MJ_FRAMES frames concurrent)"
    else
        fail "multi MJPEG" "only $MJ_FRAMES frames"
    fi
else
    fail "multi MJPEG" "no data"
fi

# Check PTS monotonicity on stream0 under concurrent load
$FFPROBE -v quiet -print_format csv -show_frames \
    -show_entries frame=media_type,pts_time "$MS_DIR/stream0.mkv" 2>/dev/null > "$MS_DIR/s0_frames.csv"
MULTI_PTS_BAD=$(awk -F',' '
BEGIN { last=""; count=0; bad=0 }
$1=="frame" && $2=="video" && $3!="N/A" && $3!="" {
    count++
    if (last != "" && count > 2 && $3+0 <= last+0) bad++
    last = $3
}
END { print bad }
' "$MS_DIR/s0_frames.csv" 2>/dev/null || echo "0")

if [ "$MULTI_PTS_BAD" -eq 0 ]; then
    pass "multi stream0 PTS monotonic under load"
else
    fail "multi stream0 PTS" "$MULTI_PTS_BAD inversions under concurrent load"
fi

rm -f "$MS_DIR"/*.mkv "$MS_DIR"/*.bin "$MS_DIR"/*.csv
set -eo pipefail
echo ""

# UDP transport probe
echo "=== UDP transport ==="
set +eo pipefail
udp_ok=$(timeout 8 $FFPROBE -v quiet -print_format json -show_streams \
    -rtsp_transport udp -i "rtsp://$DEVICE_IP:$RTSP_PORT/stream0" 2>/dev/null || echo "{}")
if echo "$udp_ok" | grep -q '"codec_name"'; then
    pass "UDP RTSP connects"
else
    fail "UDP RTSP" "no stream via UDP"
fi
set -eo pipefail

echo ""

# ── Summary ──

echo "========================================"
echo " Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo " Device:  $DEVICE_IP ($SENSOR)"
echo " Duration: ${DURATION}s per codec"
echo " Logs:    $LOG_DIR/"
echo "========================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
