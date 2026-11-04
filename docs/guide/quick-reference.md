# Quick reference

Every feature of the language on one page: how to declare variables, declare
and call functions, use collections, classes, exceptions, imports, and every
builtin. It covers version `0.2.1` plus the unreleased work in this tree
(imports, HTTP builtins and SIMD vectors).

> **Name.** The language is being renamed **cstar**. Until that change lands, the
> command is `pentest` and source files end in `.pen` (or `.pent`).

How to read the examples: a `# →` comment shows what the line prints. The
outputs come from the executed examples in the [language tour](language-tour.md),
the [builtins guide](builtins.md) and [gotchas](gotchas.md). Those pages explain
each feature in more depth, and [`SPEC.md`](../../SPEC.md) has the exact rules.

**Contents**

1. [Run a program](#1-run-a-program)
2. [Program shape](#2-program-shape)
3. [Comments](#3-comments)
4. [Variables](#4-variables)
5. [Types](#5-types)
6. [Operators](#6-operators)
7. [Text and bytes](#7-text-and-bytes)
8. [Collections](#8-collections)
9. [Control flow](#9-control-flow)
10. [Functions](#10-functions)
11. [Scope](#11-scope)
12. [Classes](#12-classes)
13. [Exceptions](#13-exceptions)
14. [Modules and imports](#14-modules-and-imports)
15. [Input, output, files and HTTP](#15-input-output-files-and-http)
16. [All builtins](#16-all-builtins)
17. [Methods on builtin values](#17-methods-on-builtin-values)
18. [SIMD vectors](#18-simd-vectors)
19. [Memory](#19-memory)
20. [Not available yet](#20-not-available-yet)
21. [Differences from Python](#21-differences-from-python)

---

## 1. Run a program

```bash
build/pentest run hello.pen        # compile (cached) and run
build/pentest hello.pen            # the same as run
build/pentest build hello.pen      # make ./hello.pentest   (-o NAME to choose the name)
build/pentest check hello.pen      # only look for errors; prints nothing when the file is fine
```

Exit codes: `0` success, `1` compile or runtime error, `2` wrong command usage.
More in the [CLI reference](cli.md).

---

## 2. Program shape

There is no required structure: no `void main`, no class wrapper, no imports to
start. A file is a **script**, and its statements run from top to bottom.

```python
print("Hello, Pentest!")           # → Hello, Pentest!
```

* **Blocks** start with a colon `:` and are indented with **spaces**. A tab in
  the indentation is a compile error.
* **One statement per line.** There are no semicolons. A statement can continue
  onto the next line only inside `( )`.
* **An empty file** is a valid program that prints nothing.
* **`main:` is optional.** It is a block, not a function. It runs *in place* at
  its position in the file, then the code after it keeps running.

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

# prints:
# 1: top level
# 2: inside main
# 3: helper
# 4: after main
```

The seven execution rules:

1. Function and class names are known before anything runs, so you can call a
   function above its definition.
2. Top-level statements run in source order.
3. Declaring a function never runs its body; only a call does.
4. A file without `main:` is just a script.
5. Only the bodies of called functions run.
6. `main:` runs in place, in the global scope. There can be at most one, you
   can't `return` from it, and `main()` is a syntax error.
7. Variables are not hoisted; assignments happen in execution order.

---

## 3. Comments

There are exactly four forms:

```python
# hash comment
// slash comment (always a comment, never an operator)
/* block comment — does not nest */
"""triple-quoted comment
spanning lines (this is a COMMENT, not a string)"""
print("only this line runs")       # → only this line runs
```

> **Watch out:** `//` is never floor division. `x = 7 // 2` assigns `7`, and
> everything after `//` is ignored. `"""` is also a comment, so there are no
> multi-line strings.

---

## 4. Variables

Assigning to a name creates the variable. There is no `var`, `let` or type
keyword.

```python
host = "example.com"
port = 443
port += 1            # += -= *= /= %= **= &= |= ^= <<= >>=
host = 8080          # a variable may change to another type
print(host, port)    # → 8080 444
```

* **Reading a variable before its assignment runs** is a runtime error:
  `global variable 'later' used before assignment`.
* **Reading a name that is never assigned anywhere** is a compile error:
  `undefined variable 'x'`.
* **Reserved names:** builtin names such as `sum`, `len`, `list`, `max`, `id`,
  `format`, `input` and `time` can't be used as variable, function or parameter
  names ([all builtins](#16-all-builtins)).
* **Several variables at once:** use destructuring
  ([section 8](#8-collections)), for example `a, b = [1, 2]`.

---

## 5. Types

Values carry their type at runtime, and a variable can hold any type. There are
**no automatic conversions**: `"a" + 1` is an error. Convert explicitly with
`str()`, `int()`, `float()` or `bool()`.

| Type | Examples | Notes |
|---|---|---|
| `int` | `0`, `42`, `-7` | signed 64-bit; overflow **wraps** around |
| `float` | `1.5`, `0.25` | 64-bit double; needs digits on both sides of `.`; no `1e5` form |
| `bool` | `True`, `False` | its own type: `True == 1` is `False` |
| `text` | `"héllo"` | UTF-8, indexed by **character** (code point) |
| `bytes` | `b"GET /"`, `b"\x00\xff"` | raw bytes; indexing gives ints 0–255 |
| `None` | `None` | no value |
| `list` | `[1, 2, 3]` | ordered, changeable |
| `map` | `{"a": 1}` | key → value, keeps insertion order |
| `set` | `{1, 2, 3}` | unique values, keeps insertion order |
| `tuple` | `(1, 2)` | ordered, cannot change |
| `range` | `range(0, 10, 2)` | lazy sequence of ints |
| `function` | `scan` | functions are values |
| `class`, `instance`, `method` | `Host`, `Host("a")`, `h.probe` | see [Classes](#12-classes) |
| `fvec`, `ivec` | `fvec([1.0, 2.0])` | SIMD vectors, see [section 18](#18-simd-vectors) |
| `file` | `open("x.txt")` | see [section 15](#15-input-output-files-and-http) |

```python
print(type(1), type(1.5), type(True), type("a"), type(b"a"), type(None))
# → int float bool text bytes none
print(type([1]), type({}), type({1}), type(()), type(range(3)))
# → list map set tuple range
print("float:", 5.0, 0.1 + 0.2, 1 / 3.0)
# → float: 5.0 0.30000000000000004 0.3333333333333333
```

`type()` returns the name as `text`. `None` is `"none"` from `type()`, but it
prints as `None`.

---

## 6. Operators

| Kind | Operators | Notes |
|---|---|---|
| Arithmetic | `+ - * / % **` | `int / int` truncates toward zero; `%` works on ints only and follows the left operand's sign |
| Joining | `+` | text + text and bytes + bytes only |
| Comparison | `== != < <= > >=` | `==` never fails; ordering only between numbers, or text/bytes of the same type |
| Logic | `and or not` | short-circuit, **always return `bool`** |
| Bitwise | `& \| ^ ~ << >>` | ints only; shift count 0–63 |
| Membership | `in` | in a range, text, bytes, list, tuple, map (keys) or set; there is no `not in`, so write `not (x in c)` |
| Identity | `is`, `is not` | same object |
| Conditional | `a if cond else b` | the only "pick a value" form |

```python
print(7 + 2, 7 - 2, 7 * 2)          # → 9 5 14
print(7 / 2, -7 / 2, 7.0 / 2)       # → 3 -3 3.5
print(7 % 3, -7 % 3, 7 % -3)        # → 1 -1 1
print(2 ** 10, 2 ** 0.5)            # → 1024 1.4142135623730951
print(9223372036854775807 + 1)      # → -9223372036854775808   (wraps)
print("ab" + "cd", b"a" + b"b")     # → abcd b"ab"
print(1 < 2, "a" < "b", 1 == 1.0, True == 1, "1" == 1)
# → True True True False False
print(1 and 2, 0 or "x", not [])    # → True True True
print(3 if 1 > 2 else 4)            # → 4
print(6 & 3, 6 | 3, 6 ^ 3, ~6, 1 << 10, -16 >> 2)
# → 2 7 5 -7 1024 -4
print(2 in [1, 2, 3], "ht" in "https", "k" in {"k": 1}, 5 in range(10))
# → True True True True
a = [1]
b = [1]
print(a == b, a is b, a is a, a is not b)
# → True False True True
```

**Precedence**, from tightest to loosest:

1. `( )`, calls, indexing
2. `**` (right to left)
3. unary `-` `~`
4. `* / %`
5. `+ -`
6. `<< >>`
7. `&`
8. `^`
9. `|`
10. comparisons, `in`, `is`
11. `not`
12. `and`
13. `or`
14. `a if c else b`

So `-2 ** 2` is `-4`, and `not a == b` means `not (a == b)`. Comparisons don't
chain: write `0 < x and x < 10`, not `0 < x < 10`.

Dividing by zero raises `division by zero` (or `modulo by zero`).

---

## 7. Text and bytes

`text` is UTF-8 and counts **characters**, not bytes. `bytes` is raw data.

```python
t = "héllo wörld"
print(len(t), t[1], t[-1], t[0:5], t[::-1])
# → 11 é d héllo dlröw olléh
print(t.upper(), t.find("wörld"), t.replace("l", "L"))
# → HéLLO WöRLD 6 héLLo wörLd
print("a,b,c".split(","), "-".join(["x", "y"]), "  pad ".strip())
# → [a, b, c] x-y pad
print("https://x".starts_with("https"), "x.pen".ends_with(".pen"))
# → True True
raw = encode("héllo")               # text → bytes
print(raw, len(raw), raw[1], decode(raw))
# → b"h\xc3\xa9llo" 6 195 héllo
```

**Building text.** There are no f-strings, no `%` formatting and no `"ab" * 3`
repetition. Use `+` with `str()`, use `format()`, or join with commas:

```python
host = "web1"
port = 443
print("host=" + host + " port=" + str(port))   # → host=web1 port=443
print(format(port, "06d"), format(0.5, ".1%"))  # → 000443 50.0%
label = "port", 443                            # unparenthesised comma → joined text
print(label)                                   # → port 443
```

* **Indexing:** `t[i]`, with `t[-1]` for the last character.
* **Slicing:** `t[start:stop:step]` always makes a new value.
* **Methods** are listed in [section 17](#17-methods-on-builtin-values).
* **`upper()`/`lower()`** change ASCII letters only (`é` stays `é`).
* **`print`** shows text without quotes, even inside lists. Use `repr(x)` for
  the quoted form.

---

## 8. Collections

### List

```python
hosts = ["web1", "web2"]
hosts.append("web3")
hosts.insert(0, "gw")
print(hosts, len(hosts), hosts[0], hosts[-1], hosts[1:3])
# → [gw, web1, web2, web3] 4 gw web3 [web1, web2]
print(hosts.pop(), hosts.index("web1"))     # → web3 1
hosts[0] = "edge"
hosts.reverse()
print(hosts)                                # → [web2, web1, edge]
alias = hosts            # lists are shared by reference: no copy is made
alias.append("x")
print(hosts)                                # → [web2, web1, edge, x]
```

### Map

```python
ports = {"http": 80, "https": 443}
ports["ssh"] = 22
print(ports, ports["https"], ports.get("ftp"), ports.get("ftp", 21))
# → {http: 80, https: 443, ssh: 22} 443 None 21
print(ports.keys(), ports.values(), ports.items())
# → [http, https, ssh] [80, 443, 22] [(http, 80), (https, 443), (ssh, 22)]
print(ports.pop("ssh"), len(ports), "http" in ports)
# → 22 2 True
for name in ports:          # looping over a map gives its KEYS
    print(name, ports[name])
# → http 80
# → https 443
```

### Set and tuple

```python
s = {3, 1, 3, 2}
s.add(9)
s.discard(1)
print(s, len(s), 2 in s, set([1, 1, 2]), type(set()))
# → {3, 2, 9} 3 True {1, 2} set
t = (1, "a", 2.5)
print(t, t[1], len(t), t[1:])
# → (1, a, 2.5) a 3 (a, 2.5)
```

* `{}` is an empty **map**. An empty set is `set()`.
* Lists, maps, sets and instances can't be map keys or set members.
* Mutating methods (`append`, `insert`, `reverse`, `clear` …) return `None`.
* An index past the end is a runtime error (`list index out of range`).

### Destructuring

```python
first, *rest = [10, 20, 30]
print(first, rest)              # → 10 [20, 30]
a, (b, c) = (1, (2, 3))
print(a, b, c)                  # → 1 2 3
*init, last = "xyz"
print(init, last)               # → [x, y] z
x, y = "ab"
print(x, y)                     # → a b
pair = ("port", 443)            # with parentheses: a tuple
print(pair, type(pair))         # → (port, 443) tuple
label = "port", 443             # without parentheses: joined TEXT
print(label, type(label))       # → port 443 text
```

> **Comma-join.** `a, b` without parentheses on the right of `=` or after
> `return` makes **text** (the items joined with a space), not a tuple. Write
> `(a, b)` when you want a tuple.

---

## 9. Control flow

### if / elif / else

```python
n = 7
if n < 5:
    print("small")
elif n < 10:
    print("medium")             # → medium
else:
    print("large")
if n > 0: print("positive")     # one-line form  → positive
```

**False values:** `False`, `None`, `0`, `0.0`, empty text or bytes, and empty
collections and ranges. Everything else counts as true.

### while, for, break, continue, pass

```python
i = 0
while i < 10:
    i += 1
    if i % 2 == 0:
        continue                # skip to the next round
    if i > 7:
        break                   # leave the loop
    print(i)                    # → 1, 3, 5, 7 (one per line)
for x in range(3):
    print(x)                    # → 0, 1, 2
for x in range(10, 0, -4):
    print(x)                    # → 10, 6, 2
for ch in "hi":
    print(ch)                   # → h, i
for item in [1, "b"]:
    pass                        # do nothing
```

* **`for` takes exactly one name.** `for a, b in pairs:` is a syntax error, so
  destructure inside the loop instead:

  ```python
  ports = {"http": 80, "https": 443}
  for item in ports.items():
      name, port = item
      print(name, port)
  ```
* **`range(stop)`, `range(start, stop)`, `range(start, stop, step)`** take ints
  only. `stop` is not included, and `step` can't be `0`.
* **Changing a collection while looping over it** is undefined.

### switch / case

```python
port = 443
switch port:
    case 80:
        print("http")
    case 443:
        print("https")          # → https
    case *:                     # wildcard, must be last
        print("other")
```

* Labels must be constant literals.
* The first match wins, and there is no fall-through.
* `break` inside a `switch` is an error, because `break` only ever leaves a
  loop.

---

## 10. Functions

### Declare and call

A function is declared as `name(parameters):`, with no `def`, no `void` and no
return type. Call it with `name(arguments)`.

```python
hello():                        # no parameters
    print("hello")

greet(name):                    # one parameter
    print("hi", name)

add(a, b):
    return a + b                # send a value back

hello()                         # → hello
greet("bob")                    # → hi bob
print(add(2, 3))                # → 5
```

* **No `return`:** a function without a `return` gives back `None`.
* **Comma-join:** `return name, port` returns *text* such as `"web1 443"`. Use
  `return (name, port)` for a tuple.
* **Order doesn't matter:** you can call a function above the place where it's
  declared.

### Defaults, named arguments, *args, **opts

```python
add(a, b = 10, *more, **opts):
    print(a, b, more, opts)
    return a + b

print(add(1))                   # → 1 10 [] {}      then  11
print(add(1, 2, 3, 4, x = 5))   # → 1 2 [3, 4] {x: 5}  then  3
print(add(b = 1, a = 2))        # → 2 1 [] {}       then  3
```

| Form | Meaning |
|---|---|
| `b = 10` | default value, **evaluated fresh at every call** |
| `*more` | extra positional arguments, collected into a **list** |
| `**opts` | unknown named arguments, collected into a **map** |
| `f(a = 2)` | named argument at the call site |

**Wrong argument counts are compile errors** when the compiler knows the
function, for example `function 'add' expects 2 argument(s), got 1`.

### Functions are values, and closures

```python
make_counter():
    n = 0
    bump():                     # a nested function
        nonlocal n              # change the outer function's n
        n += 1
        return n
    return bump                 # return the function itself

c = make_counter()
print(c(), c(), c())            # → 1 2 3

twice(f, x):                    # take a function as a parameter
    return f(f(x))
inc(v):
    return v + 1
print(twice(inc, 5))            # → 7
```

* **Closures** capture variables by reference: they see later changes.
* **Nested functions** are created when the enclosing function starts, even if
  they're declared inside an `if`.
* **There are no lambdas.** Declare a small named function and pass it.
* **Builtins are not values.** `f = print` or `map(str, xs)` is a compile
  error. Wrap the builtin in your own function:

```python
by_len(s):
    return len(s)

words = ["pear", "fig", "banana"]
print(sorted(words, key=by_len))   # → [fig, pear, banana]
```

### Recursion

```python
print(fact(10))                 # → 3628800  (called before its declaration)
fact(n):
    if n <= 1:
        return 1
    return n * fact(n - 1)
```

Very deep recursion can overflow the C stack, and that can't be caught.

---

## 11. Scope

* **Top-level code and `main:` share one global scope.**
* **Assigning inside a function creates a local variable.**
* **Name lookup:** reading a name looks in the function first, then the
  enclosing functions, then the globals.
* **Changing an outer variable:** use `global name` to change a global, or
  `nonlocal name` to change an enclosing function's variable.

```python
counter = 0
bump():
    global counter
    counter += 1
bump()
bump()
print(counter)                  # → 2
```

### Cross-scope access with variable.

Reach a variable without declaring `global`/`nonlocal`:

| Form | Reaches |
|---|---|
| `variable.global a` | the global `a` |
| `variable.parent a` | the enclosing function's `a` |
| `variable.<fn> a` | the parameter or local `a` of another function `<fn>` |

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
    reader()                    # → global: 1
    print(target)               # → 2
    box()                       # → shared: 2
    sib()                       # → sibling: 2
    print(variable.box counter) # → 99
```

Full rules: [cross-scope access](../reference/cross-scope-access.md).

### A variable and a function may share a name

A bare name reads the variable, and a call reaches the function:

```python
a = 10
a():
    return 20
print(a)                        # → 10
print(a())                      # → 20
```

Class names are the exception: a class can't share its name with a function.

---

## 12. Classes

```python
class Host:
    __init__(self, name, port = 22):      # constructor
        self.name = name                  # attributes are created by assignment
        self.port = port
        self.up = False

    probe(self, open_port):               # a method: self comes first
        self.up = self.port == open_port
        return self

    describe(self):
        return self.name, self.port, self.up

main:
    h = Host("web1", 443)                 # create an instance
    print(h.probe(open_port = 443).describe())   # → web1 443 True
    print(h, type(h), h.probe)
    # → <Host instance> Host <method 'probe' of Host>
    h.note = "dynamic attribute"          # add an attribute at any time
    print(h.note)                         # → dynamic attribute
    g = h
    print(g is h, Host("web1", 443) is h) # → True False
```

* **Methods** take an explicit `self` and accept named arguments.
* **`obj.method` without calling it** gives a bound method value.
* **Where classes go:** declare a class at the top level or directly in
  `main:`, never inside a function.
* **Comparison:** instances are equal only when they are the same object.
* **Not supported yet:** inheritance, `super()`, class attributes,
  decorators/properties, operator overloading, raising your own classes as
  exceptions, and instances as map keys.

---

## 13. Exceptions

```python
safe_div(a, b):
    try:
        return a / b
    except ZeroDivisionError as e:
        print("caught:", e)
        return None
    finally:
        print("checked", a, b)            # runs on every way out

print(safe_div(6, 3))     # → checked 6 3   then  2
print(safe_div(1, 0))     # → caught: ZeroDivisionError: division by zero
                          #   checked 1 0   then  None
try:
    xs = [1]
    print(xs[3])
except IndexError as e:
    print(type(e), e.type, "|", e.message)
# → IndexError IndexError | list index out of range

try:
    raise RuntimeError("custom failure")
except TypeError:
    print("not me")
except:
    print("bare except catches the rest")  # → bare except catches the rest
```

* **Every runtime error can be caught.** Each is raised as an instance of one
  of 12 classes: `Exception`, `RuntimeError`, `TypeError`, `ValueError`,
  `NameError`, `ZeroDivisionError`, `IndexError`, `KeyError`, `AttributeError`,
  `StopIteration`, `FileNotFoundError`, `OSError`.
* **Matching is by exact class.** `except Exception:` does **not** catch
  everything. Only a bare `except:` does, and it can't bind the error to a name.
* **Raising:** `raise` takes an instance (`RuntimeError("why")`) or a builtin
  class. To re-raise, write `raise e`; there is no bare `raise`.
* **Fields:** instances have `.type` and `.message`.
* **Uncaught errors** print `file:line: runtime error: message` and exit with
  code `1`.

---

## 14. Modules and imports

```python
import lib.toolkit as tk          # use it as:  tk.greet("x")
from lib.toolkit import greet     # use it as:  greet("x")
from lib.toolkit import greet as hello
```

* **File lookup:** `lib.toolkit` loads `lib/toolkit.pen` (or `.pent`), or
  `lib/toolkit/__init__.pen`.
* **Search order:** the importing file's folder, then the current folder, then
  each folder in `$PENTEST_PATH`.
* **Bound name:** `import lib.toolkit` binds **`toolkit`**, the last part of the
  path, not `lib`.
* **Imported names are read-only:** after `from m import x`, assigning to `x`
  is an error.
* **Private names:** names that start with `_` can't be imported.
* **Module code:** a module's top-level code runs once, at the first import. Its
  `main:` is ignored.
* **Imports go at the top level only**, never inside a function.
* **Unused code costs nothing:** `run` and `build` compile only the imported
  functions you use. `check` validates all of them.

Full guide: [modules and imports](modules.md).

---

## 15. Input, output, files and HTTP

### print and input

```python
print("a", 1, True)               # → a 1 True   (joined with spaces, then a newline)
print()                           # an empty line
name = input("name? ")            # one line from the keyboard, without the newline
if name is None:                  # None means end of input
    print("no input")
```

Command-line arguments (`pentest run prog.pen a b`) reach the program, but no
builtin can read them yet.

### Files

```python
f = open("scan.log", "w")
f.write("host1 up\n")
f.writelines(["host2 down\n", "host3 up\n"])
f.close()
for line in open("scan.log"):     # a file loops line by line
    print(line.strip())
# → host1 up
# → host2 down
# → host3 up
f = open("scan.log")
print(f.readline().strip(), len(f.readlines()))   # → host1 up 2
f.close()
try:
    open("missing.log")
except FileNotFoundError as e:
    print(e)
# → FileNotFoundError: [Errno 2] No such file or directory: 'missing.log'
```

| Item | Details |
|---|---|
| Modes | `"r"` (default), `"w"`, `"a"`, optionally with `"t"` or `"b"` (binary); UTF-8 only |
| `read()` / `read(n)` | all the rest / up to `n` characters (bytes in binary mode) |
| `readline()` | one line **including** its newline; `""` at the end of the file |
| `readlines()` | a list of the remaining lines |
| `write(s)` / `writelines(items)` | write text (bytes in binary mode) |
| `seek(…)` / `tell()` | move / current position |
| `flush()` / `close()` | flush / close (closing twice is fine) |
| `f.name`, `f.closed` | readable attributes |

### HTTP

```python
resp = http_get("http://127.0.0.1:8080/")
nl = resp.find("\n")
status = int(resp[0:nl])          # 200, 404 … or 0 if the connection failed
body = resp[nl + 1:]

headers = "Authorization: Bearer abc\nAccept: text/plain"
resp = http_post("http://127.0.0.1:8080/api", "{\"a\": 1}", headers)
q = "q=" + url_encode("a b&c")    # → q=a%20b%26c
```

| Call | Result |
|---|---|
| `http_get(url[, headers])` | `"STATUS\nBODY"` as text; the status is `0`, followed by the curl error text, when the connection itself fails |
| `http_post(url, body[, headers])` | the same; `body = None` sends an empty body; the default `Content-Type` is `application/json` |
| `url_encode(text)` | percent-encoding: `A-Z a-z 0-9 - _ . ~` stay as they are, every other byte becomes `%XX` |

`headers` is `None` or text with one `Name: value` line per header. All calls
share one keep-alive connection, so only the first call pays for the
connection setup.

### Time

`time()` returns seconds since 1970 as a float (wall clock). `clock()` returns
monotonic seconds, for measuring how long something takes.

---

## 16. All builtins

**78 builtin names are reserved.** All of them are *call-only*: you can call
them, but you can't assign them, pass them as values, or use their names for
your own variables, functions or parameters. Their argument counts are checked
at compile time.

### Core (12)

| Call | Result |
|---|---|
| `print(a, b, …)` | prints the values joined by spaces, then a newline |
| `len(x)` | number of characters (text), bytes (bytes) or elements (collections) |
| `str(x)` | the display form as text; works for every type |
| `int(x)` | from int, float (truncates), bool, text or bytes (strict decimal) |
| `float(x)` | from int, float, bool, text or bytes |
| `bool(x)` | true or false by the falsy rules; never fails |
| `type(x)` | the type name as text (`"none"` for `None`, the class name for instances) |
| `range(stop)` / `range(start, stop[, step])` | lazy range of ints |
| `encode(t)` / `decode(b)` | text → bytes / bytes → text (UTF-8) |
| `set()` / `set(iterable)` | a new set |
| `collect()` | runs the garbage collector now and returns how many blocks it freed |

### Environment (3) and network (3)

`input([prompt])`, `time()`, `clock()`, `http_get(...)`, `http_post(...)`,
`url_encode(...)`, all described in [section 15](#15-input-output-files-and-http).

### SIMD vectors (5)

`fvec`, `ivec`, `fvec_fill`, `ivec_fill`, `dot`, described in
[section 18](#18-simd-vectors).

### Python-style builtins (43)

| Call | Result |
|---|---|
| `abs(x)` | absolute value |
| `all(it)` / `any(it)` | `True` if every / any element is true (`all([])` is `True`) |
| `ascii(x)` | like `repr`, but escapes every non-ASCII character |
| `bin(x)` / `oct(x)` / `hex(x)` | `"0b101"` / `"0o10"` / `"0xff"` |
| `bytes(…)` | `bytes()` empty, `bytes(n)` n zero bytes, `bytes([104, 105])`, `bytes("hi", "utf-8")` |
| `callable(x)` | `True` for functions, bound methods and classes |
| `chr(i)` / `ord(c)` | code point → one-character text / back |
| `delattr(obj, name)` | removes a dynamic attribute from an instance |
| `dict(…)` | a new map: `dict(a=1, b=2)`, `dict(other_map)`, `dict(list_of_pairs)` |
| `dir(obj?)` | sorted names: the globals, or an instance's attributes and methods |
| `divmod(a, b)` | `(a / b, a % b)` with **truncating** division: `divmod(-7, 2)` → `(-3, -1)` |
| `enumerate(it, start=0)` | a list of `(index, item)` pairs |
| `filter(fn, it)` | a list of the items for which `fn(item)` is true (`fn` may be `None`) |
| `format(value, spec)` | format mini-language: `format(3.14159, ".2f")` → `3.14`, `format(42, ">8")`, `format(255, "08b")` |
| `getattr(obj, name, default?)` | read an attribute by name |
| `globals()` / `locals()` / `vars(obj?)` | a map of names → values (globals, the current function's names, or an instance's attributes) |
| `hasattr(obj, name)` | `True` if `getattr` would succeed |
| `hash(x)` | an int hash; lists, maps, sets and instances can't be hashed |
| `id(x)` | an int identity number |
| `isinstance(x, type)` | **exact** type check: `isinstance(5, int)` is `True`, `isinstance(True, int)` is `False` |
| `iter(x)` / `next(it, default?)` | make an iterator / take its next item (`StopIteration` at the end, or `default`) |
| `list(it?)` / `tuple(it?)` | a new list / tuple |
| `map(fn, it…)` | a list of `fn(item)` results |
| `max(…)` / `min(…)` | largest / smallest; options `key=` and `default=` |
| `open(path, mode="r")` | open a file ([section 15](#15-input-output-files-and-http)) |
| `pow(b, e, mod?)` | power; with `mod` the result is `(b ** e) % mod` for ints |
| `repr(x)` | the quoted form: `repr("it")` → `'it'` |
| `reversed(seq)` | a reversed list |
| `round(x, ndigits?)` | half-even rounding: `round(2.5)` → `2`, `round(3.14159, 2)` → `3.14` |
| `setattr(obj, name, v)` | set an instance attribute by name |
| `slice(start?, stop?, step?)` | the tuple `(start, stop, step)`; slice with `xs[a:b:c]` instead |
| `sorted(it, key?, reverse?)` | a new sorted list (stable) |
| `sum(it, start=0)` | adds everything up; a text `start` joins text |
| `zip(a, b, …)` | a list of tuples, stopping at the shortest input |

`zip`, `map`, `filter`, `enumerate` and `reversed` return **lists right away**;
they are not lazy as in Python.

### Reserved stubs (9) and rejected names (3)

* **Always fail with a clear `RuntimeError`:** `super`, `property`,
  `classmethod`, `staticmethod`, `issubclass`, `breakpoint`, `help`,
  `memoryview`, `object`.
* **Compile errors**, because programs are compiled ahead of time and there is
  no interpreter: `eval`, `exec`, `compile`.

---

## 17. Methods on builtin values

Call methods as `value.method(args)`. Method arguments are **positional only**.
Methods that change the value return `None`.

| Type | Methods |
|---|---|
| `list` | `append(v)`, `pop([i[, default]])`, `insert(i, v)`, `index(v)`, `remove(v)`, `reverse()`, `clear()` |
| `map` | `get(k[, default])`, `keys()`, `values()`, `items()`, `pop(k[, default])`, `clear()` |
| `set` | `add(v)`, `remove(v)`, `discard(v)`, `pop()`, `clear()` |
| `text` | `upper()`, `lower()`, `split(sep)`, `join(items)`, `find(needle)`, `replace(old, new)`, `strip()`, `starts_with(p)`, `ends_with(p)` |
| `bytes` | `decode()` |
| `file` | see [Files](#files) |

`keys()`, `values()` and `items()` return new lists (snapshots).

---

## 18. SIMD vectors

Fixed-length number arrays that use the CPU's SIMD instructions for fast math.

```python
a = fvec([1.0, 2.0])              # float64 lanes
b = fvec([3.0, 4.0])
c = a + b                         # elementwise: + - * / %
print(dot(a, b))                  # → 11.0
z = fvec_fill(1000, 0.0)          # 1000 zeros
n = ivec([1, 2, 3])               # int64 lanes
```

| Call | Result |
|---|---|
| `fvec(iterable)` | a float vector from numbers |
| `ivec(iterable)` | an int vector, from ints and bools only |
| `fvec_fill(n, v)` / `ivec_fill(n, v)` | `n` copies of `v` |
| `dot(a, b)` | inner product (float for `fvec`, int for `ivec`) |

* **What works:** indexing, `v[i] = x`, loops, `len`, `sum`, `min`, `max` and
  `==` behave as they do for lists.
* **What doesn't:** both sides must have the same length, and `fvec` can't be
  mixed with `ivec`. Slicing and hashing are not supported.
* **Float sums:** `sum` and `dot` on `fvec` may differ from a plain loop in the
  last digits, because SIMD adds the numbers in a different order.

---

## 19. Memory

You never free memory yourself. A garbage collector reclaims everything,
including reference cycles. `collect()` forces a collection; it exists for tests,
so don't write programs that depend on *when* collection happens.

---

## 20. Not available yet

* **Language:** inheritance, generators, lambdas, decorators, operator
  overloading, type annotations, multi-line strings, f-strings, floor division
  and text repetition (`"a" * 3`).
* **Library:** a standard library beyond the builtins, reading command-line
  arguments, processes and environment variables.
* **Platforms:** Linux x86_64 is the only target today.

Planned work is on the project roadmap. Known bugs are in
[known issues](../reference/known-issues.md).

---

## 21. Differences from Python

| Python | Pentest |
|---|---|
| `def f(x):` | `f(x):` |
| `7 // 2` is floor division | `//` starts a comment; `7 / 2` is `3` |
| `0 or "x"` returns `"x"` | `and`/`or` return `True`/`False`; use `x if x else default` |
| `a, b` makes a tuple | `a, b` makes joined **text**; write `(a, b)` |
| `for a, b in pairs:` | `for item in pairs:` then `a, b = item` |
| `lambda x: …` | declare a named function |
| `key=len` | `key=` needs your own function, because builtins aren't values |
| `except Exception:` catches all | only a bare `except:` catches all |
| `isinstance(True, int)` is `True` | `False`, because types match exactly |
| `divmod(-7, 2)` is `(-4, 1)` | `(-3, -1)`, because division truncates |
| `zip`/`map`/`filter` are lazy | they return lists |
| `"""text"""` is a string | `"""…"""` is a comment |
| `str` type | the type is called `text` |
| `dict` | called `map`; `{}` is an empty map |

More surprises are listed in [gotchas](gotchas.md).
