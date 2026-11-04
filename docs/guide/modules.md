# Modules and imports (Stage 2D)

Split a program across files and pull in only the declarations you use. Imports
are resolved and expanded **before semantic analysis and code generation**, so a
usable import costs nothing at run time: the compiled program is still one flat
C translation unit with direct calls, no module objects and no lookup tables.

> **Status (unreleased).** Modules are the Stage 2D feature and are present in
> this working tree; `pentest --version` still prints `0.2.1`. The normative
> rules are [SPEC §14](../../SPEC.md#14-modules-and-imports-stage-2d); this page
> is the guided tour.
> The VSCode extension does not yet highlight `import`/`from` or understand
> module names.

The syntax is Python-like:

```text
import m                      # bind the module name m
import m as x                 # bind the module under the alias x
from m import a               # bind the declaration a directly
from m import a as b          # ... under the alias b
from m import a, b as c       # several names at once
```

Module names are dotted identifiers (`import lib.toolkit`); imports are only
allowed at the top level — inside a function you get
`'import' is only allowed at the top level`.

## A worked example

```text
tools/
  scan.pen            # entry: pentest run tools/scan.pen
  lib/
    hosts.pen
    ports.pen
```

<!-- no-run -->
```python
# tools/lib/hosts.pen
PREFIX = "host"

name(expand):
    return PREFIX + ":" + name

class Host:
    __init__(self), name:
        self.name = name

    label(self):
        return expand(self).name
```

<!-- no-run -->
```python
# tools/lib/ports.pen
name(probe):
    return "probe " + name
```

<!-- no-run -->
```python
# tools/scan.pen
import lib.hosts as hosts
from lib.ports import probe

main:
    h = hosts.Host("web1")
    print(h).label()
    print(probe("web1"))
```

Running `build/pentest run tools/scan.pen` prints:

```text
host:web1
probe web1
```

`import lib.hosts` resolves `lib/hosts` relative to the importing file and binds
the **last component** of the dotted name — `hosts` — not `lib`. `hosts.Host`
constructs the imported class, `hosts.PREFIX` reads its module global, and
`probe` is bound directly by the `from` import.

## Resolution

A specifier `a.b` becomes the path `a/b` and is tried, in this order:

1. **the directory of the importing file** — so a module's own siblings
   import each other with short names (`import ports` inside `lib/`);
2. **the current working directory** of the `pentest` process;
3. **each directory in `$PENTEST_PATH`** (colon-separated, like `PATH`).

At each root the first existing candidate wins:

| Candidate | Example |
|---|---|
| `<path>.pen` | `lib/hosts.pen` |
| `<path>.pent` | `lib/hosts.pent` |
| `<path>/__init__.pen` | `lib/hosts/__init__.pen` |
| `<path>/__init__.pent` | `lib/hosts/__init__.pent` |

So `import pkg` is a package: it loads `pkg/__init__.pen` (`.pent`
equivalently) when no `pkg.pen`/`pkg.pent` exists, and the initializer's
top-level code and declarations are the module. There is no relative-dot syntax
(`from . import x` is not accepted).

Paths are canonicalised (`realpath`), so a module reached by two different
spellings — or imported twice by two importers — is loaded and initialised
exactly **once**.

```bash
PENTEST_PATH=/opt/pentest/lib:/srv/shared build/pentest run scan.pen
```

## Using imported names

* **Module alias.** `tk.greet` reads a top-level function, `tk.Box` a class,
  `tk.PREFIX` a top-level global of module `tk`. Attribute access reaches only
  declarations made *in that file*; a name another module re-imported is not
  re-exported (`from lib.mid import twice` where `mid` itself imported `twice`
  is an error). The specifier is not a nested namespace: after
  `import a.b`, use `b.name`, not `a.b.name`.
* **Direct names.** `from m import a` makes `a` usable exactly like a local
  top-level declaration of the importing file — functions, classes and globals
  alike.
* **Private names.** A top-level name starting with `_` is module-private:
  `from m import _x` → `cannot import private name '_x' from module 'm'`, and
  `m._x` → `'_x' is private in module '<path>'`.
* **The alias is not a value.** `x = toolkit` → `module alias 'toolkit' cannot
  be used as a value; write toolkit.name`.
* **Binding rules.** An alias or imported name may not collide with a
  declaration of the importing file (`import alias 'x' conflicts with a
  declaration in this module`, `imported name 'x' conflicts with a declaration
  in this module`), with another alias, or with a different module's import of
  the same name. Two aliases for the **same** module are fine.
* **Assignment.** A `from`-imported name is read-only: assigning it, or
  declaring it `global`, is an error (`cannot assign to imported name 'x'` /
  `cannot declare imported name 'x' global`). A module's global *is* writable
  through its qualified name — `tk.PREFIX = "x"` updates the module global
  that `tk.greet` reads. A local of the same name shadows the import inside a
  function, as usual.
* **Module globals stay qualified.** Importers never see a module's globals
  unqualified; they are rewritten to module-unique internal names (visible in
  generated C as `__ptm<id>_<name>`), so two modules may each define `PREFIX`
  with no interaction.

## Selective loading: you only pay for what you name

`pentest run` and `pentest build` prune imported declarations that are not
reachable from the entry program **before** semantic analysis and code
generation. An unused imported function, class or pure global is never
analysed, never emitted and never compiled — importing a large library costs
only the parts the program uses.

<!-- no-run -->
```python
# lib/toolkit.pen
PREFIX = "toolkit"

name(greet):
    return PREFIX + ": " + name

# Deliberately invalid. Nothing below references it.
unused_bad():
    return no_such_name + 1
```

<!-- no-run -->
```python
import lib.toolkit

main:
    print("only the entry code runs")
```

`pentest run` prints `only the entry code runs`: the reachability pass drops
`greet` and the broken `unused_bad`, so semantic analysis never sees the
undefined name. **`pentest check` is strict** — it validates every imported
declaration, reachable or not, so the same program is rejected with

```text
lib/toolkit.pen:9:12: error: undefined variable 'no_such_name'
      return no_such_name + 1
             ^
```

Rules of the pruning pass:

* declarations in the **entry file are never dropped**; only imported items can
  be;
* a declaration is kept when the entry program references it, or when a kept
  declaration references it (transitively);
* an imported module's non-declaration top-level code is always kept and runs —
  that is module initialization (see below);
* an imported top-level global whose initializer is a **pure** expression
  (literals, names, arithmetic and collection literals) may be dropped; anything
  that can have an effect — a call, a method call, an index/attribute read —
  is kept as initialization even if its name is never used.

Diagnostics from imported files name the imported file and line
(`lib/toolkit.pen:9:12`), never the entry file that imported it.

### Zero cost, by construction

* No runtime module object exists: the linker renames imported declarations to
  unique internal names, rewrites references, and flattens everything into the
  entry `Program`. Code generation and the runtime are unchanged, so a call to
  an imported function is the same static call, with the same speed, as a call
  to a local one.
* Pruning is one worklist pass over the top-level items — O(AST size) before
  compilation, not per call.
* A program with **no imports** takes a fast path: one scan of the top-level
  items and nothing else. The import layer cannot slow down single-file
  programs.

## When does imported code run?

Imports behave like top-level statements at their textual position:

* The first time a module is imported, its top-level code runs **once**, in
  source order, at that import statement's position in the importer.
* Importing the same module again (from anywhere) runs nothing and binds the
  same declarations.
* Imported declarations are registered before execution, so modules can call
  functions declared later in the file and refer to earlier modules.
* An imported module's `main:` block is **ignored** — only the entry file's
  `main:` runs. Top-level code in a module is the way to initialize it.

<!-- no-run -->
```python
# lib/util.pen
print("util initialised")
BASE = 10
```

<!-- no-run -->
```python
# app.pen
print("entry 1")
import lib.util
print("entry 2")

main:
    print("main:", util.BASE)
```

```text
entry 1
util initialised
entry 2
main: 10
```

**Cycles are compile errors.** If `a` imports `b` and `b` imports `a`, the
second load is rejected at the offending `import`:
`circular import: 'a' is already being loaded`.

## Errors

| Situation | Message (prefix `error():`) |
|---|---|
| no candidate file found | `cannot find module 'lib.nope'` |
| importing a `_name` | `cannot import private name '_x' from module 'm'` |
| qualified access to `_name` | `'_x' is private in module '<path>'` |
| missing declaration | `module 'm' has no declaration 'x'` |
| alias/value/name collisions | `import alias 'x' conflicts with a declaration in this module`; `imported name 'x' conflicts with a declaration in this module`; `two modules are imported with the alias 'x'`; `imported name 'x' is already imported from another module` |
| using an alias as a value | `module alias 'x' cannot be used as a value; write x.name` |
| assigning an import | `cannot assign to imported name 'x'`; `cannot declare imported name 'x' global` |
| import not at top level | `'import' is only allowed at the top level` |
| import cycle | `circular import: 'm' is already being loaded` |
| malformed import syntax | `expected module name`, `expected 'import' after module name`, `expected a name to import`, `expected alias after 'as'` |

The general diagnostic format and exit codes are in
[errors and diagnostics](errors-and-diagnostics.md); the CLI's strict/lenient
split is in the [CLI reference](cli.md).

## The run cache follows dependencies

The `run` cache key covers the entry source **and the canonical path plus the
contents of every imported module**, so editing any dependency re-keys the
program and the next `pentest run` recompiles it. Editing an unrelated file
does not. Nothing else changes: builds are still single self-contained
executables that need no module files at run time.

## A library module built on HTTP

A networking library is an ordinary module whose functions wrap the `http_post`
builtin. One example is the author's Jev decision client, `AI.pen`. It is kept
outside the repository, so it isn't included here. It calls the Jev decision
playground and falls back across three independent sites. Using such a module
looks like this:

```text
# your tool.pen
from AI import jev

main:
    state = "there is an apple on the table"
    jev.noul(state, "Is this urgent?")            # prints True/False
    jev.choose(state, "Which team?", ["Billing", "Sales"])
    dist = jev.score(state, "Rate frustration.", ["Calm", "Annoyed", "Furious"])
```

Run it with the library directory on `$PENTEST_PATH`:

```bash
PENTEST_PATH=/path/to/library-dir pentest run tool.pen
```

Because modules are expanded at compile time, importing `AI` adds zero runtime
overhead when you don't use it, and using it still produces a single
self-contained binary.

## Limitations

* **Top level only.** No imports inside functions, no `import` expression, no
  dynamic `importlib`.
* **No re-export chain.** Only a module's own top-level declarations are
  importable; names it imported are not re-exported (import them from their
  defining module).
* **No nested module attributes.** After `import a.b`, `b.name` works but
  `a.b.name` does not — the specifier binds its last component only.
* **No relative dotted imports** (`from .sibling import x`); use the plain
  name — modules in the same directory resolve against it first.
* **Static only.** Resolution happens at compile time against the filesystem;
  a missing module is a compile error, never a runtime failure.
* **No module objects at run time.** You cannot store a module in a variable,
  pass it to a function, or enumerate its attributes.
* **Shared global state.** Imported module globals are single storage per
  program: assigning `m.x` from anywhere is visible to every reader of `m.x`.
* **Editor support.** The VSCode extension highlights and completes
  `import`/`from` as keywords; module names themselves resolve at compile time
  (there are no dynamic module objects, so no cross-file completion yet).

Next: the [CLI reference](cli.md), [errors and diagnostics](errors-and-diagnostics.md),
and the normative [SPEC §14](../../SPEC.md#14-modules-and-imports-stage-2d).
