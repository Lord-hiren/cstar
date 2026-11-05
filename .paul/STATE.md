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
Phase: 3 of 4 (Fix known issues) — Not started
Plan: Not started
Status: Ready to plan
Last activity: 2026-11-05 — Phase 2 complete (5/5 plans), transitioned to Phase 3

Progress:
- Milestone: [███████░░░] 73% (8 of 11 plans; 2 of 4 phases)
- Phase 3: [░░░░░░░░░░] 0% (0 of 2 plans)

## Loop Position

Current loop state:
```
PLAN ──▶ APPLY ──▶ UNIFY
  ○        ○        ○     [Ready to plan Phase 3]
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

Last commit: ce83ac3 docs(02-05): guides, README and doc tools say cstar (+ metadata commits c6d229a and the Phase 2 transition commit)
Branch: phase-2-rename (from phase-1-dev-environment; local only); Phase 3 gets its own branch from here
main: 6c82d72 (local and remote, unchanged)
Feature branches merged: none (merge into main and push only when you ask)

## Session Continuity

Last session: 2026-11-05
Stopped at: Phase 2 complete, ready to plan Phase 3
Next action: /paul:plan for Phase 3 (03-01 in-place text growth #9; 03-02 #7, #8, #10)
Resume file: .paul/ROADMAP.md

---
*STATE.md — Updated after every significant action*
