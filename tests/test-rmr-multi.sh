#!/bin/bash
#
# test-rmr-multi.sh -- rmr recording a front and a rear camera together
#
# The mock rvd publishes "main" (front); rcd with a fake EthCam module
# (tests/fake_ethcam.py) publishes "s1_main" (rear). rmr records both
# with [recording] streams = main, s1_main:R in mode both, signed, with
# SEI timecodes, GPS records from rtd's NMEA replay and audio. Checks:
# segment and clip files pair (X.mp4 + X_R.mp4) and verify, no frame is
# lost on either track, the front keeps recording while the rear camera
# is gone and the rear resumes when it returns, and a legacy one-stream
# config keeps its file names and status fields.
#
# Requires: asan-out (./build-asan.sh), python3, ffmpeg with libx264.

set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RAPTOR_DIR="$(dirname "$SCRIPT_DIR")"
OUT="$RAPTOR_DIR/asan-out"
WORK=$(mktemp -d /tmp/test-rmr-multi.XXXXXX)
PASS=0
FAIL=0
PIDS=()
FAKE_PID=
RMR_PID=

pass() { echo "  PASS  $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL  $1"; FAIL=$((FAIL + 1)); }

cleanup() {
	for p in "${PIDS[@]}"; do
		kill "$p" 2> /dev/null
	done
	for _ in $(seq 30); do
		alive=0
		for p in "${PIDS[@]}"; do
			kill -0 "$p" 2> /dev/null && alive=1
		done
		[ $alive = 0 ] && break
		sleep 0.1
	done
	for p in "${PIDS[@]}"; do
		kill -9 "$p" 2> /dev/null
	done
	wait 2> /dev/null
	rm -rf "${WORK:?}"
}
trap cleanup EXIT

for t in python3 ffmpeg; do
	if ! command -v "$t" > /dev/null; then
		echo "test-rmr-multi: $t not installed, skipping"
		exit 0
	fi
done
for b in create_rings rvd rcd rtd rmr raptorctl rverify; do
	if [ ! -x "$OUT/$b" ]; then
		echo "test-rmr-multi: run ./build-asan.sh first"
		exit 1
	fi
done

ffmpeg -v error -f lavfi -i testsrc=size=320x240:rate=30 -frames:v 240 -c:v libx264 \
	-bf 0 -x264-params keyint=30:scenecut=0 -f h264 "$WORK/clip.h264" || exit 1

mkdir -p /var/run/rss 2> /dev/null || { sudo mkdir -p /var/run/rss && sudo chmod 1777 /var/run/rss; }
for d in rvd rcd rtd rmr create_rings; do
	pkill -f "asan-out/$d" 2> /dev/null
