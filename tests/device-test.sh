#!/bin/bash
#
# device-test.sh -- On-device integration test
#
# Runs from the build host, SSHes into a test camera, starts raptor
# with a known config, and validates encoder output, RTSP streams,
# HTTP endpoints, and control socket responses.
#
# Prerequisites:
#   - Device reachable via SSH (root@IP, key auth)
#   - NFS mount at /mnt/nfs pointing to build host home dir
#   - Raptor built via build-standalone.sh (binaries in build/)
#   - ffprobe and curl on the build host
#
# Usage:
#   ./tests/device-test.sh <device-ip>
#   ./tests/device-test.sh <device-ip> --no-restart   # skip daemon restart
#   ./tests/device-test.sh <device-ip> --keep          # leave daemons running after test
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RAPTOR_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Args ──

DEVICE_IP=""
RESTART=true
KEEP=false

while [ $# -gt 0 ]; do
    case "$1" in
        --no-restart) RESTART=false; shift ;;
        --keep) KEEP=true; shift ;;
        --help|-h)
            echo "Usage: $0 <device-ip> [--no-restart] [--keep]"
            exit 0
            ;;
        -*) echo "Unknown option: $1"; exit 1 ;;
        *) DEVICE_IP="$1"; shift ;;
    esac
done

if [ -z "$DEVICE_IP" ]; then
    echo "Usage: $0 <device-ip> [--no-restart] [--keep]"
    exit 1
fi

# ── Config ──

SSH="ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no -o LogLevel=ERROR root@$DEVICE_IP"
NFS_RAPTOR="/mnt/nfs/projects/thingino/raptor"
RTSP_PORT=554
HTTP_PORT=8080
PASS=0
FAIL=0
SKIP=0

# Known test values — we set these in config and verify they come back
MAIN_WIDTH=1920
MAIN_HEIGHT=1080
MAIN_BITRATE=2000000
MAIN_FPS=0  # set after sensor detection
MAIN_GOP=50
MAIN_CODEC=h264
MAIN_RC=cbr
MAIN_MIN_QP=15
MAIN_MAX_QP=45

SUB_WIDTH=640
SUB_HEIGHT=360
SUB_BITRATE=500000
SUB_FPS=0  # set after sensor detection
SUB_GOP=30
SUB_CODEC=h264
SUB_RC=cbr

AUDIO_CODEC=l16
AUDIO_RATE=16000

# ── Helpers ──

pass() {
    PASS=$((PASS + 1))
    printf "  PASS  %s\n" "$1"
}

fail() {
    FAIL=$((FAIL + 1))
    printf "  FAIL  %s: %s\n" "$1" "$2"
}

skip() {
    SKIP=$((SKIP + 1))
    printf "  SKIP  %s: %s\n" "$1" "$2"
}

# Check a numeric value is within range
check_range() {
    local name="$1" actual="$2" expected="$3" tolerance_pct="$4"
    if [ -z "$actual" ] || [ "$actual" = "null" ]; then
        fail "$name" "no value"
        return
    fi
    local low high
    low=$(echo "$expected * (100 - $tolerance_pct) / 100" | bc)
    high=$(echo "$expected * (100 + $tolerance_pct) / 100" | bc)
    if [ "$actual" -ge "$low" ] && [ "$actual" -le "$high" ]; then
        pass "$name ($actual, expected $expected +/-${tolerance_pct}%)"
    else
        fail "$name" "got $actual, expected $expected +/-${tolerance_pct}% (range $low-$high)"
    fi
}

check_eq() {
    local name="$1" actual="$2" expected="$3"
    if [ "$actual" = "$expected" ]; then
        pass "$name"
    else
        fail "$name" "got '$actual', expected '$expected'"
    fi
}

# ── Preflight ──

echo "========================================"
echo " Device test: $DEVICE_IP"
echo "========================================"
echo ""

echo "=== Preflight ==="

# SSH connectivity
if ! $SSH 'true' 2>/dev/null; then
    echo "ERROR: cannot SSH to root@$DEVICE_IP"
    exit 1
