# Gotchas and FAQ

Pentest looks like Python, and most of the time it behaves like Python — but it
is a different language, and the differences are deliberate. This page is the
checklist of things that surprise people, each with the reason and the
spelling that works. Every program here is run against the real compiler.

## Syntax

### `import` and `from` are keywords

A program that used `import` or `from` as an identifier no longer compiles:
they are reserved for [modules](modules.md). `as` was already a keyword.
Imports are top level only — inside a function →
`'import' is only allowed at the top level`.

### `//` starts a comment — there is no floor division

```python
x = 7 // 2
print(x)
print(7 / 2)
```

Output:

```text
7
3
```

`x = 7 // 2` assigns `7` (everything after `//` is discarded). `/` between ints
truncates toward zero. Use `/` — and remember `-7 / 2` is `-3`, not `-4`.

### `in` is both membership and the `for` binder

`x in container` tests membership;
`for x in container:` iterates.

```python
print("b" in["a", "b"])
for c in "ab":
    print(c)
```

Output:

```text
True
a
b
```

### `for` binds exactly one name

`for a, b in pairs:` is a syntax error (`expected 'in' after loop variable`).
Bind one name and destructure inside the body — or use `items()`:

```python
ports = {"http": 80, "https": 443}
for item in ports.items():
    name, port = item
    print(name, port)
```

Output:

```text
http 80
https 443
```

### The type is `text`, not `string`; `None` is `none` from `type()`

`type("a")` is `"text"` and `type(None)` is `"none"` — but `print(None)`
shows `None`, and so do error messages.

### No string formatting syntax, no repetition

There are no f-strings, no `%` on text and no `"ab" * 3` (`*` on text is a
runtime error). Build strings with `+`, `str()`, `format()` or comma-join:

```python
host = "web1"
port = 443
print("host=" + host + " port=" + str(port))
print(format(port, "06d"), format(0.5, ".1%"))
```

Output:

```text
host=web1 port=443
000443 50.0%
```

## Values

### An unparenthesised comma list makes **text**, not a tuple

```python
pair = "a", 1
print(pair, type(pair))
pair = ("a", 1)
print(pair, type(pair))
```

Output:

```text
a 1 text
(a, 1) tuple
```

At the right-hand side of `=` or in a `return`, `a, b` joins the display forms
with a space. Parenthesise for a tuple. (SPEC §4.4.)

### `and` / `or` return a `bool`, not an operand

```python
print(0 or "default", "x" and 5)
name = None
print(name or "anon")
```

Output:

```text
True True
True
```

There is no `x or default` idiom. Use the conditional expression:
`name if name else "anon"`.

### `{}` is an empty **map**; empty sets and maps display the same

```python
print({}, (set()), (type({})), type(set()))
```

Output:

```text
{} {} map set
```

Use `set()` for an empty set, and `type()` to tell them apart.

### Integers wrap; floats and ints do not mix in `%`

```python
print(9223372036854775807 + 1)
print(2 ** 64)
print(7 % 3)
```

Output:

```text
-9223372036854775808
0
1
```

`%` accepts two `int`s only (`5 % 2.0` is a runtime error), and bitwise
operators do not accept `bool`.

### `==` is total, ordering is not

`1 == "1"` is simply `False`, but `1 < "1"`, `None < 1` or `[1] < [2]` raise
`unsupported operand types`. `True == 1` is `False` (bool is its own type), and
`text` never equals `bytes`.

### Text is code points; `upper()`/`lower()` are ASCII only

```python
print(len("héllo"), "héllo".upper())
```

Output:

```text
5 HéLLO
```

### `print` shows text without quotes — even inside collections

`print(["a", "b"])` shows `[a, b]`. Use `repr("a")` for a quoted form of a
single value.

## Names and scope

### Imports bind the last name, not a namespace

