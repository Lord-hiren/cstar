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
Phase: 2 of 4 (Rename to cstar) — In progress
Plan: 02-01 complete (1 of 4 in phase)
Status: Ready for next PLAN (02-02)
Last activity: 2026-11-05 — 02-01 unified: cstar CLI; make test 376/0

Progress:
- Milestone: [████░░░░░░] 40% (4 of 10 plans; 1 of 4 phases)
- Phase 2: [██░░░░░░░░] 25% (1 of 4 plans)

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
| Build the Windows checkout from WSL (`wsl -d Ubuntu-24.04 -- …`) | Phase 1 | Every build/test command goes through WSL |
| LF everywhere; test expectations `-text` (byte-exact) | Phase 1 | Never convert line endings; `.out` files are stored exactly |
| `examples/` stays private; docs use inline programs | Phase 1 | No links into `examples/` or `extras/` |
| One branch per phase, a commit per task; no push without approval (repo is public) | Phase 1 | `main` changes only when you ask |
| 2026-11-05: source extension `.cst`; old names (`.pen`/`.pent`, `PENTEST_PATH`, `pentest` command) keep working until v0.4 | Phase 2 | Rename lands in small steps; nothing breaks |
| 2026-11-05: `index.cst` is the default entry (`cstar run`/`build`/`check` with no file); build output = source name without extension (`hello.cst` → `hello`) | Phase 2 | No more `.pentest` output files |
| 2026-11-05: rename internals too (C++ namespace, `pt_` prefixes, `pentest_rt.*`) | Phase 2 | Own plan (02-03); Phase 2 grows to 4 plans |

Full decision log: PROJECT.md → Key Decisions.

### Deferred Issues

| Issue | Origin | Effort | Revisit |
|-------|--------|--------|---------|
| D-6: SPEC lacks `fvec`/`ivec`/`fvec_fill`/`ivec_fill`/`dot`; says "seventy" (code has 78) | 01-02 | M | With 02-03 (SPEC) or its own plan |
| D-7: VSCode extension `RESERVED` lacks the 5 vector builtins | 01-02 | S | 02-02 (editor extension) |
| D-8: docs/history headers say "current suite is 368 checks" | 01-02 | S | When the rename touches history docs |
| D-9: CI not yet run on GitHub (branch not pushed); GitHub `main` still has the broken `file_line_edges.out` | 01-03 | S | Whenever you approve a push |
| Auto-GPU float policy; memory-access scope; "freely code in app" meaning | Init | S | v0.8 / v0.5 / v0.4 planning |

### Blockers/Concerns

None.

### Git State

Last commit: 7bdf09a docs(02-01): SPEC CLI sections, CLAUDE.md and changelog
Branch: phase-2-rename (from phase-1-dev-environment; 10 commits ahead of main, local only)
main: 6c82d72 (local and remote, unchanged)
Feature branches merged: none (merge into main and push only when you ask)

## Session Continuity

Last session: 2026-11-05
Stopped at: Plan 02-01 loop closed (SUMMARY written)
Next action: /paul:plan for 02-02 (language name in messages, test files → .cst, editor extension + D-7)
Resume file: .paul/phases/02-rename-to-cstar/02-01-SUMMARY.md

---
*STATE.md — Updated after every significant action*
