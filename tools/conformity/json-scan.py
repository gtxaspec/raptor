#!/usr/bin/env python3
"""json-scan: find hand-written JSON in C, across concatenated literals.

The rule this implements is json-gate.sh's; this is the half that a
line-oriented grep could not do.

WHY NOT GREP. The old gate matched the two characters `{\\"` on one
line, which is the idiomatic spelling and nothing else. All four of
these are the same defect and all four sailed past it:

    snprintf(b, n, "{ \\"host\\": \\"%s\\" }", host);      a space
    snprintf(b, n, "{\\n  \\"host\\": \\"%s\\"\\n}", host);  a newline
    snprintf(b, n, OBJ_OPEN "\\"host\\":\\"%s\\"}", host);  brace in a macro
    snprintf(b, n, "%s\\"host\\":\\"%s\\"}", "{", host);    brace as an argument

C concatenates adjacent string literals, so the unit that matters is
the RUN -- every literal the compiler will glue together, including
across newlines and across a macro name sitting between two of them.
That is what this scans, which is why the brace can be anywhere in it.

TWO RULES, and they answer different questions.

  shape   a run containing `{"` (escaped) is a JSON object written out
          by hand. Constant or not, it is the convention: JSON comes
          from a serializer. This is the old gate's rule, widened to
          allow whitespace after the brace and to see across a run.

  inject  a run containing a JSON key separator -- a quoted key, a
          colon, and something a value can start with -- together with
          a printf conversion. This is the one with teeth: a document
          assembled around a value at runtime, whatever the braces
          look like.

`inject` is deliberately not "any \\" plus any %". rhd's
WWW-Authenticate header carries an escaped quote and a %d in the same
snprintf and is not JSON, so the key separator is required; and a log
line reading `\\"%s\\": the standard form is a bare number` is a quoted
name followed by English, so a JSON value start is required after the
colon. Both shapes are in the tree and neither is a document.

WHAT IT STILL WILL NOT SEE: a fragment with no key separator of its
own, assembled into a document somewhere else -- `"\\"host\\",", h` and
then a brace added later. Catching that needs dataflow, not a scanner,
and CI is not where that belongs.

Exit 1 and print `file:line: text` (grep -n's shape, so the callers do
not care which half found it) when anything matches; silent 0 when not.
"""

import re
import sys

# A printf conversion, not a literal %%. Flags, width, precision and
# length modifiers are all optional; the conversion character is what
# makes it one.
CONV = re.compile(r"%[-+ #0']*[0-9*]*(?:\.[0-9*]+)?(?:hh|h|ll|l|L|q|j|z|t)?[diouxXeEfFgGaAcspn]")

# A JSON key/value separator: an escaped quote closing a key, a colon,
# and then something a JSON value can actually start with -- another
# string, an object, an array, a number, a keyword, or a conversion
# standing in for one.
#
# The value start is what keeps prose out. A log line reads
#
#     RSS_WARN("gpio.%s \\"%s\\": the standard form is a bare number")
#
# which is a quoted name followed by a colon followed by English, and
# is not a document. Two of those live in the tree today.
KEY_SEP = re.compile(r'\\"\s*:\s*(?:\\"|[\[{0-9%-]|true|false|null)')

# An object opening onto a quoted key.
OBJ_OPEN = re.compile(r'\{\s*\\"')

IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def literal_runs(text):
    """Yield (line, raw) for each run of adjacent string literals.

    `raw` is the source spelling of the contents with the quotes
    removed and the parts concatenated -- so a backslash-quote stays a
    backslash and a quote, which is what both rules look for.

    A run survives whitespace, comments and a bare identifier between
    two literals, because a macro that expands to a literal is glued in
    by the compiler like any other part. Any other token ends it.
    """
    i, n, line = 0, len(text), 1
    parts, start = [], 0

    def flush():
        nonlocal parts, start
        if parts:
            yielded = (start, "".join(parts))
            parts = []
            return yielded
        return None

    while i < n:
        c = text[i]

        if c == "\n":
            line += 1
            i += 1
            continue

        if c.isspace():
            i += 1
            continue

        # A macro's line continuation is whitespace to the compiler, so
        # a document split across the lines of a #define is still one
        # run. Without this the brace and the conversion can land in
        # different runs and neither half looks like a document.
        if c == "\\" and i + 1 < n and text[i + 1] == "\n":
            line += 1
            i += 2
            continue

        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue

        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += text.count("\n", i, j)
            i = j
            continue

        if c == "'":  # a character literal, never a run
            i += 1
            while i < n and text[i] != "'":
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue

        if c == '"':
            if not parts:
                start = line
            i += 1
            begin = i
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    i += 2
                else:
                    i += 1
            parts.append(text[begin:i])
            line += text.count("\n", begin, i)
            i += 1
            continue

        m = IDENT.match(text, i)
        if m:
            # Glue only between literals; an identifier on its own
            # starts nothing.
            i = m.end()
            if not parts:
                continue
            continue

        run = flush()
        if run:
            yield run
        i += 1

    run = flush()
    if run:
        yield run


