---
phase: 02-rename-to-cstar
plan: 03
subsystem: editor-extension
tags: [rename, vscode, textmate, settings-fallback, install-script, junction, d-7]

# Dependency graph
requires:
  - phase: 02-rename-to-cstar (02-01)
    provides: the `cstar` CLI (+ `pentest` alias), `.cst`, `CSTAR_PATH`
  - phase: 02-rename-to-cstar (02-02)
    provides: final cstar error-message wording; test programs in `.cst`
provides:
  - "VSCode extension cstar-lang.cstar-lang: language id cstar for .cst (+ legacy .pen/.pent), grammar source.cstar"
  - "cstar.* settings and commands, with pentest.* fallbacks (settings deprecated, commands aliased) until v0.4"
  - "Extension runs `cstar check` / `cstar run`, falling back to a `pentest` CLI; CSTAR_PATH (+ PENTEST_PATH)"
  - "D-7 closed: fvec/ivec/fvec_fill/ivec_fill/dot in RESERVED, BUILTIN_DOCS and the grammar; all 78 compiler builtins covered"
  - "install-vscode-extension.sh: new id, removes stale links to this folder, Windows junction, macOS path fixed"
affects: [02-04 internals (hover text still quotes the runtime's `(pentest)` User-Agent and runtime/pentest_rt.c), 02-05 docs, v0.4 (drop legacy names; Windows CLI makes diagnostics work natively)]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Legacy-setting fallback: inspect() the new key at every level; use the old key only when the new one is unset"
    - "Install links are removed only when they resolve to this repo's editors/vscode; real folders are never deleted"
    - "Real-VSCode check: Code.exe --extensionDevelopmentPath --extensionTestsPath with a throwaway profile (unset ELECTRON_*/VSCODE_* first)"

key-files:
  created:
    - editors/vscode/syntaxes/cstar.tmLanguage.json (git mv from pentest.tmLanguage.json)
    - editors/vscode/snippets/cstar.json (git mv from pentest.json)
  modified:
    - editors/vscode/package.json
    - editors/vscode/src/extension.js
    - editors/vscode/test/smoke.js
    - editors/vscode/README.md
    - install-vscode-extension.sh
    - docs/development/editor-extension.md
    - CHANGELOG.md

key-decisions:
  - "Extension id cstar-lang.cstar-lang; one language id `cstar` claims .cst, .pen and .pent (no separate pentest language)"
  - "Legacy CLI candidates (`pentest`) are tried only while cstar.executable is at its default"
  - "On Windows the install script links with a directory junction (Git Bash ln -s silently copies)"

patterns-established:
  - "Before a human-verify checkpoint, try to automate it in a real host (user asked Claude to test itself)"

# Metrics
duration: ~35min (plan commit 04:30Z → task 3 04:43Z; real-VSCode check 04:50Z)
started: 2026-11-05T04:30:41Z
completed: 2026-11-05T05:05:00Z
description: "VSCode extension renamed to cstar (id, language, grammar, settings, CLI) with pentest.* fallbacks; D-7 vector builtins added; install script handles legacy links and Windows junctions; smoke 1363/0 and 45/45 in a real VS Code 1.144.1"
type: Summary
about: "cstar"
---

# Phase 2 Plan 03: VSCode extension → cstar — Summary

**The editor extension is now `cstar-lang.cstar-lang`.** It works as follows:
- **Files:** it opens `.cst` files, plus `.pen`/`.pent` until v0.4. The grammar is `source.cstar`.
- **CLI:** it runs `cstar check` and `cstar run`, and falls back to a `pentest` CLI.
- **Settings:** it reads the `cstar.*` settings. Old `pentest.*` settings and command ids keep working.
- **Vector builtins:** it now knows `fvec`, `ivec`, `fvec_fill`, `ivec_fill` and `dot` (D-7).

Results: the smoke test passes 1363/0. In a real VS Code 1.144.1, all 45 end-to-end checks pass.

## Performance

| Metric | Value |
|--------|-------|
| Duration | ~35 min of commits (plus the context-heavy Task 1 before them) |
| Completed | 2026-11-05 |
| Tasks | 3 auto + 1 human-verify checkpoint (verified by Claude in a real VS Code, at the user's request) |
| Qualify results | 3 PASS. Task 1 needed 17 smoke assertions updated to the new names (expected) |
| Commits | `dd29e34` (plan), `90108f7` (rename), `e1f65aa` (D-7), `f3d789b` (docs + install script) |
| Diff | 9 files, +819 / −463 |

## Acceptance Criteria Results

| Criterion | Status | Notes |
|-----------|--------|-------|
| AC-1: Identity and language | ✅ Pass | package.json is `cstar-lang`/`cstar-lang`, "cstar Language"; the language `cstar` covers `.cst/.pen/.pent`; grammar `source.cstar` with every scope ending in `.cstar` (38 suffixes); snippets `cstar.json`; activation `onLanguage:cstar`. Real VS Code: `cstar` is registered, `pentest` is not, and `.pen`/`.pent` open as cstar |
| AC-2: Settings and commands, with legacy fallbacks | ✅ Pass | `setting()` reads `cstar.*` first and falls back to `pentest.*`; the `pentest.*` settings carry a `deprecationMessage`. Real VS Code: default 400; only the legacy setting at 777 → 777; cstar at 555 → 555. All four command ids are registered |
| AC-3: The extension talks to the cstar CLI | ✅ Pass | Candidate order from the VS Code log: `cstar`, `~/.local/bin/cstar`, `~/Projects/Pentest/build/cstar`, then the same three for `pentest`. Temp file `cstar-check-….cst` with `CSTAR_PATH`+`PENTEST_PATH`. Run opens a terminal named "cstar" |
| AC-4: Text says cstar | ✅ Pass | Hover, completion and warnings say cstar and use ```` ```cstar ```` fences. Remaining `pentest` mentions are legacy notes, the `.pen/.pent` associations, or factual runtime references (User-Agent, `runtime/pentest_rt.c` → 02-04) |
| AC-5: Vector builtins (D-7) | ✅ Pass | All five are in RESERVED, BUILTIN_DOCS and the grammar. Cross-check: 78 compiler builtins, 0 missing. Real VS Code: `fv` offers `fvec` → `fvec(${1:iterable})`; hover shows the float64 contract; `x.dot(` stays a method |
| AC-6: Smoke test green | ✅ Pass | 1294 → 1363 checks, 0 failed (new sections: rename fallbacks, D-7) |
| AC-7: Docs and install script | ✅ Pass | Script tested against a fake HOME on Git Bash (junctions) and WSL (symlinks), 13/13 each, twice, with the repo untouched. README, editor guide and CHANGELOG updated. `check_docs --links`: 316 links, 0 broken |
| AC-8: Looks right in VSCode | ✅ Pass (automated) | The user asked Claude to test it. A separate VS Code instance loaded the extension and ran 45 checks against the real API, 45/45. Covered: language mode, highlighting (`fvec` → `#DCDCAA` in Dark+/Dark Modern, `#795E26` in Light+), completion, snippets, hover (print/fvec/dot), comment toggle, Run terminal, and the expected "cstar CLI not found" log line. Not machine-checked: the notification popup itself |

## Accomplishments

- **Nothing breaks for existing users.** Old settings, keybindings, `.pen` files and a `pentest` CLI keep
  working until v0.4, and VS Code flags the old settings as deprecated.
- **The editor finally knows every builtin the compiler reserves.** A cross-check script proves the
  RESERVED set matches the compiler, and the smoke test now guards it.
- **The install script really installs on Windows.** It used to make a silent copy; it now creates a live
  junction. It also cleans up old links to this folder without ever deleting a real directory.
- **First end-to-end test in a real VS Code.** It covers the grammar engine, theme colours and the provider
  pipelines, not just stubs.

## Task Commits

| Task | Commit | Type | Description |
|------|--------|------|-------------|
| Plan | `dd29e34` | docs | Plan 02-03 |
| Task 1: rename | `90108f7` | feat | package.json; grammar and snippets `git mv` + scopes; extension.js settings/CLI/commands/text; smoke updates + fallback checks |
| Task 2: D-7 | `e1f65aa` | feat | 5 vector builtins in RESERVED, BUILTIN_DOCS and the grammar; smoke section |
| Task 3: docs + install | `f3d789b` | docs | install-vscode-extension.sh; extension README; editor-extension.md; CHANGELOG |
| Task 4: checkpoint | — | — | Real-VS-Code check 45/45 (scratchpad script, not committed) |

Plan metadata: committed with this SUMMARY.

## Files Created/Modified

| File | Change | Purpose |
|------|--------|---------|
| `editors/vscode/package.json` | Modified | Identity, language, grammar/snippet paths, `cstar.*` and deprecated `pentest.*` settings, `cstar.run` |
| `editors/vscode/syntaxes/cstar.tmLanguage.json` | Renamed + modified | Scopes `.cstar`; builtin alternation gains the 5 vector builtins |
| `editors/vscode/snippets/cstar.json` | Renamed | Same snippets under the new language |
| `editors/vscode/src/extension.js` | Modified | `setting()`, `cliNames()`, `cliLocations()`, both command prefixes, cstar text, D-7 entries, `_test` exports |
| `editors/vscode/test/smoke.js` | Modified | New names; fallback, alias and D-7 sections (1363 checks) |
| `editors/vscode/README.md` | Modified | cstar names, install, settings + legacy note, CLI lookup, Windows note; dropped the git-ignored `extras/AI.pen` pointer |
| `install-vscode-extension.sh` | Modified | New id, legacy-link cleanup, junction on Windows, macOS path |
| `docs/development/editor-extension.md` | Modified | Layout/sizes, settings fallback, diagnostics env, install |
| `CHANGELOG.md` | Modified | Unreleased → Changed (extension, D-7) and Tooling (install script) |

## Decisions Made

| Decision | Rationale | Impact |
|----------|-----------|--------|
| One language id `cstar` for all three extensions | Two language ids would split settings and providers, and the legacy files are the same language | v0.4 just drops `.pen/.pent` from the list |
| Legacy `pentest` CLI candidates only while `cstar.executable` is at its default | An explicit user choice must not be second-guessed | Predictable diagnostics |
| Junction on Windows, never delete real folders | Git Bash `ln -s` copies; deleting a user's folder is irreversible | The dev install is live on Windows; cleanup is safe |

## Deviations from Plan

### Summary

| Type | Count | Impact |
|------|-------|--------|
| Auto-fixed | 3 | Small, all in files the task already owned |
| Scope additions | 1 | Broader legacy-link cleanup, same intent |
| Deferred | 1 new (D-10) | Optional |

**Total impact:** small; no scope creep beyond the install script's own correctness.

### Auto-fixed Issues

**1. [Bug] Install script copied instead of linking on Windows**
- **Found during:** Task 3 (probing Git Bash: `ln -s` produced a plain copy with `MSYS` unset)
- **Fix:** on MINGW/MSYS/CYGWIN, `"$COMSPEC" /c mklink /J`, falling back to `ln -s` with a "this is a copy"
  note. Git Bash treats junctions as `-L`, `pwd -P` resolves them, and `rm -f` / `rm -rf` remove only the
  link (verified with a scratch source file).
- **Commit:** `f3d789b`

**2. [Bug] Install script's macOS path was a folder VS Code doesn't read**
- **Fix:** `~/.vscode/extensions` on Darwin, as on Linux. `sed` uses `[[:space:]]` instead of the GNU-only `\s`.
- **Commit:** `f3d789b`

**3. [Docs] README pointed public readers at the git-ignored `extras/AI.pen`**
- **Fix:** the note was removed; the HTTP builtins moved into the feature rows (Phase 1 decision: no
  links into `extras/`).
- **Commit:** `f3d789b`

### Scope additions

**1. Legacy cleanup covers any link to this folder, not only `pentest-lang.pentest-lang`**
- The old README told users to link by hand as `pentest.pentest-lang-0.2.1`. The script now removes every
  link that resolves to this repo's `editors/vscode`, whatever its name. Legacy-named real folders, and
  links to other places, are only reported.

### Deferred Items

- **D-10 (new, S):** keep the real-VS-Code end-to-end test (`vscode-e2e/index.js` + `run.sh`, currently in
  the session scratchpad) as a local-only check in the repo. CI has no VS Code GUI. Optional.

## Issues Encountered

| Issue | Resolution |
|-------|------------|
| `cmd.exe` not on the Bash tool's PATH | Use `"$COMSPEC"` (set by Windows) with `MSYS_NO_PATHCONV=1` |
| `wsl` not on PowerShell's PATH in this session | `& "C:\Program Files\WSL\wsl.exe"` (as CLAUDE.md notes) |
| Code.exe would start as Node or attach to the running window | Unset every inherited `ELECTRON_*`/`VSCODE_*` variable; use a throwaway `--user-data-dir` |
| E2E toggle-comment check read the line too early | `toggleLineComment` doesn't return its `editor.edit()` promise; the test now waits for the change. Harmless for keybindings |
| The shell `grep` wrapper garbled output | Used the Grep/Read tools |

## Next Phase Readiness

**Ready:**
- 02-04 (internal names) can rename `runtime/pentest_rt.*` and update the extension's one hover reference
  to it in the same plan, together with the `(pentest)` User-Agent string the hover quotes.

**Concerns:**
- The extension's hard-coded dev fallback is `~/Projects/Pentest/build/<name>`. That's harmless, but it no
  longer matches where the repo lives; revisit with v0.4 packaging.
- On Windows, diagnostics stay off until a native (or WSL-bridged) CLI exists (v0.4).

**Blockers:**
- None

---
*Built with PAUL Framework v1.4 · https://chrisai.cv/skool · https://youtube.com/@chris-ai-systems*
*Phase: 02-rename-to-cstar, Plan: 03*
*Completed: 2026-11-05*
