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
#include "pentest_rt.h"

static pt_value pt_g_total;                 /* one static per global; zero-init == unset */
static pt_value pt_g_i;                     /* the for-loop target is a global here */
static pt_value pt_g_names;
static int64_t pt_ng_i = 0; static char pt_ng_ok_i = 0;   /* native-int lane for `i` */

static pt_value pt_fn_square(pt_value* bound, pt_cell** env) PT_UNUSED;
static const char* const pt_mpn_pt_fn_square[] = { "n" };
static const pt_funmeta pt_meta_pt_fn_square PT_UNUSED =
    {"square", pt_mpn_pt_fn_square, 1u, 1u, 0ull, false, false, pt_fn_square};

static pt_value pt_fn_square(pt_value* bound, pt_cell** env) {
  pt_value pt_l_n = pt_unset();             /* parameter materialised from bound[] */
  pt_value pt_t0 = pt_unset(); /* … pt_t1, pt_t2: GC-visible temporaries */
  pt_value* pt_slots[] = { &pt_l_n, &pt_t0, &pt_t1, &pt_t2 };
  pt_frame pt_fr;
  pt_frame_begin(&pt_fr, pt_slots, 4u);     /* shadow-stack frame: GC roots */
  pt_gc_protect(bound, 1u);
  pt_gc_maybe();                            /* safe point at function entry */
  pt_l_n = bound[0];
  pt_gc_unprotect(1u);
  pt_t0 = pt_guard(pt_l_n, "n", PT_SCOPE_LOCAL, 3);   /* used-before-assignment check */
  pt_t1 = pt_guard(pt_l_n, "n", PT_SCOPE_LOCAL, 3);
  pt_t2 = pt_mul_f(pt_t0, pt_t1, 3);        /* inline numeric fast path; 3 = source line */
  pt_frame_end(&pt_fr);
  return pt_t2;
}

static void pt_main_block(void) {
  /* … temporaries and a frame, as above … */
  pt_t3 = pt_range3(pt_int(1LL), pt_int(4LL), pt_int(1), 6);
  int64_t pt_iv0 = pt_t3.as.r.start;       /* `for … in range(…)` is open-coded */
  for (; (step > 0) ? (pt_iv0 < stop) : (pt_iv0 > stop); pt_iv0 = /* overflow-safe step */) {
    pt_ng_i = pt_iv0; pt_ng_ok_i = 1;      /* bind the loop variable natively */
    pt_t7 = pt_guard(pt_g_total, "total", PT_SCOPE_GLOBAL, 7);
    { pt_value pt_t9[] = { pt_int(pt_nread_g(pt_ng_i, pt_ng_ok_i, 7, "i")) };
      pt_t8 = pt_fn_square(pt_t9, NULL); }   /* direct call: callee known statically */
    pt_t10 = pt_add_f(pt_t7, pt_t8, 7);
    pt_g_total = pt_t10;
    pt_t7 = pt_unset(); pt_t8 = pt_unset(); pt_t10 = pt_unset();   /* clear temps so the GC can reclaim */
    pt_gc_maybe();                          /* loop back-edge safe point */
  }
  pt_t11 = pt_list_of((pt_value[]){ PT_STR("a"), PT_STR("b") }, 2u);
  pt_g_names = pt_t11;
  /* … */
  { pt_value pt_args[] = { PT_STR("total:"), pt_t12, pt_t13 }; pt_print(pt_args, 3, 9); }
}

static pt_value* const pt_g_roots[] = { &pt_g_total, &pt_g_i, &pt_g_names };

