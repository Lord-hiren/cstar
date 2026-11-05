# Cross-scope variable access — `variable.<qualifier> <name>`

> Normative. Summarised in SPEC §4.1/§8; user-level introduction in the
> [language tour](../guide/language-tour.md#cross-scope-access-with-variable).

Status: **normative for the cross-scope access feature** (referenced from
`src/ast.h`, `src/parser.cpp`, `src/semantic.cpp`, `src/codegen.cpp` and
the runtime). Stage: `0.2.1`. Section numbers are cited from code
comments and test headers — §3 syntax, §4 semantics (lookup + pinned
storage), §5 lowering.

`variable.` lets one function reach a binding that ordinary name
resolution would never find there: a module global without a `global`
declaration, the enclosing function's variable without `nonlocal`, or a
sibling function's param/local by name.

## 1. The three forms

```
variable.global a          # a module global, from anywhere
variable.parent a          # the nearest enclosing function's binding
variable.<function> a      # a param/local of a sibling function
```

Each form is a single expression (a read) and may also appear as an
assignment target — plain (`=`) or compound (`+=`, `-=`, …) — and as a
leaf inside a tuple-destructuring target. It may not be a `for` loop
variable or an `except ... as` binding (those grammar positions take a
plain identifier only).

## 2. One storage per binding *(the model)*

A binding is **pinned** the moment any `variable.` site references it:
from that point on the binding has exactly ONE storage — a
program-lifetime static slot — and **every** access to it, the owner's
own reads/writes included, lowers to that slot. There is no copy, no
sync and no second slot anywhere: `variable.a x = 5` inside sibling `b`
is literally the same C storage that `print(x)` inside `a` reads.

Consequences, all deliberate — the normative detail lives in §4
(lookup + pinned semantics) and §5 (lowering):

* **Forward references work.** Qualified sites are resolved in a final
  semantic phase after every function's params/locals/boxing flags are
  final, so `variable.<fn> <name>` may target a function declared later
  in the file — matching the language's forward-reference philosophy.
* **Recursion shares the pin.** Pinned storage is program-lifetime, not
  per-invocation (like a C `static` local): recursive activations all
  read/write the one slot.

## 3. Syntax *(no new keyword)*

The atom is parsed contextually: `Ident("variable")` `.` qualifier `name`
— four tokens on one line, where the qualifier is any identifier plus the
reserved `global` keyword (`variable.global` is unambiguous because
`global` can never be a name). The old grammar can never follow an
attribute read with a bare identifier, so no existing program changes
meaning; a program that legitimately uses `variable` as an ordinary name
keeps working unless it then writes `variable.<ident> <ident>` — which
was a syntax error before and is now this construct.

```
b = variable.global a               # read
variable.global a = 20              # write
variable.holder x += 1              # compound
(variable.parent p, q) = (7, 8)     # destructuring leaf
print(variable).a xs[0]            # qualified object of an index chain
```

## 4. Semantics *(lookup + pinned storage)*

### 4.1 Lookup rules

* `variable.global n` — `n` must be a module global (declared at top
  level or through `global n` somewhere). Function names are rejected.
* `variable.parent n` — the nearest **enclosing** function that binds
  `n` wins (outward, `nonlocal`-like; the current function itself is
  skipped). No enclosing binding is an error — `global`-declared names
  in enclosing functions do not count (use `variable.global`).
* `variable.<fn> n` — `<fn>` must share the site's scope: a top-level
  function when the site sits at top level (incl. inside `main:`), or
  a nested function of the site's own enclosing function otherwise (a
  nested function may name *itself*; to reach the enclosing function by
  name is what `variable.parent` is for). `n` may be any param, the
  vararg/kwarg slot or any local of `<fn>`.

Resolution is declaration-order independent: sites are recorded during
body resolution and validated in a final phase, so any qualified site
may target any function in the file. Failures are compile errors naming
the exact form: `no binding for 'variable.<q> <n>' found` /
`'<fn>' is not a function sharing this scope`.

