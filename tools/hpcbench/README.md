# hpcbench — the scoreboard for the speed phases

A small, honest A/B corpus for measuring the five speed dimensions against
optimized C, plus the categories where C should win outright (branchy
logic, pointer chasing, I/O) so the scoreboard stays honest. It exists to
answer one question per workload: *does the fused Pentest program match
hand-written single-pass C, and by how much does it beat the unfused
forms?*

## Workloads

| Workload | Kernel | Variants |
|---|---|---|
| `saxpy` | `y = 2.5 * x + y`, 40 passes over 8M doubles | `pen-fused`, `c-multi`, `c-fused` |
| `chain3` | `z = (2.0 * x + 1.0) * 3.0 + 0.5`, 30 passes | `pen-fused`, `pen-dyn`, `c-multi`, `c-fused` |
| `dot` | `s = dot(x, y)`, 50 passes over 8M doubles | `pen-fused`, `c-fused` |
| `collatz` | total stopping steps over 1M..4M — unpredictable branches | `pen`, `c` |
| `ptrlist` | 6 passes over a shuffled 1M-node linked list — dependent loads | `pen`, `c` |
| `io` | write 600k formatted records, read the file back, 4 reps | `pen`, `pen-join`, `c-stdio`, `c-raw` |

8M doubles = 64 MB per array — comfortably past any LLC, so every pass
pays real DRAM bandwidth and *pass count is the structural term*.

**The baselines, defined honestly:**

* **c-multi** — one C loop per operation, with real temporary arrays.
  This is what an unfused compilation emits and a common clarity-first C
  style. It is a legitimate C program, compiled with the same flags.
* **c-fused** — one C loop holding the whole expression: what a C
  programmer writes by hand for these kernels, and what `clang -O3`
  vectorizes to. This is the bar for *parity*.
* **pen-dyn** (`chain3` only) — the same Pentest arithmetic with a
  float-variable coefficient, which the v1 shape lattice deliberately
  does not prove (scalars must be int-lane or literals), so the statement
  takes the dynamic route: one heap temporary per operator. Same
  checksum, identical math — it prices **fusion itself**.
* **collatz / ptrlist** — one idiomatic form per language, no fusion
  analogue: collatz is all dependency chain and unpredictable branches
  (Pentest's proven-int slots compile to raw int64 C), ptrlist is the
  object model under a latency-bound dependent load chain (class
  instances chasing `val`/`nxt` attributes, identical Lehmer-LCG shuffled
  chain in both languages).
* **io** — the same task, idiomatic in each language: `pen` writes one
  formatted record per `f.write` call, `c-stdio` one `fprintf` per
  record; `pen-join` preformats+joins into one `write`, `c-raw`
  preformats into one `fwrite` (each language's fused-write form). The
  runner deletes the data file the workloads leave in the temp dir
  (Pentest has no file-delete builtin).

All three `.pen` files exercise the fused kernel emitter
(`analyze_native_fvecs`); all C files compile with the driver's own line:
`-std=c11 -O3 -march=native`, and never `-ffast-math` (bit parity is a
contract).

## Method (the repo's rules, applied)

* **Primary metric: kernel seconds**, measured inside each program with
  the same clock (`CLOCK_MONOTONIC` in C, `clock()` in Pentest) around
  the timed section only — setup, checksum and printing are outside.
* **One warm-up pass before the timed section, in every variant.** A
  fresh 64 MB buffer's first touch costs a page-fault + zero-fill pass
  (measured: ~140 ms on this host) that has nothing to do with kernel
  speed; C pays it in its setup fill, so the `.pen` variants pay it in an
  explicit untimed statement (which also parks the buffer in the
  runtime's reuse pool). The warm-up is value-neutral, or applied
  symmetrically when it is not (saxpy gets 41 passes in every variant).
* **Secondary: whole-process CPU time** from `getrusage(RUSAGE_CHILDREN)`
  around each run (single-threaded work, so it corroborates rather than
  replaces the kernel clock).
* **Interleaved, rotated, min-of-N.** The runner executes rounds in
  rotating start order (defeats thermal/scheduler drift) and reports the
  minimum of `HPCBENCH_ROUNDS` samples (default 5).
* **Checksums asserted identical** across every variant of a workload in
  every round (exact float equality — the values are exact dyadic
  doubles, so any semantic divergence fails the run).
* This host's noise floor is ±30–50%: **only effects ≥ ~1.5× should be
  claimed** from these numbers.

## Running

```bash
make                                   # build/pentest must exist
python3 tools/hpcbench/run.py --write  # builds, verifies, runs, saves RESULTS.md
HPCBENCH_ROUNDS=9 python3 tools/hpcbench/run.py   # more samples
```

Build artifacts go to a temporary directory — nothing is left in the
tree. Exit code 1 means a build failure, a crash, or a checksum
mismatch (i.e. the comparison itself is invalid — never report those
numbers).

## Reading the results

* `pen-fused` ≈ `c-fused` (within noise) is **parity with hand-written
  SIMD C** — the Phase 1 bar.
* `pen-fused` vs `c-multi` is the pass-count win over idiomatic
  multi-statement C (chain3: one pass instead of three → the big number
  available without leaving a single core).
* `pen-fused` vs `pen-dyn` is the fusion win *inside* Pentest.
* Beating `c-fused` itself is what the later phases are for (multicore
  over race-free loops, layout, work elimination) — do not expect it here.
* `collatz` tests whether the int-lane keeps branchy scalar code at C
  speed; `ptrlist` prices the object model (attribute lookup under a
  dependent-load chain, where memory latency hides some but not all of
  the instruction overhead); `io` prices formatted write throughput.
  These three are where C is expected to win — the `vs fastest C`
  ratio column exists mainly for them.

Recorded runs live in [RESULTS.md](RESULTS.md).
