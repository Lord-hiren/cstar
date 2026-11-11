# Known issues (version 0.2.1)

Defects and spec deviations that were **reproduced against the real toolchain**
(this page is the living registry — it holds what is currently open). Each
entry has a minimal repro, the observed behaviour, the behaviour the
specification promises, and (where we have one) a workaround.

How to read it: the SPEC and the stage contracts are normative, so when the
compiler disagrees with them the compiler is wrong and the entry is a bug.
When a fix lands, delete the entry and note it in
[CHANGELOG.md](../../CHANGELOG.md).

The six issues found during the documentation pass (second+ named `except`
clauses, uncatchable `int`/`float` parse errors, `float` raising the
wrong class, the SIGPIPE death without `sha256sum`, left-counting
`format` grouping and its `,`-after-width rejection, and overstated hover
text for `Exception`/`RuntimeError`) are **fixed** — as are the GC
use-after-free on heap text/bytes buffers and `print`'s per-argument heap
allocation found afterwards — see the `Fixed` section of the changelog for
pointers to the regression tests. So are the four issues recorded after
them, each with a regression test:

| # | Was | Regression test |
|---|---|---|
| 7 | `format(42, "08,")` gave `00000042` (Python `0,000,042`); the same padding step also misplaced a float's `-` sign, ignored explicit fills next to the `0` flag and counted bytes instead of code points | case `bi_py_format_pad` |
| 8 | a `run` that could not cache left `$TMPDIR/cstar-XXXXXX/program` behind | harness check `cacheless_cleanup` |
| 9 | text built with `s += x` inside a function was quadratic | case `txt_build_amortized`, harness check `text_build_grow` |
| 10 | `fvec % x` returned wrong values instead of a `TypeError` | case `rt_fvec_mod` |

**No issue is currently open.** New entries keep counting from 11.

---

## Documentation corrections made during this pass

For traceability, these were wrong in the *docs* (not the compiler) and have
been corrected:

* README claimed a 320-check suite built from 316 cases; the figures moved
  with the fixes below and are **368 checks = 361 program cases + 7 extra
  checks**.
* SPEC §6.10 listed `Exception` as the "catch-all" for `except Exception:`;
  the implementation (correctly, per the contract) matches by exact class.
* SPEC §10 said `build tools/scan.pen` writes `tools/scan.pentest`; the
  driver writes `./scan.pentest` (current directory). The SPEC text was
  corrected to match the implementation.
* `examples/functions.pen` no longer compiled since `0.2.1` made `sum` a
  reserved builtin; the example's function was renamed.
* `docs/performance/optimization.md` §8 cited `lab.pen` and
  `c-things/test_timeout.c`, which are git-ignored local scratch files; the
  section now says so.
* An earlier draft implied `check` should ignore unused imported code like
  `run` does. That is **not** a bug: `check` is deliberately strict and
  validates every imported declaration, reachable or not
  ([modules](../guide/modules.md#selective-loading-you-only-pay-for-what-you-name)).
