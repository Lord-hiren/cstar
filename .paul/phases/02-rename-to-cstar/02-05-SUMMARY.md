---
phase: 02-rename-to-cstar
plan: 05
subsystem: documentation-spec
tags: [rename, spec, d-6, simd-vectors, docs-checker, normative, known-issue-10]

# Dependency graph
requires:
  - phase: 02-rename-to-cstar (02-01 … 02-04)
    provides: cstar CLI/paths, messages, editor extension and internal names that the docs now describe
provides:
  - "SPEC §5.11.9: fvec/ivec/fvec_fill/ivec_fill/dot fully specified; SPEC counts 78 builtins (D-6 closed)"
  - "SPEC, contracts and reference docs say cstar; m1 contract's M1 CLI marked superseded by SPEC §10"
  - "README, guides, internals, development/performance docs, CLAUDE.md and the docs checker say cstar, with today's facts"
  - "Known issue #10 (fvec % returns wrong values) logged for Phase 3"
  - "Phase 2 complete: everything user-facing says cstar; legacy names only as legacy notes"
affects: [Phase 3 (known issues #7, #8, #9, #10), Phase 4 release notes, v0.4 legacy removal (D-11 bench tooling)]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Specify from observation: probe the compiler for every edge before writing normative text; check quoted messages against code or actual output"
    - "Meaning-preserving wording passes: compare code spans before/after with the rename rules normalised"
    - "Final sweep with an allowlist: every remaining old-name hit gets a category; 0 untagged"

key-files:
  created:
    - .paul/phases/02-rename-to-cstar/02-05-SUMMARY.md
  modified:
    - SPEC.md
    - docs/contracts/m1-semantics.md, m2a-semantics.md, m2b-semantics.md
    - docs/reference/known-issues.md
    - README.md, docs/README.md, docs/guide/*.md, docs/development/*.md, docs/internals/*.md
    - docs/performance/README.md, docs/performance/optimization.md
    - tests/check_docs.py, tests/cases/*.cst (9 files, comments only)
    - CLAUDE.md, CHANGELOG.md, .gitignore, bench/cases/**/*.cpp, bench/gen*.py, bench/SPEED.md

key-decisions:
  - "fvec %: SPEC states the intended rule (TypeError, like float %); the compiler bug is known issue #10, fixed in Phase 3 (your choice)"
  - "m1 contract §9 (M1 CLI) is superseded by SPEC §10 — kept as the M1 record, no longer normative"
  - "Benchmark tooling (bench runners, workloads, tools/hpcbench) keeps the old names until D-11"

patterns-established:
  - "When a normative doc and the compiler disagree while specifying, stop, ask, write the intended rule, and log the bug"

# Metrics
duration: ~19 min of commits (plan 17:16Z → task 3 17:36Z) plus probes and verification
started: 2026-11-05T17:16:49Z
completed: 2026-11-05T17:50:00Z
description: "SPEC specifies the SIMD vectors and counts 78 builtins (D-6); SPEC, contracts, guides, internals, README, CLAUDE.md and the docs checker say cstar with today's facts; fvec % bug logged as known issue #10; sweep 0 untagged; all gates green in a fresh clone — Phase 2 complete"
type: Summary
about: "cstar"
---

# Phase 2 Plan 05: Docs and SPEC say cstar — Summary

**The words now match the tool.**
- **SPEC (D-6):** a new §5.11.9 specifies the five SIMD vector builtins, every message checked against the
  compiler, and the builtin count is 78 everywhere.
- **SPEC, contracts, guides, internals, README, CLAUDE.md and the docs checker** say cstar and describe
  today's behaviour. Several guides were still describing the pre-02-01 CLI; that is fixed.
- **Bug found:** specifying the vectors exposed one compiler bug, `fvec %` returning wrong values. It is
  known issue #10, scheduled for Phase 3.

This completes Phase 2.

## Performance

| Metric | Value |
|--------|-------|
| Duration | ~19 min between plan and last task commit, plus three probe rounds and verification |
| Completed | 2026-11-05 |
| Tasks | 3 of 3 (auto); one decision asked mid-task (fvec `%`) |
| Qualify results | 3 PASS. Task 3 caught one self-inflicted sample break before verification (see deviations) |
| Commits | `d6e5666` (plan), `65d5b59` (D-6), `854b25c` (normative docs), `9552911` (guides + tools) |
| Diff | 55 files, +507 / −316 |

## Acceptance Criteria Results

| Criterion | Status | Notes |
|-----------|--------|-------|
| AC-1: SPEC specifies the vector builtins and the true count (D-6) | ✅ Pass | **§5.11.9:** signatures, arity, element and scalar rules, operators, equality, lanes/indexing/stores, slicing, iteration, reductions, unhashability, and every message. **Messages:** 32 checked — 19 in code verbatim, 11 identical to the compiler's actual output (format-built), 2 exempt (a code expression; the intended `%` TypeError). **Counts:** seventy-eight in §3.5, §5.11.1, §8 and §10; `fvec`/`ivec` added to the §5.1 tags (eighteen), heap objects and the §5.1/§9.3 type names. **Sample** passes. **Headings:** 90 → 91, only 5.11.9 added |
| AC-2: Normative docs say cstar without changing meaning | ✅ Pass | **Code-span multiset** before/after (rename rules normalised): m2a, m2b and known-issues identical. SPEC and m1 differ only by the added legacy/supersession spans and the removed stale `.pentest`. **Section numbers** unchanged. Legacy notes and the corrections record kept |
| AC-3: Every other living doc and doc tool says cstar | ✅ Pass | ~280 lines in 50 files. `check_docs.py` runs samples as `example.cst` via `build/cstar`. CLAUDE.md vocabulary note updated. **Final sweep:** 579 remaining hits all tagged — bench tooling 214, history 162, code/legacy support 110, legacy notes 68, M1 record 12, private files 6, fixture data 3, dev path 2, corrections record 2 — 0 untagged |
| AC-4: All gates green | ✅ Pass | Local and in a fresh clone of `9552911`: 0 build warnings, make test 376/0, smoke 1363/0, check_docs 320 links / 0 broken, 53 samples pass (one more: the SPEC vector sample) |

## Accomplishments

- **The normative SPEC covers every builtin the compiler has.** §5.11.9 was written from probes of the
  real compiler (three rounds, about 65 cases), not from the guide's summary, which turned out to be wrong
  in three places.
- **A silent-wrong-math bug surfaced and is on record.** `fvec([5.0]) % fvec([2.0])` returns `[2.5]`, and
  with a scalar on the left `%` multiplies. Known issue #10 states the cause precisely.
- **A reader meets one name.** README → guides → SPEC → contracts all say cstar. Legacy names appear only
  where they explain legacy support, and the sweep proves it (0 untagged).
- **The docs gate itself is renamed.** `check_docs.py` runs on `build/cstar` and `.cst`, and all 53
  samples, with their renamed expected outputs, pass.

## Task Commits

| Task | Commit | Type | Description |
|------|--------|------|-------------|
| Plan | `d6e5666` | docs | Plan 02-05 |
| Task 1: D-6 | `65d5b59` | docs | SPEC §5.11.9 + counts + type lists; known issue #10 |
| Task 2: normative docs | `854b25c` | docs | SPEC/contracts/reference wording; m1 §9 superseded; known-issues #7–#9 |
| Task 3: guides + tools | `9552911` | docs | Guides, README, internals, CLAUDE.md, CHANGELOG, check_docs.py, test comments, bench prose |

Plan metadata: committed with this SUMMARY.

## Files Created/Modified

| File | Change | Purpose |
|------|--------|---------|
| `SPEC.md` | Modified | §5.11.9 (new); counts; type lists; title and prose; stale CLI facts |
| `docs/contracts/m1-semantics.md` | Modified | Title/prose; §9 superseded by SPEC §10; CLI table rows |
| `docs/contracts/m2a-semantics.md`, `m2b-semantics.md` | Modified | Titles and prose |
| `docs/reference/known-issues.md` | Modified | #10 added; #7–#9 use cstar names (#8 repro re-run) |
| `docs/guide/cli.md` | Modified | Rewritten to SPEC §10: `.cst`, index.cst, `./<base>`, collision error, `CSTAR_PATH`, `-lcurl` |
| `docs/guide/{getting-started,quick-reference,gotchas,modules,language-tour,…}.md` | Modified | cstar names and today's facts (build output, cache, module candidates) |
| `docs/guide/builtins.md` | Modified | SIMD section matches §5.11.9 |
| `docs/internals/*`, `docs/performance/{README,optimization}.md`, `docs/development/*` | Modified | cstar names; paths |
| `README.md`, `docs/README.md`, `CLAUDE.md`, `CHANGELOG.md` | Modified | Name, quick start (376 checks), vocabulary note, header + entries |
| `tests/check_docs.py` | Modified | `build/cstar`, `example.cst`, `cstar-docs-` |
| `tests/cases/*.cst` (9) | Modified (comments) | Prose; no line moved |
| `bench/cases/**/*.cpp`, `bench/gen*.py`, `bench/SPEED.md`, `.gitignore` | Modified (prose) | Language name only; tooling unchanged (D-11) |

## Decisions Made

| Decision | Rationale | Impact |
|----------|-----------|--------|
| `fvec %` → SPEC states the TypeError rule; fix in Phase 3 (asked; you chose the recommended option) | Normative docs lead the code; keeps 02-05 docs-only | Phase 3 gains issue #10 |
| m1 §9 superseded by SPEC §10 | Contracts outrank SPEC, so the stale M1 CLI conflicted with §10; SPEC §10 is complete and current | One normative source for the CLI |
| Keep the membership message's `->` wording | It is normative (m2a/m2b contracts + 7 test expectations) | Logged as D-12 for a deliberate decision |

## Deviations from Plan

### Summary

| Type | Count | Impact |
|------|-------|--------|
| Auto-fixed | 4 | Doc accuracy; one self-inflicted break caught before verification |
| Scope additions | 2 | Known-issue entry; stale-fact fixes beyond wording |
| Deferred | 2 new | #10 (Phase 3), D-12 |

**Total impact:** positive. The plan found and recorded a real compiler bug and fixed several stale facts.
There was no scope creep into code.

### Auto-fixed Issues

**1. [Self-inflicted, caught] The scripted rename broke two samples**
- `(?<=\w)\.pen` turned `"x.pen".ends_with(".pen")` into `"x.cst".ends_with(".pen")` (language-tour,
  quick-reference). Expected output `True` would have failed.
- **Fix:** `.ends_with(".cst")`. Found in the post-pass review, before check_docs ran.
- **Commit:** `9552911`

**2. [Docs] The builtins guide's SIMD section was wrong in three places**
- `fvec` does not take `bool` scalars; `%` is not scalar-like on `fvec`; `==` yields one `bool`.
- **Fix:** aligned with §5.11.9, with a link to issue #10.
- **Commit:** `9552911`

**3. [Docs] Guides still described the pre-02-01 CLI**
- `.pentest` build output, `~/.cache/pentest/`, `pentest-XXXXXX`, `$PENTEST_PATH` only, and a module
  candidate order without `.cst`. cli.md also lacked `index.cst`, the collision error and `-lcurl`;
  README said 372 checks.
- **Fix:** rewritten to SPEC §10 and the driver's real names (`<key>.bin`, `cstar-XXXXXX`, the clang
  message).
- **Commit:** `9552911`

**4. [Normative] The m1 contract still specified the M1 CLI**
- Since contracts win over SPEC, it contradicted SPEC §10.
- **Fix:** a supersession note plus updated table rows; the M1 rules are kept as the record.
- **Commit:** `854b25c`

### Scope additions

**1. Known issue #10 entry** (docs/reference/known-issues.md, Task 1) — needed once the bug was found;
links from SPEC and builtins.md.

**2. SPEC stale facts:** the scope list promised `.pentest` output; the exit-code table and pipeline step
named only `.pen`/`.pent`.

### Deferred Items

- **Known issue #10:** `fvec %` returns wrong values → Phase 3 (with #7, #8, #9).
- **D-12 (new, S):** the membership TypeError says `unsupported operand types for ->: …` (the retired
  operator). It is normative in the m2a/m2b contracts and 7 test expectations; changing it to `in` means
  contract + SPEC + expectations together.
- **Still open:** D-9 (CI on GitHub), D-10 (e2e editor test), D-11 (bench tooling, now incl. bench/
  runners and workloads).

## Issues Encountered

| Issue | Resolution |
|-------|------------|
| A sed script failed on a `#` in replacement text (delimiter clash) | Re-run with `@` delimiters; nothing had been applied |
| A hook rewrote a `sed … ; grep` command into an invalid one | Split: sed in Bash, the check with the Grep tool |
| Known issue #8's repro first failed: a PATH holding only clang lacks `ld` | Rebuilt PATH as `/usr/bin` minus `sha256sum`; repro confirmed (`program` left in `cstar-XXXXXX/`) |
| `perl -i` rewrote unchanged files (a "modified" notice for fvec_errors.cst) | Verified only 9 test files changed content |
| Ledger apply times for 02-04/02-05 were estimates | Set to the first task commit (17:03:39Z / 17:24:26Z) in this UNIFY |

## Next Phase Readiness

**Ready:**
- Phase 2 is complete. Code, CLI, editor, internals and docs all say cstar; the legacy names work until
  v0.4.
- Phase 3 can start on final names: issue #9 lives in `runtime/cstar_rt.c`; #10 is a small runtime fix
  plus a regression case (the expected TypeError wording is already in SPEC §5.11.9).

**Concerns:**
- `main` is now two phases behind the local branch, and nothing has been pushed (D-9). CI has never run
  on GitHub.

**Blockers:**
- None

---
*Built with PAUL Framework v1.4 · https://chrisai.cv/skool · https://youtube.com/@chris-ai-systems*
*Phase: 02-rename-to-cstar, Plan: 05*
*Completed: 2026-11-05*
