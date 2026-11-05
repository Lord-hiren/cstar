---
phase: 01-dev-environment-repo-hygiene
plan: 03
subsystem: infra
tags: [ci, github-actions, ubuntu-24.04, gitattributes, crlf, tests]

# Dependency graph
requires:
  - phase: 01-dev-environment-repo-hygiene (01-02)
    provides: clean branch, untracked build/, green docs checker
provides:
  - .github/workflows/ci.yml running the four gates (warning-free build, make test, smoke test, check_docs) on ubuntu-24.04
  - A fix for a latent bug since the initial commit: file_line_edges.out lost its CR byte, so every fresh clone failed 1 test
  - Byte-exact storage for test expectations (.gitattributes -text for tests/cases/*.out/.err/.in)
affects: [phase 2 rename (CI gates every change once pushed), v0.4 phase 9 (Windows CI matrix)]

# Tech tracking
tech-stack:
  added: [GitHub Actions (actions/checkout@v4, actions/setup-node@v4)]
  patterns:
    - "Fresh-clone rehearsal (clone to the WSL filesystem, run the workflow's commands) before trusting CI or a push"
    - "Test expectation files are -text: never line-ending converted"

key-files:
  created:
    - .github/workflows/ci.yml
  modified:
    - .gitattributes
    - tests/cases/file_line_edges.out (committed bytes only; content as intended)
    - docs/development/building-and-testing.md
    - CLAUDE.md
    - CHANGELOG.md

key-decisions:
  - "Fix the committed expectation file and mark test expectations -text (user-approved boundary exception)"
  - "No push for now: CI verified by the local fresh-clone rehearsal only"

patterns-established:
  - "Rehearse CI in a fresh clone; local working copies can hide repository bugs"

# Metrics
duration: one session
completed: 2026-11-04T19:20:00Z
description: "CI workflow for the four gates; fresh-clone rehearsal found and fixed a test broken in every clone since the initial commit; 372/0 in a fresh clone; not pushed (user choice)"
type: Summary
about: "cstar"
---

# Phase 1 Plan 03: Linux CI — Summary

**A GitHub Actions workflow now runs the four gates: a warning-free build, `make test`, the smoke test and
the docs checker. The local fresh-clone rehearsal found a bug in the repository itself: since the initial
commit, the `file_line_edges` test failed in every fresh clone. That's now fixed, and a fresh clone passes
372/372. The branch was not pushed (your choice), so CI hasn't run on GitHub yet.**

## Performance

| Metric | Value |
|--------|-------|
| Duration | One session |
| Completed | 2026-11-04 |
| Tasks | 3 of 4 executed. Task 4 (push + watch the run) was skipped by decision `no-push` |
| Qualify results | 3 PASS (Task 2 passed after the approved fix) |
| Commits | `ed64763` (CI), `f97a7be` (fix) |

## Acceptance Criteria Results

| Criterion | Status | Notes |
|-----------|--------|-------|
| AC-1: Workflow valid, four gates | ✅ Pass | PyYAML parse + structure check: triggers, ubuntu-24.04, 20-minute timeout, read-only permissions, 7 steps, no secrets |
| AC-2: Fresh-clone rehearsal green | ✅ Pass | After the fix: 0 warnings, 372/0, 1294/0, 315 links / 0 broken, 52 samples |
| AC-3: Nothing published without approval | ✅ Pass | Nothing pushed; local and remote `main` still `6c82d72` |
| AC-4: CI result recorded | ✅ Pass (no-push branch) | Recorded as unverified on GitHub; the local rehearsal is the evidence |
| AC-5: CI documented | ✅ Pass | building-and-testing.md "Continuous integration", CLAUDE.md, CHANGELOG |

## Accomplishments

- **Every future push is gated.** Once the branch is pushed, every push to `main`/`phase-*` and every PR to
  `main` runs the same four commands that pass locally.
- **A hidden bug found and fixed.** The initial commit was made with `core.autocrlf=true`, which stripped
  the single intentional CR from `tests/cases/file_line_edges.out`. Working copies on the author's machine kept
  the byte, so the suite looked green locally but failed (371/372) in every fresh clone, including GitHub's
  `main`.
- **Test expectations can't be corrupted again.** They're now `-text`, so git stores them byte for byte.

## Task Commits

| Task | Commit | Type | Description |
|------|--------|------|-------------|
| Task 1: CI workflow + docs | `ed64763` | ci | `.github/workflows/ci.yml`, building-and-testing, CLAUDE.md, CHANGELOG |
| Task 2: Fresh-clone rehearsal (+ approved fix) | `f97a7be` | fix | `.gitattributes` `-text` rules; restore the CR byte in `file_line_edges.out` |
| Task 3: Push decision | — | — | `no-push` |
| Task 4: Push + watch run | — | — | Skipped (`no-push`) |

## Files Created/Modified

| File | Change | Purpose |
|------|--------|---------|
| `.github/workflows/ci.yml` | Created | The four gates on ubuntu-24.04 |
| `.gitattributes` | Modified | `tests/cases/*.out`, `*.err`, `*.in` → `-text` |
| `tests/cases/file_line_edges.out` | Modified (committed bytes) | `a` → `a\r`: restores the intended expected output |
| `docs/development/building-and-testing.md` | Modified | "Continuous integration" subsection |
| `CLAUDE.md`, `CHANGELOG.md` | Modified | CI pointer; Unreleased → Tooling entry |

## Decisions Made

| Decision | Rationale | Impact |
|----------|-----------|--------|
| Fix the committed `.out` bytes and mark test expectations `-text` | Fresh clones failed 1 test; the expectation is compared byte for byte | 372/372 in any clone |
| Don't push yet (`no-push`) | The repo is public; you chose to keep everything local for now | The first GitHub Actions run happens whenever the branch is pushed |

## Deviations from Plan

### Summary

| Type | Count | Impact |
|------|-------|--------|
| Auto-fixed (user-approved) | 1 | Essential: fixes a broken test in every clone |
| Skipped | 1 | Task 4, by decision |
| Deferred | 1 | CI unverified on GitHub |

### Auto-fixed Issues

**1. [Repository data] The committed expectation file was missing a CR byte**
- **Found during:** Task 2 (fresh-clone rehearsal: 371/372)
- **Issue:** `tests/cases/file_line_edges.out` in the initial commit `6c82d72` had `a\n` where the test prints
  `a\r\n`. The local working copy had the correct bytes, so the test only failed in clones.
- **Fix:** `.gitattributes` `-text` for `tests/cases/*.out|*.err|*.in`, then
  `git add --renormalize tests/cases/file_line_edges.out`. A plain `git add` didn't re-read the file, because
  its cached stats matched.
- **Boundary:** `tests/*` was protected by the plan; the fix ran only after you approved it.
- **Verification:** the staged blob has 1 CR and is byte-identical to the working copy; the fresh-clone
  rehearsal gives 372/0.
- **Commit:** `f97a7be`

### Deferred Items

- **D-9:** the first GitHub Actions run is pending until the branch is pushed. When it is, check the run on
  the Actions tab, or through `GET /repos/Lord-hiren/cstar/actions/runs` (the repo is public).
- **Note:** GitHub's `main` still has the broken `file_line_edges.out` until this branch is merged and pushed.

## Issues Encountered

| Issue | Resolution |
|-------|------------|
| `gh` not installed; curl blocked by a hook | Used the public REST API through the context-mode sandbox (`https` in node) |
| `git add` didn't stage the CR byte after the attribute change | `git add --renormalize` |

## Next Phase Readiness

**Ready:**
- A clean branch with a green fresh-clone rehearsal, as the base for Phase 2 (rename)
- CI that will gate the rename as soon as anything is pushed

**Concerns:**
- 6 commits on `phase-1-dev-environment` are local only; nothing is backed up on GitHub yet.

**Blockers:**
- None

---
*Built with PAUL Framework v1.4 · https://chrisai.cv/skool · https://youtube.com/@chris-ai-systems*
*Phase: 01-dev-environment-repo-hygiene, Plan: 03*
*Completed: 2026-11-04*
