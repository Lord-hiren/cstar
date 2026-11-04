# M2 Stage 2C — GC, Exceptions, Classes Progress

> **Historical record.** Written while Stage 2C was landed; counts and
> "remaining" items describe that moment (the current suite is 368 checks).
> See the [history overview](README.md).

**Scope of this document:** a working record of Stage 2C — the garbage
collector sector (§1), the exception sector (§2), and the class/object
sector (§3) — the decisions made while landing them, the defects they
uncovered, and what remains. It complements
[m2c-semantics.md](../contracts/m2c-semantics.md), which stays the normative
contract for the stage; nothing here overrides it.

**Status:** **ALL LANDED — suite 291/291 green** (gate log
`/tmp/opencode/suite-2c-final1.log`), VSCode smoke 754/754. Sector A
(GC) + Sector B (exceptions) + Sector C (classes) + formal tests +
superseded-2B-expectation updates (`rt_meth_bare_ref`,
`rt_meth_named_arg` per §3.2) + the six semantic probes landed as
`err_cls_*` cases + VSCode 2C parity (grammar, snippets, completions,
indent rules, smoke extended with six 2C sections). Bench re-recorded:
`bench/run-m2c.sh` → `bench/results/m2c-baseline.md` +
`docs/performance/baseline-m2c.md` — the contract's RSS rows collapse (dispatch
344 MB → 12.4 MB, text_ops 89 → 12.4, while_string 44 → 12.4), collector
tax 1.10–1.85× (dispatch_ops **0.77× — faster**). SPEC/README retagged
`0.2.0-m2c`. Remaining: staged commits.

## 1. What the stage delivered

### 1.1 Sector A — non-moving mark & sweep GC (contract §1)

| Piece | Lowering / design |
|---|---|
| Heap blocks | registry block `pt_gcblk` immediately **before** the payload; `pt_obj` layout unchanged; `blk.h.kind` doubles as the value tag |
| New tags | `PT_GC_TEXT`/`PT_GC_BYTES` (100–102): text buffers registered in an interior-pointer **tset**, so interior slices stay valid across sweeps |
| Roots | `pt_init(..., roots, nroots)` — one pointer per program global (codegen `pt_g_roots[]`), plus shadow frames, bound vectors, protect stack, handler stack, builtin class singletons |
| Safe points | function entries, loop back-edges, statement boundaries; generated temps are cleared at statement end so dropped cycles become collectible |
| `collect()` builtin | forced full collection, returns the freed-block count |

### 1.2 Sector B — exceptions (contract §2)

| Piece | Design |
|---|---|
| Front-end | keywords `try except finally raise as`; `Try`/`Raise` AST; except+`as` binding; `finally` with break/continue/return exits |
| Unwinder | `setjmp` in generated C; `pt_handler {prev, env, frame_depth, prot_depth, call_depth, exc, kind}`; `pt_try_enter/leave`; `pt_throw` pops the top handler, restores depths, frees abandoned `pt_call` bound vectors (`pt_call_bounds`), `longjmp`s; empty stack → 2B-format print + `exit(1)` |
| Classes | 9 fixed names eagerly created in `pt_init` into `pt_exc_cls[]` (`is_exc = true`); `pt_exc_class(name)` lookup; `pt_exc_named(name)` (unknown → RuntimeError) |
| Matching | `pt_exc_matches` — **exact identity** of the class object (no inheritance in 2C) |
| Runtime errors | `pt_runtime_error` kept its signature but now throws via `pt_throw_message`; `pt_runtime_error_as(cls, line, fmt, …)` public — 66 internal sites reclassified by message substring (71 `_as` calls total) |
| Instances | `as.exc.{cls,msg,line}`; empty message displays the bare class name; `type(e)` = class name; equality/`is` = pointer identity; `pt_raise` validates (instance rethrow / bare exception class constructs / else TypeError) |
| Codegen try lowering | C scope per try with `pt_handler pt_h{c,f}<N>`; except+finally = nested inner CATCH + outer FINALLY; landing copies `h.exc` into a `decl_temp()` first; `try_stack_` cleared at emit boundaries; exit sequences emit nothing when the stack is empty (byte-compat) |

### 1.3 Sector C — classes, instances, `is` (contract §3)

