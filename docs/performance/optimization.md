# Optimization — how Pentest gets speed without a dedicated optimizer

Status: **informative companion to `SPEC.md` §11.8** (implementation &
architecture; the SPEC governs if they differ). Part of the
[performance documentation](README.md). Stage: `0.2.1`. Everything here is a *lowering rule* or a
*runtime property*, not a post-processing pass: the code generator emits
fast code directly, there is no IR rewrite step, and nothing about
performance ever changes a program's output (byte-compat contract, §9).

## 0. Principles

1. **Optimized by design.** Each optimization is a decision made where the
   code is emitted (semantic facts + lowering), so there is no separate
   optimizer to maintain, no intermediate junk, and no pass that can
   introduce regressions between releases.
2. **Zero junk.** Compilation produces the C translation unit, the object
   cache entry, and the binary — no `*.tmp` strays, no leaked scratch
   directories (verified after every suite run: no `/tmp/pentest-*`, no
   `build/*.tmp`). Generated C compiles with empty stderr (see §6).
3. **Conservative by one slot.** Anything the analysis cannot *prove*
   exits an optimization lane rather than risking a behavior change
   (poisoning, §1).
4. **Best default flags, no dangerous ones.** Every program compiles with
   `clang -std=c11 -O3 -march=native` (raised from `-O2` when the SIMD
   work landed). `-ffast-math` is never used: it would break bit-parity
   with Python, which is a contract, not a benchmark detail.

## 1. Native-int specialization (the int64 lane)

**What.** A variable whose every write is provably an integer expression
is kept as a raw `int64_t` pair (value + guard flag) instead of a tagged
`cs_value`, and loops over such slots lower to straight C — `while`
becomes a C `while`, no tag checks, no dispatch.

**How it is proven.** The semantic/codegen analysis tracks per-slot
writes (`SlotWrite` lists for globals and locals): a slot joins the lane
only if every write is an integer literal or an expression over
lane-members (`+ - * / %` with native-compound forms `+= -= *= /= %=`),
or — for a `for` target over an open-coded `range()` — the loop's own
int64 induction variable (an `int_bind` write: the range-loop emitter
binds the native slot directly, and every other iterable still poisons,
exactly as before). **Poisoning** is
the exit door: a write the proof cannot cover — `**=`, bitwise or shift
compounds, a non-int expression, a cross-scope pin (§7) — marks the slot
poisoned, and a poisoned slot leaves the lane for the whole program.
The cost is exactly one slot's fast path; the benefit is that no
analysis mistake can change results.

A range-loop target's *assigned-ness* stays dynamic — the range may be
empty — so DA keeps its used-flag even with zero reads (the
`globals()`/`locals()`/`vars()`/`dir()` name tables consult it), and a
read after an empty loop raises the same `NameError` the boxed path
raises (`tests/cases/bi_lane_for`, `err_bi_lane_for_empty`,
`err_bi_lane_for_local`). This is where the parity builtins meet the
lane: `(abs(i - n)) % k` inside a counted loop now compiles to pure `int64`
arithmetic — measured **94 ms vs 1361 ms (14.4×)** on a 20M-iteration
`acc += (abs(i - 10000000)) % 97` loop (min-of-3 interleaved, both sides
byte-identical output).

**Definite-assignment no-flag reads.** A parallel definite-assignment
walker (`da_check_expr` / `da_g` / `da_l`) proves which reads are
dominated by an assignment; those reads skip the guard-flag check
entirely — one branch removed from every proven read on the hot path.

The lane is emitted as such pure straight-C that **clang reduces proven
counted loops to closed form**: a 50M-iteration `acc += i; i += 1` loop
compiles to a single constant `add` (measured end-to-end in 75 *ns*,
sum still exact under wrapping semantics) — the strongest form of
"optimized by design": the emitted code is simple enough for the C
optimizer to finish the job.

**Edge parity is part of the contract.** C integer semantics are *not*
Pentest semantics by default; the lane guarantees exact parity for wrap,
division/modulo sign rules, and `MIN / -1` (`tests/cases/nint_edges`,
`nint_loop`). Any divergence is a bug, not a performance trade.

## 2. Dynamic numeric fast paths (`_f` operators)

