# Code generation

What `src/codegen.cpp` emits, how to read it, and how to look at it. The
normative contract is SPEC §11.3 (and the stage contracts it cites); this page
adds a worked example and the naming conventions.

## A complete worked example

Source:

```python
total = 0
n(square):
    return n * n

main:
    for i in range(1, 4):
        total += square(i)
    names = ["a", "b"]
    print("total:", total, names)
```

Running it prints `total: 14 [a, b]`. Below is the real output of the `0.2.1`
generator, **abridged for readability** — repeated temporaries are elided with
`/* … */` and the overflow-safe loop-step expression is shown in prose — with
explanatory comments added:

```c
#include "cstar_rt.h"

static cs_value cs_g_total;                 /* one static per global; zero-init == unset */
static cs_value cs_g_i;                     /* the for-loop target is a global here */
static cs_value cs_g_names;
static int64_t cs_ng_i = 0; static char cs_ng_ok_i = 0;   /* native-int lane for `i` */

static cs_value cs_fn_square(cs_value* bound, cs_cell** env) CS_UNUSED;
static const char* const cs_mpn_cs_fn_square[] = { "n" };
static const cs_funmeta cs_meta_cs_fn_square CS_UNUSED =
    {"square", cs_mpn_cs_fn_square, 1u, 1u, 0ull, false, false, cs_fn_square};

static cs_value cs_fn_square(cs_value* bound, cs_cell** env) {
  cs_value cs_l_n = cs_unset();             /* parameter materialised from bound[] */
  cs_value cs_t0 = cs_unset(); /* … cs_t1, cs_t2: GC-visible temporaries */
  cs_value* cs_slots[] = { &cs_l_n, &cs_t0, &cs_t1, &cs_t2 };
  cs_frame cs_fr;
  cs_frame_begin(&cs_fr, cs_slots, 4u);     /* shadow-stack frame: GC roots */
  cs_gc_protect(bound, 1u);
  cs_gc_maybe();                            /* safe point at function entry */
  cs_l_n = bound[0];
  cs_gc_unprotect(1u);
  cs_t0 = cs_guard(cs_l_n, "n", CS_SCOPE_LOCAL, 3);   /* used-before-assignment check */
  cs_t1 = cs_guard(cs_l_n, "n", CS_SCOPE_LOCAL, 3);
  cs_t2 = cs_mul_f(cs_t0, cs_t1, 3);        /* inline numeric fast path; 3 = source line */
  cs_frame_end(&cs_fr);
  return cs_t2;
}

static void cs_main_block(void) {
  /* … temporaries and a frame, as above … */
  cs_t3 = cs_range3(cs_int(1LL), cs_int(4LL), cs_int(1), 6);
  int64_t cs_iv0 = cs_t3.as.r.start;       /* `for … in range(…)` is open-coded */
  for (; (step > 0) ? (cs_iv0 < stop) : (cs_iv0 > stop); cs_iv0 = /* overflow-safe step */) {
    cs_ng_i = cs_iv0; cs_ng_ok_i = 1;      /* bind the loop variable natively */
    cs_t7 = cs_guard(cs_g_total, "total", CS_SCOPE_GLOBAL, 7);
    { cs_value cs_t9[] = { cs_int(cs_nread_g(cs_ng_i, cs_ng_ok_i, 7, "i")) };
      cs_t8 = cs_fn_square(cs_t9, NULL); }   /* direct call: callee known statically */
    cs_t10 = cs_add_f(cs_t7, cs_t8, 7);
    cs_g_total = cs_t10;
    cs_t7 = cs_unset(); cs_t8 = cs_unset(); cs_t10 = cs_unset();   /* clear temps so the GC can reclaim */
    cs_gc_maybe();                          /* loop back-edge safe point */
  }
  cs_t11 = cs_list_of((cs_value[]){ CS_STR("a"), CS_STR("b") }, 2u);
  cs_g_names = cs_t11;
  /* … */
  { cs_value cs_args[] = { CS_STR("total:"), cs_t12, cs_t13 }; cs_print(cs_args, 3, 9); }
}

static cs_value* const cs_g_roots[] = { &cs_g_total, &cs_g_i, &cs_g_names };

int main(int argc, char** argv) {
  cs_init(argc, argv, "demo.cst", cs_g_roots, 3u);   /* path, args, GC root vector */
  cs_frame cs_fr; cs_frame_begin(&cs_fr, NULL, 0u);
  cs_gc_maybe();
  cs_g_total = cs_int(0LL);                          /* top-level statements, in order */
  cs_gc_maybe();
  cs_main_block();                                   /* `main:` runs in place */
  cs_frame_end(&cs_fr);
  return 0;
}
```

