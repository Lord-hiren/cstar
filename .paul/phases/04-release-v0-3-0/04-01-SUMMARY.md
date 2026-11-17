---
phase: 04-release-v0-3-0
plan: 01
subsystem: release
tags: [release, v0.3.0, versioning, changelog, git-tag, github, ci]

# Dependency graph
requires:
  - phase: 03-fix-known-issues
    provides: "No open known issues; the CHANGELOG Fixed section"
provides:
  - "cstar 0.3.0: version string, extension manifest, SPEC and docs describing it as released"
  - "CHANGELOG [0.3.0] — 2026-11-13 with highlights; a fresh empty [Unreleased]"
  - "Annotated tag v0.3.0 on the release commit d31f120, on GitHub"
  - "GitHub main fast-forwarded to the release; first CI run on main, green (run 43374607457)"
  - "Release notes for the GitHub Release page (scratchpad)"
affects: [v0.4 Cross-platform SDK (first milestone after a release), /paul:complete-milestone]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Release = a gated commit (all four CI gates, local + fresh clone) + an annotated tag; publishing is a separate decision checkpoint"
    - "Check CI through GitHub's public REST API when gh is not available"
    - "Version mentions are classified: current-version statements change, historical feature tags stay"

key-files:
  created:
    - .paul/phases/04-release-v0-3-0/04-01-SUMMARY.md
  modified:
    - src/main.cpp
    - editors/vscode/package.json
    - SPEC.md
    - CHANGELOG.md
    - README.md
    - docs/history/README.md

key-decisions:
  - "Publish: fast-forward main to the release commit and push main + tag v0.3.0 (your choice at the checkpoint)"
  - "Historical feature tags like '(0.2.1)' stay; only current-version statements say 0.3.0"
  - "History docs get appended rows and a dated note, never rewrites"

patterns-established:
  - "No push without an explicit checkpoint decision; fast-forward only, never force"

# Metrics
duration: ~36 min from the plan commit to the release commit, then ~20 min to publish and confirm CI
started: 2026-11-13T04:54:19Z
completed: 2026-11-13T05:53:12Z
description: "cstar 0.3.0 released: version string and docs updated, CHANGELOG [0.3.0] — 2026-11-13, annotated tag v0.3.0 on d31f120; at your approval main was fast-forwarded and pushed with the tag, and the first CI run on main passed"
type: Summary
about: "cstar"
---

# Phase 4 Plan 01: Release cstar 0.3.0 — Summary

**cstar 0.3.0 is released.** `cstar --version` prints `cstar 0.3.0`, and SPEC and the docs describe 0.3.0 as
released. CHANGELOG has its `[0.3.0] — 2026-11-13` section. An annotated `v0.3.0` tag sits on the release commit
`d31f120`.