Dynamic (untagged-lane) arithmetic on numeric values used to pay a
cross-object call per operation. Now `runtime/cstar_rt.h` defines
static-inline fast paths — `cs_add_f` … `cs_mod_f`, `cs_neg_f`,
`cs_lt_f` … `cs_ge_f`, `cs_eq_f`, `cs_ne_f` — emitted directly by the
code generator for numeric-shaped operands. Each performs the *identical*
tag checks as the out-of-line fallback and falls back to it on any
mismatch, so behavior is bit-for-bit unchanged; only the call is gone on
the hot path. Measured: **~35% faster dynamic loops** (dyn_float 2.27s →
1.52s, dyn_int 0.41s → 0.26s on 10M-iteration loops).

## 3. GC safe-point elision at loop back edges

A garbage collection can only start at a **safe point**, and back edges
are where long loops would otherwise pay the check every iteration.
`alloc_free_expr` / `body_alloc_free` prove that a loop body cannot
allocate (no string building, no list/map/set operations, no calls not
proven free); such bodies emit **no** back-edge safe point. Cross-scope
stores count as heap-free in this proof — a plain `variable.…` store is a
direct write into a pinned static / module global (`docs/reference/cross-scope-access.md`
§5) — so a qualified statement in a hot loop does not drag the check back
in: measured, a 50M-iteration native loop with a `variable.global` store
in its body runs at control speed (112 vs 107 ms) instead of roughly
doubling (236 ms while the check was kept). The proof is conservative:
any possibly-allocating expression keeps the check — including compound
qualified `+=`/`*=` over dynamic slots, where a text operand could
concatenate — so the collector's behavior on real programs is untouched
by default; the optimization only removes checks that could never fire.

## 4. Header-level inlining of the guards

`cs_guard` (unset-slot read check) and `cs_truthy` (condition
normalization) are `static inline` in `cstar_rt.h`. An ordinary guarded
read costs one compare-and-branch with no call; this is what keeps
non-nativized reads — including every `variable.` qualified read (§7) —
cheap enough that no further special casing is needed.

## 5. Compiled-artifact caches

The runtime translation unit is compiled once at `make` time and embedded; two
independent caches under `$XDG_CACHE_HOME/pentest` (fallback `~/.cache/pentest`)
cover everything emitted per program or per machine:

* **Embedded runtime object (build time)** — `make` compiles
  `runtime/cstar_rt.c` with exactly the on-demand flags
  (`clang -std=c11 -O3 -march=native -c`) into `build/cstar_rt.o` and embeds
  it next to the runtime sources, together with the build host's
  `/proc/cpuinfo` `flags` line. A cold cache installs and links that object, so
  the first-ever `run`/`build` is dominated by gen.c's own clang run
  (~0.2–0.4 s across the speedtest cases, comparable to their C twins) instead
  of paying a ~6 s runtime compile. The object is machine code for the build
  host's ISA: before using it the driver compares the current `flags` line, and
  on any mismatch — a binary copied to another CPU, kernel-masked features —
  silently falls back to the runtime-object cache below. Cost: one runtime
  compile per `make` (~6 s) and ~290 KB in the binary.
* **Run-mode binary cache** — `pentest run` compiles once and reuses the
  executable for repeat runs. Key = `sha256(entry source bytes + every
  imported module's source bytes + kVersion + toolchain identity)` where the
  toolchain identity is the size + mtime
  of the running `pentest` binary: editing the source or any imported module
  invalidates (source hash), and — crucially — **rebuilding or updating the
  toolchain re-keys every entry automatically**, so a stale executable from
  before a `make` can never be served. The identity read is a single `stat` of
  `/proc/self/exe` (microseconds; an unchanged binary keeps its hits).
* **Runtime object cache** — when the embedded object is unusable, the runtime
  is compiled to `rt-<sha>.o` once and linked from there on subsequent builds
  instead of re-running clang every time. The key covers the embedded runtime
  content, the object-compile flags and the current CPU's `flags` line, so a
  cache shared across machines never serves an object built for another ISA.

## 6. Lowering cleanups that keep the generated C clean

* **Condition splicing.** A native-int condition is spliced bare into
  `if`/`while` instead of wrapped in redundant parentheses — kills the
  clang `-Wparentheses-equality` stderr noise (`((a == b))`) that would
  otherwise pollute compiler output for a byte-clean build.
