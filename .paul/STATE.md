---
description: "cstar — current position and accumulated context"
type: ProjectState
about: "cstar"
---

# Project State

## Project Reference

See: .paul/PROJECT.md (updated 2026-11-05 after Phase 2)

**Core value:** Write code as freely as JavaScript or Python and get speed that matches or beats C, on Windows and Linux.
**Current focus:** v0.3 cstar Foundation — Phase 3: Fix known issues

## Current Position

Milestone: v0.3 cstar Foundation (v0.3.0)
Phase: 3 of 4 (Fix known issues) — In Progress
Plan: 03-01 complete
Status: Ready for next PLAN (03-02: #7, #8, #10)
Last activity: 2026-11-06 — Unified 03-01 (known issue #9 fixed; text built in functions is amortized)

Progress:
- Milestone: [████████░░] 82% (9 of 11 plans; 2 of 4 phases)
- Phase 3: [█████░░░░░] 50% (1 of 2 plans)

## Loop Position

Current loop state:
```
PLAN ──▶ APPLY ──▶ UNIFY
  ✓        ✓        ✓     [Loop complete - ready for next PLAN]
```

## Accumulated Context

### Decisions

| Decision | Phase | Impact |
|----------|-------|--------|
| Rename to cstar; milestone order by priority (see ROADMAP.md) | Init | Sets all future work |
| Build the Windows checkout from WSL (`wsl -d Ubuntu-24.04 -- …`); long scratch work goes under `/root` (WSL clears `/tmp` when it idles out) | Phase 1–2 | Every build/test command goes through WSL |
| LF everywhere; test expectations `-text` (byte-exact) | Phase 1 | Never convert line endings; `.out` files are stored exactly |
| One branch per phase, a commit per task; no push without approval (repo is public) | Phase 1 | `main` changes only when you ask |
| Legacy names (`pentest`, `.pen`/`.pent`, `$PENTEST_PATH`, editor `pentest.*`) work until v0.4 | Phase 2 | Remove them in v0.4, together with D-11 |
| Internal names are `cstar_rt.{h,c}`, `cs_`/`CS_`, `namespace cstar`, `__csm<id>_` | Phase 2 | Phase 3 runtime work uses these names |
| When writing the SPEC exposes a bug, SPEC states the intended rule and the bug becomes a known issue (`fvec %` → #10) | Phase 2 | Phase 3 fixes #10 to match SPEC §5.11.9 |
| Human-verify checkpoints are automated by Claude where possible | Phase 2 | Fewer manual steps for you |
| 2026-11-06: no `Co-Authored-By` trailer on commits (removed from history at your request) | Phase 3 | Commit messages carry no Claude attribution |
| 2026-11-06: #9 fixed by widening the codegen ownership proof (non-retaining + terminal reads), runtime unchanged; the writes rule stays strict | Phase 3 | `s = prefix` builders still copy (documented) |

Full decision log: PROJECT.md → Key Decisions.

### Deferred Issues

| Issue | Origin | Effort | Revisit |
|-------|--------|--------|---------|
| Known issue #10: `%` on an `fvec` returns wrong values (should be a TypeError) | 02-05 | S | Phase 3, plan 03-02 |
| D-9: CI not yet run on GitHub (branch not pushed); GitHub `main` still has the broken `file_line_edges.out` | 01-03 | S | Whenever you approve a push |
| D-10: keep the real-VS-Code end-to-end test (45 checks) in the repo as a local-only check | 02-03 | S | Optional; any editor plan |
| D-11: benchmark tooling (tools/hpcbench, bench/ runners, generators and workloads) still uses `.pen` and `build/pentest` (both work until v0.4) | 02-04/02-05 | S | Before the v0.4 legacy removal |
| D-12: the membership TypeError names the retired `->` operator (`unsupported operand types for ->: …`); normative in m2a/m2b + 7 test expectations — changing it to `in` means contract + SPEC + tests together | 02-05 | S | When convenient (a deliberate wording decision) |
| Auto-GPU float policy; memory-access scope; "freely code in app" meaning | Init | S | v0.8 / v0.5 / v0.4 planning |

### Blockers/Concerns

- Phase 2: `main` (local and on GitHub) is two phases behind the local branch, and CI has never run on
  GitHub (D-9). Nothing is pushed until you decide to merge and push.

### Git State

Last commit: 10a47c4 docs(03-01): close known issue #9 (+ plan metadata commit)
Branch: phase-3-known-issues (from phase-2-rename; local only)
main: 6c82d72 (local and remote, unchanged)
Feature branches merged: none (merge into main and push only when you ask)

## Session Continuity

Last session: 2026-11-06
Stopped at: Plan 03-01 unified
Next action: /paul:plan for 03-02 (#7 format regrouping, #8 cache-less temp cleanup, #10 fvec %)
Resume file: .paul/phases/03-fix-known-issues/03-01-SUMMARY.md

---
*STATE.md — Updated after every significant action*
