# Pentest M1 performance baseline

- Date: 2026-10-13 09:42:30 UTC
- Machine: `Linux DeathMachine 7.1.5+kali-amd64 #1 SMP PREEMPT_DYNAMIC Kali 7.1.5-1kali1 (2026-07-29) x86_64 GNU/Linux`
- CPU: Intel(R) Core(TM) i7-6600U CPU @ 2.60GHz (4 logical cores), RAM: 15.4 GiB
- Compiler (C++ twins): Debian clang version 21.1.8 (13)
- Compiler (Pentest backend, invoked internally by pentest-m1): Debian clang version 21.1.8 (13)
- Pentest compiler: pentest 0.1.0-m1 (preserved copy: `bench/tools/pentest-m1`)
- C++ twin flags: `clang++ -std=c++17 -O2`
- Pentest compile flags (fixed by the M1 CLI): `clang -std=c11 -O2 -o <out> gen.c pentest_rt.c -lm`
- Timing pinning: taskset -c 0 (pinned)
- Repetitions: 7 per binary; **min** and **median** wall-clock ms of
  program execution only (compile excluded: .pen programs are built ahead of
  time with `pentest-m1 build`, which never caches).

## Runtime (program execution only)

| Case | Workload | Pentest min (ms) | Pentest median (ms) | C++ min (ms) | C++ median (ms) | Median ratio | Peak RSS Pentest (KB) | Peak RSS C++ (KB) |
|---|---|---|---|---|---|---|---|---|
| startup | process start + init + `print(0)` + teardown | 4.407 | 5.694 | 5.559 | 7.495 | 0.8x | 12372 | 12224 |
| int_loop | 12,976,128 int LCG steps (K=2048 x N=6336) | 497.235 | 531.297 | 8.094 | 9.177 | 57.9x | 12136 | 12220 |
| float_loop | 12,976,128 double steps (K=2048 x N=6336) | 532.497 | 592.343 | 49.418 | 52.137 | 11.4x | 12324 | 12220 |
| call_chain | 2,400,256 frames x (6 int ops + 3 cmp ops + 1 call) (K=1024 x N=2344) | 643.328 | 742.788 | 21.708 | 22.615 | 32.8x | 12272 | 12116 |
| call_overhead | 12,976,128 pass-through calls (K=2048 x N=6336) | 78.093 | 81.777 | 28.561 | 30.049 | 2.7x | 12340 | 12340 |
| string_ops | 600,000 x 16-byte string appends (K=125 x N=6000, period-5 reset) | 77.040 | 88.765 | 12.317 | 12.861 | 6.9x | 45004 | 12216 |
| mixed | 500,000 int steps + 400,000 appends (K=125 x N=4000) | 76.876 | 80.810 | 10.552 | 11.780 | 6.9x | 31580 | 12268 |

## Compile time (.pen -> native, cold; `pentest-m1 build` never caches)

| Case | .pen source bytes | Pentest build (ms) | C++ twin build (ms) |
|---|---|---|---|
| startup | 85 | 344 | - |
| int_loop | 207275 | 111785 | - |
| float_loop | 197032 | 117192 | - |
| call_chain | 203531 | 47400 | - |
| call_overhead | 160176 | 86899 | - |
| string_ops | 170558 | 35355 | - |
| mixed | 171835 | 58927 | - |

## `run`-mode content-hash cache probe (int_loop, fresh XDG_CACHE_HOME)

| Measurement | Wall time (ms) |
|---|---|
| cold `run` (compile + exec) | 120180 |
| warm `run` (cache hit, exec only) | 513 |

The cache lives in `$XDG_CACHE_HOME/pentest/` (fallback `~/.cache/pentest/`),
keyed by sha256(source bytes + compiler version). The probe directory was
removed after measuring. All other timings above use `build`-mode binaries,
which are unaffected by this cache.
