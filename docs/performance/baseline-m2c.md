# Pentest M2-2C Performance Baseline

> **Provenance:** the tables below were recorded with `pentest 0.2.0-m2c`,
> *before* the `0.2.1` optimization lane (native-int specialization,
> dynamic fast paths, GC safe-point elision, artifact caches). The 2C
> contract checks they validate (RSS floors, leak collapse) stand, but the
> **time ratios are historical** — current mechanisms and numbers live in
> [optimization.md](optimization.md).

Recorded by `bench/run-m2c.sh` (results file:
`bench/results/m2c-baseline.md`). Methodology identical to
`docs/performance/baseline-m2b.md`: same machine, same pinning (`taskset -c 0`), same
7-rep min/median protocol, same 10 case pairs with C++ twins and
byte-identical outputs — so the 2B→2C deltas below are apples-to-apples.

## 1. What the 2C re-record shows

Stage 2C adds the non-moving mark & sweep collector (SPEC §7.3,
contract §1). Under 2B every heap object leaked, so peak RSS grew with
the workload; the contract's obligation was that the leak-driven rows
**collapse toward the 12 MB floor**. They do:

| Case | 2B RSS | 2C RSS | Contract |
|---|---|---|---|
| dispatch_ops | 344 MB | **12.4 MB** | `344 MB → bounded` ✅ at floor |
| text_ops | 89 MB | **12.4 MB** | `89 MB → bounded` ✅ at floor |
| while_string | 44 MB | **12.4 MB** | `44 MB → bounded` ✅ at floor |
| closure_ops | 119 MB | **12.4 MB** | ✅ at floor |
| coll_mixed | 166 MB | **30.3 MB** | ✅ bounded (see §6) |

Every case now peaks within the twins' envelope (12.3–16.7 MB) except
`coll_mixed` (30.3 MB — bounded working set between safe points, §6).
The collector's cost is a **1.10–1.85× median-time tax** on 9 of 10
cases — and `dispatch_ops` got *faster* (0.77×, §6).

## 2. The cases

Identical to `docs/performance/baseline-m2b.md` §2 (same generated sources, same C++
twins, same verification): while_int / while_float / while_string /
list_ops / map_ops / set_ops / text_ops / closure_ops / dispatch_ops /
coll_mixed. Nothing changed but the compiler under test
(`pentest 0.2.0-m2c`).

## 3. Runtime results (program execution only)

Machine: Intel i7-6600U @ 2.60 GHz, taskset -c 0, 7 reps, min/median ms.
`pentest 0.2.0-m2c`, clang 21.1.8, `clang -std=c11 -O2` vs twin
`clang++ -std=c++17 -O2`. Full table in
`bench/results/m2c-baseline.md`.

| Case | Pentest min | Pentest median | C++ min | Median ratio | Peak RSS Pentest | Peak RSS C++ |
|---|---|---|---|---|---|---|
| while_int | 1703.8 | 1750.5 | 10.9 | 143.9x | 12 MB | 12 MB |
| while_float | 1689.2 | 1735.8 | 60.2 | 26.8x | 12 MB | 12 MB |
| while_string | 207.2 | 212.4 | 18.0 | 10.5x | 12 MB | 12 MB |
| list_ops | 52.9 | 58.8 | 9.4 | 4.8x | 12 MB | 12 MB |
| map_ops | 120.1 | 130.3 | 31.6 | 3.9x | 17 MB | 12 MB |
| set_ops | 123.1 | 125.5 | 81.8 | **1.5x** | 14 MB | 12 MB |
| text_ops | 520.8 | 559.8 | 145.5 | 3.4x | 12 MB | 12 MB |
| closure_ops | 237.2 | 258.2 | 7.6 | 29.1x | 12 MB | 12 MB |
| dispatch_ops | 352.1 | 377.8 | 6.1 | 49.5x | 12 MB | 12 MB |
| coll_mixed | 466.8 | 508.6 | 13.3 | 33.6x | 30 MB | 12 MB |

Run-to-run variance on this machine measured up to ±9% on the loop cases
(the mins are the robust statistic).

## 4. Compile time and the run-mode cache

Every case compiles in **2.4–2.8 s** (2B: 1.4–2.0 s — ~1.5× slower,
consistent with the larger generated C: try/except handler scopes, the
GC root vector and statement-boundary temp clearing). Still flat across
workloads. `while_int` (278 source bytes): 2339 ms Pentest build vs
222 ms twin.

Cache probe (`run` mode, fresh `XDG_CACHE_HOME`, while_int): cold 4164 ms
(compile + exec) vs warm 1673 ms (exec only) — keying is unchanged
(sha256 of source + compiler version; the 2C retag invalidated every 2B
cache entry by construction).

## 5. Before/after vs the 2B baseline (the headline)

### Peak RSS — the leak is gone

| Case | 2B Pentest RSS (KB) | 2C Pentest RSS (KB) | Collapse |
|---|---|---|---|
| while_int | 12344 | 12300 | 1.0x (already at floor) |
| while_float | 12396 | 12356 | 1.0x (already at floor) |
| while_string | 44188 | 12440 | **3.6x smaller** |
| list_ops | 12296 | 12544 | 1.0x (already at floor) |
| map_ops | 15856 | 16720 | ~parity (bounded) |
| set_ops | 13652 | 13548 | 1.0x (bounded) |
| text_ops | 89380 | 12356 | **7.2x smaller** |
| closure_ops | 119128 | 12428 | **9.6x smaller** |
| dispatch_ops | 344180 | 12428 | **27.7x smaller** |
| coll_mixed | 166016 | 30256 | **5.5x smaller** |

