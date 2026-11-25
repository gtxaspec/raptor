#!/bin/bash
#
# soak-dashcam.sh -- a dashcam on the bench, driven like one in a car
#
# Runs on a host against a camera reachable as root over ssh (key auth).
# rtd simulates a drive ([telematics] sim: cold start, route, fix drops,
# an impact every SIM_IMPACT_S so rmr records clips), and a logged-in web
# UI session loads the camera the way a browser does: one heartbeat stream
# per tab, the GPS page's poll, and a page load's burst of requests every
# minute. Every 30 s it samples the camera; it fails on an OOM kill, a
# recording daemon restarting, a lost frame, a card drop or ssh slower
# than SSH_MAX_MS. The camera's raptor.conf is restored at the end.
#
# usage: RAPTOR_SOAK_PASSWORD=<web UI password> tests/soak-dashcam.sh <host> [minutes]
# env:   TABS (1), SIM_IMPACT_S (120), SSH_MAX_MS (1500), NO_WEBUI=1

set -u
HOST=${1:?usage: $0 <host> [minutes]}
MINUTES=${2:-30}
TABS=${TABS:-1}
SIM_IMPACT_S=${SIM_IMPACT_S:-120}
SSH_MAX_MS=${SSH_MAX_MS:-1500}
CONF=/etc/raptor.conf
BAK=/etc/raptor.conf.soak-bak
WATCHED="rvd rcd rmr rtd"
WORK=$(mktemp -d /tmp/soak-dashcam.XXXXXX)
BG=()
FAIL=0

say() { echo "$(date +%T) $*"; }
fail() { say "FAIL  $*"; FAIL=$((FAIL + 1)); }
cam() { ssh -o BatchMode=yes -o ConnectTimeout=20 "root@$HOST" "$@"; }

cleanup() {
	for p in "${BG[@]}"; do kill "$p" 2>/dev/null; done
	say "restoring $CONF"
	cam "[ -f $BAK ] && mv $BAK $CONF && kill \$(pidof rtd) 2>/dev/null" || say "WARN: restore failed, $BAK left on the camera"
	rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

pids() { cam "for d in $WATCHED; do printf '%s=%s ' \$d \"\$(pidof \$d)\"; done"; }
counters() {
	cam 'printf "oom=%s " $(dmesg | grep -c "invoked oom-killer")
		raptorctl rmr status | tr "{,}" "\n\n\n" | tr -d "\"" | awk -F: "\$1 ~ /^(frames_lost|card_drops)\$/ {s[\$1] += \$2} \$1 == \"io_peak_kb\" {p = \$2} END {printf \"frames_lost=%d card_drops=%d io_peak_kb=%d \", s[\"frames_lost\"], s[\"card_drops\"], p}"
		printf "avail_kb=%s " $(sed -n "s/^MemAvailable: *\([0-9]*\).*/\1/p" /proc/meminfo)
		printf "rootfs_kb=%s " $(awk "\$3 ~ /^ubiblock|^mtdblock/ {s += \$6} END {print int(s / 2)}" /proc/diskstats)
		raptorctl rtd status | grep -oE "\"(sim_impacts|fix|sats)\":[a-z0-9]+" | tr -d "\"" | tr ":\n" "= "'
}
field() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }

# ── Simulated drive ──
say "camera $HOST, $MINUTES min, $TABS tab(s), an impact every $SIM_IMPACT_S s"
cam "[ -f $BAK ] || cp $CONF $BAK" || { echo "cannot reach $HOST"; exit 2; }
cam "sed -i '/^sim/d' $CONF
	grep -q '^\[telematics\]' $CONF || printf '\n[telematics]\n' >>$CONF
	for kv in 'sim_impact_every_s = $SIM_IMPACT_S' 'sim_fix_after_s = 30' 'sim = true'; do
		sed -i \"/^\\[telematics\\]/a \$kv\" $CONF
	done
	kill \$(pidof rtd)"
