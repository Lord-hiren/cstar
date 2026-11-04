---
phase: 01-dev-environment-repo-hygiene
plan: 02
subsystem: infra
tags: [git, gitignore, docs, spec, hygiene, libcurl]

# Dependency graph
requires:
  - phase: 01-dev-environment-repo-hygiene (01-01)
    provides: WSL toolchain, LF line endings, 372/1294/52 baseline, deferred items D-1..D-5
provides:
  - Phase branch phase-1-dev-environment with the 01-01 and 01-02 work committed (main untouched)
  - build/ and scan.log out of git and ignored
  - Docs whose counts, dependency claims and links match the code; check_docs.py fully green (0 broken links)
affects: [01-03 Linux CI, phase 2 rename (cleaner diff), 02-02 editor extension (D-7), SPEC plan (D-6)]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "One branch per phase (phase-1-dev-environment); a commit per task, each with the Co-Authored-By trailer"
    - "examples/ is private scratch: docs use inline programs or tests/cases, never links into examples/ or extras/"

key-files:
  created:
    - .paul/phases/01-dev-environment-repo-hygiene/01-02-SUMMARY.md
  modified:
    - .gitignore
    - README.md
    - SPEC.md
    - docs/guide/{getting-started,language-tour,builtins,sqli,modules,gotchas,cli}.md
    - docs/development/{building-and-testing,editor-extension}.md
    - docs/internals/{architecture,runtime}.md
    - docs/performance/optimization.md
    - docs/reference/known-issues.md
    - CLAUDE.md
    - CHANGELOG.md

key-decisions:
  - "examples/ stays private (option C); docs reference no files in it"
  - "Work for v0.3 happens on a phase branch; main is only touched at the phase transition"

patterns-established:
  - "Before writing a count in the docs, confirm it against the source (builtins: src/semantic.cpp is_reserved + src/builtins.h)"
  - "Stale claims in records (docs/history, baselines, contracts, known-issues 'this pass') are left as written"

# Metrics
duration: one session
completed: 2026-11-04T18:45:00Z
description: "Phase branch with 3 commits; build/ and scan.log untracked; docs corrected (372 tests, 1294 smoke checks, 78 builtins, libcurl); check_docs 315 links / 0 broken"
type: Summary
about: "cstar"
---

# Phase 1 Plan 02: Repo hygiene and stale-doc fixes — Summary

**All work is now committed on the branch `phase-1-dev-environment`, and build output is no longer
tracked. The docs match the code again: 372 tests, 1,294 smoke checks, 78 builtins, and libcurl listed as a
dependency. `check_docs.py` is fully green for the first time: 315 links, 0 broken.**

## Performance

| Metric | Value |
|--------|-------|
| Duration | One session |
| Completed | 2026-11-04 |
| Tasks | 4 of 4 (3 auto, 1 decision checkpoint) |
| Qualify results | 4 PASS, 0 GAP, 0 DRIFT |
| Files modified | 20 docs/config files; 19 build files and scan.log removed from the index |

## Acceptance Criteria Results

| Criterion | Status | Notes |
|-----------|--------|-------|
| AC-1: Work committed on a phase branch | ✅ Pass | `49342a4`, `f33c6a9`, `db881a6` on `phase-1-dev-environment`; `main` still `6c82d72`; every commit has the trailer; status clean |
| AC-2: Build output and scan.log untracked | ✅ Pass | `git ls-files build scan.log` is empty; both ignored; rebuild + `make test` 372/0 leaves status clean |
| AC-3: Counts match reality | ✅ Pass | 372 = 365 + 7, 1294, 78 (confirmed in source). Only protected records still say 368 |
| AC-4: Dependency and network claims honest | ✅ Pass | README, getting-started, cli, architecture, runtime and SPEC §2/§11.4/§12 say libc + libm + libcurl; 89 SPEC headings before and after |
| AC-5: No broken links | ✅ Pass | check_docs: 315 links, 0 broken. The only remaining `examples/` mentions are plain text (SPEC §11.5 "not in the repository", CLAUDE.md) or protected records |
| AC-6: Nothing else regresses | ✅ Pass | `make test` 372/0; smoke 1294/0; samples 52/0 |

## Accomplishments

- **First fully green docs check.** 0 broken links, down from 3. Every count and dependency claim in a
  current-state doc now matches the code.
- **Clean version control.** Build products are out of git, the work is split into three reviewable commits
  on a phase branch, and `main` is untouched. The Phase 2 rename diff will contain only rename changes.
- **The README quick start works in a fresh clone.** It creates `hello.pen` inline instead of pointing at a
  git-ignored file.

## Task Commits

| Task | Commit | Type | Description |
|------|--------|------|-------------|
| Task 1: Phase branch + commit 01-01 work | `49342a4` | chore | `.gitattributes`, CLAUDE.md, quick reference, getting-started (WSL), `.paul/` |
| Task 2: Stop tracking build output | `f33c6a9` | chore | `git rm --cached build/` (19 files), remove scan.log, `.gitignore` |
| Task 3: Decision checkpoint | — | — | Option C chosen (examples/ stays private) |
| Task 4: Docs corrections | `db881a6` | docs | Counts, dependency claims, example links, SPEC wording, CHANGELOG |

Plan metadata: committed with this SUMMARY (docs: complete plan 01-02).

## Files Created/Modified