done
sleep 0.5
rm -f /var/run/rss/*.pid /var/run/rss/*.sock /dev/shm/rss_ring_* /dev/shm/rss_osd_* 2> /dev/null

python3 - "$WORK/drive.nmea" << 'NMEA_EOF'
import sys
from functools import reduce
def s(body):
    return "$%s*%02X\n" % (body, reduce(lambda a, c: a ^ ord(c), body, 0))
with open(sys.argv[1], "w") as f:
    for i in range(60):
        t = "1200%02d.000" % i
        lat = "3403.%04d" % (1000 + i * 3)
        lon = "11814.%04d" % (6000 - i * 3)
        f.write(s("GNGGA,%s,%s,N,%s,W,1,09,0.9,80.0,M,-33.0,M,," % (t, lat, lon)))
        f.write(s("GNRMC,%s,A,%s,N,%s,W,19.44,45.0,181126,,,A" % (t, lat, lon)))
NMEA_EOF

base_conf() { # storage_dir
	cat << EOF
[sensor]
model = gc2053
name = gc2053
i2c_addr = 0x37

[stream0]
width = 1920
height = 1080
fps = 25
bitrate = 2000000
codec = h264
gop = 25

[stream1]
width = 640
height = 360
fps = 25
bitrate = 500000
codec = h264

[audio]
enabled = true
sample_rate = 16000
codec = l16

[osd]
enabled = false

[ircut]
enabled = false

[motion]
enabled = false

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
sub = false

[telematics]
gps_replay = $WORK/drive.nmea
set_clock = false

[recording]
enabled = true
audio = true
storage_path = $1
segment_seconds = 10
sei_timecode = true
sign = true
sign_key = $WORK/sign.key
prebuffer_sec = 2
clip_length_sec = 60
EOF
}

base_conf "$WORK/rec" > "$WORK/multi.conf"
printf 'mode = both\nstreams = main, s1_main:R\n\n[log]\nlevel = debug\n' >> "$WORK/multi.conf"
base_conf "$WORK/legacy" > "$WORK/legacy.conf"
printf 'mode = continuous\nstream = 0\n\n[log]\nlevel = debug\n' >> "$WORK/legacy.conf"
mkdir -p "$WORK/rec" "$WORK/legacy"

start() { # name args...
	local name=$1
	shift
	"$@" > "$WORK/$name.log" 2>&1 &
	PIDS+=($!)
	LAST_PID=$!
}

fake_start() {
	python3 "$SCRIPT_DIR/fake_ethcam.py" --ip 127.0.0.12 --h264 "$WORK/clip.h264" \
		>> "$WORK/fake.log" 2>&1 &
	PIDS+=($!)
	FAKE_PID=$!
	for _ in $(seq 50); do
		grep -q "fake ethcam" "$WORK/fake.log" && return 0
		sleep 0.1
	done
	return 1
}

status() { "$OUT/raptorctl" rmr status 2> /dev/null; }
# st_get <python expression over d>
st_get() {
	status | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
    print($1)
except Exception:
    print('')" 2> /dev/null
}
wait_for() { # seconds python-condition
	for _ in $(seq $(($1 * 5))); do
		[ "$(st_get "$2")" = "True" ] && return 0
		sleep 0.2
	done
	return 1
}

# Closed pairs: base names with both X.mp4 and X_R.mp4, neither open now
closed_pairs() { # dir
	local open
	open=$(st_get "' '.join(t['file'] for t in d['tracks']) + ' ' + ' '.join(t['clip'] for t in d['tracks'])")
	python3 - "$1" "$open" << 'PY_EOF'
import os, sys
root, open_files = sys.argv[1], set(sys.argv[2].split())
files = set()
for dp, _, names in os.walk(root):
    for n in names:
        if n.endswith(".mp4"):
            files.add(os.path.join(dp, n))
# names sort by time: anything at or past an open file of the same
# directory may still be written (a track can rotate before the other)
def closed(f):
    d = os.path.dirname(f)
    return all(f < o for o in open_files if os.path.dirname(o) == d)
for f in sorted(files):
    if f.endswith("_R.mp4"):
        continue
    r = f[:-4] + "_R.mp4"
    if r in files and closed(f) and closed(r):
        print(f)
PY_EOF
}

verify() { # file -> "ok" or a reason
	local out
	out=$("$OUT/rverify" -t -g -k "$WORK/sign.key.pub" "$1" 2>&1)
	local rc=$?
	echo "$out" > "$1.rverify"
	if [ $rc -ne 0 ] || ! echo "$out" | grep -q "RESULT: VERIFIED"; then
		echo "rverify rc=$rc"
		return
	fi
	if echo "$out" | grep -q "not cleanly closed"; then
		echo "not cleanly closed"
		return
	fi
	local tc
	tc=$(echo "$out" | sed -n 's/^timecodes: \([0-9]*\) frames.*/\1/p')
	[ "${tc:-0}" -gt 0 ] || { echo "no timecodes"; return; }
	local gps
	gps=$(echo "$out" | sed -n 's/^gps records: \([0-9]*\).*/\1/p')
	[ "${gps:-0}" -gt 0 ] || { echo "no gps records"; return; }
	echo ok
}

