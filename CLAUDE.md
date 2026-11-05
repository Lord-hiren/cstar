# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

cstar (formerly Pentest) is a Python-like language for security tooling. The `cstar` toolchain (C++17, `src/`; `pentest` remains an alias of the same driver until v0.4) translates a `.cst` program (legacy `.pen`/`.pent` are accepted until v0.4) into **one C11 translation unit**, compiles it with `clang -O3 -march=native`, and links it against a C11 runtime (`runtime/cstar_rt.{h,c}`). There is no IR and no optimiser pass: every performance decision is made where C is emitted.

**Linux x86_64 only.** You need `clang`/`clang++`, GNU `make`, `sha256sum`, `/proc/cpuinfo` and the libcurl dev headers (the runtime's `http_get`/`http_post` builtins use libcurl; `Makefile` and `src/main.cpp` link `-lcurl`).

**Building from Windows:** use WSL (`Ubuntu-24.04`, which has clang 18, make, libcurl dev and python3 installed). Run commands from the repo root, because `wsl` starts in the current Windows folder (`/mnt/c/...`):

```bash
wsl -d Ubuntu-24.04 -- make
wsl -d Ubuntu-24.04 -- make test
wsl -d Ubuntu-24.04 -- bash -c 'build/cstar run tests/cases/NAME.cst </dev/null | diff tests/cases/NAME.out -'
wsl -d Ubuntu-24.04 -- python3 tests/check_docs.py
node editors/vscode/test/smoke.js      # runs natively on Windows
```

- **Line endings:** `.gitattributes` forces LF line endings; never convert files back to CRLF. CRLF breaks `tests/run_tests.sh` and every byte-exact `.out` comparison.
- **After a line-ending conversion:** `git status` may list unchanged files as modified, because git cached their old sizes. `git add -u` on those files refreshes the cache without staging any content.
- **The PATH after installing WSL:** a shell started before the WSL install may not have `wsl` on its PATH. Call `"C:\Program Files\WSL\wsl.exe"` directly in that case.
- **Baseline:** recorded results are in `.paul/phases/01-dev-environment-repo-hygiene/01-01-BASELINE.md`.

## Commands

```bash
make                                   # build/cstar + the build/pentest alias (also precompiles + embeds the runtime)
make test                              # bash tests/run_tests.sh: every tests/cases/*.cst + 11 CLI checks (~30 s)
make clean                             # rm -rf build

build/cstar run  prog.cst [args]       # compile (content-hash cached) and run
build/cstar build prog.cst -o out      # native executable; default ./<basename> (no extension) in CWD
build/cstar check prog.cst             # lex+parse+link(strict)+semantic only, no clang; silent on success
build/cstar run                        # no file: runs ./index.cst (exit 2 if missing)

# Single test case (the harness has no filter): stdin is <name>.in or /dev/null
build/cstar run tests/cases/NAME.cst </dev/null | diff tests/cases/NAME.out -

node editors/vscode/test/smoke.js      # editor extension checks (no deps)
python3 tests/check_docs.py            # run every doc code sample + check links (not part of make test)
python3 tests/check_docs.py --links    # links/anchors only, instant
python3 tests/check_docs.py --samples docs/guide/language-tour.md
```

Before a change lands, `make && make test` and `node editors/vscode/test/smoke.js` should both pass. Exit codes: `0` ok, `1` compile or runtime error, `2` usage error.

**CI:** `.github/workflows/ci.yml` runs four gates on Ubuntu 24.04 for pushes to `main`/`phase-*` and for PRs to `main`: a warning-free `make`, `make test`, the smoke test and `check_docs.py`. Keep local commands and the workflow in sync.

**Seeing the generated C:** the driver deletes `gen.c` after linking. To keep it, put a `clang` shim first on `PATH` that copies `*/gen.c` before calling the real clang (recipe in `docs/internals/code-generation.md` § "Inspecting generated C"). Any clang warning from `gen.c` is a bug.

## Architecture (pipeline)

```
src/lexer.cpp     tokens, INDENT/DEDENT, comments, literals           — stops at FIRST error
src/parser.cpp    recursive descent → AST (src/ast.h)                 — stops at FIRST error
src/modules.cpp   link_imports: resolve, reject cycles, mangle (__csm<id>_<name>),
                  flatten into entry AST; run/build prune unused imported decls,
                  `check` (strict) validates all of them; no-op with no imports
src/semantic.cpp  5 phases — reports ALL errors
src/codegen.cpp   AST → gen.c (+ native-int lane, definite assignment, alloc-free proof)
src/main.cpp      CLI, temp dir, sha256sum cache key, runtime-object cache, clang invocations
```

- **Semantic phases** (in order): declaration discovery (this is why forward references work), globals, top-level/`main:` statements, function bodies (locals, capture analysis/cell boxing, static arity checks), then cross-scope `variable.<q> name` resolution. The last runs after everything else so it can target functions declared later in the file.
- **Callee identity and builtin shapes are resolved at compile time.** Arity and named-argument errors are compile errors. Each builtin becomes one direct `cs_builtin_*` C call, driven by the table in `src/builtins.h`. Builtins the program doesn't use emit nothing.
- **Runtime embedding:** `tools/embed.cpp` produces `build/runtime_embed.h` from the runtime sources, a precompiled `build/cstar_rt.o` and the build host's `/proc/cpuinfo` flags line, which acts as an ISA fingerprint. As a result, **any runtime edit rebuilds every TU** and re-keys the runtime cache. If the CPU doesn't match, the driver falls back to `$XDG_CACHE_HOME/cstar/rt-<key>.o`. Module search: importing file's folder, CWD, `$CSTAR_PATH`, then the legacy `$PENTEST_PATH`.
- **Runtime model:** `cs_value` is a 16-byte tagged union passed by value. Memory is managed by a non-moving mark & sweep GC whose roots are shadow-stack frames (`cs_frame_begin`), registered globals and a protect stack. Runtime errors become exception instances. Every failing operation is passed its source line, so an uncaught error prints `file:line: runtime error: …`.
- **Codegen invariants** (`docs/internals/code-generation.md` § "Rules the generator must keep"):
  - forward declarations come before all definitions;
  - every failing op gets its line number;
  - reads are guarded unless definite assignment proves them safe;
  - temporaries are cleared at statement boundaries, and every live value is rooted at every safe point;
  - `return` and `try` keep the frame and protect depths restorable;
  - integer edge cases behave as cstar specifies (wrap, signs, `MIN / -1`).
- **Optimisation lanes are conservative:** a lane bails out ("poisons") on anything it can't prove. Performance changes are validated A/B with byte-identical stdout, stderr and exit codes (`docs/performance/optimization.md` §9).

## Tests (`tests/cases/`)

| Sidecar | Meaning |
|---|---|
| `NAME.out` | **required**, exact byte match of stdout |
| `NAME.err` | each non-empty line must appear as a **substring** of stderr; if the file is absent, stderr must be empty |
| `NAME.exit` | expected exit code (default 0) |
| `NAME.in` | stdin |

- Prefixes group cases by area: `rt_` uncaught runtime errors, `err_` compile/runtime failures, `cf_`, `bi_`/`bi_py_`, `col_`, `fn_`, `cls_`/`exc_`/`gc_`, `varq_`/`xvar_`, `imp_` (fixtures in `tests/cases/lib/`), and others. The full table is in `docs/development/building-and-testing.md`.
- To add a case, start the `.cst` with a one-line comment naming what it proves and its SPEC §. Generate the `.out` with `build/cstar run`, **review it** (cross-check against Python where parity applies), then add `.err`/`.exit` for failures.
- Every bug fix lands with a regression case. Never loosen a test to make it pass.
- The harness isolates `XDG_CACHE_HOME` and scratch dirs per run.

## Rules that are easy to break

- **Normative docs:** `SPEC.md`, `docs/contracts/*` and `docs/reference/cross-scope-access.md`. When the compiler disagrees with them, the compiler is wrong. Where contracts and SPEC differ, the contracts win. To change behaviour, update the contract or SPEC first, then the code.
- **SPEC section numbers never change.** Source comments, tests and the editor extension cite `SPEC §…`, so add new subsections instead of renumbering.
- **Quoted error messages are normative.** Tests match them as substrings. Change the message text in code, SPEC and `.err` files together.
- **Language changes need editor updates.** A new builtin, keyword, method or exception must also go into `editors/vscode/src/extension.js` (`BUILTIN_DOCS`, `RESERVED`, `KEYWORD_DOCS`, `METHOD_DOCS`, `EXCEPTION_DOCS`) and the grammar/snippets. The smoke test asserts on these lists (see `docs/development/editor-extension.md`).
- **Moving or renaming a doc** means updating every reference to it (`grep -rn "docs/" src runtime bench editors tests`) and rerunning `check_docs.py --links`.
- **Doc code samples are executed:** a ```` ```python ```` fence followed by a ```` ```text ```` fence is a test (stdout then stderr). Use `<!-- no-run -->` before the fence to skip one.
- **Some docs are historical records:** `docs/history/*` and `docs/performance/baseline-*.md` describe a past moment. Add a note to them instead of updating.
- **Repeated counts and versions:** test counts, "N builtins" and the version string appear in several docs and may already be stale. The real values come from `make test` and `cstar --version`.
- **Vocabulary:** write *text* (not string) and *map* (not dict). The language is **cstar** (lowercase; called Pentest before v0.3) and the CLI is `cstar`. The legacy names — the `pentest` command, `.pen`/`.pent`, `$PENTEST_PATH`, the editor's `pentest.*` settings — keep working until v0.4, so mention them only as legacy. Historical docs, recorded bench results and the benchmark tooling (`bench/` runners and workloads, `tools/hpcbench`) still use the old names.
- **The language deliberately differs from Python:** `//` is a comment, `and`/`or` return `bool`, an unparenthesised `a, b` joins into text, exception matching is by exact class, and `eval`/`exec` are rejected. Read `docs/guide/gotchas.md` before writing `.cst` code.
- **`build/` is git-ignored**; `make` recreates it. `examples/` and top-level `tests/*.cst` / `tests/*.pen` are git-ignored scratch.
- **Bench runners overwrite recorded results:** `bench/run-m2b.sh`, `run-m2c.sh` and `run-varq.sh` rewrite the baselines in `bench/results/` that `docs/performance/` quotes. Only `bench/run.sh` is insulated, because it uses the frozen `bench/tools/pentest-m1`.
- **Fixed bugs:** when you fix a bug listed in `docs/reference/known-issues.md`, delete its entry and note the fix in `CHANGELOG.md` under *Unreleased*.