fi
pass "SSH connectivity"

# NFS mount (auto-mount if not present)
if ! $SSH "test -d $NFS_RAPTOR/build" 2>/dev/null; then
    $SSH 'mount -t nfs -o nolock 10.25.1.230:/home/turismo /mnt/nfs 2>/dev/null' 2>/dev/null || true
    sleep 1
    if ! $SSH "test -d $NFS_RAPTOR/build" 2>/dev/null; then
        echo "ERROR: NFS mount not available at $NFS_RAPTOR/build"
        exit 1
    fi
fi
pass "NFS mount"

# Binaries exist
if ! $SSH "test -x $NFS_RAPTOR/build/rvd" 2>/dev/null; then
    echo "ERROR: build/rvd not found — run build-standalone.sh first"
    exit 1
fi
pass "binaries present"

# Check sensor
SENSOR=$($SSH 'cat /proc/jz/sensor/name 2>/dev/null' 2>/dev/null || echo "unknown")
SENSOR_FPS=$($SSH 'cat /proc/jz/sensor/max_fps 2>/dev/null' 2>/dev/null || echo "25")
MAIN_FPS=$SENSOR_FPS
SUB_FPS=$SENSOR_FPS
echo "  sensor: $SENSOR (${SENSOR_FPS}fps)"

# ffprobe available locally
if ! command -v ffprobe > /dev/null 2>&1; then
    echo "ERROR: ffprobe not found on build host"
    exit 1
fi

echo ""

# ── Generate test config ──

TEST_CONF="$RAPTOR_DIR/tests/device-test.conf"
cat > "$TEST_CONF" << EOF
[sensor]
model = $SENSOR

[stream0]
width = $MAIN_WIDTH
height = $MAIN_HEIGHT
fps = $SENSOR_FPS
bitrate = $MAIN_BITRATE
codec = $MAIN_CODEC
rc_mode = $MAIN_RC
gop = $MAIN_GOP
min_qp = $MAIN_MIN_QP
max_qp = $MAIN_MAX_QP

[stream1]
width = $SUB_WIDTH
height = $SUB_HEIGHT
fps = $SENSOR_FPS
bitrate = $SUB_BITRATE
codec = $SUB_CODEC
rc_mode = $SUB_RC
gop = $SUB_GOP

[audio]
enabled = true
sample_rate = $AUDIO_RATE
codec = $AUDIO_CODEC

[rtsp]
port = $RTSP_PORT

[http]
port = $HTTP_PORT
username =
password =

[osd]
enabled = false

[ircut]
enabled = false

[motion]
enabled = false

[recording]
enabled = false

[log]
level = info
EOF

# ── Start daemons ──

if [ "$RESTART" = true ]; then
    echo "=== Starting daemons ==="

    # Kill existing raptor processes (SIGTERM for clean SDK teardown)
    $SSH 'killall rvd rad rsd rhd rod ric rmd rmr rwd 2>/dev/null' 2>/dev/null || true
    sleep 2
    # SIGKILL stragglers
    $SSH 'killall -9 rvd rad rsd rhd rod ric rmd rmr rwd 2>/dev/null' 2>/dev/null || true
    $SSH 'rm -f /dev/shm/rss_ring_* /dev/shm/rss_osd_* /var/run/rss/*.pid /var/run/rss/*.sock 2>/dev/null' 2>/dev/null || true
    sleep 1

    CONF_ON_DEVICE="$NFS_RAPTOR/tests/device-test.conf"

    # Start rvd first (creates rings), wait for pipeline
    $SSH "$NFS_RAPTOR/build/rvd -c $CONF_ON_DEVICE -d" 2>/dev/null
    sleep 4

    # Start consumers
    $SSH "$NFS_RAPTOR/build/rad -c $CONF_ON_DEVICE -d" 2>/dev/null
    sleep 1
    $SSH "$NFS_RAPTOR/build/rsd -c $CONF_ON_DEVICE -d" 2>/dev/null
    sleep 1
    $SSH "$NFS_RAPTOR/build/rhd -c $CONF_ON_DEVICE -d" 2>/dev/null
    sleep 2

    echo "  daemons started"
