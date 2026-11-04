# Getting started

From a fresh checkout to a compiled native executable in a few steps.

## Requirements

| Need | Why | Verified with |
|---|---|---|
| Linux x86_64 | the only supported host *and* target | — |
| `clang` and `clang++` | `clang++` builds the toolchain (C++17); `clang` compiles every Pentest program (C11, `-O3 -march=native`) at run/build time. **`clang` must be on `PATH` whenever you use `pentest`**, not just when building it | clang 21.1.8 (README states 14+ as the minimum; older versions were not tested here) |
| GNU `make` | builds the toolchain | — |
| `sha256sum` (GNU coreutils) | optional: keys the compiled-program cache. Without it, caching is disabled and every `run` compiles (which also leaves `program` in `$TMPDIR` — [known issue 8](../reference/known-issues.md#8-a-cache-less-run-leaves-its-binary-in-the-temp-dir)) | coreutils 9.10 |
| `bash` | the test harness only | — |
| `node` (optional) | the VSCode extension's smoke test | v24 |
| `python3` (optional) | the benchmark case generators only | 3.x |

The toolchain itself is plain C++17/C11 with no third-party libraries. A
compiled Pentest program needs only libc and libm.

## Build the toolchain

```bash
make            # builds build/pentest
make test       # optional: runs the 368-check suite (about a minute)
```

`make` compiles `src/*.cpp`, precompiles the runtime once with the flags the
driver uses on demand, then embeds both the runtime sources and that object
into the binary (`tools/embed.cpp` generates `build/runtime_embed.h`) — so the
finished `build/pentest` is self-contained, does not read `runtime/` at run
time, and a first-ever `pentest file.pen` links the embedded object instead of
recompiling the runtime (see [optimization §5](../performance/optimization.md#5-compiled-artifact-caches)).

Check it works:

```bash
build/pentest --version     # pentest 0.2.1
```

Optionally put it on your `PATH` (the VSCode extension also looks in
`~/.local/bin`):

```bash
mkdir -p ~/.local/bin && ln -sf "$PWD/build/pentest" ~/.local/bin/pentest
```

## Your first program

Create `hello.pen`:

```python
print("Hello, Pentest!")
```

Output:

```text
Hello, Pentest!
```

Run it. `run` compiles through C with `clang`, caches the executable, and
executes it:

```bash
build/pentest run hello.pen          # same as: build/pentest hello.pen
```

Build a standalone executable you can copy anywhere on a compatible Linux
system (it needs no `pentest` install):

```bash
build/pentest build hello.pen -o hello.pentest
./hello.pentest
```

Only parse and analyse — fast, never calls `clang`, silent on success:

```bash
build/pentest check hello.pen && echo ok
```

## What a compile error looks like

Compile errors point at the exact line and column and exit with code `1`:

```python
host = "example.com"
print(hots)
```

Output:

```text
example.pen:2:7: error: undefined variable 'hots'
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
example.pen:2: runtime error: list index out of range
```

See [errors and diagnostics](errors-and-diagnostics.md) for the catalogue.

## Where files go

| Artifact | Location |
|---|---|
| `build` output | `<source basename>.pentest` in the **current directory**, or the `-o` path |
| `run` cache | `$XDG_CACHE_HOME/pentest/` (default `~/.cache/pentest/`): one `<sha256>.pentest` per distinct program and one `rt-<sha256>.o` compiled-runtime object shared by all programs |
| Scratch while compiling | `$TMPDIR/pentest-XXXXXX/` (default `/tmp`), removed afterwards |

The cache key covers the source, the toolchain version **and** the toolchain
binary itself, so rebuilding `pentest` automatically invalidates old entries.
Deleting `~/.cache/pentest` is always safe. Details: [CLI reference](cli.md#the-run-cache).

## Try the bundled examples

```bash
for f in examples/*.pen; do echo "== $f"; build/pentest run "$f"; done
```

| Example | Shows |
|---|---|
| `examples/hello.pen` | the smallest program, variables and `print` |
| `examples/arithmetic.pen` | operators and precedence |
| `examples/functions.pen` | declarations, forward references |
| `examples/main_flow.pen` | the `main:` execution model |
| `examples/classes.pen` | classes, exceptions, identity, `collect()` |

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
