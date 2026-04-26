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
        local cleanup_ssh="${SSH:-ssh -o ConnectTimeout=3 -o StrictHostKeyChecking=no -o LogLevel=ERROR root@$DEVICE_IP}"
        $cleanup_ssh 'umount -f -l /tmp/raptor-test 2>/dev/null; killall rvd rad rsd rhd rod ric rmd rmr rwd rwc rsp rfs 2>/dev/null' 2>/dev/null
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
RTSP_TRANSPORT="tcp"
SSH_PASS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --duration) DURATION="$2"; shift 2 ;;
        --codec) CODEC_FILTER="$2"; shift 2 ;;
        --udp) RTSP_TRANSPORT="udp"; shift ;;
        --password) SSH_PASS="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 <device-ip> [--duration <sec>] [--codec <codec>] [--udp] [--password <pw>]"
            exit 0 ;;
        -*) echo "Unknown option: $1"; exit 1 ;;
        *) DEVICE_IP="$1"; shift ;;
    esac
done

[ -z "$DEVICE_IP" ] && { echo "Usage: $0 <device-ip> [--duration <sec>] [--codec <codec>] [--udp] [--password <pw>]"; exit 1; }

DOCKER_NFS_CONTAINER="raptor-nfs-$(echo "$DEVICE_IP" | tr '.' '-')"
SSH_BASE="ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no -o LogLevel=ERROR root@$DEVICE_IP"
if [ -n "$SSH_PASS" ]; then
    SSH="sshpass -p $SSH_PASS $SSH_BASE"
else
    SSH="$SSH_BASE"
fi

# Unmount on device BEFORE killing Docker (prevents D-state hang)
$SSH 'umount -f -l /tmp/raptor-test 2>/dev/null' 2>/dev/null || true
# Kill any stale raptor-nfs containers (from prior runs on any device IP)
docker ps -a --filter 'name=raptor-nfs-' --format '{{.Names}}' 2>/dev/null \
    | xargs -r docker rm -f > /dev/null 2>&1 || true
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
echo " Transport: $RTSP_TRANSPORT"
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

# Clean stale NFS mounts first (prevents D state on killall)
$SSH 'umount -f -l /tmp/raptor-test 2>/dev/null' 2>/dev/null || true
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
ao_enabled = true

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

# ── Frame analysis (reusable) ──

