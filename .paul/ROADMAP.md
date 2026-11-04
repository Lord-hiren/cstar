---
description: "cstar — milestone and phase structure"
type: Roadmap
about: "cstar"
---

# Roadmap: cstar

## Overview

From a Linux-only compiler called Pentest to cstar: a cross-platform SDK with native interop, a built-in
server, packages, GPU-accelerated AI and user-defined rules. Milestones are ordered by priority. Each one
unlocks the next:

- **The rename comes first** because it's cheapest now.
- **The SDK builds the platform layer** that everything after it depends on.
- **FFI makes calling libraries easy**, including CUDA, Vulkan, ONNX and sockets.
- **Packages ship before GPU work** so GPU libraries can be distributed as packages.
- **User-defined rules come last**: they're the hardest milestone and nothing depends on them.

## Milestones

| # | Version | Name | Phases | Status |
|---|---------|------|--------|--------|
| 1 | v0.3 | cstar Foundation | 1–4 | 🚧 In Progress |
| 2 | v0.4 | Cross-platform SDK | 5–10 | 📋 Planned |
| 3 | v0.5 | Native Interop | 11–14 | 📋 Planned |
| 4 | v0.6 | Built-in Server & JSON | 15–19 | 📋 Planned |
| 5 | v0.7 | Package Manager | 20–23 | 📋 Planned |
| 6 | v0.8 | Tensors & Auto-GPU | 24–28 | 📋 Planned |
| 7 | v0.9 | AI Training | 29–32 | 📋 Planned |
| 8 | v0.10 | User-defined Rules | 33–36 | 📋 Planned |

## Current Milestone

**v0.3 cstar Foundation** (v0.3.0)
Status: In progress
Phases: 0 of 4 complete

| Phase | Name | Plans | Status | Completed |
|-------|------|-------|--------|-----------|
| 1 | Dev environment & repo hygiene | 3 | In progress | - |
| 2 | Rename to cstar | 3 | Not started | - |
| 3 | Fix known issues | 2 | Not started | - |
| 4 | Release v0.3.0 | 1 | Not started | - |

## Phase Details

### Phase 1: Dev environment & repo hygiene

**Goal:** The project builds and passes its tests on a Linux toolchain reachable from this Windows machine,
and the repo stops tracking build output.
**Depends on:** Nothing (first phase)
**Research:** Unlikely (standard setup)

**Scope:**
- Linux toolchain access (WSL Ubuntu + clang, make, libcurl dev) and a recorded baseline of `make test` and the smoke test
- Stop tracking `build/` and add it to `.gitignore`; remove the committed `scan.log`
- Fix stale claims in the docs: "no third-party dependencies", "no networking yet", test counts
- Linux CI on GitHub Actions running `make test` and the smoke test

**Plans:**
- [x] 01-01: Toolchain setup, LF line endings (`.gitattributes`) and baseline test run — 2026-11-04
- [ ] 01-02: Repo hygiene and stale-doc fixes
- [ ] 01-03: Linux CI

### Phase 2: Rename to cstar

**Goal:** Everything user-facing says cstar: the CLI, source extension, environment variables, cache folder,
VSCode extension and docs.
**Depends on:** Phase 1 (a passing baseline to compare against)
**Research:** Unlikely (mechanical, about 1,060 mentions in 114 files)

