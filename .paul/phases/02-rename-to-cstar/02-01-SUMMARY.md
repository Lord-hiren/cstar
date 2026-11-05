---
phase: 02-rename-to-cstar
plan: 01
subsystem: cli
tags: [cli, rename, cstar, cst, index.cst, CSTAR_PATH, makefile, harness]

# Dependency graph
requires:
  - phase: 01-dev-environment-repo-hygiene
    provides: WSL toolchain, green baseline (372/0), CI workflow, fresh-clone rehearsal script
provides:
  - "`cstar` binary plus a `pentest` alias (same driver; alias until v0.4)"
  - ".cst canonical source extension; legacy .pen/.pent accepted until v0.4"
  - "index.cst default entry for run/build/check; extension-free build output"
  - "CSTAR_PATH module search (legacy PENTEST_PATH after it); .cst candidates first"
  - "cstar cache ($XDG_CACHE_HOME/cstar/<key>.bin) and cstar-* temp names"
  - "Harness: 11 CLI checks (4 new); SPEC §2/§10/§11.6/§14.2 describe the cstar CLI"
affects: [02-02 test files + editor, 02-03 internals, 02-04 docs, v0.4 legacy-name removal]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Legacy names stay as aliases until v0.4 (command, extensions, env var)"
    - "CLI probe script (scratchpad) + fresh-clone rehearsal as the verification pair for driver changes"

key-files:
  created: []
  modified:
    - Makefile
    - src/main.cpp
    - src/modules.cpp
    - tests/run_tests.sh
    - .github/workflows/ci.yml
    - SPEC.md
    - CLAUDE.md
    - CHANGELOG.md

key-decisions:
  - ".cst extension; legacy names until v0.4; index.cst default entry; extension-free output (user decisions 2026-11-05)"
  - "Driver and harness committed together so that every commit passes make test"

patterns-established:
  - "Every new CLI behaviour gets a harness extra-check, not just a probe"

# Metrics
duration: one session
completed: 2026-11-05T01:10:00Z
description: "The CLI is cstar: .cst sources, index.cst default entry, ./hello build output, CSTAR_PATH, cstar cache; pentest/.pen/.pent/PENTEST_PATH still work; make test 376/0"
type: Summary
about: "cstar"
---

# Phase 2 Plan 01: cstar CLI — Summary

**The command line is now `cstar`:**
- `.cst` sources, with `./index.cst` as the default entry
- `cstar build hello.cst` writes `./hello`
- modules come from `$CSTAR_PATH`; the cache is `~/.cache/cstar`

**Every old name still works until v0.4:** the `pentest` command, `.pen`/`.pent` files and `$PENTEST_PATH`.
`make test` gives 376/0 (365 cases + 11 CLI checks), with green gates in a fresh clone.

## Performance

| Metric | Value |
|--------|-------|
| Duration | One session |
| Completed | 2026-11-05 |
| Tasks | 3 of 3 (all auto) |
| Qualify results | 3 PASS, 0 GAP, 0 DRIFT |
| Commits | `d8659ee` (plan), `546944d` (driver + harness + CI), `7bdf09a` (SPEC + docs) |

## Acceptance Criteria Results

| Criterion | Status | Notes |
|-----------|--------|-------|
| AC-1: `cstar` command + `pentest` alias | ✅ Pass | Both print `cstar 0.2.1`; usage names `cstar`, `.cst` and `index.cst`; the alias runs programs (probe + `cli_alias` check) |
| AC-2: `.cst` canonical, legacy extensions work | ✅ Pass | run, bare form and check pass for `.cst`/`.pen`/`.pent`; `.pentest` is rejected with exit 2 and a message naming `.cst` |
| AC-3: `index.cst` default entry | ✅ Pass | run/check/build without a file use `./index.cst`, and build writes `./index`; missing file → exit 2 + message; bare `cstar` → usage, exit 2 |
| AC-4: Extension-free build output | ✅ Pass | `tools/greet.cst` → `./greet`; `hello.pent` → `./hello`; `lib.cst` next to `lib/` → exit 2 suggesting `-o`; `-o` still overrides |
| AC-5: Module search | ✅ Pass | `.cst` preferred over `.pen`; legacy `.pen` and `__init__.cst` resolve; CSTAR_PATH, PENTEST_PATH, and CSTAR_PATH first |
| AC-6: Cache and temp names | ✅ Pass | `$XDG_CACHE_HOME/cstar/<key>.bin`; no `pentest*` cache or temp leftovers |
| AC-7: Harness and CI green | ✅ Pass | `make test` 376/0, local and in a fresh clone; ci.yml reports `build/cstar --version`; smoke 1294/0; docs 315 links / 0 broken, 52 samples |
| AC-8: SPEC describes the cstar CLI | ✅ Pass | §2, §10, §11.6 and §14.2 updated; 89 headings before and after, none changed |

## Accomplishments

