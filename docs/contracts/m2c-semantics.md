# M2-2C Working Contract — classes, exceptions, garbage collection

> Part of the [stage contracts](README.md) (normative; they govern over
> [`SPEC.md`](../../SPEC.md) if the two differ).

Status: **normative for Stage 2C integration** (like `M2-2A/2B-SEMANTICS.md`
before it). The stage ends with `SPEC.md`/`README.md` being rewritten from
this record. Decisions marked *(ratified)* were made by the integrator
during design; deviations require updating this file first.

Stage: `0.2.0-m2c`. Order of work: **GC → exceptions → classes + `is`**,
each sector re-verified at 268/268 before the next begins.

## 0. Scope

In: garbage collection of every leaking 2B allocation kind (objects,
text/bytes buffers, closure cells); catchable exceptions
(`try/except/finally`, `raise`, runtime errors become throwable builtin
exceptions with byte-identical uncaught output); user classes
(instantiation, methods, instance attributes, `__init__`); object identity
`is` / `is not`; `collect()` builtin.

Out (documented, later stages): inheritance/MRO of any kind; user-defined
exception hierarchies (custom raisable types); class-level attributes;
generators; weak references; modules/imports (2D); static typing.

## 1. Garbage collection

### 1.1 Algorithm *(ratified)*

Non-moving **mark&sweep** over a registry of every heap allocation, with
precise roots. No reference counting — although `cs_obj.refcount` was
reserved for RC, RC would require incref/decref discipline through every
generated temp, every bound-vector slot and every runtime op's internals
(error-prone and pervasive); mark&sweep needs only *root registration*,
which the codegen already almost provides. The `refcount` field is
reused as the mark bit's carrier where convenient; behavior is
irrelevant to programs.

### 1.2 What is collected *(ratified)*

One intrusive registry (doubly-linked, prefix `{next, prev, kind,
marked}`) covers every leak class 2B measured:

| kind | payload | trace |
|---|---|---|
| `CS_LIST CS_MAP CS_SET CS_TUPLE CS_FUNCTION` (+ 2C: class/instance/exception/bound-method) | `cs_obj` | per-tag members (seq items, table entries, fun env cells, …) |
| text buffer | bytes after the prefix | leaf |
| bytes buffer | bytes after the prefix | leaf |
| closure cell (`cs_cell`) | the cell's `cs_value` | the value |
| env array (`cs_cell**`) | **not** registered — owned by its `CS_FUNCTION` object and freed with it | (cells inside are individually registered) |

Member arrays (`seq.items`, `table.e`, `table.slots`) are owned by their
object, reallocated in place, and freed with the owner at sweep.
Realloc never happens during a collection (see 1.4 safe points), so
member pointers are always coherent at mark time. Static string
literals (`CS_STR`) and builtin class singletons are immortal: never
registered, never freed.

`cs_text_alloc` / `cs_bytes_alloc` / `cs_cell_alloc` route through the
prefix allocator; existing `cs_obj_new` gains registry linkage.

### 1.3 Roots *(ratified)*

1. **Globals**: codegen emits `static cs_value* const cs_g_roots[] = { &cs_g_a, ... };`
   (or a NULL/0 pair) handed to `cs_init`.
2. **Shadow frames**: every generated function (and `cs_main_block`)
   declares *all* locals **and all temporaries at function top**
   (temp declarations are hoisted from mid-body) and registers their
   addresses in a shadow-stack frame at entry, popping at exit. Because
   every slot's address escapes into the frame, values are memory-
   coherent after a `longjmp` landing (§2.5).
3. **Handler stack** (§2): each pending entry roots its caught
   exception value.
4. **Protect stack**: values that exist only in a runtime/call-site C
   local while generated code runs are protected across the call. This is
   implemented **callee-side**: every generated function protects its own
   `bound` vector from its first line until slot materialization completes
   — covering the binder's `*args`/`**kwargs` (dynamic) *and* the static
   `barr[]` compound literal uniformly, since neither is addressable from
   the caller's registered slots. (The `cs_call` binder additionally frees
   its bound vector after the call — a 2B leak closed here.)
5. **Builtin singletons** (exception class values).

### 1.4 Collection trigger *(ratified)*

Allocations count bytes; when the total exceeds a threshold that doubles
after each sweep, the *next safe point* collects. Safe points are emitted
by codegen where **all live values sit in registered slots**: function
entries, loop back-edges (`while`/`for`), and top-level statement
boundaries in `cs_main_block` and C `main`. The runtime never collects
mid-operation, so no runtime op needs protection except the callback case
in 1.3(4).

**Statement-end temp clearing (implementation rule):** after each
statement, the emitter assigns `cs_unset()` to every temp that statement
created. Without this, a stale temp slot would keep the last-evaluated
values (and everything they reference, cycles included) reachable until
the slot was reused or the frame exited — reclamation would not be
prompt. `return` skips the clearing: its frame is unlinked instead.

New builtin **`collect()`**: forces a full collection now, returns the
number of reclaimed registry blocks (int) — deterministic test hook.

### 1.5 Semantics visible to programs *(ratified)*

No program can observe collection except through memory use and
`collect()`'s return: non-moving, no finalizers/weakrefs, `is` (§3.4)
is stable for live objects. Mutating a collection during its own
iteration stays undefined (2B §).

## 2. Exceptions

### 2.1 Syntax *(ratified)*

```
try_stmt := "try" ":" body
            ("except" [expr ["as" ident]] ":" body)*
            ["finally" ":" body]
raise_stmt := "raise" expr
```

