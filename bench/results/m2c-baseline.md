# Pentest Stage 2C performance baseline

- Date: 2026-10-15 04:27:27 UTC
- Machine: `Linux DeathMachine 7.1.5+kali-amd64 #1 SMP PREEMPT_DYNAMIC Kali 7.1.5-1kali1 (2026-07-29) x86_64 GNU/Linux`
- CPU: Intel(R) Core(TM) i7-6600U CPU @ 2.60GHz (4 logical cores), RAM: 15.4 GiB
- Compiler (C++ twins): Debian clang version 21.1.8 (13)
- Compiler (Pentest backend, invoked internally): Debian clang version 21.1.8 (13)
- Pentest compiler: pentest 0.2.0-m2c
- C++ twin flags: `clang++ -std=c++17 -O2`
- Pentest compile flags (fixed by the CLI): `clang -std=c11 -O2 -o <out> gen.c pentest_rt.c -lm`
- Timing pinning: taskset -c 0 (pinned)
- Repetitions: 7 per binary; **min** and **median** wall-clock ms of
  program execution only (compile excluded: .pen programs are built ahead
  of time with `build`, which never caches).
- Methodology identical to the 2B baseline (bench/run-m2b.sh +
  docs/performance/baseline-m2b.md, same machine/pinning/reps) so the before/after
  tables below are apples-to-apples; the 2B rows are the frozen
  `0.2.0-m2b` leak-everything run.

## Runtime (program execution only)

| Case | Workload | Pentest min (ms) | Pentest median (ms) | C++ min (ms) | C++ median (ms) | Median ratio | Peak RSS Pentest (KB) | Peak RSS C++ (KB) |
|---|---|---|---|---|---|---|---|---|
| while_int | 12,976,128 int LCG steps (real while loop) | 1703.847 | 1750.473 | 10.915 | 12.162 | 143.9x | 12300 | 12552 |
| while_float | 12,976,128 double steps (real while loop) | 1689.249 | 1735.826 | 60.185 | 64.704 | 26.8x | 12356 | 12388 |
| while_string | 750,000 frames: 600,000 appends + 150,000 resets (= M1 string_ops) | 207.234 | 212.363 | 18.023 | 20.307 | 10.5x | 12440 | 12440 |
| list_ops | 200,000 appends + sum pass + index reads | 52.925 | 58.778 | 9.436 | 12.341 | 4.8x | 12544 | 12344 |
| map_ops | 150,000 inserts + 300,000 lookups + key-sum pass | 120.085 | 130.311 | 31.564 | 33.708 | 3.9x | 16720 | 12352 |
| set_ops | 240,000 adds (half dup) + 120,000 finds + full drain | 123.113 | 125.539 | 81.839 | 83.186 | 1.5x | 13548 | 12444 |
| text_ops | 200,000 x (upper+split+join+replace+find+starts_with) | 520.840 | 559.798 | 145.517 | 165.849 | 3.4x | 12356 | 12556 |
| closure_ops | 300,000 closure creations + calls via function value | 237.195 | 258.173 | 7.631 | 8.882 | 29.1x | 12428 | 12344 |
| dispatch_ops | 300,000 indirect calls (positional+named+**kwargs) | 352.121 | 377.781 | 6.095 | 7.631 | 49.5x | 12428 | 12492 |
| coll_mixed | 202,752 reps int+list+map+text (period-80 reset) | 466.799 | 508.564 | 13.288 | 15.144 | 33.6x | 30256 | 12284 |

## Before/after vs the recorded 2B baseline (same machine, pinning and reps)

### Timings — the collector's cost (2B leak-everything vs 2C mark & sweep)

| Case | 2B median (ms) | 2C median (ms) | 2C/2B | 2B build (ms) | 2C build (ms) |
|---|---|---|---|---|---|
| while_int | 1242.316 | 1750.473 | 1.41x | 1835 | 2339 |
| while_float | 1235.847 | 1735.826 | 1.40x | 1726 | 2559 |
| while_string | 169.524 | 212.363 | 1.25x | 1854 | 2535 |
| list_ops | 39.687 | 58.778 | 1.48x | 1933 | 2761 |
| map_ops | 85.428 | 130.311 | 1.53x | 1787 | 2502 |
| set_ops | 86.090 | 125.539 | 1.46x | 1830 | 2633 |
| text_ops | 349.213 | 559.798 | 1.60x | 2038 | 2489 |
| closure_ops | 235.075 | 258.173 | 1.10x | 1448 | 2683 |
| dispatch_ops | 487.523 | 377.781 | 0.77x | 1385 | 2812 |
| coll_mixed | 275.237 | 508.564 | 1.85x | 1425 | 2393 |

### Peak RSS — the 2C headline (2B grew without bound; 2C must sit at the floor)

| Case | 2B Pentest RSS (KB) | 2C Pentest RSS (KB) | Collapse | 2C C++ RSS (KB) |
|---|---|---|---|---|
| while_int | 12344 | 12300 | 1.0x smaller | 12552 |
| while_float | 12396 | 12356 | 1.0x smaller | 12388 |
| while_string | 44188 | 12440 | 3.6x smaller | 12440 |
| list_ops | 12296 | 12544 | 1.0x smaller | 12344 |
| map_ops | 15856 | 16720 | 0.9x smaller | 12352 |
| set_ops | 13652 | 13548 | 1.0x smaller | 12444 |
| text_ops | 89380 | 12356 | 7.2x smaller | 12556 |
| closure_ops | 119128 | 12428 | 9.6x smaller | 12344 |
| dispatch_ops | 344180 | 12428 | 27.7x smaller | 12492 |
| coll_mixed | 166016 | 30256 | 5.5x smaller | 12284 |

(2B rows are read from bench/results/timings-m2b.txt, rss-m2b.txt and
compile-pen-m2b.txt — the frozen `0.2.0-m2b` run on this machine. The 2B
RSS column is the leak: 44–344 MB of heap for workloads that need ~12 MB;
the 2C column is the same workloads after the non-moving mark & sweep
collector landed (SPEC §7.3). The timing ratio is the collector's cost.)

## Compile time (.pen -> native, cold; `build` never caches)

| Case | .pen source bytes | Pentest build (ms) | C++ twin build (ms) |
|---|---|---|---|
| while_int | 278 | 2339 | 222 |
| while_float | 249 | 2559 | 857 |
| while_string | 517 | 2535 | 762 |
| list_ops | 333 | 2761 | 646 |
| map_ops | 439 | 2502 | 824 |
| set_ops | 457 | 2633 | 687 |
| text_ops | 533 | 2489 | 1135 |
| closure_ops | 373 | 2683 | 246 |
| dispatch_ops | 427 | 2812 | 225 |
| coll_mixed | 703 | 2393 | 1071 |

## `run`-mode content-hash cache probe (while_int, fresh XDG_CACHE_HOME)

| Measurement | Wall time (ms) |
|---|---|
| cold `run` (compile + exec) | 4164 |
| warm `run` (cache hit, exec only) | 1673 |

The cache lives in `$XDG_CACHE_HOME/pentest/` (fallback `~/.cache/pentest/`),
keyed by sha256(source bytes + compiler version). The probe directory was
removed after measuring. All other timings above use `build`-mode binaries,
which are unaffected by this cache.