echo "=== rmr multi-stream ==="
fake_start || { echo "fake module did not start"; exit 1; }
start rings "$OUT/create_rings" --skip-video
sleep 0.5
start rvd "$OUT/rvd" -c "$WORK/multi.conf" -f -d
start rcd "$OUT/rcd" -c "$WORK/multi.conf" -f -d
RCD_PID=$LAST_PID
start rtd "$OUT/rtd" -c "$WORK/multi.conf" -f -d
RTD_PID=$LAST_PID
sleep 1
start rmr "$OUT/rmr" -c "$WORK/multi.conf" -f -d
RMR_PID=$LAST_PID

if wait_for 15 "len(d['tracks']) == 2 and all(t['frames'] > 0 for t in d['tracks'])"; then
	pass "both tracks recording"
else
	fail "both tracks recording: $(status)"
fi
[ "$(st_get "d['tracks'][1]['tag']")" = "R" ] && pass "rear track tagged R" || fail "rear tag"
[ "$(st_get "d['file'] == d['tracks'][0]['file']")" = "True" ] &&
	pass "top-level status describes the first track" || fail "top-level file field"

PAIR=
for _ in $(seq 150); do
	PAIR=$(closed_pairs "$WORK/rec" | grep -v '/clips/' | head -1)
	[ -n "$PAIR" ] && break
	sleep 0.2
done
if [ -n "$PAIR" ]; then
	pass "segment pair $(basename "$PAIR") + $(basename "${PAIR%.mp4}")_R.mp4"
	R=$(verify "$PAIR")
	[ "$R" = ok ] && pass "front segment verifies (signed, timecodes, gps)" || fail "front segment: $R"
	R=$(verify "${PAIR%.mp4}_R.mp4")
	[ "$R" = ok ] && pass "rear segment verifies (signed, timecodes, gps)" || fail "rear segment: $R"
	if command -v ffprobe > /dev/null; then
		for f in "$PAIR" "${PAIR%.mp4}_R.mp4"; do
			S=$(ffprobe -v quiet -show_entries stream=codec_type -of csv=p=0 "$f" | sort | tr '\n' ' ')
			[ "$S" = "audio video " ] && pass "$(basename "$f") has audio and video" ||
				fail "$(basename "$f") streams: '$S'"
		done
	fi
else
	fail "no closed segment pair: $(find "$WORK/rec" -name '*.mp4' | sort | tr '\n' ' ')"
fi
# A front file can lack a rear (the rear starts later); a rear file
# always takes the name of the front file of its period.
orphans() { # dir [find args]
	local dir=$1
	shift
	for r in $(find "$dir" -path '*/clips' -prune -o -name '*_R.mp4' "$@" -print); do
		[ -e "${r%_R.mp4}.mp4" ] || printf '%s ' "$(basename "$r")"
	done
}
ORPH=$(orphans "$WORK/rec")
[ -z "$ORPH" ] && pass "every rear segment pairs a front one" || fail "rear orphans: $ORPH"

LOST=$(st_get "[t['frames_lost'] for t in d['tracks']]")
[ "$LOST" = "[0, 0]" ] && pass "no frames lost on either track" || fail "frames lost: $LOST"

echo "--- event clip ---"
ev() { "$OUT/raptorctl" -j "{\"daemon\":\"rmr\",\"cmd\":\"$1\"}" > /dev/null 2>&1; }
ev start
if wait_for 5 "all(t['clip'] for t in d['tracks'])"; then
	pass "start opens a clip on both tracks"
else
	fail "clips open: $(status)"