| Piece | Design |
|---|---|
| Front-end | keywords `class is`; block-form `class Name:`; `ExprKind::Attr` (bare `object.name`); `AssignTarget::Kind::Attr`; `is`/`is not` at `==` precedence/chaining; `FuncDecl.is_method` |
| Semantic | classes are **ordinary guarded globals** (top level or main block only — `classes must be declared at the top level`); body must hold function declarations or `pass` (`only function declarations are allowed in a class body`); methods registered in `prog_.functions`/`global_funcs_` with `c_name = pt_fn_<Class>_<method>` but **not** in `funcs_` (bare method name = undefined); duplicate class/method checks; explicit `self` (no injection) |
| Runtime values | `PT_INSTANCE` {class, dynamic fields}, `PT_BOUND_METHOD` {instance, fn}; `pt_class_new` / `pt_class_add_method` / `pt_getattr` / `pt_setattr` / `pt_identical` |
| Attribute reads | instance fields → (exceptions) `.type`/`.message` → methods (yield a bound method); else `AttributeError: '<owner>' object has no attribute '<name>'` |
| Construction | `Point(1,2)` allocates the instance, looks up `__init__`, assembles `[self, …pos, …named]` (bounds-tracked + instance GC-protected across the call), discards the return; missing `__init__` with arguments → `class '%s' takes arguments but has no __init__` |
| Bound methods | re-dispatch through `pt_call` with `self` prepended — **named arguments/kwargs work on methods**; builtin container methods keep the positional-only runtime check |
| Displays | `<class Point>`, `<Point instance>`, `<method 'move' of Point>` (meta keeps the source name) — all deterministic, no addresses |
| `is` / `is not` | `pt_identical(a,b,line)` — heap objects by pointer, text/bytes by shared storage (`p+len`), non-heap by tag+value; never constant-folded; missing operands raise the usual guards |
| Codegen | `StmtKind::Class` emits `pt_g_<C> = pt_class_new(…)` then `pt_class_add_method` per method (metas + closures already emitted by the function machinery); `Attr` → `pt_getattr`; attr target → `pt_setattr` (compound reads the field first, object evaluated once); `is`/`is not` → `pt_bool(pt_identical(…))` via `binary_text`; `find_function` skips `is_method` |

## 2. Decisions made during integration (integrator ratifications)

