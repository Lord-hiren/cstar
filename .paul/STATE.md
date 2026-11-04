---
description: "cstar — current position and accumulated context"
type: ProjectState
about: "cstar"
---

# Project State

## Project Reference

See: .paul/PROJECT.md (updated 2026-11-04 after Phase 1)

**Core value:** Write code as freely as JavaScript or Python and get speed that matches or beats C, on Windows and Linux.
**Current focus:** v0.3 cstar Foundation — Phase 2: Rename to cstar

## Current Position

Milestone: v0.3 cstar Foundation (v0.3.0)
Phase: 2 of 4 (Rename to cstar)
Plan: Not started
Status: Ready to plan
Last activity: 2026-11-04 — Phase 1 complete (3/3 plans), transitioned to Phase 2

Progress:
- Milestone: [███░░░░░░░] 33% (3 of 9 plans; 1 of 4 phases)
- Phase 2: [░░░░░░░░░░] 0%

## Loop Position

Current loop state:
```
PLAN ──▶ APPLY ──▶ UNIFY
  ○        ○        ○     [Ready for first PLAN of Phase 2]
```

## Accumulated Context

### Decisions

| Decision | Phase | Impact |
|----------|-------|--------|
| Rename to cstar; milestone order by priority (see ROADMAP.md) | Init | Sets all future work |
| Build the Windows checkout from WSL (`wsl -d Ubuntu-24.04 -- …`) | Phase 1 | Every build/test command goes through WSL |
| LF everywhere; test expectations `-text` (byte-exact) | Phase 1 | Never convert line endings; `.out` files are stored exactly |
| `examples/` stays private; docs use inline programs | Phase 1 | No links into `examples/` or `extras/` |
| One branch per phase, a commit per task; no push without approval (repo is public) | Phase 1 | `main` changes only when you ask |

Full decision log: PROJECT.md → Key Decisions.

### Deferred Issues

| Issue | Origin | Effort | Revisit |
|-------|--------|--------|---------|
| Source extension: `.cstar` or `.cst`? `.pen` transition? Rename internal `pt_` prefixes? | Init | S | **02-01 planning (now)** |
| D-6: SPEC lacks `fvec`/`ivec`/`fvec_fill`/`ivec_fill`/`dot`; says "seventy" (code has 78) | 01-02 | M | With 02-03 (SPEC) or its own plan |
| D-7: VSCode extension `RESERVED` lacks the 5 vector builtins | 01-02 | S | 02-02 (editor extension) |
| D-8: docs/history headers say "current suite is 368 checks" | 01-02 | S | When the rename touches history docs |
| D-9: CI not yet run on GitHub (branch not pushed); GitHub `main` still has the broken `file_line_edges.out` | 01-03 | S | Whenever you approve a push |
| Auto-GPU float policy; memory-access scope; "freely code in app" meaning | Init | S | v0.8 / v0.5 / v0.4 planning |

### Blockers/Concerns

None.

### Git State

Last commit: (Phase 1 transition commit, see `git log -1`)
Branch: phase-1-dev-environment (8 commits ahead of main, local only)
main: 6c82d72 (local and remote, unchanged)
Feature branches merged: none (merge into main and push only when you ask)

## Session Continuity

Last session: 2026-11-04
Stopped at: Phase 1 complete, ready to plan Phase 2
Next action: /paul:plan for Phase 2 (02-01: naming decisions and driver/CLI rename)
Resume file: .paul/ROADMAP.md

---
*STATE.md — Updated after every significant action*
