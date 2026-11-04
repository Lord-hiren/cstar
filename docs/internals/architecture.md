# Architecture

How a `.pen` file becomes a native executable, and where each step lives in the
source tree. The normative summary is SPEC §11; this page is the guided tour.

## The idea (SPEC §11.1, "Approach A")

Pentest is a **source-to-C translator plus a small C runtime**. The compiler
turns a program into one C11 translation unit; `clang -O3 -march=native` does
the native code
generation and optimisation; the result is linked against a runtime library
that provides the value model, the garbage collector, exceptions, collections
and every builtin. There is **no IR and no separate optimiser pass**: every
performance decision is made at the point where C is emitted
([performance](../performance/optimization.md)).

```text
 prog.pen
    │
    ▼  src/lexer.cpp     tokens, INDENT/DEDENT, comments, literals     (first error stops)
    ▼  src/parser.cpp    recursive descent → AST (src/ast.h)           (first error stops)
    ▼  src/modules.cpp   resolve imports, detect cycles, mangle,       (run/build: prune
    │                    flatten, prune unused imported declarations   unused code)
    ▼  src/semantic.cpp  discovery, name resolution, validation,       (collects ALL errors)
    │                    capture analysis, builtin/arity checks
    ▼  src/codegen.cpp   AST → gen.c  (one C11 translation unit)
    │
    ▼  src/main.cpp      writes gen.c (+ runtime sources if needed); links
    │                    the runtime object embedded by `make`, or a cached
    │                    rt-<key>.o;  clang -O3 -march=native  gen.c  …  -lm
    ▼
 native executable  ──links──►  runtime/pentest_rt.{c,h}  (libc + libm only)
```

A program with no imports returns from `link_imports` immediately, so the
module layer adds no traversal to the common single-file case
([modules](../guide/modules.md)).

The driver (`src/main.cpp`) owns the CLI, the temp directory, the `run` cache,
and the `clang` invocations ([CLI reference](../guide/cli.md)). Everything up to
and including code generation runs in-process in one C++17 binary.

## Source map

Sizes are approximate lines as of `0.2.1`.

| Path | Lines | Role |
|---|---|---|
| `src/main.cpp` | ~620 | CLI driver, temp dir, cache key (`sha256sum` via `popen`), runtime-object cache, `clang` invocation, exit codes |
| `src/lexer.{h,cpp}` | ~870 | tokens; indentation stack → `INDENT`/`DEDENT`; the four comment forms; int/float/text/bytes literals; maximal-munch operators |
| `src/parser.{h,cpp}` | ~1570 | recursive descent over the EBNF in SPEC §4; precedence climbing; comma-join and destructuring; the contextual `variable.<q> <n>` atom |
| `src/ast.h` | ~340 | the AST shared by frontend and backend: `Expr`, `AssignTarget`, `Stmt`, `FuncDecl`, `MainDecl`, `TopLevel`, `Program`, plus the semantic annotations |
| `src/semantic.{h,cpp}` | ~2090 | the five-phase semantic pass (below) |
| `src/modules.{h,cpp}` | ~1100 | import linker: specifier resolution, cycle detection, `__ptm<id>_<name>` mangling, flattening, reachability prune |
| `src/builtins.h` | ~120 | the Python-parity builtin table: slots, arity, variadic/dict/slice forms; stub and rejected-name predicates |
| `src/codegen.{h,cpp}` | ~4070 | the C emitter and its analyses (native-int lane, definite assignment, allocation-free proof) |
| `src/diagnostics.{h,cpp}` | ~85 | `Diagnostics`: located/unlocated errors with caret display; error counting |
| `src/frontend.h` | ~25 | the two entry points: `parse_source` and `semantic_check` |
| `runtime/pentest_rt.h` | ~1000 | the runtime's public contract: `pt_value`, tags, constructors, the function ABI, GC and exception API, inline fast paths |
| `runtime/pentest_rt.c` | ~5850 | the runtime implementation |
| `tools/embed.cpp` | ~140 | build-time helper: turns the runtime sources, the precompiled object and the build host's CPU fingerprint into `build/runtime_embed.h` |
| `Makefile` | — | `clang++ -std=c++17 -Wall -Wextra -O2`; dependency files; precompiles the runtime and embeds it |

## The frontend contract (`src/frontend.h`)