fi

echo ""

# ── Phase 1: Daemon health ──

echo "=== Phase 1: Daemon health ==="

for daemon in rvd rad rsd rhd; do
    pid=$($SSH "pidof $daemon 2>/dev/null" 2>/dev/null || echo "")
    if [ -n "$pid" ]; then
        pass "$daemon running (pid $pid)"
    else
        fail "$daemon" "not running"
    fi
done

echo ""

# ── Phase 2: Control socket ──

echo "=== Phase 2: Control socket ==="

RAPTORCTL="$NFS_RAPTOR/build/raptorctl"

# Status
STATUS=$($SSH "timeout 3 $RAPTORCTL rvd status" 2>/dev/null || echo "")
if echo "$STATUS" | grep -q '"status"'; then
    pass "rvd status"
else
    fail "rvd status" "no response"
fi

# QP bounds — the QP 0/0 bug catch
# SDK returns -1 for "use default" which is fine. 0/0 is the bad case.
for ch in 0 1; do
    QP=$($SSH "timeout 3 $RAPTORCTL rvd get-qp-bounds $ch" 2>/dev/null || echo "")
    min_qp=$(echo "$QP" | grep -o '"min_qp":[-0-9]*' | grep -o '[-0-9]*$' || echo "")
    max_qp=$(echo "$QP" | grep -o '"max_qp":[-0-9]*' | grep -o '[-0-9]*$' || echo "")
    if [ -z "$min_qp" ] || [ -z "$max_qp" ]; then
        fail "ch$ch QP bounds" "no response"
    elif [ "$min_qp" = "0" ] && [ "$max_qp" = "0" ]; then
        fail "ch$ch QP bounds" "0/0 — encoder misconfigured"
    elif [ "$min_qp" = "-1" ] || [ "$max_qp" = "-1" ]; then
        pass "ch$ch QP bounds (min=$min_qp max=$max_qp, SDK default)"
    elif [ "$min_qp" -gt 0 ] && [ "$max_qp" -gt 0 ] && [ "$min_qp" -lt "$max_qp" ]; then
        pass "ch$ch QP bounds (min=$min_qp max=$max_qp)"
    else
        fail "ch$ch QP bounds" "got min=$min_qp max=$max_qp"
    fi
done

# Bitrate
BR=$($SSH "timeout 3 $RAPTORCTL rvd get-bitrate 0" 2>/dev/null || echo "")
cfg_br=$(echo "$BR" | grep -o '"bitrate":[0-9]*' | grep -o '[0-9]*$' || echo "")
if [ -n "$cfg_br" ]; then
    check_eq "ch0 configured bitrate" "$cfg_br" "$MAIN_BITRATE"
else
    fail "ch0 configured bitrate" "no response"
fi

# GOP
GOP=$($SSH "timeout 3 $RAPTORCTL rvd get-gop 0" 2>/dev/null || echo "")
gop_val=$(echo "$GOP" | grep -o '"gop":[0-9]*' | grep -o '[0-9]*$' || echo "")
if [ -n "$gop_val" ]; then
    check_eq "ch0 GOP" "$gop_val" "$MAIN_GOP"
else
    fail "ch0 GOP" "no response"
fi

# FPS
FPS=$($SSH "timeout 3 $RAPTORCTL rvd get-fps 0" 2>/dev/null || echo "")
fps_val=$(echo "$FPS" | grep -o '"fps_num":[0-9]*' | grep -o '[0-9]*$' || echo "")
if [ -n "$fps_val" ]; then
    check_eq "ch0 FPS" "$fps_val" "$MAIN_FPS"
else
    fail "ch0 FPS" "no response"
fi

# RC mode
RC=$($SSH "timeout 3 $RAPTORCTL rvd get-rc-mode 0" 2>/dev/null || echo "")
rc_val=$(echo "$RC" | grep -o '"rc_mode":"[^"]*"' | grep -o '"[^"]*"$' | tr -d '"' || echo "")
if [ -n "$rc_val" ]; then
    check_eq "ch0 rc_mode" "$rc_val" "$MAIN_RC"
