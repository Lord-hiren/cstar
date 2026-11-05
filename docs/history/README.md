# History

How Pentest got to `0.2.1`: the milestones, the records of how each was landed,
and the original plan. For a per-version list of what changed, see the
[changelog](../../CHANGELOG.md).

## Documents here

| File | What it is |
|---|---|
| [design.md](design.md) | the original Milestone 1 design/plan (vision, syntax, execution model, scope, open questions). Written before any code; **superseded** by the contracts and the SPEC where they differ, but still the source of the four canonical examples (SPEC Appendix A.1–A.4) and of the "Approach A / Approach B" compiler decision |
| [progress-m2b.md](progress-m2b.md) | working record of Stage 2B backend integration: what landed, integrator decisions, defects found, test accounting (233/233 at the point it was written) |
| [progress-m2c.md](progress-m2c.md) | working record of Stage 2C (GC, exceptions, classes): what landed per sector, ratified decisions, defects found, test inventory |

These are **records, not specifications**. Where one says "remaining" or quotes a
test count, it describes the moment it was written; for the current suite see
[building and testing](../development/building-and-testing.md). They also keep
the internal names of their time: `pentest_rt` is now `cstar_rt`, the
`pt_`/`PT_` prefixes are `cs_`/`CS_`, and `__ptm` module names are `__csm`.

## Timeline

Dates and commits are from the repository history.

| Date | Milestone | Commits |
|---|---|---|
| 2026-10-12 | Design document written (file timestamp; first committed on 10-13 together with M1) | — |
| 2026-10-13 | **M1** — the strict core (version string `0.1.0-m1`): single-file scripts, functions, `main():`, `run`/`build`/`check`, content-hash cache | `37c194a` v0.1 |
| 2026-10-13 | Stage 2A (control flow, operators, `range`) and Stage 2B frontend + runtime land together as a checkpoint; M1 baseline frozen and measured; collections lowered through codegen | `59666cd`, `feab461`, `1ea4261` |
| 2026-10-14 | **Stage 2B** complete — function overhaul (uniform ABI, closures, `nonlocal`), 35 function-sector tests, VSCode parity, SPEC/README rewrite, perf lane with three measured table fixes; retagged `0.2.0-m2b` | `af40fd8`, `21566f4`, `489e3db`, `3603043`, `21e665d` |
| 2026-10-15 | **Stage 2C** — garbage collector, exceptions, classes and `is`; 23 acceptance cases; VSCode parity; RSS baseline re-recorded; SPEC/README rewrite; tagged `0.2.0-m2c` | `e506be8`, `0ae3810`, `b4ab583`, `5077512`, `cfc633a`, `dcf1c71` |
| 2026-10-15 → 16 | **`0.2.1`** — optimisation lane, `input`/`time`/`clock`, cross-scope `variable.` access, the Python-parity builtin set | `86deffe`, `c1aa8a7`, `db2d029` |
| 2026-10-16 | Documentation reorganised (this tree), known issues recorded | — |

Stage 2A has no separate commit: its implementation and its contract
([m2a-semantics](../contracts/m2a-semantics.md)) were both first committed in the
first M2 checkpoint (`59666cd`).

## Naming you will meet in the code

* **"Stage 2D"** originally labelled the cross-scope `variable.` work in a few
  code comments and in `bench/run-varq.sh`; the SPEC (§13) reserves Stage 2D
  for **modules/imports, generators and reachability analysis**. Modules,
  multi-file programs and selective-loading reachability analysis are now
  implemented in the unreleased tree ([modules](../guide/modules.md); SPEC §14);
  generators remain. Cross-scope access shipped in `0.2.1`.
* **"post-2C"** in comments means work done after the 2C contract closed, i.e. the
  `0.2.1` lane.
* **`TODO.txt`** is cited by some comments as the origin of the "speed goal". It is a
  git-ignored local file and is not part of the repository.
* **`M2-2B §13`**-style citations refer to sections of the stage contracts in
  [`../contracts/`](../contracts/README.md).

## Roadmap (from SPEC §13)

| Stage | Scope | State |
|---|---|---|
| 2A | control flow, expressions, `range()` | done |
| 2B | collections, text/bytes, functions and closures | done |
| 2C | classes, exceptions, garbage collection | done |
| 0.2.1 | performance lane, cross-scope access, Python-parity builtins | done |
| 2D | modules/imports and multi-file programs, selective loading, generators | modules/imports + selective loading implemented in the unreleased tree; generators not started |
| 2E | tooling and performance: diagnostics polish, editor support, benchmarks, possibly a direct LLVM backend | not started |

Beyond Milestone 2: static typing, a standard library (`net`, `web`, `bytes`,
`parallel`, `proc`, `parse`, …), class-system depth (inheritance, user exception
types), the deferred builtins `bytearray`/`complex`/`frozenset`, and portability
beyond Linux x86_64. Before any of that, the remaining low-severity entries in
[known issues](../reference/known-issues.md) (a `format()` padding corner and
cache-less scratch litter) should be closed.
