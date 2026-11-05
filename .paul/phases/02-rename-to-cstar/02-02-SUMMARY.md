---
phase: 02-rename-to-cstar
plan: 02
subsystem: language-surface
tags: [rename, error-messages, spec, tests, cst, git-mv]

# Dependency graph
requires:
  - phase: 02-rename-to-cstar (02-01)
    provides: CLI accepts .cst; legacy_extensions harness check guards .pen/.pent
provides:
  - "The five user-visible messages name cstar (code + SPEC + guides + expectations together)"
  - "371 test programs renamed .pen → .cst with history kept (365 cases + 6 lib fixtures)"
  - "Harness runs tests/cases/*.cst; contributor docs describe .cst and the 11 extra checks"
affects: [02-03 editor extension (hover texts still say Pentest), 02-04 internals (comments), 02-05 docs]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Normative message change = code + SPEC quote + .out/.err + guide quote in ONE commit"
    - "Search test expectations with grep -a: some .out files are binary (-text) and normal search tools skip them"

key-files:
  created: []
  modified:
    - src/semantic.cpp
    - runtime/pentest_rt.c
    - SPEC.md
    - docs/guide/builtins.md
    - docs/guide/errors-and-diagnostics.md
    - tests/run_tests.sh
    - tests/cases/** (371 renames + 6 expectation edits)
    - docs/development/building-and-testing.md
    - .gitignore
    - CLAUDE.md
    - CHANGELOG.md

key-decisions:
  - "Language name in messages is lowercase `cstar` (matches the command and project name)"
  - "Fixture data `toolkit: pentest` (imp_module.out) is left alone: it isn't the product name"

patterns-established:
  - "Before claiming a message is untested, grep -a every tests/cases/*.out|.err (binary-safe)"

# Metrics
duration: one session
completed: 2026-11-05T02:10:00Z
description: "Error messages say cstar (5 messages, code + SPEC + tests together); all 371 test programs are .cst with history kept; make test 376/0, fresh clone green"
type: Summary
about: "cstar"
---

# Phase 2 Plan 02: Error messages + .cst tests — Summary

**Every user-visible error message that named the language now says `cstar`:**
- `'eval' is not available in cstar: …`
- `cstar supports only the UTF-8 encoding`
- `… (cstar does not support negative moduli)`
- `… (cstar strings are UTF-8)`

The code, SPEC quotes, guides and test expectations changed together. All 371 test programs were renamed
`.pen` → `.cst` with their git history kept. `make test` is 376/0, locally and in a fresh clone.

## Performance

| Metric | Value |
|--------|-------|
| Duration | One session |
| Completed | 2026-11-05 |
| Tasks | 2 of 2 (auto) |
| Qualify results | 2 PASS. Task 1 needed one fix loop: `bi_py_text.out` (see deviations) |
| Commits | `f685090` (plan), `8f3b997` (messages), `b41c737` (.cst tests) |

## Acceptance Criteria Results

| Criterion | Status | Notes |
|-----------|--------|-------|
| AC-1: Messages say cstar | ✅ Pass | Probe: bytes(), open(), pow() and chr() print the cstar messages; `check` on `eval("1")` prints the semantic message; no string literal in src/ or runtime/ contains "Pentest" |
| AC-2: SPEC, docs and expectations match | ✅ Pass | SPEC quotes at 970, 993, 1015, 1106, 1153, 1988; builtins.md; errors-and-diagnostics.md; 5 expectation files. 89 SPEC headings, unchanged |
| AC-3: Test files are .cst | ✅ Pass | 365 + 6 `.cst`; 0 tracked `.pen` under tests/; 371 renames (`git log --follow` reaches the initial commit); err_uncaught_raise.err expects `.cst:4:` |
| AC-4: Harness runs .cst and stays green | ✅ Pass | 376/0, local and fresh clone (`b41c737`); `legacy_extensions` still covers .pen/.pent; smoke 1294/0; docs 315 links / 0 broken, 52 samples |

## Accomplishments

- **Nothing a user sees in an error message says Pentest any more**, and the normative SPEC quotes
  agree word for word.
- **The test suite, the repo's biggest body of example code, is now written in `.cst`**, with no lost
  history.
- **The contributor guide matches the harness again:** the 11 extra checks are listed, with the 376 count.

## Task Commits

| Task | Commit | Type | Description |
|------|--------|------|-------------|
| Plan | `f685090` | docs | Plan 02-02; Phase 2 split into 5 plans |
| Task 1: messages | `8f3b997` | feat | 5 messages; SPEC ×6 quotes; 2 guides; 5 expectation files |
| Task 2: .cst tests | `b41c737` | test | 371 `git mv`; harness; .gitignore; CLAUDE.md; building-and-testing; CHANGELOG |

Plan metadata: committed with this SUMMARY.

## Files Created/Modified

| File | Change | Purpose |
|------|--------|---------|
| `src/semantic.cpp` | Modified | eval/exec/compile message |
| `runtime/pentest_rt.c` | Modified | UTF-8 (×2), pow, chr messages |
| `SPEC.md` | Modified (quotes only) | §5.11 rows, deviations list, §5.11.6 block, §9 table |
| `docs/guide/builtins.md`, `errors-and-diagnostics.md` | Modified | Quoted eval message |
| `tests/cases/{bi_py_bytes,bi_py_files,bi_py_numeric_errors,bi_py_text}.out`, `err_bi_py_unavailable.err` | Modified | New message text |
| `tests/cases/**/*.pen` → `*.cst` (371) | Renamed | Test programs and module fixtures |
| `tests/cases/err_uncaught_raise.err` | Modified | Path `.cst:4:` |
| `tests/run_tests.sh` | Modified | `*.cst` glob; `.cst` paths in the extra checks; `lib/broken.cst` |
| `docs/development/building-and-testing.md` | Modified | `.cst` paths; 11 extra checks; 376 = 365 + 11 |
| `.gitignore`, `CLAUDE.md`, `CHANGELOG.md` | Modified | `tests/*.cst` scratch; `.cst` paths; Unreleased → Changed |

## Decisions Made

| Decision | Rationale | Impact |
|----------|-----------|--------|
| Lowercase `cstar` in messages | Consistent with the command and project name | All future messages follow it |
| Leave `imp_module.out`'s `toolkit: pentest` | It's fixture data printed by a test module, not the product name | None |

## Deviations from Plan

### Summary

| Type | Count | Impact |
|------|-------|--------|
| Auto-fixed | 2 | Essential: plan facts were incomplete |
| Scope additions | 1 | Same doc, same topic |
| Deferred | 0 new | — |

**Total impact:** small. Two planning facts were wrong or incomplete; both were caught by the gates and fixed
inside the plan's intent.

### Auto-fixed Issues

**1. [Planning fact] The chr() surrogate message *is* tested, in `bi_py_text.out`**
- **Found during:** Task 1 verification (`make test` → 375/376, `bi_py_text` failed)
- **Issue:** The plan said the chr message wasn't covered by any test. `bi_py_text.out` contains binary
  bytes, git marks it `-text`, and the search tools skip binary files, so it never showed up.
- **Fix:** `sed` on line 15 only (the file shrank by exactly 2 bytes, Pentest → cstar). A binary-safe
  `grep -a` scan of all `-text` expectation files found no other occurrence.
- **Commit:** `8f3b997`

**2. [Planning fact] One more SPEC quote: §5.11.5 deviations list (line 1106)**
- **Fix:** Updated with the other quotes (the UTF-8 message, wrapped across lines).
- **Commit:** `8f3b997`

### Scope additions

**1. `building-and-testing.md` harness section brought fully up to date**
- **Planned:** only the test-file extension.
- **Also done:** the extra-checks list (7 → 11, now a table) and the count 376 = 365 + 11, both stale
  since 02-01. Same file and the same topic; leaving them stale next to the edit would have been wrong.
- **Commit:** `b41c737`

### Deferred Items

None new. Still scheduled:
- **02-03:** the editor's hover texts saying "not available in Pentest"
- **02-04:** "Pentest" in code comments
- **02-05:** prose such as SPEC:1020 "Pentest strings are valid UTF-8", and test-case names `*.pen` in other
  docs

## Issues Encountered

| Issue | Resolution |
|-------|------------|
| Binary test expectations are invisible to Grep/rg | Use `grep -a` over `git ls-files --eol` `-text` entries |
| A bash `grep` with `\b` under the command wrapper returned false negatives | Cross-checked with the Grep tool and targeted reads |

## Next Phase Readiness

**Ready:**
- Plan 02-03 (VSCode extension) can rely on `cstar` being the binary, `.cst` the extension, and the
  messages final

**Concerns:**
- The extension has 409 mentions, including the settings keys (`pentest.executable`). Existing users'
  settings need a fallback from `pentest.*` to `cstar.*`.

**Blockers:**
- None

---
*Built with PAUL Framework v1.4 · https://chrisai.cv/skool · https://youtube.com/@chris-ai-systems*
*Phase: 02-rename-to-cstar, Plan: 02*
*Completed: 2026-11-05*