* **No scratch files.** The pipeline writes only its final artifacts;
  verified zero-stray after full-suite runs (§0).

## 7. Cross-scope access: pin poisoning

A binding reachable through `variable.<qualifier>` (see
`VARIABLE-ACCESS.md` §4) is *pinned* to one dynamic `cs_value` storage,
so it — and any global reached that way — is **poisoned out of the
native-int lane**. The qualified path reads the dynamic slot; the
ordinary path must read the same slot, and poisoning is how "one
storage" is enforced without a merge pass. Conservative by one slot,
exactly like every other poison: correctness by construction, and the
cost exists only for bindings that actually use the feature.

## 8. Long-running tools: the chunked-timeout pattern

For work that must survive under a time budget (the acceptance benchmark
`lab.pen` and its C twin `c-things/test_timeout.c` use the same shape — both
are git-ignored local scratch files and are **not in the repository**, so the
timings below cannot be reproduced from a clean checkout):
run the work in **chunks** and check elapsed time at chunk boundaries
instead of every iteration. A per-iteration clock read would dominate
the loop; chunking amortizes it while keeping the process responsive to
the deadline. Result: `lab.pen` executes in **~2.34s vs the C twin's
2.37–2.51s** on the same shape — the dynamic loop is within noise of C.

## 9. How performance changes are validated

* **Byte-compat first.** Fast paths are validated A/B against the
  pre-optimization binary with *identical stdout/stderr/exit codes*; the
  full suite (372 checks: 365 program cases + 7 extra checks, as of this
  writing) is the gate for every change.
* **Interleaved min-of-N A/B, CPU-pinned.** Benchmarks compare the minimum
  of N runs — ≥ 5 for large effects, ≥ 30 when reading small ones — with
  binary order alternating, pinned with `taskset -c 0` on a shared host,
  and the 37 speedtest cases additionally re-verified byte-identical
  against their C twins after every runtime edit. Only effects ≥ ~1.5×
  are claimed as real on this noisy 4-CPU host.
* **The code-layout envelope.** A runtime edit of a few bytes — a macro
  that grows one branch — shifts the alignment of every function after it
  in `runtime.o` and can swing a cache-resident microbenchmark by ±10%,
  with the sign arbitrary per binary pair: `str_hash` read 0.93×, 1.00×
  and 1.08× across three builds whose *behaviour* on that workload is
  identical (its hot loop performs no allocations, so the edited code
  never executes there). Rebuilding both variants with
  `-falign-functions=64` neutralizes the shift and reads the behavioural
  truth (0.996× there). Single-digit deltas from ordinary builds are
  layout artifacts until proven otherwise.
* **No environment leakage.** `tests/run_tests.sh` isolates cache and
  scratch dirs per run so a green suite can never be an artifact of a
  warm cache.

## 10. Known characteristics (deliberately not optimized)

* A non-nativized scalar loop still pays the dynamic-dispatch tax — that
  is the lane's entry price, not a defect; keeping the lane conservative
  is what preserves exact edge parity (§1).
* `coll_mixed` benches reach a ~30 MB GC high-water (allocation churn,
  not a leak — RSS falls back after collection).
* Deep recursion overflows the C stack (the language runs on the
  process stack; no heap-allocated frames yet).

See `SPEC.md` §12 for the full limitations ledger.

## 11. Python-parity builtins — the zero-junk call contract

The `0.2.1` builtin set (SPEC §5.11, ~50 `cs_builtin_*` entries in
`runtime/cstar_rt.c`) is held to the same standard as the lane: every
call is a **direct C call with compile-time-known shape**, and running a
builtin allocates nothing beyond its own result.

* **No dispatch, no checks at runtime.** `src/builtins.h` drives
  compile-time arity and named-argument validation (SPEC §5.11.1), so the
  emitted call is `cs_builtin_sorted(…)` — no table lookup, no wrapper,
  and builtins the program does not use emit nothing at all.
* **Scalar paths allocate NOTHING.** `abs`/`round`/`pow`/`min`/`max`/
  `sum`/`len`/`ord`/`hash`/`divmod`/`isinstance`/`format` and friends
  return immediates or reuse their arguments; the alloc-free proof
  (`alloc_free_expr`, §3) recognizes them, so a hot loop calling them
  keeps no back-edge safe point.
