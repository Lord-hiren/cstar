# Benchmarking

The harness in `bench/` measures compiled Pentest programs against hand-written
C++ twins that do exactly the same work and print byte-identical output. Results
and interpretation live in [performance](../performance/README.md); this page is
how to run it.

## Prerequisites

`clang` and `clang++`, `python3` (case generators only), `bash`, GNU coreutils,
and ideally `taskset` (runs are pinned to core 0 when it is available, and fall
back to unpinned otherwise). Build the toolchain first (`make`).

## The runners

| Script | Compiler under test | Cases | Writes |
|---|---|---|---|
| `bench/run.sh` | the **preserved M1 compiler** `bench/tools/pentest-m1` (never invokes `make`, never touches `build/pentest`) | `bench/cases/*.pen` — M1 call-chain workloads | `bench/results/m1-baseline.md`, `timings.txt`, `rss.txt`, `compile-*.txt` |
| `bench/run-m2b.sh` | current `build/pentest` | `bench/cases/m2b/` — real loops, collections, closures | `m2b-baseline.md`, `*-m2b.txt` |
| `bench/run-m2c.sh` | current `build/pentest` | the same `m2b/` cases | `m2c-baseline.md`, `*-m2c.txt` (including before/after tables vs the recorded 2B run) |
| `bench/run-varq.sh` | current `build/pentest` | `bench/cases/varq/cross_scope.{pen,cpp}` | `varq-baseline.md` |

> **Warning — the runners overwrite recorded baselines.** `run-m2b.sh`,
> `run-m2c.sh` and `run-varq.sh` all measure whatever `build/pentest` currently is
> and rewrite their files in `bench/results/`. The checked-in `m2b`/`m2c` results
> are the *recorded* baselines that the docs in `docs/performance/` quote.
> Copy them elsewhere (or commit first) before re-running, and update the baseline
> documents if you intend a new recording.

Only `bench/run.sh` is insulated from the compiler you are developing, because it
uses the frozen M1 binary.

## What every runner does

For each case (a `<name>.pen` and a `<name>.cpp` twin):

1. regenerate the `.pen` from the generator (`bench/gen.py`, `bench/gen-m2b.py`) —
   deterministic, no randomness;
2. build the twin: `clang++ -std=c++17 -O2` into `bench/bin/`;
3. build the `.pen` with `pentest build` (which never serves from the program
   cache) and record the cold compile time;
4. **assert both binaries print byte-identical output** and abort loudly if not;
5. run each binary `BENCH_REPS` times (default 7; 9 for `run-varq.sh`), pinned to
   one core, timing **execution only**, and report min and median wall time plus
   peak RSS;
6. probe the `run`-mode cache (cold vs warm) on one case;
7. write the markdown results file (and the raw `timings-*.txt`, `rss-*.txt`, `compile-*-*.txt` tables).

`bench/bin/` and `bench/results/cache-probe/` are git-ignored.

## Environment variables

| Variable | Effect |
|---|---|
| `BENCH_REPS=n` | timing repetitions |
| `BENCH_REBUILD=1` | reuse existing binaries in `bench/bin` (compile times are then marked as not re-measured) |
| `CXX` | C++ compiler for the twins (`run-varq.sh`; default `clang++`) |

## Methodology notes

* **Min and median, not mean.** Run-to-run variance on the reference machine was
  up to ±9% on loop cases; the minimum is the robust statistic.
* **Identical work.** Twins replicate the runtime's float formatting and its
  wrapping integer semantics so outputs match to the byte.
* **Interleaved A/B for changes.** When evaluating an optimisation, alternate the
  old and new binaries over at least three runs and compare minima, and require
  identical stdout/stderr/exit codes first
  ([optimization §9](../performance/optimization.md)).
* **Cross-scope acceptance.** `run-varq.sh` passes if the Pentest binary runs the
  workload in under 20 ms *or* is at least as fast as the C twin
  ([cross-scope access](../reference/cross-scope-access.md) §6); it exits non-zero
  only if both miss.

## Adding a case

Add the generator entry (or a hand-written pair) so that `<name>.pen` and
`<name>.cpp` compute the same thing from fixed constants, add its name to the runner's `CASE_ORDER` list (and the workload description
text the runner prints), run the runner once, and check that the
byte-identical assertion holds before trusting any timing.
