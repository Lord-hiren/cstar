# Pentest M1 Performance Baseline

> **Historical record** (`pentest 0.1.0-m1`). Part of the
> [performance documentation](README.md); reproduce with `bench/run.sh`
> ([benchmarking](../development/benchmarking.md)). Current mechanisms:
> [optimization.md](optimization.md). Internal names have since been renamed
> (`pentest_rt` → `cstar_rt`, `pt_`/`PT_` → `cs_`/`CS_`); this page keeps the
> old ones.

Status: **measured on the frozen M1 compiler** (`bench/tools/pentest-m1`,
`pentest 0.1.0-m1`, copied from `build/pentest` before Milestone 2 work began).
All numbers in this document were produced by `bench/run.sh` on the machine
described in the results header. This baseline exists so M2 (control flow,
collections) can be compared against a fixed reference; the harness in
`bench/` is designed to be reused unchanged once M2 lands.

The full machine-generated results live in `bench/results/m1-baseline.md`
(same table as below, regenerated on every run).

## 1. What M1 can and cannot express (why the benchmarks look the way they do)

M1 (see `docs/contracts/m1-semantics.md`) has **no conditionals, no loops, and no
boolean operators** — only straight-line code: assignments, arithmetic,
comparisons (as expressions), functions, and `print`. Two consequences
dominate this benchmark suite:

1. **Iteration is impossible to terminate via recursion.** A recursive M1
   function has no way to express a base case (there is no `if`), so every
   recursive call chain is infinite. Measured: `function f(n): return f(n-1)`
   crashes the M1-generated program with **SIGSEGV (exit 139)** after blowing
   the stack — clang does **not** turn the generated `return f(tmp)` into a
   tail call (the A-normalized codegen assigns the call result to a temp and
   then returns it; empirically the stack grows and the process dies). This
   was verified for three shapes: `return f(n-1)`, `m = n-1; return f(m)`, and
   a two-op LCG body. **M2 note: this is the single most important M1
   limitation the benchmark suite ran into.**
2. **The only terminating "loop" is unrolled straight-line code or a finite
   call structure.** Unrolling alone is source-size-bound (each LCG step is
   ~35 bytes of .pen and one top-level statement). The suite therefore uses
   **finite acyclic call chains**: K distinct functions `f0…f{K-1}` where
   `f0` performs one step and `fi` performs one step then calls `f{i-1}`
   (never the reverse, so the chain terminates at depth K), repeated N times
   as straight-line top-level assignments. Total work = K×N steps, max stack
   depth = K, source size O(K+N). This is *the* iteration idiom available to
   M1; every loop-ish case documents its (K, N).

Additional M1 facts baked into the design:

- Every variable read compiles to a `pt_guard()` **external call** and every
  operator to an external `pt_add`/`pt_lt`/… call (cross-translation-unit,
  no inlining at the fixed `-O2`), so Pentest workloads are optimizer-proof
  and cost is dominated by dynamic dispatch — the C++ twins are compiled the
  usual way and the optimizer applies to them (it even deletes the dead
  comparison chain in `call_chain`; M1 cannot elide anything).
- Integer arithmetic wraps two's-complement (identical to `uint64_t`);
  `%` and `<` are signed — the twins mirror this exactly.
- Functions return a single value and cannot write globals, so cases needing
  two accumulators (int + string, `mixed`) use two parallel chains.
- The string runtime mallocs on every concatenation and **never frees** (no
  GC in M1 — documented limitation), so string cases use period-5 seed
  resets to keep total leaked memory linear instead of quadratic. Peak
  intermediate string: 72 bytes; final printed string: 72 bytes. Measured
  peak RSS: `string_ops` 45,004 KB (~32 MB above the ~12 MB baseline),
  `mixed` 31,580 KB (~19 MB above baseline); C++ twins stay at ~12.2 MB.
- Float printing replicates the runtime's normative shortest-round-trip
  rule (`%.[15|16|17]g` + `.0` suffix); the C++ twin contains a line-for-line
  copy of `pt_fmt_float`, so outputs are byte-identical.

## 2. The cases

Each case is a pair `bench/cases/<name>.pen` + `bench/cases/<name>.cpp`.
The .pen files are generated deterministically by `bench/gen.py`
(K, N constants live there); the twins hardcode the identical op counts.
`run.sh` fails loudly if any pair's stdout differs by a single byte.

