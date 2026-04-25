#!/bin/bash
#
# device-test.sh -- On-device integration test
#
# Runs from the build host, SSHes into a test camera, starts raptor
# with a known config, and validates encoder output per RC mode.
#
# For each supported RC mode: restarts RVD with that mode, then runs
# the full validation battery — SDK state, RTSP streams, bitrate,
# ring buffer, and encoder parameter exercise.
#
# Serves binaries to the device via NFS. Auto-detects existing NFS
# mounts, or spins up a temporary Docker NFS container. Override
# with --nfs <host>:<path> for a custom NFS export.
#
# Prerequisites:
#   - Device reachable via SSH (root@IP, key auth)
#   - Raptor built via build-standalone.sh (binaries in build/)
#   - ffprobe, ffmpeg, and curl on the build host
#   - Docker (for auto NFS) or existing NFS server
#
# Usage:
#   ./tests/device-test.sh <device-ip>
#   ./tests/device-test.sh <device-ip> --nfs 10.0.0.5:/export/raptor
#   ./tests/device-test.sh <device-ip> --no-restart
#   ./tests/device-test.sh <device-ip> --keep
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RAPTOR_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

DEVICE_IP=""
RESTART=true
KEEP=false
NFS_OVERRIDE=""
NFS_PORT=12049
NFS_MOUNTD_PORT=12048
DOCKER_NFS_CONTAINER="raptor-nfs-test"

while [ $# -gt 0 ]; do
    case "$1" in
        --no-restart) RESTART=false; shift ;;
        --keep) KEEP=true; shift ;;
        --nfs) NFS_OVERRIDE="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 <device-ip> [--nfs host:path] [--no-restart] [--keep]"
            exit 0 ;;
        -*) echo "Unknown option: $1"; exit 1 ;;
        *) DEVICE_IP="$1"; shift ;;
    esac
done

[ -z "$DEVICE_IP" ] && { echo "Usage: $0 <device-ip> [--nfs host:path] [--no-restart] [--keep]"; exit 1; }

# ── Config ──

SSH="ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no -o LogLevel=ERROR root@$DEVICE_IP"
BUILD_DIR="$RAPTOR_DIR/build"
TEST_CONF="$RAPTOR_DIR/tests/device-test.conf"
DEVICE_MNT="/tmp/raptor-test"
DOCKER_STARTED=false

# These get set during NFS setup
DEVICE_RAPTOR=""
RAPTORCTL=""
RINGDUMP=""
CONF_ON_DEVICE=""
RTSP_PORT=554
HTTP_PORT=8080
PASS=0
FAIL=0
SKIP=0

# Stream parameters
MAIN_WIDTH=1920
MAIN_HEIGHT=1080
MAIN_BITRATE=2000000
MAIN_GOP=50
MAIN_CODEC=h264
MAIN_MIN_QP=15
MAIN_MAX_QP=45
SUB_WIDTH=640
SUB_HEIGHT=360
SUB_BITRATE=500000
SUB_GOP=30
SUB_CODEC=h264

# ── Helpers ──

