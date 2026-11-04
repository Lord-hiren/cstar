---
description: "cstar — current position and accumulated context"
type: ProjectState
about: "cstar"
---

# Project State

## Project Reference

See: .paul/PROJECT.md (updated 2026-11-04)

**Core value:** Write code as freely as JavaScript or Python and get speed that matches or beats C, on Windows and Linux.
**Current focus:** v0.3 cstar Foundation — Phase 1: Dev environment & repo hygiene

## Current Position

Milestone: v0.3 cstar Foundation (v0.3.0)
Phase: 1 of 4 (Dev environment & repo hygiene) — In progress
Plan: 01-02 complete (2 of 3 in phase)
Status: Ready for next PLAN (01-03)
Last activity: 2026-11-04 — 01-02 unified: build/ untracked, docs corrected, check_docs 0 broken links

Progress:
- Milestone: [██░░░░░░░░] 22% (2 of 9 plans)
- Phase 1: [███████░░░] 67% (2 of 3 plans)

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
| Rename to cstar | Phase 2 | Every user-facing name changes |
| Milestone order by priority (see ROADMAP.md) | All | Sets the order of all future work |
| GPU/AI libraries loaded at runtime (proposed) | v0.8 | Binaries keep working without a GPU |
| Build the Windows checkout from WSL via `/mnt/c` | Phase 1 (01-01) | All commands go through `wsl -d Ubuntu-24.04 -- …` |
| LF line endings enforced by `.gitattributes` | Phase 1 (01-01) | Never convert back to CRLF |
| 2026-11-04: `examples/` stays private (git-ignored); docs reference no files in it (option C) | Phase 1 (01-02) | Examples in docs are inline code; SQLi/AI files are described, not linked |

### Deferred Issues

| Issue | Origin | Effort | Revisit |
|-------|--------|--------|---------|
| Source extension: `.cstar` or `.cst`? | Init | S | Phase 2 planning |
| Auto-GPU float policy: exact-only by default (recommended) or fast? | Init | S | v0.8 planning |
| Memory access: own process only, or other processes too? | Init | S | v0.5 planning |
| What "freely code in app" means for the SDK (REPL? embedding?) | Init | S | v0.4 planning |
| D-6: SPEC doesn't document `fvec`/`ivec`/`fvec_fill`/`ivec_fill`/`dot` and still says "seventy" builtins (code has 78) | 01-02 planning | M | Own plan (normative SPEC addition), at the latest with 02-03 |
| D-7: VSCode extension `RESERVED` lacks the 5 vector builtins (out of sync with `src/semantic.cpp`) | 01-02 | S | 02-02 (editor extension) |
| D-8: docs/history headers say "current suite is 368 checks" (records, left as is) | 01-02 | S | When the rename touches history docs |

### Blockers/Concerns

| Blocker | Impact | Resolution Path |
|---------|--------|-----------------|
None. (Resolved in 01-01: WSL Ubuntu-24.04 toolchain installed; working tree converted to LF with `.gitattributes`.)

## Session Continuity

Last session: 2026-11-04
Stopped at: Plan 01-02 loop closed (SUMMARY written; branch phase-1-dev-environment, main untouched)
Next action: /paul:plan for 01-03 (Linux CI). Note: CI needs the branch pushed to origin — ask first
Resume file: .paul/phases/01-dev-environment-repo-hygiene/01-02-SUMMARY.md

---
*STATE.md — Updated after every significant action*
