# Building and testing

## Build

```bash
make              # build/pentest
make clean        # rm -rf build   (build/ is git-ignored; make recreates it)
make test         # runs tests/run_tests.sh
```

Compiler flags are `clang++ -std=c++17 -Wall -Wextra -O2 -Isrc -Ibuild`, with
`-MMD -MP` dependency files so header edits rebuild what they should. The only
generated source is `build/runtime_embed.h`, produced by `build/embed`
(compiled from `tools/embed.cpp`). It embeds `runtime/pentest_rt.{c,h}`, the
precompiled runtime object `build/pentest_rt.o` and the build host's
`/proc/cpuinfo` `flags` line (`build/pentest_rt.cpu`) — so **any edit to the
runtime rebuilds the object, the header and every translation unit that
includes it**, and changes the runtime-object cache key.

The precompiled object is what makes a first-ever `run`/`build` fast: `make`
pays the ~6 s `clang -std=c11 -O3 -march=native` runtime compile once and the
driver links the embedded object instead of recompiling the runtime for every
new program. Since that object is CPU-specific, the embedded `flags` line acts
as an ISA fingerprint: on a different machine (or after a kernel upgrade that
changes the line) the driver transparently falls back to compiling and caching
the runtime locally.

`build/` holds only generated files and is git-ignored, so rebuilding never
shows up in `git status`.

## The test suite

```bash
make test         # or: bash tests/run_tests.sh
```

Current size: **372 checks = 365 program cases + 7 extra checks**. A full run
takes about 70 seconds on the development machine (measured), because every
case is a real compile (`clang -O3 -march=native`) and run.

### How a case works

`tests/run_tests.sh` runs `build/pentest run tests/cases/<name>.pen` for every
case, with stdin taken from `<name>.in` if present, else `/dev/null` (so
`input()` can never hang the harness), and checks:

| Sidecar | Meaning |
|---|---|
| `<name>.out` | **required** — expected stdout, compared byte-for-byte |
| `<name>.err` | optional — every non-empty line must appear as a **substring** of stderr; if the file is absent, stderr must be **empty** |
| `<name>.exit` | optional — expected exit code (default `0`) |
| `<name>.in` | optional — stdin |

Because `.err` matching is by substring, **quoted error messages in the SPEC are
normative**: change the wording and cases fail.

Seven extra CLI-level checks follow the cases: `check_valid` (silent, exit 0 on a
good file), `check_error` (non-zero on a bad one), `build_roundtrip` (`build` then
run the binary directly), `pent_extension` (`.pent` accepted by `run` and the
bare form, and `build script.pent` → `script.pentest`), `no_sha256sum` (the
driver still runs without the hash tool), `check_imports_valid` (a multi-file
program checks clean) and `check_imports_strict` (`check` rejects a broken
declaration that `run` would prune).

### Isolation

The harness sets a private `XDG_CACHE_HOME` and a private scratch directory for
each run and removes both on exit, so a green run can never be an artefact of a
warm cache, and no `/tmp/pentest-*` litter should remain after a run.

### Case naming

Names are a prefix plus a topic; error cases start with `err_`.

| Prefix | Area |
|---|---|
| `rt_` | **uncaught runtime errors**: tiny programs that fail at exactly one operation, with the message in `.err` and exit code `1` (largest family) |
| `cf_` | control flow (`if`, `while`, `for`, `switch`, truthiness, ternary, short-circuit) |
| `bi_` | builtins (core and Python-parity), incl. `bi_py_*` |
| `col_` | collections and their literals/indexing/iteration/display |
| `fn_` | functions, defaults, named arguments, closures |
| `cls_`, `exc_`, `gc_` | classes, exceptions, garbage collection (Stage 2C) |
| `varq_`, `xvar_` | cross-scope `variable.` access |
| `imp_` | modules/imports; fixtures they import live in `tests/cases/lib/` |
| `des_`, `sl_`, `meth_`, `mem_`, `txt_`, `jn_`, `sw_`, `ops_`, `comp_`, `scope_`, `nint_`, `doc_` | destructuring, single-line suites (`sl_`), builtin methods, membership, text/bytes, comma-join, `switch`, operators, comparison, scope, native-int lane edge cases, the four canonical design examples (slicing lives under `col_slice_*`) |
| `err_*` | compile-time and runtime failures, with `.err` and `.exit` sidecars |

### Adding a test

1. Write `tests/cases/<name>.pen` (start with a one-line comment saying what it
   proves and which SPEC section it exercises, as many existing cases do).
2. Run it and **review** the output before accepting it:
   `build/pentest run tests/cases/<name>.pen > tests/cases/<name>.out`.
3. For a failing program also write `<name>.err` (the message substrings) and
   `<name>.exit` (`1`).
4. `make test`.

Never make a failing test pass by loosening it: a failing case is a signal about
the compiler, or about the contract — in which case update the contract first.

### Regression cases from the documentation pass

The bugs the documentation pass reproduced each landed with a regression case:
`try_multi_named.pen` (multi-clause `except` — every clause after the first),
`bi_py_parse_catch.pen` + `rt_bi_parse_finally.pen` (caught `int()`/`float()`
parse errors, `finally` on the unwind edge), `bi_py_format_group.pen`
(right-counting thousands grouping and `","` after a width, outputs identical
to Python), and the CLI check (e) `no_sha256sum` (a `PATH` without
`sha256sum` disables caching instead of killing the driver with SIGPIPE).
The GC text-liveness fix followed the same pattern — one case per defect
(`gc_text_liveness.pen`). The next bug fixed should land the same way — a
case per defect, with outputs cross-checked against Python where parity
applies.

## The editor extension's tests

```bash
node editors/vscode/test/smoke.js     # 1294 checks; needs only node
```

See [editor extension](editor-extension.md).

## Verifying the documentation

Code samples in the guides are meant to be *executed*. The convention and the
checker are described in the [documentation guide](documentation-guide.md).

## Continuous checks to run before a change lands

* `make && make test` — green.
* `node editors/vscode/test/smoke.js` — green (it asserts on builtin lists,
  keyword lists and doc pointers, so language additions need extension updates).
* For performance-sensitive changes: the A/B procedure in
  [optimization §9](../performance/optimization.md) and, where relevant, the bench
  runners ([benchmarking](benchmarking.md)).