**Scope:**
- Decide first: the source extension (`.cstar` or `.cst`; not `.cs`, which is C#), whether `.pen`/`.pent` keep working during a transition, and whether internal C prefixes (`pt_`, `pentest_rt`) are renamed
- Driver: binary name, `--version`, usage text, extension checks, `CSTAR_PATH`, cache folder
- Tests: harness, `.err` expectations, the extension check
- VSCode extension: language id, grammar, snippets, smoke test
- Docs, SPEC, README, CHANGELOG (SPEC section numbers unchanged)

**Plans:**
- [ ] 02-01: Naming decisions and driver/CLI rename
- [ ] 02-02: Tests and editor extension
- [ ] 02-03: Docs and SPEC

### Phase 3: Fix known issues

**Goal:** The three open known issues are closed, each with a regression test.
**Depends on:** Phase 2
**Research:** Unlikely (#9 may need a design spike)

**Scope:**
- #9: text concatenation in a loop is O(n²). It matters for servers, which build a lot of text.
- #8: a run that can't use the cache leaves its binary in the temp folder
- #7: `format` zero-padding combined with grouping doesn't regroup

**Plans:**
- [ ] 03-01: In-place text growth (#9)
- [ ] 03-02: `format` regrouping (#7) and cache-less cleanup (#8)

### Phase 4: Release v0.3.0

**Goal:** A tagged v0.3.0 release under the cstar name.
**Depends on:** Phase 3
**Research:** Unlikely

**Scope:**
- Bump the version to `cstar 0.3.0`; update the CHANGELOG and the counts in the docs
- Tag the release on GitHub

**Plans:**
- [ ] 04-01: Version bump, changelog, tag

## 📋 Planned Milestones

### v0.4 Cross-platform SDK

**Goal:** Install once on Windows or Linux and run programs with no separate compiler.
**Prerequisite:** v0.3 complete

| Phase | Focus | Research |
|-------|-------|----------|
| 5 | Platform layer in the driver: process start, temp folders, paths, cache location; built-in SHA-256; CPU fingerprint via cpuid | Unlikely |
| 6 | Portable runtime: replace POSIX-only code, file APIs, HTTP client on Windows | Likely (libcurl vs WinHTTP) |
| 7 | Bundled C compiler: TinyCC for fast dev runs, clang for release builds | Likely |
| 8 | Builtins for program arguments and environment variables | Unlikely |
| 9 | Cross-platform test harness and a Windows + Linux CI matrix | Unlikely |
| 10 | Installers (Windows, Linux) and the VSCode extension finding the SDK | Likely |

### v0.5 Native Interop

**Goal:** Inline C, calls into any C library, and direct memory access.
**Prerequisite:** v0.4 complete

| Phase | Focus | Research |
|-------|-------|----------|
| 11 | Interop design: new SPEC section and GC safety rules | Likely |
| 12 | Inline C blocks | Unlikely |
| 13 | FFI: call functions in shared libraries (dlopen / LoadLibrary) | Likely |
| 14 | Memory access: a pointer/buffer type; other-process memory if chosen | Likely |

### v0.6 Built-in Server & JSON

**Goal:** Build HTTP APIs in a few lines, like Node.js or Flask.
**Prerequisite:** v0.5 complete

| Phase | Focus | Research |
|-------|-------|----------|
| 15 | JSON parse and stringify | Unlikely |
| 16 | Cross-platform TCP sockets | Likely |
| 17 | Event loop (epoll/IOCP vs libuv) | Likely |
| 18 | HTTP server API: routes, request/response, static files | Unlikely |
| 19 | Object- and text-heavy speed (the `ptrlist` 0.35× gap); benchmarks vs Node.js and Flask | Likely |

### v0.7 Package Manager

**Goal:** The community can publish and install cstar packages.
**Prerequisite:** v0.6 complete

| Phase | Focus | Research |
|-------|-------|----------|
| 20 | Manifest (`cstar.toml`) and lock file design | Likely |
| 21 | `cstar add` / `install` from git: version resolution, cache | Unlikely |
| 22 | `cstar new` project scaffolding and a publishing guide | Unlikely |
| 23 | Hosted registry and package security (checksums, signing) | Likely (may be deferred) |

### v0.8 Tensors & Auto-GPU

**Goal:** Tensor math that automatically uses any available GPU, falling back to the CPU.
**Prerequisite:** v0.7 complete

| Phase | Focus | Research |
|-------|-------|----------|
| 24 | `tensor` type on the CPU, built on the existing SIMD vectors | Unlikely |
| 25 | GPU backend choice: ggml vs vendor libraries vs our own | Likely |
| 26 | Device detection and auto-pickup (CUDA, ROCm, DirectML, Vulkan, CPU), loaded at runtime | Likely |
| 27 | Auto-offload of loops the compiler proves safe (exact-results rule) | Likely |
| 28 | Running pre-trained models (ONNX Runtime) | Likely |

### v0.9 AI Training

**Goal:** Train neural networks in cstar on any GPU.
**Prerequisite:** v0.8 complete

| Phase | Focus | Research |
|-------|-------|----------|
| 29 | Autograd | Likely |
| 30 | Layers, loss functions, optimisers | Unlikely |
| 31 | Data loading, batching, model save/load, ONNX export | Likely |
| 32 | Training benchmarks vs PyTorch | Unlikely |

### v0.10 User-defined Rules

**Goal:** Users can extend the language, like Lisp.
**Prerequisite:** v0.9 complete

| Phase | Focus | Research |
|-------|-------|----------|
| 33 | Operator overloading (`__add__`, `__eq__`, …) | Unlikely |
| 34 | Decorators | Unlikely |
| 35 | Templates (compile-time substitution) | Likely |
| 36 | Macros (code that runs at compile time) | Likely |

## Backlog (unscheduled)

- Generators and async/await (may be pulled into v0.6 for the server)
- Inheritance, lambdas, multi-name `for a, b in …`, f-strings
- Standard library modules: `net`, `web`, `proc`, `parse` (SPEC §13)
- A direct LLVM backend (SPEC §13, Stage 2E)

---
*Roadmap created: 2026-11-04*
*Last updated: 2026-11-04*