### 4.2 Pinned storage

The first `variable.` site that resolves a binding marks it pinned; the
codegen then allocates its storage as a **program-lifetime static
instead of a frame slot**. Two shapes, decided by whether some closure
also captures the binding:

* **Plain pin** — `static cs_cell cs_p_<c_name>_<name>;` at file scope.
* **Pinned cell** — `static cs_cell* cs_px_<c_name>_<name>;` pointing at
  a heap cell created once at program entry (§4.3).

Either way the binding follows these rules:

* **No frame slot.** Pinned bindings declare no per-invocation frame
  entry — the static *is* the storage — and are excluded from the
  frame's root registration (a per-invocation entry would only
  double-register them; they are rooted at file scope like a module
  global instead). An unboxed pinned **param** materializes straight
  into the static slot at entry; a pinned binding assigned at the top
  of the owner's body still executes that assignment on every entry.
* **The owner's plain name is the pin.** Inside the owner function,
  every ordinary read/write of the binding hits the same static — the
  qualified path and the ordinary path are one storage by construction.
* **Unset stays honest.** A pin starts unset; reading it before the
  owner's first assignment raises the ordinary
  `used before assignment` runtime error (same `cs_guard`, same message
  as any slot read — including reads through `variable.`).
* **Values persist across invocations.** After the owner's first
  assignment, the pin keeps its value until rebound — re-entering the
  owner does not reset it.
* **One storage ⇒ one lane.** The qualified path reads the dynamic
  `cs_value` slot, so the binding — and any global reached through
  `variable.global` — is poisoned out of the native-int lane: the
  ordinary path must read the same slot. Conservative by one slot,
  exactly like every other poison (see
  [optimization.md](../performance/optimization.md) §7).

### 4.3 Closures: immortal pinned cells

Closures must share cells through env slots that point at *registered
GC blocks*, never at static memory — hence the indirection of the
pinned-cell shape. A pinned binding that is also captured does not
allocate per invocation: entry aliases the owner's `cs_cell_<name>` to
the immortal pinned cell created once before any function runs, and that
cell is registered in the collector's pinned-cell table, **rooted at
every collection for the whole program**. So closures from every
invocation share the one cell, its value survives any collection, and
the program-lifetime rule of §4.2 holds uniformly.

## 5. Lowering *(codegen)*

* **Reads** (`emit_expr`, `ExprKind::VarQual`): the pinned static (or
  the module global, for `variable.global`) is read directly —
  `cs_guard(<storage>, "<name>", …)`, the same guard and message as an
  ordinary read of that storage, no frame lookup of any kind.
* **Plain/compound stores** (`AssignTarget::Kind::VarQual`): stored
  into the pinned static (or module global) directly; compound forms
  read the slot with the same guard an ordinary compound assignment
  uses. Destructuring leaves store through the same lvalue.
* **`has_effect` / DA / allocation analysis** treat a qualified read
  like an ordinary guarded read (it can raise the unset error), except
  that its value never counts toward the owner's local int-lane sets —
  §4.2's poison decides the lane, not the local context.

## 6. What is *not* supported

* `variable.` as a `for` loop variable or an `except ... as` binding
  (grammar positions take a plain identifier).
* Reaching into arbitrary non-sibling functions, methods, or an
  enclosing function *by name* (use `variable.parent`).
* Cross-file access: a `variable.` site resolves against the declarations
  of the file it appears in; imported modules' bindings are renamed to
  module-unique internal names and are not reachable this way.

## 7. Performance stance

The feature needs no dedicated optimization because it has no
slow path to begin with: pinning moves the binding's storage from a
frame slot to a static, which the C compiler treats identically (one
load/store, constant-foldable address). The only cost paid anywhere is
the poison slot in the native-int lane — correctness by construction,
not by pass. See [optimization.md](../performance/optimization.md) §7.
