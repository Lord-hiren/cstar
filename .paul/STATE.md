---
description: "cstar — current position and accumulated context"
type: ProjectState
about: "cstar"
---

# Project State

## Project Reference

See: .paul/PROJECT.md (updated 2026-11-13 after Phase 3)

**Core value:** Write code as freely as JavaScript or Python and get speed that matches or beats C, on Windows and Linux.
**Current focus:** v0.3 cstar Foundation — Phase 4: Release v0.3.0

## Current Position

Milestone: v0.3 cstar Foundation (v0.3.0)
Phase: 4 of 4 (Release v0.3.0) — Not started
Plan: Not started
Status: Ready to plan
Last activity: 2026-11-13 — Phase 3 complete (no open known issues), transitioned to Phase 4

Progress:
- Milestone: [█████████░] 91% (10 of 11 plans; 3 of 4 phases)
- Phase 4: [░░░░░░░░░░] 0% (0 of 1 plans)

## Loop Position

Current loop state:
```
PLAN ──▶ APPLY ──▶ UNIFY
  ○        ○        ○     [Ready for next PLAN — Phase 4]
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
| 2026-11-11 (03-02 plan): #7 is fixed together with the format padding cluster a Python differential found (one CPython-ported layout; code-point width); bool keeps formatting as text (own type) | Phase 3 | format() matches Python on its whole subset; deviations listed in SPEC §5.11.5 |
| 2026-11-11 (03-02 plan): #8 via unlink + `fexecve` of the cache-less binary — no fork, the process model is unchanged | Phase 3 | `run` leaves nothing in `$TMPDIR`; the v0.4 Windows driver needs its own approach |
| 2026-11-11 (03-02 apply): a value that is neither a number nor text (bool, None, collections) formats exactly like its display text, `0` flag included (`format(True, "05")` → `True0`) | Phase 3 | SPEC §5.11.5 item 16 lists every format deviation |

Full decision log: PROJECT.md → Key Decisions.

### Deferred Issues

| Issue | Origin | Effort | Revisit |
|-------|--------|--------|---------|
| D-9: CI not yet run on GitHub (branch not pushed); GitHub `main` still has the broken `file_line_edges.out` | 01-03 | S | Whenever you approve a push |
| D-10: keep the real-VS-Code end-to-end test (45 checks) in the repo as a local-only check | 02-03 | S | Optional; any editor plan |
| D-11: benchmark tooling (tools/hpcbench, bench/ runners, generators and workloads) still uses `.pen` and `build/pentest` (both work until v0.4) | 02-04/02-05 | S | Before the v0.4 legacy removal |
| D-13: format error parity — cstar accepts a sign/`=`/`,` on text specs and a sign on `c` (Python raises), and rejects `,` with `e`/`g`/`%` (Python allows); documented as SPEC §5.11.5 item 16 (with bool/None-as-text and the shorter malformed-spec messages) | 03-02 | S | When convenient (decide: match Python or keep) |
| D-12: the membership TypeError names the retired `->` operator (`unsupported operand types for ->: …`); normative in m2a/m2b + 7 test expectations — changing it to `in` means contract + SPEC + tests together | 02-05 | S | When convenient (a deliberate wording decision) |
| Auto-GPU float policy; memory-access scope; "freely code in app" meaning | Init | S | v0.8 / v0.5 / v0.4 planning |

### Blockers/Concerns

- Phase 4: `main` (local and on GitHub) is three phases behind the local branches, and CI has never run on
  GitHub (D-9). Tagging v0.3.0 on GitHub means merging and pushing — only when you approve it.

### Git State

Last commit: the Phase 3 transition commit (after 8f9160a docs(03-02): complete plan 03-02)
Branch: phase-3-known-issues (from phase-2-rename; local only)
main: 6c82d72 (local and remote, unchanged)
Feature branches merged: none (merge into main and push only when you ask)

## Session Continuity

Last session: 2026-11-13
Stopped at: Phase 3 complete, ready to plan Phase 4
Next action: /paul:plan for Phase 4 (04-01: version bump, changelog, tag)
Resume file: .paul/ROADMAP.md

---
*STATE.md — Updated after every significant action*