What to notice:

* **One static per global**, zero-initialised — and zero *is* the `unset` tag, so
  "used before assignment" falls out for free.
* **Every function has the uniform ABI** `cs_value f(cs_value* bound, cs_cell** env)`
  (SPEC §11.3). A call site with a known callee fills a stack `bound[]` and calls
  directly; the callee's entry code materialises defaults and the empty
  vararg/kwarg containers. A `cs_funmeta` describes name, parameters and
  defaults for dynamic calls and error messages.
* **Every fallible operation takes the source line** as its last argument —
  that is where `file:LINE: runtime error:` comes from.
* **Temporaries are explicit and rooted.** Each function registers all its local
  and temporary slots in a `cs_frame` so the collector can find live values, and
  clears temporaries at statement boundaries so dead values are collectible.
* **Safe points** (`cs_gc_maybe()`) sit at function entries, loop back-edges and
  top-level statement boundaries — and are **omitted** where a loop body is
  proven allocation-free.
* **`for … in range(…)` becomes a C `for`** with saturating step arithmetic
  (no signed-overflow UB), and `i`, a proven integer, is stored in a native
  slot (`cs_ng_i`) — the "native-int lane".
* **Int-lane-viable functions get a native twin.** Beside the uniform ABI, a
  function whose every binding is a proven integer (params guaranteed int by
  the call site, locals by the lane analysis, all returns int-shaped, definite
  return) is also emitted as `static int64_t cs_fn_<name>_i(int64_t …)` — no
  bound vector, no local `cs_value` slots, no boxing; the body compiles to
  plain C int64 arithmetic. Static call sites whose arguments are all
  int-shaped call the twin; every other call site keeps the dynamic entry
  point, so float/string behavior is bit-identical (the call-site gate is the
  whole type contract). Mutual recursion closes through a monotone fixpoint.
* **`main:` is just `cs_main_block()`**, called at its textual position among
  the top-level statements.

## Naming conventions in generated C

| Prefix | Meaning |
|---|---|
| `cs_g_<name>` | a global (module-level) variable; also listed in `cs_g_roots` |
| `cs_l_<name>` | a local or parameter |
| `cs_t<N>` | a compiler temporary (registered in the frame) |
| `cs_fn_<name>` | a function (`cs_fn_` is also the `c_name` set by the semantic pass) |
| `cs_meta_…`, `cs_mpn_…` | the function's `cs_funmeta` and its parameter-name array |
| `cs_ng_<name>`, `cs_ng_ok_<name>` | native-int global slot + its "assigned" flag |
| `cs_fn_<name>_i` | the native int-lane twin of `cs_fn_<name>` (int64 params → int64 result) |
| `cs_iv<N>` | the induction variable of an open-coded `range()` loop |
| `cs_cell_<name>` | a captured (boxed) local's heap cell |
| `cs_p_<fn>_<name>` / `cs_px_<fn>_<name>` | program-lifetime storage for a binding pinned by `variable.` (plain / immortal-cell shape) |
| `cs_main_block` | the lowered `main:` body |

### Imported declarations

The generator never sees an `import`: the linker ([modules](../guide/modules.md))
has already flattened the module graph and renamed every imported declaration to
an internal identifier of the form `__csm<id>_<name>`. Those names flow through
the ordinary manglers, so an imported function becomes `cs_fn___csm1_greet`,
an imported class keeps its `__csm1_` prefix in `cs_class_new`, and an imported
global becomes `cs_g___csm1_PREFIX`. The prefix keeps two modules that both
define `greet` distinct. Imported module top-level statements are emitted at
their import position (module initialization); an imported module's `main:`
block is dropped. Unused imported declarations pruned by `run`/`build` are
absent from the AST, so no C is emitted for them at all. In `check` mode nothing
is pruned, but code generation does not run.

## How constructs lower