fi
sleep 3
ev stop
wait_for 5 "not any(t['clip'] for t in d['tracks'])" || fail "clips still open after stop"
CLIP=$(closed_pairs "$WORK/rec/clips" | head -1)
if [ -n "$CLIP" ]; then
	pass "clip pair $(basename "$CLIP") + $(basename "${CLIP%.mp4}")_R.mp4"
	R=$(verify "$CLIP")
	[ "$R" = ok ] && pass "front clip verifies" || fail "front clip: $R"
	R=$(verify "${CLIP%.mp4}_R.mp4")
	[ "$R" = ok ] && pass "rear clip verifies" || fail "rear clip: $R"
else
	fail "no clip pair: $(find "$WORK/rec/clips" -name '*.mp4' 2> /dev/null | tr '\n' ' ')"
fi

echo "--- simulated drive: an impact makes a clip ---"
kill "$RTD_PID"
wait "$RTD_PID" 2> /dev/null
sed 's|^gps_replay = .*|sim = true\nsim_fix_after_s = 1\nsim_impact_every_s = 8\nimpact_clip_sec = 3|' \
	"$WORK/multi.conf" > "$WORK/sim.conf"
touch "$WORK/.sim"
start rtd-sim "$OUT/rtd" -c "$WORK/sim.conf" -f -d
RTD_PID=$LAST_PID
if wait_for 15 "all(t['clip'] for t in d['tracks'])"; then
	pass "a simulated impact opens a clip on both tracks"
else
	fail "no clip from a simulated impact: $(status)"
fi
wait_for 10 "not any(t['clip'] for t in d['tracks'])" || fail "impact clip still open"
kill "$RTD_PID"
wait "$RTD_PID" 2> /dev/null
start rtd "$OUT/rtd" -c "$WORK/multi.conf" -f -d
RTD_PID=$LAST_PID
grep -q "simulated drive" "$WORK/rtd-sim.log" && grep -q "impact: " "$WORK/rtd-sim.log" &&
	pass "rtd logs the simulated impact" || fail "rtd-sim log: $(tail -3 "$WORK/rtd-sim.log")"
SIMCLIP=$(closed_pairs "$WORK/rec/clips" | while read -r f; do [ "$f" -nt "$WORK/.sim" ] && echo "$f"; done | head -1)
if [ -n "$SIMCLIP" ]; then
	R=$(verify "$SIMCLIP")
	[ "$R" = ok ] && pass "impact clip verifies" || fail "impact clip: $R"
	grep -qE "^ +[0-9-]+ [0-9:]+ +51\.47[0-9]+ +-0\.00" "$SIMCLIP.rverify" &&
		pass "impact clip carries the simulated route" || fail "no simulated fix in $(basename "$SIMCLIP")"
else
	fail "no impact clip pair"
fi

echo "--- rear camera unplugged ---"
kill "$FAKE_PID"
wait "$FAKE_PID" 2> /dev/null
if wait_for 10 "not d['tracks'][1]['active']"; then
	pass "rear track goes idle"
else
	fail "rear track still active: $(status)"
fi
F0=$(st_get "d['tracks'][0]['file'] + ' ' + str(d['tracks'][0]['frames'])")
D0=$(st_get "d['tracks'][0]['dropped']")
sleep 4
F1=$(st_get "d['tracks'][0]['file'] + ' ' + str(d['tracks'][0]['frames'])")
if [ "${F0% *}" != "${F1% *}" ] || [ "${F1##* }" -gt "${F0##* }" ]; then
	pass "front keeps recording without the rear ($F0 -> $F1)"
else
	fail "front stalled: $F0 -> $F1"
fi
[ "$(st_get "d['tracks'][0]['frames_lost']")" = "0" ] && [ "$(st_get "d['tracks'][0]['dropped']")" = "$D0" ] &&
	pass "front loses nothing while the rear is gone" || fail "front lost frames: $(status)"

fake_start || fail "fake restart"
touch "$WORK/.rear-back"
sleep 0.1
if wait_for 20 "d['tracks'][1]['active'] and d['tracks'][1]['frames'] > 0 and d['tracks'][1]['file'] != ''"; then
	pass "rear track resumes"