| Case | Emulation strategy | What it measures |
|---|---|---|
| `startup` | `main(): print(0)` | Process spawn + runtime init (`pt_init`) + one `print` + teardown. Identical structure on both sides. |
| `int_loop` | chain K=2048 × N=6336 | 12,976,128 LCG steps `a = a*1664525 + 1013904223` (wrapping int64). Per step in .pen: 1 guard + `pt_mul` + `pt_add` + call/return (~39–57 ns measured). |
| `float_loop` | chain K=2048 × N=6336 | 12,976,128 double steps `x = x*1.0000001 + 0.5` (converges to ≈ −5e6; no overflow). Same guard/dispatch structure with float ops. |
| `call_chain` | chain K=1024 × N=2344 | 2,400,256 frames of a mixed straight-line body: 6 integer ops (`* + % -`), 3 comparison ops (`< == !=`), 13 guarded reads, 1 call — M1's closest analogue to a "branchy" workload. Comparisons are expressions; bools cannot influence ints (no bool→int), so the per-frame bools are dead by construction (C elides them; M1 must run them — part of the measured gap). Prints the final int plus one observed bool. |
| `call_overhead` | chain K=2048 × N=6336 | 12,976,128 pass-through calls: per frame one param guard + call + return, zero arithmetic. C++ twin: one `noinline` passthrough with an empty-asm optimizer fence, same call count. |
| `string_ops` | chain K=125 × N=6000, appends unwind, period-5 reset | 600,000 concatenations of a 16-byte literal via `pt_concat`'s malloc+2×memcpy+NUL pattern; identical intermediate-length sequence on both sides (8→24→40→56→72 per period). Twin frees buffers; M1 leaks (~29 MB). |
| `mixed` | two parallel chains K=125 × N=4000 | 500,000 LCG steps + 400,000 concatenations (same reset pattern as `string_ops`). Per repetition: K int steps then a chain reset + K-position string chain. |

Deliberate twin divergences (documented, applied consistently): the C++
trip counts are laundered through `argc` (`iters + (argc > 1000 ? 1 : 0)`) so
the optimizer cannot fold loops that the .pen side cannot fold; the
`call_overhead` twin carries a zero-cost `asm volatile("" ::: "memory")`
fence (the .pen side is fenced by its external runtime calls); the string
twins free what M1 leaks.

## 3. Results

*(table generated by `bash bench/run.sh` — reproduced here from
`bench/results/m1-baseline.md`; see that file for the authoritative,
freshly measured copy)*

Measured 2026-10-13 on the machine in the results header (Debian clang
21.1.8, i7-6600U @ 2.60 GHz, `taskset -c 0`, 7 reps, min/median wall-clock
ms of program execution only; full copy in `bench/results/m1-baseline.md`):

| Case | Workload | Pentest min (ms) | Pentest median (ms) | C++ min (ms) | C++ median (ms) | Median ratio | Peak RSS Pentest (KB) | Peak RSS C++ (KB) |
|---|---|---|---|---|---|---|---|---|
| startup | process start + init + `print(0)` + teardown | 4.407 | 5.694 | 5.559 | 7.495 | 0.8x | 12372 | 12224 |
| int_loop | 12,976,128 int LCG steps (K=2048 x N=6336) | 497.235 | 531.297 | 8.094 | 9.177 | 57.9x | 12136 | 12220 |
| float_loop | 12,976,128 double steps (K=2048 x N=6336) | 532.497 | 592.343 | 49.418 | 52.137 | 11.4x | 12324 | 12220 |
| call_chain | 2,400,256 frames x (6 int ops + 3 cmp ops + 1 call) (K=1024 x N=2344) | 643.328 | 742.788 | 21.708 | 22.615 | 32.8x | 12272 | 12116 |
| call_overhead | 12,976,128 pass-through calls (K=2048 x N=6336) | 78.093 | 81.777 | 28.561 | 30.049 | 2.7x | 12340 | 12340 |
| string_ops | 600,000 x 16-byte string appends (K=125 x N=6000, period-5 reset) | 77.040 | 88.765 | 12.317 | 12.861 | 6.9x | 45004 | 12216 |
| mixed | 500,000 int steps + 400,000 appends (K=125 x N=4000) | 76.876 | 80.810 | 10.552 | 11.780 | 6.9x | 31580 | 12268 |

Cold compile time (.pen → native via `pentest-m1 build`, never cached):

| Case | .pen source bytes | Pentest build (ms) |
|---|---|---|
| startup | 85 | 344 |
| int_loop | 207275 | 111785 |
| float_loop | 197032 | 117192 |
| call_chain | 203531 | 47400 |
| call_overhead | 160176 | 86899 |
| string_ops | 170558 | 35355 |
| mixed | 171835 | 58927 |