At your approval, GitHub `main` was fast-forwarded from `6c82d72` to `d31f120` and pushed with the tag. The first CI
run on `main` passed all four gates: [run 43374607457](https://github.com/Lord-hiren/cstar/actions/runs/43374607457).

## Performance

| Metric | Value |
|--------|-------|
| Duration | ~36 min from the plan to the release commit; ~20 min to publish and confirm CI |
| Started | 2026-11-13T04:54:19Z (plan commit) |
| Completed | 2026-11-13T05:53:12Z (CI confirmed) |
| Tasks | 3 auto (3 PASS on qualify) + 1 decision checkpoint (publish) |
| Commits | `ef6e49e` (plan), `ac82bba`, `d31f120` (release commit) |
| Diff | 19 files, +104 / −67 |

## Acceptance Criteria Results

| Criterion | Status | Notes |
|-----------|--------|-------|
| AC-1: The version is 0.3.0 wherever it states the current version | ✅ Pass | `cstar --version`, `pentest --version` and `-v` all print `cstar 0.3.0`; the manifest says 0.3.0. Every current-version statement was updated: SPEC title, §1, §10, §12; README; docs index; CLI guide; getting started; language tour; quick reference; known issues; editor-extension and performance pages. The 0.2.1 feature tags stay |
| AC-2: Shipped work is described as released | ✅ Pass | No live document says "unreleased". The only hits are CLAUDE.md and documentation-guide.md, which point contributors at the CHANGELOG section. SPEC numbers are unchanged. The §5.11.8/§5.11.9 anchors moved and sqli.md follows. check_docs: 310 links, 0 broken; 53 samples pass |
| AC-3: CHANGELOG has the 0.3.0 release | ✅ Pass | One empty `[Unreleased]`, then `[0.3.0] — 2026-11-13` with seven highlights. Compared with the old `[Unreleased]`, the body changes in only 2 lines (see Deviations) |
| AC-4: A gated, tagged release commit | ✅ Pass | Working tree and fresh clone of `phase-4-release`: make with no warnings, make test 381/0, smoke 1363/0, check_docs 0 broken + 53 samples. Annotated `v0.3.0` → `d31f120`; the tagger is your git identity; no co-author line |
| AC-5: Publishing happens only as you choose | ✅ Pass | You chose **publish**. Local `main` fast-forwarded (`git fetch . phase-4-release:main`); `git push origin main` went `6c82d72..d31f120` with no force; the tag was pushed. `ls-remote`: `main` and `v0.3.0^{}` = `d31f120`. CI run on `main` succeeded in ~2.5 min (build, tests, smoke, docs). The release notes are ready to paste |

## Accomplishments

- **First release under the cstar name.** The tag is the stable reference the v0.4 SDK and future packages build on.
- **GitHub `main` is current and green.** D-9 is closed: the broken `file_line_edges.out` is gone from `main`, and its
  first CI run passed.
- **The release is honest.** No document calls shipped features "unreleased", no stale version is left, and history
  is preserved: historical feature tags stay and the history page only gained rows and a note.

## Task Commits

| Task | Commit | Type | Description |
|------|--------|------|-------------|
| Plan | `ef6e49e` | docs | Plan 04-01 (survey: version classes, unreleased wording, GitHub state) |
| Task 1: version + released wording | `ac82bba` | chore | `kVersion`, manifest, SPEC, README, 14 docs |
| Task 2: changelog + history | `d31f120` | docs | `[0.3.0] — 2026-11-13`, highlights, history rows/note |
| Task 3: gates + tag | — | — | All gates, local and fresh clone; annotated tag `v0.3.0` → `d31f120` |
| Checkpoint: publish | — | — | Fast-forward `main`, push `main` + tag, CI green |

Plan metadata: committed with this SUMMARY (on `phase-4-release`; not pushed).

## Files Created/Modified

| File | Change | Purpose |
|------|--------|---------|
| `src/main.cpp` | Modified | `kVersion` = "0.3.0" |
| `editors/vscode/package.json` | Modified | version 0.3.0 |
| `SPEC.md` | Modified | Title, normative-status paragraph, §1, §5.11.8/§5.11.9 headings, §10, §12, §13, §14 status |
| `README.md` | Modified | Version, "New in 0.3.0" status, `--version` row, roadmap now points at v0.4 |
| `docs/README.md`, `docs/reference/{spec-map,known-issues}.md`, `docs/contracts/README.md` | Modified | Current version; the v0.3 carve-out no longer "unreleased" |
| `docs/guide/{cli,getting-started,modules,language-tour,quick-reference,sqli}.md` | Modified | 0.3.0 version lines, released wording, the new SPEC anchor |
| `docs/development/editor-extension.md`, `docs/performance/{README,optimization}.md` | Modified | 0.3.0 package version / "current" stage |
| `CHANGELOG.md` | Modified | `[0.3.0] — 2026-11-13` + highlights; new empty `[Unreleased]` |
| `docs/history/README.md` | Modified | Timeline rows (2026-11-04, -05, -06→11, -13), a 0.3.0 roadmap row, a dated note |

## Decisions Made

| Decision | Rationale | Impact |
|----------|-----------|--------|
| Publish: fast-forward `main` and push it with the tag (your choice) | The release should be the default branch, with CI on it | GitHub `main` = cstar 0.3.0; D-9 closed |
| Keep historical feature tags `(0.2.1)` | They record when a feature shipped; rewriting them would falsify history | Version sweeps must classify, not replace blindly |
| Release notes in the scratchpad, not the repo | The GitHub Release page is yours to create (no `gh`); the CHANGELOG is the record in the repo | You paste them on github.com |

## Deviations from Plan

### Summary

| Type | Count | Impact |
|------|-------|--------|
| Auto-fixed | 1 | Release correctness |
| Scope additions | 1 | Small doc paragraph |
| Deferred | 0 | — |

**Total impact:** small. Both are wording fixes that the release would otherwise have shipped wrong.

### Auto-fixed Issues

**1. [Docs] A CHANGELOG entry in the release section named the old version**
- **Found during:** Task 2.
- **Issue:** a `[Unreleased]` entry said "`--version` prints `cstar 0.2.1`". Moved into `[0.3.0]` unchanged, that
  would be false. The plan said to keep entries byte-for-byte.
- **Fix:** it now says `cstar 0.3.0`. This is the only content change besides dropping "unreleased", as the
  old-vs-new body diff shows (2 lines).
- **Commit:** `d31f120`.

### Scope Additions

1. **The README roadmap paragraph was rewritten.** It still said "Next: fix the known issues", which Phase 3 had done.
   It now names v0.4 (the cross-platform SDK) and the later milestones, plus generators, typing and the stdlib from
   SPEC §13 (`ac82bba`).

### Deferred Items

None.

## Issues Encountered

| Issue | Resolution |
|-------|------------|
| `git fetch . d31f120:main` failed (fetch needs a ref name), so the first `git push origin main` was a no-op ("Everything up-to-date") | Used `git fetch . phase-4-release:main` (still fast-forward only), then pushed |
| `gh` not installed anywhere (Git Bash, PowerShell, WSL) | CI checked through the public REST API; the Release page is left to you, with the notes ready |

## Next Phase Readiness

**Ready:**
- Milestone v0.3 cstar Foundation is complete (4 of 4 phases). Next: `/paul:complete-milestone`, then v0.4
  (Cross-platform SDK).

**Concerns:**
- The PAUL metadata commits (this UNIFY and the transition) are on `phase-4-release`, so GitHub `main` is a few
  commits behind. Pushing them needs your yes.
- The GitHub Release page for `v0.3.0` is not created yet. That's yours: notes in
  `scratchpad/p0401/release-notes-v0.3.0.md`.
- The `backup/phase-1-dev-environment` and `backup/phase-2-rename` branches still wait for your yes to delete.

**Blockers:**
- None

---
*Built with PAUL Framework v1.4 · https://chrisai.cv/skool · https://youtube.com/@chris-ai-systems*
*Phase: 04-release-v0-3-0, Plan: 01*
*Completed: 2026-11-13*
