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
pointers to the regression tests. What remains open:

| # | Area | Summary | Severity |
|---|---|---|---|
| [7](#7-format-zero-padding-plus-grouping-does-not-regroup) | builtins | `format(42, "08,")` → `00000042`, Python `0,000,042` | low — narrow spec corner |
| [8](#8-a-cache-less-run-leaves-its-binary-in-the-temp-dir) | driver | a run that cannot cache leaves `$TMPDIR/cstar-XXXXXX/program` | low — cosmetic litter |
| [9](#9-text-concatenation-in-a-loop-is-quadratic) | runtime | `s += "x"` in a loop is O(n²) (no refcount-based in-place resize) | medium — common pattern, easy workaround |
| [10](#10-float-vector-modulo-returns-wrong-values) | runtime | `fvec([5.0]) % fvec([2.0])` → `[2.5]` (wrong values) instead of a `TypeError` | medium — silent wrong results |

---

## 7. `format` zero-padding plus grouping does not regroup

**Repro**

```python
print(format(42, "08,"), format(1234567, "012,"), format(42, "06,"))
```

Python prints `0,000,042 0,001,234,567 00,042`; cstar prints
`00000042 0001,234,567 000042`. (`format(1234567, "+012,")` happens to
coincide: `+001,234,567`.)

**Expected** (SPEC §5.11.3 grammar `[0][width][,]`, Python parity) — with
zero-padding **and** grouping, Python pads the digit run until the
eventually-grouped result reaches the width, then groups the padded run;
the grouped form may overshoot the width (`08,` → eight digits `00000042`
→ `0,000,042`, ten chars).

**Cause.** Both numeric paths of `cs_format_apply` in
[runtime/cstar_rt.c](../../runtime/cstar_rt.c) group first into
`numbuf` and then prepend `width - (len(numbuf))` zero characters to the
already-grouped string — the commas never move. Plain `","`, `",.2f"` and
`width[,]` padding without the `0`/`=`-zero path are correct.

**Workaround.** Do the padding yourself: `format` without `,`, then insert
separators into the zero-padded digit run.

---

## 8. A cache-less run leaves its binary in the temp dir

**Repro** — any `cstar run` while caching is unavailable (no `sha256sum`
on `PATH`, or an unwritable cache directory):

```bash
t=$(mktemp -d)
echo 'print("hi")' > "$t/hello.cst"
PATH=/tmp/onlyclang TMPDIR="$t" build/cstar run "$t/hello.cst"
ls "$t"/cstar-*/   # program
```

**Observed** — `$TMPDIR/cstar-XXXXXX/program` remains after the program
exits. The generated sources are removed; only the executable stays.

**Why.** Without a usable cache the driver compiles into the temp directory
and `execv`s the binary in place (`try_exec` in `src/main.cpp`): the
driver's process *becomes* the program, so nothing can clean up afterwards.
This is deliberate (`remove_sources`: "the rmdir fails harmlessly when the
compiled executable lives inside dir") but it is the configuration where the
zero-junk wording of [optimization.md](../performance/optimization.md) §0
does not hold. In the normal configuration (cache keyed and writable) the
executable goes to the cache and the scratch directory is fully removed.

**Workaround.** Keep `sha256sum` installed and the cache directory writable;
otherwise `rm -rf "${TMPDIR:-/tmp}"/cstar-*` afterwards.

---

## 9. Text concatenation in a loop is quadratic

**Repro**

```python
s = ""
i = 0
while i < 100000:
    s += "x"
    i += 1
print(len(s))
```

≈ 0.63 s on the reference machine; CPython does the same program in ≈ 0.05 s.

**Why.** CPython's `s += x` resizes the string's buffer **in place** when the
buffer's refcount is 1. cstar has no per-buffer refcounts, and an in-place
append would be unsound whenever a second value shares the buffer
(`s2 = s; s += "a"; s2 += "b"` would corrupt what `s2` sees). So every `+`
allocates a fresh GC buffer and copies both operands: O(n) per step, O(n²)
over the loop, plus GC churn for the dead intermediates (reclaimed correctly
since the text-kind fix above). The same cost class as Java `String` and
Ruby `String` concatenation.

**Workaround.** Append to a list and join — `join` pre-sizes and copies once
(O(n), ≈ 7× faster for the repro above):

```python
parts = []
i = 0
while i < 100000:
    parts.append("x")
    i += 1
s = "".join(parts)
```

---

## 10. Float-vector modulo returns wrong values

**Repro**

```python
print(fvec([5.0, -5.0]) % fvec([2.0, 2.0]), 1.0 % fvec([4.0]), fvec([1.0]) % 0.0)
```

The program prints `[2.5, -2.5] [4.0] [inf]`: no error, and none of the
values is a remainder.

**Expected** (SPEC §5.11.9): `%` is not defined for `float`, so on an `fvec`
it raises the same `TypeError` as the scalar form —
`unsupported operand types for %: fvec and fvec` (`fvec and float`,
`float and fvec`). `%` on an `ivec` is correct.

**Cause.** The `fvec` kernels in [runtime/cstar_rt.c](../../runtime/cstar_rt.c)
switch on `+`, `-`, `*` and send every other operator to their `default`
branch, and `%` reaches them: `cs_fv_kernel` (vector with vector or with a
right-hand scalar) divides, `cs_fv_kernel_sl` (left-hand scalar) multiplies.

**Workaround.** None needed in correct programs: cstar has no float modulo.
For integer-valued data use an `ivec`, whose `%` is exact.

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