* **Collection builders allocate exactly once per result.** `map`/`zip`/
  `filter`/`enumerate`/`reversed`/`list`/`tuple`/`sorted`/`dict` grow a
  single result buffer (one amortized growth path, no intermediate
  collections); variadic arguments and the `globals()`/`locals()`/`vars()`
  `dir()` name tables are C11 compound literals and `cs_name_slot` rows
  **at the call site** — no temporaries, no reflection over live frames.
* **The native lane absorbs the numeric builtins** (§1): over proven-int
  operands `abs`/`round` (1 arg)/`pow` (2 args)/`min`/`max` (2 positional)
  lower to `cs_nabs`/`cs_npow`/`cs_nmin`/`cs_nmax` — and with `for`
  targets over open-coded `range()` now lane-members (§1), the canonical
  `acc += (abs(i - n)) % k` loop is pure `int64` arithmetic end to end:
  measured **94 ms vs 1361 ms (14.4×)** on 20M iterations, min-of-3
  interleaved.
* **The audit method** is the gen.c capture: build with a watcher on
  `$TMPDIR` (the compiler deletes `gen.c` after linking) and inspect the
  emitted C — every call site visible, nothing hidden behind macros.

`runtime/cstar_rt.c` cites this section from its parity-builtins
header comment.

## 12. SIMD vector types and expression fusion (`fvec` / `ivec`)

Two mechanisms, one goal: make vector arithmetic fast *structurally* —
by lanes and by eliminating memory traffic — rather than by shaving
scalar instructions.

**First-class vectors, hand-vectorized.** `fvec` (float64) and `ivec`
(int64) are real object-model types (SPEC-facing rules in the
[builtins guide](../guide/builtins.md#simd-vectors-five-fvec-ivec-and-friends)),
whose operators are written as explicit clang vector extensions —
`cs_vd`/`cs_vi` lane types, `CS_VW` = 8/4/2 lanes under
`__AVX512F__`/`__AVX2__`/baseline, `memcpy` lane loads, splat macros —
plus a scalar tail. Two reasons for explicit lanes over hoping the
auto-vectorizer runs: generated code must produce the same results with
any `clang` version and flags, and dynamic (runtime-dispatched) operators
would otherwise stay scalar inside a called loop. Every op keeps the
scalar operators' exact per-element semantics (wrap, division/modulo
edges, error class and line), so SIMD never changes observable behavior.

**Expression fusion (one loop per chain).** The dynamic route is one
allocation per operator: `y = a * x + y` materializes `a * x`, then the
sum — two full-size temporaries written and collected for arithmetic the
CPU could do in registers. `analyze_native_fvecs()` proves which slots
*only* ever hold `fvec`/`ivec` values (the same candidate-fixpoint
discipline as the native-int lane, §1: start permissive, intersect the
kind of every write, poison cells/pins/try-functions, drop `/=`, `%=` and
range binds) and, at emission, re-checks the shape of each assignment
against the final proof. A proven slot assigned `+ - *` (or `+= -= *=`)
over proven/int-lane-scalar operands lowers to a single kernel block:
operands hoisted and read once, one result buffer, a lane loop, a scalar
tail — the Halide/XLA trick of intermediate elimination, at statement
granularity. Division/modulo and unproven slots simply fall back to the
(already SIMD) dynamic operators. The proof *is* the type contract:
per-element arithmetic is identical by construction, length mismatches
raise before the first store, and `fvec_ops`/`ivec_ops` pin
fused-versus-scalar bit parity in the suite.

**Where the wins come from.** Fusion removes memory traffic
(temporaries), lanes remove instruction count (8 doubles per `vfmadd`
on AVX-512), and the two compose — a fused chain executes one pass over
its operands and one over its result instead of two passes per operator.
The scoreboard corpus lives in `tools/hpcbench/` (interleaved, min-of-N,
both idiomatic multi-pass C and hand-fused single-pass C baselines).

**Buffer lifecycle — why the kernel alone wasn't enough.** A fused chain
is memory-bound; what it writes *into* matters as much as what it
computes. Three runtime changes close the gap to hand-fused C:
lane buffers are **not zero-filled** on allocation (every producer fills
`[0, len)` and nothing reads past `len`, so the extra 64 MB pass was dead
work); swept buffers of ≥ 64 KB are **parked in an exact-size reuse pool**
instead of `munmap` + fresh `mmap` + page-fault every statement; and a
vector's lane buffer **counts as GC pressure** (the ~250 B object header
alone is invisible, so a 64 MB-working-set loop would never cross the
collect floor, never sweep its garbage vectors, and re-fault a fresh
buffer — and its page-zero pass — on every rep). Together these take a
fused `z = f(x)` chain from ~1.4× behind hand-fused single-pass C to
parity, and a fused reduction (`dot`) past it via the lane-tree.

