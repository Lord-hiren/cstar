# Runtime internals

`runtime/pentest_rt.{h,c}` (about 10,400 lines of C11; needs libc, libm and libcurl) is linked
into every Pentest program. This page explains how it is organised and why. The
public contract is the header; the semantic contracts are SPEC §5–§9 and the
stage contracts. Anything marked *runtime-private* in the source is not part of
the contract between compiler and runtime.

## How the runtime is shipped

* `tools/embed.cpp` converts `pentest_rt.c` and `pentest_rt.h` into
  `build/runtime_embed.h`, which is compiled into `build/pentest`.
* `make` also precompiles the runtime with the exact on-demand flags
  (`clang -std=c11 -O3 -march=native -c`) and embeds that object plus the
  build host's `/proc/cpuinfo` `flags` line. A first-ever compile installs and
  links the embedded object, so the cold run skips the ~6 s runtime compile;
  the `flags` guard makes a binary copied to another CPU fall back safely.
* For each compile the driver writes both sources into a scratch directory. If
  the embedded object is unusable (CPU mismatch, old build), the runtime is
  built **once per distinct content, flags and CPU** into
  `$XDG_CACHE_HOME/pentest/rt-<sha>.o`; later programs only compile their own
  `gen.c` and link against that object ([CLI reference](../guide/cli.md#the-run-cache)).
  Changing any byte of the runtime (even a comment) changes the key, so the first
  run after editing it recompiles the runtime.
* The runtime needs only **libc, libm and libcurl** (libcurl backs the HTTP
  builtins) — no pthread, no other libraries.

## The value model

```c
typedef struct pt_value {
  pt_tag tag;
  union { int64_t i; double d; bool b;
          struct { const char* p; size_t len; } s;     /* text and bytes */
          struct { int64_t start, stop, step; } r;     /* lazy range */
          struct pt_obj* o; } as;                      /* heap objects */
} pt_value;
```

A `pt_value` is a 16-byte tagged union passed and returned **by value**.
Tags: `PT_UNSET` (zero — the initial state of all static storage), `PT_NONE`,
`PT_INT`, `PT_FLOAT`, `PT_BOOL`, `PT_STRING` (this is *text*; UTF-8, may contain
NUL), `PT_BYTES`, `PT_RANGE`, then the heap tags `PT_LIST`, `PT_MAP`, `PT_SET`,
`PT_TUPLE`, `PT_FUNCTION`, `PT_CLASS`, `PT_EXCEPTION`, `PT_INSTANCE`,
`PT_BOUND_METHOD`, `PT_ITERATOR`, `PT_FILE`, and the internal sentinel
`PT_MISSING`.

* `PT_MISSING` marks an omitted argument slot in a function's `bound[]` vector;
  it must never reach a user operation (hitting one is reported as an internal
  error).
* `text` and `bytes` share one payload (`pointer + length`). Text is immutable;
  literals point at static storage, computed text at GC-registered buffers.
  `len`, indexing and slicing on text are code-point based; the runtime has a
  UTF-8 helper layer for that.
* `range` is three int64 fields — never materialised.

### Heap objects

Every non-scalar value points at a `pt_obj`, a tagged struct whose payload
depends on the kind:

| Kind | Payload |
|---|---|
| list, tuple | one growable array `{items, len, cap}` (tuples never grow) |
| map, set | one **insertion-ordered open-addressed hash table**: an append-only entry array (`key`, `val`, `hash`, `used`) plus an index table; deletes leave tombstones so order stays stable. Sets ignore `val` |
| function | `{ pt_funmeta* meta, pt_cell** env, env_n }` |
| class | `{ name, is_exc, methods table }` — builtin exception classes and user classes share it |
| exception | `{ class, message, line }` |
| instance | `{ class, n_inl, attrs table }` — the first **two** attributes live in a compact inline tail of the object's own block (key + value pairs; a zeroed key is the tombstone, deletes never reuse — `vars()`/`dir()` keep insertion order); further fields spill into the attrs table, which stays NULL-backed until the third field arrives |
| bound method | `{ receiver, function }` |
| iterator | pointer to malloc'd iteration state (only programs that call `iter()` allocate one) |
| file | `{ FILE*, binary, writable, path, next_open }` |
| fvec, ivec | `{ len, data }` — `data` is a 64-byte-aligned lane buffer (`double*`/`int64_t*`, capacity rounded up to a multiple of 8, **not** zero-filled — every producer fills `[0, len)` and nothing reads past `len`). A **leaf** payload: the GC marks the object but never traces into `data`; the sweep parks the buffer in the lane-buffer reuse pool (below) rather than freeing it |

Equality is total (`pt_equal` never raises); hashing follows it, so `1` and
`1.0` collide on purpose and `True` does not. Unhashable kinds raise
`unhashable type: '<t>'`.

## Functions, closures, and the call ABI

Every function — top-level, nested, method — has the signature
`pt_value f(pt_value* bound, pt_cell** env)`. A `pt_funmeta` (static const, one
per declaration) records name, parameter names, required count, a default mask,
and vararg/kwarg flags. Function **values** are `{meta, env}`.

* **Static calls** (callee known at compile time) skip the binder: the call site
  fills `bound[]` itself. Arity errors were already compile errors.
* **Dynamic calls** use `pt_call`, which binds positionals then names, collects
  extras into a fresh list (`*args`) or map (`**kwargs`), and raises the binding
  errors with the exact documented wording.
* **Defaults** arrive as `PT_MISSING` and are evaluated by the callee's entry
  code, per call, in parameter order — so they are fresh each call and may read
  globals and earlier parameters.
* **Closures**: a captured local lives in a heap `pt_cell { pt_value v; }`
  created in the enclosing function's prologue, initialised to `unset` so the
  used-before-assignment guard still applies. A nested function's `env` is an
  array of cell pointers; capture is transitive.
* **Pinned bindings** (`variable.`) use program-lifetime statics, or an
  *immortal* cell when also captured ([cross-scope access](../reference/cross-scope-access.md)).

## Garbage collection (SPEC §7.3, contract m2c §1)

A **non-moving mark & sweep** collector over a registry of blocks.

* **Registry.** Every heap allocation that outlives its operation — objects,
  text/bytes buffers, closure cells — is a block: a header `{next, prev, size,
  kind, marked}` followed by a max-aligned payload, all linked in one list.
  Static literals and builtin singletons are never registered. Because nothing
  moves, `is` identity stays valid across collections. Heap text/bytes
  buffers carry the block kinds `PT_GC_TEXT`/`PT_GC_BYTES` and are additionally
  listed in a payload-pointer set: a `PT_STRING`/`PT_BYTES` value marks its
  block only when its pointer is exactly a registered buffer base, which is
  why every producer must allocate through `pt_gc_buf(PT_GC_TEXT/…)`, never
  the value tag (passing the value tag hid the buffer from the collector —
  fixed, with `tests/cases/gc_text_liveness.pen` as the regression).
  Text slices copy; text/bytes iteration pins the source object (`it.src`),
  so a cursor cannot outlive its buffer.
* **Roots.** (1) the global root vector (one pointer per program global, handed
  to `pt_init`); (2) **shadow-stack frames** — each generated function registers
  the addresses of all its locals and temporaries with `pt_frame_begin` and pops
  them on every return path; (3) the *protect stack* (`pt_gc_protect`) for values
  that live only in C locals across a safe point, such as a callee's `bound[]`
  vector; (4) in-flight exception handler values; (5) the builtin exception-class
  singletons; (6) the immortal pinned cells.
* **Safe points.** Collections happen *only* at `pt_gc_maybe()` calls: function
  entries, loop back-edges (omitted for allocation-free bodies) and top-level
  statement boundaries — never in the middle of a runtime operation. So no
  runtime function needs to defend against a collection.
* **Trigger.** `pt_gc_maybe` collects when registered bytes exceed a limit that
  starts at 1 MiB and is reset after each collection to twice the live size (never
  below 1 MiB). `collect()` forces a collection. A vector's lane buffer counts
  toward these registered bytes (added in `pt_vec_alloc`, subtracted on sweep) —
  the ~250 B `pt_obj` header alone is invisible, so a loop churning 64 MB
  vectors would otherwise never cross the floor, never collect, and leak + re-fault
  a fresh buffer every statement.
* **Lane-buffer reuse pool.** Vector lane buffers (≥ 64 KB) are not returned to
  the allocator on sweep; they are parked in exact-size buckets and handed
  straight back out by the next `pt_vec_alloc` of the same capacity. This
  removes the per-statement `mmap`/`munmap`/page-fault round-trip that dominated
  fused kernels on 64 MB working sets (the kernel itself runs at DRAM speed;
  re-faulting a fresh buffer every pass does not). Parked buffers may hold stale
  bytes — safe because every producer fills `[0, len)` and nothing reads past
  `len`; the pool's total is capped so a program touching many distinct large
  sizes cannot pin memory.
* **Swept-block parking (GC blocks).** Dead blocks of the churn-prone kinds
  (cells, lists, tuples, functions, iterators, fvec/ivec objects, and — since
  the text-pooling wave — TEXT/BYTES payloads) are parked in exact-
  (size, kind) buckets instead of being `free`d, and the next
  `pt_gc_new_full` of the same shape hands them straight back out: no
  `malloc`/`free` round trip per cell or text buffer in a churn loop. A
  parked block is unlinked from the registry (the tracer and sweep never see
  it), its whole header is re-initialized on pop (so a popped text block
  never inherits a stale spare-capacity flag), and popped TEXT/BYTES
  payloads are re-registered in the code-point tset with a bumped
  allocation generation — a memo keyed on the old (payload, length,
  generation) can never be impersonated by the block's next life. The pool
  is capped by both block count and total parked bytes, so a program that
  churns many sizes — including large texts — and stops cannot pin memory.
* **Mark.** Iterative with an explicit worklist (no C-stack recursion on deep
  structures). Cycles are collected like any garbage. Every heap-object tag
  reachable from a root marks its block — including the leaf kinds with no
  members to trace: an open file (`PT_FILE`) and a boxed iterator
  (`PT_ITERATOR`) must be marked or the sweep closes the handle / frees the
  source out from under a live reference (both tags were once missing from
  the mark switch — fixed, with `tests/cases/gc_roots.pen` as the regression).
* **Sweep.** Walks the registry, freeing unmarked blocks (and closing files,
  freeing iterator state, parking vector lane buffers and unmarked GC blocks
  of the reusable kinds into their reuse pools — see the swept-block-parking
  bullet above). The count of freed blocks is what `collect()` returns.
* **Not provided:** finalizers, weak references, heap introspection.

Why the discipline matters: generated code clears temporaries at statement
boundaries and registers every temp, so the collector's view of liveness is exact
at every safe point. Inside loop bodies the eager clear is skipped — emission is
A-normalized (every temp is assigned before its next read), so the statement-tail
`pt_unset()` there is a dead store on the back-edge path, and a registered slot
holding a stale pointer is only ever *marked* (never swept-then-read) at a
collection; the trade is that one extra iteration of garbage stays pinned.

## Exceptions (SPEC §6.10, contract m2c §2)

* A **handler stack** of `pt_handler { jmp_buf env, frame depth, protect depth,
  call depth, pending value, kind }` — `setjmp`/`longjmp`, no threads.
* A `try` lowers to a `setjmp` landing pad in the *generated* function (a
  `setjmp`'d frame must not have returned). `pt_try_enter` pushes the handler
  recording the current shadow-frame and protect-stack depths; `pt_throw` pops
  it, **restores those depths** (so the GC's view stays consistent after the
  unwind), stores the exception and `longjmp`s.
* `pt_runtime_error_as(class_name, line, fmt, …)` is how runtime code raises a
  mapped exception. `pt_runtime_error(line, fmt, …)` raises `RuntimeError`.
* With an **empty handler stack**, `pt_throw` runs the *uncaught printer*:
  `<file>:<line>: runtime error: <message>` on stderr, then `exit(1)`. The class
  name is intentionally not printed — this keeps the uncaught format
  byte-identical to the pre-exception milestones.
* Twelve builtin classes are created as singletons in `pt_init`. Matching
  (`pt_exc_matches`) is **exact identity** of the exception's class pointer — no
  inheritance exists.
* `finally` is lowered by the compiler, not the runtime: it duplicates the body
  onto normal, `break`/`continue`/`return`, and unwind edges.

Every runtime error — including the `int()`/`float()` text-parse failures —
reaches user code through this machinery as a catchable instance.

## Classes and instances (SPEC §5.12)

A class value owns a method table (`name → function`). `pt_class_call`
constructs an instance and, if `__init__` exists, calls it with the instance
prepended; reading `obj.name` checks instance fields, then methods (yielding a
bound method), else raises `AttributeError`. Attributes are plain table entries;
there is no descriptor protocol, inheritance or class attribute. Classes,
instances and bound methods are unhashable.

## Iteration

One protocol (`pt_iter_*`) serves `for`, destructuring, `in`-style helpers and
the parity builtins. It iterates `range` lazily, `text` by code point, `bytes` as
ints, sequences by element, maps by key and sets by element; boxed iterators
(`iter()`/`next()`) wrap the same state. Mutation during iteration is undefined.

## Builtins and methods

* Each Python-parity builtin is one exported `pt_builtin_<name>`; the fifteen
  original builtins have dedicated `pt_builtin_*` entry points too. The slot
  layouts are generated from `src/builtins.h` at compile time, so the runtime
  receives fixed slots with `pt_missing()` for omitted optionals.
* The five vector builtins (`fvec`, `ivec`, `fvec_fill`, `ivec_fill`, `dot`)
  are `pt_builtin_*` entry points registered with the original-15 name rules
  (call-only, positional-only); every vector operator and reduction is
  hand-vectorized as explicit `pt_vd`/`pt_vi` lanes (`PT_VW` per target
  feature, scalar tail) — never left to the auto-vectorizer — and `sum`/
  `min`/`max` take direct lane walks over `fvec`/`ivec` arguments.
* Methods on builtin types dispatch through `pt_call_method` by receiver tag and
  method name (list/map/set/text/bytes/file families).
* `globals()`, `locals()`, `vars()`, `dir()` receive a **`pt_name_slot` table built
  at the call site**; the runtime never inspects live frames.
* `format()` is a self-contained implementation of a documented subset of
  Python's format mini-language; `repr`/`ascii` have their own escaper.
* Open files are tracked in a registry so exit and the GC sweep close them.

### Output fast path (`print`, `input`)

`print()` formats the five common scalar cases (int/float/bool/None/text)
without any heap allocation: ints go through `pt_itoa_ll` (byte-identical to
`snprintf("%lld")`, unsigned-space negation for `INT64_MIN`) and floats through
a fixed stack buffer, and the bytes land in a runtime-owned 256 KB line buffer
(`PT_OUT_CAP`) that is flushed with a raw `write(1)`. The buffer is flushed
**at the end of every `print` when stdout is a TTY** (SPEC §5.11.5 Python
`line_buffering=True` parity), before `input()` blocks on a prompt, and from an
`atexit` hook covering `main` return and `exit(1)` — so program output order is
unchanged and byte-identical. The uncaught-error printers flush stdout before
writing to stderr so a crash message can never jump ahead of pending output.
A hard signal (SIGPIPE/abort/segv) loses the unflushed tail — the same
contract C's stdio has. Complex types (list/map/…) still build a display
buffer; only its formatting of scalars is cheaper. Net effect: 1M
`print(a)` runs 0.21s → 0.069s on the reference machine — faster than a C
`printf` reference loop ([optimization §2](../performance/optimization.md)).

## Inline fast paths in the header

`pentest_rt.h` defines `static inline` helpers so the C compiler can fold them
into generated code: `pt_guard`, `pt_truthy`, the shadow-stack frame push/pop
(`pt_frame_begin`/`pt_frame_end`), the protect-stack push/pop
(`pt_gc_protect`/`pt_gc_unprotect`), the dynamic numeric operators
`pt_add_f … pt_ne_f` (identical tag checks to the out-of-line versions, falling
back on any mismatch), and the native-int helpers (`pt_nabs`, `pt_npow`,
`pt_nmin`, `pt_nmax`, `pt_nread_g`, …) used by the native lane. Behaviour is
bit-for-bit the same as the slow path; only the call disappears
([optimization §2, §4](../performance/optimization.md)).

## Reading the source

`pentest_rt.c` is organised in banner-commented sections, in this order:
process state → GC state → exception state → error reporting → helpers → GC
registry and heap-object layout → mark & sweep → arithmetic/comparison/equality
→ `print` → UTF-8 and display → input/time/clock → range, truthiness, `**`,
bitwise, membership, iteration → hashing and the ordered table → exceptions →
collection constructors → classes → length/index/slice/destructuring/comma-join →
original builtins → function values and the binder → method dispatch (list, map,
set, text, bytes) → the Python-parity builtins (repr/ascii, files, format,
reflection, identity, iteration, numeric, text/number conversion, slices, stubs).
`grep -n '^/\* ---- ' runtime/pentest_rt.c` prints the table of contents.
