---
phase: 03-fix-known-issues
plan: 01
subsystem: codegen-performance
tags: [known-issue-9, text, amortized-append, ownership-proof, codegen, a-b-proof]

# Dependency graph
requires:
  - phase: 02-rename-to-cstar
    provides: final internal names (cs_text_cat2, analyze_text_cats) and the A/B tooling pattern
provides:
  - "Text built with s += x / s = s + x inside functions uses the in-place append (amortized O(n))"
  - "A wider ownership proof: non-retaining reads anywhere, terminal reads after the last append"
  - "Regression case txt_build_amortized and harness check text_build_grow (inspects generated C)"
  - "Known issue #9 closed; eligibility rules in docs/performance/optimization.md"
affects: [03-02 (same branch), v0.6 server work (handlers build text in functions), Phase 4 release notes]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Guard an optimisation deterministically: assert on the generated C (clang shim) for positive and negative programs, not on timing"
    - "Flow-sensitive extension of a flow-insensitive proof: pre-order statement index + enclosing-loop ids decide whether a later append can follow a read"

key-files:
  created:
    - tests/cases/txt_build_amortized.cst
    - tests/cases/txt_build_amortized.out
  modified:
    - src/codegen.cpp
    - tests/run_tests.sh
    - docs/reference/known-issues.md
    - docs/performance/optimization.md
    - docs/development/building-and-testing.md
    - README.md
    - CLAUDE.md
    - CHANGELOG.md

key-decisions:
  - "Widen the codegen proof, not the runtime: the runtime's in-place append was already correct"
  - "Keep the writes rule (literals and own appends only): it is what guarantees a single appender per buffer"

patterns-established:
  - "Before planning a known-issue fix, re-measure the issue's own repro: #9's repro was already fixed, the real gap was elsewhere"

# Metrics
duration: ~12 min between plan and last task commit, plus probes, A/B and rehearsal
started: 2026-11-06T03:57:44Z
completed: 2026-11-06T04:26:00Z
description: "Text built inside functions is amortized: the analyze_text_cats proof accepts non-retaining and terminal reads; 400k appends 4.9 s -> 0.011 s; A/B 366/366 identical; regression case + generated-C harness check; known issue #9 closed"
type: Summary
about: "cstar"
---

# Phase 3 Plan 01: Amortized text building inside functions — Summary

**Building text with `s += x` inside a function is now linear.** The compiler's ownership proof for the existing
in-place append also accepts reads that cannot keep the buffer, and reads after the last append. Returning, printing
or testing the text no longer forces a full copy per append. There is no runtime change, and outputs are
byte-identical.