## 13. Formatted output and pointer chasing (text chains, instance layout)

Two more structural fusions, aimed at the two workloads where the dynamic
route paid per-operation heap traffic: formatted file I/O (`io`) and
instance-field traversal (`ptrlist`).

**Text-chain fusion (`cs_sb` builder).** `f.write((str(i)) + " " + (str(v)) +
"\n")` is the canonical record loop; the dynamic route built it as five
GC-block allocations (two `str()` conversions + three concatenations)
plus a transient `malloc` per conversion and a growing-priority copy per
`+`, for a 15-byte record. The codegen now recognizes a `+` tree whose
parts are **all provably text** — string literals and `str()` calls, and
nothing else (a bare operand could carry a non-text value, so the
dynamic path keeps its exact operand-type error) — and emits one
`cs_sb` stack buffer instead: literals copy straight in, proven-int
`str()` arguments format straight in via `cs_sb_puti` (the display
path's digit code — `str(int)`'s exact spelling, no heap text ever), and
other `str()` results materialize once and append. Two tails: a value
chain becomes **one** heap text (`cs_sb_finish`), and
`f.write(<chain>)` hands the buffer to `cs_file_write_buf` — **one
`fwrite`, zero heap traffic**, the file-side equivalent of the print
fast path, with the receiver's checks in the dynamic branch's order (a
non-file receiver rebuilds the text and goes through `cs_call_method`,
so every error is byte-identical). An all-literal/all-int-lane chain
also reports `alloc_free` (§3), so the loop back-edge safe point elides.
Measured on `io` (600k formatted records × write/close/reopen/read ×4
reps): **0.27 s vs 0.35 s for hand-written C** (one preformatted `fwrite`
— C's own `fprintf` re-parses its format string per record and is slower
still), min-of-N interleaved; `io` went from 8.4× behind C to 1.3×
ahead of it. The read side joined in: a whole-file `read()` sizes the
file with `fstat` and fills **one registered block with one `fread`**
(no growing raw buffer, no second copy, no zeroing pass), and the old
byte-at-a-time `getc` loop (one call + stdio lock per byte) became a
chunked `fread` wherever no byte ever has to be pushed back
(`readline`/`read(size)` keep the precise byte loop — `getc_unlocked`).

**Instance inline fields.** The object model charges every `Node` three
mallocs (`cs_obj` block + `attrs.e` + `attrs.slots`), and a traversal
step paid the corresponding serialized dependent misses plus a full
hash-and-probe per attribute read. `cs_instance_new` now allocates the
first `CS_INST_INLINE` (= 2) fields **in the instance's own GC block**
(a tail beyond `cs_obj`, which itself stays small for every other kind):
one allocation per instance, and `cs_getattr`'s common path is a linear
scan over ≤ 2 entries whose keys compare by storage pointer
(`cs_value_eq`'s new same-buffer fast path — the `cs_text_lit` name
literals merge to one `.rodata` pointer per program). The inline area is
append-only (a delete tombstones, never reuses), so `vars()`/`dir()`
keep exact insertion order across inline+spill; a third field spills
into the ordinary table, marked/swept exactly as before. Classes with
two fields — the linked-node/2-D-point shape — now cross the heap with
one miss per node instead of three allocations and as many misses per
step.

