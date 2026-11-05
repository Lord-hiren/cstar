# Getting started

From a fresh checkout to a compiled native executable in a few steps.

## Requirements

| Need | Why | Verified with |
|---|---|---|
| Linux x86_64 | the only supported host *and* target | — |
| `clang` and `clang++` | `clang++` builds the toolchain (C++17); `clang` compiles every cstar program (C11, `-O3 -march=native`) at run/build time. **`clang` must be on `PATH` whenever you use `cstar`**, not just when building it | clang 21.1.8 (README states 14+ as the minimum; older versions were not tested here) |
| GNU `make` | builds the toolchain | — |
| libcurl dev headers (`libcurl4-openssl-dev` on Ubuntu) | the runtime's HTTP builtins; every program is linked with `-lcurl` | libcurl 8.5.0 |
| `sha256sum` (GNU coreutils) | optional: keys the compiled-program cache. Without it, caching is disabled and every `run` compiles (which also leaves `program` in `$TMPDIR` — [known issue 8](../reference/known-issues.md#8-a-cache-less-run-leaves-its-binary-in-the-temp-dir)) | coreutils 9.10 |
| `bash` | the test harness only | — |
| `node` (optional) | the VSCode extension's smoke test | v24 |
| `python3` (optional) | the benchmark case generators only | 3.x |

The toolchain itself is plain C++17/C11. A compiled cstar program needs
libc, libm and libcurl, which backs the HTTP builtins
([builtins guide](builtins.md)).

### Windows (WSL)

The toolchain runs on Linux, so on Windows you build and run it inside WSL
(Windows Subsystem for Linux). This was verified on Windows 11 with Ubuntu
24.04 (clang 18.1.3).

1. In PowerShell, run `wsl --install -d Ubuntu-24.04` and accept the
   administrator prompt. **Restart Windows** when it asks, then run the same
   command again to install Ubuntu and create a Linux user.
2. Install the tools inside Ubuntu:

   ```bash
   wsl -d Ubuntu-24.04 -u root -- bash -c "apt-get update && apt-get install -y build-essential clang make libcurl4-openssl-dev python3"
   ```

3. From the repository folder in a Windows terminal, build and test. `wsl`
   starts in the current folder, which it sees as `/mnt/c/...`:

   ```bash
   wsl -d Ubuntu-24.04 -- make
   wsl -d Ubuntu-24.04 -- make test
   wsl -d Ubuntu-24.04 -- build/cstar run hello.cst
   ```

The repository's `.gitattributes` keeps every file's line endings LF on
Windows too. Don't convert them to CRLF: CRLF breaks the test harness and every
expected-output comparison.

## Build the toolchain

```bash
make            # builds build/cstar
make test       # optional: runs the 372-check suite (about a minute)
```

`make` compiles `src/*.cpp`, precompiles the runtime once with the flags the
driver uses on demand, then embeds both the runtime sources and that object
into the binary (`tools/embed.cpp` generates `build/runtime_embed.h`) — so the
finished `build/cstar` is self-contained, does not read `runtime/` at run
time, and a first-ever `cstar file.cst` links the embedded object instead of
recompiling the runtime (see [optimization §5](../performance/optimization.md#5-compiled-artifact-caches)).

Check it works:

```bash
build/cstar --version     # cstar 0.2.1
```

Optionally put it on your `PATH` (the VSCode extension also looks in
`~/.local/bin`):

```bash
mkdir -p ~/.local/bin && ln -sf "$PWD/build/cstar" ~/.local/bin/cstar
```

## Your first program

Create `hello.cst`:

```python
print("Hello, cstar!")
```

Output:

```text
Hello, cstar!
```

Run it. `run` compiles through C with `clang`, caches the executable, and
executes it:

```bash
build/cstar run hello.cst          # same as: build/cstar hello.cst
```

Build a standalone executable you can copy anywhere on a compatible Linux
system (it needs no `cstar` install):

```bash
build/cstar build hello.cst       # writes ./hello
./hello
```

Only parse and analyse — fast, never calls `clang`, silent on success:

```bash
build/cstar check hello.cst && echo ok
```

## What a compile error looks like

Compile errors point at the exact line and column and exit with code `1`:

```python
host = "example.com"
print(hots)
```

Output:

```text
example.cst:2:7: error: undefined variable 'hots'
  print(hots)
        ^
```

Runtime errors name the line only (never a column) and also exit `1`:

```python
xs = [1, 2, 3]
print(xs[10])
```

Output:

```text
example.cst:2: runtime error: list index out of range
```

See [errors and diagnostics](errors-and-diagnostics.md) for the catalogue.

## Where files go

| Artifact | Location |
|---|---|
| `build` output | `<source basename>` (no extension) in the **current directory**, or the `-o` path |
| `run` cache | `$XDG_CACHE_HOME/cstar/` (default `~/.cache/cstar/`): one `<sha256>.bin` per distinct program and one `rt-<sha256>.o` compiled-runtime object shared by all programs |
| Scratch while compiling | `$TMPDIR/cstar-XXXXXX/` (default `/tmp`), removed afterwards |

The cache key covers the source, the toolchain version **and** the toolchain
binary itself, so rebuilding `cstar` automatically invalidates old entries.
Deleting `~/.cache/cstar` is always safe. Details: [CLI reference](cli.md#the-run-cache).

## More examples

The repository doesn't ship a separate examples folder. Two good sources of
complete, runnable programs:

* **The [language tour](language-tour.md).** Every sample there is a complete
  program, shown with its exact output. Save one as `example.cst` and run it.
* **The test programs in `tests/cases/`.** Each `NAME.cst` has its expected
  output next to it in `NAME.out`:

```bash
build/cstar run tests/cases/fn_closure_capture.cst
cat tests/cases/fn_closure_capture.out
```

## Next steps

* Learn the language: the [language tour](language-tour.md).
* Know the pitfalls up front: [gotchas](gotchas.md).
* Look something up: the [builtins guide](builtins.md) or, for the precise rule,
  the [specification](../../SPEC.md) (use the [spec map](../reference/spec-map.md)
  to find the section).
* Edit comfortably: the [VSCode extension](../development/editor-extension.md)
  (highlighting, completion, live error squiggles, a Run button).
* Work on the compiler: [architecture](../internals/architecture.md) and
  [building and testing](../development/building-and-testing.md).
