#!/bin/bash
#
# test-rcd.sh -- rcd (external cameras) against fake EthCam modules
#
# Two fake modules (tests/fake_ethcam.py) on 127.0.0.12 (H.264, slot 1)
# and 127.0.0.13 (H.265, slot 2) replay clips that ffmpeg makes here. The
# test checks the rings decode, carry stream info, lose no frames, honour
# keyframe requests, and that a camera that goes away comes back.
#
# Requires: asan-out (./build-asan.sh), python3, ffmpeg with libx264;
# libx265 for the H.265 camera.

set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RAPTOR_DIR="$(dirname "$SCRIPT_DIR")"
OUT="$RAPTOR_DIR/asan-out"
WORK=$(mktemp -d /tmp/test-rcd.XXXXXX)
PASS=0
FAIL=0
SKIP=0
PIDS=()

pass() { echo "  PASS  $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL  $1"; FAIL=$((FAIL + 1)); }
skip() { echo "  SKIP  $1"; SKIP=$((SKIP + 1)); }

cleanup() {
	for p in "${PIDS[@]}"; do
		kill "$p" 2>/dev/null
	done
	wait 2>/dev/null
	rm -rf "${WORK:?}"
}
trap cleanup EXIT

for t in python3 ffmpeg ffprobe; do
	if ! command -v "$t" > /dev/null; then
		echo "test-rcd: $t not installed, skipping"
		exit 0
	fi
done
if [ ! -x "$OUT/rcd" ] || [ ! -x "$OUT/ringdump" ]; then
	echo "test-rcd: run ./build-asan.sh first"
	exit 1
fi

# long GOP so a keyframe request is visible against the natural I frames
ffmpeg -v error -f lavfi -i testsrc=size=320x240:rate=30 -frames:v 240 -c:v libx264 \
	-bf 0 -x264-params keyint=240:scenecut=0 -f h264 "$WORK/clip.h264" || exit 1
H265=1
ffmpeg -v error -f lavfi -i testsrc=size=320x240:rate=30 -frames:v 240 -c:v libx265 \
	-x265-params keyint=240:scenecut=0:bframes=0:log-level=error -f hevc \
	"$WORK/clip.hevc" 2> /dev/null || H265=0

mkdir -p /var/run/rss 2> /dev/null || { sudo mkdir -p /var/run/rss && sudo chmod 1777 /var/run/rss; }
rm -f /var/run/rss/rcd.pid /var/run/rss/rcd.sock

fake_start() { # ip [extra args]
	local ip=$1
	shift
	python3 "$SCRIPT_DIR/fake_ethcam.py" --ip "$ip" --h264 "$WORK/clip.h264" "$@" \
		> "$WORK/fake-$ip.log" 2>&1 &
	PIDS+=($!)
	FAKE_PID=$!
	for _ in $(seq 50); do
		grep -q "fake ethcam" "$WORK/fake-$ip.log" && return 0
		sleep 0.1
	done
	return 1
}

fake_start 127.0.0.12 || { echo "fake module did not start"; exit 1; }
FAKE1=$FAKE_PID
if [ $H265 = 1 ]; then
	fake_start 127.0.0.13 --h265 "$WORK/clip.hevc" || exit 1
fi

cat > "$WORK/raptor.conf" << EOF
[cameras]
enabled = true
iface =
dhcp = false
power_gpio = -1

[camera1]
enabled = true
ip = 127.0.0.12
slot = 1
codec = h264
sub = true

[camera2]
enabled = $([ $H265 = 1 ] && echo true || echo false)
ip = 127.0.0.13
slot = 2
codec = h265
sub = false
EOF

"$OUT/rcd" -c "$WORK/raptor.conf" -f -d > "$WORK/rcd.log" 2>&1 &
PIDS+=($!)
RCD_PID=$!

ctl() { "$OUT/raptorctl" rcd "$@" 2> /dev/null; }
cam_field() { # camera field
	ctl status | python3 -c "
import json, sys
d = json.load(sys.stdin)
c = [c for c in d['cameras'] if c['camera'] == $1][0]
v = c
for k in '$2'.split('.'):
    v = v[int(k)] if isinstance(v, list) else v[k]
print(v)" 2> /dev/null
}
wait_state() { # camera state seconds
	for _ in $(seq $(($3 * 10))); do
		[ "$(cam_field "$1" state)" = "$2" ] && return 0
		sleep 0.1
	done
	return 1
}

echo "=== rcd ==="
if wait_state 1 streaming 10; then pass "camera1 streaming"; else fail "camera1 streaming"; cat "$WORK/rcd.log"; fi
[ "$(cam_field 1 fw)" = "2019-12-12-Ver1.00" ] && pass "firmware version read" || fail "firmware version read"

