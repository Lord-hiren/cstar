# Pentest documentation

Pentest is a Python-like language for security tooling that compiles to native
Linux executables (through C). This is the map of everything written about it.
Current version: **0.2.1** (`pentest --version`).

## Start here

| If you want to… | Read |
|---|---|
| install it and run a program | [Getting started](guide/getting-started.md) |
| learn the language | [Language tour](guide/language-tour.md) |
| split a program across files | [Modules and imports](guide/modules.md) |
| avoid the common traps | [Gotchas and FAQ](guide/gotchas.md) |
| understand an error message | [Errors and diagnostics](guide/errors-and-diagnostics.md) |
| use the command line | [CLI reference](guide/cli.md) |
| find a builtin or method | [Builtins guide](guide/builtins.md) |
| know the exact rule | [`SPEC.md`](../SPEC.md) via the [specification map](reference/spec-map.md) |
| know what is currently broken | [Known issues](reference/known-issues.md) |
| work on the compiler or runtime | [Architecture](internals/architecture.md) → [Code generation](internals/code-generation.md) → [Runtime](internals/runtime.md) |
| run the tests or benchmarks | [Building and testing](development/building-and-testing.md), [Benchmarking](development/benchmarking.md) |
| change the editor extension | [Editor extension](development/editor-extension.md) |
| change these docs | [Documentation guide](development/documentation-guide.md) |

## Contents

### Guide — writing Pentest programs (`guide/`)

| Page | Contents |
|---|---|
| [getting-started.md](guide/getting-started.md) | requirements, building, first program, `run`/`build`/`check`, where files go |
| [language-tour.md](guide/language-tour.md) | the whole language in 16 sections, every example executed |
| [modules.md](guide/modules.md) | multi-file programs: `import`/`from`, resolution, selective loading, module initialization, zero runtime cost |
| [gotchas.md](guide/gotchas.md) | the surprises: `//`, `->` calls, `in`, comma-join, `and`/`or`, builtins as values, exact exception matching … |
| [errors-and-diagnostics.md](guide/errors-and-diagnostics.md) | compile-time vs runtime errors, common messages and fixes, exception classes, exit codes |
| [cli.md](guide/cli.md) | commands, exit codes, the run cache, the compile pipeline, environment |
| [builtins.md](guide/builtins.md) | all seventy-eight builtins, the method tables, files, stubs, deviations from Python |
| [sqli.md](guide/sqli.md) | worked SQL-injection walkthroughs (UNION-based GET, raw-POST templating) against a local lab on 127.0.0.1 |

### Reference (`reference/`) and the specification

| Page | Contents |
|---|---|
| [`../SPEC.md`](../SPEC.md) | the normative language specification (sections cited from code — numbering is stable) |
| [spec-map.md](reference/spec-map.md) | the SPEC by question, a section index, and document precedence |
| [SPEC §14](../SPEC.md#14-modules-and-imports-stage-2d) | normative: `import`/`from`, module resolution, selective loading |
| [cross-scope-access.md](reference/cross-scope-access.md) | normative: `variable.global` / `.parent` / `.<fn>` |
| [known-issues.md](reference/known-issues.md) | reproduced bugs and spec deviations, with workarounds |

### Contracts (`contracts/`) — normative, per stage

[README](contracts/README.md) · [M1](contracts/m1-semantics.md) ·
[M2 2A](contracts/m2a-semantics.md) · [M2 2B](contracts/m2b-semantics.md) ·
[M2 2C](contracts/m2c-semantics.md)

### Internals (`internals/`)

| Page | Contents |
|---|---|
| [architecture.md](internals/architecture.md) | pipeline, source map, AST, the five semantic phases, design decisions |
| [code-generation.md](internals/code-generation.md) | a real annotated `gen.c`, naming conventions, how each construct lowers, how to inspect output |
| [runtime.md](internals/runtime.md) | value model, heap objects, the call ABI, the garbage collector, exceptions, builtins |

### Performance (`performance/`)

[README](performance/README.md) · [optimization.md](performance/optimization.md) ·
baselines: [M1](performance/baseline-m1.md), [2B](performance/baseline-m2b.md),
[2C](performance/baseline-m2c.md)

### Development (`development/`)

[building-and-testing.md](development/building-and-testing.md) ·
[benchmarking.md](development/benchmarking.md) ·
[editor-extension.md](development/editor-extension.md) ·
[documentation-guide.md](development/documentation-guide.md)

### History (`history/`)

[README (timeline, naming, roadmap)](history/README.md) ·
[design.md](history/design.md) · [progress-m2b.md](history/progress-m2b.md) ·
[progress-m2c.md](history/progress-m2c.md) · and the root
[CHANGELOG](../CHANGELOG.md)

## Which documents are authoritative?

1. **Normative:** `SPEC.md`, `contracts/*`, `reference/cross-scope-access.md`. If the
   SPEC and a contract disagree, the contract governs — except that the
   unreleased v0.3 surface-syntax migration is front-end-only, so SPEC §4
   governs the surface form wherever a contract's examples predate it. Quoted
   error messages are
   normative (tests match on them).
2. **Informative:** everything else. If it disagrees with the SPEC, the SPEC wins.
3. **Records:** `history/*` and `performance/baseline-*` describe a past moment.

Where the *compiler* disagrees with the normative text, that is a bug —
[known issues](reference/known-issues.md) lists the ones found so far.

## Other places with documentation

* [`../editors/vscode/README.md`](../editors/vscode/README.md) — the extension for users.
* Source headers: each `src/*` and `runtime/*` file opens with a comment citing the
  SPEC/contract sections it implements.
* Test cases in `tests/cases/` double as executable examples; many open with a
  comment saying what they prove and which SPEC section they exercise.
