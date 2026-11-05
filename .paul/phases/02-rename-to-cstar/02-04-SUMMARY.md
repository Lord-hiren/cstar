---
phase: 02-rename-to-cstar
plan: 04
subsystem: compiler-runtime-internals
tags: [rename, refactor, a-b-proof, codegen, runtime, mangling, user-agent]

# Dependency graph
requires:
  - phase: 02-rename-to-cstar (02-03)
    provides: the extension's hover quotes the runtime file and the User-Agent (updated here)
provides:
  - "runtime/cstar_rt.{h,c} (history kept); cs_/CS_ prefixes, namespace cstar, __csm<id>_ mangling throughout"
  - "Byte-identical behaviour, proven: 365/365 outputs, 275/275 gen.c (reverse-mapped), 20/20 sources (reverse-mapped)"
  - "Code comments say cstar; http_get/http_post send User-Agent jev-pipeline/1.0 (cstar)"
  - "SPEC, contracts, internals, CLAUDE.md, README, bench, test comments use the new names; history docs carry a mapping note"
  - "D-8 closed (history notes no longer quote a stale suite count)"
affects: [02-05 docs wording, Phase 3 runtime work (#9 text growth now lands in cstar_rt.c), CLAUDE.md readers]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Rename proof: forward-map the tree, then reverse-map sources and generated C and diff them against the base build"
    - "Generated C composes prefixed names (cs_meta_cs_fn_x); the reverse map must also match after '_'"
    - "WSL clears /tmp when it idles out: keep long-running scratch under /root, or run everything in one session"

key-files:
  created:
    - runtime/cstar_rt.h (git mv from pentest_rt.h)
    - runtime/cstar_rt.c (git mv from pentest_rt.c)
  modified:
    - src/*.cpp, src/*.h
    - tools/embed.cpp
    - Makefile
    - SPEC.md
    - docs/contracts/*.md, docs/internals/*.md, docs/performance/optimization.md
    - docs/reference/*.md, docs/guide/cli.md, docs/guide/modules.md, docs/development/building-and-testing.md
    - docs/history/{README,progress-m2b,progress-m2c}.md, docs/performance/baseline-{m1,m2b}.md (notes)
    - README.md, CLAUDE.md, CHANGELOG.md
    - bench/run-m2b.sh, bench/run-m2c.sh, bench/gen-m2b.py, bench/cases/**/*.cpp, bench/SPEED.md
    - tests/cases/*.cst (7 files, comment lines only)
    - editors/vscode/src/extension.js

key-decisions:
  - "Prefix cs_/CS_ and mangling __csm: unused before, same length as pt_/PT_/__ptm"
  - "Historical records keep old names plus a one-line mapping note; frozen tools and recorded results untouched"

patterns-established:
  - "A mechanical rename lands as its own commit so a reverse-mapping diff can prove it pure"

# Metrics
duration: ~16min of commits (plan 16:55Z → task 3 17:09Z) plus three A/B runs
started: 2026-11-05T16:55:35Z
completed: 2026-11-05T17:15:00Z
description: "Internal names are cstar (cstar_rt, cs_/CS_, namespace cstar, __csm), proven a pure rename by A/B (365/365 outputs, 275/275 gen.c, 20/20 sources); comments and User-Agent say cstar; living references updated, history noted; D-8 closed"
type: Summary
about: "cstar"
---

# Phase 2 Plan 04: Internal names → cstar — Summary

**Contributors now read only cstar names.** The runtime is `runtime/cstar_rt.{h,c}`; code uses `cs_value`,
`CS_INT` and `namespace cstar`; module mangling is `__csm<id>_<name>`. About 6,700 identifiers were
renamed, and a byte-level A/B comparison against the pre-rename compiler shows no change in behaviour.

The only intended behaviour change is the HTTP `User-Agent`, now `jev-pipeline/1.0 (cstar)`. A local
server confirmed it end to end.

## Performance

| Metric | Value |
|--------|-------|
| Duration | ~16 min between plan and last task commit, plus three A/B runs (one lost, see Issues) |
| Completed | 2026-11-05 |
| Tasks | 3 of 3 (auto) |
| Qualify results | 3 PASS. Task 1 needed one tooling fix (composite reverse mapping) before the proof read clean; the code was right first time |
| Commits | `3f11e63` (plan), `8870201` (code rename), `6115fa9` (comments + UA), `6fb2b1f` (references) |
| Diff | 64 files, +5,252 / −5,232 |

## Acceptance Criteria Results

| Criterion | Status | Notes |
|-----------|--------|-------|
| AC-1: Code uses only the new internal names | ✅ Pass | `runtime/cstar_rt.{h,c}`; `git log --follow` reaches the initial commit. No old identifier in src/, runtime/, tools/embed.cpp or the Makefile. Every remaining `pentest` is legacy behaviour: the Makefile alias, the main.cpp alias comment, `PENTEST_PATH` |
| AC-2: The rename is provably pure (A/B) | ✅ Pass | A = base `3f11e63`, B = renamed tree. Outputs 365/365 byte-identical (stdout, stderr, exit). gen.c 275/275 identical after reverse mapping; the other 90 cases are compile-time errors (82 err_, 4 imp_, 3 rt_ arity/reserved, 1 varq_) with identical outputs. Sources 20/20 identical to the base after reverse mapping (Task 1 commit). Final tree vs A, after Tasks 2–3: 365/365 and 275/275 again |
| AC-3: Comments and the User-Agent say cstar | ✅ Pass | 34 comment lines. A local Python server saw `GET`/`POST User-Agent: jev-pipeline/1.0 (cstar)`. SPEC:1192 and the extension hover (2×) quote the same text |
| AC-4: Living references use the new names | ✅ Pass | 37 files mapped (508 `cs_`, 67 `CS_`, 51 `cstar_rt`, 9 `__csm`). Test comments only, no lines moved. History notes in 5 files. CHANGELOG Unreleased entry. Old names remain only in excluded/historical files |
| AC-5: All gates green | ✅ Pass | make warning-free; make test 376/0; smoke 1363/0; check_docs 318 links / 0 broken, 52 samples pass. Fresh clone of `6fb2b1f` in WSL: same results |

## Accomplishments

- **The largest rename of the phase landed with proof, not hope.** Every generated program behaves
  identically, and the compiler sources differ from the base only by the name mapping.
- **CLAUDE.md, SPEC and the internals docs now match the code**, so the next contributor's searches for
  `cs_value` or `cstar_rt.c` find both the code and its docs.
- **Phase 3 can start on final names.** Known issue #9 (text growth) will be written directly in
  `cstar_rt.c`.

## Task Commits

| Task | Commit | Type | Description |
|------|--------|------|-------------|
| Plan | `3f11e63` | docs | Plan 02-04 |
| Task 1: code rename | `8870201` | refactor | git mv runtime; mapping over src/ runtime/ embed Makefile; known-issues link |
| Task 2: comments + UA | `6115fa9` | refactor | 34 comment lines; User-Agent in runtime, SPEC and hover; extension comment |
| Task 3: references | `6fb2b1f` | docs | 37 living files; composite sample names; 3 stale facts; history notes (+D-8); CHANGELOG |

Plan metadata: committed with this SUMMARY.

## Files Created/Modified

| File | Change | Purpose |
|------|--------|---------|
| `runtime/cstar_rt.{h,c}` | Renamed + modified | Runtime under the cstar name; `cs_`/`CS_`; guard `CSTAR_RT_H`; UA |
| `src/*` (16 files) | Modified | `cs_` emission, `namespace cstar`, `__csm` mangling, embed names, comments |
| `tools/embed.cpp`, `Makefile` | Modified | `build/cstar_rt.o`, `cstar_rt.cpu`, runtime paths |
| `SPEC.md`, `docs/contracts/*` | Modified | Implementation-name references; m2a §10 heading (number unchanged); UA quote |
| `docs/internals/*`, `docs/performance/optimization.md`, `docs/reference/*`, guides | Modified | New names; 3 stale facts on edited lines |
| `docs/history/*`, `docs/performance/baseline-{m1,m2b}.md` | Modified (note only) | Old → new name mapping; D-8 |
| `CLAUDE.md`, `README.md`, `CHANGELOG.md` | Modified | Names; Unreleased entry |
| `bench/*` (sources, generator, m2b/m2c runners, SPEED.md) | Modified | Names in comments and flag descriptions |
| `tests/cases/*.cst` (7) | Modified (comments) | Names in "what this proves" comments |
| `editors/vscode/src/extension.js` | Modified | UA in hover; runtime-file comment |

## Decisions Made

| Decision | Rationale | Impact |
|----------|-----------|--------|
| `cs_`/`CS_`/`__csm`/`cstar_rt` | Unused before; same length as the old prefixes, so no arithmetic or alignment changes | Any future name with these prefixes is runtime-owned |
| History keeps old names + a note | CLAUDE.md rule: historical records get notes, not rewrites | Readers can map old names in one step |
| Fix factual errors on edited lines; leave prose wording to 02-05 | Avoid half-renamed files (e.g. bench "Pentest side" comments) | 02-05 has a clear list |

## Deviations from Plan

### Summary

| Type | Count | Impact |
|------|-------|--------|
| Auto-fixed | 3 | Doc accuracy; no code impact |
| Scope additions | 1 | D-8 closed while editing the same notes |
| Deferred | 0 new | 02-05 handoff items listed below |

**Total impact:** none on behaviour. The tooling fix only made the proof read correctly.

### Auto-fixed Issues

**1. [Docs] Composite names in the code-generation sample**
- **Found during:** Task 3 (composite-leftover scan)
- **Issue:** the word-boundary mapping turned `pt_meta_pt_fn_square` into `cs_meta_pt_fn_square`.
- **Fix:** 3 names set to `cs_mpn_cs_fn_square` / `cs_meta_cs_fn_square`, which is what the compiler now
  emits (seen in the A/B gen.c).
- **Commit:** `6fb2b1f`

**2. [Docs] Three stale facts on lines already being edited**
- The m1 contract said the runtime is "embedded in the `pentest` binary" → `cstar`.
- cli.md gave the temp folder as `$TMPDIR/pentest-XXXXXX` → `cstar-XXXXXX` (main.cpp:151).
- architecture.md said `build/pentest` carries the runtime → `build/cstar`.
- **Commit:** `6fb2b1f`

**3. [Tooling] The reverse mapping for generated C** (scratchpad only)
- Codegen joins prefixed names at run time, so the inner prefix follows `_`. A `reverse-gen` mode also
  matches there. It can only create false differences, never hide real ones.

### Scope additions

**1. D-8 closed:** the history notes said "the current suite is 368 checks". They now point to
building-and-testing (history README, progress-m2b, progress-m2c), in the same edit as the rename note.

### Deferred Items / handoff to 02-05

- Prose "Pentest" on lines this plan didn't otherwise need: the bench twins' "the Pentest side" /
  "Pentest runtime's", code-generation.md "Pentest's wrap/sign rules"
- The temp-folder name `pentest-XXXXXX` in getting-started.md:150 and known-issues.md #8 (lines 25, 71)
- Still open: D-6 (SPEC vector builtins), D-9 (CI on GitHub), D-10, D-11

## Issues Encountered

| Issue | Resolution |
|-------|------------|
| WSL idled out after the first 10-minute A/B run, and its reboot wiped `/tmp` (both the outputs and compiler A), so the first compare read missing files | Scratch moved to `/root`; build A, run and compare in one WSL session (`ab-all.sh`); the data persisted for later comparisons |
| PowerShell mangled an inline `bash -c` loop | Script files in the scratchpad, as before |
| Ledger timestamps for the 02-04 plan/apply entries were estimates (05:30Z/06:00Z) | Plan corrected to its commit time (16:55Z) in this UNIFY; apply (first set to an estimated 16:56Z) was re-set to the first task commit, 17:03:39Z, in 02-05's UNIFY |

## Next Phase Readiness

**Ready:**
- 02-05 (docs and SPEC wording) is the last Phase 2 plan. Code, internals and references are final; what
  remains is prose ("Pentest" in README/guides/SPEC wording), D-6, and the handoff list above.

**Concerns:**
- SPEC/contracts are normative, and 02-05 will touch a lot of prose there: keep section numbers and quoted
  messages exactly as they are.

**Blockers:**
- None

---
*Built with PAUL Framework v1.4 · https://chrisai.cv/skool · https://youtube.com/@chris-ai-systems*
*Phase: 02-rename-to-cstar, Plan: 04*
*Completed: 2026-11-05*