sleep 2
HDR=$("$OUT/ringdump" s1_main 2>&1)
echo "$HDR" | grep -q "Size:      320x240" && pass "s1_main stream info 320x240" || fail "s1_main stream info"
echo "$HDR" | grep -q "Codec:     H.264\|Codec:     h264\|(0)" && pass "s1_main codec h264" || fail "s1_main codec"
if echo "$HDR" | grep -q "drop_source=0 drop_publish=0"; then
	pass "s1_main no lost frames"
else
	fail "s1_main lost frames: $(echo "$HDR" | grep 'Src seq')"
fi

# from the newest frame, after a keyframe request: the ring's oldest frames
# are mid-GOP and carry no parameter sets
decode() { # ring codec frames fields
	"$OUT/ringdump" "$1" -i > /dev/null 2>&1
	"$OUT/ringdump" "$1" -d -s -n "$3" 2> /dev/null | ffprobe -v quiet -f "$2" -count_frames \
		-select_streams v:0 -show_entries "stream=$4" -of csv=p=0 -
}
N=$(decode s1_main h264 90 codec_name,width,height,nb_read_frames)
case "$N" in
h264,320,240,*) pass "s1_main decodes ($N)" ;;
*) fail "s1_main decodes: '$N'" ;;
esac

SUBF=$(cam_field 1 streams.1.frames)
[ "${SUBF:-0}" -gt 30 ] && pass "s1_sub frames ($SUBF)" || fail "s1_sub frames ($SUBF)"
[ "$(cam_field 1 streams.0.dropped)" = "0" ] && pass "no dropped frames" || fail "dropped frames"

K0=$(cam_field 1 streams.0.keyframes)
"$OUT/ringdump" s1_main -i > /dev/null 2>&1
sleep 1
K1=$(cam_field 1 streams.0.keyframes)
[ "$K1" -gt "$K0" ] && pass "keyframe on request ($K0 -> $K1)" || fail "keyframe on request ($K0 -> $K1)"

if [ $H265 = 1 ]; then
	if wait_state 2 streaming 10; then pass "camera2 streaming"; else fail "camera2 streaming"; fi
	N=$(decode s2_main hevc 60 codec_name,width,nb_read_frames)
	case "$N" in
	hevc,320,*) pass "s2_main decodes h265 ($N)" ;;
	*) fail "s2_main decodes h265: '$N'" ;;
	esac
else
	skip "camera2 h265 (no libx265)"
fi

S0=$(cam_field 1 sessions)
kill "$FAKE1"
if wait_state 1 error 6; then pass "camera1 loss detected"; else fail "camera1 loss detected"; fi
fake_start 127.0.0.12 || fail "fake restart"
if wait_state 1 streaming 10 && [ "$(cam_field 1 sessions)" -gt "$S0" ]; then
	pass "camera1 reconnects"
else
	fail "camera1 reconnects"
fi

# the loss counters must fire: a module that skips and corrupts frames
kill "$FAKE_PID"
fake_start 127.0.0.12 --skip 10 --corrupt 7 || fail "faulty fake start"
wait_state 1 streaming 10 || fail "streaming from the faulty module"
sleep 3
LOST=$("$OUT/ringdump" s1_main 2>&1 | sed -n 's/.*drop_source=\([0-9]*\).*/\1/p')
[ "${LOST:-0}" -gt 0 ] && pass "skipped frames counted as lost ($LOST)" || fail "skipped frames not counted"
DROP=$(cam_field 1 streams.0.dropped)
[ "${DROP:-0}" -gt 0 ] && pass "bad checksums dropped ($DROP)" || fail "bad checksums not dropped"
kill "$FAKE_PID"
fake_start 127.0.0.12 || fail "fake restart"
wait_state 1 streaming 10 || fail "streaming again"

S1=$(cam_field 1 sessions)
ctl reconnect 1 | grep -q '"ok"' && pass "reconnect command" || fail "reconnect command"
sleep 3
[ "$(cam_field 1 sessions)" -gt "$S1" ] && pass "reconnect opens a new session" || fail "reconnect opens a new session"

kill "$RCD_PID"
wait "$RCD_PID" 2> /dev/null
RC=$?
[ $RC -eq 0 ] && pass "rcd exits cleanly" || fail "rcd exit status $RC"
if grep -q "ERROR: AddressSanitizer\|runtime error:\|LeakSanitizer\|WARNING: ThreadSanitizer" "$WORK/rcd.log"; then
	fail "sanitizer report"
	grep -A20 "Sanitizer\|runtime error" "$WORK/rcd.log" | head -40
else
	pass "no sanitizer reports"
fi

echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ $FAIL -eq 0 ]