`run`-mode cache probe (int_loop, fresh `XDG_CACHE_HOME`): cold `run`
120180 ms vs warm `run` 513 ms (~234× — the warm path skips parse, codegen
and clang entirely).

## 4. Findings (for M2)

1. **Unconditional recursion = guaranteed SIGSEGV (exit 139).** No base case
   is expressible in M1; `f(n) = f(n-1)` crashes. There is no runtime depth
   guard or error message — a raw stack-overflow crash with no output.
   M2's `while`/`if` make recursion terminable; M2 should also consider a
   explicit recursion-depth guard with a proper runtime error.
2. **Finite call chains are the only iteration idiom in M1**, and they work
   well: K=4096-deep chains run fine (default 8 MB stack; frames are a few
   hundred bytes). Max surviving call depth measured separately — see §5.
3. **Compile time is clang-dominated and superlinear in top-level statement
   count.** The M1 frontend (lex+parse+semantic) is fast — ~0.08 s for a
   20 000-statement file — but `clang -std=c11 -O2` on the generated
   A-normalized C shows a sharp cliff: measured (K=64) N=2 500 → 7.6 s,
   N=5 000 → 18.7 s, N=10 000 → 79.8 s, and N=12 000 (different case) →
   246 s. Chain depth K is much cheaper (N=10 000: K=64 → 80 s,
   K=256 → 85 s, K=1024 → 116 s) until very deep chains add their own cost
   (K=4096, N=1000 → 240 s). Suite policy: top-level statements ≤ ~12 000,
   K ≤ 2048. **M2's loop construct eliminates this problem** — the same
   workload will compile with O(1) source statements.
4. **Every program op is an external call.** Guarded variable reads,
   every arithmetic/comparison op, and every call cross the TU boundary to
   `pentest_rt.c` with no inlining. Measured per-op costs from the baseline
   run: LCG int step (guard+mul+add+call/return) **38.3 ns**
   (497.235 ms / 12,976,128), float step **41.0 ns**, pass-through call
   **6.0 ns** (78.093 ms / 12,976,128 — the pure-dispatch floor), mixed
   frame with ~23 runtime calls **268 ns** (643.328 ms / 2,400,256), and
   small-string concat ~80–160 ns including the malloc+2×memcpy. Median
   Pentest/C++ ratios range **2.7× (call_overhead) to 57.9× (int_loop)**,
   with string cases at 6.9× — the ratio tracks op density per unit of
   useful work, not a constant: cases where the C++ twin's optimizer folds
   work away (int_loop's tight loop) show the widest gaps, while pure
   dispatch (call_overhead) is only 2.7× because the C++ twin also cannot
   eliminate its `noinline` calls. Process startup is the only regime where
   the two sides are effectively tied (Pentest 4.4 ms vs C++ 5.6 ms min —
   both dominated by exec+dynamic linking, not user code).
5. **`build` never caches; `run` caches on content hash.** `run` stores the
   binary in `$XDG_CACHE_HOME/pentest/<sha256(source+version)>.pentest`;
   a warm `run` skips parse+codegen+clang entirely. The probe (int_loop)
   shows cold `run` = 120,180 ms vs warm `run` = **513 ms (~234×)**. Any M2
   harness comparing compile times must clear or redirect
   `XDG_CACHE_HOME` (this suite uses `build` mode, which is immune, and
   points the probe at a scratch dir).
6. **M1 leaks every string intermediate** (no GC): string workloads must be
   size-bounded by construction. Measured peak RSS: `string_ops` 45,004 KB
   (**~32 MB leaked** above the ~12.2 MB runtime baseline), `mixed` 31,580
   KB (~19 MB leaked); the C++ twins stay flat at ~12.2 MB. M2 collections
   will need the same discipline or a GC.
7. **Optimizer asymmetry is real and one-sided**: the Pentest side executes
   3 comparison ops + 13 guards per `call_chain` frame that the C++ compiler
   provably deletes (dead bool chain). This inflates the ratio for
   `call_chain` relative to a naive op-count model; it is an honest
   measurement of "dynamic language cannot see through its own dispatch".
8. **`main():` vs top-level**: the special `main():` block compiles to a
   separate C function called once; top-level statements inline into `main`.
   The suite puts hot code at top level (one less call); `startup` uses
   `main():` as the idiomatic form.

## 5. Stack-depth probe (separate from the suite)

To bound the chain depth available to the idiom, two things were measured
on the machine's default 8 MB stack (both probes live in
`/tmp/opencode/pentest-bench-probe/`, independent of `bench/`):

