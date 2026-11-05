---
description: "Write code as freely as JavaScript or Python and get speed that matches or beats C, on Windows and Linux"
type: Project
about: "cstar"
---

# cstar

## What This Is

cstar (formerly Pentest) is a Python-like language that compiles to native code through C, with speed that
matches or beats C. It is growing into a full Windows and Linux SDK with a built-in web server, AI training
with automatic GPU pickup, direct memory access, a package manager and user-defined rules.

## Core Value

Developers write code as freely as in JavaScript or Python and get speed that matches or beats C, on Windows
and Linux, with servers, packages and low-level memory access built in.

## Current State

| Attribute | Value |
|-----------|-------|
| Type | Application (programming language + toolchain) |
| Version | 0.2.1 (`cstar --version`), plus unreleased work in the tree |
| Status | Working compiler on Linux (and on Windows through WSL); Phase 1 done, rename to cstar next |
| Last Updated | 2026-11-04 |

## Requirements

### Core Features

- Cross-platform SDK: install once on Windows or Linux, no separate compiler needed
- Built-in web server for APIs (Node.js / Flask style) with JSON
- Native interop: inline C, calling C libraries (FFI), direct memory access
- Package manager for community packages (npm / pip style)
- Tensors with automatic GPU pickup, then AI training
- User-defined rules (operator overloading → decorators → templates → macros)

### Validated (Shipped)

- [x] Core language: control flow, collections, text/bytes, functions and closures, classes, exceptions, GC — 0.2.1
- [x] Cross-scope variable access (`variable.global/parent/<fn>`) — 0.2.1
- [x] Script-style code with no boilerplate (`main:` optional) — 0.2.1
- [x] Imports (`import`/`from`), HTTP client builtins, SIMD vectors (`fvec`/`ivec`) — in tree, unreleased
- [x] VSCode extension (highlighting, completion, hover, diagnostics, Run) — 0.2.1
- [x] Build and test from Windows through WSL; LF-only, byte-exact repository; docs that match the code — Phase 1
- [x] CI workflow for the four gates (warning-free build, tests, smoke test, docs checker) — Phase 1 (not yet run on GitHub)

### Active (In Progress)

None yet.

### Planned (Next)

- [ ] v0.3 cstar Foundation — rename, repo hygiene, known-issue fixes
- [ ] v0.4 Cross-platform SDK
- [ ] v0.5 Native Interop
- [ ] v0.6 Built-in Server & JSON
- [ ] v0.7 Package Manager
- [ ] v0.8 Tensors & Auto-GPU
- [ ] v0.9 AI Training
- [ ] v0.10 User-defined Rules

### Out of Scope

- `eval` / `exec` / `compile` — by design: programs are compiled ahead of time, no interpreter ships
- macOS — not part of the current vision; revisit after v0.4

## Target Users

**Primary:** developers who want scripting-language ease with native speed
- Security engineers writing scanners, parsers and network tools (the project's origin)
- API/backend builders (v0.6+) and AI developers (v0.8+)

**Secondary:** package authors in the cstar community (v0.7+)

## Constraints

### Technical Constraints

- `SPEC.md`, `docs/contracts/*` and `docs/reference/cross-scope-access.md` are normative; SPEC section numbers never change; quoted error messages are normative
- Optimisations must keep output byte-identical (stdout, stderr, exit code)
- Programs stay single-file native binaries; heavy optional libraries (GPU, ONNX) are loaded at runtime, never required
- Runtime depends on libc + libm + libcurl today
- The toolchain is Linux x86_64 only until v0.4; this Windows development machine builds it through WSL (Ubuntu 24.04)

### Business Constraints

- To be defined during /paul:plan

## Key Decisions

| Decision | Rationale | Date | Status |
|----------|-----------|------|--------|
| Rename the project to cstar | Project vision: the fastest and easiest language | 2026-11-04 | Active |
| Milestone order: rename → SDK → interop → server → packages → GPU → training → rules | Each milestone unlocks the next; the rename is cheapest now | 2026-11-04 | Active |
| GPU/AI libraries are loaded at runtime, never linked | The same binary must run on machines without a GPU | 2026-11-04 | Proposed |
| Auto-GPU for plain loops only when results are exact; tensor/AI code always uses GPU floats | Keeps the byte-identical output promise for normal code | 2026-11-04 | Proposed |
| Server uses a single-threaded event loop (Node style) | Matches the single-threaded GC | 2026-11-04 | Proposed |
| Packages come from git first, a hosted registry later | No server cost or registry attack surface at the start | 2026-11-04 | Proposed |
| AI: cstar's own API on top of an existing C engine (e.g. ggml) | Writing kernels for every GPU vendor would take years; confirm in research | 2026-11-04 | Proposed |
| Build the Windows checkout from WSL (`wsl -d Ubuntu-24.04 -- …`) | Keeps the Windows editor workflow; the toolchain is Linux-only until v0.4 | 2026-11-04 | Active |
| LF line endings everywhere; test expectations stored byte-exact (`-text`) | CRLF broke the harness; autocrlf had silently corrupted a test file | 2026-11-04 | Active |
| `examples/` stays private; docs use inline programs | Your choice: examples are local scratch | 2026-11-04 | Active |
| One branch per phase, a commit per task; push only with your approval | The repo is public; main changes only on request | 2026-11-04 | Active |

## Success Metrics

| Metric | Target | Current | Status |
|--------|--------|---------|--------|
| Test suite | 100% green on Linux and Windows | 372/372 in a fresh clone (WSL Ubuntu 24.04, 2026-11-04); CI ready, not yet run on GitHub; Windows-native not yet | On track |
| Loop/printing speed vs C (`-O3 -march=native`) | ≥ 1.0× | 0.96×–1.84× (hpcbench, 2026-10-18) | On track |
| Object-heavy speed vs C (`ptrlist`) | ≥ 0.8× (proposed) | 0.35× | At risk |
| Install → first program | One installer, no separate compiler | Needs clang + make on Linux | Not started |
| Server throughput vs Node.js | ≥ Node.js (proposed) | No server yet | Not started |

## Tech Stack / Tools

| Layer | Technology | Notes |
|-------|------------|-------|
| Compiler | C++17 (`src/`), built with clang++ | lexer → parser → modules → semantic → codegen |
| Backend | One C11 file compiled by `clang -O3 -march=native` | No IR; speed decisions made where C is emitted |
| Runtime | C11 (`runtime/cstar_rt.{h,c}`) | Embedded in the compiler; libc + libm + libcurl |
| Memory | Non-moving mark & sweep GC | Shadow-stack roots |
| Editor | VSCode extension (JavaScript) | `editors/vscode/` |
| Tests | Bash harness, Node smoke test, Python docs checker | `make test` is the gate |

## Links

| Resource | URL |
|----------|-----|
| Repository | https://github.com/Lord-hiren/cstar |
| Documentation | docs/README.md |

---
*PROJECT.md — Updated when requirements or context change*
*Last updated: 2026-11-04 after Phase 1*
