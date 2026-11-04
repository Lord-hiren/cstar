# Builtins guide

Pentest reserves **seventy-eight** builtin names. This page is a working reference:
what each one does, the shapes that are easy to get wrong, and the deviations
from Python. The normative text is SPEC §5.11 (per-builtin error wording is
there too); the method tables are SPEC §5.10.

## The rules that apply to every builtin

1. **Call-only.** A builtin can only appear as the target of a call. You cannot
   read one as a value (`f = sorted`, `map(str, xs)`, `callable(print)`),
   assign
   to it, declare a function or parameter with its name, or use it as a loop
   variable. A bare read is reported as `undefined variable 'sorted'`. Pass a
   named function declaration (`name(…):`) wherever Python would take a callable (`key=`, `map`,
   `filter`). There are no lambdas.
2. **Compile-time checks.** Arity and named arguments are validated when you
   compile: `'abs' expects at most 1 argument(s), got 2`,
   `'round' has no named argument 'nd'`.
3. **Zero overhead.** Each builtin compiles to one direct C call; nothing is
   emitted for builtins a program does not use
   ([performance](../performance/optimization.md) §11).
4. **The original fifteen take no named arguments** at all
   (`builtin 'print' does not take named arguments`). The Python-parity ones
   use their documented slot names (`round(number=2.5, ndigits=0)`).

## Quick reference

### Core, environment and network (the original fifteen plus the HTTP builtins)

| Call | Result |
|---|---|
| `print(a, b, …)` | writes the display forms joined by a space + newline; returns `None` |
| `len(x)` | `int`: code points of `text`, bytes of `bytes`, element count of collections |
| `str(x)` | the display form as `text` (works for every type) |
| `int(x)` | from `int`/`float` (truncates)/`bool`/`text`/`bytes` (strict decimal) |
| `float(x)` | from `int`/`float`/`bool`/`text`/`bytes` |
| `bool(x)` | truthiness; never raises |
| `type(x)` | the type name as `text` — `"none"` for `None`; the class name for instances |
| `range(stop)` / `range(start, stop[, step])` | lazy range of `int`s; zero step is an error |
| `encode(t)` / `decode(b)` | `text` ⇄ `bytes` (UTF-8; `decode` validates) |
| `set()` / `set(iterable)` | a `set` (a `map` contributes its keys) |
| `collect()` | forces garbage collection, returns blocks freed |
| `input()` / `input(prompt)` | one line from stdin as `text` (newline stripped); `None` at end of input |
| `time()` | wall-clock seconds since the epoch (`float`) |
| `clock()` | monotonic seconds (`float`), for measuring intervals |
| `http_get(url[, headers])` | HTTP GET `url`; returns `STATUS\nBODY` — the status code (`0` + curl error text when the connection itself fails), a newline, the raw body |
| `http_post(url, body[, headers])` | POST `body` (`None` = empty body) to `url`; returns `STATUS\nBODY` like `http_get` |
| `url_encode(text)` | percent-encoding for query strings / form bodies: `A-Za-z0-9-_.~` stay literal, every other byte → `%XX` (uppercase hex) |

The network builtins are libcurl-backed and share one persistent keep-alive
handle per process (HTTP/3 preferred, HTTP/2 fallback via ALPN, TCP fast
open), so every call after the first skips the TCP + TLS handshake. `headers`
is `None` or a `text` of newline-separated `Name: value` lines (Burp-style
raw header blocks). A POST with a body defaults to
`Content-Type: application/json` unless `headers` supplies its own
`Content-Type`. See `extras/AI.pen` for a decision client and
[sqli.md](sqli.md) for worked SQL-injection examples (`examples/sqli_*.pen`
against `examples/sqli_lab.py` on `http://127.0.0.1:8080`).

### SIMD vectors (five: `fvec`, `ivec`, and friends)

