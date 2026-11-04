# Pentest

Pentest is a Python-like programming language for cybersecurity tooling —
scanners, parsers, network utilities and automation — that **compiles to
native executables**. You write short, readable `.pen` scripts (indentation
blocks, functions, collections, classes, exceptions, a `main:` block); the
`pentest` toolchain translates them to C and builds a standalone Linux binary, so
your tools run fast and ship as a single file with no interpreter to install.

**Version `0.2.1`** · Linux x86_64 · toolchain in C++17, runtime in C11 · one
third-party library (libcurl, for the HTTP builtins).

> **Status.** The language core is complete: control flow, collections,
> text/bytes, functions and closures, classes, exceptions, garbage collection,
> cross-scope variable access and 78 reserved builtins (43 Python-parity).
> **In this working tree (unreleased):** multi-file programs — `import`/`from`
> with selective loading ([guide](docs/guide/modules.md), SPEC §14) — and the
> `()` call-syntax overhaul. Not yet: generators, inheritance, a standard
> library (networking is limited to the HTTP client builtins) and non-Linux
> targets. Some bugs are known and
> documented — see [known issues](docs/reference/known-issues.md).

## Quick start

Requirements: Linux x86_64, `clang`/`clang++` (developed with clang 21; 14+ is the
stated minimum), GNU `make`, `sha256sum` and the libcurl dev headers. Details
in [getting started](docs/guide/getting-started.md) (including Windows through
WSL).

```bash
make                                    # builds build/pentest
echo 'print("Hello, Pentest!")' > hello.pen
build/pentest run hello.pen             # compile (cached) and run
build/pentest build hello.pen -o hello.pentest && ./hello.pentest
build/pentest check hello.pen           # parse + analyse only
make test                               # 372 checks (about a minute)
```

## A taste of the language

Collections, methods, destructuring and `text` with code-point semantics:

```python
main:
    hosts = ["web1", "web2"]
    hosts.append("web3")
    first, *rest = hosts
    print(first, len(rest))
    ports = {"http": 80, "https": 443}
    print(ports["https"], ports.get("ftp", 21))
    print(len("héllo"), "héllo"[1])
```

Output:

```text
web1 2
443 21
5 é
```

Closures, default and named arguments:

```python
make_adder(n):
    add(x, by = 1):
        return x + n + by
    return add

main:
    add2 = make_adder(2)
    print(add2(10))
    print(add2(10, by = 5))
```

Output:

```text
13
17
```

Classes, exceptions, identity and the garbage collector:

```python
class Host:
    __init__(self, name):
        self.name = name
        self.up = False

    probe(self, port):
        self.up = port == 443
        return self

main:
    h = Host("web1").probe(port=443)
    print(h.name, h.up, h is h)
    try:
        h.nope
    except AttributeError as e:
        print("caught:", e)
    print(collect() >= 0)
```

Output:

```text
web1 True True
caught: AttributeError: 'Host' object has no attribute 'nope'
True
```

Cross-scope variable access (`variable.global`, `variable.parent`,
`variable.<function>`):

```python
target = 1

reader():
    print("global:", variable.global target)
    variable.global target = 2

box():
    counter = 0

    bump():
        variable.parent counter += 1

    bump()
    bump()
    print("shared:", counter)

main:
    reader()
    print(target)
    box()
```

Output:

```text
global: 1
2
shared: 2
```

Continue with the [language tour](docs/guide/language-tour.md).

## Highlights

* **Script-style execution.** Top-level statements run in order; `main:` runs in
  place; function declarations are discovered first, so forward references work.
* **Dynamic values, compile-time checks.** Arity, named arguments, reserved names
  and undefined names are compile errors with `file:line:col` and a caret; all
  semantic errors are reported together.
* **Runtime errors are exceptions.** Catchable with `try`/`except`/`finally`; if
  uncaught they print `file:line: runtime error: …` and exit `1`.
* **Native speed where it can be proven.** Integer variables the compiler can prove
  are `int64` become straight C loops; see [performance](docs/performance/README.md).
* **Automatic memory management.** A non-moving mark & sweep collector reclaims
  everything, including cycles.
* **Python-parity builtins**, each compiled to one direct C call; `eval`/`exec`/
  `compile` are rejected at compile time by design.
* **Editor support.** A VSCode extension with highlighting, completion, hover, live
  error squiggles and a Run button ([setup](docs/development/editor-extension.md)).

Things that differ from Python on purpose: `//` is a comment (no floor division);
`()` is the call operator and `in` tests membership; `and`/`or` return `bool`;
an unparenthesised `a, b` joins into
text; builtins are call-only; exception matching is by exact class. See
[gotchas](docs/guide/gotchas.md).

## Documentation

Everything is indexed in **[docs/README.md](docs/README.md)**. The essentials:

| | |
|---|---|
| [Getting started](docs/guide/getting-started.md) | install, first program, where files go |
| [Language tour](docs/guide/language-tour.md) | the language, with executed examples |
| [Modules and imports](docs/guide/modules.md) | multi-file programs, selective loading, zero runtime cost |
| [Gotchas](docs/guide/gotchas.md) | what surprises Python programmers |
| [Errors and diagnostics](docs/guide/errors-and-diagnostics.md) | reading messages, exit codes |
| [CLI reference](docs/guide/cli.md) | `run`, `build`, `check`, the cache |
| [Builtins guide](docs/guide/builtins.md) | all 78 builtins, methods, files |
| [`SPEC.md`](SPEC.md) | the normative specification ([map by topic](docs/reference/spec-map.md)) |
| [Known issues](docs/reference/known-issues.md) | reproduced bugs with workarounds |
| [Architecture](docs/internals/architecture.md) | how the compiler and runtime fit together |
| [Building and testing](docs/development/building-and-testing.md) | tests, benchmarks, contributing |
| [Changelog](CHANGELOG.md) | what changed per version |

## CLI summary

| Command | Behaviour |
|---|---|
| `pentest <file>` / `pentest run <file> [args…]` | compile (content-hash cached) and execute |
| `pentest build <file> [-o out]` | emit a native executable (default `<basename>.pentest` in the current directory) |
| `pentest check <file>` | parse + semantic analysis only, including every imported declaration (strict); silent on success |
| `pentest --help`, `--version` | usage; prints `pentest 0.2.1` |

Source files end in `.pen` or `.pent`. Exit codes: `0` success, `1` compile/runtime
error, `2` usage error. Full reference: [CLI](docs/guide/cli.md).

## Repository layout

| Path | Contents |
|---|---|
| `src/` | the compiler: driver, lexer, parser, import linker (`modules.cpp`), semantic pass, C code generator, diagnostics |
| `runtime/` | the C11 runtime (`pentest_rt.{h,c}`) linked into every program |
| `tests/` | the harness (`run_tests.sh`), 365 cases in `tests/cases/`, and `check_docs.py` |
| `editors/vscode/` | the VSCode extension |
| `bench/` | benchmark runners, generated cases, C++ twins, recorded results |
| `tools/` | `embed.cpp`, which embeds the runtime into the compiler |
| `docs/` | all documentation ([index](docs/README.md)) |
| `build/` | build outputs created by `make` |

## Roadmap

Next, in the order set by the specification (§13): fix the
[known issues](docs/reference/known-issues.md); finish Stage 2D (generators on
top of the now-landed modules/imports); Stage 2E — tooling and performance,
possibly a direct LLVM backend. Beyond that: static typing, a standard library
(`net`, `web`, `proc`, `parse`, …) and other platforms. See
[history](docs/history/README.md).
