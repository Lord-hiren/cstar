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
Phase: 1 of 4 (Dev environment & repo hygiene) — Applying
Plan: 01-02 approved, executing
Status: Applying 01-02
Last activity: 2026-11-04 — Plan 01-02 approved; APPLY started

Progress:
- Milestone: [█░░░░░░░░░] 11% (1 of 9 plans)
- Phase 1: [███░░░░░░░] 33% (1 of 3 plans)

## Loop Position

Current loop state:
```
PLAN ──▶ APPLY ──▶ UNIFY
  ✓        ◉        ○     [Applying 01-02]
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

### Deferred Issues

| Issue | Origin | Effort | Revisit |
|-------|--------|--------|---------|
| Source extension: `.cstar` or `.cst`? | Init | S | Phase 2 planning |
| Auto-GPU float policy: exact-only by default (recommended) or fast? | Init | S | v0.8 planning |
| Memory access: own process only, or other processes too? | Init | S | v0.5 planning |
| What "freely code in app" means for the SDK (REPL? embedding?) | Init | S | v0.4 planning |
| D-1..D-5: broken `examples/` links in SPEC, stale counts, "no third-party libs" claim, tracked `build/`, ignored `examples/` | 01-01 | S | Planned in 01-02 |
| D-6: SPEC doesn't document `fvec`/`ivec`/`fvec_fill`/`ivec_fill`/`dot` and still says "seventy" builtins (code has 78) | 01-02 planning | M | Own plan (normative SPEC addition), at the latest with 02-03 |

### Blockers/Concerns

| Blocker | Impact | Resolution Path |
|---------|--------|-----------------|
None. (Resolved in 01-01: WSL Ubuntu-24.04 toolchain installed; working tree converted to LF with `.gitattributes`.)

## Session Continuity

Last session: 2026-11-04
Stopped at: Plan 01-02 created
Next action: Review and approve plan, then run /paul:apply .paul/phases/01-dev-environment-repo-hygiene/01-02-PLAN.md
Resume file: .paul/phases/01-dev-environment-repo-hygiene/01-02-PLAN.md

---
*STATE.md — Updated after every significant action*