| 400,000 appends | Before | After |
|-----------------|--------|-------|
| in a function, then `return s` | 4.937 s | **0.011 s** |
| in a function, `for … in range` | 5.019 s | **0.008 s** |
| top-level global (the #9 repro) | 0.009 s | 0.008 s |
| `t = s` before the loop (sharing) | 4.680 s | 6.557 s — still copies, by design |

## Performance

| Metric | Value |
|--------|-------|
| Duration | ~12 min between plan and last task commit (+ probes, A/B, rehearsal) |
| Completed | 2026-11-06 |
| Tasks | 3 of 3 (auto) |
| Qualify results | 3 PASS on the first verification |
| Commits | `878a009` (plan), `2a2e17c` (fix + case + check), `10a47c4` (docs) |
| Diff | 10 files, +374 / −77 |

## Acceptance Criteria Results

| Criterion | Status | Notes |
|-----------|--------|-------|
| AC-1: Function-local builders are amortized | ✅ Pass | 7 probes now emit `cs_text_cat2(…, 1)`: local return, in-loop tests (len/==/in/find/starts_with/ends_with/`if s:`/print), early return, nested loops, top-level global then print, bytes, recursion. The old compiler admits none of them. Timing as in the table above |
| AC-2: Sharing patterns keep copy semantics | ✅ Pass | 6 negative probes are not admitted: alias between appends, a user function in the loop, try/finally, a function appending to a global, a ternary branch, alias-then-append. The case prints `abc abcd False` and `[x, xx, xxx]` (snapshots) |
| AC-3: Nothing observable changes | ✅ Pass | A/B against the pre-change compiler: 366/366 cases byte-identical (stdout, stderr, exit). In generated C, only `gc_text_liveness` (0 → 2 sites) and the new case (0 → 11) gained in-place appends; none lost one |
| AC-4: Guarded by tests; issue closed; gates green | ✅ Pass | `txt_build_amortized` (11 lines, each checked by hand) and `text_build_grow`. make test 378/0, smoke 1363/0, check_docs 319 links / 0 broken + 53 samples. Fresh clone of `10a47c4` green. #9 removed, CHANGELOG Fixed, optimization.md rules |

## Accomplishments

- **The common real-world shape is fast.** Servers and tools build text in functions and return it; that was the
  quadratic case.
- **The proof stays a proof.** Each new read class has a written soundness argument in codegen.cpp. The single
  appender is guaranteed by the unchanged writes rule.
- **The optimisation has a deterministic guard.** The harness check reads the generated C, so CI catches both a
  silent loss of the fast path and an unsafe admission.

## Task Commits

| Task | Commit | Type | Description |
|------|--------|------|-------------|
| Plan | `878a009` | docs | Plan 03-01 |
| Tasks 1+2: proof + case + check | `2a2e17c` | fix | analyze_text_cats read rules; txt_build_amortized; text_build_grow |
| Task 3: docs | `10a47c4` | docs | known-issues #9 removed; optimization.md rules; counts; CHANGELOG |

Plan metadata: committed with this SUMMARY.

## Files Created/Modified

| File | Change | Purpose |
|------|--------|---------|
| `src/codegen.cpp` | Modified | CatPos/CatRead, statement indices and loop ids; non-retaining exemptions; terminal check; soundness comment |
| `tests/cases/txt_build_amortized.{cst,out}` | Created | Builders that grow in place + sharing that must copy |
| `tests/run_tests.sh` | Modified | Extra check (k) text_build_grow; header lists twelve checks |
| `docs/reference/known-issues.md` | Modified | #9 removed; intro notes the fix |
| `docs/performance/optimization.md` | Modified | Wave 4: eligibility rules and what still copies |
| `docs/development/building-and-testing.md`, `README.md`, `CLAUDE.md` | Modified | 378 = 366 + 12; text_build_grow row |
| `CHANGELOG.md` | Modified | Unreleased → Fixed |

## Decisions Made

| Decision | Rationale | Impact |
|----------|-----------|--------|
| Widen the compiler's proof instead of adding a runtime high-water mark | The runtime append was correct; only admission was too strict | No runtime/ABI change; outputs provably identical |
| Keep the writes rule | It guarantees a single appender per grow-flagged buffer | `s = prefix` builders still copy (documented) |

## Deviations from Plan

### Summary

| Type | Count | Impact |
|------|-------|--------|
| Auto-fixed | 1 | Tooling only |
| Scope additions | 0 | — |
| Deferred | 1 (optional follow-up) | — |

**Total impact:** none — executed as planned.

### Auto-fixed Issues

**1. [Tooling] The A/B script miscounted generated-C sites**
- `grep -c … || echo 0` printed `0` twice when a file had no match, so the counter reported 0 cases.
- **Fix:** recounted from the saved run (`${c:-0}`). 2 cases gained sites, none lost one. Test results were
  unaffected.

### Deferred Items

- Possible follow-up (not logged as an issue): relax the writes rule, so that `s = prefix` builders, or a slot
  written from another value, can also grow in place. That needs either a runtime tip/high-water mark or an
  escape analysis of the source value.

## Issues Encountered

| Issue | Resolution |
|-------|------------|
| A hook rewrote a grep-based Bash call (auto-mode had no verdict) | Read the AST with the Grep tool instead |
| PowerShell mangled an inline `bash -c` with `$?` | Script files in the scratchpad (known pattern) |

## Next Phase Readiness

**Ready:**
- 03-02 (#7 format regrouping, #8 cache-less temp cleanup, #10 `fvec %` TypeError) on the same branch. The A/B and
  probe tooling carries over.

**Concerns:**
- `main` and GitHub are now further behind (Phase 2 + 03-01 local only; D-9).

**Blockers:**
- None

---
*Built with PAUL Framework v1.4 · https://chrisai.cv/skool · https://youtube.com/@chris-ai-systems*
*Phase: 03-fix-known-issues, Plan: 01*
*Completed: 2026-11-06*
