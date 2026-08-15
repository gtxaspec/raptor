# Conformity hooks

Local git hooks that run this family's mechanical standards at the
moments they're cheapest to fix: `pre-commit` checks the staged diff,
`commit-msg` checks the message, `pre-push` runs the exact range
checks CI runs — so a push that passes locally cannot fail those CI
gates. Hooks are a safety net for the committer, not enforcement: CI
remains the wall for everyone, `--no-verify` is the documented escape
hatch, and every check fails open when its tooling is missing.

## Design: one engine, tracked policy

The engine (this directory) lives once, in raptor. Each repo in the
family declares which checks apply to it in a tracked one-line
`.conformity` manifest at its root — policy is versioned and reviewed
in the repo it governs, while the implementation cannot drift across
seven copies. A repo with no manifest gets no checks.

Checks:

| Token | What it does |
|-------|--------------|
| `format` | diff-scoped `git-clang-format-19` (staged at commit, outgoing range at push), same exclusions as CI |
| `json-gate` | no hand-written JSON in production C — calls `../json-gate.sh`, the same script `test-all.sh` stage 0 runs |
| `ips` | refuses lab/private addresses (`10.x`, `192.168.x`) in touched files — the check CI cannot do for you |
| `trailers` | refuses Signed-off-by / Co-authored-by / review-tag trailer lines, in the message at commit and across the range at push |

## Activation (one command per clone)

The repos are siblings (see the dev guide), so:

```bash
# inside raptor:
git config core.hooksPath tools/conformity/hooks
# inside any sibling (raptor-hal, raptor-ipc, raptor-common, compy,
# raptor-test, raptor-docs):
git config core.hooksPath ../raptor/tools/conformity/hooks
```

Git never auto-activates hooks from a clone — by design, a cloned
repo must not execute code — so this is a deliberate one-time opt-in
per clone. Without it, nothing changes and CI still gates everything
it always did.

## Rules for changing this directory

- Checks stay dumb and fast: greps and diff-scoped format, sub-second
  at commit, seconds at push. No test suites in hooks — that
  discipline lives in the workflow and in CI.
- A check that false-positives is a bug: fix it or delete it before
  people learn to `--no-verify` by reflex.
- `json-gate.sh` is the single source for the JSON rule; the suite
  and the hooks both call it. Never fork the pattern.
