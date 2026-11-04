---
description: "Baseline of build and test results before any cstar changes"
type: Baseline
about: "cstar"
---

# Baseline — plan 01-01

The starting point that every later plan compares against. Recorded 2026-11-04, before the rename or any
compiler/runtime change.

## Environment

| Item | Value |
|---|---|
| Commit | `6c82d72f04b51ef7f4372619bb7ed3927760b2d9` (initial commit) + uncommitted docs edits (no source changes) |
| Host | Windows 11 Pro 10.0.26300, WSL 3.0.3, kernel 6.18.54.1-microsoft-standard-WSL2 |
| Distro | Ubuntu 24.04.5 LTS (`Ubuntu-24.04`, WSL 2) |
| Working copy | `/mnt/c/work/project/cstar/Pentest` (the Windows checkout, LF line endings via `.gitattributes`) |
| clang | Ubuntu clang 18.1.3 (1ubuntu1) |
| make | GNU Make 4.3 |
| libcurl | 8.5.0 (`libcurl4-openssl-dev`) |
| sha256sum | GNU coreutils 9.4 |
| python3 | 3.12.3 |
| ld | GNU ld (Binutils for Ubuntu) 2.42 |
| node (Windows) | v24.16.0 |

## Results

| Suite | Command | Result | Wall time |
|---|---|---|---|
| Build | `make clean && make` (in WSL) | exit 0, **0 warnings, 0 errors**; `build/pentest --version` → `pentest 0.2.1` | 20.0 s |
| Tests | `make test` (in WSL) | **372 passed, 0 failed** (365 program cases + 7 CLI checks) | 31.6 s |
| Editor | `node editors/vscode/test/smoke.js` (Windows) | **1294 checks, 1294 passed, 0 failed** | 0.1 s |
| Docs | `python3 tests/check_docs.py` (in WSL) | samples **52 passed, 0 failed, 1 skipped**; links 313 checked, **3 broken** | 4.6 s |

## Findings (recorded, not fixed)

1. **Three broken links in `SPEC.md`.** Lines 1215–1217 link to `examples/sqli_union.pen`,
   `examples/sqli_post.pen` and `examples/sqli_lab.py`. `examples/` is git-ignored and those files don't exist,
   so the links break in every clone. `SPEC.md` is normative and was out of bounds for this plan.
   → Candidate for 01-02 (stale docs).
2. **Stale counts in the docs.** The docs claim "368 checks = 361 cases + 7" (actual: 372 = 365 + 7) and
   "1234/1241 smoke checks" (actual: 1294). → 01-02.
3. **Tracked build output changes on every rebuild.** `make` modified 12 tracked files under `build/`.
   → 01-02 untracks `build/`.

## How to reproduce

```bash
# from Windows (PowerShell or Git Bash)
wsl -d Ubuntu-24.04 -u root -- bash -lc 'cd /mnt/c/work/project/cstar/Pentest && make clean && make && make test'
wsl -d Ubuntu-24.04 -u root -- bash -lc 'cd /mnt/c/work/project/cstar/Pentest && python3 tests/check_docs.py'
node editors/vscode/test/smoke.js
```
