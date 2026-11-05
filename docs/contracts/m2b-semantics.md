# Pentest M2 Stage 2B — Normative Semantics and Decisions

> Part of the [stage contracts](README.md) (normative; they govern over
> [`SPEC.md`](../../SPEC.md) if the two differ).

Authoritative contract for Milestone 2 Stage 2B (collections, Text/Bytes,
slicing, destructuring, comma-join, methods, builtins, and the full function
overhaul: defaults, named/variadic args, function values, closures, global/
nonlocal). All components conform; error wording is normative (tests match on
substrings). M1 and M2-2A remain normative where this document does not
override them; the full existing suite must stay green except where this
document lists an explicit, approved behavior change.

## 0. Stage 2B decisions (user-approved)

| Question | Decision |
|---|---|
| Text vs Bytes | **Distinct types.** `"..."` literals are **Text** (validated UTF-8; indexing/iteration yields whole code points, never splits a sequence). New `b"..."` literals are **Bytes** (indexing/iteration yields int bytes). `encode(text)->bytes`, `decode(bytes)->text` builtins. |
| Collection literals | Python-style: `[1,2]` list; `{"k": v}` map; `{1,2,3}` set; `()` empty tuple; `(a,)` 1-tuple; `{}` empty map; `set()` empty-set constructor. |
| Comma-join scope | **Every unparenthesized comma** that is parsed as a full `expr` (assignment RHS, `return` value, expression statement) joins with a single space into Text. Tuples require parentheses. Call args and collection elements are NOT joins (they parse at `ternary` level). |
| Destructuring rest | Yes: `first, *rest = xs`; also flat `a, b = xs` and nested `(a,b), c = xs`. |
| Function features | Full set: default params (evaluated fresh **each call**), named args, `*args`, `**kwargs`, function values/callbacks, nested functions + closures, `global`/`nonlocal`. |
| Collection mutation UX | **Python-style methods** on builtin collections only (`xs.append(1)`, `m.get(k)`, `s.add(v)`); plus `len/str/int/float/bool/type` builtins. Map iteration = **keys**. User classes/methods land in 2C. |

