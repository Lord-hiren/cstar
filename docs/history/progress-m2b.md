# M2 Stage 2B — Backend Integration Progress

> **Historical record.** Written while Stage 2B was landed; counts and
> "remaining" items describe that moment (the current suite is 368 checks).
> See the [history overview](README.md).

**Scope of this document:** a working record of the Stage 2B *backend
integration* sub-goal (collections / Text / Bytes sector), the decisions
made while landing it, the defects it uncovered, and what remains.
It complements [m2b-semantics.md](../contracts/m2b-semantics.md), which stays the
normative contract for the stage; nothing here overrides it.

**Status:** backend sub-goals (collections, functions) + §15 function
tests + SPEC/README retag + VSCode 2B parity COMPLETE. Suite:
**268/268 green.** Version retagged `0.2.0-m2b`. Remaining: the §15
bench extension (collection/function/text cases with recorded numbers).

## 1. What "backend integration" delivered

The frontend (lexer, parser, semantic pass) and the runtime library were
already 2B-complete at the baseline commit (`59666cd`). This sub-goal made
the collection sector **executable end-to-end** by lowering every 2B
collection node in `src/codegen.cpp`:

| Surface | Lowering |
|---|---|
| Bytes literals `b"..."` | `PT_BYTES("…")` |
| List / map / set / tuple literals (incl. `[]`, `{}`, `set()`, `()`, `(x,)`, nesting) | `pt_list_new/of`, `pt_map_new/of`, `pt_set_new/of`, `pt_tuple_of` |
| Indexing `xs[i]`, `m[k]`, text/bytes index | `pt_index_get` (negative indices, range checks, map keys) |
| Index assignment `xs[i] = v`, `xs[i] <op>= v` | `pt_index_set` (+ `pt_index_get` for compound); container and index each evaluated **once** |
| Slicing `a:b:c` on text/bytes/list/tuple | `pt_slice` with `pt_unset()` for open bounds |
| Method calls `obj.m(args)` | `pt_call_method` (semantic already rejects named args and bare refs) |
| Comma-join `a, b` at full-expr positions | `pt_join` |
| Destructuring `a, b = …`, `(a, b), c = …`, `a, *r, b = …`, `*r = …`, index leaves | `pt_unpack_len` / `pt_unpack_get` / `pt_unpack_rest` / `pt_unpack_check` |
| Builtins `len str int float bool type encode decode set` | direct `pt_builtin_*` calls |
| `for x -> <list/map/set/tuple/text/bytes>` | the existing `pt_iter_open/next` protocol (now covers every iterable) |
| `->` membership over all containers | existing `pt_contains` (runtime was already extended) |
| `global a, b` declaration | no-op codegen — the semantic pass already resolves the writes to `pt_g_*` |

The M1/2A code paths are unchanged: a plain `x = …` still emits
byte-identical C, and the `((void)x)` warning suppression was extended to
walk the new AST nodes.

**Deliberately NOT lowered here (pending sub-goal: the function sector):**
the uniform function ABI (defaults evaluated at call time, `*args`,
`**kwargs`, named-argument binding at the call site, function values /
indirect calls, nested functions + closures/cells, `nonlocal`). Those
nodes now fail with an explicit
`… requires the pending 2B function-ABI sub-goal (line N)` message instead
of the pre-integration generic failures.

## 2. Decisions made during integration (integrator ratifications)

1. **Collection-literal evaluation order.** Elements evaluate
   left-to-right as written; a map literal evaluates key-before-value,
   pair by pair (`{"k": f(), "j": g()}` → `f(), g()` interleaved with
   their keys). Generated C A-normalizes every effectful element into its
   own temp first, so no C evaluation-order question can arise.
2. **Compound index assignment reads before the RHS.**
   `xs[i] += v` is lowered as `t = xs[i]` (current element), then `v`,
   then `t + v`, then `xs[i] = result` — matching the expanded form's
   left operand order. `xs` and `i` are evaluated once, before all of it.
3. **Destructuring count rule.** A rest-free pattern needs exactly as
   many elements as leaves (both directions error via
   `pt_unpack_check`, `expected N = leaf count`). A pattern with a rest
   leaf needs at least `L-1` elements (the rest may be empty) and never
   errors on extra elements; its too-few message also reports `N = L`
   (leaf count) so "expected" uniformly means "number of targets".