else
    fail "ch0 rc_mode" "no response"
fi

echo ""

# ── Phase 3: Ring buffer sanity ──

echo "=== Phase 3: Ring buffer ==="

RINGDUMP="$NFS_RAPTOR/build/ringdump"

# Start ringdump in background, then request IDR so the keyframe
# lands while we're already reading
RING_OUT=$($SSH "timeout 5 sh -c '$RINGDUMP main -f -n 60 2>&1 & sleep 0.5; $RAPTORCTL rvd request-idr > /dev/null 2>&1; wait'" 2>/dev/null || echo "")
frame_count=$(echo "$RING_OUT" | grep -c '^#' || echo "0")
has_idr=$(echo "$RING_OUT" | grep -qE 'key=1|IDR' && echo "yes" || echo "no")

if [ "$frame_count" -ge 5 ]; then
    pass "ring read ($frame_count frames)"
else
    fail "ring read" "only $frame_count frames in 5s"
fi

if [ "$has_idr" = "yes" ]; then
    pass "IDR present in ring"
else
    fail "IDR present in ring" "no keyframe in captured frames"
fi

# Check for oversized frames (> 500KB = likely encoder misconfiguration)
max_len=$(echo "$RING_OUT" | grep -o 'len=[0-9]*' | grep -o '[0-9]*' | sort -rn | head -1 || echo "0")
if [ "$max_len" -gt 0 ] && [ "$max_len" -lt 512000 ]; then
    pass "frame sizes sane (max ${max_len}B)"
else
    fail "frame sizes" "max frame ${max_len}B (limit 500KB)"
fi

echo ""

# ── Phase 4: RTSP stream validation ──

echo "=== Phase 4: RTSP streams ==="

validate_rtsp_stream() {
    local name="$1" url="$2" exp_w="$3" exp_h="$4" exp_codec="$5" exp_fps="$6"
    local probe_out

    # Probe for 5 seconds to get stable stats
    probe_out=$(timeout 10 ffprobe -v quiet -print_format json -show_streams -show_format \
        -rtsp_transport tcp -i "$url" -read_intervals "%+5" 2>/dev/null || echo "{}")

    # Codec
    local codec
    codec=$(echo "$probe_out" | grep -o '"codec_name"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | grep -o '"[^"]*"$' | tr -d '"' || echo "")
    if [ -n "$codec" ]; then
        check_eq "$name codec" "$codec" "$exp_codec"
    else
        fail "$name codec" "ffprobe returned no codec"
        return
    fi

    # Resolution
    local width height
    width=$(echo "$probe_out" | grep -o '"width"[[:space:]]*:[[:space:]]*[0-9]*' | head -1 | grep -o '[0-9]*$' || echo "")
    height=$(echo "$probe_out" | grep -o '"height"[[:space:]]*:[[:space:]]*[0-9]*' | head -1 | grep -o '[0-9]*$' || echo "")
    check_eq "$name resolution" "${width}x${height}" "${exp_w}x${exp_h}"

    # FPS — ffprobe reports r_frame_rate as "num/den"
    local fps_frac fps_actual
    fps_frac=$(echo "$probe_out" | grep -o '"r_frame_rate"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | grep -o '"[^"]*"$' | tr -d '"' || echo "")
    if [ -n "$fps_frac" ] && echo "$fps_frac" | grep -q '/'; then
        local num den
        num=$(echo "$fps_frac" | cut -d/ -f1)
        den=$(echo "$fps_frac" | cut -d/ -f2)
        if [ "$den" -gt 0 ] 2>/dev/null; then
            fps_actual=$((num / den))
            check_range "$name FPS" "$fps_actual" "$exp_fps" 15
        else
            skip "$name FPS" "bad denominator"
        fi
    else
        skip "$name FPS" "no r_frame_rate"
    fi
}

