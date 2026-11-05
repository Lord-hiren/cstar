# Language tour

A practical walk through the whole cstar language, from `print` to classes,
exceptions and cross-scope access. Every example on this page is a complete
program followed by the exact output the `0.2.1` compiler produces for it —
these blocks are executed against the real toolchain whenever the docs are
verified (see [documentation guide](../development/documentation-guide.md)).

This page teaches. When you need the precise rule, follow the `§` links into
the normative [language specification](../../SPEC.md).

**Contents**

1. [Hello, script](#1-hello-script)
2. [Comments](#2-comments)
3. [Values and types](#3-values-and-types)
4. [Variables and assignment](#4-variables-and-assignment)
5. [Operators](#5-operators)
6. [Text and bytes](#6-text-and-bytes)
7. [Collections](#7-collections)
8. [Control flow](#8-control-flow)
9. [Functions](#9-functions)
10. [The execution model: scripts and `main:`](#10-the-execution-model-scripts-and-main)
11. [Scope: `global`, `nonlocal` and `variable.`](#11-scope-global-nonlocal-and-variable)
12. [Classes](#12-classes)
13. [Exceptions](#13-exceptions)
14. [Builtins](#14-builtins)
15. [Memory management](#15-memory-management)
16. [What cstar deliberately does not have](#16-what-cstar-deliberately-does-not-have)

How to run the snippets: save one as `example.cst`, then
`build/cstar run example.cst` (see [getting started](getting-started.md)).
When a snippet fails on purpose, the output block shows what it prints to
standard output first and then the diagnostic it writes to standard error.

---

## 1. Hello, script

A cstar file is a *script*: statements run top to bottom.

```python
print("Hello, cstar!")
```

Output:

```text
Hello, cstar!
```

Blocks are introduced by a colon and **indentation with spaces** — a tab in
leading whitespace is a compile error. A statement ends at the newline; there
are no semicolons and no line-continuation character (a statement can span
lines only inside `( )`). An empty file is a valid program that prints nothing
and exits `0`.

---

## 2. Comments

There are exactly four comment forms (SPEC §3.4):

```python
# hash comment
// slash comment (always a comment, never an operator)
/* block comment — does not nest */
"""triple-quoted comment
spanning lines (this is a COMMENT, not a string)"""
print("only this line runs")
```

Output:

```text
only this line runs
```

> **Footgun: `//` is not floor division.** It always starts a comment, so
> `x = 7 // 2` assigns `7`. Use `/` (integer division truncates toward zero).
> Because the comment runs to the end of the line, `print(7 // 2)` on a single
> line swallows the closing parenthesis and is a syntax error. There is no
> multi-line string literal either: `"""` is a comment.

---

## 3. Values and types

cstar is dynamically typed: values carry a tag at runtime and a variable may
be reassigned to a value of another type. There are **no implicit conversions**
between categories (no int→text, no bool→int).

| Type | Examples | Notes |
|---|---|---|
| `int` | `0`, `42`, `-7` | signed 64-bit; arithmetic **wraps** (no overflow error) |
| `float` | `1.5`, `0.25` | IEEE double; digits required on both sides of the `.`; no exponent literal |
| `bool` | `True`, `False` | its own type — `True == 1` is `False` |
| `text` | `"héllo"` | UTF-8, indexed by **code point** |
| `bytes` | `b"GET /"`, `b"\x00\xff"` | raw bytes, indexed as ints 0–255 |
| `None` | `None` | the absence of a value |
| `list` | `[1, 2, 3]` | ordered, mutable, a reference type |
| `map` | `{"a": 1}` | insertion-ordered key→value, mutable |
| `set` | `{1, 2, 3}` | insertion-ordered, unique, mutable |
| `tuple` | `(1, 2)` | ordered, immutable |
| `range` | `range(0, 10, 2)` | lazy |
| `function` | `scan` | functions are values ([§9](#9-functions)) |
| `class`, `instance`, `method` | `Host`, `Host("a")`, `h.probe` | [§12](#12-classes) |

`type(x)` returns the type name as `text`. Two spellings to remember: `None`
is `"none"` from `type()` (but `None` in `print` and error messages), and the
string type is **`text`**, never `string`.

```python
print(type(1), type(1.5), type(True), type("a"), type(b"a"), type(None))
print(type([1]), type({}), type({1}), type(()), type(range(3)))
print(1, 1.5, True, "text", b"by\n", None)
print([1, "a"], {"k": [1, 2]}, (1,), (), range(3))
print()
print("float:", 5.0, 0.1 + 0.2, 1 / 3.0)
```

Output:

```text
int float bool text bytes none
list map set tuple range
1 1.5 True text b"by\x0a" None
[1, a] {k: [1, 2]} (1,) () range(0, 3, 1)

float: 5.0 0.30000000000000004 0.3333333333333333
```

`print` takes any number of arguments, joins them with one space and ends with
a newline. `text` prints raw — **without quotes, also inside collections** (use
`repr()` for the quoted form). Floats print as the shortest string that
round-trips. The full display table is SPEC §5.6.

---

## 4. Variables and assignment

Assignment creates the variable; there is no declaration keyword.

```python
host = "example.com"
port = 443
port += 1            # all eleven compound forms: += -= *= /= %= **= &= |= ^= <<= >>=
host = 8080          # reassigning to another type is fine
print(host, port)
```

Output:

```text
8080 444
```

Variables are **not hoisted**: a variable exists from program start but is
*unset* until its assignment actually executes. Reading it earlier is a
**runtime** error:

```python
print("start")
print(later)
later = 1
```

Output:

```text
start
example.cst:2: runtime error: global variable 'later' used before assignment
```

(Reading a name that is never assigned anywhere is caught at compile time:
`undefined variable 'x'`.) Runtime errors carry the source line, never a column.

---

## 5. Operators

### Arithmetic

```python
print(7 + 2, 7 - 2, 7 * 2)
print(7 / 2, -7 / 2, 7.0 / 2)     # int / int truncates toward zero
print(7 % 3, -7 % 3, 7 % -3)      # % follows the dividend's sign; ints only
print(2 ** 10, 2 ** 0.5)          # int ** int stays int; any float -> float
print(9223372036854775807 + 1)    # int arithmetic wraps (two's complement)
print("ab" + "cd", b"a" + b"b")   # + concatenates text/text and bytes/bytes
```

Output:

```text
9 5 14
3 -3 3.5
1 -1 1
1024 1.4142135623730951
-9223372036854775808
abcd b"ab"
```

Division or modulo by zero is a runtime error (`division by zero`,
`modulo by zero`). There is no string repetition and no floor division
(SPEC §5.2–§5.4).

**Precedence** (SPEC §4.2): parentheses group first, then `**` (right
associative), then unary `-`/`~`, then `*` `/` `%`, then `+` `-` — each level
left to right. `**` binds tighter than unary minus (`-2 ** 2` is `-4`):

```python
print(1 + 2 * 3, (1 + 2) * 3)     # 7 9
print(8 / 4 / 2, 6 / 2 * 3)       # same level: left to right → 1 9
print(10 - 3 - 2, -2 ** 2)        # 5 -4
```

### Comparison, equality and logic

```python
print(1 < 2, "a" < "b", 1 == 1.0, True == 1, "1" == 1)
print([1, 2] == [1, 2], {"a": 1} == {"a": 1}, None == None)
print(1 and 2, 0 or "x", not [])    # and / or / not ALWAYS return bool
print(3 if 1 > 2 else 4)            # conditional expression
```

Output:

```text
True True True False False
True True True
True True True
4
```

`==` and `!=` never raise; ordering (`<` …) is defined only for numbers and for
same-type `text`/`bytes`. `and`/`or` short-circuit but, unlike Python, return a
`bool` rather than one of their operands (SPEC §6.2).

### Bitwise operators

`& | ^ ~ << >>` work on `int` only (no bool promotion); shift counts must be in
`0..63`.

```python
print(6 & 3, 6 | 3, 6 ^ 3, ~6, 1 << 10, -16 >> 2)
```

Output:

```text
2 7 5 -7 1024 -4
```

### Membership `in` and identity `is`

`in` is cstar's membership test (the job Python's `in` does). It is also the
binder in `for` loops. The right operand may be a `range`, `text`, `bytes`,
`list`, `tuple`, `map` (tests keys) or `set`.

```python
print(2 in[1, 2, 3], "ht" in "https", "k" in {"k": 1}, 5 in range(10))
a = [1]
b = [1]
print(a == b, a is b, a is a, a is not b)
print(1 is 1, None is None, 1 is 1.0)
```

Output:

```text
True True True True
True False True True
True True False
```

`==` compares structure; `is` compares object identity (never constant-folded).
Immediate values (`int`, `float`, `bool`, `None`) are identical when their tag
and value match (SPEC §5.13).

---

## 6. Text and bytes

`text` is UTF-8 and is indexed, sliced, measured and iterated by **code point**.
`bytes` is raw: indexing yields ints. `encode()` and `decode()` convert between
them (these are builtin *functions*, not methods).

```python
t = "héllo wörld"
print(len(t), t[1], t[-1], t[0:5], t[::-1])
print(t.upper(), t.find("wörld"), t.replace("l", "L"))
print("a,b,c".split(","), "-".join(["x", "y"]), "  pad ".strip())
print("https://x".starts_with("https"), "x.cst".ends_with(".cst"))
raw = encode("héllo")
print(raw, len(raw), raw[1], decode(raw))
for c in "añ":
    print(c)
```

Output:

```text
11 é d héllo dlröw olléh
HéLLO WöRLD 6 héLLo wörLd
[a, b, c] x-y pad
True True
b"h\xc3\xa9llo" 6 195 héllo
a
ñ
```

Note `upper()`/`lower()` map **ASCII letters only** (`é` stays `é`). The complete
method list is in SPEC §5.10 and the [builtins guide](builtins.md).

---

## 7. Collections

### Lists

```python
hosts = ["web1", "web2"]
hosts.append("web3")
hosts.insert(0, "gw")
print(hosts, len(hosts), hosts[0], hosts[-1], hosts[1:3])
print(hosts.pop(), hosts.index("web1"))
hosts[0] = "edge"
hosts.reverse()
print(hosts)
alias = hosts            # lists are references: no copy is made
alias.append("x")
print(hosts)
```

Output:

```text
[gw, web1, web2, web3] 4 gw web3 [web1, web2]
web3 1
[web2, web1, edge]
[web2, web1, edge, x]
```

Mutating methods (`append`, `insert`, `reverse`, `clear`, …) return `None`.
Indexing is Python-style (negative from the end); slices always produce a **new**
object. Indexing out of range is a runtime error (`list index out of range`).

### Maps

```python
ports = {"http": 80, "https": 443}
ports["ssh"] = 22
print(ports, ports["https"], ports.get("ftp"), ports.get("ftp", 21))
print(ports.keys(), ports.values(), ports.items())
print(ports.pop("ssh"), len(ports), "http" in ports)
for name in ports:          # iterating a map yields its KEYS
    print(name, ports[name])
```

Output:

```text
{http: 80, https: 443, ssh: 22} 443 None 21
[http, https, ssh] [80, 443, 22] [(http, 80), (https, 443), (ssh, 22)]
22 2 True
http 80
https 443
```

`{}` is an empty **map**; the only empty-set literal is `set()`. `1` and `1.0`
are the same key; `True` is a different key from `1`. Unhashable keys
(`list`, `map`, `set`, instances …) raise `unhashable type: '<t>'`.

### Sets and tuples

```python
s = {3, 1, 3, 2}
s.add(9)
s.discard(1)
print(s, len(s), 2 in s, set([1, 1, 2]), type(set()))
t = (1, "a", 2.5)
print(t, t[1], len(t), t[1:])
```

Output:

```text
{3, 2, 9} 3 True {1, 2} set
(1, a, 2.5) a 3 (a, 2.5)
```

### Destructuring — and the comma-join trap

```python
first, *rest = [10, 20, 30]
print(first, rest)
a, (b, c) = (1, (2, 3))
print(a, b, c)
*init, last = "xyz"
print(init, last)
x, y = "ab"                    # any iterable can be unpacked
print(x, y)
pair = ("port", 443)           # parenthesised: a tuple
print(pair, type(pair))
label = "port", 443            # UNparenthesised comma: joined TEXT
print(label, type(label))
```

Output:

```text
10 [20, 30]
1 2 3
[x, y] z
a b
(port, 443) tuple
port 443 text
```

> **Comma-join.** An unparenthesised comma list at a full-expression position
> (the right-hand side of `=`, a `return` value) **joins the display forms of
> its items with a single space into `text`** — it does *not* build a tuple.
> Write `(a, b)` when you want a tuple. This is deliberate (it makes
> `return name, port` a handy one-line formatter) but it surprises Python
> programmers; the same rule is why `a, (b, c) = 1, (2, 3)` fails with
> `too many values to unpack (expected 2, got 8)` — the right side became the
> 8-character text `1 (2, 3)`. See SPEC §4.4.

---

## 8. Control flow

### `if` / `elif` / `else`

```python
n = 7
if n < 5:
    print("small")
elif n < 10:
    print("medium")
else:
    print("large")
if n > 0: print("positive")       # single-line suite
```

Output:

```text
medium
positive
```

Conditions use **truthiness**, which is total (it never raises): `False`,
`None`, `0`, `0.0`, empty `text`/`bytes`, and empty collections and ranges are
falsy; everything else is truthy (SPEC §6.1).

### `while`, `for`, `break`, `continue`, `pass`

`for` binds **one** name with `in`. It iterates any iterable:
`range`, `text` (by code point), `bytes` (ints), lists, maps (keys), sets,
tuples.

```python
i = 0
while i < 10:
    i += 1
    if i % 2 == 0:
        continue
    if i > 7:
        break
    print(i)
for x in range(3):
    print(x)
for x in range(10, 0, -4):
    print(x)
for ch in "hi":
    print(ch)
for item in [1, "b"]:
    pass
print("done")
```

Output:

```text
1
3
5
7
0
1
2
10
6
2
h
i
done
```

`range(stop)`, `range(start, stop)` and `range(start, stop, step)` take
**ints only**, the stop is exclusive and the step must be non-zero. Mutating a
collection while iterating over it is undefined.

### `switch` / `case`

Labels are constant literals only; the first match wins; there is no
fall-through; `case *:` is an optional wildcard that must come last.
`break` inside a `switch` is an error — `break` only ever means "leave the
loop" (SPEC §6.7–§6.8).

```python
port = 443
switch port:
    case 80:
        print("http")
    case 443:
        print("https")
    case *:
        print("other")
switch "ssh":
    case "ssh":
        print("22")
    case "ftp":
        print("21")
```

Output:

```text
https
22
```

---

## 9. Functions

Functions are declared with `name():` (no parameters) or
`name(p1, p2 = default):` headers. Parameters may have defaults, a trailing `*args` collects extra positionals into a list and
`**opts` collects unknown named arguments into a map. Call sites may use named
arguments.

```python
add(a, b = 10, *more, **opts):
    print(a, b, more, opts)
    return a + b

print(add(1))
print(add(1, 2, 3, 4, x = 5))
print(add(b = 1, a = 2))
```

Output:

```text
1 10 [] {}
11
1 2 [3, 4] {x: 5}
3
2 1 [] {}
3
```

Defaults are evaluated **at call time**, freshly per call, so a mutable default
never leaks state between calls. A function with no `return` returns `None`.
Arity and named-argument errors are **compile-time** errors whenever the callee
is known statically (`function 'add' expects 2 argument(s), got 1`).

### Functions are values, closures capture by reference

```python
make_counter():
    n = 0
    bump():
        nonlocal n
        n += 1
        return n
    return bump

c = make_counter()
print(c(), c(), c())
fns = [make_counter(), make_counter()]
print(fns[0](), fns[0](), fns[1]())
twice(f, x):
    return f(f(x))
inc(v):
    return v + 1
print(twice(inc, 5))
```

Output:

```text
1 2 3
1 2 1
7
```

There are no lambdas: pass a named function declaration (`name(…):`).
**Builtins are not values** —
`map(str, xs)` or `f = print` is a compile error (`undefined variable 'str'`)
because every builtin can only appear as a call target (SPEC §5.11.1).

### Forward references and recursion

Declarations are discovered before anything runs, so a function may be called
above its definition, and mutual recursion just works.

```python
print(fact(10))
fact(n):
    if n <= 1:
        return 1
    return n * fact(n - 1)
```

Output:

```text
3628800
```

Very deep recursion can overflow the native C stack (there is no guaranteed
catchable error — SPEC §12).

---

## 10. The execution model: scripts and `main:`

Seven rules (SPEC §7) explain everything:

1. Function and class names are discovered before execution, so forward
   references work.
2. Top-level statements run in source order.
3. A function declaration (`name():` / `name(params):`) never runs its body;
   only a call does.
4. A file without `main:` is just a script.
5. Only *called* function bodies run.
6. `main:` is not a function: it is a block that runs **in place**, at its
   textual position, in global scope — then top-level execution continues.
7. Variables are not hoisted; assignments happen in execution order.

```python
print("1: top level")

main:
    print("2: inside main")
    helper()

print("4: after main")

helper():
    print("3: helper")

never_called():
    print("never printed")
```

Output:

```text
1: top level
2: inside main
3: helper
4: after main
```

`main` is a keyword: it cannot be declared as a function and `main()` is a
syntax error, there can be at most one `main:` block, and `return` is not
allowed inside it.

### Modules and imports

Since the Stage 2D work (unreleased in this tree; SPEC §14) a program may be
split across files:

```python
import lib.toolkit as tk          # binds the module:  tk.greet("x")
from lib.toolkit import greet     # binds one declaration:  greet("x")
from lib.toolkit import greet as hello
```

A dotted specifier resolves relative to the importing file first, then the
working directory, then `$CSTAR_PATH` (and the legacy `$PENTEST_PATH`);
`lib/toolkit` loads `lib/toolkit.cst` (or a legacy `.pen`/`.pent`) or
`lib/toolkit/__init__.cst`. The first import runs
the module's top-level code once, at the import's position; an imported
module's `main:` is ignored. `run`/`build` compile only the declarations the
program uses, so an unused (even broken) imported function costs nothing —
`check` is strict and validates it anyway. Full guide:
[modules and imports](modules.md).

---

## 11. Scope: `global`, `nonlocal` and `variable.`

* Top level and `main:` **share the global scope**.
* An assignment inside a function creates a **local**; reading a name that is
  not local falls back to the enclosing function(s), then to globals.
* To *rebind* a global from a function, declare `global name` first; to rebind
  an enclosing function's variable, declare `nonlocal name`.

```python
counter = 0
bump():
    global counter
    counter += 1
bump()
bump()
print(counter)
```

Output:

```text
2
```

### One name, two namespaces: variables and functions

Because calls use `f(…)` while reads use the bare name `f`, a global
**variable** and a global **function** may share a name. Usage decides
which one a site means: a bare read takes the variable, a call takes the
function.

```python
a = 10
a():
    return 20

print(a)
print(a())

# `global` redirects the variable side; the call still reaches the function
swap():
    global a
    a = 99
    print(a, a())
swap()
print(a)
```

Output:

```text
10
20
99 20
99
```

A `class` name is the exception — it is exclusive: classes are constructed
with the same `()` syntax, so a same-named function would shadow the
constructor (`name 'C' is already declared as a function`). Same for a
nested declaration colliding with a parameter or local of its enclosing
function.

### Cross-scope access with `variable.`

Since `0.2.1` a function can reach a binding that normal resolution would not
find, *without* a `global`/`nonlocal` declaration, via
`variable.<qualifier> <name>`:

| Form | Reaches |
|---|---|
| `variable.global a` | the module global `a` |
| `variable.parent a` | the nearest enclosing function's `a` |
| `variable.<fn> a` | a parameter or local of a sibling function `<fn>` (declared anywhere in the file) |

Each form works as a read, a plain or compound assignment target, or a
destructuring leaf. There is exactly **one storage per binding** — the owner
and every qualified site see the same value.

```python
target = 1

reader():
    print("global:", variable.global target)
    variable.global target = 2

box():
    counter = 0
    bump():
        variable.parent counter += 1
    bump()
    bump()
    print("shared:", counter)

sib():
    print("sibling:", variable.box counter)
    variable.box counter = 99

main:
    reader()
    print(target)
    box()
    sib()
    print(variable.box counter)
```

Output:

```text
global: 1
2
shared: 2
sibling: 2
99
```

`variable` is not a keyword; only the exact four-token shape
`variable <qualifier> <name>` is special. Full rules, including pinned storage
and what is not supported, are in
[cross-scope access](../reference/cross-scope-access.md).

---

## 12. Classes

A class is a block of methods. Methods take an explicit `self`; `__init__` is
the constructor; attributes are **dynamic** — created by assignment on an
instance. Calling a method accepts named arguments, and `obj.method` without a
call yields a bound method value.

```python
class Host:
    __init__(self, name, port = 22):
        self.name = name
        self.port = port
        self.up = False

    probe(self, open_port):
        self.up = self.port == open_port
        return self

    describe(self):
        return self.name, self.port, self.up

main:
    h = Host("web1", 443)
    print(h.probe(open_port = 443).describe())
    print(h, type(h), h.probe)
    h.note = "dynamic attribute"
    print(h.note)
    g = h
    print(g is h, Host("web1", 443) is h)
```

Output:

```text
web1 443 True
<Host instance> Host <method 'probe' of Host>
dynamic attribute
True False
```

What classes do **not** have (SPEC §5.12, §12): inheritance, class attributes,
decorators/properties, user-defined exception types, and instances as map keys
(they are unhashable). Classes must be declared at top level (or directly in
`main:`), never inside a function. Instances compare by identity.

---

## 13. Exceptions

`try` / `except [Class] [as name]` / `finally` and `raise`. **Every runtime
error is catchable**: it is raised as an instance of one of twelve builtin
classes (`Exception`, `RuntimeError`, `TypeError`, `ValueError`, `NameError`,
`ZeroDivisionError`, `IndexError`, `KeyError`, `AttributeError`,
`StopIteration`, `FileNotFoundError`, `OSError`) with the same message you
would see uncaught.

```python
safe_div(a, b):
    try:
        return a / b
    except ZeroDivisionError as e:
        print("caught:", e)
        return None
    finally:
        print("checked", a, b)

print(safe_div(6, 3))
print(safe_div(1, 0))

try:
    xs = [1]
    print(xs[3])
except IndexError as e:
    print(type(e), e.type, "|", e.message)

try:
    raise RuntimeError("custom failure")
except TypeError:
    print("not me")
except:
    print("bare except catches the rest")

try:
    try:
        raise ValueError("inner")
    except ValueError as e:
        raise e                     # there is no bare `raise`; re-raise the binding
except ValueError as e2:
    print("re-raised:", e2)

raise KeyError("fatal")
```

Output:

```text
checked 6 3
2
caught: ZeroDivisionError: division by zero
checked 1 0
None
IndexError IndexError | list index out of range
bare except catches the rest
re-raised: ValueError: inner
example.cst:34: runtime error: fatal
```

Rules worth knowing (SPEC §6.10):

* **Matching is by exact class** — there is no inheritance. `except TypeError:`
  does not catch a `ValueError`, and **`except Exception:` does not catch
  everything** (it only catches exceptions raised as exactly `Exception`).
  A bare `except:` is the catch-all.
* `raise` takes an exception instance or a builtin class (empty message);
  user classes cannot be raised.
* `finally` runs exactly once on every path out, including `return`, `break`
  and `continue`.
* An exception that nothing catches prints
  `<file>:<line>: runtime error: <message>` to stderr and exits `1` — the class
  name is not part of the uncaught output.

> **Exceptions per contract.** Multi-clause `except`, catchable
> `int()`/`float()` parse errors and `finally` on every unwind edge behave as
> SPEC §6.10 and §9.3 describe; the regression cases are `try_multi_named`,
> `bi_py_parse_catch` and `rt_bi_parse_finally`.

---

## 14. Builtins

Seventy-eight names are reserved as builtins. All of them are **call-only**: they can
be called but never read as values, assigned, redeclared, or used as parameter
or loop names. Arity and named arguments are checked at compile time.

| Group | Names |
|---|---|
| Core | `print range len str int float bool type encode decode set collect` |
| Environment | `input time clock` |
| Network (unreleased) | `http_get http_post url_encode` |
| SIMD vectors (unreleased) | `fvec ivec fvec_fill ivec_fill dot` |
| Python-parity (43) | `abs all any ascii bin bytes callable chr delattr dict dir divmod enumerate filter format getattr globals hasattr hash hex id isinstance iter list locals map max min next oct open ord pow repr reversed round setattr slice sorted sum tuple vars zip` |
| Always-failing stubs (9) | `super property classmethod staticmethod issubclass breakpoint help memoryview object` |
| Rejected at compile time (3) | `eval exec compile` |

The [builtins guide](builtins.md) has a reference table with one line per
builtin and the documented deviations from Python.

---

## 15. Memory management

Heap objects (`text`/`bytes` buffers, collections, functions, closures,
classes, instances) are reclaimed by a non-moving mark-and-sweep garbage
collector, including reference cycles. You never free anything. `collect()`
forces a collection and returns how many blocks were freed — it exists for
tests and diagnostics, and programs must not depend on *when* collection
happens.

```python
drop(n):
    xs = []
    for i in range(n):
        xs.append([i, i])
    return len(xs)
print(drop(1000))
print(collect() >= 0)
```

Output:

```text
1000
True
```

Implementation details are in [runtime internals](../internals/runtime.md).

---

## 16. What cstar deliberately does not have

Beyond the released single-file core, the unreleased Stage 2D work adds
imports ([§10](#modules-and-imports)); everything else below is still absent:
no inheritance, no generators, no lambdas, no string repetition (`"a" * 3`), no
floor division (`//` is a comment), no multi-line strings, no type annotations,
no `eval`/`exec`, and no standard-library or networking modules yet. It targets
Linux x86_64 only. The full ledger is SPEC §12, and the roadmap is in
[history](../history/README.md).

Next: the [CLI reference](cli.md), the [builtins guide](builtins.md), and
[errors and diagnostics](errors-and-diagnostics.md).