def scan(path, exempt_line):
    """Return a list of `path:line: text` findings."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return []

    lines = text.splitlines()
    out = []
    for line, raw in literal_runs(text):
        shape = OBJ_OPEN.search(raw)
        inject = KEY_SEP.search(raw) and CONV.search(raw)
        if not shape and not inject:
            continue
        # A run's exemption is any of its lines carrying the construct:
        # a run is often several lines long and the marker sits on the
        # first of them, so match anywhere in the run, not on line one.
        window = lines[line - 1 : line + 4]
        if exempt_line and any(exempt_line.search(x) for x in window):
            continue
        why = "interpolated into JSON" if inject else "hand-written JSON"
        src = lines[line - 1].strip() if line - 1 < len(lines) else ""
        out.append("%s:%d: %s [%s]" % (path, line, src, why))
    return out


# Both rules by example, and the near misses that must stay quiet. The
# gate under-matched for three weeks without anyone noticing, so it
# carries its own cases: `json-scan.py --selftest`.
SELFTEST = [
    (True, r'snprintf(b, n, "{\"host\":\"%s\"}", h);', "the idiomatic spelling"),
    (True, r'snprintf(b, n, "{ \"host\": \"%s\" }", h);', "a space after the brace"),
    (True, r'snprintf(b, n, "{\n \"host\": \"%s\"\n}", h);', "a newline after it"),
    (True, r'snprintf(b, n, OPEN "\"host\":\"%s\"}", h);', "the brace in a macro"),
    (True, r'snprintf(b, n, "%s\"host\":\"%s\"}", "{", h);', "the brace as an argument"),
    (True, 'snprintf(b, n, "{\\"a\\":1,"\n               "\\"h\\":\\"%s\\"}", h);',
     "the conversion on a later line than the brace"),
    (True, r'static const char body[] = "{\"status\":\"error\"}";', "a constant document"),
    (False, r'snprintf(b, n, "realm=\"raptor\"\r\nLength: %d\r\n", len);',
     "an HTTP header quotes and counts, and is not JSON"),
    (False, r'RSS_WARN("gpio.%s \"%s\": the standard form is a number", k, v);',
     "a quoted name, a colon, then English"),
    (False, r'snprintf(b, n, "he said \"%s\" loudly", s);', "a quoted value, no key"),
    (False, r'return "a constant with \"quotes\" and no colon";', "quotes alone"),
    (False, r'#include "rhd.h"', "an include"),
    (False, r'const char *u = "http://example/*not a comment*/";',
     "a comment opener inside a string"),
    (False, "/* a comment mentioning {\\\"json\\\":\\\"%s\\\"} */ int x;",
     "a comment describing the thing it forbids"),
]

# The lexer traps, which need more than one line each.
SELFTEST_MULTILINE = [
    (True, '#define T(k) \\\n\t"{\\"cmd\\":\\"" k "\\","\\\n\t"\\"v\\":\\"%s\\"}"',
     "a document split across a macro's continuation lines"),
]


def selftest():
    import tempfile, os

    bad = 0
    cases = [(w, "void f(void)\n{\n\t" + src + "\n}\n", why) for w, src, why in SELFTEST]
    cases += [(w, src + "\n", why) for w, src, why in SELFTEST_MULTILINE]
    for want, body, why in cases:
        fd, path = tempfile.mkstemp(suffix=".c")
        with os.fdopen(fd, "w") as fh:
            fh.write(body)
        got = bool(scan(path, None))
        os.unlink(path)
        if got != want:
            bad += 1
            print("FAIL  expected %s: %s" % ("a hit" if want else "silence", why))
            print("      %s" % body.replace("\n", "\\n"))
    print("json-scan selftest: %d cases, %d failed" % (len(cases), bad))
    return 1 if bad else 0


def main(argv):
    if len(argv) > 1 and argv[1] == "--selftest":
        return selftest()
    exempt = re.compile(argv[1]) if argv[1] else None
    hits = []
    for path in argv[2:]:
        hits += scan(path, exempt)
    for h in hits:
        print(h)
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
