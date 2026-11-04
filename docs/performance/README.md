# Performance

Pentest compiles to native code through C, and its performance work follows one
rule: **make the emitted code simple and let `clang -O3 -march=native` finish
the job** — no
separate optimiser, no IR rewrite pass, and no optimisation that can change
program output. This directory holds the mechanisms and the measurements.

| Document | What it is | Status |
|---|---|---|
| [optimization.md](optimization.md) | every mechanism (native-int lane, dynamic fast paths, GC safe-point elision, header inlining, artifact caches, pin poisoning, builtin call contract), why each is safe, how changes are validated | **current** (`0.2.1`) |
| [baseline-m1.md](baseline-m1.md) | the M1 compiler's baseline: call-chain workloads, compile-time cliff, stack-depth probe | historical |
| [baseline-m2b.md](baseline-m2b.md) | Stage 2B: real loops, collections, closures; the optimisations that the baseline itself drove | historical |
| [baseline-m2c.md](baseline-m2c.md) | Stage 2C: the garbage collector — leak collapse to the RSS floor, and its time cost | historical for *times* (recorded before `0.2.1`); the RSS findings stand |

The raw machine-generated tables are in `bench/results/`; how to reproduce them is
in [benchmarking](../development/benchmarking.md).

## What the recorded baselines say

All numbers below are quoted from the baseline documents, measured on an
Intel i7-6600U at 2.60 GHz pinned to one core with `clang` 21.1.8, comparing each
Pentest program against a hand-written C++ twin with byte-identical output.

* **Memory (Stage 2C, still true).** Before the collector every heap object
  leaked: peak RSS reached 344 MB on `dispatch_ops`, 166 MB on `coll_mixed`,
  119 MB on `closure_ops`. With the collector those collapse to the ~12 MB floor
  (30 MB for `coll_mixed`, whose working set is bounded between safe points).
* **Time (Stage 2C, historical).** The collector's safe points cost a 1.10–1.85×
  median-time tax on 9 of 10 cases (`dispatch_ops` got *faster*, 0.77×, because
  its stable working set stays in hot allocator bins). Before the `0.2.1` lane the
  dynamic integer/float loops were 144× / 27× slower than C++ — which is what the
  next section is about.
* **Compile time.** 2.4–2.8 s per program in 2C, flat across workloads, dominated
  by `clang -O3 -march=native` compiling the runtime. `make` now precompiles
  that runtime and embeds the object, so a first-ever program links it (cold
  build ≈ 0.2–0.4 s across the speedtest cases, versus ~4–6.5 s before), with
  the per-CPU `rt-<key>.o` cache as the fallback
  ([CLI](../guide/cli.md#the-run-cache)).

## What changed in `0.2.1`

[optimization.md](optimization.md) records measured effects of each mechanism,
for example:

* a 20M-iteration `acc += (abs(i - n)) % k` loop: **94 ms vs 1361 ms (14.4×)** once
  `for` targets over open-coded `range()` joined the native-int lane;
* ~35% faster *dynamic* (non-native) numeric loops from the inline `_f` operators;
* GC safe-point elision keeps a 50M-iteration native loop with a cross-scope store
  at control speed (112 ms vs 107 ms);
* the same program cold-compiles in 3.74 s vs a 3.92 s hand-written C twin built
  with plain clang.

> **Caveat — no full re-record yet.** The suite-level tables above were *not*
> re-measured after the `0.2.1` lane; the figures in `optimization.md` are
> targeted A/B measurements of individual mechanisms. Running
> `bash bench/run-m2c.sh` against the current compiler would produce the current
> suite numbers (it overwrites `bench/results/m2c-baseline.md` — see the warning in
> [benchmarking](../development/benchmarking.md)).

## Principles to keep when changing the compiler

1. **Byte-compat first.** Every optimisation is validated A/B against the
   pre-change binary with identical stdout, stderr and exit codes; the whole suite
   is the gate.
2. **Conservative by one slot.** Anything the analysis cannot prove exits the
   fast lane ("poisoning") rather than risking a behaviour change.
3. **Zero junk.** Compilation leaves no scratch files and the generated C compiles
   with empty stderr.
4. **Measure honestly.** min-of-N, interleaved A/B, identical C++ twins, execution
   time only — see the methodology sections of the baselines.
