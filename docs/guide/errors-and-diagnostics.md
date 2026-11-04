# Errors and diagnostics

How Pentest reports problems, what the common messages mean, and how runtime
errors relate to exceptions. The normative catalogues are SPEC §9.1 (lexer and
parser), §9.2 (semantic) and §9.3 (runtime); every quoted message there is
normative and the test-suite matches on those substrings.

## Two kinds of error

| | Compile-time | Runtime |
|---|---|---|
| When | `check`, `run`, `build` — before anything executes | while the program runs |
| Location | `file:line:col` plus the source line and a caret | `file:line` only (no column) |
| Prefix | `error():` | `runtime error:` |
| Count | lexer/parser: the **first** error; semantic pass: **all** errors | one — the first uncaught exception |
| Catchable | no | yes, with `try`/`except` |
| Exit code | `1` | `1` |

`<file>` is the path exactly as you typed it on the command line for the entry
file; a diagnostic in an imported module names that module's canonical
(`realpath`) path — which may be absolute even if you imported it relatively.

## Compile-time diagnostics

Layout (the source line is indented two spaces, the caret points at the
offending token):

```text
<file>:<line>:<col>: error: <message>
  <source line>
  <caret line>
```

The lexer and parser stop at the first problem:

```python
print("ok")
if True
    print("x")
```

Output:

```text
example.pen:2:8: error: expected ':'
  if True
         ^
```

The semantic pass keeps going and reports **every** error, so you can fix
several at once. The order follows the analysis passes, not strictly the source
order:

```python
add(a, b):
    return a + b

print(add(1))
print(zzz)
```

Output:

```text
example.pen:4:7: error: function 'add' expects 2 argument(s), got 1
  print(add(1))
        ^
example.pen:5:7: error: undefined variable 'zzz'
  print(zzz)
        ^
```

Calls to functions declared in the file are checked **at compile time** — both
the argument count and the names of named arguments:

```python
f(a, b = 2):
    return a + b
print(f(1, 2, 3))
print(f(c = 1))
```

Output:

```text
example.pen:3:7: error: function 'f' expects at most 2 argument(s), got 3
  print(f(1, 2, 3))
        ^
example.pen:4:7: error: unexpected argument name 'c' in call to 'f'
  print(f(c = 1))
        ^
example.pen:4:7: error: function 'f' expects at least 1 argument(s), got 0
  print(f(c = 1))
        ^
```

Reserved builtin names cannot be reused:

```python
sum = 0
len(x):
    return 1
```

Output:

```text
example.pen:2:1: error: cannot declare function with reserved name 'len'
  len(x):
  ^
example.pen:1:1: error: cannot assign to reserved name 'sum'
  sum = 0
  ^
```

### Common compile errors

| Message | Usual cause | Fix |
|---|---|---|
| `tabs are not allowed for indentation` | a tab in leading whitespace | indent with spaces |
| `inconsistent indentation` | a dedent to a column that matches no enclosing block | align with an earlier level |
| `expected ':'` | missing colon after `if`/`for`/`while`/`class`/`try`/… or a declaration header | add it |
| `expected an indented block after ':'` | empty body | add a statement or `pass` |
| `unclosed '('` | missing `)` — also caused by `//` comments swallowing it | close the bracket; remember `//` is a comment |
| `undefined variable 'x'` | never assigned anywhere, a typo, or a builtin used as a value | check spelling; builtins are call-only |
| `undefined function 'f'` | calling a name that is not a declared function | declare it |
| `function 'f' expects N argument(s), got M` | wrong positional count for a known function | fix the call |
| `cannot assign to reserved name 'print'` | assigning to one of the seventy-eight builtins | rename |
| `name 'f' is already declared as a function` | a `class` shares its name with a function, or a nested declaration/assignment collides with a parameter, local, or nested function | rename one (a plain variable may share a name with a function since 0.3 — reads take the variable, `()` takes the function) |
| `'main' must be declared as the special block 'main:'` | wrote `function main:` or called `main()` | use the block form `main:` |
| `'break' outside of a loop` | `break`/`continue` outside `while`/`for` (also inside a bare `switch`) | restructure |
| `'return' outside of a function` / `'return' is not allowed inside 'main:'` | `return` at top level or in `main:` | move into a function |
| `integer literal out of range` | literal > 9223372036854775807 | arithmetic wraps, but literals cannot |
| `no binding for nonlocal 'x' found` | no enclosing function binds `x` | assign it in the outer function first |
| `classes must be declared at the top level` | `class` inside a function | move it out |
| `'eval' is not available in Pentest…` | `eval`/`exec`/`compile` | not supported by design |

### Import errors