`import lib.toolkit` binds **`toolkit`** (or the alias after `as`), because
that is the name the linker can rewrite statically. `lib` itself does not
exist in the program, so `lib.toolkit.greet` is `undefined variable 'lib'`.
After `import a.b`, write `b.name`; to use a different name, alias it:
`import lib.toolkit as tk`. A module alias is not a value (`x = tk` is an
error), `from m import x` makes `x` read-only (`cannot assign to imported
name 'x'`), and `_`-prefixed module declarations cannot be imported or read
from outside at all.

### `check` is strict about imports — `run` is not

`pentest run`/`build` drop unused imported declarations *before* analysing
them, so an unused broken function in an imported library does not stop the
program. `pentest check` validates **every** imported declaration and fails on
that same file. If a library must always be fully clean, run `check` in CI; if
a large library has unused corners, `run` still works. Both modes agree on
everything the program actually uses. See
[modules and imports](modules.md#selective-loading-you-only-pay-for-what-you-name).

### Builtins are not values

```python
f = sorted
```

Output:

```text
example.pen:1:5: error: undefined variable 'sorted'
  f = sorted
      ^
```

You cannot pass `str`, `len` or `abs` to `map`/`sorted(..., key=…)`; wrap
them in a named function declaration (`name(…):`). You also cannot name your own function or variable `sum`,
`len`, `max`, `list`, `id`, `hash`, `format`, `input`, `time`, … — all seventy-eight
builtin names are reserved
([builtins guide](builtins.md)). Check the list before naming helpers;
`examples/functions.pen` itself once broke when `sum` became reserved.

### Variables are not hoisted

A global exists from program start but is *unset* until its assignment runs.
Reading it earlier is a **runtime** `used before assignment` error. Functions,
however, *are* discovered up front and can be called before their definition.

### Assigning in a function creates a local

To change a global use `global name`; to change an enclosing function's
variable use `nonlocal name` — or reach out with
[`variable.`](../reference/cross-scope-access.md).

### Nested functions are created at function entry

A nested function declaration (`name():` / `name(params):`) under an `if` is
still created when the enclosing function
is entered — declaration position controls nothing (SPEC §7.2). Defaults are
evaluated at call time, fresh each call.

### `main:` is a block, not a function

It runs in place, in global scope, at its position in the file. Code *after* it
still runs. You cannot call it, return from it, or declare a function named `main`.

## Control flow

* **`break` only means "leave the loop".** It is an error inside a bare
  `switch`, and inside a loop it breaks the loop even from within a `switch`.
* **`switch` labels are constants only**; first match wins, no fall-through;
  `case *:` must be last.
* **Mutating a list/map/set while iterating it is undefined.**
* **Iterating a map yields keys.** `keys()`, `values()` and `items()` return
  snapshot lists, not live views.

## Exceptions and classes

* **Matching is by exact class.** `except TypeError:` does not catch a
  `ValueError`, and `except Exception:` is *not* a catch-all. Only a bare
  `except:` catches everything (but cannot bind the value).
* **User classes cannot be raised or subclassed.** There is no inheritance, no
  `super()`, no class attributes, and instances are unhashable (no instance map
  keys).

## Builtins

* `zip`, `map`, `filter`, `enumerate`, `reversed` return **lists**, eagerly.
* `divmod` truncates (Python floors).
* `input()` returns `None` at end of input (and `""` for an empty line) —
  check before using the result as text.
* Program arguments (`pentest run prog.pen a b`) reach the process but **no
  builtin can read them yet**.
* `format()` with the `,` option **and** zero-padding (`"08,"`-style specs)
  pads the already-grouped string instead of regrouping a padded digit run —
  `format(42, "08,")` gives `00000042` where Python gives `0,000,042` — see
  [known issues](../reference/known-issues.md) §7.

## Tooling

* `pentest check` is silent on success, by design.
* A change to the `pentest` binary re-keys the whole run cache; the first run
  after a rebuild recompiles the runtime (~seconds), later ones are fast.
* `build` writes `<basename>.pentest` into the **current directory**, not next to
  the source.