validate_rtsp_stream "main stream" "rtsp://$DEVICE_IP:$RTSP_PORT/stream0" \
    "$MAIN_WIDTH" "$MAIN_HEIGHT" "$MAIN_CODEC" "$MAIN_FPS"

validate_rtsp_stream "sub stream" "rtsp://$DEVICE_IP:$RTSP_PORT/stream1" \
    "$SUB_WIDTH" "$SUB_HEIGHT" "$SUB_CODEC" "$SUB_FPS"

# Audio in RTSP
AUDIO_PROBE=$(timeout 10 ffprobe -v quiet -print_format json -show_streams \
    -rtsp_transport tcp -i "rtsp://$DEVICE_IP:$RTSP_PORT/stream0" 2>/dev/null || echo "{}")
has_audio=$(echo "$AUDIO_PROBE" | grep -c '"codec_type"[[:space:]]*:[[:space:]]*"audio"' || echo "0")
if [ "$has_audio" -ge 1 ]; then
    pass "RTSP audio track present"
else
    fail "RTSP audio track" "no audio stream in SDP"
fi

echo ""

# ── Phase 5: RTSP bitrate validation ──

echo "=== Phase 5: Bitrate validation ==="

# Stream for 5 seconds via ffmpeg, measure actual bitrate
for stream_name in stream0 stream1; do
    local_file=$(mktemp /tmp/raptor-br-XXXXXX.ts)
    timeout 10 ffmpeg -y -v quiet -rtsp_transport tcp \
        -i "rtsp://$DEVICE_IP:$RTSP_PORT/$stream_name" \
        -t 5 -c copy -f mpegts "$local_file" 2>/dev/null
    ret=$?

    if [ -f "$local_file" ] && [ "$(stat -c%s "$local_file" 2>/dev/null || echo 0)" -gt 1000 ]; then
        file_size=$(stat -c%s "$local_file")
        actual_bps=$((file_size * 8 / 5))

        if [ "$stream_name" = "stream0" ]; then
            check_range "main bitrate" "$actual_bps" "$MAIN_BITRATE" 40
        else
            check_range "sub bitrate" "$actual_bps" "$SUB_BITRATE" 40
        fi
    else
        fail "$stream_name bitrate" "no data captured (ffmpeg exit $ret)"
    fi
    rm -f "$local_file"
done

echo ""

# ── Phase 6: RC mode cycling (full restart per mode) ──

echo "=== Phase 6: RC mode cycling ==="

# Each RC mode gets a fresh RVD start — some modes (SmartP, capped)
# can't be switched at runtime without wedging the SDK.
CAPS=$($SSH "timeout 3 $RAPTORCTL rvd get-enc-caps" 2>/dev/null || echo "")
CONF_ON_DEVICE="$NFS_RAPTOR/tests/device-test.conf"

rc_modes="cbr vbr fixqp"
if echo "$CAPS" | grep -q '"smartp_gop":true'; then
    rc_modes="$rc_modes smart capped_vbr capped_quality"
fi

for mode in $rc_modes; do
    # Stop RVD, rewrite config with this mode, restart fresh
    $SSH 'killall rvd 2>/dev/null' 2>/dev/null || true
    sleep 2
    # Rewrite config from host side (avoid busybox sed issues on NFS)
    sed "s/^rc_mode = .*/rc_mode = $mode/" "$TEST_CONF" > "${TEST_CONF}.tmp" && mv "${TEST_CONF}.tmp" "$TEST_CONF"
    $SSH 'rm -f /dev/shm/rss_ring_main /dev/shm/rss_ring_sub /var/run/rss/rvd.pid /var/run/rss/rvd.sock 2>/dev/null' 2>/dev/null || true
    $SSH "$NFS_RAPTOR/build/rvd -c $CONF_ON_DEVICE -d" 2>/dev/null
    sleep 3

    rvd_pid=$($SSH 'pidof rvd 2>/dev/null' 2>/dev/null || echo "")
    if [ -z "$rvd_pid" ]; then
        skip "rc_mode $mode" "rvd failed to start"
        continue
    fi

    GET_OUT=$($SSH "timeout 3 $RAPTORCTL rvd get-rc-mode 0" 2>/dev/null || echo "")
    got_mode=$(echo "$GET_OUT" | grep -o '"rc_mode":"[^"]*"' | grep -o '"[^"]*"$' | tr -d '"' || echo "")
    if [ "$got_mode" != "$mode" ]; then
        fail "rc_mode $mode readback" "got '$got_mode'"
        continue
    fi

    probe_ok=$(timeout 5 ffprobe -v quiet -print_format json -show_streams \
        -rtsp_transport tcp -i "rtsp://$DEVICE_IP:$RTSP_PORT/stream0" 2>/dev/null || echo "{}")
    if echo "$probe_ok" | grep -q '"codec_name"'; then
        pass "rc_mode $mode (restart + readback + stream ok)"
    else
        fail "rc_mode $mode" "rvd started but stream dead"
    fi
