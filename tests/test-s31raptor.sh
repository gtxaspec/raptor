#!/bin/sh
# S31raptor hook behavior (/etc/raptor.d) against a stand-in hook dir.
#
# S31raptor runs here with stub daemons and stub start-stop-daemon, pidof and
# usleep, its directories pointed into a scratch tree. Hooks must be owned by
# root, so the whole test runs in a user namespace where our files are root's.
#
# Usage: ./tests/test-s31raptor.sh

set -e

RAPTOR_DIR="$(cd "$(dirname "$0")/.." && pwd)"

if [ -z "$S31_TEST_INNER" ]; then
	if ! unshare -Ur true 2>/dev/null; then
		echo "SKIP: unshare -Ur unavailable (no unprivileged userns?)"
		exit 0
	fi
	S31_TEST_INNER=1 exec unshare -Ur sh "$0" "$@"
fi

WORK="${TMPDIR:-/tmp}/s31-suite.$$"
mkdir -p "$WORK/bin" "$WORK/run" "$WORK/shm" "$WORK/hooks"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
ok() {
	PASS=$((PASS + 1))
	echo "  ok   $1"
}
bad() {
	FAIL=$((FAIL + 1))
	echo "  FAIL $1"
}
check() { # check <name> <command...>
	name=$1
	shift
	if "$@"; then ok "$name"; else bad "$name"; fi
}

TRACE="$WORK/trace"

sed -e "s|^RUNDIR=.*|RUNDIR=$WORK/run|" \
	-e "s|^SHMDIR=.*|SHMDIR=$WORK/shm|" \
	-e "s|^HOOKDIR=.*|HOOKDIR=$WORK/hooks|" \
	"$RAPTOR_DIR/config/S31raptor" >"$WORK/S31raptor"
chmod 755 "$WORK/S31raptor"

cat >"$WORK/bin/start-stop-daemon" <<EOF
#!/bin/sh
echo "ssd \$*" >>"$TRACE"
case "\$*" in
	*/usr/bin/rvd*) : >"$WORK/shm/rss_ring_main" ;;
esac
exit 0
EOF
cat >"$WORK/bin/pidof" <<EOF
#!/bin/sh
echo "pidof \$*" >>"$TRACE"
exit 1
EOF
printf '#!/bin/sh\nexit 0\n' >"$WORK/bin/usleep"
printf '#!/bin/sh\nexit 0\n' >"$WORK/bin/logger"
printf '#!/bin/sh\nexit 1\n' >"$WORK/bin/raptorctl"
chmod 755 "$WORK"/bin/*
PATH="$WORK/bin:$PATH"
export PATH

hook() { # hook <name> <body>
	printf '#!/bin/sh\n%s\n' "$2" >"$WORK/hooks/$1"
	chmod 755 "$WORK/hooks/$1"
}
reset() {
	rm -f "$WORK/hooks"/* "$TRACE" "$WORK/out" "$WORK/shm"/*
	chmod 755 "$WORK/hooks"
	: >"$TRACE"
}
s31() { # s31 <verb> [timeout]; output in $WORK/out
	RAPTOR_HOOK_TIMEOUT=${2:-10} sh "$WORK/S31raptor" "$1" >"$WORK/out" 2>&1
}
line_of() { grep -n "$1" "$TRACE" | head -n 1 | cut -d: -f1; }

echo "=== S31raptor hooks ==="

reset
hook 10-first "echo \"hook first \$1\" >>$TRACE"
hook 20-second "echo \"hook second \$1\" >>$TRACE"
s31 restart
check "hooks run with stop, then start" \
	test "$(grep '^hook' "$TRACE" | tr '\n' ',')" = \
	"hook first stop,hook second stop,hook first start,hook second start,"
check "stop hooks run before the first daemon stop" \
	test "$(line_of 'hook second stop')" -lt "$(line_of 'pidof rsr')"
check "start hooks run after the stack is up" \
	test "$(line_of 'hook first start')" -gt "$(line_of 'ssd.*watch')"

reset
hook 10-plain "echo ran >>$TRACE"
chmod 644 "$WORK/hooks/10-plain"
hook 20-gw "echo gw >>$TRACE"
chmod 775 "$WORK/hooks/20-gw"
hook 30-ow "echo ow >>$TRACE"
chmod 757 "$WORK/hooks/30-ow"
ln -s /usr/bin/true "$WORK/hooks/40-foreign"
s31 stop
check "a non-executable, group-writable or world-writable hook is skipped" \
	test -z "$(grep -E '^(ran|gw|ow)$' "$TRACE")"
check "a skipped hook is reported" grep -q '20-gw' "$WORK/out"
check "a hook owned by someone else is reported and skipped" grep -q '40-foreign' "$WORK/out"

reset
hook 10-ok "echo ran >>$TRACE"
chmod 775 "$WORK/hooks"
s31 stop
check "no hook runs from a group-writable hook directory" test -z "$(grep '^ran$' "$TRACE")"
check "the skipped directory is reported" grep -q 'hooks' "$WORK/out"

reset
hook 10-hang "sleep 60"
hook 20-after "echo after >>$TRACE"
t0=$(date +%s)
s31 stop 1
t1=$(date +%s)
check "a hung hook is killed at the timeout" test $((t1 - t0)) -lt 10
check "the hook after a hung one still runs" grep -q '^after$' "$TRACE"
check "the timeout is reported" grep -q '10-hang' "$WORK/out"

reset
hook 10-reenter "sh $WORK/S31raptor stop >/dev/null 2>&1; echo \$? >$WORK/reenter.rc"
s31 stop
check "a hook calling S31raptor is refused, not nested" test "$(cat "$WORK/reenter.rc")" = 1

reset
rmdir "$WORK/hooks"
s31 restart
check "a missing hook directory is no error" grep -q 'Done' "$WORK/out"
mkdir "$WORK/hooks"

echo
echo "$PASS checks passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