for _ in $(seq 20); do
	sleep 2
	cam 'raptorctl rtd status 2>/dev/null | grep -q "\"sim\":true"' && break
done
cam 'raptorctl rtd status | grep -q "\"sim\":true"' || { fail "rtd did not start simulating"; exit 1; }
say "rtd simulating"

# ── Web UI load ──
if [ -z "${NO_WEBUI:-}" ]; then
	: "${RAPTOR_SOAK_PASSWORD:?set RAPTOR_SOAK_PASSWORD, or NO_WEBUI=1}"
	JAR=$WORK/cookies
	pw=$(printf '%s' "$RAPTOR_SOAK_PASSWORD" | base64)
	curl -s -c "$JAR" -H 'Content-Type: application/json' \
		-d "{\"username\":\"root\",\"password\":\"$pw\",\"encoding\":\"base64\"}" \
		"http://$HOST/x/login.cgi" | grep -q '"success":true' || { fail "web UI login"; exit 1; }
	web() { curl -s -b "$JAR" -o /dev/null --max-time 30 -w '%{time_total}' "http://$HOST$1"; }
	for _ in $(seq "$TABS"); do
		curl -s -N -b "$JAR" "http://$HOST/x/json-heartbeat.cgi" >/dev/null &
		BG+=($!)
		(while :; do web /x/json-telematics.cgi >>"$WORK/gps.t"; echo >>"$WORK/gps.t"; sleep 2; done) &
		BG+=($!)
	done
	(while :; do
		for u in /info-gps.html /x/session-status.cgi /x/json-config-webui.cgi /x/json-heartbeat-slow.cgi \
			'/x/agent.cgi?agent_path=%2Fapi%2Fv1%2Fsettings%2Ftelematics%2Fspeed-unit'; do
			web "$u" >/dev/null &
		done
		wait
		sleep 60
	done) &
	BG+=($!)
	say "web UI load running"
fi

# ── Watch ──
p0=$(pids)
c0=$(counters)
say "start: $p0"
say "start: $c0"
end=$(($(date +%s) + MINUTES * 60))
worst_ssh=0
while [ "$(date +%s)" -lt "$end" ]; do
	sleep 30
	t=$(date +%s%N)
	cam true || { fail "ssh unreachable"; continue; }
	ms=$((($(date +%s%N) - t) / 1000000))
	[ "$ms" -gt "$worst_ssh" ] && worst_ssh=$ms
	c=$(counters)
	p=$(pids)
	say "ssh ${ms} ms  $c"
	[ "$ms" -gt "$SSH_MAX_MS" ] && fail "ssh took $ms ms"
	if [ "$p" != "$p0" ]; then
		fail "a daemon restarted: $p0 -> $p"
		p0=$p
	fi
done

# ── Verdict ──
c=$(counters)
for k in oom frames_lost card_drops; do
	a=$(field "$k" "$c0")
	b=$(field "$k" "$c")
	[ "${b:-0}" -gt "${a:-0}" ] && fail "$k went from ${a:-0} to $b"
done
impacts=$(field sim_impacts "$c")
[ "${impacts:-0}" -gt 0 ] || [ $((MINUTES * 60)) -lt "$SIM_IMPACT_S" ] || fail "no simulated impact"
clips=$(cam "logread | grep -c 'motion clip started'")
say "impacts ${impacts:-0}, clips started (log) $clips, worst ssh $worst_ssh ms, rootfs read $(($(field rootfs_kb "$c") - $(field rootfs_kb "$c0"))) KB"
if [ -s "$WORK/gps.t" ]; then
	say "GPS poll: $(sort -n "$WORK/gps.t" | awk 'NF {a[n++] = $1} END {if (n) printf "median %.2f s, worst %.2f s over %d", a[int(n / 2)], a[n - 1], n}')"
fi
[ "$FAIL" -eq 0 ] && say "PASS" || say "FAILED: $FAIL"
[ "$FAIL" -eq 0 ]