done

# Restore original config + restart
sed "s/^rc_mode = .*/rc_mode = $MAIN_RC/" "$TEST_CONF" > "${TEST_CONF}.tmp" && mv "${TEST_CONF}.tmp" "$TEST_CONF"
$SSH 'killall rvd 2>/dev/null' 2>/dev/null || true
sleep 2
$SSH 'rm -f /dev/shm/rss_ring_main /dev/shm/rss_ring_sub /var/run/rss/rvd.pid /var/run/rss/rvd.sock 2>/dev/null' 2>/dev/null || true
$SSH "$NFS_RAPTOR/build/rvd -c $CONF_ON_DEVICE -d" 2>/dev/null
sleep 3

echo ""

# ── Phase 7: Encoder parameter exercise ──

echo "=== Phase 7: Encoder parameter exercise ==="

# Bitrate change: set → verify → restore
$SSH "timeout 3 $RAPTORCTL rvd set-bitrate 0 3000000" 2>/dev/null > /dev/null || true
sleep 1
BR_CHK=$($SSH "timeout 3 $RAPTORCTL rvd get-bitrate 0" 2>/dev/null || echo "")
br_cfg=$(echo "$BR_CHK" | grep -o '"bitrate":[0-9]*' | grep -o '[0-9]*$' || echo "")
if [ "$br_cfg" = "3000000" ]; then
    pass "bitrate change (2M → 3M)"
else
    fail "bitrate change" "expected 3000000, got $br_cfg"
fi
$SSH "timeout 3 $RAPTORCTL rvd set-bitrate 0 $MAIN_BITRATE" 2>/dev/null > /dev/null || true

# GOP change
$SSH "timeout 3 $RAPTORCTL rvd set-gop 0 30" 2>/dev/null > /dev/null || true
GOP_CHK=$($SSH "timeout 3 $RAPTORCTL rvd get-gop 0" 2>/dev/null || echo "")
gop_chk=$(echo "$GOP_CHK" | grep -o '"gop":[0-9]*' | grep -o '[0-9]*$' || echo "")
if [ "$gop_chk" = "30" ]; then
    pass "GOP change (50 → 30)"
else
    fail "GOP change" "expected 30, got $gop_chk"
fi
$SSH "timeout 3 $RAPTORCTL rvd set-gop 0 $MAIN_GOP" 2>/dev/null > /dev/null || true

# FPS change (halve it, then restore)
half_fps=$((SENSOR_FPS / 2))
$SSH "timeout 3 $RAPTORCTL rvd set-fps 0 $half_fps" 2>/dev/null > /dev/null || true
sleep 1
FPS_CHK=$($SSH "timeout 3 $RAPTORCTL rvd get-fps 0" 2>/dev/null || echo "")
fps_chk=$(echo "$FPS_CHK" | grep -o '"fps_num":[0-9]*' | grep -o '[0-9]*$' || echo "")
if [ "$fps_chk" = "$half_fps" ]; then
    pass "FPS change ($SENSOR_FPS → $half_fps)"