4. **`pt_unpack_rest` API extended to an explicit `[start, stop)` span.**
   The runtime originally implemented "from `start` to the end", which
   made a rest with trailing fixed leaves (`*h, t`, `a, *r, b`) — required
   by §15's "rest (middle + last)" — impossible. The header comment
   already said "Elements [start, count)", so the parameter list gained a
   `stop` (the codegen passes `n - (leaves after the rest)`); `stop` is
   clamped to the element count.
5. **Leading `*rest` statement dispatch.** `*h, t = xs` was rejected by
   the statement dispatcher although the §3 grammar allows a rest target
   anywhere in a `target_list`; the speculative assignment dispatch now
   also fires on a leading `*`.
6. **Method-arity message wording is a normative quirk.** The runtime
   prints `'list.append() takes 1 argument(s), got 0` with no closing
   apostrophe — exactly what M2-2B-SEMANTICS.md §12 quotes. Do not
   "fix" it without updating the contract; tests match on substrings.
7. **Retired M1-only error tests.** `err_nested_function` and
   `err_func_as_var` asserted M1 restrictions that 2B §0 explicitly lifts
   (nested functions are legal; reading a function name yields a function
   value). Their positive replacements require the function-ABI sub-goal
   and will be added with it. `make test` was kept green by removing the
   obsolete asserts rather than leaving the suite red.

## 3. Defects found and fixed by this sub-goal

1. **`bytes + bytes` produced `text`** (found by the new `txt_concat`
   acceptance test). `pt_concat` always returned a `pt_str()`-tagged
   value, so the concatenation of two bytes values lost its kind while
   mixed `bytes + text` correctly errored. Fixed: the result keeps the
   operand tag (§5: `+` concatenates text+text or bytes+bytes).
2. **Leading rest patterns did not parse** (decision 5 above).
3. **Middle rest was structurally impossible** via the old
   `pt_unpack_rest` signature (decision 4 above).