| File | Change | Purpose |
|------|--------|---------|
| `.gitignore` | Modified | Ignore `build/` and `/scan.log` |
| `build/*` (19), `scan.log` | Removed from index | Generated files; build/ stays on disk |
| `README.md` | Modified | libcurl, 78 builtins, 372 checks, 365 cases, inline quick start, no `examples/` row |
| `SPEC.md` | Modified (wording only) | §2 executable deps, §5.11.8 SQLi links → guide, §11.4 runtime deps, §11.5 `examples/**` row, §12 networking bullet |
| `docs/guide/getting-started.md` | Modified | libcurl requirement row, deps sentence, 372, "More examples" instead of bundled examples |
| `docs/guide/language-tour.md` | Modified | "Seventy-eight names", Network and SIMD rows |
| `docs/guide/builtins.md`, `modules.md`, `sqli.md` | Modified | No links to missing files; AI.pen and the SQLi lab described as not included |
| `docs/guide/gotchas.md`, `cli.md` | Modified | No `examples/functions.pen` path; libcurl dependency |
| `docs/reference/known-issues.md` | Modified | Issue #8 repro creates its own `hello.pen` |
| `docs/development/building-and-testing.md`, `editor-extension.md` | Modified | 372 = 365 + 7, 1294; build/ is ignored |
| `docs/performance/optimization.md` | Modified | 372 = 365 + 7 |
| `docs/internals/architecture.md`, `runtime.md` | Modified | `-lcurl`; libcurl dependency; runtime about 10,400 lines |
| `CLAUDE.md`, `CHANGELOG.md` | Modified | build/ ignored; Unreleased → Documentation entry |

## Decisions Made

| Decision | Rationale | Impact |
|----------|-----------|--------|
| `examples/` stays private (option C, user's choice) | The author keeps examples as local scratch | Docs use inline programs and `tests/cases`; no examples ship |
| One branch per phase, a commit per task | Repo rule: branch before committing on the default branch | `main` changes only at the phase transition (with user approval) |

## Deviations from Plan

### Summary

| Type | Count | Impact |
|------|-------|--------|
| Auto-fixed | 3 | Same-category stale claims found while editing |
| Scope additions | 0 | — |
| Deferred | 3 | Logged to STATE.md |

**Total impact:** small extensions of the same fix, no scope creep. The decision went to option C
instead of the recommended A, which is the user's choice and was handled within the plan.

### Auto-fixed Issues

**1. [Docs/SPEC] Two more stale SPEC spots beyond the three listed**
- **Found during:** Task 4
- **Issue:** SPEC §2 line 122 ("standalone apart from the system C library and libm") and the §11.5 row
  `examples/**` | "example programs" were stale in the same way as the listed lines.
- **Fix:** Wording only: "C library, libm and libcurl"; "local example programs (git-ignored, not in the
  repository)". No rule change, no renumbering (89 headings before and after).
- **Commit:** `db881a6`

**2. [Docs] Stale claims in files the plan didn't list**
- **Found during:** Task 4 searches
- **Issue:**
  - `cli.md:53` said programs need only libc + libm
  - `gotchas.md:234` named `examples/functions.pen`
  - the known-issues #8 repro used `examples/hello.pen`
  - the README said "361 cases" and had an `examples/` layout row
  - `runtime.md` said "about 6,800 lines" (actual 8,584 + 1,837 = ~10,400)
- **Fix:** Corrected each one.
- **Commit:** `db881a6`

**3. [Tooling] PowerShell 5.1 quoting and an rtk/zap hook rewriting commands**
- **Issue:** Inline `bash -c "…"` with nested quotes was mangled. One inline `node -e` was rejected by
  the hook. `git` isn't on the PATH of the PowerShell tool.
- **Fix:** Scripts in the scratchpad, run through WSL or node; git through Git Bash.

### Deferred Items

- **D-6** (existing): SPEC doesn't document `fvec`/`ivec`/`fvec_fill`/`ivec_fill`/`dot` and still says
  "seventy" builtins.
- **D-7** (new): the VSCode extension's `RESERVED` list lacks the five vector builtins
  (`editors/vscode/src/extension.js`), so the editor is out of sync with `src/semantic.cpp`. The smoke test
  passes because it checks the extension's own lists. → 02-02 (tests and editor extension).
- **D-8** (new): the docs/history headers (`history/README.md:16`, `progress-m2b.md:4`, `progress-m2c.md:4`)
  say "the current suite is 368 checks". They're records, so they were left alone; replace the number with
  "see `make test`" when the rename touches them.

## Issues Encountered

| Issue | Resolution |
|-------|------------|
| Session restarted mid-Task 4 | Resumed from the branch and STATE.md; no work lost |
| The first count search (with `\b`) returned nothing under the command wrapper | Re-ran without word boundaries; verified each hit by reading |

## Next Phase Readiness

**Ready:**
- A clean, committed base on `phase-1-dev-environment` for 01-03 (CI) and for the Phase 2 rename
- Green gates to compare against: tests 372/0, smoke 1294/0, docs 315 links / 0 broken, 52 samples

**Concerns:**
- The branch exists only locally. CI (01-03) only runs on GitHub, which needs a push to `origin`
  (github.com/Lord-hiren/cstar), and that needs your approval.

**Blockers:**
- None

---
*Built with PAUL Framework v1.4 · https://chrisai.cv/skool · https://youtube.com/@chris-ai-systems*
*Phase: 01-dev-environment-repo-hygiene, Plan: 02*
*Completed: 2026-11-04*