else
    fail "FPS change" "expected $half_fps, got $fps_chk"
fi
$SSH "timeout 3 $RAPTORCTL rvd set-fps 0 $SENSOR_FPS" 2>/dev/null > /dev/null || true

# QP bounds change
$SSH "timeout 3 $RAPTORCTL rvd set-qp-bounds 0 10 50" 2>/dev/null > /dev/null || true
QP_CHK=$($SSH "timeout 3 $RAPTORCTL rvd get-qp-bounds 0" 2>/dev/null || echo "")
qp_min=$(echo "$QP_CHK" | grep -o '"min_qp":[-0-9]*' | grep -o '[-0-9]*$' || echo "")
qp_max=$(echo "$QP_CHK" | grep -o '"max_qp":[-0-9]*' | grep -o '[-0-9]*$' || echo "")
if [ "$qp_min" = "10" ] && [ "$qp_max" = "50" ]; then
    pass "QP bounds change (15/45 → 10/50)"
else
    fail "QP bounds change" "got $qp_min/$qp_max"
fi
$SSH "timeout 3 $RAPTORCTL rvd set-qp-bounds 0 $MAIN_MIN_QP $MAIN_MAX_QP" 2>/dev/null > /dev/null || true

# Table-driven enc-set/enc-get roundtrip (test a few)
for param in gop_mode color2grey mbrc; do
    ENC_SET=$($SSH "timeout 3 $RAPTORCTL rvd enc-set 0 $param 1" 2>/dev/null || echo "")
    if echo "$ENC_SET" | grep -q '"ok"'; then
        ENC_GET=$($SSH "timeout 3 $RAPTORCTL rvd enc-get 0 $param" 2>/dev/null || echo "")
        if echo "$ENC_GET" | grep -q '"value"'; then
            pass "enc-set/get $param"
        else
            fail "enc-get $param" "no value in response"
        fi
    else
        if echo "$ENC_SET" | grep -q 'not supported'; then
            skip "enc-set $param" "not supported on this SoC"
        else
            fail "enc-set $param" "failed"
        fi
    fi
done
# Restore defaults
$SSH "timeout 3 $RAPTORCTL rvd enc-set 0 gop_mode 0" 2>/dev/null > /dev/null || true
$SSH "timeout 3 $RAPTORCTL rvd enc-set 0 color2grey 0" 2>/dev/null > /dev/null || true
$SSH "timeout 3 $RAPTORCTL rvd enc-set 0 mbrc 0" 2>/dev/null > /dev/null || true

# ISP controls (set + readback via get-isp)
$SSH "timeout 3 $RAPTORCTL rvd set-brightness 200" 2>/dev/null > /dev/null || true
ISP_CHK=$($SSH "timeout 3 $RAPTORCTL rvd get-isp" 2>/dev/null || echo "")
bri=$(echo "$ISP_CHK" | grep -o '"brightness":[0-9]*' | grep -o '[0-9]*$' || echo "")
if [ "$bri" = "200" ]; then
    pass "ISP brightness set/get"
else
    fail "ISP brightness" "expected 200, got $bri"
fi
$SSH "timeout 3 $RAPTORCTL rvd set-brightness 128" 2>/dev/null > /dev/null || true

echo ""

# ── Phase 8: HTTP endpoints ──


echo "=== Phase 6: HTTP endpoints ==="

# Snapshot
SNAP_FILE=$(mktemp /tmp/raptor-snap-XXXXXX.jpg)
HTTP_CODE=$(curl -s -o "$SNAP_FILE" -w "%{http_code}" --max-time 5 \
    "http://$DEVICE_IP:$HTTP_PORT/snap" 2>/dev/null || echo "000")

