---
description: "cstar — completed milestone log"
type: Milestones
about: "cstar"
---

# Milestones

Completed milestone log for this project.

| Milestone | Completed | Duration | Stats |
|-----------|-----------|----------|-------|
| v0.3 cstar Foundation (0.3.0) | 2026-11-17 | 9 days to the release (2026-11-04 → 2026-11-13), closed 2026-11-17 | 4 phases, 11 plans |

---

## ✅ v0.3 cstar Foundation

**Completed:** 2026-11-17 (released as cstar 0.3.0 on 2026-11-13, tag `v0.3.0`)
**Duration:** 9 days of work, 2026-11-04 → 2026-11-13; closed out 2026-11-17

### Stats

| Metric | Value |
|--------|-------|
| Phases | 4 (dev environment, rename, known issues, release) |
| Plans | 11 (3 + 5 + 2 + 1) |
| Commits | 48 since `main` @ `6c82d72` (46 up to the release commit `d31f120`) |
| Key files (from plan summaries) | 60 |
| Repository diff to the release | 522 files, +14,946 / −53,368 (most of the deletions are build output that stopped being tracked) |
| Test suite | 372 checks (365 cases + 7) → **381** (368 cases + 13); smoke 1294 → **1363**; docs 310 links + 53 samples |
| Known issues | 4 open → **0** |

### Key Accomplishments

- **A working development loop on this machine.** cstar builds and tests from Windows through WSL. The repository is
  LF-only and byte-exact, build output is no longer tracked, and a CI workflow runs four gates: warning-free build,
  tests, editor smoke test, docs checker. CI is green on GitHub `main`.
- **Pentest became cstar everywhere**: the `cstar` CLI, `.cst` sources, `index.cst` as the default entry,
  extension-less `build` output, `$CSTAR_PATH`, the cache folder, error messages, the VS Code extension
  (`cstar-lang.cstar-lang`), internal names (`cstar_rt`, `cs_`), SPEC and every doc.
  - An A/B comparison proved it a pure rename: identical outputs and identical generated C apart from names.
  - The legacy names keep working until v0.4.
- **The SPEC caught up with the code.** §5.11.9 specifies the SIMD vectors, all 78 builtins are counted, and every
  remaining `format` deviation from Python is listed (§5.11.5 item 16).
- **Text building is linear.** `s += x` inside a function grows in place: 400,000 appends went from 4.9 s to
  0.011 s, with 366/366 outputs byte-identical.
- **`format()` matches Python.** A 5,705-pair differential found #7 to be one of seven defects in the same padding
  step. One CPython port fixed them all, so code points count and a precision cut can no longer produce invalid
  UTF-8.
- **No open known issues.**
  - A run without a cache leaves nothing in `$TMPDIR` (`fexecve`, same process model).
  - `fvec %` raises the scalar `TypeError`.
  - Every fix has a regression test shown to fail on the old build.
- **cstar 0.3.0 released.** The version string, SPEC and docs describe it as released, and the CHANGELOG has its
  `[0.3.0]` section. The annotated tag `v0.3.0` points at the gated release commit. GitHub `main` was fast-forwarded
  to it, and its first CI run passed.
- **The editor extension was checked in a real VS Code** (45 end-to-end checks), not only by the smoke test.

### Key Decisions

| Decision | Date |
|----------|------|
| Rename to cstar; milestone order rename → SDK → interop → server → packages → GPU → training → rules | 2026-11-04 |
| Build from WSL; LF everywhere; byte-exact test expectations; one branch per phase, push only with your approval | 2026-11-04 |
| `.cst`, `index.cst`, extension-less build output; legacy names until v0.4; internal names renamed too | 2026-11-05 |
| When writing the SPEC exposes a bug, SPEC states the rule and the bug becomes a known issue | 2026-11-05 |
| Human-verify checkpoints are automated where possible | 2026-11-05 |
| No `Co-Authored-By` trailer in commits (history rewritten) | 2026-11-06 |
| Performance fixes widen compiler proofs before adding runtime machinery | 2026-11-06 |
| Parity bugs are measured with a differential against the reference implementation first | 2026-11-11 |
| Values that are neither numbers nor text format as their display text (bool, None, collections) | 2026-11-11 |
| A cache-less run unlinks its binary and `fexecve`s it | 2026-11-11 |
| A release is a gated commit + annotated tag; publishing is a separate decision, fast-forward only | 2026-11-13 |
| Version sweeps keep historical feature tags such as `(0.2.1)` | 2026-11-13 |

### Carried Forward

- D-10: keep the real-VS-Code end-to-end test in the repo as a local-only check (optional).
- D-11: the benchmark tooling still uses `.pen` / `build/pentest`; fix before the v0.4 legacy removal.
- D-12: the membership `TypeError` still names the retired `->` operator.
- D-13: format error parity, documented in SPEC §5.11.5 item 16.
- The GitHub Release page for `v0.3.0`, which you create (notes prepared).

Archive: [milestones/v0.3.0-ROADMAP.md](milestones/v0.3.0-ROADMAP.md)

---
