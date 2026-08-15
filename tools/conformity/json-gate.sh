#!/bin/bash
# json-gate: no hand-written JSON in production C.
#
# Structured formats are built by serializers, never string assembly:
# a "%s" into a JSON literal is one edit away from injection, and the
# safety of today's literal requires provenance reasoning no reviewer
# should have to repeat. Two documented exemptions: raptorctl_help.c
# prints example -j syntax (display text, not construction), and
# raptor-ipc's transport error frame in rss_ctrl.c (a dependency-free
# layer emitting a constant shape with one integer).
#
# Single source of truth: test-all.sh stage 0 and the conformity git
# hooks both call this script, so the rule cannot drift between them.
#
# Usage:
#   json-gate.sh --tree <dir>...    scan whole trees (CI / suite mode)
#   json-gate.sh --files <file>...  scan specific files (hook mode)
#
# Prints violations and exits 1 when any exist; silent exit 0 when clean.
set -u

MODE=${1:---tree}
shift || true
[ $# -ge 1 ] || exit 0

EXEMPT='raptorctl_help\.c|rss_ctrl\.c'
EXCLUDE_DIRS='/tests/|/fuzz/|/\.deps/|/build/|/asan-out/|/third_party/'

case "$MODE" in
--tree)
    HITS=$(/usr/bin/grep -rn '{\\"' --include='*.c' --include='*.h' "$@" 2>/dev/null |
        grep -vE "$EXCLUDE_DIRS" | grep -vE "$EXEMPT" || true)
    ;;
--files)
    FILES=$(printf '%s\n' "$@" | grep -E '\.(c|h)$' |
        grep -vE "$EXCLUDE_DIRS" | grep -vE "^(tests|fuzz)/" | grep -vE "$EXEMPT" || true)
    [ -n "$FILES" ] || exit 0
    # shellcheck disable=SC2086
    HITS=$(/usr/bin/grep -n '{\\"' $FILES 2>/dev/null || true)
    ;;
*)
    echo "json-gate: unknown mode $MODE" >&2
    exit 2
    ;;
esac

if [ -n "$HITS" ]; then
    echo "$HITS"
    echo "Build JSON with cJSON (rss_ctrl_cmd*/rss_ctrl_resp_*), never by hand." >&2
    exit 1
fi
exit 0