else
	fail "rear track resumes: $(status)"
fi
NEWR=
for _ in $(seq 100); do
	NEWR=$(find "$WORK/rec" -path '*/clips' -prune -o -name '*_R.mp4' -newer "$WORK/.rear-back" -print | head -1)
	[ -n "$NEWR" ] && break
	sleep 0.2
done
[ -n "$NEWR" ] && pass "rear writes again ($(basename "$NEWR"))" || fail "no rear file after the camera returned"
LOST=$(st_get "[t['frames_lost'] for t in d['tracks']]")
[ "$LOST" = "[0, 0]" ] && pass "no frames lost across the rear outage" || fail "frames lost: $LOST"
PAIR2=
for _ in $(seq 150); do
	for f in $(closed_pairs "$WORK/rec" | grep -v '/clips/'); do
		[ "${f%.mp4}_R.mp4" -nt "$WORK/.rear-back" ] && PAIR2=$f && break
	done
	[ -n "$PAIR2" ] && break
	sleep 0.2
done
if [ -n "$PAIR2" ]; then
	R=$(verify "${PAIR2%.mp4}_R.mp4")
	[ "$R" = ok ] && pass "segments pair again after the outage ($(basename "$PAIR2"))" ||
		fail "rear segment after the outage: $R"
else
	fail "no segment pair after the outage"
fi

kill "$RMR_PID"
wait "$RMR_PID" 2> /dev/null
RC=$?
[ $RC -eq 0 ] && pass "rmr exits cleanly" || fail "rmr exit status $RC"
BAD=
for f in $(find "$WORK/rec" -name '*.mp4' -newer "$WORK/.rear-back"); do
	R=$(verify "$f")
	case "$R" in ok | "no gps records" | "no timecodes") ;; *) BAD="$BAD $(basename "$f"):$R" ;; esac
done
[ -z "$BAD" ] && pass "files closed at shutdown verify" || fail "shutdown files:$BAD"

echo "--- legacy single stream ---"
start rmr-legacy "$OUT/rmr" -c "$WORK/legacy.conf" -f -d
RMR_PID=$LAST_PID
wait_for 10 "d['frames'] > 0" || fail "legacy rmr not recording: $(status)"
KEYS=$(st_get "','.join(sorted(k for k in d if k != 'tracks'))")
WANT="bytes,clip,dropped,file,frames,frames_lost,gps,io_peak_kb,io_pending_kb,mode,recording,sei_timecode,sign,storage_path,timelapse"
[ "$KEYS" = "$WANT" ] && pass "legacy status fields unchanged" || fail "status fields: $KEYS"
[ "$(st_get "len(d['tracks']) == 1 and d['tracks'][0]['ring'] == 'main' and d['tracks'][0]['tag'] == ''")" = "True" ] &&
	pass "legacy config records one untagged main track" || fail "legacy tracks: $(status)"
sleep 1
kill "$RMR_PID"
wait "$RMR_PID" 2> /dev/null
NAMES=$(find "$WORK/legacy" -name '*.mp4' -printf '%f\n')
if [ -n "$NAMES" ] && ! echo "$NAMES" | grep -qvE '^[0-9]{2}-[0-9]{2}-[0-9]{2}\.mp4$'; then
	pass "legacy file names HH-MM-SS.mp4 ($(echo "$NAMES" | tr '\n' ' '))"
else
	fail "legacy file names: $(echo "$NAMES" | tr '\n' ' ')"
fi

echo "--- rear stream info published after rmr attached ---"
kill "$FAKE_PID" "$RCD_PID" 2> /dev/null
wait "$FAKE_PID" "$RCD_PID" 2> /dev/null
rm -f /dev/shm/rss_ring_s1_* "$WORK"/fake.log
ffmpeg -v error -f lavfi -i testsrc=size=320x240:rate=30 -frames:v 240 -c:v libx265 \
	-x265-params keyint=30:scenecut=0:bframes=0:log-level=error -f hevc "$WORK/clip.hevc" || exit 1