First-class fixed-length vector types with guaranteed SIMD lanes
([internals](../internals/runtime.md#heap-objects)); they follow the
original-fifteen rules (call-only, no named arguments).

| Call | Result |
|---|---|
| `fvec(iterable)` | an `fvec` of `float64` lanes from any iterable of numbers (`bool` counts as `0.0`/`1.0`); non-number element → `TypeError: fvec() elements must be numbers, got <t>` |
| `ivec(iterable)` | an `ivec` of `int64` lanes; strictly `int`/`bool` elements — `ivec([1.0])` raises `TypeError: ivec() elements must be ints, got float` |
| `fvec_fill(n, v)` | `n` copies of the numeric scalar `v` as an `fvec` (`n` must be a non-negative `int`) |
| `ivec_fill(n, v)` | `n` copies of the `int`/`bool` scalar `v` as an `ivec` |
| `dot(a, b)` | inner product as a scalar: `float` for two `fvec`, `int` (wrapping) for two `ivec`; a length or type mismatch raises before any lane is read |

Operators on vectors are elementwise and keep the scalar operators' exact
per-element semantics: `+ - *` (and the compound forms) work on any
numeric mix for `fvec` and `int`/`bool` for `ivec`; `/` `%` behave like
the scalar operators too (float zero-divisor and int `MIN / -1` edges
included). Operands must have equal length — `ValueError: fvec length
mismatch` — and mixing `fvec` with `ivec` is a `TypeError`. Indexing,
assignment (`v[i] = …`), iteration, `==` (elementwise), `len()`,
`type()`, `sum`/`min`/`max` and `str` all work as for lists; unlike
lists, vectors are **unhashable**, and slicing raises
`TypeError: 'fvec' object does not support slicing` (likewise `ivec`).
`sum(v)` over an `fvec` accumulates in vector-lane order — for values
that are not exactly representable sums, its last bits may differ from a
sequential loop (`dot` likewise may fuse a multiply-add); integer
reductions wrap, so their results are order-independent.

### Python-parity (43)

| Group | Names |
|---|---|
| Numbers | `abs divmod pow round sum min max hex bin oct` |
| Conversion / construction | `list tuple dict bytes chr ord format repr ascii` |
| Iteration | `iter next enumerate zip map filter reversed sorted all any` |
| Reflection | `isinstance callable getattr setattr hasattr delattr id hash dir vars locals globals` |
| Files | `open` (plus the file methods below) |
| Other | `slice` |

```python
by_len(s):
    return len(s)

words = ["pear", "fig", "banana"]
print(sorted(words, key=by_len), sorted(words, reverse=True))
print(max(words, key=by_len), min(3, 1, 2), sum([1, 2, 3], 10))
print(enumerate(["a", "b"], start=1), zip([1, 2], "ab"))
print(any([0, 1]), all([]), abs(-3), round(2.5), round(3.14159, 2), divmod(-7, 2))
print(hex(255), bin(5), oct(8), chr(65), ord("A"), repr("it"))
print(format(3.14159, ".2f"), format(42, ">8"), format(255, "08b"))
print(isinstance(5, int), isinstance("x", "int"), callable(by_len), len(dict(a=1, b=2)))
print(list(reversed([1, 2, 3])), tuple([1, 2]), list("ab"), map(by_len, words), filter(by_len, ["", "x"]))
it = iter([10, 20])
print(next(it), next(it), next(it, "done"))
```

Output:

```text
[fig, pear, banana] [pear, fig, banana]
banana 1 16
[(1, a), (2, b)] [(1, a), (2, b)]
True True 3 2 3.14 (-3, -1)
0xff 0b101 0o10 A 65 'it'
3.14       42 11111111
True False True 2
[3, 2, 1] (1, 2) [a, b] [4, 3, 6] [x]
10 20 done
```

Things this output shows:

* `enumerate`, `zip`, `map`, `filter` and `reversed` are **eager** — they return
  plain lists, so they print as lists and can be indexed and re-iterated.
* `round(2.5)` is `2` (half-even, like Python). `divmod(-7, 2)` is
  `(-3, -1)`:
  `/` and `%` truncate (C semantics), so `divmod` does too — Python gives
  `(-4, 1)`.
* `isinstance` matches the **exact** type (no subclassing), and also accepts a
  type-name string. A bare builtin type name (`int`, `text`, …) in `classinfo`
  is rewritten at compile time.
* `sum` of `text` with a `text` start concatenates (Python refuses).

### Reflection and attributes

```python
class P:
    __init__(self):
        self.x = 1
p = P()
setattr(p, "y", 2)
print(getattr(p, "x"), getattr(p, "z", "none"), hasattr(p, "y"), dir(p))
n = 5
show():
    local_v = 1
    print(locals())
show()
print(globals()["n"])
t0 = clock()
print(time() > 1000000000, clock() >= t0)
print(bytes(3), bytes([104, 105]), bytes("hi", "utf-8"), ascii("é"), id(1) == id(1))
print(slice(1, 5), pow(2, 10), pow(2, 10, 1000), hash("a") == hash("a"))
```

Output:

```text
1 none True [__init__, x, y]
{local_v: 1}
5
True True
b"\x00\x00\x00" b"hi" b"hi" '\xe9' True
(1, 5, None) 1024 24 True
```

`globals()`, `locals()`, `vars()` and `dir()` build their name tables **at
the call site** at compile time — nothing introspects live frames at run time.
`dir()` with no argument always lists the globals. `slice()` returns an inert
`(start, stop, step)` tuple; use `xs[a:b:c]` to actually slice.

### Files

`open(path, mode="r")` supports modes `r`, `w`, `a` with an optional `t` or
`b` (`x` is rejected) and UTF-8 only. `type(f)` is `"file"`; `f.name` and
`f.closed`
are readable attributes. A file is iterable by line, and `next(f)` reads one
line (raising `StopIteration` at end of file).

| Method | Behaviour |
|---|---|
| `read()` / `read(n)` | whole rest / up to `n` characters (bytes in binary mode) |
| `readline()` | one line **including** its newline; empty `text` at end of file |
| `readlines()` | list of remaining lines |
| `write(s)` / `writelines(items)` | write text (bytes in binary mode) |
| `seek(…)` / `tell()` | reposition / current offset |
| `flush()` / `close()` | flush / close (idempotent) |

```python
f = open("scan.log", "w")
f.write("host1 up\n")
f.writelines(["host2 down\n", "host3 up\n"])
f.close()
for line in open("scan.log"):
    print(line.strip())
f = open("scan.log")
print(f.readline().strip(), len(f.readlines()))
f.close()
try:
    open("missing.log")
except FileNotFoundError as e:
    print(e)
```

Output:

```text
host1 up
host2 down
host3 up
host1 up 2
FileNotFoundError: [Errno 2] No such file or directory: 'missing.log'
```

Network access is exactly the three builtins above (`http_get`/`http_post`);
there is no process or environment access — files are the only other I/O
besides `print` and `input`.

## Methods on builtin values

Called as `obj.name(args)`. Arguments are **positional only** (a named
argument is a runtime error `method arguments must be positional`); mutating
methods
return `None`. Unknown methods raise `'<t>' object has no method '<m>'`. These
are the complete sets (SPEC §5.10):

| Type | Methods |
|---|---|
| `list` | `append(v)`, `pop([i[, default]])`, `insert(i, v)`, `index(v)`, `remove(v)`, `reverse()`, `clear()` |
| `map` | `get(k[, default])`, `keys()`, `values()`, `items()`, `pop(k[, default])`, `clear()` |
| `set` | `add(v)`, `remove(v)`, `discard(v)`, `pop()`, `clear()` |
| `text` | `upper()`, `lower()` (ASCII letters only), `split(sep)`, `join(iterable)`, `find(needle)`, `replace(old, new)`, `strip()`, `starts_with(p)`, `ends_with(p)` |
| `bytes` | `decode()` |
| `file` | the table above |

`keys()`, `values()` and `items()` return fresh **lists** (snapshots, not
views).
User-class methods are separate: they accept named arguments (SPEC §5.12).

## Exception classes

Twelve classes exist at program start and are readable as values:
`Exception`, `RuntimeError`, `TypeError`, `ValueError`, `NameError`,
`ZeroDivisionError`, `IndexError`, `KeyError`, `AttributeError`, and — added
with the Python-parity builtins — `StopIteration` (`next()` exhausted),
`FileNotFoundError` and `OSError` (file failures). Calling one builds an
instance: `RuntimeError("why")`. Instances have `.type` and `.message`. See
the [tour](language-tour.md#13-exceptions).

## Stubs and unavailable names

* **Always-failing stubs** — reserved so Python code fails clearly rather than
  silently: `super property classmethod staticmethod issubclass breakpoint help
  memoryview object`. Arguments are evaluated, then the call raises
  `RuntimeError` with its reason (e.g. `super() is not supported: classes have
  no inheritance`).
* **Rejected at compile time**: `eval`, `exec`, `compile`. Pentest compiles
  ahead of time and ships no interpreter, so
  `'eval' is not available in Pentest: programs are compiled ahead of time and
  the runtime never compiles source`.
* **Not implemented, deferred**: `bytearray`, `complex`, `frozenset`.

## Deviations from Python at a glance

The full numbered list is SPEC §5.11.5. The ones that bite:

| Python | Pentest |
|---|---|
| `divmod(-7, 2)` → `(-4, 1)` | `divmod(-7, 2)` → `(-3, -1)` — truncating |
| `zip`/`map`/`filter`/`enumerate`/`reversed` are lazy | eager lists |
| ordering lists/tuples (`max([1,2],[3])`, `sorted([[2],[1]])`) | runtime error unless you pass a scalar `key=` |
| `isinstance(True, int)` → `True` | `isinstance(True, int)` → `False` — exact match, no subclassing |
| `sum(["a"], "")` raises | `sum(["a"], "")` concatenates |
| `bytes` repr `b'…'` | `b"…"` |
| `eval`/`exec` work | compile-time error |
| `f = sorted` | compile-time error (builtins are not values) |