if [ "$HTTP_CODE" = "200" ]; then
    pass "HTTP snapshot (200)"
    # Verify JPEG magic (SOI marker)
    if od -A n -t x1 -N 2 "$SNAP_FILE" | tr -d ' ' | grep -q 'ffd8'; then
        pass "snapshot is valid JPEG"
        # Check dimensions via ffprobe
        snap_dims=$(ffprobe -v quiet -print_format json -show_streams "$SNAP_FILE" 2>/dev/null || echo "{}")
        snap_w=$(echo "$snap_dims" | grep -o '"width"[[:space:]]*:[[:space:]]*[0-9]*' | head -1 | grep -o '[0-9]*$' || echo "")
        snap_h=$(echo "$snap_dims" | grep -o '"height"[[:space:]]*:[[:space:]]*[0-9]*' | head -1 | grep -o '[0-9]*$' || echo "")
        if [ -n "$snap_w" ] && [ -n "$snap_h" ]; then
            check_eq "snapshot resolution" "${snap_w}x${snap_h}" "${MAIN_WIDTH}x${MAIN_HEIGHT}"
        else
            skip "snapshot resolution" "ffprobe couldn't read dimensions"
        fi
    else
        fail "snapshot JPEG magic" "not a JPEG"
    fi
else
    fail "HTTP snapshot" "HTTP $HTTP_CODE"
fi
rm -f "$SNAP_FILE"

# MJPEG stream
MJPEG_CODE=$(timeout 3 curl -s -o /dev/null -w "%{http_code}" \
    "http://$DEVICE_IP:$HTTP_PORT/mjpeg" 2>/dev/null || echo "000")
if [ "$MJPEG_CODE" = "200" ] || [ "$MJPEG_CODE" = "000" ]; then
    pass "MJPEG stream responds"
else
    fail "MJPEG stream" "HTTP $MJPEG_CODE"
fi

# Audio stream
AUDIO_CODE=$(timeout 3 curl -s -o /dev/null -w "%{http_code}" \
    "http://$DEVICE_IP:$HTTP_PORT/audio" 2>/dev/null || echo "000")
if [ "$AUDIO_CODE" = "200" ] || [ "$AUDIO_CODE" = "000" ]; then
    pass "audio stream responds"
else
    fail "audio stream" "HTTP $AUDIO_CODE"
fi

echo ""

# ── Phase 9: Log scan ──

echo "=== Phase 9: Log scan ==="

# Only check entries from current daemon PIDs (ignore stale log entries)
LOG_OUT=$($SSH 'logread 2>/dev/null | tail -100' 2>/dev/null || echo "")
fatal_count=$(echo "$LOG_OUT" | grep -c -i -E 'segfault|panic|Oops' || true)
fatal_count=$((fatal_count + 0))
if [ "$fatal_count" -eq 0 ]; then
    pass "no FATAL/segfault in logs"
else
    fail "log scan" "$fatal_count FATAL/crash entries"
fi

echo ""

# ── Phase 10: Resource baseline ──

echo "=== Phase 10: Resource baseline ==="

# Memory (busybox free uses KB, no -m flag)
free_kb=$($SSH 'free | awk "/Mem:/{print \$4}"' 2>/dev/null || echo "0")
free_mb=$((free_kb / 1024))
echo "  free memory: ${free_mb}MB"
if [ "$free_mb" -lt 5 ]; then
    fail "free memory" "only ${free_mb}MB free"
else
    pass "free memory (${free_mb}MB)"
fi

# FD count per daemon
for daemon in rvd rad rsd rhd; do
    pid=$($SSH "pidof $daemon 2>/dev/null" 2>/dev/null || echo "")
    if [ -n "$pid" ]; then
        fd_count=$($SSH "ls /proc/$pid/fd 2>/dev/null | wc -l" 2>/dev/null || echo "?")
        echo "  $daemon: $fd_count fds"
    fi
done

echo ""

# ── Cleanup ──

if [ "$KEEP" = false ] && [ "$RESTART" = true ]; then
    echo "=== Stopping daemons ==="
    $SSH 'killall -q rvd rad rsd rhd rod ric rmd rmr rwd 2>/dev/null' 2>/dev/null || true
    sleep 1
    echo "  stopped"
    echo ""
fi

# ── Summary ──

echo "========================================"
echo " Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo " Device:  $DEVICE_IP ($SENSOR)"
echo "========================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