int main(int argc, char** argv) {
  pt_init(argc, argv, "demo.pen", pt_g_roots, 3u);   /* path, args, GC root vector */
  pt_frame pt_fr; pt_frame_begin(&pt_fr, NULL, 0u);
  pt_gc_maybe();
  pt_g_total = pt_int(0LL);                          /* top-level statements, in order */
  pt_gc_maybe();
  pt_main_block();                                   /* `main:` runs in place */
  pt_frame_end(&pt_fr);
  return 0;
}
```

What to notice:

* **One static per global**, zero-initialised — and zero *is* the `unset` tag, so
  "used before assignment" falls out for free.
* **Every function has the uniform ABI** `pt_value f(pt_value* bound, pt_cell** env)`
  (SPEC §11.3). A call site with a known callee fills a stack `bound[]` and calls
  directly; the callee's entry code materialises defaults and the empty
  vararg/kwarg containers. A `pt_funmeta` describes name, parameters and
  defaults for dynamic calls and error messages.
* **Every fallible operation takes the source line** as its last argument —
  that is where `file:LINE: runtime error:` comes from.
* **Temporaries are explicit and rooted.** Each function registers all its local
  and temporary slots in a `pt_frame` so the collector can find live values, and
  clears temporaries at statement boundaries so dead values are collectible.
* **Safe points** (`pt_gc_maybe()`) sit at function entries, loop back-edges and
  top-level statement boundaries — and are **omitted** where a loop body is
  proven allocation-free.
* **`for … in range(…)` becomes a C `for`** with saturating step arithmetic
  (no signed-overflow UB), and `i`, a proven integer, is stored in a native
  slot (`pt_ng_i`) — the "native-int lane".
* **Int-lane-viable functions get a native twin.** Beside the uniform ABI, a
  function whose every binding is a proven integer (params guaranteed int by
  the call site, locals by the lane analysis, all returns int-shaped, definite
  return) is also emitted as `static int64_t pt_fn_<name>_i(int64_t …)` — no
  bound vector, no local `pt_value` slots, no boxing; the body compiles to
  plain C int64 arithmetic. Static call sites whose arguments are all
  int-shaped call the twin; every other call site keeps the dynamic entry
  point, so float/string behavior is bit-identical (the call-site gate is the
  whole type contract). Mutual recursion closes through a monotone fixpoint.
* **`main:` is just `pt_main_block()`**, called at its textual position among
  the top-level statements.

## Naming conventions in generated C

| Prefix | Meaning |
|---|---|
| `pt_g_<name>` | a global (module-level) variable; also listed in `pt_g_roots` |
| `pt_l_<name>` | a local or parameter |
| `pt_t<N>` | a compiler temporary (registered in the frame) |
| `pt_fn_<name>` | a function (`pt_fn_` is also the `c_name` set by the semantic pass) |
| `pt_meta_…`, `pt_mpn_…` | the function's `pt_funmeta` and its parameter-name array |
| `pt_ng_<name>`, `pt_ng_ok_<name>` | native-int global slot + its "assigned" flag |
| `pt_fn_<name>_i` | the native int-lane twin of `pt_fn_<name>` (int64 params → int64 result) |
| `pt_iv<N>` | the induction variable of an open-coded `range()` loop |
| `pt_cell_<name>` | a captured (boxed) local's heap cell |
| `pt_p_<fn>_<name>` / `pt_px_<fn>_<name>` | program-lifetime storage for a binding pinned by `variable.` (plain / immortal-cell shape) |
| `pt_main_block` | the lowered `main:` body |

### Imported declarations

The generator never sees an `import`: the linker ([modules](../guide/modules.md))
has already flattened the module graph and renamed every imported declaration to
an internal identifier of the form `__ptm<id>_<name>`. Those names flow through
the ordinary manglers, so an imported function becomes `pt_fn___ptm1_greet`,
an imported class keeps its `__ptm1_` prefix in `pt_class_new`, and an imported
global becomes `pt_g___ptm1_PREFIX`. The prefix keeps two modules that both
define `greet` distinct. Imported module top-level statements are emitted at
their import position (module initialization); an imported module's `main:`
block is dropped. Unused imported declarations pruned by `run`/`build` are
absent from the AST, so no C is emitted for them at all. In `check` mode nothing
is pruned, but code generation does not run.

## How constructs lower

| Source | Lowered form |
|---|---|
| `a + b`, `a < b`, … | `pt_add_f` / `pt_lt_f` … inline fast path with out-of-line fallback; native-int operands use plain C arithmetic with Pentest's wrap/sign rules |
| `x` (global/local read) | `pt_guard(slot, "name", scope, line)` unless definite assignment proves the guard unnecessary |
| `f(args)` to a known function | stack `bound[]` + direct `pt_fn_f(bound, env)`; arity/named checks already done at compile time |
| `f(args)` to an int-lane-viable function over int-shaped positional args | `pt_int(pt_fn_f_i(args…))` — the call runs in the callee's int64 lane; anything else (floats, named args, defaults) keeps the dynamic form |
| `f(args)` otherwise | `pt_call(fnv, posvals, npos, argnames, nnamed, line)` — binds at run time |
| `obj.m(args)` | `pt_call_method(obj, "m", args, nargs, argnames, nnamed, line)` |
| `obj.attr` / `obj.attr = v` | `pt_getattr` / `pt_setattr` |
| `a is b` | `pt_bool(pt_identical(a, b, line))` |
| `[..]`, `{..}`, `(..)` | `pt_list_of`, map/set/tuple constructors from compound-literal arrays |
| `print(…)` | a local array + `pt_print(arr, n, line)` |
| Python-parity builtin | one direct `pt_builtin_<name>(…)` call; unused builtins emit nothing |
| `fvec(…)`, `ivec(…)`, `fvec_fill(…)`, `ivec_fill(…)`, `dot(a, b)` | one direct `pt_builtin_*` call (the original-15 lowering, arity already checked) |
| `x = a * b + c` where `x`/`a`/`b`/`c` are slots the fvec analysis proved `fvec`/`ivec`-only | a **fused kernel block**: each operand read once into a temp, one `pt_fvec_new`/`pt_ivec_new`, a lane loop (`PT_VW` vector loads + store) followed by a scalar tail, then one slot store — no intermediate vectors (`analyze_native_fvecs`, [performance §12](../performance/optimization.md)) |
| `if`/`while`/`switch` | C control flow; `switch` is a first-match chain; conditions use total `pt_truthy` |
| `for x in range(…)` | open-coded C `for` (above) |
| `for x in other` | the runtime iteration protocol |
| `try`/`except`/`finally` | a C scope holding a `pt_handler` and `setjmp` landing pad; `finally` bodies are duplicated onto each exit edge |
| `class C: …` | `pt_class_new(name, line)` bound to the global, then `pt_class_add_method(...)` per method |
| nested `name(params):` declaration | a C function taking the closure `env`; captured locals live in `pt_cell`s; closures are built in the enclosing function's prologue |
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
PATH=/tmp/shim:$PATH build/pentest build prog.pen -o /tmp/prog && less /tmp/cap/gen.c
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
6. Output must match C semantics *and* Pentest semantics for integer edges
   (wrap, division/modulo signs, `MIN / -1`).