| Source | Lowered form |
|---|---|
| `a + b`, `a < b`, … | `cs_add_f` / `cs_lt_f` … inline fast path with out-of-line fallback; native-int operands use plain C arithmetic with cstar's wrap/sign rules |
| `x` (global/local read) | `cs_guard(slot, "name", scope, line)` unless definite assignment proves the guard unnecessary |
| `f(args)` to a known function | stack `bound[]` + direct `cs_fn_f(bound, env)`; arity/named checks already done at compile time |
| `f(args)` to an int-lane-viable function over int-shaped positional args | `cs_int(cs_fn_f_i(args…))` — the call runs in the callee's int64 lane; anything else (floats, named args, defaults) keeps the dynamic form |
| `f(args)` otherwise | `cs_call(fnv, posvals, npos, argnames, nnamed, line)` — binds at run time |
| `obj.m(args)` | `cs_call_method(obj, "m", args, nargs, argnames, nnamed, line)` |
| `obj.attr` / `obj.attr = v` | `cs_getattr` / `cs_setattr` |
| `a is b` | `cs_bool(cs_identical(a, b, line))` |
| `[..]`, `{..}`, `(..)` | `cs_list_of`, map/set/tuple constructors from compound-literal arrays |
| `print(…)` | a local array + `cs_print(arr, n, line)` |
| Python-parity builtin | one direct `cs_builtin_<name>(…)` call; unused builtins emit nothing |
| `fvec(…)`, `ivec(…)`, `fvec_fill(…)`, `ivec_fill(…)`, `dot(a, b)` | one direct `cs_builtin_*` call (the original-15 lowering, arity already checked) |
| `x = a * b + c` where `x`/`a`/`b`/`c` are slots the fvec analysis proved `fvec`/`ivec`-only | a **fused kernel block**: each operand read once into a temp, one `cs_fvec_new`/`cs_ivec_new`, a lane loop (`CS_VW` vector loads + store) followed by a scalar tail, then one slot store — no intermediate vectors (`analyze_native_fvecs`, [performance §12](../performance/optimization.md)) |
| `if`/`while`/`switch` | C control flow; `switch` is a first-match chain; conditions use total `cs_truthy` |
| `for x in range(…)` | open-coded C `for` (above) |
| `for x in other` | the runtime iteration protocol |
| `try`/`except`/`finally` | a C scope holding a `cs_handler` and `setjmp` landing pad; `finally` bodies are duplicated onto each exit edge |
| `class C: …` | `cs_class_new(name, line)` bound to the global, then `cs_class_add_method(...)` per method |
| nested `name(params):` declaration | a C function taking the closure `env`; captured locals live in `cs_cell`s; closures are built in the enclosing function's prologue |
| `variable.<q> x` | direct read/write of the pinned static (or the module global) |

## Inspecting generated C

The driver deletes `gen.c` after linking (a "zero junk" property), so to read the
output of a particular program either build with a `clang` shim, or watch the
scratch directory:

```bash
mkdir -p /tmp/shim /tmp/cap
cat > /tmp/shim/clang <<'EOF'
#!/bin/sh
for a in "$@"; do case "$a" in */gen.c) cp "$a" /tmp/cap/gen.c ;; esac; done
exec /usr/bin/clang "$@"
EOF
chmod +x /tmp/shim/clang
PATH=/tmp/shim:$PATH build/cstar build prog.cst -o /tmp/prog && less /tmp/cap/gen.c
```

(Adjust `/usr/bin/clang` to your `clang`.) The generated file compiles with empty
stderr — if you ever see a clang warning from `gen.c`, that is a bug.

## Rules the generator must keep

These are the invariants that make the output safe; they are why changes to
`codegen.cpp` are validated against the full suite and byte-identical A/B runs
([performance §9](../performance/optimization.md)):

1. Every function forward declaration precedes any definition (forward calls and
   mutual recursion).
2. Every runtime-failing operation receives the source line.
3. Reads of globals/locals are guarded unless definite assignment proves
   otherwise.
4. Generated temporaries are cleared at statement boundaries; every live value is
   in a registered frame slot, a registered global root, or the protect stack at
   every safe point.
5. `return` paths pop the frame; `try` lowering keeps frame/protect depths
   restorable.
6. Output must match C semantics *and* cstar semantics for integer edges
   (wrap, division/modulo signs, `MIN / -1`).