python3 "$SCRIPT_DIR/fake_ethcam.py" --ip 127.0.0.12 --h264 "$WORK/clip.h264" --h265 "$WORK/clip.hevc" --delay 2 \
	>> "$WORK/fake.log" 2>&1 &
PIDS+=($!)
for _ in $(seq 50); do
	grep -q "fake ethcam" "$WORK/fake.log" && break
	sleep 0.1
done
mkdir -p "$WORK/late"
# H.265: an unset header reads as codec 0 (H.264), which never matches
base_conf "$WORK/late" | sed '/^\[camera1\]/,/^\[/ s/^codec = h264$/codec = h265/' > "$WORK/late.conf"
printf 'mode = continuous\nstreams = main, s1_main:R\n\n[log]\nlevel = debug\n' >> "$WORK/late.conf"
start rcd-late "$OUT/rcd" -c "$WORK/late.conf" -f -d
for _ in $(seq 50); do
	[ -e /dev/shm/rss_ring_s1_main ] && break
	sleep 0.1
done
start rmr-late "$OUT/rmr" -c "$WORK/late.conf" -f -d
RMR_PID=$LAST_PID
if wait_for 15 "len(d['tracks']) == 2 and all(t['frames'] > 0 for t in d['tracks'])"; then
	pass "rear track records once its stream info arrives"
else
	fail "rear track after late stream info: $(status)"
fi
kill "$RMR_PID"
wait "$RMR_PID" 2> /dev/null
R=$(find "$WORK/late" -name '*_R.mp4' | head -1)
V=$([ -n "$R" ] && verify "$R")
case "$V" in
ok | "no gps records") pass "late rear file verifies" ;;
*) fail "late rear file: ${R:-none} $V" ;;
esac
[ -n "$R" ] && [ "$(ffprobe -v quiet -select_streams v -show_entries stream=codec_name,width -of csv=p=0 "$R")" = "hevc,320" ] &&
	pass "late rear file is H.265 320 wide" || fail "late rear codec: $( [ -n "$R" ] && ffprobe -v quiet -select_streams v -show_entries stream=codec_name,width -of csv=p=0 "$R")"

echo "--- no writable storage: standby without stall warnings ---"
base_conf /proc/rmr-no-such-dir > "$WORK/standby.conf"
printf 'mode = continuous\nstream = 0\n\n[log]\nlevel = debug\n' >> "$WORK/standby.conf"
start rmr-standby "$OUT/rmr" -c "$WORK/standby.conf" -f -d
RMR_PID=$LAST_PID
sleep 4
kill "$RMR_PID"
wait "$RMR_PID" 2> /dev/null
if grep -q "standing by" "$WORK/rmr-standby.log" && ! grep -q "main loop stalled" "$WORK/rmr-standby.log"; then
	pass "standby sleeps without stall warnings"
else
	fail "standby: $(grep -c 'main loop stalled' "$WORK/rmr-standby.log") stall warnings"
fi

for l in "$WORK"/rmr*.log; do
	if grep -q "ERROR: AddressSanitizer\|runtime error:\|LeakSanitizer\|WARNING: ThreadSanitizer" "$l"; then
		fail "sanitizer report in $(basename "$l")"
		grep -A20 "Sanitizer\|runtime error" "$l" | head -40
	fi
done
grep -q "Sanitizer\|runtime error:" "$WORK"/rmr*.log || pass "no sanitizer reports"

if [ $FAIL -ne 0 ] && [ -n "${RMR_MULTI_KEEP:-}" ]; then
	cp -r "$WORK" "$RMR_MULTI_KEEP"
	echo "kept logs in $RMR_MULTI_KEEP"
fi
echo "=== Results: $PASS passed, $FAIL failed ==="
[ $FAIL -eq 0 ]