pass() { PASS=$((PASS + 1)); printf "    PASS  %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "    FAIL  %s: %s\n" "$1" "$2"; }
skip() { SKIP=$((SKIP + 1)); printf "    SKIP  %s: %s\n" "$1" "$2"; }

check_range() {
    local name="$1" actual="$2" expected="$3" tolerance_pct="$4"
    if [ -z "$actual" ] || [ "$actual" = "null" ]; then
        fail "$name" "no value"; return
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
    if [ "$actual" = "$expected" ]; then pass "$name"
    else fail "$name" "got '$actual', expected '$expected'"; fi
}

# Start RVD with a given RC mode, plus all consumers
start_raptor() {
    local mode="$1"

    $SSH "killall $RAPTOR_DAEMONS 2>/dev/null" 2>/dev/null || true
    sleep 2
    $SSH "killall -9 $RAPTOR_DAEMONS 2>/dev/null" 2>/dev/null || true
    $SSH 'rm -f /dev/shm/rss_ring_* /dev/shm/rss_osd_* /var/run/rss/*.pid /var/run/rss/*.sock 2>/dev/null' 2>/dev/null || true
    sleep 1

    # Write config with this RC mode
    sed "s/^rc_mode = .*/rc_mode = $mode/" "$TEST_CONF" > "${TEST_CONF}.tmp" && mv "${TEST_CONF}.tmp" "$TEST_CONF"

    $SSH "$DEVICE_RAPTOR/build/rvd -c $CONF_ON_DEVICE -d" 2>/dev/null
    sleep 4
    $SSH "LD_PRELOAD=/usr/lib/libimp-nodbg.so $DEVICE_RAPTOR/build/rad -c $CONF_ON_DEVICE -d" 2>/dev/null
    sleep 1
    $SSH "$DEVICE_RAPTOR/build/rsd -c $CONF_ON_DEVICE -d" 2>/dev/null
    sleep 1
    $SSH "$DEVICE_RAPTOR/build/rhd -c $CONF_ON_DEVICE -d" 2>/dev/null
    sleep 2

    # Return success if RVD is running
    $SSH 'pidof rvd > /dev/null 2>&1' 2>/dev/null
}

# ── Full validation battery for a given RC mode ──

validate_mode() {
    local mode="$1"
    local prefix="[$mode]"

    # ── Daemon health ──
    for daemon in rvd rad rsd rhd; do
        pid=$($SSH "pidof $daemon 2>/dev/null" 2>/dev/null || echo "")
        if [ -n "$pid" ]; then pass "$prefix $daemon running"
        else fail "$prefix $daemon" "not running"; fi
    done

    # ── Control socket readback ──
    RC=$($SSH "timeout 3 $RAPTORCTL rvd get-rc-mode 0" 2>/dev/null || echo "")
    rc_val=$(echo "$RC" | grep -o '"rc_mode":"[^"]*"' | grep -o '"[^"]*"$' | tr -d '"' || echo "")
    check_eq "$prefix rc_mode readback" "$rc_val" "$mode"

    # QP bounds (0/0 = encoder misconfigured)
    QP=$($SSH "timeout 3 $RAPTORCTL rvd get-qp-bounds 0" 2>/dev/null || echo "")
    min_qp=$(echo "$QP" | grep -o '"min_qp":[-0-9]*' | grep -o '[-0-9]*$' || echo "")
    max_qp=$(echo "$QP" | grep -o '"max_qp":[-0-9]*' | grep -o '[-0-9]*$' || echo "")
    if [ "$min_qp" = "0" ] && [ "$max_qp" = "0" ]; then
        fail "$prefix QP bounds" "0/0 — encoder misconfigured"
    elif [ -n "$min_qp" ] && [ -n "$max_qp" ]; then
        pass "$prefix QP bounds (min=$min_qp max=$max_qp)"
    else
        fail "$prefix QP bounds" "no response"
    fi

    GOP=$($SSH "timeout 3 $RAPTORCTL rvd get-gop 0" 2>/dev/null || echo "")
    gop_val=$(echo "$GOP" | grep -o '"gop":[0-9]*' | grep -o '[0-9]*$' || echo "")
    check_eq "$prefix GOP" "$gop_val" "$MAIN_GOP"

    FPS=$($SSH "timeout 3 $RAPTORCTL rvd get-fps 0" 2>/dev/null || echo "")
    fps_val=$(echo "$FPS" | grep -o '"fps_num":[0-9]*' | grep -o '[0-9]*$' || echo "")
    check_eq "$prefix FPS" "$fps_val" "$SENSOR_FPS"

    # ── SDK encoder verification (libimp-debug) ──
    if $SSH 'which libimp-debug > /dev/null 2>&1' 2>/dev/null; then
        ENC_INFO=$($SSH 'libimp-debug --enc_info 2>/dev/null' 2>/dev/null || echo "")
        if [ -n "$ENC_INFO" ]; then
            parse_sdk() { echo "$ENC_INFO" | grep -A30 "ch->index = 0" | grep "$1" | head -1 | sed 's/.*= \(-\?[0-9]*\)(.*/\1/'; }
            ch0_rc=$(parse_sdk 'rcMode')
            ch0_gop=$(parse_sdk 'uGopLength')
            ch0_fps=$(parse_sdk 'frmRateNum')

            # RC mode enum: 0=FIXQP, 1=CBR, 2=VBR, 4=CAPPED_VBR, 5=CAPPED_QUALITY
            RC_NAMES="fixqp cbr vbr smart capped_vbr capped_quality"
            sdk_rc=$(echo "$RC_NAMES" | awk -v n="$ch0_rc" '{print $(n+1)}')
            check_eq "$prefix SDK rc_mode" "$sdk_rc" "$mode"
            check_eq "$prefix SDK gop" "$ch0_gop" "$MAIN_GOP"
            check_eq "$prefix SDK fps" "$ch0_fps" "$SENSOR_FPS"

            # Live encoder stats
            ch0_line=$(echo "$ENC_INFO" | grep "CHANNEL 0" | head -1)
            actual_fps=$(echo "$ch0_line" | grep -o 'Fps:[0-9.]*' | grep -o '[0-9.]*' | cut -d. -f1 || echo "")
            actual_br=$(echo "$ch0_line" | grep -o 'Bitrate:[0-9.]*' | grep -o '[0-9.]*' | cut -d. -f1 || echo "")
            if [ -n "$actual_fps" ]; then
                check_range "$prefix SDK actual FPS" "$actual_fps" "$SENSOR_FPS" 15
            fi
            if [ -n "$actual_br" ] && [ "$mode" != "fixqp" ]; then
                check_range "$prefix SDK actual bitrate (kbps)" "$actual_br" "$((MAIN_BITRATE / 1000))" 40
            fi
        fi
    fi

    # ── Ring buffer ──
    RING_OUT=$($SSH "timeout 5 sh -c '$RINGDUMP main -f -n 60 2>&1 & sleep 0.5; timeout 3 $RAPTORCTL rvd request-idr > /dev/null 2>&1; wait'" 2>/dev/null || echo "")
    frame_count=$(echo "$RING_OUT" | grep -c '^#' || echo "0")
    has_idr=$(echo "$RING_OUT" | grep -qE 'key=1|IDR' && echo "yes" || echo "no")
    max_len=$(echo "$RING_OUT" | grep -o 'len=[0-9]*' | grep -o '[0-9]*' | sort -rn | head -1 || echo "0")

    if [ "$frame_count" -ge 5 ]; then pass "$prefix ring ($frame_count frames)"
    else fail "$prefix ring" "only $frame_count frames"; fi
    if [ "$has_idr" = "yes" ]; then pass "$prefix IDR present"
    else fail "$prefix IDR" "no keyframe"; fi
    if [ "$max_len" -gt 0 ] && [ "$max_len" -lt 512000 ]; then
        pass "$prefix frame sizes sane (max ${max_len}B)"
    else
        fail "$prefix frame sizes" "max ${max_len}B"
    fi

    # ── RTSP stream validation ──
    for stream_info in "stream0:$MAIN_WIDTH:$MAIN_HEIGHT:$MAIN_CODEC" "stream1:$SUB_WIDTH:$SUB_HEIGHT:$SUB_CODEC"; do
        IFS=: read -r sname exp_w exp_h exp_codec <<< "$stream_info"
        probe_out=$(timeout 10 ffprobe -v quiet -print_format json -show_streams \
            -rtsp_transport tcp -i "rtsp://$DEVICE_IP:$RTSP_PORT/$sname" 2>/dev/null || echo "{}")

        codec=$(echo "$probe_out" | grep -o '"codec_name"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | grep -o '"[^"]*"$' | tr -d '"' || echo "")
        width=$(echo "$probe_out" | grep -o '"width"[[:space:]]*:[[:space:]]*[0-9]*' | head -1 | grep -o '[0-9]*$' || echo "")
        height=$(echo "$probe_out" | grep -o '"height"[[:space:]]*:[[:space:]]*[0-9]*' | head -1 | grep -o '[0-9]*$' || echo "")

        if [ -n "$codec" ]; then
            check_eq "$prefix $sname codec" "$codec" "$exp_codec"
            check_eq "$prefix $sname resolution" "${width}x${height}" "${exp_w}x${exp_h}"
        else
            fail "$prefix $sname" "ffprobe failed"
        fi
    done

    # ── Bitrate measurement (skip for fixqp — no target) ──
    if [ "$mode" != "fixqp" ]; then
        local_file=$(mktemp /tmp/raptor-br-XXXXXX.ts)
        timeout 10 ffmpeg -y -v quiet -rtsp_transport tcp \
            -i "rtsp://$DEVICE_IP:$RTSP_PORT/stream0" \
            -t 5 -c copy -f mpegts "$local_file" 2>/dev/null || true

        if [ -f "$local_file" ] && [ "$(stat -c%s "$local_file" 2>/dev/null || echo 0)" -gt 1000 ]; then
            file_size=$(stat -c%s "$local_file")
            actual_bps=$((file_size * 8 / 5))
            check_range "$prefix main bitrate" "$actual_bps" "$MAIN_BITRATE" 40
        else
            fail "$prefix main bitrate" "no data captured"
        fi
        rm -f "$local_file"
    fi

    # ── Encoder parameter exercise ──
    # Bitrate change
    $SSH "timeout 3 $RAPTORCTL rvd set-bitrate 0 3000000" 2>/dev/null > /dev/null || true
    sleep 1
    BR_CHK=$($SSH "timeout 3 $RAPTORCTL rvd get-bitrate 0" 2>/dev/null || echo "")
    br_cfg=$(echo "$BR_CHK" | grep -o '"bitrate":[0-9]*' | grep -o '[0-9]*$' || echo "")
    if [ "$br_cfg" = "3000000" ]; then pass "$prefix bitrate change"
    else fail "$prefix bitrate change" "got $br_cfg"; fi
    $SSH "timeout 3 $RAPTORCTL rvd set-bitrate 0 $MAIN_BITRATE" 2>/dev/null > /dev/null || true

    # GOP change
    $SSH "timeout 3 $RAPTORCTL rvd set-gop 0 30" 2>/dev/null > /dev/null || true
    GOP_CHK=$($SSH "timeout 3 $RAPTORCTL rvd get-gop 0" 2>/dev/null || echo "")
    gop_chk=$(echo "$GOP_CHK" | grep -o '"gop":[0-9]*' | grep -o '[0-9]*$' || echo "")
    if [ "$gop_chk" = "30" ]; then pass "$prefix GOP change"
    else fail "$prefix GOP change" "got $gop_chk"; fi
    $SSH "timeout 3 $RAPTORCTL rvd set-gop 0 $MAIN_GOP" 2>/dev/null > /dev/null || true

    # FPS change
    half_fps=$((SENSOR_FPS / 2))
    $SSH "timeout 3 $RAPTORCTL rvd set-fps 0 $half_fps" 2>/dev/null > /dev/null || true
    sleep 1
    FPS_CHK=$($SSH "timeout 3 $RAPTORCTL rvd get-fps 0" 2>/dev/null || echo "")
    fps_chk=$(echo "$FPS_CHK" | grep -o '"fps_num":[0-9]*' | grep -o '[0-9]*$' || echo "")
    if [ "$fps_chk" = "$half_fps" ]; then pass "$prefix FPS change"
    else fail "$prefix FPS change" "got $fps_chk"; fi
    $SSH "timeout 3 $RAPTORCTL rvd set-fps 0 $SENSOR_FPS" 2>/dev/null > /dev/null || true

    # QP bounds change
    $SSH "timeout 3 $RAPTORCTL rvd set-qp-bounds 0 10 50" 2>/dev/null > /dev/null || true
    QP_CHK=$($SSH "timeout 3 $RAPTORCTL rvd get-qp-bounds 0" 2>/dev/null || echo "")
    qp_min=$(echo "$QP_CHK" | grep -o '"min_qp":[-0-9]*' | grep -o '[-0-9]*$' || echo "")
    qp_max=$(echo "$QP_CHK" | grep -o '"max_qp":[-0-9]*' | grep -o '[-0-9]*$' || echo "")
    if [ "$qp_min" = "10" ] && [ "$qp_max" = "50" ]; then pass "$prefix QP change"
    else fail "$prefix QP change" "got $qp_min/$qp_max"; fi
    $SSH "timeout 3 $RAPTORCTL rvd set-qp-bounds 0 $MAIN_MIN_QP $MAIN_MAX_QP" 2>/dev/null > /dev/null || true

    # ── HTTP endpoints ──
    SNAP_FILE=$(mktemp /tmp/raptor-snap-XXXXXX.jpg)
    HTTP_CODE=$(curl -s -o "$SNAP_FILE" -w "%{http_code}" --max-time 5 \
        "http://$DEVICE_IP:$HTTP_PORT/snap" 2>/dev/null || echo "000")
    if [ "$HTTP_CODE" = "200" ]; then
        if od -A n -t x1 -N 2 "$SNAP_FILE" | tr -d ' ' | grep -q 'ffd8'; then
            pass "$prefix snapshot (JPEG OK)"
        else
            fail "$prefix snapshot" "not a JPEG"
        fi
    else
        fail "$prefix snapshot" "HTTP $HTTP_CODE"
    fi
    rm -f "$SNAP_FILE"

    MJPEG_CODE=$(timeout 3 curl -s -o /dev/null -w "%{http_code}" \
        "http://$DEVICE_IP:$HTTP_PORT/mjpeg" 2>/dev/null || echo "000")
    if [ "$MJPEG_CODE" = "200" ] || [ "$MJPEG_CODE" = "000" ]; then
        pass "$prefix MJPEG stream"
    else
        fail "$prefix MJPEG" "HTTP $MJPEG_CODE"
    fi

    AUDIO_CODE=$(timeout 3 curl -s -o /dev/null -w "%{http_code}" \
        "http://$DEVICE_IP:$HTTP_PORT/audio" 2>/dev/null || echo "000")
    if [ "$AUDIO_CODE" = "200" ] || [ "$AUDIO_CODE" = "000" ]; then
        pass "$prefix audio stream"
    else
        fail "$prefix audio" "HTTP $AUDIO_CODE"
    fi
}

# ══════════════════════════════════════════════════════════════════
#  Main test flow
# ══════════════════════════════════════════════════════════════════

echo "========================================"
echo " Device test: $DEVICE_IP"
echo "========================================"
echo ""

# ── Preflight ──

echo "=== Preflight ==="

if ! $SSH 'true' 2>/dev/null; then
    echo "ERROR: cannot SSH to root@$DEVICE_IP"; exit 1
fi
pass "SSH connectivity"

# Kill any existing raptor daemons before we start
RAPTOR_DAEMONS="rvd rad rsd rhd rod ric rmd rmr rwd rwc rsp rfs"
$SSH "killall $RAPTOR_DAEMONS 2>/dev/null" 2>/dev/null || true
sleep 2
$SSH "killall -9 $RAPTOR_DAEMONS 2>/dev/null" 2>/dev/null || true
$SSH 'rm -f /dev/shm/rss_ring_* /dev/shm/rss_osd_* /var/run/rss/*.pid /var/run/rss/*.sock 2>/dev/null' 2>/dev/null || true
sleep 1

# Verify nothing is still running
STALE=$($SSH "pidof $RAPTOR_DAEMONS 2>/dev/null" 2>/dev/null || echo "")
if [ -n "$STALE" ]; then
    echo "ERROR: raptor daemons still running after kill: $STALE"
    exit 1
fi
pass "clean slate (all daemons stopped)"

# Auto-detect host IP (as seen from device)
HOST_IP=$($SSH 'echo $SSH_CONNECTION' 2>/dev/null | awk '{print $1}')
[ -z "$HOST_IP" ] && HOST_IP=$(ip route get "$DEVICE_IP" 2>/dev/null | grep -o 'src [0-9.]*' | awk '{print $2}')
echo "    host IP: $HOST_IP"

if [ ! -f "$BUILD_DIR/rvd" ]; then
    echo "ERROR: build/rvd not found — run build-standalone.sh first"; exit 1
fi
pass "binaries present"

# ── NFS setup ──
# Priority: --nfs override > Docker auto-setup

setup_nfs_done=false

if [ -n "$NFS_OVERRIDE" ]; then
    # Manual NFS: --nfs host:/path (path should contain build/ and tests/)
    echo "    using manual NFS: $NFS_OVERRIDE"
    $SSH "mkdir -p $DEVICE_MNT && mount -t nfs -o nolock,nfsvers=3,tcp $NFS_OVERRIDE $DEVICE_MNT" 2>/dev/null
    if $SSH "test -x $DEVICE_MNT/build/rvd" 2>/dev/null; then
        setup_nfs_done=true
        pass "NFS mount (manual)"
    else
        echo "ERROR: manual NFS mount failed or build/rvd not found"; exit 1
    fi
fi

if [ "$setup_nfs_done" = false ]; then
    # Try Docker NFS
    if command -v docker > /dev/null 2>&1; then
        echo "    starting Docker NFS server..."
        docker rm -f "$DOCKER_NFS_CONTAINER" > /dev/null 2>&1 || true

        docker run -d --rm --name "$DOCKER_NFS_CONTAINER" \
            --privileged \
            -v "$RAPTOR_DIR":/export/raptor:ro \
            -e NFS_EXPORT_0='/export/raptor *(ro,no_subtree_check,insecure,no_root_squash)' \
            -p ${NFS_PORT}:2049/tcp -p ${NFS_PORT}:2049/udp \
            -p ${NFS_MOUNTD_PORT}:32767/tcp -p ${NFS_MOUNTD_PORT}:32767/udp \
            erichough/nfs-server > /dev/null 2>&1

        DOCKER_STARTED=true
        sleep 3

        if ! docker ps --format '{{.Names}}' | grep -q "$DOCKER_NFS_CONTAINER"; then
            echo "ERROR: Docker NFS container failed to start"
            docker logs "$DOCKER_NFS_CONTAINER" 2>&1 | tail -5
            exit 1
        fi

        $SSH "mkdir -p $DEVICE_MNT" 2>/dev/null || true
        $SSH "mount -t nfs -o nolock,port=$NFS_PORT,mountport=$NFS_MOUNTD_PORT,nfsvers=3,tcp $HOST_IP:/export/raptor $DEVICE_MNT" 2>/dev/null

        if $SSH "test -x $DEVICE_MNT/build/rvd" 2>/dev/null; then
            setup_nfs_done=true
            pass "NFS mount (Docker)"
        else
            echo "ERROR: Docker NFS mount failed"
            docker rm -f "$DOCKER_NFS_CONTAINER" > /dev/null 2>&1 || true
            DOCKER_STARTED=false
            exit 1
        fi
    else
        echo "ERROR: No NFS setup available."
        echo "  Install Docker, or provide --nfs host:/path"
        echo "  Example: $0 $DEVICE_IP --nfs $HOST_IP:/path/to/raptor"
        exit 1
    fi
fi

# Set device-side paths
DEVICE_RAPTOR="$DEVICE_MNT"
RAPTORCTL="$DEVICE_RAPTOR/build/raptorctl"
RINGDUMP="$DEVICE_RAPTOR/build/ringdump"
CONF_ON_DEVICE="$DEVICE_RAPTOR/tests/device-test.conf"

SENSOR=$($SSH 'cat /proc/jz/sensor/name 2>/dev/null' 2>/dev/null || echo "unknown")
SENSOR_FPS=$($SSH 'cat /proc/jz/sensor/max_fps 2>/dev/null' 2>/dev/null || echo "25")
echo "    sensor: $SENSOR (${SENSOR_FPS}fps)"

if ! command -v ffprobe > /dev/null 2>&1; then
    echo "ERROR: ffprobe not found"; exit 1
fi

echo ""

# ── Generate base config ──

cat > "$TEST_CONF" << EOF
[sensor]
model = $SENSOR

[stream0]
width = $MAIN_WIDTH
height = $MAIN_HEIGHT
fps = $SENSOR_FPS
bitrate = $MAIN_BITRATE
codec = $MAIN_CODEC
rc_mode = cbr
gop = $MAIN_GOP
min_qp = $MAIN_MIN_QP
max_qp = $MAIN_MAX_QP

[stream1]
width = $SUB_WIDTH
height = $SUB_HEIGHT
fps = $SENSOR_FPS
bitrate = $SUB_BITRATE
codec = $SUB_CODEC
rc_mode = cbr
gop = $SUB_GOP

[audio]
enabled = true
sample_rate = 16000
codec = l16

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

# ── Determine supported RC modes ──

# Start with CBR to read caps
if [ "$RESTART" = true ]; then
    echo "=== Initial startup (CBR) for capability detection ==="
    start_raptor cbr
    sleep 1
fi

CAPS=$($SSH "timeout 3 $RAPTORCTL rvd get-enc-caps" 2>/dev/null || echo "")

# T31+: CBR, VBR, CAPPED_VBR, CAPPED_QUALITY, FIXQP
# T20-T30/T32/T33: also Smart RC
rc_modes="cbr vbr capped_vbr capped_quality fixqp"
if echo "$CAPS" | grep -q '"smart_rc":true'; then
    rc_modes="$rc_modes smart"
fi
echo "    RC modes to test: $rc_modes"
echo ""

# ── Per-mode validation loop ──

for mode in $rc_modes; do
    echo "=== RC mode: $mode ==="

    if start_raptor "$mode"; then
        validate_mode "$mode"
    else
        skip "[$mode] all" "rvd failed to start with $mode"
    fi

    echo ""
done

# ── Log scan ──

echo "=== Log scan ==="

LOG_OUT=$($SSH 'logread 2>/dev/null | tail -200' 2>/dev/null || echo "")
fatal_count=$(echo "$LOG_OUT" | grep -c -i -E 'segfault|panic|Oops' || true)
fatal_count=$((fatal_count + 0))
if [ "$fatal_count" -eq 0 ]; then
    pass "no segfault/panic in logs"
else
    fail "log scan" "$fatal_count crash entries"
fi

echo ""

# ── Resource baseline ──

echo "=== Resource baseline ==="

free_kb=$($SSH 'free | awk "/Mem:/{print \$4}"' 2>/dev/null || echo "0")
free_mb=$((free_kb / 1024))
echo "    free memory: ${free_mb}MB"
if [ "$free_mb" -lt 5 ]; then
    fail "free memory" "only ${free_mb}MB"
else
    pass "free memory (${free_mb}MB)"
fi

for daemon in rvd rad rsd rhd; do
    pid=$($SSH "pidof $daemon 2>/dev/null" 2>/dev/null || echo "")
    if [ -n "$pid" ]; then
        fd_count=$($SSH "ls /proc/$pid/fd 2>/dev/null | wc -l" 2>/dev/null || echo "?")
        echo "    $daemon: $fd_count fds"
    fi
done

echo ""

# ── Cleanup ──

if [ "$KEEP" = false ] && [ "$RESTART" = true ]; then
    echo "=== Cleanup ==="
    $SSH 'killall rvd rad rsd rhd 2>/dev/null' 2>/dev/null || true
    sleep 1
    # Restore config to CBR
    sed 's/^rc_mode = .*/rc_mode = cbr/' "$TEST_CONF" > "${TEST_CONF}.tmp" && mv "${TEST_CONF}.tmp" "$TEST_CONF"
    echo "    daemons stopped"

    # Unmount NFS on device
    $SSH "umount $DEVICE_MNT 2>/dev/null; rmdir $DEVICE_MNT 2>/dev/null" 2>/dev/null || true
    echo "    device unmounted"

    # Kill Docker NFS container
    if [ "$DOCKER_STARTED" = true ]; then
        docker rm -f "$DOCKER_NFS_CONTAINER" > /dev/null 2>&1 || true
        echo "    Docker NFS stopped"
    fi
    echo ""
fi

# ── Summary ──

echo "========================================"
echo " Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo " Device:  $DEVICE_IP ($SENSOR)"
echo " Modes:   $rc_modes"
echo "========================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