4. **Pre-existing (documented, NOT fixed here — function sub-goal):**
   a static call with *named* arguments (`f(y=1, x=5)`) passes semantic
   checks but the current codegen binds them **positionally** (`f(x, y)`
   receives `y=1` in `x`'s slot). The fix belongs with the uniform
   call-ABI lowering; until then named arguments on calls to fixed-arity
   user functions bind incorrectly. (`f(x=1, y=2)` in declaration order
   happens to work, which is why it can go unnoticed.)

## 4. Test-suite accounting

- Baseline at `59666cd`: 161 checks, 148 green, 13 red (the two approved
  2B change classes: `string`→`text` display rename, lifted M1
  restrictions).
- At this sub-goal: **233 checks, 233 green** (the stage closed at
  **268/268** — see §6). Added 74 acceptance cases from the §15
  collections-sector test plan:
  - `col_*` (19) — literals/display/nesting, aliasing, truthiness,
    indexing (incl. single-evaluation proofs), slicing (incl. new-object
    proof), iteration over every iterable, membership over every
    container, map insertion-order & key identity (`1`/`1.0` same key,
    `True`/`1` distinct), set uniqueness & order;
  - `rt_col_*` (12) — index/slice/map/hash errors;
  - `des_*` (4) + `rt_des_*` (4) — flat/nested/rest (leading, middle,
    trailing, lone) patterns, index leaves, local-scope leaves,
    join-vs-tuple disambiguation, count errors;
  - `jn_*` (3) — comma-join at assignment/return/expr-statement
    positions, nested displays, unaffected call/literal commas;
  - `meth_*` (4) + `rt_meth_*` (5) — the full method surface per §15 plus
    unknown-method / wrong-arity / wrong-type / named-arg / bare-ref
    errors;
  - `bi_*` (6) + `rt_bi_*` (6) — all builtins incl. error paths and
    reserved-name rules;
  - `txt_*` (4) + `rt_txt_*` (4) — code-point semantics (multibyte),
    bytes literals/display, encode/decode (+ invalid UTF-8), concatenation
    and mixed-type errors.
- 11 `rt_*` sidecars were retargeted from `string` to `text` (approved 2B
  change 1), and the 2 superseded M1-only error cases removed (§2/7).

## 5. Function sector (sub-goal 2) — COMPLETE

Landed the uniform function ABI end-to-end. Everything §5 of the previous
revision listed is now implemented; the decisions made while landing it:

1. **Uniform ABI for every function** (§13): `pt_value f(pt_value* bound,
   pt_cell** env)`. The static direct-call fast path of M1/2A is replaced
   by a stack bound vector at the call site (`pt_value b[] = {...};
   f(b, NULL)`), keeping binding semantics identical between static and
   dynamic calls. Top-level callees receive `env = NULL`.
2. **`callee_expr != null` ⇔ dynamic call.** The parser attaches a bare
   Var `callee_expr` to every name call; the semantic pass now DROPS it
   for static calls (top-level, env-less target), so codegen's
   "branch on callee_expr first" rule is sound. Both fields staying set is
   legal for dynamic calls (the bare name is kept for diagnostics).
3. **Entry-code order** (§13): locals/cells declared → closures hoisted →
   bound slots materialized (defaults in parameter order, vararg/kwarg
   default to fresh empty list/map on PT_MISSING) → body. Hoisting before
   slot materialization lets a default expression reference hoisted
   closures and locals; cells exist before any closure captures them.
4. **Capture storage:** own boxed bindings are `pt_cell*` variables
   allocated unset at entry; captured names are `env[i]->v` (the capture
   list order of the declaration is the env layout, deterministic per
   §6.4). Transitive capture hands cells down through the env chain —
   the parent resolves each of the child's captures to its own cell or
   its own env slot.
5. **`nonlocal`-declared names are dropped from the function's locals**
   (like `global`-declared ones). Defect found: without the drop,
   `n = n + 1` under `nonlocal n` collected `n` as a local, so the WRITE
   hit the owner's cell (bind_name) while the READ hit a fresh local cell
   — "used before assignment". `drop_global_declared_locals` generalized
   to `drop_declared_locals`.
6. **`*vararg`/`**kwarg` boxing flags** (`vararg_is_cell`/`kwarg_is_cell`)
   added to FuncDecl; a nested function may capture the vararg/kwarg name
   like any param.
7. **pt_funmeta** emitted per function (PT_UNUSED — metas are unused when
   a function is never passed around); slot names in ABI order; the
   has-default mask covers positional slots only.
8. **Runtime addition:** `pt_env_new(n)` — heap env vector, zero-filled
   (closures outlive their frame; 2B leaks by design).
9. **Generated C stays warning-clean** under `-Wall -Wextra` (verified
   via a kept gen.c): `(void)env` is emitted when no capture/closure slot
   referenced it (usage tracked during emission, since a function with
   nested decls may still never touch its own env).

Integration verification at this sub-goal: 233/233 checks green (no
regressions in the collections/M1/2A sectors); function-sector behavior
validated with
closure/defaults/vararg/kwargs/nonlocal/transitive-capture/recursion/
function-value smoke programs (§15 function-sector acceptance tests are
the follow-up task).

## 6. Completion record (tests + docs + editor)

- **§15 function-sector tests** (commit `21566f4`): 35 cases / 110 files —
  14 positive `fn_*` (incl. the retired-case replacements), 8 `rt_fn_*`,
  13 `err_fn_*`. Suite: **268/268 green** (233 prior + 35). The test
  agent found **zero** suspected compiler/runtime bugs — every probed
  behavior matched the contract byte-for-byte, including the PROGRESS
  §3.4 defect (static named-arg calls) now verified fixed by
  `fn_named_args`.
- **SPEC.md + README.md** rewritten for 2B; version retagged `0.2.0-m2b`
  (`src/main.cpp` kVersion + docs). All quoted error messages were
  grep-verified against the compiler/runtime sources and the `.err`
  fixtures; both new README Quickstart demos were executed and match
  their documented output exactly.
- **VSCode extension 2B parity** (commit `489e3db`): `global`/`nonlocal`
  keywords, `b"..."` literals (word-bounded — `bad` never matches),
  collection punctuation + slice-colon scoping, method-call scope escape
  (`b.decode()`), index-assign targets, 9 new builtin completions +
  gated 23-name method completions, 4 new snippets. Smoke suite recreated
  as `editors/vscode/test/smoke.js` (the referenced 457-check script did
  not exist anywhere): **653/653 green** (362 baseline on the 2A
  extension + 291 new 2B checks, zero 2A regressions).
- **Contract amendments (integrator):**
  1. `method arguments must be positional` moved from §12 (runtime) to
     §11 (compile-time) — the semantic pass rejects named method
     arguments statically (`semantic.cpp` MethodCall); the
     implementation is stricter than the original table.
  2. Static arity wording clarified in §11 (see below).
- **Orphan sidecars removed**: `err_nested_function.{out,exit}` and
  `err_func_as_var.out` — leftovers of the two retired M1-only error
  cases (their `.pen` files were deleted at the 2B decision); the
  harness iterates `*.pen` so they were inert, now gone.
- **Test-agent clarifications recorded** (behavior confirmed, now in
  §11/§6.3 of the contract): vararg/kwarg names are not named-argument
  targets (`f(1, rest=[9])` → `unexpected argument name 'rest'`);
  `**kwargs` absorbs only *named* extras (extra positionals against
  `g(a, **kw)` still give `expects at most 1`); a conflict diagnostic
  for assigning to a nested function's name may be reported at both the
  declaration and the assignment site (collect-all diagnostics);
  `main():` executes at its declaration position and a nested
  declaration inside `main():` registers as a global function callable
  after `main`.

## 7. Performance lane (sub-goal 3) — COMPLETE, Stage 2B done

- §15 bench extension landed: `bench/gen-m2b.py` + `bench/run-m2b.sh` +
  `bench/cases/m2b/` (10 cases + C++ twins, byte-equality verified every
  run; the `while_*` cases reuse the exact M1 workloads — same step
  counts/arithmetic, and `while_string` the exact frame structure — for a
  true chains-vs-loops before/after). Recorded baseline:
  `bench/results/m2b-baseline.md`; write-up, findings, and next
  optimizations: `docs/performance/baseline-m2b.md` (anchor: `docs/performance/baseline-m1.md`).
- Headline numbers (i7-6600U, pinned, 7 reps, `pentest 0.2.0-m2b`):
  compile **1.4–2.0 s flat for every case** and **19–68x faster** than the
  M1 chain programs that do the same work (superlinear clang cliff gone);
  collection-level runtime **competitive with -O2 C++** (set_ops 1.3x,
  map_ops 3.6x, list_ops 6.0x, text_ops 3.2x); scalar loops 25–155x
  (documented dynamic-dispatch tax) and real loops 1.9–2.3x slower than
  M1's inlined call chains on identical arithmetic — the honest cost of
  per-iteration loop bookkeeping, with the named fixes in
  `docs/performance/baseline-m2b.md` §7.
- Three performance defects were **found by the new baseline and fixed in
  the same stage** (each re-verified at 268/268 before re-measuring;
  `set_ops` ledger: 46,752 ms → 26,640 → 23,353 → **86 ms**, 647x → 1.3x
  vs the C++ twin):
  1. `pt_table_first` rescanned the insertion-order array from index 0 on
     every call — front-draining `pop()` was quadratic. Fixed with a
     maintained `first` cursor (append-only entries ⇒ amortized O(1)).
  2. `pt_table_clear_slot`'s forward re-home walk spanned the whole live
     probe run per delete — a second quadratic. Provably unnecessary
     (`find` skips tombstones and dead-entry slots; insert reuses
     tombstones in the key's own chain; the 75% rehash guard keeps
     empties reachable) — now plain tombstoning.
  3. `pt_contains` (map/set arm of `->`) computed the hash then scanned
     the whole entry array — O(size) membership. Now probes the slot
     table via `pt_table_find` (identical match condition).
- Hot-path inlining: `pt_guard` + `pt_truthy` moved to `static inline` in
  `runtime/pentest_rt.h` (collection truthiness delegates to
  `pt_truthy_obj`; `struct pt_obj` stays runtime-private). Gains: 11–24%
  on method/dispatch-heavy programs, retained across runs; no measurable
  change on pure-arith loops (documented, with the static-bool-condition
  elision fix named).

**Stage 2B is complete.** Next: Stage 2C — classes, exceptions, GC
(+ object identity `is`); the GC also clears the documented no-GC RSS
rows in `docs/performance/baseline-m2b.md` §3/§7.