Approved behavior changes vs earlier milestones (deliberate, tested):
1. Type display names change: the Text type is displayed **`text`** (was `string`); Bytes is **`bytes`**. Runtime error messages and `type()` use these. M1 tests asserting `string` are updated to `text`.
2. String literals become **Text**: indexing/iteration over a Text yields **1-code-point Text** (for ASCII this is identical to 2A's 1-byte behavior). Interior NUL is legal (U+0000).
3. `nested function declarations are not allowed` is **removed** — nested `function` inside a function body (and inside `main():`) is now a legal closure.
4. `print`/`range` remain **call-target-only** (cannot be read as values) — unchanged from M1/2A.

## 1. Lexical additions

New tokens:
- `[` `]` Brackets; `{` `}` Brace; `.` Dot.
- Bytes literal: the lexer treats `b` **immediately** followed by `"` (no space) as the start of a Bytes literal (like a string, same escapes **plus `\xHH`**). `b "x"` (with space) is not a bytes literal. Bytes literals are single-line, double-quoted; unterminated → `unterminated bytes literal`.
- Text literals keep the M1 escape set `\n \t \r \\ \" \0` (unknown escape → `unknown escape sequence '\x'`).
- `*` (Star) is reused as the rest marker in a destructuring target and as the `*name` parameter marker; context disambiguates.

No new keywords. `global` and `nonlocal` are **new keywords** (reserved).
`set` is an ordinary IDENT (a builtin constructor call, like `range`).

## 2. Type system (normative)

Tags: `unset`, `None`, `int`, `float`, `bool`, `text`, `bytes`, `range`,
`list`, `map`, `set`, `tuple`, `function`.

- **Value categories.** `int/float/bool/None` are immediate. `text`/`bytes`
  are immutable (pointer+length; `text` stores UTF-8, may contain NUL).
  `list/map/set/tuple/function/range` are **heap objects** (referenced by
  pointer; **2B leaks them — no reclamation**; see §15 note).
- **Display / `str()` forms** (single normative table, used by `print`,
  `str()`, and comma-join):
  - `text` → itself; `int` → decimal; `float` → M1 shortest-round-trip;
    `bool` → `True`/`False`; `None` → `None`.
  - `bytes` → `b"..."` where printable ASCII (0x20–0x7E except `"` and `\`)
    are literal, everything else `\xHH`, `"` and `\` escaped.
  - `list` → `[e1, e2]` (space after comma); `tuple` → `(e1, e2)`; 1-tuple →
    `(e,)`; empty tuple → `()`; `set` → `{e1, e2}` (insertion order); `map` →
    `{k1: v1, k2: v2}`; `range` → `range(s, t, p)` (2A).
  - `function` → `<function name>`.
  - Nesting recurses. Cycles are not detected (leaked/undefined; 2C GC note).
- **Reference semantics (§7.5 ratified):** assigning a `list/map/set` to
  another variable copies the **reference**; mutating through one is visible
  through the other. `tuple/text/bytes` are immutable (value-like).
  `function` values are references. Aliasing is tested.

## 3. Grammar (EBNF, normative additions)

```ebnf
stmt        := compound_stmt | simple_stmt NL
simple_stmt := target_list augassign expr            (* incl. destructuring *)
             | "return" [ expr ]
             | "break" | "continue" | "pass"
             | "global" NAME { "," NAME }
             | "nonlocal" NAME { "," NAME }
             | expr                                   (* expr-stmt; may be a Join *)

target_list := target { "," target }                  (* >1 => Tuple pattern *)
target      := NAME [ "[" subscript "]" ]             (* Name, or Name[index] *)
             | "*" NAME                               (* rest, inside a Tuple pattern *)
             | "(" target_list [ "," ] ")"          (* nested pattern; a single
                                                        target needs the trailing comma *)
subscript   := [ expr ] ":" [ expr ] [ ":" expr ]     (* slice — target only if index *)
             | expr                                   (* index *)

(* function declarations, incl. nested inside a function body or main(): *)
func_decl   := "function" NAME "(" [ params ] ")" ":" NL INDENT block DEDENT
params      := param { "," param } [ "," vararg ] [ "," kwarg ]
             | vararg [ "," kwarg ] | kwarg
param       := NAME [ "=" expr ]                      (* default *)
vararg      := "*" NAME                               (* collects extra positionals -> list *)
kwarg       := "**" NAME                              (* collects extra named -> map *)

primary     := INT | FLOAT | STRING | BSTRING | "True" | "False" | "None"
             | list_lit | set_or_map_lit | "(" [ tuple_or_group ] ")"
             | NAME
list_lit    := "[" [ ternary { "," ternary } ] "]"
set_or_map_lit := "{" "}"                              (* empty map *)
               | "{" ternary ":" ternary { "," ternary ":" ternary } "}"  (* map *)
               | "{" ternary { "," ternary } "}"        (* set *)
tuple_or_group := expr                                 (* (e) group *)
               | ternary "," [ ternary { "," ternary } ]  (* tuple / 1-tuple *)

postfix     := primary { "(" [ args ] ")"             (* call: bare, indirect, nested *)
                        | "[" subscript "]"           (* index / slice *)
                        | "." NAME "(" [ args ] ")" } (* method call ONLY (no bare attr) *)
args        := arg { "," arg }
arg         := [ NAME "=" ] ternary                   (* named arg when NAME "=" *)
```

Expression grammar from 2A is unchanged except: `expr := ternary { "," ternary }`
(the **join** level, lowest above nothing — only produced at assignment RHS,
`return`, and expression-statement positions; call args / literals parse
`ternary`). Postfix (call/index/slice/method) binds tightest. A `Join` node is
built when an `expr` production sees `>= 2` comma-separated `ternary`s.

Rules:
- **Assignment is still a statement.** The LHS is a `target_list`; with
  `>= 2` targets it is a destructuring (Tuple) pattern. `a, b = (1, 2)`.
  Because bare comma on the RHS is a **join**, to destructure a literal you
  must parenthesize/list it: `a, b = (1, 2)` or `a, b = [1, 2]` (NOT
  `a, b = 1, 2`, which joins to `"1 2"`).
- **Method calls** exist only as `obj.name(args)`; a bare `obj.name` (no call)
  is a **syntax error** (`methods must be called; method references are not
  supported yet`).
  *Superseded by M2-2C §3.2: a bare `obj.name` is now a legal attribute
  read (runtime `AttributeError` when absent), and method arguments may
  be named.*
- Indirect calls: the callee of `(...)` may be any expression (`f(x)`,
  `f(x)(y)`, `get_fn()(1)`).
- Index-assign `xs[i] = v` / `m[k] = v` and compound `xs[i] += v` are legal
  targets; the index expression is evaluated **once**.
- Nested `function` is legal inside a function body and inside `main():`.
- `global`/`nonlocal` list one or more names.

## 4. Collections (normative)

- **list** `[...]`: ordered, mutable, duplicates. `xs[i]`, `xs[i] = v`,
  `xs[a:b:c]`, iteration, membership.
- **map** `{...}`: **insertion-ordered** key/value. `m[k]` fetch (missing →
  runtime error `map key not found`), `m[k] = v` insert/replace, `m.pop(k)`,
  membership `k -> m` (key). Iteration yields **keys**.
- **set** `{...}` / `set()`: unique, **insertion-ordered**. `s.add(x)`,
  membership, iteration in insertion order.
- **tuple** `(...)`: ordered, **immutable** (no index-assign). `()` empty,
  `(x,)` 1-tuple. Elements may be mutable objects (shallow immutability).
- **Indexing** `xs[i]`: negative `i` counts from the end (Python). Out of
  range → runtime error `list index out of range` / `tuple index out of range`
  / `text index out of range` / `bytes index out of range`. Non-int index →
  `list indices must be integers, not <t>` style (`<type> indices must be
  integers, not <t>`). Map uses `m[k]` (any hashable key; missing → `map key
  not found`).
- **Slicing** `xs[a:b:c]` on **text/bytes/list/tuple only** (map/set →
  runtime error `'<t>' object does not support slicing`). Python semantics:
  negative bounds, open bounds (`[:4]`, `[2:]`, `[:]`), step; stop exclusive;
  out-of-range clamps; **zero step** → runtime error `slice step cannot be
  zero`. Returns a **new** object (list copy, new text/bytes, new tuple).
- **Iteration** (`for x -> c`): list/tuple/set → elements; map → keys;
  text → 1-code-point Text; bytes → int byte; range → int (2A). Non-iterable
  → `'<t>' object is not iterable`. Mutating a collection **during its own
  iteration is UNDEFINED** (documented; no error guaranteed).
- **Membership** `v -> c`: range (2A), text (substring), bytes (subbytes),
  list/tuple (element `==`), map (key), set (element). Result is bool. Bad
  container → `unsupported operand types for ->: <t1> and <t2>`.

### Hashing and keys (map/set)
- **Hashable** (valid keys/elements): int, float, bool, None, text, bytes,
  tuple (all elements hashable). **Unhashable**: list, map, set, range,
  function → runtime error `unhashable type: '<t>'`.
- Equality/hash follow the **total `==`**: int `1` and float `1.0` are the
  **same key** (hash equal; first insertion's value/type retained for that
  key). `bool True`/`False` are distinct from int `1`/`0` (since `1 == True`
  is False). text vs bytes with equal bytes are **distinct**. Tuple keys
  compare/hash element-wise.

## 5. Text and Bytes (normative)

- **Text** is UTF-8. `len(t)` = code points. `t[i]` = 1-code-point Text.
  `for c -> t` yields code points. `"..."` literal; escapes as M1.
- **Bytes** is raw. `len(b)` = bytes. `b[i]` = int. `for x -> b` yields ints.
  `b"..."` literal; escapes M1 set + `\xHH`.
- `encode(text) -> bytes` (UTF-8); `decode(bytes) -> text` — invalid UTF-8 →
  runtime error `invalid UTF-8 in decode()`. `encode`/`decode` take the other
  type or error `encode() expects text, got <t>` / `decode() expects bytes,
  got <t>`.
- `+` concatenates text+text or bytes+bytes; mixed → type error. Text/bytes
  comparisons are byte-wise lexicographic (M1 string rule).

## 6. Functions (normative)

- **Parameters**: positional (required), optional defaults, optional `*name`
  (varargs → list), optional `**name` (kwargs → map). Order:
  `[positional…] [*vararg] [**kwarg]`. At most one vararg/kwarg. Required
  params must precede defaulted params → compile error
  `parameter with default must come after required parameters`.
- **Call binding**: positional args fill positional params in order; named
  args (`f(x=1)`) fill by name. Errors (compile-time where statically
  knowable):
  - unknown named arg → `unexpected argument name 'foo' in call to 'f'`
  - named arg matching an already-positionally-bound param →
    `argument 'x' got multiple values in call to 'f'`
  - missing required (no default, not vararg-covered) →
    `function 'f' expects at least N argument(s), got M` (or the exact form
    below)
  - too many positionals with no vararg → `function 'f' expects at most N argument(s), got M`
  - With `*args`, positionals are unbounded above the required count; with
    `**kwargs`, named args are unbounded. Static checks apply within these
    bounds; fully dynamic residue is checked at runtime where needed.
- **Defaults**: evaluated **fresh at each call** for each omitted argument
  (avoids Python's shared-mutable-default footgun and Pentest's
  define-time-vs-discovery timing problem). A default may reference globals
  and earlier params.
- **Function values**: `f = greet` binds a function value; `f(1)` calls it.
  Functions are first-class (args, returns, stored in collections). Indirect
  calls are dynamic. `print`/`range` remain call-only (cannot be read as
  values).
- **Nested functions and closures**: `function` inside a function body
  captures enclosing locals **by reference** (no declaration needed to read).
  Reassigning an enclosing function's variable from a nested function
  requires `nonlocal name`. Captured variables outlive their frame (heap
  cells). Deep nesting captures from the correct enclosing frame.
- **`global`/`nonlocal`**: declarations inside a function. `global name`
  makes subsequent assignments target the module global; `nonlocal name`
  targets the nearest enclosing function's binding. Reading a global needs no
  declaration. Rebinding a name that resolves to an enclosing function's
  local requires `nonlocal`. Mutating a mutable object's *contents*
  (`xs.append(1)`) is **not** rebinding and needs no declaration.
  **Placement**: a `global`/`nonlocal` declaration for a name must appear
  before any binding or use of that name in the function (compile error
  `name 'x' is used before 'global' declaration` / `... 'nonlocal' ...`).
  *(0.2.1 note: `variable.global` / `variable.parent` / `variable.<fn>`
  provide declaration-free qualified access to such bindings — the plain
  syntax rules above are unchanged; see
  [cross-scope-access.md](../reference/cross-scope-access.md) and SPEC §8.)*
- **Nested declarations — normative model (integrator-ratified; SEM and CGEN
  must implement exactly this):**
  1. A `function` statement is legal in a function body and in `main():`
     (any statement position, including inside `if`/`for`/`while` bodies).
     Top-level (outside `main():`) declarations keep the M1 behavior.
  2. **Name binding:** a nested decl's name becomes a **local** of the
     enclosing function (initialized to its closure at entry). A decl in
     `main():` or at top level registers as an ordinary **global function**
     (M1 behavior). A nested decl whose name collides with a parameter or an
     existing local/function is a compile error using the existing M1
     function-conflict wording; assigning to a nested function's name later
     is likewise a conflict error (never silently rebinding).
  3. **Hoisting:** nested closures are created in the **enclosing function's
     prologue** — before any body statement runs: (a) heap cells are
     allocated for every boxed (captured) local, initialized to `unset`
     (so M1's used-before-assignment guard still applies); (b) closure
     values are built capturing those cells; (c) they are stored into the
     named locals. Consequences: nested functions are callable before their
     source line (forward references work, matching M1's discovery
     philosophy); a declaration's body never executes at declaration; a
     nested function defined under a condition is still created at entry
     (declaration position controls nothing — documented divergence from
     Python).
  4. **Capture analysis (transitive):** a nested function captures every
     name that resolves to an ENCLOSING function's local/param, including
     references made only inside decls nested within it (if `b` contains
     `c` which mentions `x` owned by `a`, then `b` also captures `x` and
     hands it down through `b`'s env so `c`'s creation can capture it).
     Globals are never captured (static storage). Captured locals are
     **boxed**: all reads/writes in the owner go through the cell; the
     owner's `local_is_cell`/`param_is_cell` flags record boxing.
  5. **Calls to nested functions by bare name** go through the local's
     function value (dynamic `cs_call`); there is no static fast path for
     nested/capturing functions. Static direct calls (`cs_fn_x(...)`
     without env) are used only for top-level, env-less functions. Arity
     and named-argument checks remain STATIC whenever the callee
     declaration is known (including nested callees).
  6. Reading a top-level or nested function name as a value yields a
     function value (VarRes::Function); `print`/`range`/other builtins
     remain call-only.
- **Recursion** unchanged (2A). Recursion-depth safety: deep recursion may
  overflow the native stack — documented; a controlled runtime failure is
  acceptable (no guaranteed catch in 2B).

## 7. Comma-join (normative)

Where a full `expr` is parsed (assignment RHS, `return`, expression
statement), `a, b, c` evaluates each operand and joins their **display
forms** (§2) with a single space, producing **Text**. Examples:
- `m = "hello", "world"` → Text `hello world`.
- `return 1, 2` → Text `1 2`.
- `x = (1, 2), 3` → Text `(1, 2) 3` (tuple display, no flattening).
- `print(a, b)` → two args (print space-joins them — same visual result).
- `f(a, b)` → two args; `[a, b]` → two elements; `(a, b)` → tuple.

## 8. Builtins (normative)

Reserved call-only builtins: `print`, `range` (existing), plus `len`, `str`,
`int`, `float`, `bool`, `type`, `encode`, `decode`, `set`. Each is
call-target-only (cannot be read as values) and cannot be assigned, used as a
parameter, or redeclared (same reserved-name machinery as `print`, with the
appropriate name in the message).

- `len(x)`: text (code points), bytes, list, map, set, tuple. Else
  `object of type '<t>' has no length()`.
- `str(x)`: display form (§2) as Text. Works for all types (Bytes → `b"..."` form).
- `int(x)`: from text (decimal parse; invalid → `invalid literal for int(): '<s>'`), float (truncate toward zero), bool (0/1), bytes (decimal parse). From int → itself.
- `float(x)`: from text (parse), int, bool, bytes. Invalid → `invalid literal for float(): '<s>'`.
- `bool(x)`: truthiness (§9).
- `type(x)`: type name as Text (`"int" "float" "bool" "text" "bytes" "none" "list" "map" "set" "tuple" "range" "function"`).
- `encode(t)` / `decode(b)`: §5.
- `set(iterable)` / `set()`: build a set from a list/tuple/text/bytes/... `set()` → empty set (the only empty-set literal).

## 9. Truthiness (2B extension)

Falsy (unchanged + additions): `False`, `None`, int `0`, float `0.0`, empty
text, empty bytes, empty list, empty map, empty set, empty tuple, empty range.
Everything else truthy. `and`/`or`/`not` unchanged (2A: return bool,
short-circuit).

## 10. Compound assign / index targets

`+= -= *= /= %= **= &= |= ^= <<= >>=` (2A) extended to **index targets**:
`xs[i] += v` ≡ `xs[i] = xs[i] + v` with `xs` and `i` each evaluated **once**.
Same runtime op semantics/errors as 2A.

## 11. Compile-time errors (semantic — collect ALL; M1/2A rows preserved, plus)

| Condition | Message |
|---|---|
| read of `print`/`range`/any new builtin as a value | same treatment `print` receives (M1) |
| assignment to a builtin name | `cannot assign to reserved name '<b>'` |
| param named a builtin | `cannot use reserved name '<b>' as parameter` |
| `function <b>():` | `cannot declare function with reserved name '<b>'` |
| named argument in a method call | `method arguments must be positional` (integrator amendment: raised at COMPILE time — method calls are statically dispatched; was listed under §12) *Superseded by M2-2C §3.2: named arguments are legal on user bound methods; the runtime wording remains for builtin container methods.* |
| default after required… (order) | `parameter with default must come after required parameters` |
| unknown named arg (static) | `unexpected argument name '<n>' in call to '<f>'` |
| named+positional duplicate (static) | `argument '<n>' got multiple values in call to '<f>'` |
| too few (static) | `function '<f>' expects at least <n> argument(s), got <m>` |
| too many (static, no vararg) | `function '<f>' expects at most <n> argument(s), got <m>` |
| bare method reference `obj.name` uncalled | parser: `methods must be called; method references are not supported yet` *Superseded by M2-2C §3.2 (legal attribute read).* |
| `global`/`nonlocal` used before declaration | `name '<n>' is used before 'global' declaration` / `...'nonlocal'...` |
| `nonlocal` with no enclosing binding | `no binding for nonlocal '<n>' found` |
| `global` at top level / main (no-op warning?) | allowed but redundant; not an error |

M1/2A semantic rules (duplicate function/param, only-one-main, undefined
names, function-as-variable, return placement, reserved names, arity for
fixed functions) all still apply, adapted for indirect calls (a call whose
callee is not a bare known function name skips static arity check → runtime).

## 12. Runtime errors (M1/2A tables plus)

| Condition | Message |
|---|---|
| list/tuple/text/bytes index out of range | `list index out of range` / `tuple index out of range` / `text index out of range` / `bytes index out of range` |
| non-int index | `<container-type> indices must be integers, not <t>` |
| map missing key | `map key not found` |
| slice step 0 | `slice step cannot be zero` |
| slice on map/set | `'<t>' object does not support slicing` |
| unhashable key/element | `unhashable type: '<t>'` |
| not iterable | `'<t>' object is not iterable` (2A) |
| unknown method for type | `'<t>' object has no method '<m>'` |
| wrong method arg count/type | `'<t>.<m>() takes N argument(s)` / descriptive |
| `encode`/`decode` type | `encode() expects text, got <t>` / `decode() expects bytes, got <t>` |
| `decode` invalid UTF-8 | `invalid UTF-8 in decode()` |
| `int`/`float` parse | `invalid literal for int(): '<s>'` / `invalid literal for float(): '<s>'` |
| `len` of non-sized | `object of type '<t>' has no length()` |
| unpack too few/too few | `not enough values to unpack (expected N, got M)` |
| unpack too many | `too many values to unpack (expected N, got M)` |
| unpack non-iterable | `'<t>' object is not iterable` |
| index-assign on immutable/text | `'text' does not support item assignment` (also bytes/tuple) |
| calling a non-function | `object of type '<t>' is not callable` |

Static arity wording (integrator clarification, verified against the
implementation): the `at least`/`at most` forms apply to functions with
defaults or a `*args` vararg. A function whose positional parameters are
all required and which has no vararg keeps the **M1 fixed-arity form** —
`function 'f' expects N argument(s), got M` (no `at least`/`at most`) —
asserted by the pre-existing `err_arity` case.

Type names in all messages: `unset None int float bool text bytes range list
map set tuple function`.

## 13. Generated-C / runtime contract additions

- **Heap objects** (`list/map/set/tuple/function`) are `cs_obj*` behind a
  header with a reserved refcount field (unused in 2B). cs_value's union
  gains a pointer slot for heap objects; text/bytes stay pointer+length
  inline (immutable). 2B **leaks** heap objects (no reclamation) — documented
  limitation, scheduled for 2C.
- New runtime constructors/ops for each collection, indexing (neg + range
  check), slicing (full Python normalization), iteration protocol extended to
  all iterables, membership extended, hashing + total-equality map/set,
  display/`str()` for every type, the §8 builtins, and runtime method
  dispatch `cs_call_method(obj, name, args, n, line)`.
- **Function values / bindings / closures — normative ABI (SEM, CGEN and RT
  must agree; §6 nested model applies):**
  - Every generated function has the **uniform ABI**
    `cs_value cs_fn_x(cs_value* bound, cs_cell** env)`; top-level functions
    are called with `env = NULL`. `cs_cell` is `{ cs_value v; }`, heap.
  - The callee receives a **bound vector**: `bound[i]` is either a value or
    the sentinel `CS_MISSING`. Entry code (a) evaluates `CS_MISSING`
    defaults **in parameter order** (a default may reference globals and
    earlier `bound` params), (b) when `has_vararg` and the slot is
    `CS_MISSING`, builds an empty list (else uses the binder's list),
    (c) when `has_kwarg`, likewise an empty map. Then normal body semantics
    apply (boxed locals are the cells, not the bound vector — copy bound →
    cell/local at entry; a boxed param's cell is allocated in the prologue
    and initialized from `bound`).
  - **Static call site** (callee known, positional-only): stack-allocate
    `cs_value bound[N]`, fill supplied positionals left-to-right, put
    `CS_MISSING` in omitted default slots, call `cs_fn_x(bound, env)`
    directly (`env` = NULL at top level). Named args on a static call are
    placed into their slots at the call site (semantic has verified names).
  - **Dynamic call**: evaluate all argument values and their name strings,
    then `cs_call(fnv, posvals, npos, argnames, nnamed, line)`; the runtime
    binder uses the function's `cs_funmeta` to fill a bound vector and
    raises the §11 binding errors at RUNTIME when the callee was not known
    statically (missing required, unknown name, duplicate binding). Calling
    a non-function → `object of type '<t>' is not callable`.
  - `cs_funmeta` (carried by every function value): function display name,
    param names + count, required count, per-param has-default mask,
    has_vararg/has_kwarg, and the fn pointer.
  - **Function value** = `{ cs_funmeta* meta; cs_cell** env; }` tagged
    `function`. Function equality (if exercised) is by meta identity.
  - `global` targets static global storage; `nonlocal` writes the captured
    cell; builtins stay direct static calls.
- Codegen keeps M1/2A structure; the Call node's authoritative callee is an
  expression (`callee_expr`); bare-ident fast path and builtin detection
  retained. Method calls lower to `cs_call_method`. Index-assign evaluates
  object+index once into temporaries.
- Every failing op receives its AST node's Pentest source line (M1 rule).

## 14. Ownership (Stage 2B agents)

```
docs/contracts/m2b-semantics.md  src/ast.h  src/frontend.h  src/codegen.h
src/diagnostics.*                                          (contract — integrator)
src/lexer.{h,cpp}  src/parser.{h,cpp}                       (frontend syntax)
src/semantic.{h,cpp}                                       (semantic + capture analysis)
src/codegen.cpp                                            (C backend + closures)
runtime/cstar_rt.{h,c}                                   (collections + functions + builtins)
src/main.cpp Makefile tools/embed.cpp                      (integrator)
tests/** bench/** editors/vscode/** SPEC.md README.md      (test/docs/vscode agents, later)
```
Agents must not run `make`; syntax-check single files
(`clang++ -std=c++17 -Wall -Wextra -fsyntax-only -Isrc src/<f>.cpp`,
`clang -std=c11 -fsyntax-only runtime/cstar_rt.c`) and scratch-test in
/tmp. Contract files are read-only to agents; report suspected bugs.

## 15. Test plan (Stage 2B acceptance — contract for the test agent)

Collections: literals (all six forms + nesting), empty forms, indexing
(in-range/negative/out-of-range/non-int), index-assign (list/map, single-
eval proof), slicing (open bounds/negative/step/zero-step error/map-set
error/new-object proof), iteration (list/map-keys/set/tuple/text-codepoints/
bytes-ints), membership (all container types + unhashable + bad-type),
reference aliasing (`b = a; b.append(...)` mutates `a`), map insertion-order
+ 1-vs-1.0 same-key + True-vs-1 distinct-key, set uniqueness + order.
Text/Bytes: code-point indexing/iteration (incl. a multibyte char), `len`,
encode/decode round-trip + invalid-UTF-8 error, `+`, mixed-type errors,
bytes display, `\xHH` literals.
Destructuring: flat/nested/rest (middle + last), too-few/too-many errors,
index targets, join-vs-tuple disambiguation (`a, b = (1,2)` vs `a, b = 1,2`).
Comma-join: assignment/return/expr-stmt, nested tuple display, `print`/call/
literal commas unaffected.
Methods: every listed method per type (append/pop/insert/index/remove/
reverse/clear; map get/keys/values/items/pop/clear; set add/remove/discard/
pop/clear; text upper/lower/split/join/find/replace/strip/starts_with/
ends_with), unknown-method error, wrong-arg error, method-on-wrong-type.
Builtins: len/str/int/float/bool/type/encode/decode/set incl. error paths.
Functions: defaults (fresh-per-call proof), named args + all binding errors,
*args/**kwargs capture, function values (pass/return/store/indirect call),
closures (capture-by-reference, lifetime after return, recursion via closure,
deep nesting), global/nonlocal (rebind vs mutate distinction, placement
errors, nonlocal-no-binding error). Reserved builtin names.
Preserve: full 161-check suite green except approved changes (string→text
display names, nested-function restriction lifted).

## 16. Guiding constraints

Preserve M1/2A semantics (wrap, toward-zero division, total equality, print
formatting, execution model, `main():`). Performance is a first-class 2B
goal: benchmark collection/function/text overhead (the bench suite is
extended). No C undefined behavior. Keep the toolchain pure C++/C11.