* `parse_source(path, source, Program&, Diagnostics&)` lexes and parses. On the
  **first** lexical or syntax error it reports through `Diagnostics` and returns
  `false`. The semantic pass is not run.
* `link_imports(Program&, entry_path, strict, LinkResult&, Diagnostics&)`
  (`src/modules.h`) expands imports after parsing and before semantic analysis.
  It parses each imported module, rejects cycles, mangles imported declarations,
  flattens the module graph into the entry AST, and in `run`/`build` mode
  (`strict == false`) prunes unused imported declarations. No imports → returns
  immediately.
* `semantic_check(Program&, Diagnostics&)` annotates the AST and reports **all**
  semantic errors; returns `false` if there were any.
* `generate_c(const Program&, out_path)` (`src/codegen.h`) is only called on a
  program that passed both.

`pentest check` runs parse → link (strict) → semantic, which is why it is fast
and never needs `clang`. Strict linking validates every imported declaration,
including ones `run` would prune
([modules § selective loading](../guide/modules.md#selective-loading-you-only-pay-for-what-you-name)).

## The AST in one screen

`Program` holds `items` (an ordered list of `TopLevel`: a statement, a function
declaration or the `main:` block), plus what the semantic pass fills in:

* `functions` — *every* function including nested ones, in declaration order;
* `globals` — global names in discovery order (one C `static` each);
* `has_main`;
* `pins` / `pin_globals` — bindings fixed to static storage by cross-scope
  access ([cross-scope access](../reference/cross-scope-access.md)).

`FuncDecl` carries parameters/defaults/vararg/kwarg, the body, and annotations:
mangled `c_name` (`pt_fn_<name>`), ordered `locals`, which params/locals are
**cells** (captured by a nested function, so heap-boxed), `captures`, whether
it is nested or a method, and the static arity bounds (`min_args`/`max_args`)
used for compile-time call checking. Names are resolved to a `VarRes`
(param / local / global) on every `Var`, `Assign` and `For` node.

## The semantic pass — five phases (`src/semantic.cpp`)

| Phase | What it does |
|---|---|
| 1. declaration discovery | registers every top-level function, class (and its methods) and the `main:` block — never runs bodies; this is what makes forward references legal |
| 2. globals | collects global names from top-level and `main:` assignments; reports reserved-name misuse and the remaining name collisions (a `class` against a function, a nested declaration against an enclosing parameter/local/nested function) — a plain variable *may* share a function's name since 0.3 (the variable/function namespace split, SPEC §8) |
| 3. top-level and `main:` statements | resolves names in global scope |
| 4. function bodies | resolves every function body (top-level and nested, recursively): locals, parameter checks, `global`/`nonlocal` placement, static call-binding checks, break/continue/return placement, transitive capture analysis (boxing) |
| 5. cross-scope access | after every function's locals and boxing are final, resolves each recorded `variable.<qualifier> <name>` site and pins the bindings it references |

Phase 5 runs last on purpose: it lets `variable.<fn> x` target a function
declared *later* in the file.

## Design decisions worth knowing

* **Dynamic values, static knowledge.** Values are tagged at run time, but the
  compiler resolves everything it can: callee identity (so arity and named-arg
  errors are compile-time), builtin shapes (one direct C call), which variables
  are provably integers (the native lane).
* **Errors are first-class.** Every operation that can fail receives its source
  line number as a constant; runtime errors become exception instances and the
  uncaught printer reproduces the exact `<file>:<line>: runtime error:` format.
* **The runtime is embedded.** `build/pentest` carries `pentest_rt.{c,h}` inside
  it and writes them out per compile. A single self-contained binary is the whole
  toolchain besides `clang`.
* **Conservative lanes.** Each optimisation lane exits on anything it cannot
  prove, rather than risk a behaviour change ("poisoning").
* **Section numbers are API.** Source comments cite `SPEC §…` and contract
  sections; keep them stable (see the
  [documentation guide](../development/documentation-guide.md)).

## Where to go next

* What the emitted C looks like: [code generation](code-generation.md).
* The value model, GC, exceptions and builtins: [runtime](runtime.md).
* Why it is fast, and how that is validated: [optimization](../performance/optimization.md).
* How the tests exercise all of this: [building and testing](../development/building-and-testing.md).