# Analyze a capture file: PTS/DTS monotonicity, IDR, frame sizes, timing, A/V sync
# Usage: check_frames <prefix> <capture_file>
check_frames() {
    local prefix="$1"
    local capture="$2"
    local frames="$LOG_DIR/frames-$(echo "$prefix" | tr '[]/ ' '----').csv"
    local errors="$LOG_DIR/errors-$(echo "$prefix" | tr '[]/ ' '----').txt"

    $FFPROBE -v quiet -print_format csv \
        -show_frames \
        -show_entries frame=media_type,key_frame,pts_time,pkt_dts_time,duration_time,pkt_size \
        "$capture" > "$frames" 2>/dev/null

    if [ ! -s "$frames" ]; then
        fail "$prefix ffprobe" "no frame data"
        return
    fi

    local analysis
    analysis=$(awk -F',' '
    BEGIN {
        v_count=0; a_count=0; v_idr=0; v_zero=0; v_small_idr=0
        v_pts_bad=0; v_dts_bad=0; a_pts_bad=0
        v_gap=0; a_gap=0
        v_last_pts=""; v_last_dts=""
        a_last_pts=""
        v_first_pts=""; a_first_pts=""
    }
    $1 == "frame" {
        media = $2; key = $3; pts = $4; dts = $5; dur = $6; size = $7
        if (pts == "N/A" || pts == "") next

        if (media == "video") {
            v_count++
            if (key == 1) { v_idr++; if (size+0 < 1000 && v_count > 1) v_small_idr++ }
            if (size+0 == 0) v_zero++
            if (v_first_pts == "") v_first_pts = pts
            if (v_last_pts != "" && v_count > 2 && pts+0 <= v_last_pts+0) v_pts_bad++
            if (v_last_dts != "" && dts != "N/A" && dts+0 < v_last_dts+0) v_dts_bad++
            v_last_pts = pts
            if (dts != "N/A") v_last_dts = dts
            if (dur != "N/A" && dur+0 > 0) {
                expected_dur = 1.0 / '"$SENSOR_FPS"'
                if (dur+0 > expected_dur * 3) v_gap++
            }
        }
        else if (media == "audio") {
            a_count++
            if (a_first_pts == "") a_first_pts = pts
            if (a_last_pts != "" && a_count > 2 && pts+0 <= a_last_pts+0) a_pts_bad++
            a_last_pts = pts
            if (dur != "N/A" && dur+0 > 0.1) a_gap++
        }
    }
    END {
        av_delta = "N/A"
        if (v_first_pts != "" && a_first_pts != "") {
            d = v_first_pts - a_first_pts; if (d < 0) d = -d
            av_delta = sprintf("%.3f", d)
        }
        printf "v_count=%d v_idr=%d v_zero=%d v_small_idr=%d ", v_count, v_idr, v_zero, v_small_idr
        printf "v_pts_bad=%d v_dts_bad=%d v_gap=%d ", v_pts_bad, v_dts_bad, v_gap
        printf "a_count=%d a_pts_bad=%d a_gap=%d ", a_count, a_pts_bad, a_gap
        printf "av_delta=%s\n", av_delta
    }' "$frames")

    eval "$analysis"

    # Error detail log
    awk -F',' '
    BEGIN { last_v=""; last_a=""; vc=0; ac=0 }
    $1=="frame" && ($4!="N/A" && $4!="") {
        if ($2=="video") { vc++; if (last_v!="" && vc>2 && $4+0<=last_v+0) printf "VIDEO PTS: frame %d pts=%s prev=%s\n",NR,$4,last_v; last_v=$4 }
        if ($2=="audio") { ac++; if (last_a!="" && ac>2 && $4+0<=last_a+0) printf "AUDIO PTS: frame %d pts=%s prev=%s\n",NR,$4,last_a; last_a=$4 }
    }' "$frames" > "$errors" 2>/dev/null

    # Report
    [ "$v_count" -gt 0 ] && pass "$prefix video frames ($v_count)" || fail "$prefix video frames" "none"
    [ "$v_pts_bad" -eq 0 ] && pass "$prefix video PTS monotonic" || fail "$prefix video PTS" "$v_pts_bad non-monotonic"
    [ "$v_dts_bad" -eq 0 ] && pass "$prefix video DTS monotonic" || fail "$prefix video DTS" "$v_dts_bad non-monotonic"
    [ "$v_idr" -gt 0 ] && pass "$prefix video IDR present ($v_idr)" || fail "$prefix video IDR" "none"
    [ "$v_zero" -eq 0 ] && pass "$prefix video no zero-size frames" || fail "$prefix video zero-size" "$v_zero frames"
    [ "$v_small_idr" -eq 0 ] && pass "$prefix video IDR size sane" || fail "$prefix video IDR size" "$v_small_idr undersized"
    [ "$v_gap" -eq 0 ] && pass "$prefix video no timing gaps" || fail "$prefix video gaps" "$v_gap"
    [ "$a_count" -gt 10 ] && pass "$prefix audio frames ($a_count)" || fail "$prefix audio frames" "only $a_count"
    [ "$a_pts_bad" -eq 0 ] && pass "$prefix audio PTS monotonic" || fail "$prefix audio PTS" "$a_pts_bad non-monotonic"
    [ "$a_gap" -eq 0 ] && pass "$prefix audio no timing gaps" || fail "$prefix audio gaps" "$a_gap"

    if [ "$av_delta" != "N/A" ]; then
        av_ms=$(echo "$av_delta * 1000" | bc | cut -d. -f1)
        [ "${av_ms:-9999}" -lt 500 ] && pass "$prefix A/V sync (${av_delta}s)" || fail "$prefix A/V sync" "${av_delta}s (>500ms)"
    else
        skip "$prefix A/V sync" "missing timestamps"
    fi
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
    timeout $((DURATION + 10)) $FFMPEG -y -v quiet -rtsp_transport $RTSP_TRANSPORT \
        -i "rtsp://$DEVICE_IP:$RTSP_PORT/stream0" \
        -t "$DURATION" -c copy -f matroska "$capture" 2>/dev/null
    ret=$?

    if [ ! -f "$capture" ] || [ "$(stat -c%s "$capture" 2>/dev/null || echo 0)" -lt 10000 ]; then
        fail "$prefix capture" "no data ($FFMPEG exit $ret)"
        set -eo pipefail
        return
    fi
    pass "$prefix capture ($(du -h "$capture" | cut -f1))"

    # Verify audio codec via show_streams
    local stream_info
    stream_info=$($FFPROBE -v quiet -print_format json -show_streams "$capture" 2>/dev/null || echo "{}")
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
    local actual_acodec actual_arate
    actual_acodec=$(echo "$audio_block" | grep '^codec_name=' | cut -d= -f2 || echo "")
    actual_arate=$(echo "$audio_block" | grep '^sample_rate=' | cut -d= -f2 || echo "")

    if [ -n "$actual_acodec" ]; then
        [ "$actual_acodec" = "$expect_acodec" ] && pass "$prefix audio codec ($actual_acodec)" || fail "$prefix audio codec" "got $actual_acodec, expected $expect_acodec"
    else
        fail "$prefix audio codec" "no audio stream found"
    fi
    if [ -n "$actual_arate" ] && [ -n "$expect_arate" ]; then
        [ "$actual_arate" = "$expect_arate" ] && pass "$prefix audio sample rate ($actual_arate)" || fail "$prefix audio sample rate" "got $actual_arate, expected $expect_arate"
    fi

    # Full frame analysis
    check_frames "$prefix" "$capture"

    rm -f "$capture"
    set -eo pipefail
}

# ══════════════════════════════════════════════════════════════════
#  Main
# ══════════════════════════════════════════════════════════════════

# ── Per-codec streaming analysis ──

BC_TESTED=false

for line in $CODECS; do
    IFS=: read -r codec_id config_name config_rate expect_codec expect_rate <<< "$line"

    if [ -n "$CODEC_FILTER" ] && [ "$CODEC_FILTER" != "$codec_id" ]; then
        continue
    fi

    echo "=== Audio codec: $codec_id (${config_rate}Hz) ==="

    if start_all "$config_name" "$config_rate"; then
        pass "[$codec_id] daemons started"
        sleep 1
        analyze_stream "$codec_id" "$expect_codec" "$expect_rate"

        # Run backchannel on first codec only (reuses same session)
        if [ "$BC_TESTED" = false ]; then
            BC_TESTED=true
            echo ""
            echo "  --- ONVIF backchannel (on $codec_id session) ---"
    set +eo pipefail

    BC_RESPONSE=$(python3 -c "
import re, socket
host = '$DEVICE_IP'
port = $RTSP_PORT
full_url = f'rtsp://{host}:{port}/stream0'
sock = socket.create_connection((host, port), timeout=10)
request = (
    f'DESCRIBE {full_url} RTSP/1.0\r\n'
    'CSeq: 1\r\n'
    'Accept: application/sdp\r\n'
    'Require: www.onvif.org/ver20/backchannel\r\n'
    'User-Agent: stream-health-test/1.0\r\n'
    '\r\n'
)
sock.sendall(request.encode())
data = b''
while b'\r\n\r\n' not in data:
    chunk = sock.recv(4096)
    if not chunk: break
    data += chunk
header, _, rest = data.partition(b'\r\n\r\n')
m = re.search(br'Content-Length:\s*(\d+)', header, re.I)
if m:
    need = int(m.group(1)) - len(rest)
    while need > 0:
        chunk = sock.recv(4096)
        if not chunk: break
        rest += chunk
        need -= len(chunk)
sock.close()
print((header + b'\r\n\r\n' + rest).decode('utf-8', 'replace'))
" 2>/dev/null || echo "FAILED")

    if echo "$BC_RESPONSE" | grep -q 'RTSP/1.0 200'; then
        pass "backchannel DESCRIBE accepted (200 OK)"
    else
        fail "backchannel DESCRIBE" "not 200 OK"
    fi

    # ONVIF backchannel uses a=sendonly (server perspective: "I will only send
    # on this track" = client should send audio TO me)
    if echo "$BC_RESPONSE" | grep -q 'a=sendonly'; then
        pass "backchannel has sendonly track (ONVIF)"
    else
        fail "backchannel sendonly" "no a=sendonly in SDP"
    fi

    AUDIO_SECTIONS=$(echo "$BC_RESPONSE" | grep -c '^m=audio' || true)
    AUDIO_SECTIONS=$((AUDIO_SECTIONS + 0))
    if [ "$AUDIO_SECTIONS" -ge 2 ]; then
        pass "backchannel extra audio track ($AUDIO_SECTIONS audio sections)"
    else
        fail "backchannel audio tracks" "only $AUDIO_SECTIONS audio section(s), expected >=2"
    fi

    BC_CODEC=$(echo "$BC_RESPONSE" | sed -n '/a=sendonly/,/^m=/p' | grep 'a=rtpmap' | head -1 || echo "")
    if [ -n "$BC_CODEC" ]; then
        pass "backchannel codec: $(echo "$BC_CODEC" | sed 's/a=rtpmap://')"
    else
        skip "backchannel codec" "no rtpmap after recvonly"
    fi

    # Send actual audio via backchannel
    # Generate 1s of PCMU test tone (1kHz sine wave, mu-law encoded)
    BC_SEND_RESULT=$(python3 -c "
import re, socket, struct, math, time

host = '$DEVICE_IP'
port = $RTSP_PORT
url = f'rtsp://{host}:{port}/stream0'
cseq = [0]

def next_cseq():
    cseq[0] += 1
    return cseq[0]

def send_recv(sock, req):
    sock.sendall(req.encode())
    data = b''
    while b'\r\n\r\n' not in data:
        chunk = sock.recv(4096)
        if not chunk: break
        data += chunk
    header, _, rest = data.partition(b'\r\n\r\n')
    m = re.search(br'Content-Length:\s*(\d+)', header, re.I)
    if m:
        need = int(m.group(1)) - len(rest)
        while need > 0:
            chunk = sock.recv(4096)
            if not chunk: break
            rest += chunk
            need -= len(chunk)
    return (header + b'\r\n\r\n' + rest).decode('utf-8', 'replace')

sock = socket.create_connection((host, port), timeout=10)

# DESCRIBE with backchannel
resp = send_recv(sock, f'DESCRIBE {url} RTSP/1.0\r\nCSeq: {next_cseq()}\r\nAccept: application/sdp\r\nRequire: www.onvif.org/ver20/backchannel\r\n\r\n')
if '200' not in resp.split('\r\n')[0]:
    print('DESCRIBE_FAILED')
    sock.close()
    raise SystemExit(1)

# Find backchannel control track
# Find the backchannel control track — look for the media section with a=sendonly
bc_track = None
lines = resp.split('\r\n')
for i, line in enumerate(lines):
    if 'a=control:backchannel' in line:
        bc_track = 'backchannel'
        break
    if 'a=sendonly' in line:
        # Walk backwards to find the a=control in this section
        for j in range(i-1, max(i-10, 0), -1):
            if lines[j].startswith('a=control:'):
                bc_track = lines[j].split(':',1)[1].strip()
                break
        if bc_track:
            break

if not bc_track:
    print('NO_BC_TRACK')
    sock.close()
    raise SystemExit(1)

# Find session from video SETUP first (needed for interleaved)
session = None

# SETUP video (interleaved ch 0-1)
resp = send_recv(sock, f'SETUP {url}/video RTSP/1.0\r\nCSeq: {next_cseq()}\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n')
m = re.search(r'Session:\s*(\S+)', resp)
if m:
    session = m.group(1).split(';')[0]

if not session:
    print('NO_SESSION')
    sock.close()
    raise SystemExit(1)

# SETUP audio playback (interleaved ch 2-3)
resp = send_recv(sock, f'SETUP {url}/audio RTSP/1.0\r\nCSeq: {next_cseq()}\r\nSession: {session}\r\nTransport: RTP/AVP/TCP;unicast;interleaved=2-3\r\nRequire: www.onvif.org/ver20/backchannel\r\n\r\n')

# SETUP backchannel (interleaved ch 4-5)
resp = send_recv(sock, f'SETUP {url}/{bc_track} RTSP/1.0\r\nCSeq: {next_cseq()}\r\nSession: {session}\r\nTransport: RTP/AVP/TCP;unicast;interleaved=4-5\r\nRequire: www.onvif.org/ver20/backchannel\r\n\r\n')
if '200' not in resp.split('\r\n')[0]:
    print('SETUP_BC_FAILED')
    sock.close()
    raise SystemExit(1)

# PLAY
resp = send_recv(sock, f'PLAY {url} RTSP/1.0\r\nCSeq: {next_cseq()}\r\nSession: {session}\r\nRange: npt=0.000-\r\n\r\n')
if '200' not in resp.split('\r\n')[0]:
    print('PLAY_FAILED')
    sock.close()
    raise SystemExit(1)

# Load PCMU test audio (or generate silence)
test_audio = '$SCRIPT_DIR/backchannel-test.raw'
try:
    with open(test_audio, 'rb') as f:
        samples = list(f.read())
except:
    samples = [0xFF] * 4000  # 0.5s silence

# Send as RTP over interleaved channel 4
# 20ms frames = 160 samples each
seq = 0
ts = 0
ssrc = 0x12345678
sent = 0
for frame_start in range(0, len(samples), 160):
    frame = bytes(samples[frame_start:frame_start+160])
    if len(frame) < 160:
        break
    # RTP header: V=2, P=0, X=0, CC=0, M=0, PT=0 (PCMU)
    rtp = struct.pack('!BBHII', 0x80, 0, seq & 0xFFFF, ts, ssrc) + frame
    # RTSP interleaved: $ + channel + length
    interleaved = b'\x24' + struct.pack('!BH', 4, len(rtp)) + rtp
    try:
        sock.sendall(interleaved)
        sent += 1
    except:
        break
    seq += 1
    ts += 160
    time.sleep(0.018)

# TEARDOWN
try:
    sock.sendall(f'TEARDOWN {url} RTSP/1.0\r\nCSeq: {next_cseq()}\r\nSession: {session}\r\n\r\n'.encode())
except:
    pass
sock.close()

if sent > 10:
    print(f'SENT_OK:{sent}')
else:
    print(f'SENT_FEW:{sent}')
" 2>/dev/null || echo "SCRIPT_ERROR")

    if echo "$BC_SEND_RESULT" | grep -q 'SENT_OK'; then
        BC_FRAMES=$(echo "$BC_SEND_RESULT" | grep -o '[0-9]*$')
        pass "backchannel audio sent ($BC_FRAMES RTP frames)"
    elif echo "$BC_SEND_RESULT" | grep -q 'SENT_FEW'; then
        BC_FRAMES=$(echo "$BC_SEND_RESULT" | grep -o '[0-9]*$')
        fail "backchannel send" "only $BC_FRAMES frames sent"
    else
        fail "backchannel send" "$BC_SEND_RESULT"
    fi

    set -eo pipefail
        fi
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
timeout $((DURATION + 10)) "$FFMPEG" -y -v quiet -rtsp_transport $RTSP_TRANSPORT \
    -i "rtsp://$DEVICE_IP:$RTSP_PORT/stream0" \
    -t "$DURATION" -c copy -f matroska "$MS_DIR/stream0.mkv" > /dev/null 2>&1 &
PID_S0=$!
timeout $((DURATION + 10)) "$FFMPEG" -y -v quiet -rtsp_transport $RTSP_TRANSPORT \
    -i "rtsp://$DEVICE_IP:$RTSP_PORT/stream1" \
    -t "$DURATION" -c copy -f matroska "$MS_DIR/stream1.mkv" > /dev/null 2>&1 &
PID_S1=$!
wait $PID_S0 2>/dev/null
wait $PID_S1 2>/dev/null

# Full analysis on both streams
if [ "$(stat -c%s "$MS_DIR/stream0.mkv" 2>/dev/null || echo 0)" -gt 10000 ]; then
    check_frames "[multi/stream0]" "$MS_DIR/stream0.mkv"
else
    fail "[multi/stream0]" "no data captured"
fi

if [ "$(stat -c%s "$MS_DIR/stream1.mkv" 2>/dev/null || echo 0)" -gt 5000 ]; then
    check_frames "[multi/stream1]" "$MS_DIR/stream1.mkv"
else
    fail "[multi/stream1]" "no data captured"
fi

rm -f "$MS_DIR"/*.mkv
set -eo pipefail
echo ""

# Alternate transport probe (test whichever we're NOT using)
if [ "$RTSP_TRANSPORT" = "tcp" ]; then
    ALT_TRANSPORT="udp"
else
    ALT_TRANSPORT="tcp"
fi
echo "=== Alternate transport ($ALT_TRANSPORT) ==="
set +eo pipefail
alt_ok=$(timeout 8 "$FFPROBE" -v quiet -print_format json -show_streams \
    -rtsp_transport "$ALT_TRANSPORT" -i "rtsp://$DEVICE_IP:$RTSP_PORT/stream0" 2>/dev/null || echo "{}")
if echo "$alt_ok" | grep -q '"codec_name"'; then
    pass "$ALT_TRANSPORT RTSP connects"
else
    fail "$ALT_TRANSPORT RTSP" "no stream via $ALT_TRANSPORT"
fi
set -eo pipefail

echo ""

# ── Backchannel probe ──

echo ""

# ── Summary ──

echo "========================================"
echo " Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo " Device:  $DEVICE_IP ($SENSOR)"
echo " Duration: ${DURATION}s per codec"
echo " Transport: $RTSP_TRANSPORT"
echo " Logs:    $LOG_DIR/"
echo "========================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