Import diagnostics name the file and line that contains the `import` — or the
imported file when the error is inside it (the caret can point into a library
file). The messages and their triggers are tabulated in
[modules § Errors](modules.md#errors); the most common:

| Message | Meaning |
|---|---|
| `cannot find module 'lib.nope'` | no candidate file in the importer's directory, the working directory or `$PENTEST_PATH` |
| `circular import: 'm' is already being loaded` | `m` (transitively) imports the file doing the importing |
| `module 'm' has no declaration 'x'` | `from m import x` where `x` is not top-level in `m` |
| `cannot import private name '_x' from module 'm'` / `'_x' is private in module '<path>'` | `_`-prefixed names are module-private |
| `import alias 'x' conflicts with a declaration in this module` | the alias collides with a top-level name of the importing file |
| `module alias 'x' cannot be used as a value; write x.name` | a module is not a value (assigning/passing/printing it) |
| `cannot assign to imported name 'x'` | `from m import x` names are read-only; also `cannot declare imported name 'x' global` |
| `'import' is only allowed at the top level` | an `import` inside a function or `main:` body |
| `expected module name` / `expected 'import' after module name` / `expected a name to import` / `expected alias after 'as'` | malformed import statement |

Remember the two-mode split: `pentest run` ignores *unused* imported code, but
`pentest check` validates all of it, so a broken unused declaration fails only
`check` ([modules](modules.md#selective-loading-you-only-pay-for-what-you-name)).

## Runtime errors

```text
<file>:<line>: runtime error: <message>
```

Only the line is known at run time. These are the ones you will meet most:

| Message | Meaning |
|---|---|
| `global variable 'x' used before assignment` / `local variable 'x' …` | the assignment has not executed yet (variables are not hoisted) |
| `division by zero` / `modulo by zero` | zero divisor (also for floats) |
| `unsupported operand types for +: text and int` | no implicit conversions between types |
| `list index out of range` (also `tuple`/`text`/`bytes`) | index outside `-len..len-1` |
| `map key not found` | `m[k]` for a missing key (use `m.get(k)`) |
| `'int' object has no attribute 'foo'` | attribute read on a value that has none |
| `object of type 'int' is not callable` | calling a non-function |
| `unhashable type: 'list'` | list/map/set/instance used as a map key or set element |
| `invalid literal for int(): 'x'` | unparseable text in `int()` |
| `not enough values to unpack (expected N, got M)` | destructuring length mismatch (often the [comma-join](language-tour.md#destructuring--and-the-comma-join-trap) rule) |

```python
print("a" + 1)
```

Output:

```text
example.pen:1: runtime error: unsupported operand types for +: text and int
```

Type names inside messages are `int float bool text bytes range list map set
tuple function class method None unset`; an instance reports its **class name**.

## Runtime errors are exceptions

Every runtime error is raised as an instance of one of twelve classes, with its
message unchanged, and is therefore catchable. When an exception is uncaught the
driver prints the `runtime error:` line above — **the class name is not part of
that output** — and exits `1`.

| Class | Typical sources |
|---|---|
| `NameError` | used-before-assignment guards |
| `TypeError` | bad operand/argument types, not callable/iterable/subscriptable, wrong argument counts at run time |
| `ValueError` | bad values: `int()`/`float()` parse errors, `negative exponent`, shift count, zero slice/range step, unpack counts, invalid UTF-8 |
| `ZeroDivisionError` | `division by zero`, `modulo by zero` |
| `IndexError` | sequence index out of range |
| `KeyError` | `map key not found`, `value not in set`, `pop from an empty set` |
| `AttributeError` | missing attribute/field |
| `StopIteration` | `next()` on an exhausted iterator with no default |
| `FileNotFoundError`, `OSError` | file operations |
| `RuntimeError` | the nine always-failing stubs and internal fallbacks |
| `Exception` | the generic class for `raise Exception("…")` |

The complete mapping is the table in SPEC §6.10, and the full runtime message
catalogue is SPEC §9.3.

## Exit codes

| Code | Meaning |
|---|---|
| `0` | success |
| `1` | any compile error, any uncaught runtime exception, unreadable source, missing `clang` |
| `2` | usage error (bad arguments or file extension) |

A program you `build` and run directly returns `1` for an uncaught error and
`0` otherwise.

## Tips

* `build/pentest check file.pen` is the fastest feedback loop — it never invokes
  `clang`.
* Use `try`/`except` around operations that can fail on untrusted input (indexing,
  map lookups, `open`), and `finally` for cleanup.
* To see what the compiler generated, read the C: it is written to a scratch
  directory and deleted after linking; see
  [internals/code-generation](../internals/code-generation.md#inspecting-generated-c).
