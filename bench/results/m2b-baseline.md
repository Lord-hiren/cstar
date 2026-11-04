# Pentest Stage 2B performance baseline

- Date: 2026-10-14 14:47:59 UTC
- Machine: `Linux DeathMachine 7.1.5+kali-amd64 #1 SMP PREEMPT_DYNAMIC Kali 7.1.5-1kali1 (2026-07-29) x86_64 GNU/Linux`
- CPU: Intel(R) Core(TM) i7-6600U CPU @ 2.60GHz (4 logical cores), RAM: 15.4 GiB
- Compiler (C++ twins): Debian clang version 21.1.8 (13)
- Compiler (Pentest backend, invoked internally): Debian clang version 21.1.8 (13)
- Pentest compiler: pentest 0.2.0-m2b
- C++ twin flags: `clang++ -std=c++17 -O2`
- Pentest compile flags (fixed by the CLI): `clang -std=c11 -O2 -o <out> gen.c pentest_rt.c -lm`
- Timing pinning: taskset -c 0 (pinned)
- Repetitions: 7 per binary; **min** and **median** wall-clock ms of
  program execution only (compile excluded: .pen programs are built ahead
  of time with `build`, which never caches).
- Methodology identical to the M1 baseline (bench/run.sh +
  docs/performance/baseline-m1.md) so the before/after table below is apples-to-apples.

## Runtime (program execution only)

| Case | Workload | Pentest min (ms) | Pentest median (ms) | C++ min (ms) | C++ median (ms) | Median ratio | Peak RSS Pentest (KB) | Peak RSS C++ (KB) |
|---|---|---|---|---|---|---|---|---|
| while_int | 12,976,128 int LCG steps (real while loop) | 1200.030 | 1242.316 | 7.480 | 8.007 | 155.2x | 12344 | 12476 |
| while_float | 12,976,128 double steps (real while loop) | 1222.882 | 1235.847 | 47.187 | 50.124 | 24.7x | 12396 | 12364 |
| while_string | 750,000 frames: 600,000 appends + 150,000 resets (= M1 string_ops) | 164.654 | 169.524 | 13.685 | 14.936 | 11.4x | 44188 | 12380 |
| list_ops | 200,000 appends + sum pass + index reads | 37.298 | 39.687 | 6.212 | 6.639 | 6.0x | 12296 | 12420 |
| map_ops | 150,000 inserts + 300,000 lookups + key-sum pass | 79.533 | 85.428 | 22.303 | 23.643 | 3.6x | 15856 | 12340 |
| set_ops | 240,000 adds (half dup) + 120,000 finds + full drain | 80.466 | 86.090 | 66.706 | 68.391 | 1.3x | 13652 | 12288 |
| text_ops | 200,000 x (upper+split+join+replace+find+starts_with) | 334.736 | 349.213 | 101.653 | 107.479 | 3.2x | 89380 | 12340 |
| closure_ops | 300,000 closure creations + calls via function value | 222.139 | 235.075 | 5.931 | 6.546 | 35.9x | 119128 | 12244 |
| dispatch_ops | 300,000 indirect calls (positional+named+**kwargs) | 467.017 | 487.523 | 5.297 | 5.677 | 85.9x | 344180 | 12548 |
| coll_mixed | 202,752 reps int+list+map+text (period-80 reset) | 260.806 | 275.237 | 10.578 | 11.103 | 24.8x | 166016 | 12476 |

## Before/after vs the M1 baseline (identical workloads, real loops vs call chains)

| Workload | M1 case (median ms) | M1 median | 2B case (median ms) | 2B median | Speedup (M1/2B) | M1 build (ms) | 2B build (ms) | Build speedup |
|---|---|---|---|---|---|---|---|---|
| 12,976,128 int LCG steps (real while loop) | int_loop (int_loop) | 531.297 | while_int (while_int) | 1242.316 | 0.4x | 111785 | 1835 | 61x |
| 12,976,128 double steps (real while loop) | float_loop (float_loop) | 592.343 | while_float (while_float) | 1235.847 | 0.5x | 117192 | 1726 | 68x |
| 750,000 frames: 600,000 appends + 150,000 resets (= M1 string_ops) | string_ops (string_ops) | 88.765 | while_string (while_string) | 169.524 | 0.5x | 35355 | 1854 | 19x |

(M1 rows are read from bench/results/m1-baseline.md — the frozen
`bench/tools/pentest-m1` run — same machine, same pinning, same reps.
Compile-time speedup reflects the elimination of M1's superlinear clang
cliff: O(K+N) unrolled statements -> O(1) loop statements.)

## Compile time (.pen -> native, cold; `build` never caches)

| Case | .pen source bytes | Pentest build (ms) | C++ twin build (ms) |
|---|---|---|---|
| while_int | 278 | 1835 | 209 |
| while_float | 249 | 1726 | 658 |
| while_string | 517 | 1854 | 695 |
| list_ops | 333 | 1933 | 597 |
| map_ops | 439 | 1787 | 722 |
| set_ops | 457 | 1830 | 642 |
| text_ops | 533 | 2038 | 1053 |
| closure_ops | 373 | 1448 | 274 |
| dispatch_ops | 427 | 1385 | 213 |
| coll_mixed | 703 | 1425 | 951 |

## `run`-mode content-hash cache probe (while_int, fresh XDG_CACHE_HOME)

| Measurement | Wall time (ms) |
|---|---|
| cold `run` (compile + exec) | 2535 |
| warm `run` (cache hit, exec only) | 1247 |

The cache lives in `$XDG_CACHE_HOME/pentest/` (fallback `~/.cache/pentest/`),
keyed by sha256(source bytes + compiler version). The probe directory was
removed after measuring. All other timings above use `build`-mode binaries,
which are unaffected by this cache.
