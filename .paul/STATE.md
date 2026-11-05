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
Phase: 2 of 4 (Rename to cstar) — Planning
Plan: 02-04 created, awaiting approval
Status: PLAN created, ready for APPLY
Last activity: 2026-11-05 — Created .paul/phases/02-rename-to-cstar/02-04-PLAN.md

Progress:
- Milestone: [█████░░░░░] 55% (6 of 11 plans; 1 of 4 phases)
- Phase 2: [██████░░░░] 60% (3 of 5 plans)

## Loop Position

Current loop state:
```
PLAN ──▶ APPLY ──▶ UNIFY
  ✓        ○        ○     [Plan 02-04 created, awaiting approval]
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
| 2026-11-05: rename internals too (C++ namespace, `pt_` prefixes, `pentest_rt.*`) | Phase 2 | Own plan (02-04); Phase 2 has 5 plans |
| 2026-11-05: editor extension is `cstar-lang.cstar-lang`, one language id `cstar` for `.cst`/`.pen`/`.pent`; `pentest.*` settings and command ids honoured until v0.4 | Phase 2 | Existing editor setups keep working |
| 2026-11-05: human-verify checkpoints are automated by Claude where possible (e.g. a real VS Code test instance) | Phase 2 | Fewer manual steps for you |

Full decision log: PROJECT.md → Key Decisions.

### Deferred Issues

| Issue | Origin | Effort | Revisit |
|-------|--------|--------|---------|
| D-6: SPEC lacks `fvec`/`ivec`/`fvec_fill`/`ivec_fill`/`dot`; says "seventy" (code has 78) | 01-02 | M | With 02-05 (docs/SPEC) or its own plan |
| D-8: docs/history headers say "current suite is 368 checks" | 01-02 | S | When the rename touches history docs |
| D-9: CI not yet run on GitHub (branch not pushed); GitHub `main` still has the broken `file_line_edges.out` | 01-03 | S | Whenever you approve a push |
| D-10: keep the real-VS-Code end-to-end test (45 checks) in the repo as a local-only check | 02-03 | S | Optional; any editor plan |
| D-11: tools/hpcbench still uses `.pen` workloads and `build/pentest` (both work until v0.4) | 02-04 planning | S | Before the v0.4 legacy removal |
| Auto-GPU float policy; memory-access scope; "freely code in app" meaning | Init | S | v0.8 / v0.5 / v0.4 planning |

### Blockers/Concerns

None.

### Git State

Last commit: e701ee9 docs(02-03): install script, README and editor guide (+ plan metadata commit)
Branch: phase-2-rename (from phase-1-dev-environment; local only)
main: 6c82d72 (local and remote, unchanged)
Feature branches merged: none (merge into main and push only when you ask)

## Session Continuity

Last session: 2026-11-05
Stopped at: Plan 02-04 created
Next action: Review and approve plan, then run /paul:apply .paul/phases/02-rename-to-cstar/02-04-PLAN.md
Resume file: .paul/phases/02-rename-to-cstar/02-04-PLAN.md

---
*STATE.md — Updated after every significant action*