New keywords: `try except finally raise as`. At least one except or
finally clause is required (compile error otherwise). `as` binds the
exception value for the clause body (scoped like other statement
bindings). `except` with no expression catches everything.

### 2.2 Builtin exception classes *(ratified)*

Runtime errors become *throwable* builtin exception values. Each is a
singleton class value (`ValueError`, `TypeError`, `NameError`,
`ZeroDivisionError`, `IndexError`, `KeyError`, `AttributeError`,
`RuntimeError`, …); instances carry `.type` (text), `.message` (text)
and the raise-site line (not exposed). Mapping rule: every
`cs_runtime_error` site adopts the closest Python-style class; the
message strings are **unchanged**. Uncaught exceptions print
`<src>:<line>: runtime error: <message>` to stderr and exit 1 —
byte-identical to 2B for every existing case.

Exception display (`print`/`str`): `ValueError: division by zero`.
`type(e)` is the type name (`"ValueError"`).

### 2.3 `raise` *(ratified)*

`raise expr` — `expr` must evaluate to an exception instance or to a
bare exception class (constructed with an empty message). Anything else:
`TypeError: exceptions must be class or instance, got <t>`. There is no
bare `raise`; re-raise via the `as` binding (`raise e`).

### 2.4 Matching *(ratified)*

`except X as e:` runs when the pending exception's class **is** `X`
(exact identity — there is no inheritance in 2C; `except:` catches
all). A non-class `except` expression is a runtime `TypeError`.
If no clause matches, the exception continues unwinding (after
`finally`, which always runs — 2.6).

### 2.5 Unwinding implementation *(ratified)*

setjmp/longjmp (C11, no threads): the runtime keeps a handler stack of
`{jmp_buf, shadow-frame depth, kind (CATCH/FINALLY), pending value}`.
`cs_throw` pops entries above nothing, restoring the shadow stack to
the entry's depth, then `longjmp`s into the landing pad, which re-raises
(`cs_rethrow`) when the entry is FINALLY (after running its block) or
when no except clause matched. Nesting composes: a try with both
excepts and a finally lowers to nested trys (inner: except logic,
outer: finally).

`break`/`continue`/`return` crossing a `finally` run the finally block
first (code-gemitted on the normal edge; unwinding edges are handled by
the handler stack).

### 2.6 Interactions *(ratified)*

- An exception raised *inside an except clause or during unwinding*
  replaces the in-flight exception (chaining deferred to a later stage;
  documented).
- `finally` runs on: normal completion, `break/continue/return` out of
  the try, and exception unwind. Always exactly once per entry path.
- Errors in `except` matching expressions propagate normally.
- `cs_runtime_error` keeps its signature but now *throws* the mapped
  class; the uncaught printer produces the 2B byte format.

## 3. Classes and object identity

### 3.1 Syntax *(ratified)*

```
class_decl := "class" ident ":" body        # methods only
method     := "function" ident "(" params ")" ":" body    # first param is self
```

Class bodies execute at their declaration point (discovered in advance
like functions; referencing a class before its declaration line runs
gives the normal used-before-assignment error). Class bodies may
contain only `function` declarations, `pass`, and comments — anything
else is a compile error. Classes are global-scope only (nested classes
are a compile error). No inheritance: `class C(Base):` is a syntax
error in 2C.

### 3.2 Semantics *(ratified)*

- `Point(1, 2)` on a class value: allocate instance, then call the
  class's `__init__(self, 1, 2)` if defined (arity-checked like normal
  calls; missing `__init__` with arguments is a runtime `TypeError`).
- `p.method(args)`: attribute lookup yields a *bound method* (instance +
  function), called through the ordinary call machinery with `self`
  prepended — so named arguments/`**kwargs` work on methods.
- Attributes are dynamic: `p.x = v` writes an instance field; reads
  check instance fields, then (for exceptions) the builtin `.type`/
  `.message` fields, then methods, then
  `AttributeError: 'Point' object has no attribute 'x'`.
  Attribute assignment on non-instances: runtime `TypeError`.
- `type(p)` is the class name (`"Point"`); class values display as
  `<class Point>`; instances as `<Point instance>`; bound methods as
  `<method 'move' of Point>` — all deterministic (no addresses: the
  test suite requires byte-equality).

### 3.3 Classes as exceptions *(ratified)*

`raise` accepts only builtin exception classes/instances (2.3). User
class instances are not raisable in 2C; user-raised errors use
`raise Exception("message")` and are caught by bare `except:` or by
inspecting `.type`/`.message`.

### 3.4 Identity `is` *(ratified)*

`a is b` — true iff both operands are the same heap object, or (for
non-object operands: int, float, bool, none) equal by value; two text
values are identical iff they share storage (literals may; computed
texts generally do not). `is not` negates. Precedence: same level as
`->`/comparisons; chaining rules identical to `==`.

## 4. Test/verification obligations

- Every existing case stays byte-identical (rt_* exit codes + messages
  especially — the uncaught format is a compatibility contract).
- New cases needed: GC reclamation + `collect()` counts, cycle
  collection (mutual list/map references), exception raise/catch/finally
  flows incl. break/return through finally and nested unwinding,
  runtime-error catchability, class/method/attribute/`__init__` flows,
  bound-method kwargs, `is`/`is not`, display of all new types.
- Bench re-record: RSS rows must collapse toward the 12 MB floor
  (dispatch_ops 344 MB → bounded, text_ops 89 MB → bounded,
  while_string 44 MB → bounded); timings re-measured and documented in
  the 2C baseline update.
