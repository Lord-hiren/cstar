# hpcbench results — 2026-10-18 02:01

* host: Intel(R) Core(TM) i7-6600U CPU @ 2.60GHz, 4 CPUs, kernel 7.1.5+kali-amd64
* clang: Debian clang version 21.1.8 (13)
* flags: `-std=c11 -O3 -march=native` for C, the driver's own line for `.pen` (no fast-math either side)
* method: 5 interleaved rounds, rotated start order; min-of-5; checksums asserted identical across variants every round

| workload | variant | kernel s (min) | CPU s (min) |
|---|---|---|---|
| saxpy | pen-fused | 0.596 | 0.650 |
| saxpy | c-multi | 0.823 | 0.895 |
| saxpy | c-fused | 0.424 | 0.482 |
| chain3 | pen-fused | 0.316 | 0.358 |
| chain3 | pen-dyn | 1.269 | 1.366 |
| chain3 | c-multi | 0.912 | 0.994 |
| chain3 | c-fused | 0.307 | 0.355 |
| dot | pen-fused | 0.425 | 0.456 |
| dot | c-fused | 0.783 | 0.815 |
| collatz | pen | 0.907 | 0.924 |
| collatz | c | 0.870 | 0.870 |
| ptrlist | pen | 1.462 | 2.885 |
| ptrlist | c | 0.516 | 0.611 |
| io | pen | 0.270 | 0.724 |
| io | pen-join | 0.893 | 1.576 |
| io | c-stdio | 0.410 | 0.516 |
| io | c-raw | 0.349 | 0.452 |

Ratios (kernel time; the Pentest baseline is the workload's first `pen*` variant; >1 means Pentest is faster):

| workload | vs c-multi | vs c-fused | vs fastest C | vs pen-dyn |
|---|---|---|---|---|
| saxpy | 1.38× | 0.71× | 0.71× | — |
| chain3 | 2.89× | 0.97× | 0.97× | 4.02× |
| dot | — | 1.84× | 1.84× | — |
| collatz | — | — | 0.96× | — |
| ptrlist | — | — | 0.35× | — |
| io | — | — | 1.29× | — |

Raw min kernel times per round are the reported values; the full sample vectors stay in the runner output above.
