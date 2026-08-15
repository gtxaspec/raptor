#!/bin/bash
# Shared plumbing for the conformity hooks. Sourced, not executed.
#
# Policy lives in each repo as a tracked one-line `.conformity`
# manifest (first line = check tokens; later lines are free comment);
# this engine lives once, in raptor, so seven repos cannot drift.
# Hooks are a safety net for the committer, never the wall -- CI is
# the wall -- so every check here fails OPEN when its tooling is
# missing, and `git commit --no-verify` remains the escape hatch.

CONF_TOP=$(git rev-parse --show-toplevel 2>/dev/null) || exit 0
CONF_TOKENS=""
if [ -f "$CONF_TOP/.conformity" ]; then
    CONF_TOKENS=$(head -1 "$CONF_TOP/.conformity")
fi

conf_has() {
    case " $CONF_TOKENS " in *" $1 "*) return 0 ;; *) return 1 ;; esac
}

conf_fail() {
    echo "conformity: $1" >&2
    echo "(bypass once with --no-verify; the CI gate will still apply)" >&2
    exit 1
}

# Format checks are diff-scoped, same exclusions as every repo's CI.
CONF_FMT_EXCLUDES=(':(exclude)tests/greatest.h' ':(exclude)include/cJSON.h'
    ':(exclude)src/cJSON.c' ':(exclude)third_party')

conf_have_clang_format() {
    command -v clang-format-19 > /dev/null 2>&1 &&
        command -v git-clang-format-19 > /dev/null 2>&1
}

# Lab addresses must never leave this machine in a commit. Full-file
# scan of the files a commit touches: the tracked trees are clean, so
# any hit is new.
CONF_IP_PATTERN='10\.25\.[0-9]+\.[0-9]+|192\.168\.[0-9]+\.[0-9]+'

conf_ip_scan() { # <file>...
    [ $# -ge 1 ] || return 0
    local hits
    # shellcheck disable=SC2086
    hits=$(/usr/bin/grep -nE "$CONF_IP_PATTERN" "$@" 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits" >&2
        conf_fail "lab addresses in the change -- scrub before committing"
    fi
}

CONF_TRAILER_PATTERN='^(Signed-off-by|Co-[Aa]uthored-[Bb]y|Change-Id|Reviewed-by|Acked-by|Tested-by):'

CONF_ENGINE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CONF_JSON_GATE="$CONF_ENGINE_DIR/../json-gate.sh"