**(a) Over-deep chains die exactly like runaway recursion.** A chain of K
distinct no-arg functions (`f0` prints, `fi` calls `f{i-1}`) was built at
K=10,000 with the preserved compiler. Note the optimizer: with no work per
frame, clang collapses the entire pass-through chain — the binary contains
**zero** `pt_fn_*` symbols and the program runs fine even under
`ulimit -s 32` (32 KB). Stack cost tracks *work per frame*, not call
structure. The suite's own binaries carry the real frames:

| Binary | K | Outlined `pt_fn_*` frames | Stack at deepest point | Crash limit (measured) |
|---|---|---|---|---|
| `int_loop.pen.bin` | 2048 | 27 (each inlines ~77 levels) | ~581 KB | SIGSEGV at `ulimit -s 576`, OK at 608+ |
| `call_chain.pen.bin` | 1024 | 86 (each inlines ~12 levels) | ~777 KB | SIGSEGV at `ulimit -s 768`, OK at 1024+ |
| `startup.pen.bin` | 0 | — | ~7 KB | SIGSEGV at `ulimit -s 16`, OK at 32+ |

Decomposition of the deepest stack (verified against `main`'s prologue in
each binary): **main's frame = 48 B × N top-level statements** (it holds one
temp per assignment; `mixed`, with two parallel chains, measures exactly
96 B × N), and **each chain level ≈ 138 B** (int_loop shape: guarded read +
mul + add) or **≈ 658 B** (call_chain shape: ~23 ops). The chain is not
fully inlined: clang outlines every ~77th (resp. ~12th) level as a
`pt_fn_*` function and inlines the levels between, so real call depth at
the deepest point is ~27 (resp. 86) frames. A `SIGSEGV`-installing
`LD_PRELOAD` handler with a heap altstack confirms the fault address is the
stack mapping's first unmapped page — i.e. the crash is genuine stack
exhaustion, and it is **byte-identical in behavior** to the
unconditional-recursion crash of finding 1 (exit 139, no message). M1 gives
no way to tell "your chain is too deep" from "your recursion never
terminates".

**(b) Practical depth ceiling.** At the suite's fixed N, total stack scales
with K at ~138–658 B/level: measured binaries imply the default 8 MB stack
supports roughly **~29,000 levels** (int_loop shape) down to **~11,000
levels** (call_chain shape). The suite's deepest chain (K=2048) sits 5–14×
below that, so **stack is not the binding constraint on chain depth —
compile time is** (finding 3): K=4096 already costs ~240 s to build, and
M1's superlinear clang cliff hits long before the stack does. Note the two
additive terms: deepening a chain costs K×(per-level bytes), but *lengthening
the top-level repetition* costs N×48 B in `main`'s frame — a K=2048 chain
with N=100,000 would overflow main's frame (4.8 MB) before the chain's own
287 KB mattered. M2's `while` loop removes both limits at once (O(1)
statements, O(1) stack).

## 6. Reproducing

```
bash bench/run.sh          # regenerates cases, builds, verifies, measures (~15 min)
```

- Uses the preserved compiler `bench/tools/pentest-m1` exclusively — the
  script never invokes `make` and never touches `build/pentest`.
- Prerequisites: `clang`/`clang++` (measured versions in the results
  header), `python3`, `taskset` (optional; timings fall back to unpinned),
  bash, GNU coreutils (`date +%s%N`, `awk`).
- `BENCH_REPS=n` changes repetitions; `BENCH_REBUILD=1` forces recompiles
  (default reuses binaries newer than their sources, in which case compile
  times are marked as not re-measured).
- Every run re-verifies byte-identical outputs for all 7 pairs and aborts
  on any mismatch.
- Case parameters (K, N) live in `bench/gen.py`; changing them requires
  updating the twin constants and the workload strings in `run.sh`.

## 7. Scope statement

These are **M1 numbers**: fully dynamic tagged values, no control flow, no
collections, iteration emulated by finite call chains + straight-line code,
one external call per runtime operation. They characterize the M1 codegen +
runtime, not the language's eventual performance. M2 extends the suite with
true loop/branch cases (`while`, `if/elif/else`, list built-ins) that reuse
the same harness, same twins philosophy (identical algorithm, identical
output bytes), and same measurement methodology, so M1→M2 deltas will be
directly measurable.