**Loop-body temp-clear DCE.** Emission is A-normalized: every temp is
assigned before its next read, so the statement-tail `cs_unset()` store
is a dead store on a loop's back-edge path — and a shadow-frame slot
holding a stale pointer is only ever *marked* at a collection, never
swept-then-read. Inside loop bodies the clear is therefore skipped
(§7.3 collection timing unchanged; at most one extra iteration of
garbage is pinned). Six cross-object calls drop out of every hot loop —
part of what took `ptrlist`'s cache-resident instruction cost from
367 ns/step to ~122 ns/step and its 1M-node kernel from 2.83 s to
1.50 s (the remaining distance to C is DRAM: the traversal touches
~2 dependent misses per step against C's one 16-byte struct load).

**Cross-tag `==`/`!=` fast path.** `cs_eq_f`/`cs_ne_f` answer any
different-tag pair (except the int/float mixture, which compares by
value) inline — `cur != None` in a traversal loop is two tag loads and a
branch, not an out-of-line operator call.

## 14. The speed waves (closing the speedtest gaps, §13 → on)

The speedtest suite (`speedtest/cases/*.pen`, 37 cases each with a
hand-written C twin) drives the work after §13. Each "wave" is one
mechanism fixed at the language/codegen/runtime core — never a
per-benchmark special case — validated by the gates of §9 (byte-identical
A/B output, interleaved min-of-N, C-twin parity of all 37 outputs).

**Wave 1 — int-lane coverage: `divmod`, `call_direct`, `branchy`.**
A chained `if` over equality comparisons against int literals compiles
to a C `switch` on the proven-int scrutinee (`native_switch_ok()` is
the proof gate — every arm is an int-literal comparison and every
`else` is a plain default), instead of the `cs_truthy(cs_eq_f(...))`
ladder. `divmod`'s operands and result stay in the native lane.
`call_direct` fell out of the *joint* fixpoint: slot lanes and
int-lane twin viability are proven together (each shrinks the other —
an `acc = acc + f(i)` write had evicted `acc` from the lane before `f`
could be proven int-returning), so a call-shaped accumulator keeps its
lane.

**Wave 2 — instance-method inline cache + C-stack bound vectors.**
`cs_call_method`/`cs_class_call` resolve a hot `p.step(i)` through a
guarded cache (class + name → function, four guards, entries pinned as
GC roots) *before* the getattr/bound-method machinery: no hash lookup,
no bound-method allocation, no `malloc` for the self+args vector.
Calls with ≤ 64 bound slots (`CS_CALL_STACK_MAX`) build the §13 bound
vector on the C stack — exact-arity paths skip the binder entirely and
wider functions keep the heap vector so deep recursion cannot overflow
the C stack.

**Wave 3 — allocator/GC: swept-block reuse.** Swept blocks of the
churn-prone kinds (cells, lists, tuples, functions, iterators,
fvec/ivec objects) park in exact-(size, kind) buckets and are handed
straight back out by `cs_gc_new_full` (see `docs/internals/runtime.md`):
no `malloc`/`free` round trip per cell or list object in a churn loop.

**Wave 4 — text: in-place growth and memoized code-point queries.**
`s = s + chunk` loops (`text_build`) concatenate amortized in place
(`cat_grow` — capacity doubling, byte-identical results): 2.2×.
Code-point queries (`unicode_scan`/`text_find`) memoize per
(payload, length, generation) in the tset instead of re-scanning.

**Wave 5 — the float lane.** The same fixpoint discipline as §1's
int lane, one level up: slots proven to hold only `CS_FLOAT`
(literals, lane reads, unary `-`, and `+ - * /` where the operands are
float-shaped, with int operands converted by the identical C cast the
runtime's numeric fast path performs) run as plain `double` in
generated code. `float_loop`: 2.02×.

**Wave 6 — guard elimination.** A definite-assignment walk proves which
slot reads can never observe `CS_UNSET` (params are filled with real
values or `CS_MISSING` by every bound-vector source; locals/globals
follow the conservative join of `analyze_definite_assign`) and drops
their used-before-assignment `cs_guard` — plus the used-flag store on
the write side for definitely-assigned native slots, which also unpins
loop counters from the stack. `class_ops` −19%.

**Wave 7 — statically-proven instance-method inlining.**
`analyze_method_inlines()` plans `MethodCall` sites whose every
dynamic-dispatch premise it can prove program-wide (receiver's class,
the name→function mapping — sound because method tables are written
only during class-statement execution), and `emit_inline_method()`
emits the callee's body at the site over per-site slots; a re-entry
stack falls back to dynamic emission for mutual recursion.
`class_ops`: 271.7 → 148.2 ms (1.83×), closing the C gap ~150× → ~38×.

**Wave 8 — text/bytes block pooling.** TEXT/BYTES join the Wave-3
parked pool: a swept text buffer is parked and the next same-size text
allocation reuses the block — re-registered in the tset with a bumped
generation, so a code-point memo keyed on the old (payload, length,
generation) can never be impersonated by the block's next life. The
pool is capped by total parked bytes (16 MB) as well as block count,
because text payloads are unbounded. `text_ops` 1.35–1.65× (the win
holds at 1.346× under `-falign-functions=64`, i.e. it is behavioural,
not layout); `sort_text` 1.23×, `text_build` a further 1.13×; all
other cases at parity — deltas below the ~10% code-layout envelope of
§9 are not attributable to the change.

**Wave 9 — slice copy-on-write + lazy spans, and exact mixed numeric lanes.**
Lists and tuples now share a `cs_seqbuf` and carry a view stride; a slice is
an O(1) view (strided and negative strides included) and every write
detaches. Sequence objects allocate only the tag + `as.seq` prefix, index and
length fast paths are header-inline, and index normalization is branchless.
A whole-lifetime int-only-list proof (plus a flat-list sub-proof for
literals and `list()`) keeps `xs[i]` and the surrounding
accumulator arithmetic in `int64`; when a slice target is read only as
`target[idx]` later in the same statement list and the source is write-free
after the snapshot, codegen emits one normalized span and fuses the element
reads (shared raw `items`/`stride`/`len` facts; a second span with equal raw
bounds is derived as `ceil(base.count/step)`). Mixed numeric code keeps the
int and float lanes: `float(<int64>)` is the exact cast, `int(<float>)`
retains its range guard, mixed comparisons use the exact int64/double
ordering helper, and `cs_nfmul`'s empty-asm barrier prevents FMA contraction
(two-rounding parity).

`slice_ops` 192.5 → 3.06 ms (C 2.73, 1.12×; was 55.95×); `mixed_num` 1717 →
38.9 ms (C 33.25, 1.17×; was 44.62×) in a REPS=15 sweep. The C twins
auto-vectorize (four iterations/AVX2 gathers for slices; four-way unroll plus
FMA and an unchecked cast for mixed), so the residual is the cost of
bounds/type/error and rounding parity, not an unoptimized scalar loop.

Scoreboard after Wave 9 (Pentest vs hand-written C, same flags):
`divmod`/`call_direct`/`branchy` at parity; `text_build` ~2.5× faster
than the pre-wave compiler (2.2× from Wave 4's in-place growth, a
further 1.13× from Wave 8's pooling); `float_loop` 2.02×;
`closure_ops` ~22×; `text_ops` ~15–17× (was ~24×); `class_ops`
~38× (was ~150×); `gc_churn` ~50×; `list_ops` ~6× (structural — C's
`realloc` contiguous array vs the language's linked list); after Wave 9,
`slice_ops` 1.12× and `mixed_num` 1.17× (both were above 40×).

## 15. Selective imports: pay only for what you name

Modules add no runtime mechanism. The linker renames imported
declarations to unique internal names, rewrites references, and flattens
the module graph into the entry AST before semantic analysis; code
generation and the runtime never learn that a module existed. An
imported call is therefore the same static call as a local one, and the
native-int lane and other emission optimizations apply to imported
functions exactly as they do to entry functions.

Compile-time cost is managed by three properties:

* **Zero-cost fast path.** A program with no imports returns from
  `link_imports` after a single scan of the top-level items. There is no
  per-node import check in the compiler or the generated code.
* **Pruning before analysis.** In `run`/`build`, one worklist pass over
  the AST keeps only imported declarations reachable from the entry
  program or from module initialization. Unused functions, classes and
  pure globals are removed *before* semantic analysis and code
  generation, so importing a large library costs only the declarations
  the program actually references. `check` is intentionally strict and
  validates everything.
* **Dependency-aware cache.** The run-cache key includes every imported
  module's source, so an edit to a dependency recompiles once and then
  hits the cache again; an unchanged dependency graph pays only the hash.

Nothing in this layer emits an instruction: there is no module registry,
no lazy binding stub and no runtime lookup. The trade-off is
compile-time flattening only.