1. **Exception-class message reclassification is substring-based, not
   call-site-based.** Each of the 66 `pt_runtime_error` sites was mapped
   to a class by its message ("argument name", "unhashable", "out of
   range", …). The uncaught printer still emits the exact 2B byte format
   `<src>:<line>: runtime error: <msg>` + `exit(1)` — no class prefix —
   so every `rt_*` expectation stays byte-identical.
2. **`raise` accepts only builtin exception classes/instances (§3.3).**
   `raise <user class>` → `TypeError: exceptions must be class or
   instance, got class` (`<t>` = `type()` spelling); `raise <instance>` →
   `… got <Class>`. The message template is the ratified §2.3 wording
   applied to both.
3. **Class names are ordinary globals — shadowing is allowed.**
   `class ValueError:` compiles; reads then see the user class (the
   exception-slot binding at C-main start is overwritten by the class
   statement, exactly like any global). The shadow is still not
   raisable. Contract is silent; Python-compat chosen (documented by
   `cls_shadow`).
4. **2B §12's static "method arguments must be positional" rule is
   lifted** — §3.2 requires kwargs on methods and `obj.m(...)` is the
   only method-call form, so the check moved to runtime (builtins keep
   the identical wording). `rt_meth_named_arg` now expects the runtime
   format: `:2: runtime error: method arguments must be positional`.
5. **Bare `obj.name` is legal (§3.2), superseding the 2B parser error.**
   `y = xs.append` resolves to an attribute read; lists have no
   attributes in 2C, so it fails at runtime with
   `:2: runtime error: 'list' object has no attribute 'append'`.
   `rt_meth_bare_ref` expectations updated; the parser's
   "methods must be called" message is gone (kept error: `Dot` +
   non-identifier).
6. **`pt_identical` uses `p+len` equality for text identity.** Same
   pointer + same length = same storage; equal content from distinct
   storage (e.g. two `"ab"+"cd"` results) is not identical. Static
   literals may be C-merged — that is an implementation detail of
   sharing, not a language rule.
7. **`try_inline` falls back for `Attr` and for named-argument method
   calls** (both need statement sequencing); the positional-only
   method-call form remains byte-identical to 2B.
8. **GC-safety of construction/dispatch:** the instance in
   `pt_class_call` is `pt_gc_protect`ed across `__init__` (the protect
   depth is restored by `pt_throw` on unwind); the self+args vectors are
   pushed on `pt_call_bounds` so a throw inside `__init__`/a method
   cannot leak them.

## 3. Defects uncovered during integration

1. **Sweep layout bug (self-found):** instance/class tables are *not* at
   union offset 0 (`cls`/`name` precede them), so the early sweep that
   freed `as.table.e` for `PT_INSTANCE`/`PT_CLASS` would have freed
   garbage — corrected to free `as.inst.attrs` / `as.cls.methods`
   explicitly (singletons skipped via `is_exc`).
2. **`pt_type_name` before `struct pt_obj`:** the `PT_INSTANCE` case
   dereferences the object, so the function moved below the complete
   struct definition (declared in the header already).
3. **Display off-by-one:** `"<method '"` is 9 bytes, emitted with length
   10 — slurped the NUL into the buffer and truncated every bound-method
   display; caught by the direct runtime smoke harness
   (`rt_class_smoke.c`) before codegen existed.
4. **Named-value truncation in `self`-prepending:** `pt_class_call` /
   `pt_bound_call` allocated/copied `npos+1` entries but `posvals`
   carries `npos+nnamed` — named arguments were dropped and the binder
   read uninitialized memory (`local variable 'k' used before
   assignment`). Fixed to copy the whole vector; found by the first
   `cls_basic` run.
5. **Front-end/codegen exhaustiveness:** `emit_expr`, `try_inline`
   (Attr) and `emit_destructure` (Attr target) needed explicit cases
   (`-Wswitch`); `has_effect` must treat `Attr` as effectful (it can
   raise) and `collect_var_reads` must walk its object.

## 4. Test inventory

New cases (all in `tests/cases/`):

- GC: `gc_collect`, `gc_determinism`, `gc_cycle` (mutual `a`↔`b` lists,
  self-map, closure↔cell↔list cycles + `collect()` counts), `gc_frames`
  (rooted frames survive), `gc_closures` (cell/closure interplay).
- Exceptions: `exc_basic`, `exc_finally` (break/continue/return crossing
  `finally`), `exc_rterr` (runtime-error catchability), `exc_nested`
  (nested unwinding), `err_uncaught_raise` (exit 1, 2B byte format).
- Classes: `cls_basic` (init/methods/kwargs/attrs/displays),
  `cls_identity` (`is`/`is not` over every value class),
  `cls_errors` (caught AttributeError/TypeError flows incl. §3.3),
  `cls_gc` (instances/bound methods survive collection),
  `cls_shadow` (reserved-name shadowing),
  `err_cls_missing_attr`, `err_cls_raise_instance` (exit 1).
- Class diagnostics (probes promoted to permanent cases):
  `err_cls_bare_method` (`undefined function 'move'`),
  `err_cls_body` (`only function declarations are allowed in a class
  body`), `err_cls_dup_method`, `err_cls_dup_class`,
  `err_cls_in_function` (`classes must be declared at the top level`),
  `err_cls_earlyuse` (runtime `global variable 'Point' used before
  assignment`).

Updated expectations (contract-superseded, see §2.4/§2.5):
`rt_meth_bare_ref.err`, `rt_meth_named_arg.err`.

## 5. Remaining / follow-ups

- **Done:** bench re-record (`bench/run-m2c.sh` →
  `bench/results/m2c-baseline.md`; write-up `docs/performance/baseline-m2c.md`) —
  RSS rows collapse toward the 12 MB floor (dispatch 344 MB → 12.4 MB,
  text_ops 89 → 12.4, while_string 44 → 12.4; coll_mixed 166 → 30 MB
  bounded); timings re-measured (1.10–1.85× collector tax,
  dispatch_ops 0.77× — faster under a stable working set).
- **Done:** SPEC/README retag to `0.2.0-m2c` + 2C sections (keywords,
  grammar, §5.12/§5.13/§6.10/§7.3, §9 restructure, §11 architecture,
  §12/§13); stale rows removed (`bare obj.name` parser error in SPEC §9;
  2B semantics §12 table annotated as superseded).
- **Done:** VSCode 2C lane — grammar (`class`/`try` family/`is`/`as`,
  class-definition, bare-attribute, `collect`), snippets (`class`,
  `init`, `try`, `raise`), completions + docs, indent rules,
  `test/smoke.js` extended (754 checks).
- Remaining: staged commits (code / tests / VSCode / chore+`.gitignore` /
  docs).
- Known limitations unchanged: deep recursion C-stack overflow, Linux
  x86_64 only, dynamic-dispatch tax on scalar loops.
