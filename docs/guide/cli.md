# Command-line reference

```text
pentest <file.pen|file.pent> [args...]        # shorthand for `run`
pentest run   <file.pen|file.pent> [args...]  # compile (cached) and execute
pentest build <file.pen|file.pent> [-o out]   # emit a native executable
pentest check <file.pen|file.pent>            # parse + analyse only
pentest --help | -h
pentest --version | -v                        # prints: pentest 0.2.1
```

The normative statement of this contract is SPEC §10; this page adds the
observed behaviour.

## Source files

* A source path **must end in `.pen` or `.pent`**. Both are accepted
  everywhere; `.pen` is the canonical spelling. Anything else is a usage error
  (exit `2`) — in particular `.pentest` (the compiled-binary extension) is
  rejected as input.
* Source is UTF-8. An empty file is a valid, silent program.
* Imports are resolved relative to the importing file, then the current
  working directory, then `$PENTEST_PATH` — see
  [modules](modules.md#resolution). Imported files are ordinary source files;
  the entry path rules do not restrict them.

## Commands

### `run`

Compiles the program (or reuses a cached executable) and executes it.
Standard input, output and error are inherited. **The program's own exit code
becomes `pentest`'s** — except that an uncaught Pentest runtime error exits
`1` by definition.

Extra arguments after the file are passed to the program as `argv[1..]`. Note
that, as of `0.2.1`, **no builtin can read them** (there is no `argv`
builtin), so they are currently inert.

```bash
echo bob | build/pentest run greet.pen
```

### `build`

Writes a native executable and does **not** run it. The default output is the
source's **basename** with `.pen`/`.pent` replaced by `.pentest`, written to
the **current working directory** — not next to the source (`tools/scan.pen` →
`./scan.pentest`); `-o out` overrides it. `-o` may be given once and needs
an argument. `build` never *serves* from the program cache, but it does use the
shared compiled-runtime object (below).

The result depends only on the system C library, libm and libcurl.

### `check`

Runs the lexer, parser and semantic pass — **no code generation and no
`clang`**. Silent with exit `0` on success; on failure prints diagnostics and
exits `1`. This is what the VSCode extension calls for live error squiggles. It
takes no arguments beyond the file.

`check` is **strict about imports**: it loads the whole module graph and
validates every imported declaration, including ones `run`/`build` would prune
as unused. Diagnostics can therefore point into imported files. See
[modules](modules.md#selective-loading-you-only-pay-for-what-you-name).

Lexer and parser errors stop at the *first* error. The semantic pass reports
**every** error it finds before failing.

## Exit codes

| Code | Meaning |
|---|---|
| `0` | success (including an empty program) |
| `1` | lexer/parser error, semantic error, uncaught runtime exception, unreadable source file, missing `clang`, or a failed `clang` run |
| `2` | usage error: no input file, a path not ending in `.pen`/`.pent`, an unknown option, `-o` without a path, extra arguments to `check` |

Usage errors print `error: <reason>` followed by the usage text on stderr.

## The `run` cache

* Location: `$XDG_CACHE_HOME/pentest/`, falling back to `~/.cache/pentest/`.
* Contents: `<key>.pentest` (a compiled program) and `rt-<key>.o` (the runtime
  compiled once and linked into every program). Compiling the runtime is by
  far the slowest part of a cold build, so `make` precompiles it and embeds
  the object in the driver: a cold cache installs that object directly (no
  clang run), and `rt-<key>.o` is the fallback for a different CPU or an older
  build. The object key covers the runtime content, the compile flags and the
  current CPU's `/proc/cpuinfo` `flags` line, so a cache shared across
  machines never serves another ISA's machine code.
* Program key: `sha256(source bytes [+ dependency paths and contents]
  + version string + toolchain identity)`,
  where *toolchain identity* is the size and mtime of the running `pentest`
  binary. Editing a program re-keys it; rebuilding or upgrading `pentest`
  re-keys **everything**, so a stale executable can never be served.
* When the program imports modules, the key material also includes the
  canonical path and full contents of every dependency (in load order), so
  editing any imported file re-keys the program too. The module files
  themselves are never needed again after compilation.
* The hash is computed by piping to `sha256sum`. If it cannot run (missing
  binary, minimal container), the driver disables caching and every `run`
  compiles — and, because the run then executes its binary in place, the
  scratch directory keeps `program` behind (see
  [known issue 8](../reference/known-issues.md#8-a-cache-less-run-leaves-its-binary-in-the-temp-dir)).
* It is always safe to delete the directory.

## The compile pipeline, as the driver runs it

1. read the source → lex → parse (all in-process);
2. **link imports**: resolve, parse and flatten the module graph, rename
   imported declarations, prune unreachable imported code in `run`/`build`
   mode (`src/modules.cpp`; skipped entirely when the program has no imports);
3. semantic analysis (all in-process);
4. generate `gen.c` and write it, with the embedded `cstar_rt.c` /
   `cstar_rt.h`, into `$TMPDIR/cstar-XXXXXX/` (`/tmp` by default);
5. obtain the runtime object: install the precompiled object embedded by
   `make` when the host CPU fingerprint matches, otherwise compile it to
   `rt-<key>.o` (`clang -std=c11 -O3 -march=native -c …`);
6. link: `clang -std=c11 -O3 -march=native -o <out> gen.c <runtime.o> -lm`
   (or `gen.c cstar_rt.c` when neither object is available);
7. for `run`: store under the program key and `exec` it.

If `clang` is not on `PATH` the driver prints
`error: 'clang' was not found on PATH; the Pentest toolchain requires clang to
build programs` and exits `1`.

## Environment variables

| Variable | Effect |
|---|---|
| `XDG_CACHE_HOME` | base of the cache directory (default `~/.cache`) |
| `TMPDIR` | base of the per-build scratch directory (default `/tmp`) |
| `PENTEST_PATH` | colon-separated extra roots searched for imported modules, after the importing file's directory and the working directory |
| `PATH` | must contain `clang` and `sha256sum` |

No other environment variable or configuration file is read.