### Timings — the collector's cost

| Case | 2B median (ms) | 2C median (ms) | 2C/2B |
|---|---|---|---|
| while_int | 1242.3 | 1750.5 | 1.41x |
| while_float | 1235.8 | 1735.8 | 1.40x |
| while_string | 169.5 | 212.4 | 1.25x |
| list_ops | 39.7 | 58.8 | 1.48x |
| map_ops | 85.4 | 130.3 | 1.53x |
| set_ops | 86.1 | 125.5 | 1.46x |
| text_ops | 349.2 | 559.8 | 1.60x |
| closure_ops | 235.1 | 258.2 | 1.10x |
| dispatch_ops | 487.5 | 377.8 | **0.77x (faster)** |
| coll_mixed | 275.2 | 508.6 | 1.85x |

The tax comes from the mandatory safe points (contract §1: function
entries, loop back-edges, statement boundaries) — per-iteration pressure
checks plus statement-end temp clearing — and from collections
themselves in allocation-heavy cases (`text_ops`, `coll_mixed`).

## 6. Findings

- **`dispatch_ops` is 23% *faster* than 2B.** The 2B run leaked ~1.1 KB
  per kwargs-bound call, so its heap (and malloc's footprint) grew
  monotonically to 344 MB over 300k calls; the 2C run keeps a stable
  ~12 MB working set, so allocation stays in hot allocator bins. This is
  consistent with working-set effects rather than the collector doing
  anything clever; it also means the "GC = slower" framing above is
  workload-dependent — reclaiming memory *helps* allocation-heavy
  dispatch.
- **`coll_mixed` peaks at 30 MB, not 12 MB.** The workload interleaves
  int/list/map/text allocation with only statement/back-edge safe points;
  garbage produced *between* safe points accumulates before it can be
  observed. The peak is bounded by the rate × safe-point interval, not by
  total work — exactly what "bounded" in the contract requires — and the
  2B value was 166 MB for the same run.
- **`map_ops` sits at 16.7 MB (2B: 15.9 MB).** Map workloads hold most of
  their memory in the *live* table (150k inserts with periodic rehash);
  reclamation cannot shrink a live table, and the extra ~4 MB is
  high-water garbage between points. Nothing here exceeds the twins by
  more than 4 MB.
- **Scalar-loop ratios stay dynamic-dispatch tax territory** (144x int,
  27x float): every operation remains an externally visible tagged-value
  call. GC removed the RSS symptom (2B §7), not the per-step call count.
- **Collection-level parity holds** (set_ops 1.5x, map_ops 3.9x,
  list_ops 4.8x) — slightly above 2B's 1.3/3.6/6.0 on set/map and better
  on list, i.e. the GC did not move collection parity materially.

## 7. Next optimizations

Carried over from `docs/performance/baseline-m2b.md` §7 (still the big ones, all
codegen-side): loop-counter strength reduction to native `int64_t`,
definite-assignment guard elision, static bool-condition truthy elision.
GC-specific levers, in order of measured payoff:

- **Safe-point cost on tight loops** (the ~1.4x on `while_int`/`while_float`):
  elide the back-edge pressure check in loop bodies the analysis proves
  non-allocating (no calls, no constructors, no literal construction
  beyond tagged constants) — no collection could trigger there anyway.
- **Statement-end temp clearing** frequency: batch clearing at loop
  boundaries instead of per statement where liveness proves a temp dead
  earlier anyway.
- **`coll_mixed` peak**: allocation-pressure-driven collection (trigger
  on bytes allocated, not just at safe points) would cut the 30 MB
  high-water mark toward the floor.

## 8. Reproducing

```
bash bench/run-m2c.sh          # regenerates cases, builds with build/pentest, verifies, measures (~8 min)
BENCH_REBUILD=1 bash bench/run-m2c.sh   # force recompiles + re-measure compile times (~8 min)
```

- Uses the current `build/pentest`. The frozen runners stay untouched:
  `bench/run.sh` + `bench/tools/pentest-m1` (M1), `bench/run-m2b.sh`
  (the recorded 2B run — its `*-m2b.txt` files are the before rows).
- Prerequisites: `clang`/`clang++`, `python3`, `taskset` (optional),
  bash, GNU coreutils, `awk`.
- Every run re-verifies byte-identical outputs for all 10 pairs and aborts
  on any mismatch; the before/after tables are parsed from
  `bench/results/{timings,rss,compile-pen}-m2b.txt` when present.
- Case parameters live in `bench/gen-m2b.py` (shared with the 2B runner).

## 9. Scope statement

These are **M2-2C numbers**: fully dynamic tagged values, real loops and
branches, closures/kwargs dispatch, hashing collections, classes and
exceptions (present but unused by these cases), non-moving mark & sweep
GC with roots at globals/frames/bound vectors/handlers, Linux x86_64
only. They characterize the 2C runtime against the C++ twins (same
algorithm, same output bytes) and the frozen 2B baseline (same workloads,
leak→reclamation). Later stages that touch dispatch (e.g. 2D modules, or
the loop-cost optimizations listed in §7) should re-record against this
file.