- **A complete CLI rename with zero breakage.** All 365 existing `.pen` test cases still pass unchanged
  through the legacy-extension path, and the old `pentest` command keeps working.
- **New conveniences:**
  - `cstar run`, `build` and `check` with no file use `./index.cst`, like Node's `index.js`.
  - Build output has no `.pentest` suffix.
  - A clear error replaces a confusing clang failure when the output name is an existing folder.
- **The harness now guards the CLI contract.** Eleven CLI checks cover every new behaviour, including the
  module-search order.

## Task Commits

| Task | Commit | Type | Description |
|------|--------|------|-------------|
| Plan + Phase 2 split | `d8659ee` | docs | Plan 02-01; ROADMAP: 4 plans in Phase 2 |
| Task 1 + Task 2: driver, Makefile, harness, CI | `546944d` | feat | One commit: see the deviation below |
| Task 3: SPEC, CLAUDE.md, CHANGELOG | `7bdf09a` | docs | SPEC §2/§10/§11.6/§14.2; Unreleased → Changed |

Plan metadata: committed with this SUMMARY.

## Files Created/Modified

| File | Change | Purpose |
|------|--------|---------|
| `Makefile` | Modified | `build/cstar` target; `build/pentest` alias (symlink, copy as fallback); `all`/`test` build both |
| `src/main.cpp` | Modified | Version/usage, `.cst` + legacy, `index.cst` default, extension-free output + directory check, cstar cache/temp names, clang-missing message |
| `src/modules.cpp` | Modified | CSTAR_PATH then PENTEST_PATH; `.cst` candidates first |
| `tests/run_tests.sh` | Modified | `build/cstar`; checks `cst_extension`, `legacy_extensions`, `cli_alias`, `index_entry`, `module_path`; `cstar-hash-*` |
| `.github/workflows/ci.yml` | Modified | Reports `build/cstar --version` |
| `SPEC.md` | Modified (wording) | §2, §10, §11.6, §14.2 |
| `CLAUDE.md`, `CHANGELOG.md` | Modified | Commands use `build/cstar`; Unreleased → Changed |

## Decisions Made

| Decision | Rationale | Impact |
|----------|-----------|--------|
| Extension `.cst`; legacy names until v0.4; `index.cst` default entry; extension-free output | Your choices (2026-11-05) | Recorded in STATE/PROJECT; implemented here |
| Bare `cstar` (no arguments) stays a usage error; only run/build/check default to `index.cst` | Keeps the SPEC §10 exit-2 contract; the decision was about `cstar run` | Documented in SPEC §10 |
| Cached programs are named `<key>.bin` | `.pentest` is gone; the cache needs some suffix | SPEC §11.6 |

## Deviations from Plan

### Summary

| Type | Count | Impact |
|------|-------|--------|
| Auto-fixed | 2 | Kept every commit green; one accuracy fix in the SPEC |
| Scope additions | 0 | — |
| Deferred | 0 new | Remaining old names are already scheduled (02-02 to 02-04) |

**Total impact:** no scope creep. All acceptance criteria were met as written.

### Auto-fixed Issues

**1. [Process] Tasks 1 and 2 in a single commit**
- **Found during:** Task 1
- **Issue:** The plan asked for one commit per task and for every commit to pass `make test`. Those
  conflict: the new driver makes the old harness check (d), which expected `script.pentest`, fail.
- **Fix:** The driver, Makefile, harness and CI changes went into one commit (`546944d`), which passes 376/0.

**2. [SPEC accuracy] The §10 compile-pipeline line now lists `-lm -lcurl`**
- The old text said `-lm` only; the driver links both. Wording only.

### Deferred Items

None new. The old names that remain are already scheduled:
- **02-02:** test files `.pen` → `.cst`; "Pentest" in runtime messages; the VSCode extension still calls
  `pentest` (works through the alias)
- **02-03:** internal names (`pentest::`, `pt_`, `pentest_rt.*`)
- **02-04:** docs; `tests/check_docs.py` and `bench/*` still use `build/pentest` (works through the alias)

## Issues Encountered

| Issue | Resolution |
|-------|------------|
| PowerShell 5.1 mangles inline `bash -c` quoting | Checks run from scripts in the scratchpad (probe-cli.sh, rebuild-test.sh, ci-rehearsal.sh) |

## Next Phase Readiness

**Ready:**
- The CLI and harness accept `.cst`. 02-02 can rename the 365 test files and their imports with
  `git mv`, and the harness only needs its glob changed.

**Concerns:**
- 02-02 changes normative error messages ("Pentest" → "cstar"), so code, SPEC quotes and `.err`/`.out`
  files must change together.

**Blockers:**
- None

---
*Built with PAUL Framework v1.4 · https://chrisai.cv/skool · https://youtube.com/@chris-ai-systems*
*Phase: 02-rename-to-cstar, Plan: 01*
*Completed: 2026-11-05*
