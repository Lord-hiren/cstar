'use strict';

// cstar Language support for VSCode.
//
//  * Autocomplete: language keywords (M1 + Stage 2A control flow + Stage 2B
//    `global`/`nonlocal` + Stage 2C `try except finally raise as class is`),
//    builtins `print`/`range` (2A), the Stage 2B reserved call-only builtins
//    `len str int float bool type encode decode set` + the Stage 2C
//    `collect`, the 0.2.1 `input time clock` + the 0.2.1 Python-parity set
//    (43 entries `abs … zip` + nine always-fail stubs + the compile-time
//    rejected `eval exec compile` — SPEC §5.11), method names after a `.`
//    (the full 2B collection/Text/Bytes
//    method surface), the 0.2.1 cross-scope form `variable.<qualifier>`
//    (qualifier completions: `global`, `parent`, and every function declared
//    in the file — NOT the method list), and every identifier declared in
//    the open document (functions get the Function icon).
//  * Hover: the same docs on pointer-over — every builtin's signature
//    (placeholders stripped) + contract, keywords/constants, the twelve
//    builtin exception classes, method names right of a dot (per-type
//    contracts), the `variable.` qualifier form, and user function
//    (`name():` / `name(params):`) and class declarations (signature + line).
//  * Live diagnostics: debounced `cstar check` runs (parse + semantic
//    analysis only — code is never compiled or executed) whose
//    `file:line:col: error: msg` output is mapped to editor squiggles.
//  * `cstar.toggleLineComment` command: line-comment toggling for BOTH
//    `#` and `//` (the built-in Ctrl+/ can only use the one token declared
//    in language-configuration.json, which is `#`).
//  * `cstar.run` command + Run button (editor title ▶ and editor context
//    menu): runs THE FILE THE DEVELOPER IS ON — the clicked editor's
//    resource when the button passes one, else the active document — after
//    saving it, via `cstar run <file>` in a reusable "cstar" terminal
//    (interactive stdin, so input() works; the terminal is recreated if it
//    was closed in the meantime).
//
// No dependencies beyond the vscode API.

const vscode = require('vscode');
const os = require('os');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

/** @type {vscode.DiagnosticCollection} */
let diagCollection;
/** @type {vscode.OutputChannel} */
let outputChannel;
/** @type {Map<string, NodeJS.Timeout>} */
const pending = new Map();
let checkCounter = 0;
let cliWarned = false;

const RESERVED = new Set([
  // M1
  'main', 'return', 'True', 'False', 'None', 'print',
  // Stage 2A — 14 new reserved keywords
  'if', 'elif', 'else', 'for', 'while', 'break', 'continue',
  'switch', 'case', 'pass', 'and', 'or', 'not', 'in',
  // Stage 2A — new reserved builtin (call-only, like print)
  'range',
  // Stage 2B — 2 new reserved keywords (declaration forms)
  'global', 'nonlocal',
  // Stage 2B — 9 new reserved builtins (call-only, §8)
  'len', 'str', 'int', 'float', 'bool', 'type', 'encode', 'decode', 'set',
  // Stage 2C — 7 new reserved keywords (exceptions + classes/identity)
  'try', 'except', 'finally', 'raise', 'as', 'class', 'is',
  // Stage 2C — new reserved builtin (call-only, §5.11)
  'collect',
  // Post-2C basics (TODO.txt) — new reserved builtins (call-only)
  'input', 'time', 'clock', 'http_post', 'http_get', 'url_encode',
  // SIMD vector builtins (call-only, no named arguments; src/semantic.cpp)
  'fvec', 'ivec', 'fvec_fill', 'ivec_fill', 'dot',
  // 0.2.1 — Python-parity builtins (SPEC §5.11.3): 43 implemented entries
  'abs', 'all', 'any', 'ascii', 'bin', 'bytes', 'callable', 'chr',
  'delattr', 'dict', 'dir', 'divmod', 'enumerate', 'filter', 'format',
  'getattr', 'globals', 'hasattr', 'hash', 'hex', 'id', 'isinstance',
  'iter', 'list', 'locals', 'map', 'max', 'min', 'next', 'oct', 'open',
  'ord', 'pow', 'repr', 'reversed', 'round', 'setattr', 'slice', 'sorted',
  'sum', 'tuple', 'vars', 'zip',
  // 0.2.1 — nine always-fail stubs (SPEC §5.11.6)
  'breakpoint', 'classmethod', 'help', 'issubclass', 'memoryview',
  'object', 'property', 'staticmethod', 'super',
  // 0.2.1 — the compile-time-rejected dynamic-code trio (SPEC §5.11.6)
  'eval', 'exec', 'compile',
]);

const KEYWORD_DOCS = {
  'main': 'Special entry block — its body runs automatically when top-level execution reaches it:\n\n```cstar\nmain:\n    print("start"\n)```',
  'return': 'Return a value from a function. Bare `return` returns `None`. Falling off the end also returns `None`.',
  'True': 'bool literal',
  'False': 'bool literal',
  'None': 'The None value (the absence of a value).',
  'if': 'Conditional statement (`if cond:`) — also used mid-expression in ternaries (`a if cond else b`). Single-line bodies are allowed:\n\n```cstar\nif x > 0:\n    print(x\nif x > 0): print(x\n)```',
  'elif': 'Another condition in an `if` chain; conditions are tried in order, once each, until one is truthy:\n\n```cstar\nif x > 0:\n    print("pos"\nelif x == 0):\n    print("zero"\n)```',
  'else': 'Optional last branch of an `if` chain — runs only when no `if`/`elif` condition matched:\n\n```cstar\nif x > 0:\n    print("pos"\nelse):\n    print("not positive"\n)```',
  'for': 'Loop with a binding variable: `for var in iterable:` — requires an indented block. Iterates a `range(n)` value or a string:\n\n```cstar\nfor i in range(3):\n    print(i\n)```\nThe loop variable leaks after the loop.',
  'while': 'Pre-test loop — may run zero iterations. `break`/`continue` apply to the innermost loop:\n\n```cstar\nwhile x < 10:\n    x += 1\n```',
  'break': "Exit the innermost loop immediately. **Loop-only**: a compile error outside any loop (`'break' outside of a loop`); a `switch` does not count as a loop.",
  'continue': "Skip to the next iteration of the innermost loop. **Loop-only**: a compile error outside any loop (`'continue' outside of a loop`).",
  'switch': 'Multi-way branch; constant-literal labels only, first match wins (no fall-through, no `break` needed). `case *:` is optional and must be the last case:\n\n```cstar\nswitch v:\n    case 1:\n        print("one"\n    case *):\n        print("other"\n)```',
  'case': 'One arm of a `switch`. The label must be a constant literal (int/float/string/`True`/`False`/`None`); duplicate labels are a compile error. `case *:` is the wildcard arm and must come last.',
  'pass': 'No-op placeholder statement — use it where a block needs at least one statement:\n\n```cstar\nif ready:\n    pass\n```',
  'and': 'Logical AND. **Returns `Bool`** and short-circuits: if the left operand is falsy, the right side is never evaluated.',
  'or': 'Logical OR. **Returns `Bool`** and short-circuits: if the left operand is truthy, the result is `True` without evaluating the right side.',
  'not': 'Logical NOT. Returns `Bool`: `not a` is `!truthy(a)`; truthiness is total (never an error).',
  'global': 'Declaration form (Stage 2B): `global name` / `global a, b` — later assignments in the function target the module global.\n\n```cstar\ncount = 0\nbump():\n    global count\n    count += 1\n```\n\nMust appear **before any binding or use** of the name in the function (else `name \'x\' is used before \'global\' declaration`). Reading a global needs no declaration; mutating an object\'s contents (`xs.append(1)`) is not a rebinding and needs no declaration. At top level / in `main:` it is allowed but redundant.',
  'nonlocal': 'Declaration form (Stage 2B): `nonlocal name` / `nonlocal a, b` — assignments target the **nearest enclosing function\'s** binding (used from a nested function to rebind an enclosing local).\n\n```cstar\nouter():\n    n = 0\n    inner():\n        nonlocal n\n        n += 1\n```\n\nMust appear **before any binding or use** of the name; without it, assigning to an enclosing local from a nested function creates/shadows a local instead. No enclosing binding → `no binding for nonlocal \'n\' found`. Mutating contents needs no declaration.',
  'try': 'Exception handling (Stage 2C). Block-form body; one or more `except` clauses and/or a `finally` (at least one required):\n\n```cstar\ntry:\n    risky(\nexcept ValueError as e):\n    print(e\nfinally):\n    print("always"\n)```\n\n`break`/`continue`/`return` crossing a `finally` run it first.',
  'except': 'Catches the pending exception. Matching is **exact class identity** (no inheritance): `except TypeError:` does not catch a `ValueError`; bare `except:` catches everything. `as e` binds the exception instance for the clause body (`.type` / `.message` attributes). A non-class except expression at runtime → `TypeError: except expects a class, got <t>`.',
  'finally': 'Runs exactly once per entry path — normal completion, `break`/`continue`/`return` out of the `try`, and exception unwind. Optional trailing clause of a `try`; a second one is a compile error (`duplicate \'finally\' clause`).',
  'raise': 'Raise an exception (Stage 2C): `raise e` with an instance (re-raise via the `as` binding), or a bare builtin exception class (`raise ValueError` — constructs an instance with an empty message). Anything else → `TypeError: exceptions must be class or instance, got <t>`; user classes/instances are not raisable. There is no bare `raise`.',
  'as': 'Binds the caught exception in an `except … as e:` clause (Stage 2C) — the only place `as` appears. The binding is scoped like other statement bindings and lives for the clause body:\n\n```cstar\ntry:\n    parse(data\nexcept ValueError as e):\n    print(e\n)```',
  'class': 'Declare a class (Stage 2C) — **block form, top level or `main:` only**, body holds methods (and `pass`) only:\n\n```cstar\nclass Point:\n    __init__(self, x, y):\n        self.x = x\n        self.y = y\n\n    move(self, dx, dy):\n        self.x += dx\n        self.y += dy\n```\n\n`self` is an explicit first parameter. Fields are dynamic (`p.x = 1` reads/writes any attribute); `Point(1, 2)` runs `__init__`. No inheritance, no class attributes; the class name is an ordinary guarded global (reads before the declaration runs → `global variable \'Point\' used before assignment`).',
  'is': 'Object-identity operator (Stage 2C): `a is b` / `a is not b` at comparison precedence. True iff the same heap object (instances, lists, …), shared storage for text/bytes, or equal non-heap values (`1 is 1` → True, `1 is 1.0` → False, `None is None` → True). Never a runtime error; never constant-folded.',
  '->': 'Retired — `->` is **not an operator** in v0.3. It spelled the call in pre-0.3 releases (and, before that, the `for` binder), but calls are now written `f(args)`, zero-argument calls `f()`, and declarations `name(args):` or `name():`.\n\n```cstar\nprint("hi")\nf(x, y)\np.step(i)\n```\nThe parser rejects it with \'->\' is not an operator; call with \'f(args)\'`.',
  'in': 'Membership / loop-binding operator. Membership — usable in `if`/`while` headers and mid-expression, returning `Bool` (a string target tests byte-substring containment):\n\n```cstar\nif x in range(5):\n    print("in range"\n)```\n\nBinding form in a `for` header:\n\n```cstar\nfor item in collection:\n    print(item\n)```\n',
  'import': 'Module import (Stage 2D): `import m` / `import m as x`. `m` may be dotted (`import lib.toolkit`). Imports resolve relative to the importing file, then the current working directory, then each `$CSTAR_PATH` entry (then the legacy `$PENTEST_PATH`) — and are top-level only. Binding the last dotted component as the module name.\n\n```cstar\nimport lib.toolkit\nimport lib.toolkit as tk\n```',
  'from': 'Selective module import (Stage 2D): `from m import a` / `from m import a as b` / `from m import a, b as c`. Imported names bind exactly like local declarations. Top-level only.\n\n```cstar\nfrom lib.ports import probe\nfrom AI import jev\n```',
};

// ---------------------------------------------------------------------------
// 2C — the twelve builtin exception classes (SPEC §6.10). Catch with exact
// class identity (no inheritance); instances expose `.type` (class-name
// text) and `.message` (raw text). Hover-only table.
// ---------------------------------------------------------------------------

const EXCEPTION_DOCS = {
  'Exception': 'The generic class for a user `raise Exception("…")` — never raised by the runtime itself. Matching is **exact**: `except Exception:` catches only exceptions whose class is exactly `Exception` (not `TypeError`, not `ValueError`, …); use a bare `except:` to catch everything. Instances expose `.type` and `.message`.',
  'TypeError': 'Wrong **type** for the operation: `len(5)`, `int([])`, calling a missing method (`\'<t>\' object has no method \'<m>\'`), `setattr` on a non-instance, unhashable keys. Raised by the runtime and most builtins.',
  'ValueError': 'Right type, bad **value**: `int("x")` (`invalid literal for int(): …`), `chr()` out of range or a surrogate, `open(path, "x")`, `pow(a, b, mod)` with `mod ≤ 0`, `max()`/`min()` over an empty sequence.',
  'NameError': 'Reading a variable before it was assigned — the guard raises it (`global variable \'x\' used before assignment`, local form likewise), including a `variable.<qualifier>` target that was never assigned.',
  'ZeroDivisionError': 'Integer `/` or `%` by zero (`division by zero`); also `divmod(a, 0)`.',
  'IndexError': 'List/tuple index out of range (`list index out of range`); negative indices count from the end.',
  'KeyError': 'Missing map key — `m[k]` and `map.pop(k)` without a default (`map key not found`).',
  'AttributeError': 'Attribute read/delete that misses: `e.nope` on an exception, `p.missing` on an instance, `delattr` on a missing field, deleting a class method (`… belongs to its class and cannot be deleted`). `getattr`/`hasattr` with a default never raise it.',
  'RuntimeError': 'Internal fallbacks (out of memory inside error reporting, unknown-class default) plus the always-failing stubs (`super()`, `property()`, `object()`, …) raise it with the reason (SPEC §5.11.6); user `raise RuntimeError("…")` also constructs it. Matching is **exact** — `except RuntimeError:` catches only `RuntimeError`; a bare `except:` catches everything.',
  'StopIteration': 'Raised by `next(iterator)` at the end of iteration when no default is given; `for` loops absorb it internally.',
  'FileNotFoundError': '`open(path)` on a path that does not exist (mode `r`); the message names the path.',
  'OSError': 'A file used the wrong way — writing to a file opened without `w`/`a` (`file not open for writing`).',
};

// ---------------------------------------------------------------------------
// 0.2.1 — cross-scope variable access `variable.<qualifier> <name>`. A
// contextual 4-token atom (NOT a reserved keyword: `variable` remains a
// legal identifier) — docs/reference/cross-scope-access.md.
// ---------------------------------------------------------------------------

const VARQUAL_DOC =
  '`variable.<qualifier> <name>` — read/write a variable in another scope, ' +
  'no `global`/`nonlocal` declaration needed (contextual form — `variable` ' +
  'itself stays a legal identifier):\n\n' +
  '- `variable.global n` — the **module global** `n`, from anywhere\n' +
  "- `variable.parent n` — the **enclosing function's** binding, from a nested function\n" +
  '- `variable.<fn> n` — a **sibling (or self) function\'s** binding, when the\n' +
  '  named function shares this scope (top-level ↔ top-level, nested ↔ nested)\n\n' +
  '```cstar\ng = 0\n\ntick():\n    variable.global g += 1\n```\n\n' +
  'Full rules: `docs/reference/cross-scope-access.md`.';

const VARQUAL_QUALIFIER_DOCS = {
  'global': '`variable.global n` — read/write the **module global** `n` from ' +
    'wherever you are. No `global` declaration, and every `variable.global` ' +
    'site and the owner share one storage:\n\n```cstar\ng = 0\ntick():\n    variable.global g += 1\n```',
  'parent': "`variable.parent n` — read/write the **enclosing function's** " +
    'binding `n` from a nested function. No `nonlocal` declaration needed:\n\n' +
    '```cstar\nouter():\n    n = 0\n    inner():\n        variable.parent n += 1\n```',
};

// ---------------------------------------------------------------------------
// Stage 2B — reserved call-only builtins (M2-2B §8). `print`/`range` keep
// their dedicated items below (2A behavior untouched).
// ---------------------------------------------------------------------------

const BUILTIN_DOCS = {
  'len': {
    sig: 'len(${1:value})',
    doc: '`len(x)` — length of **text** (code points), **bytes**, **list**, **map**, **set** or **tuple**. Anything else → `object of type \'<t>\' has no length()`.',
  },
  'str': {
    sig: 'str(${1:value})',
    doc: '`str(x)` — the display form of any value as **text** (bytes display as `b"..."`, collections as `[e1, e2]` / `{k: v}` / `{e}` / `(e1, e2)` / `()`; 2C: classes as `<class C>`, instances as `<C instance>`, bound methods as `<method \'m\' of C>`, exception instances as `C: message`).',
  },
  'int': {
    sig: 'int(${1:value})',
    doc: '`int(x)` — int from text (decimal parse; invalid → `invalid literal for int(): \'<s>\'`), float (truncates toward zero), bool (0/1), bytes (decimal parse); int passes through.',
  },
  'float': {
    sig: 'float(${1:value})',
    doc: '`float(x)` — float from text, int, bool or bytes. Invalid text → `invalid literal for float(): \'<s>\'`.',
  },
  'bool': {
    sig: 'bool(${1:value})',
    doc: '`bool(x)` — truthiness. Falsy: `False`, `None`, `0`, `0.0`, empty text/bytes/list/map/set/tuple/range; everything else is truthy — including classes, instances, bound methods and functions.',
  },
  'type': {
    sig: 'type(${1:value})',
    doc: '`type(x)` — the type name as text. The display names are: `none` `int` `float` `bool` `text` `bytes` `list` `map` `set` `tuple` `function` `range` `class` `method` — the Text type is displayed as text (2B renamed it from string). An **instance** (incl. an exception) reports its **class name** (`"Point"`, `"ValueError"`).',
  },
  'encode': {
    sig: 'encode(${1:text})',
    doc: '`encode(t)` — UTF-8 **bytes** from **text**. Wrong type → `encode() expects text, got <t>`.',
  },
  'decode': {
    sig: 'decode(${1:bytes})',
    doc: '`decode(b)` — **text** from **bytes** (validates UTF-8; invalid → `invalid UTF-8 in decode()`). Wrong type → `decode() expects bytes, got <t>`. Also available as the `bytes` method `.decode()`.',
  },
  'set': {
    sig: 'set(${1:iterable})',
    doc: '`set(iterable)` / `set()` — build a **set** from a list/tuple/text/bytes/... . `set()` is the only empty-set literal (`{}` is an empty **map**). Call-only and reserved, like `print`.',
  },
  'collect': {
    sig: 'collect()',
    doc: '`collect()` (Stage 2C) — force a full garbage collection and return the **number of blocks freed** (`int`, call-only and reserved). The only observability of the collector; programs must not depend on its value.',
  },
  'input': {
    sig: 'input(${1:prompt})',
    doc: '`input()` / `input(prompt)` — read one line from stdin as **text** without its trailing newline; `None` at end of input (blank line returns `""`). An optional prompt is printed first (str form, no newline, flushed). Call-only and reserved.',
  },
  'time': {
    sig: 'time()',
    doc: '`time()` — seconds since the Unix epoch as **float** (wall clock, Python `time.time()` shape). Call-only and reserved.',
  },
  'clock': {
    sig: 'clock()',
    doc: '`clock()` — **monotonic** seconds from an arbitrary origin as `float`; never goes backwards — use `(clock()) - t0` for elapsed time / loop timeouts. Call-only and reserved.',
  },
  'http_post': {
    sig: 'http_post(${1:url}, ${2:body})',
    doc: '`http_post(url, body)` / `http_post(url, body, headers)` — POST `body` (a JSON **text**) to `url`; the optional 3rd argument `headers` is a **text** of one `Name: value` header per line (blank lines skipped; `None` = defaults). Returns the response as **text** of the form `STATUS\\nBODY` (STATUS = numeric HTTP status, BODY = raw response body). A connection/TLS failure before any bytes returns status `0` with BODY = the error message. Default headers: `Content-Type: application/json` (suppressed when a caller header sets Content-Type), `Accept: application/json`, `User-Agent: jev-pipeline/1.0 (cstar)`. Call-only and reserved; backed by libcurl (handle reuse, keep-alive, HTTP/2–3). Reference client library: `extras/AI.pen` (`from AI import jev`).',
  },
  'http_get': {
    sig: 'http_get(${1:url})',
    doc: '`http_get(url)` / `http_get(url, headers)` — GET `url`; the optional 2nd argument `headers` is a **text** of one `Name: value` header per line (blank lines skipped; `None` = defaults). Returns the response as **text** of the form `STATUS\\nBODY` (STATUS = numeric HTTP status, BODY = raw response body). A connection/TLS failure before any bytes returns status `0` with BODY = the error message. Default headers: `Accept: application/json`, `User-Agent: jev-pipeline/1.0 (cstar)`. Call-only and reserved; backed by libcurl (handle reuse, keep-alive, HTTP/2–3).',
  },
  'url_encode': {
    sig: 'url_encode(${1:text})',
    doc: '`url_encode(t)` — percent-encode **text** for a query string / form body: the unreserved characters `A-Z a-z 0-9 - _ . ~` stay literal, every other UTF-8 byte becomes `%XX` (uppercase hex). Wrong type → `TypeError: url_encode() expects text, got <t>`. Call-only and reserved.',
  },

  // SIMD vectors — fixed-length float64 (`fvec`) / int64 (`ivec`) lanes.
  // Elementwise `+ - * / %` with the scalar operators' semantics; equal
  // lengths required (`ValueError: fvec length mismatch`); fvec and ivec
  // never mix (`TypeError`). Call-only, no named arguments.
  'fvec': {
    sig: 'fvec(${1:iterable})',
    doc: '`fvec(iterable)` — a SIMD **fvec** of float64 lanes from any iterable of numbers (`bool` counts as `0.0`/`1.0`). A non-number element → `TypeError: fvec() elements must be numbers, got <t>`. Elementwise `+ - * / %`, indexing, `v[i] = x`, iteration, `len`, `sum`/`min`/`max`; unhashable; slicing is a `TypeError`. Call-only and reserved.',
  },
  'ivec': {
    sig: 'ivec(${1:iterable})',
    doc: '`ivec(iterable)` — a SIMD **ivec** of int64 lanes; elements must be `int`/`bool` (`ivec([1.0])` → `TypeError: ivec() elements must be ints, got float`). Integer arithmetic wraps like scalar ints. Call-only and reserved.',
  },
  'fvec_fill': {
    sig: 'fvec_fill(${1:n}, ${2:v})',
    doc: '`fvec_fill(n, v)` — an **fvec** of `n` copies of the numeric scalar `v` (`n` must be a non-negative `int`). Call-only and reserved.',
  },
  'ivec_fill': {
    sig: 'ivec_fill(${1:n}, ${2:v})',
    doc: '`ivec_fill(n, v)` — an **ivec** of `n` copies of the `int`/`bool` scalar `v`. Call-only and reserved.',
  },
  'dot': {
    sig: 'dot(${1:a}, ${2:b})',
    doc: '`dot(a, b)` — inner product as a scalar: `float` for two `fvec`, `int` (wrapping) for two `ivec`. A length or type mismatch raises before any lane is read. Float results may differ in the last bits from a plain loop (vector-lane order / fused multiply-add). Call-only and reserved.',
  },

  // -------------------------------------------------------------------------
  // 0.2.1 — Python-parity builtins (SPEC §5.11.3). Same call-only/reserved
  // treatment as the legacy set; arity and named arguments are compile-time
  // checked (SPEC §5.11.1). Error wordings match CPython except cstar's
  // type names (text/None/map/class) — the documented deviations live in
  // SPEC §5.11.5.
  // -------------------------------------------------------------------------
  'abs': {
    sig: 'abs(${1:value})',
    doc: '`abs(x)` — absolute value of an `int` or `float`. `abs(INT64_MIN)` wraps per §5.3 (documented).',
  },
  'all': {
    sig: 'all(${1:iterable})',
    doc: '`all(it)` — `True` unless some element is falsy; empty → `True`; short-circuits on the first falsy element.',
  },
  'any': {
    sig: 'any(${1:iterable})',
    doc: '`any(it)` — `True` if some element is truthy; empty → `False`; short-circuits on the first truthy element.',
  },
  'ascii': {
    sig: 'ascii(${1:value})',
    doc: '`ascii(x)` — like `repr(x)` but every non-ASCII code point is escaped: `\\xNN` (< 0x100), `\\uNNNN` (< 0x10000), else `\\UNNNNNNNN`.',
  },
  'bin': {
    sig: 'bin(${1:value})',
    doc: '`bin(n)` — `0b1010` text for an `int` (`-0b…` for negatives). Non-int → `TypeError` `\'<t>\' object cannot be interpreted as an integer`.',
  },
  'bytes': {
    sig: 'bytes(${1:source})',
    doc: '`bytes()` / `bytes(n)` / `bytes(iterable)` / `bytes(b)` / `bytes(text, encoding="utf-8")` — empty, `n` zero bytes (bool → 0/1), one byte per 0..255 element, a copy, or a UTF-8 encode (**encoding required** for text; UTF-8 only). CPython wordings: `negative count`, `bytes must be in range(0, 256)`, `string argument without an encoding`.',
  },
  'callable': {
    sig: 'callable(${1:value})',
    doc: '`callable(x)` — `True` for function values, bound methods and classes (incl. exception classes); `False` otherwise. Never raises.',
  },
  'chr': {
    sig: 'chr(${1:codepoint})',
    doc: '`chr(i)` — one-character text for code point 0..0x10FFFF. Out of range → `ValueError`; surrogates rejected (UTF-8 cannot hold them) → `ValueError`.',
  },
  'delattr': {
    sig: 'delattr(${1:obj}, ${2:name})',
    doc: '`delattr(obj, name)` — delete one **dynamic instance field**; returns `None`. Missing field or non-instance → `AttributeError`; a class method → `AttributeError` (`… belongs to its class and cannot be deleted`).',
  },
  'dict': {
    sig: 'dict(${1:source})',
    doc: '`dict()` / `dict(source)` / `dict(a=1, b=2)` — build a **map**. At most one positional source (a map, or an iterable of 2-item pairs / a `zip` list) plus **arbitrary named keys** — named arguments are pairs here, not slot names.',
  },
  'dir': {
    sig: 'dir(${1:obj})',
    doc: '`dir()` — **sorted** names of the program globals table (in every scope — Python lists the local scope in functions; documented deviation). `dir(instance)` — sorted field + method names. Anything else → `TypeError` `\'<t>\' object has no attributes`.',
  },
  'divmod': {
    sig: 'divmod(${1:a}, ${2:b})',
    doc: '`divmod(a, b)` — `(quotient, remainder)` using cstar\'s C99 truncating `/` and `%` (**not** Python\'s floor — documented deviation): `divmod(-7, 2)` → `(-3, -1)`. `b == 0` → `ZeroDivisionError`.',
  },
  'enumerate': {
    sig: 'enumerate(${1:iterable})',
    doc: '`enumerate(it)` / `enumerate(it, start)` — **eager list** of `(index, item)` tuples (a list, not Python\'s iterator — documented deviation; iterate the pairs: `for p in enumerate(xs):`). `start` must be an `int`.',
  },
  'filter': {
    sig: 'filter(${1:function}, ${2:iterable})',
    doc: '`filter(fn, it)` — eager list of elements whose item is truthy; `fn` may be `None` (keeps truthy, like Python). Non-callable → `TypeError` `filter() arg 1 must be callable or None`. Callbacks are named `function` declarations (cstar has no lambdas).',
  },
  'format': {
    sig: 'format(${1:value}, ${2:spec})',
    doc: '`format(v)` / `format(v, spec)` — the Python format mini-language: `[[fill]align][sign][#][0][width][,][.precision][type]` with types `d i u b o x X c e E f F g G % s r`. Grouping counts from the right, `=`-align pads after the sign, a typeless int formats as `d`.',
  },
  'getattr': {
    sig: 'getattr(${1:obj}, ${2:name})',
    doc: '`getattr(obj, name)` / `getattr(obj, name, default)` — exactly what `obj.name` reads (instance field/method, exception `.type`/`.message`, file `.closed`/`.name`); with `default`, a miss returns it instead of raising `AttributeError`. `name` must be text.',
  },
  'globals': {
    sig: 'globals()',
    doc: '`globals()` — a **map** of the module globals (name → current value) snapshotted at the call site; unassigned names are skipped and top-level functions appear as values.',
  },
  'hasattr': {
    sig: 'hasattr(${1:obj}, ${2:name})',
    doc: '`hasattr(obj, name)` — `True` iff `getattr(obj, name)` without a default would not raise (one shared probe). Never raises on its own.',
  },
  'hash': {
    sig: 'hash(${1:value})',
    doc: '`hash(x)` — the runtime value hash as an `int` (§5.3 wrap). Unhashable (`list`/`map`/`set`, instances) → `TypeError` `unhashable type: \'<t>\'`.',
  },
  'hex': {
    sig: 'hex(${1:value})',
    doc: '`hex(n)` — `0x1f` text for an `int` (`-0x…` for negatives); same `TypeError` as `bin`.',
  },
  'id': {
    sig: 'id(${1:value})',
    doc: '`id(x)` — `int`: heap objects → their address with bit 63 set (disambiguating fold); `int`/`float`/`bool`/`None` → a deterministic value-tagged fold (`(id(1)) == (id(1))`, `(id(True)) != (id(1))`). Identical static text literals may share storage — and an id.',
  },
  'isinstance': {
    sig: 'isinstance(${1:value}, ${2:type})',
    doc: '`isinstance(x, t)` / `isinstance(x, (t1, t2))` — **exact** type match: no subclassing (`isinstance(True, int)` is `False`). `t` may be a bare builtin-type name (`int`, `text`, `type`, … — rewritten at compile time), a type-name string (`"int"` — a cstar extension Python rejects), an exception class, a user class, or a tuple of those. A shadowing parameter/local of that name wins.',
  },
  'iter': {
    sig: 'iter(${1:value})',
    doc: '`iter(x)` — boxed iterator: text/bytes by code point, list/tuple/set by element, map by key, range, file by line; `iter(iterator)` returns it unchanged (Python: `(iter(it)) is it`). Non-iterable → `TypeError` `\'<t>\' object is not iterable`.',
  },
  'list': {
    sig: 'list(${1:iterable})',
    doc: '`list(it)` / `list()` — eager **list** copy of any iterable; `list()` is `[]`.',
  },
  'locals': {
    sig: 'locals()',
    doc: '`locals()` — a **map** of the current frame: parameters + locals in declaration order (the globals table at top level); not-yet-assigned names are skipped.',
  },
  'map': {
    sig: 'map(${1:function}, ${2:iterable})',
    doc: '`map(fn, it, ...)` — eager list of `fn(...)` per index, stopping with the shortest input (like Python). `fn` is a named function declaration (`name(…):`; no lambdas).',
  },
  'max': {
    sig: 'max(${1:iterable})',
    doc: '`max(it)` / `max(a, b, ...)` / `max(..., key=fn, default=v)` — largest element. Empty without `default` → `ValueError` `max() arg is an empty sequence`; `key` must be callable or `None`.',
  },
  'min': {
    sig: 'min(${1:iterable})',
    doc: '`min(it)` / `min(a, b, ...)` / `min(..., key=fn, default=v)` — smallest element; mirror of `max` (`min() arg is an empty sequence`).',
  },
  'next': {
    sig: 'next(${1:iterator})',
    doc: '`next(it)` / `next(it, default)` — advance a boxed iterator one step; a **file** yields its next line including the trailing newline. Exhausted with `default` → `default`, without → `StopIteration`; non-iterator → `TypeError` `\'<t>\' object is not an iterator`.',
  },
  'oct': {
    sig: 'oct(${1:value})',
    doc: '`oct(n)` — `0o17` text for an `int` (`-0o…` for negatives); same `TypeError` as `bin`.',
  },
  'open': {
    sig: 'open(${1:path})',
    doc: '`open(path)` / `open(path, mode)` / `open(path, mode, encoding=…)` — a **file** value; modes `r`/`w`/`a` (+ optional `t`/`b`; `x` unsupported → `ValueError`), UTF-8 only. Missing file → `FileNotFoundError`. Methods: `read` `readline` `readlines` `write` `writelines` `seek` `tell` `flush` `close`; files iterate by line (`for line in f:`) and work with `next(f)`.',
  },
  'ord': {
    sig: 'ord(${1:char})',
    doc: '`ord(c)` — code-point `int` of a one-character text. Empty → `TypeError` `ord() expected a character, but string of length 0 found`; longer → `… of length <n> found`; non-text → `ord() expected string of length 1, but <t> found`.',
  },
  'pow': {
    sig: 'pow(${1:base}, ${2:exp})',
    doc: '`pow(a, b)` / `pow(a, b, mod)` — wrapping int power (§5.3) or `float`; the 3-arg form is int-only with an int modulus (`mod ≤ 0` → `ValueError`, negative exponent → `ValueError`) — `__int128` modular exponentiation.',
  },
  'repr': {
    sig: 'repr(${1:value})',
    doc: '`repr(x)` — the quoted/escaped form: text as a single-quoted string with the standard escapes (newline, tab, backslash, single quote — interior double quotes kept raw), collections quoting their inner text, bytes as `b"…"`; non-text → its display form.',
  },
  'reversed': {
    sig: 'reversed(${1:sequence})',
    doc: '`reversed(seq)` — **eager reversed list** of range/text/bytes/list/tuple/map (map → keys, like Python). Sets, iterators and anything else → `TypeError` `\'<t>\' object is not reversible`.',
  },
  'round': {
    sig: 'round(${1:value})',
    doc: '`round(x)` / `round(x, n)` — banker\'s half-even rounding. `int` + non-negative `n` → the value unchanged; `int` + negative `n` → rounded to that power of ten; `float` + `n` → `float`; no `n` → `int`.',
  },
  'setattr': {
    sig: 'setattr(${1:obj}, ${2:name}, ${3:value})',
    doc: '`setattr(obj, name, value)` — set a **dynamic instance field**; returns `None`. Non-instance → `TypeError` `cannot assign attribute \'<n>\' on \'<t>\'` — the same error as `obj.name = v` (documented deviation from Python\'s `AttributeError`).',
  },
  'slice': {
    sig: 'slice(${1:stop})',
    doc: '`slice(stop)` / `slice(start, stop)` / `slice(start, stop, step)` — returns the **`(start, stop, step)` tuple** (missing components → `None`); a single positional binds **stop** (`slice(5)` → `(None, 5, None)`). Inert value: `xs[slice_obj]` does not slice — use `xs[a:b]` (documented deviation).',
  },
  'sorted': {
    sig: 'sorted(${1:iterable})',
    doc: '`sorted(it)` / `sorted(it, key=fn, reverse=…)` — a new sorted **list**; stable merge sort. `key` callable or `None`; `reverse` is truth-tested (any truthy value reverses, like Python). Incomparable items → the `<` comparison `TypeError` (cstar orders only numbers/text — documented).',
  },
  'sum': {
    sig: 'sum(${1:iterable})',
    doc: '`sum(it)` / `sum(it, start)` — left fold with `+`: int/float arithmetic; a **text `start` concatenates** (`sum(["a","b"], "")` → `"ab"` — Python refuses; documented deviation).',
  },
  'tuple': {
    sig: 'tuple(${1:iterable})',
    doc: '`tuple(it)` / `tuple()` — eager **tuple** copy of any iterable; `tuple()` is `()`.',
  },
  'vars': {
    sig: 'vars(${1:obj})',
    doc: '`vars()` — like `locals()` (the frame map); `vars(instance)` — a map copy of its dynamic fields; anything else → `TypeError` `\'<t>\' object has no attributes`.',
  },
  'zip': {
    sig: 'zip(${1:iterables})',
    doc: '`zip(a, b, ...)` — **eager list** of tuples, one per index, stopping with the shortest input (a list, not Python\'s iterator — documented deviation); `zip()` is `[]`.',
  },

  // -------------------------------------------------------------------------
  // 0.2.1 — the nine always-failing stubs (SPEC §5.11.6): reserved, args
  // evaluate, then RuntimeError with the reason.
  // -------------------------------------------------------------------------
  'super': { sig: 'super()', doc: '`super()` — always raises `RuntimeError: super() is not supported: classes have no inheritance (SPEC §5.11.6)`.' },
  'property': { sig: 'property(${1:value})', doc: '`property(x)` — always raises `RuntimeError: property() is not supported: there are no descriptors or decorators`.' },
  'classmethod': { sig: 'classmethod(${1:fn})', doc: '`classmethod(f)` — always raises `RuntimeError: classmethod() is not supported: there are no decorators`.' },
  'staticmethod': { sig: 'staticmethod(${1:fn})', doc: '`staticmethod(f)` — always raises `RuntimeError: staticmethod() is not supported: there are no decorators`.' },
  'issubclass': { sig: 'issubclass(${1:cls}, ${2:class})', doc: '`issubclass(a, b)` — always raises `RuntimeError: issubclass() is not supported: classes have no inheritance`.' },
  'breakpoint': { sig: 'breakpoint()', doc: '`breakpoint()` — always raises `RuntimeError: breakpoint() is not supported: there is no interactive debugger`.' },
  'help': { sig: 'help()', doc: '`help()` — always raises `RuntimeError: help() is not supported: there is no interactive help (SPEC.md is the reference)`.' },
  'memoryview': { sig: 'memoryview(${1:bytes})', doc: '`memoryview(b)` — always raises `RuntimeError: memoryview() is not supported: bytes are immutable and have no buffer views`.' },
  'object': { sig: 'object()', doc: '`object()` — always raises `RuntimeError: object() is not supported: define your own class — every class is already a root`.' },

  // -------------------------------------------------------------------------
  // 0.2.1 — the dynamic-code trio: rejected at COMPILE time (SPEC §5.11.6).
  // -------------------------------------------------------------------------
  'eval': { sig: 'eval(${1:source})', doc: '`eval(source)` — **not available in cstar**: programs are compiled ahead of time and the runtime never compiles source. Calling it is a compile-time error (SPEC §5.11.6).' },
  'exec': { sig: 'exec(${1:source})', doc: '`exec(source)` — **not available in cstar**: calling it is a compile-time error — no embedded parser/interpreter in the runtime (SPEC §5.11.6).' },
  'compile': { sig: 'compile(${1:source})', doc: '`compile(source, filename, mode)` — **not available in cstar**: calling it is a compile-time error — the runtime never compiles source (SPEC §5.11.6).' },
};

// ---------------------------------------------------------------------------
// Stage 2B — method names after `.` (M2-2B §15; the runtime dispatch in
// runtime/cstar_rt.c is the source of truth). Only names the contract
// lists are here; user-class methods arrive in 2C.
// ---------------------------------------------------------------------------

const METHOD_DOCS = {
  // list
  'append': {
    types: 'list',
    doc: '`list.append(v)` — append one value to the end (mutates, returns None).',
  },
  'insert': {
    types: 'list',
    doc: '`list.insert(i, v)` — insert before index `i` (Python-style clamped; mutates, returns None).',
  },
  'index': {
    types: 'list',
    doc: '`list.index(v)` — index of the first element equal to `v`, else error `value not in list`.',
  },
  'reverse': {
    types: 'list',
    doc: '`list.reverse()` — reverse in place; returns None.',
  },
  'pop': {
    types: 'list, map, set',
    doc:
      '`list.pop()` / `list.pop(i)` / `list.pop(i, default)` — remove and return the last (or i-th, negative counts from the end) element; with a default, an out-of-range index returns it instead of erroring.\n\n' +
      '`map.pop(k)` / `map.pop(k, default)` — remove and return the value for `k` (missing without default → `map key not found`).\n\n' +
      '`set.pop()` — remove and return the first element in insertion order (empty set → `pop from an empty set`).',
  },
  'remove': {
    types: 'list, set',
    doc:
      '`list.remove(v)` — remove the first element equal to `v`, else error `value not in list`.\n\n' +
      '`set.remove(v)` — remove `v`, else error `value not in set`.',
  },
  'clear': {
    types: 'list, map, set',
    doc: '`clear()` — empty the collection in place; returns None. Works on list, map and set.',
  },
  // map
  'get': {
    types: 'map',
    doc: '`map.get(k)` / `map.get(k, default)` — value for key `k`; missing → `default`, or None when no default.',
  },
  'keys': {
    types: 'map',
    doc: '`map.keys()` — list of keys in insertion order (map iteration yields keys too).',
  },
  'values': {
    types: 'map',
    doc: '`map.values()` — list of values in insertion order.',
  },
  'items': {
    types: 'map',
    doc: '`map.items()` — list of `(key, value)` tuples in insertion order.',
  },
  // set
  'add': {
    types: 'set',
    doc: '`set.add(v)` — insert `v` (duplicates ignored); returns None.',
  },
  'discard': {
    types: 'set',
    doc: '`set.discard(v)` — remove `v` if present; never errors when absent.',
  },
  // text
  'upper': {
    types: 'text',
    doc: '`text.upper()` — uppercased copy (ASCII-only case mapping — documented 2B limitation).',
  },
  'lower': {
    types: 'text',
    doc: '`text.lower()` — lowercased copy (ASCII-only).',
  },
  'split': {
    types: 'text',
    doc: '`text.split(sep)` — split into a list of parts around a non-empty text `sep`.',
  },
  'join': {
    types: 'text',
    doc: '`sep.join(iterable)` — join text elements with the separator (called on the separator text).',
  },
  'find': {
    types: 'text',
    doc: '`text.find(sub)` — code-point index of the first occurrence of `sub`, or -1.',
  },
  'replace': {
    types: 'text',
    doc: '`text.replace(old, new)` — replace every occurrence of a non-empty `old` (returns a new text).',
  },
  'strip': {
    types: 'text',
    doc: '`text.strip()` — copy with leading/trailing ASCII whitespace removed.',
  },
  'starts_with': {
    types: 'text',
    doc: '`text.starts_with(p)` — Bool prefix test; `p` must be text.',
  },
  'ends_with': {
    types: 'text',
    doc: '`text.ends_with(p)` — Bool suffix test; `p` must be text.',
  },
  // bytes
  'decode': {
    types: 'bytes',
    doc: '`bytes.decode()` — no arguments; validating UTF-8 decode to text (same as the `decode()` builtin).',
  },
};

// True when the cursor sits on a method access `value.<word>` — the value may
// be a name, a call result `(f()).` or an index `xs[0].`; a digit before the
// dot that is NOT part of an identifier means a float literal (`x = 1.`), not
// a method access.
function isMethodAccess(linePrefix) {
  const m = /\.\s*([A-Za-z0-9_]*)$/.exec(linePrefix);
  if (!m) return false;
  if (/^[0-9]+$/.test(m[1])) return false; // typing the fractional part of a float
  const before = linePrefix.charAt(m.index - 1);
  if (!/[A-Za-z0-9_)\]]/.test(before)) return false;
  if (/[0-9]/.test(before)) {
    // Digit before the dot: identifier tail (`x1.`) is fine, a bare number
    // (`1.` while typing a float) is not.
    let i = m.index - 1;
    while (i >= 0 && /[0-9]/.test(linePrefix.charAt(i))) i--;
    if (!/[A-Za-z_]/.test(linePrefix.charAt(i))) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Run (TODO.txt: working Run button for .pen)
// ---------------------------------------------------------------------------

// Lazily created and reused across runs so history and scrollback live in
// one place; recreated after the user closes it (see onDidCloseTerminal in
// activate()). A real terminal (not a pipe) keeps stdin interactive, so
// input() prompts are answerable.
let runTerminal;

function ensureRunTerminal() {
  if (!runTerminal) runTerminal = vscode.window.createTerminal('cstar');
  return runTerminal;
}

// Settings live under `cstar.*`. The legacy `pentest.*` keys (kept until
// v0.4) are still honoured for any key the user has not set under cstar.
function settingIsSet(info) {
  return !!info && [
    'globalValue', 'workspaceValue', 'workspaceFolderValue',
    'globalLanguageValue', 'workspaceLanguageValue',
    'workspaceFolderLanguageValue',
  ].some((k) => info[k] !== undefined);
}

function setting(key, def, scope) {
  const cfg = vscode.workspace.getConfiguration('cstar', scope);
  if (typeof cfg.inspect !== 'function') return cfg.get(key, def);
  if (settingIsSet(cfg.inspect(key))) return cfg.get(key, def);
  const legacy = vscode.workspace.getConfiguration('pentest', scope);
  if (typeof legacy.inspect === 'function' && settingIsSet(legacy.inspect(key))) {
    return legacy.get(key, def);
  }
  return def;
}

// The configured CLI name (default `cstar`), plus the legacy `pentest`
// command as a fallback when the default is in use.
function cliNames() {
  const exe = (setting('executable', 'cstar') || 'cstar').trim();
  return exe === 'cstar' ? ['cstar', 'pentest'] : [exe];
}

// Where a bare CLI name is looked for: PATH, ~/.local/bin, and the
// developer checkout's build folder.
function cliLocations(name) {
  const home = os.homedir();
  return [
    name,
    path.join(home, '.local', 'bin', name),
    path.join(home, 'Projects', 'Pentest', 'build', name),
  ];
}

function resolveRunCli() {
  const names = cliNames();
  if (names[0].includes('/')) return names[0]; // explicit path — run as-is
  for (const name of names) {
    for (const loc of cliLocations(name).slice(1)) {
      if (fs.existsSync(loc)) return loc;
    }
  }
  return names[0]; // last resort: whatever the terminal's PATH finds
}

// Double-quote a path for the user's shell so spaces and the usual
// metacharacters stay literal.
function shellQuote(s) {
  return '"' + String(s).replace(/([\\$"`])/g, '\\$1') + '"';
}

// Runs THE FILE THE DEVELOPER IS ON: the resource the ▶ button / context
// menu was clicked for (VSCode passes it as the first argument) — falling
// back to the active editor for palette/keybinding invocations. Dirty
// documents are saved first, so the terminal never runs stale code.
async function runActiveFile(resource) {
  let doc;
  if (resource && typeof resource.fsPath === 'string') {
    doc = vscode.workspace.textDocuments.find(
      (d) => d.uri && d.uri.toString() === resource.toString()
    );
    if (!doc && typeof vscode.workspace.openTextDocument === 'function') {
      try {
        doc = await vscode.workspace.openTextDocument(resource);
      } catch (e) {
        doc = undefined;
      }
    }
  }
  if (!doc) {
    const editor = vscode.window.activeTextEditor;
    doc = editor && editor.document;
  }
  if (!doc) {
    vscode.window.showInformationMessage('cstar: open a .cst file to run.');
    return;
  }
  if (doc.languageId !== 'cstar') {
    vscode.window.showInformationMessage(
      'cstar: Run File works on .cst documents (and legacy .pen/.pent).'
    );
    return;
  }
  if (doc.isUntitled) {
    vscode.window.showWarningMessage('cstar: save the file before running.');
    return;
  }
  if (doc.isDirty) {
    let saved = false;
    try {
      saved = await doc.save();
    } catch (e) {
      saved = false;
    }
    if (!saved) {
      vscode.window.showErrorMessage(
        'cstar: could not save the file — run cancelled.'
      );
      return;
    }
  }
  const cli = resolveRunCli();
  const command = shellQuote(cli) + ' run ' + shellQuote(doc.uri.fsPath);
  let term = ensureRunTerminal();
  try {
    term.show();
    term.sendText(command);
  } catch (e) {
    // The terminal was closed/disposed after all — recreate it so the Run
    // button keeps working for the file being developed.
    runTerminal = undefined;
    term = ensureRunTerminal();
    term.show();
    term.sendText(command);
  }
}

// ---------------------------------------------------------------------------
// Activation
// ---------------------------------------------------------------------------

function activate(context) {
  outputChannel = vscode.window.createOutputChannel('cstar');
  diagCollection = vscode.languages.createDiagnosticCollection('cstar');
  context.subscriptions.push(outputChannel, diagCollection);

  context.subscriptions.push(
    vscode.languages.registerCompletionItemProvider(
      { language: 'cstar' },
      { provideCompletionItems },
      '.' // Stage 2B: pop method completions right after `value.`
    )
  );

  // Hover: the same doc tables on pointer-over — builtins (signature +
  // contract), keywords/constants, the twelve exception classes, method
  // names right of a dot, the `variable.` qualifier form, and user
  // function (`name():` / `name(params):`) and class declarations
  // (signature + declaration line).
  context.subscriptions.push(
    vscode.languages.registerHoverProvider(
      { language: 'cstar' },
      { provideHover }
    )
  );

  // `cstar.*` commands, plus the legacy `pentest.*` ids as aliases (until
  // v0.4) so existing keybindings keep working.
  for (const prefix of ['cstar', 'pentest']) {
    context.subscriptions.push(
      vscode.commands.registerCommand(prefix + '.toggleLineComment', toggleLineComment)
    );
    context.subscriptions.push(
      vscode.commands.registerCommand(prefix + '.run', runActiveFile)
    );
  }

  // A closed "cstar" terminal must not leave a dangling handle behind —
  // the next Run click then recreates it (ensureRunTerminal + the sendText
  // fallback in runActiveFile).
  context.subscriptions.push(
    vscode.window.onDidCloseTerminal((t) => {
      if (t === runTerminal) runTerminal = undefined;
    })
  );

  context.subscriptions.push(
    vscode.workspace.onDidChangeTextDocument((e) => schedule(e.document))
  );
  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument((d) => schedule(d))
  );
  context.subscriptions.push(
    vscode.workspace.onDidSaveTextDocument((d) => schedule(d, 0))
  );
  context.subscriptions.push(
    vscode.workspace.onDidCloseTextDocument((d) => {
      clearPending(d);
      diagCollection.delete(d.uri);
    })
  );

  for (const doc of vscode.workspace.textDocuments) schedule(doc, 0);
}

function deactivate() {
  for (const t of pending.values()) clearTimeout(t);
  pending.clear();
  if (runTerminal) {
    runTerminal.dispose();
    runTerminal = undefined;
  }
}

// ---------------------------------------------------------------------------
// Autocomplete
// ---------------------------------------------------------------------------

function provideCompletionItems(document, position) {
  try {
    return buildCompletions(document, position);
  } catch (err) {
    outputChannel.appendLine(`completion provider error: ${err && err.message}`);
    return [];
  }
}

// v0.3 declarations have no `function` keyword: a function header is
// `name():` or `name(params):`. Scan line-by-line (the extension has no
// parser) and mark headers inside a class body as methods. Blank/comment
// lines do not close a class body; a dedent to the class's indent does.
const DECL_KEYWORDS =
  'if|elif|else|for|while|switch|case|try|except|finally|class|main|' +
  'return|pass|break|continue|raise|global|nonlocal|and|or|not|is|in|as';

function functionDeclarations(text) {
  const out = [];
  const lines = text.split('\n');
  const fnRe = new RegExp(
    '^(\\s*)(?!(?:' + DECL_KEYWORDS + ')\\b)' +
    '([A-Za-z_][A-Za-z0-9_]*)(\\s*(\\s*.*?))?\\s*:' +
    '(?:\\s*(?:#.*|//.*)?)?$'
  );
  const clsRe = /^(\s*)class\s+([A-Za-z_][A-Za-z0-9_]*)\s*:/;
  let classIndent = -1;
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (/^\s*$/.test(line) || /^\s*(?:#|\/\/)/.test(line)) continue;
    const indent = (/^\s*/.exec(line) || [''])[0].length;
    const cm = clsRe.exec(line);
    if (cm) {
      classIndent = cm[1].length;
      out.push({
        name: cm[2],
        kind: 'class',
        sig: 'class ' + cm[2] + ':',
        line: i,
      });
      continue;
    }
    if (classIndent >= 0 && indent <= classIndent) classIndent = -1;
    const fm = fnRe.exec(line);
    if (fm) {
      out.push({
        name: fm[2],
        kind: classIndent >= 0 && indent > classIndent ? 'method' : 'function',
        sig: fm[2] + (fm[3] || '') + ':',
        line: i,
      });
    }
  }
  return out;
}

// Discover class methods from real source so after-`.` completion is derived,
// not hardcoded. Sources: classes declared in the current file, and classes
// in module files it imports (`import m` / `from m import …`).
function scanClasses(text) {
  const classes = {};
  const lines = text.split('\n');
  const clsRe = /^(\s*)class\s+([A-Za-z_][A-Za-z0-9_]*)\s*:/;
  const methRe = /^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\([^)]*\)\s*:/;
  let cur = null;
  let classIndent = -1;
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (/^\s*(?:#|\/\/)/.test(line) || /^\s*$/.test(line)) continue;
    const cm = clsRe.exec(line);
    if (cm) {
      cur = cm[2];
      classIndent = cm[1].length;
      classes[cur] = classes[cur] || [];
      continue;
    }
    if (cur !== null && classIndent >= 0) {
      const indent = (/^\s*/.exec(line) || [''])[0].length;
      if (indent <= classIndent) {
        cur = null;
        classIndent = -1;
        continue;
      }
      const mm = methRe.exec(line);
      if (mm && !classes[cur].includes(mm[1])) classes[cur].push(mm[1]);
    }
  }
  return classes;
}

function importLines(text) {
  const out = [];
  const fromRe = /^\s*from\s+([A-Za-z0-9_.]+)\s+import\s+(.+)$/gm;
  let m;
  while ((m = fromRe.exec(text)) !== null) out.push({ module: m[1] });
  const importRe = /^\s*import\s+([A-Za-z0-9_.]+)(?:\s+as\s+[A-Za-z0-9_]+)?\s*$/gm;
  while ((m = importRe.exec(text)) !== null) out.push({ module: m[1] });
  return out;
}

function resolveModule(spec, docDir) {
  const base = spec.replace(/\./g, '/');
  const dirs = [docDir];
  // $CSTAR_PATH first, then the legacy $PENTEST_PATH (until v0.4).
  for (const v of ['CSTAR_PATH', 'PENTEST_PATH']) {
    if (!process.env[v]) continue;
    for (const d of String(process.env[v]).split(':')) {
      if (d) dirs.push(d);
    }
  }
  dirs.push(process.cwd());
  for (const d of dirs) {
    // `.cst` first; the legacy `.pen` / `.pent` spellings until v0.4.
    for (const ext of ['.cst', '.pen', '.pent']) {
      try {
        const p = path.join(d, base + ext);
        if (fs.existsSync(p)) return p;
      } catch (e) {
        /* ignore unreadable/unsafe path */
      }
    }
  }
  return null;
}

function buildCompletions(document, position) {
  const items = [];

  // Replace-range: the word being typed (fall back to an empty range at the
  // cursor if not on a word).
  const wordRange = document.getWordRangeAtPosition(position) ||
    new vscode.Range(position, position);
  const applyRange = (item) => { item.range = wordRange; };

  // Stage 2B: after `value.` offer ONLY method names (the receiver's static
  // type is unknown here, so the full per-type surface is listed; `detail`
  // says which types own the name).
  const lineText = document.lineAt(position.line).text.slice(0, position.character);

  // 0.2.1: after `variable.` offer the QUALIFIER surface (global / parent /
  // sibling function names) — not the method list: `variable.` is the
  // cross-scope atom, not a receiver (docs/reference/cross-scope-access.md §3).
  const vqM = /(?:^|[^\w.])variable\.([A-Za-z0-9_]*)$/.exec(lineText);
  if (vqM) {
    const qualifierItem = (label, detail, doc, kind, sort) => {
      const it = new vscode.CompletionItem(label, kind);
      it.detail = detail;
      it.documentation = new vscode.MarkdownString(doc);
      it.sortText = sort;
      applyRange(it);
      items.push(it);
    };
    qualifierItem('global', 'variable.global n — module global',
      VARQUAL_QUALIFIER_DOCS.global, vscode.CompletionItemKind.Keyword, '0_q_global');
    qualifierItem('parent', 'variable.parent n — enclosing function binding',
      VARQUAL_QUALIFIER_DOCS.parent, vscode.CompletionItemKind.Keyword, '0_q_parent');
    const qseen = new Set();
    for (const decl of functionDeclarations(document.getText())) {
      if (decl.kind !== 'function') continue; // class methods are not qualifiers
      const fn = decl.name;
      if (fn === 'global' || fn === 'parent' || qseen.has(fn)) continue;
      qseen.add(fn);
      qualifierItem(fn, `variable.${fn} n — binding of function ${fn}`,
        `\`variable.${fn} n\` — read/write the binding of the sibling (or self) ` +
        `function \`${fn}\`. \`${fn}\` must **share this scope** — top-level ↔ ` +
        'top-level, nested ↔ nested — and naming an enclosing function by name ' +
        'redirects to `variable.parent`. The compiler reports ' +
        '`\'<fn>\' is not a function sharing this scope` otherwise.',
        vscode.CompletionItemKind.Function, '1_q_' + fn);
    }
    return items;
  }

  if (isMethodAccess(lineText)) {
    for (const name of Object.keys(METHOD_DOCS)) {
      const spec = METHOD_DOCS[name];
      const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Method);
      item.detail = `method — ${spec.types}`;
      item.documentation = new vscode.MarkdownString(spec.doc);
      item.sortText = '0_m_' + name;
      applyRange(item);
      items.push(item);
    }

    // Derived methods (no hardcoding): classes declared in this file, plus
    // classes in the module files it imports (`import m` / `from m import …`).
    const docDir = path.dirname(document.uri.fsPath);
    const allClasses = scanClasses(document.getText());
    for (const im of importLines(document.getText())) {
      const modPath = resolveModule(im.module, docDir);
      if (!modPath) continue;
      let childText = '';
      try {
        childText = fs.readFileSync(modPath, 'utf8');
      } catch (e) {
        continue;
      }
      const childClasses = scanClasses(childText);
      for (const cn of Object.keys(childClasses)) {
        const base = allClasses[cn] || [];
        for (const methodName of childClasses[cn]) {
          if (!base.includes(methodName)) base.push(methodName);
        }
        allClasses[cn] = base;
      }
    }
    const seen = new Set();
    for (const cn of Object.keys(allClasses)) {
      for (const methodName of allClasses[cn]) {
        if (methodName.startsWith('_')) continue; // internal/dunder
        if (seen.has(methodName)) continue;
        seen.add(methodName);
        const item = new vscode.CompletionItem(methodName, vscode.CompletionItemKind.Method);
        item.detail = `method — ${cn}`;
        item.documentation = new vscode.MarkdownString(
          `\`${cn}.${methodName}(…)\` — a method of the \`${cn}\` class, derived from source.`
        );
        item.sortText = '0_c_' + cn + '_' + methodName;
        applyRange(item);
        items.push(item);
      }
    }
    return items;
  }

  for (const kw of Object.keys(KEYWORD_DOCS)) {
    const item = new vscode.CompletionItem(kw, vscode.CompletionItemKind.Keyword);
    item.detail = /^[A-Za-z_]/.test(kw) ? 'cstar keyword' : 'cstar operator';
    item.documentation = new vscode.MarkdownString(KEYWORD_DOCS[kw]);
    item.sortText = '0_' + kw;
    applyRange(item);
    items.push(item);
  }

  const print = new vscode.CompletionItem('print', vscode.CompletionItemKind.Function);
  print.detail = 'builtin function';
  print.documentation = new vscode.MarkdownString(
    '`print(…)` — print any number of values, space-separated, newline-terminated.'
  );
  print.insertText = new vscode.SnippetString('print(${1:value})');
  print.sortText = '0_print';
  applyRange(print);
  items.push(print);

  const range = new vscode.CompletionItem('range', vscode.CompletionItemKind.Function);
  range.detail = 'builtin function';
  range.documentation = new vscode.MarkdownString(
    '`range(stop)` / `range(start, stop)` / `range(start, stop, step)` — lazy int ' +
      'range for `for` loops and `in` membership. **Stop is exclusive** (default ' +
      'start=0, step=1), all arguments must be int, and `step` must not be 0.\n\n' +
      '```cstar\nfor i in range(5):\n    print(i\n)```'
  );
  range.insertText = new vscode.SnippetString('range(${1:stop})');
  range.sortText = '0_range';
  applyRange(range);
  items.push(range);

  // Stage 2B reserved call-only builtins (§8): like print/range — call
  // targets only, cannot be read as values or assigned.
  for (const name of Object.keys(BUILTIN_DOCS)) {
    const spec = BUILTIN_DOCS[name];
    const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Function);
    item.detail = 'builtin function (call-only, reserved)';
    item.documentation = new vscode.MarkdownString(spec.doc);
    item.insertText = new vscode.SnippetString(spec.sig);
    item.sortText = '0_' + name;
    applyRange(item);
    items.push(item);
  }

  // 0.2.1: the cross-scope construct itself. `variable` is contextual — NOT
  // reserved — so a plain-word duplicate is suppressed in the document-word
  // pass below; accepting this item inserts the whole qualified shape.
  const vqItem = new vscode.CompletionItem('variable', vscode.CompletionItemKind.Keyword);
  vqItem.detail = 'cstar contextual form — cross-scope variable access';
  vqItem.documentation = new vscode.MarkdownString(VARQUAL_DOC);
  vqItem.insertText = new vscode.SnippetString('variable.${1:global} ${2:name}');
  vqItem.sortText = '0_variable';
  applyRange(vqItem);
  items.push(vqItem);

  // Every identifier in the document: declarations (`name():` / `name(params):`
  // and `class Name:`) keep their kind; everything else is a variable. Keeps
  // the suggest box populated with real project symbols even when word-based
  // suggestions are disabled.
  const text = document.getText();
  const declared = new Map();
  for (const decl of functionDeclarations(text)) {
    if (!declared.has(decl.name)) declared.set(decl.name, decl.kind);
  }

  const seen = new Set();
  const wordRe = /[A-Za-z_][A-Za-z0-9_]*/g;
  let m;
  while ((m = wordRe.exec(text)) !== null) {
    const w = m[0];
    // `variable` gets the richer contextual item above (it is not reserved,
    // but offering both spellings as separate entries is just noise).
    if (RESERVED.has(w) || w === 'variable' || seen.has(w)) continue;
    seen.add(w);
    const kind = declared.get(w);
    let itemKind = vscode.CompletionItemKind.Variable;
    let detail = 'variable';
    if (kind === 'function') {
      itemKind = vscode.CompletionItemKind.Function;
      detail = 'function';
    } else if (kind === 'method') {
      itemKind = vscode.CompletionItemKind.Method;
      detail = 'method';
    } else if (kind === 'class') {
      itemKind = vscode.CompletionItemKind.Class;
      detail = 'class';
    }
    const item = new vscode.CompletionItem(w, itemKind);
    item.detail = detail;
    item.sortText = (kind ? '1_' : '2_') + w;
    applyRange(item);
    items.push(item);
  }

  return items;
}

// ---------------------------------------------------------------------------
// Line-comment toggling for BOTH styles: `#` and `//`
// ---------------------------------------------------------------------------
//
// VSCode's language configuration accepts exactly ONE `comments.lineComment`
// token (which is `#`, so the built-in Ctrl+/ keeps working as before), so
// lines commented with `//` are handled by this command instead. It is
// registered at runtime as `cstar.toggleLineComment`; bind it to a key in
// keybindings.json if you want one shortcut for both styles, e.g.
//   { "key": "ctrl+/", "command": "cstar.toggleLineComment",
//     "when": "editorTextFocus && editorLangId == cstar" }

function toggleLineComment() {
  try {
    const editor = vscode.window.activeTextEditor;
    if (!editor || editor.document.languageId !== 'cstar') return;
    const doc = editor.document;

    const selections = (editor.selections && editor.selections.length)
      ? editor.selections
      : (editor.selection ? [editor.selection] : []);
    if (!selections.length) return;

    let startLine = Number.MAX_SAFE_INTEGER;
    let endLine = -1;
    for (const sel of selections) {
      let s = sel.start.line;
      let e = sel.end.line;
      if (e > s && sel.end.character === 0) e--; // selection ends at col 0 of the next line
      if (s < startLine) startLine = s;
      if (e > endLine) endLine = e;
    }
    const total = doc.lineCount; // property (NOT a function) in the API
    if (endLine < startLine || startLine >= total) return;

    const lines = [];
    for (let i = startLine; i <= endLine && i < total; i++) {
      lines.push({ line: i, text: doc.lineAt(i).text });
    }

    const markerRe = /^(\s*)(#|\/\/)( ?)/;
    // All non-blank lines already commented (either style) → uncomment.
    const allCommented = lines.every(
      (l) => /^\s*$/.test(l.text) || markerRe.test(l.text)
    );

    const edit = editor.edit((builder) => {
      for (const l of lines) {
        if (allCommented) {
          const m = l.text.match(markerRe);
          if (m) {
            builder.replace(
              new vscode.Range(l.line, 0, l.line, l.text.length),
              m[1] + l.text.slice(m[0].length)
            );
          }
        } else if (!/^\s*$/.test(l.text)) {
          const indent = l.text.match(/^\s*/)[0];
          builder.replace(
            new vscode.Range(l.line, 0, l.line, l.text.length),
            indent + '# ' + l.text.slice(indent.length)
          );
        }
      }
    });
    if (edit && typeof edit.catch === 'function') {
      edit.catch((err) => {
        if (outputChannel) {
          outputChannel.appendLine(`toggle comment error: ${err && err.message}`);
        }
      });
    }
  } catch (err) {
    if (outputChannel) {
      outputChannel.appendLine(`toggle comment error: ${err && err.message}`);
    }
  }
}

// ---------------------------------------------------------------------------
// Live diagnostics (cstar check — parse + semantic only)
// ---------------------------------------------------------------------------

function schedule(doc, delay) {
  if (doc.languageId !== 'cstar' || doc.isClosed) return;
  if (!setting('diagnostics', true, doc.uri)) {
    clearPending(doc);
    diagCollection.delete(doc.uri);
    return;
  }
  clearPending(doc);
  const ms = delay !== undefined ? delay : setting('diagnosticsDelay', 400, doc.uri);
  const t = setTimeout(() => {
    pending.delete(doc.uri.toString());
    runCheck(doc);
  }, ms);
  pending.set(doc.uri.toString(), t);
}

function clearPending(doc) {
  const t = pending.get(doc.uri.toString());
  if (t !== undefined) {
    clearTimeout(t);
    pending.delete(doc.uri.toString());
  }
}

function cliCandidates() {
  const names = cliNames();
  if (names[0].includes('/')) return [names[0]];
  // cstar locations first, then the legacy `pentest` ones.
  const list = [].concat(...names.map(cliLocations));
  return list.filter((v, i) => list.indexOf(v) === i);
}

function runCheck(doc) {
  if (doc.isClosed) return;
  const version = doc.version;
  const text = doc.getText();
  // The temp copy lives outside the project, so module imports must still
  // resolve as if the file were in its real folder. Run the check from the
  // document's own directory and put that directory first on $CSTAR_PATH
  // (and on the legacy $PENTEST_PATH, for an older pentest CLI).
  const docDir = path.dirname(doc.uri.fsPath);
  const tmpFile = path.join(
    os.tmpdir(),
    `cstar-check-${process.pid}-${++checkCounter}-${Date.now()}.cst`
  );

  fs.writeFile(tmpFile, text, (err) => {
    if (err) {
      outputChannel.appendLine(`cannot write temp file: ${err.message}`);
      return;
    }
    const cleanup = () => fs.unlink(tmpFile, () => {});
    const candidates = cliCandidates();

    const attempt = (i) => {
      if (i >= candidates.length) {
        if (!cliWarned) {
          cliWarned = true;
          vscode.window.showWarningMessage(
            'cstar CLI not found — live error diagnostics are disabled. ' +
              'Set "cstar.executable" in Settings.'
          );
          outputChannel.appendLine(
            'cstar CLI not found; tried: ' + candidates.join(', ')
          );
        }
        cleanup();
        return;
      }

      let settled = false;
      let stderr = '';
      const child = spawn(candidates[i], ['check', tmpFile], {
        cwd: docDir,
        env: Object.assign({}, process.env, {
          CSTAR_PATH: [docDir, process.env.CSTAR_PATH]
            .filter(Boolean)
            .join(':'),
          PENTEST_PATH: [docDir, process.env.PENTEST_PATH]
            .filter(Boolean)
            .join(':'),
        }),
      });
      child.stdout.on('data', () => {});
      child.stderr.on('data', (d) => {
        stderr += d;
      });
      child.on('error', () => {
        if (settled) return;
        settled = true;
        attempt(i + 1); // not found at this candidate — try the next one
      });
      child.on('close', (code) => {
        if (settled) return;
        settled = true;
        cleanup();
        if (code === 0 || code === 1) {
          applyDiagnostics(doc, version, code === 1 ? stderr : '');
        } else {
          outputChannel.appendLine(
            `cstar check exited ${code}: ${stderr.trim()}`
          );
        }
      });
    };

    attempt(0);
  });
}

const LOCATED_ERROR = /^(.+?):(\d+):(\d+): error: (.+)$/;

function applyDiagnostics(doc, version, stderr) {
  try {
    applyDiagnosticsInner(doc, version, stderr);
  } catch (err) {
    // Never let a diagnostics failure fail silently again.
    outputChannel.appendLine(`diagnostics error: ${err && err.message}`);
  }
}

function applyDiagnosticsInner(doc, version, stderr) {
  // Discard stale results: the document changed while checking, and a newer
  // check has already been scheduled by the change events.
  if (doc.isClosed || doc.version !== version) return;

  /** @type {vscode.Diagnostic[]} */
  const diags = [];
  const totalLines = doc.lineCount; // property (NOT a function) in the API
  for (const line of stderr.split('\n')) {
    const m = line.match(LOCATED_ERROR);
    if (!m) continue;
    const lineIdx = Math.max(0, parseInt(m[2], 10) - 1);
    if (lineIdx >= totalLines) continue;
    const textLine = doc.lineAt(lineIdx);
    const src = textLine.text;
    let start = Math.max(0, parseInt(m[3], 10) - 1);
    if (start > src.length) start = src.length;
    let end = start;
    while (end < src.length && /[A-Za-z0-9_]/.test(src[end])) end++;
    if (end === start) {
      if (start < src.length) {
        end = start + 1;              // non-word char at start: single-char range
      } else if (start > 0) {
        start--; end = start + 1;     // at/past end of line: cover the last char
      }                               // empty line: keep zero-width range 0..0
    }
    if (end > src.length) end = src.length;
    const range = new vscode.Range(lineIdx, start, lineIdx, Math.max(end, start));
    const d = new vscode.Diagnostic(range, m[4], vscode.DiagnosticSeverity.Error);
    d.source = 'cstar';
    diags.push(d);
    if (diags.length >= 200) break;
  }
  diagCollection.set(doc.uri, diags);
}

// ---------------------------------------------------------------------------
// Hover — the same contract docs as completions, shown on pointer-over
// ---------------------------------------------------------------------------

/** Completion signatures carry snippet placeholders; a hover shows plain
 * source: `${1:iterable}` → `iterable`, `${1}` → ``. */
function hoverSig(sig) {
  return sig.replace(/\$\{\d+:([^}]*)\}/g, '$1').replace(/\$\{\d+\}/g, '');
}

const BUILTIN_HOVER_FOOTER =
  '*cstar builtin — call-only and reserved; arity and named arguments are\n' +
  'checked at compile time (SPEC §5.11).*\n';

// `print`/`range` keep their bespoke completion items (M1/2A — the completion
// loop skips them); hover-only mirrors here so pointer-over is complete.
const LEGACY_HOVER_DOCS = {
  'print': {
    sig: 'print(${1:value})',
    doc: '`print(...)` — print any number of values, space-separated, ' +
      'newline-terminated. Displays use each value\'s display form (text ' +
      'unquoted; bytes as `b"..."`).',
  },
  'range': {
    sig: 'range(${1:stop})',
    doc: '`range(stop)` / `range(start, stop)` / `range(start, stop, step)` — ' +
      'lazy int range for `for` loops and `in` membership. **Stop is ' +
      'exclusive** (default start=0, step=1), all arguments must be int, and ' +
      '`step` must not be 0.\n\n```cstar\nfor i in range(5):\n    print(i\n)```',
  },
};

/** User function/method/class declarations in the open document, keyed by
 * name, with the declaration signature and 0-based line. */
function userDecls(document) {
  const out = new Map();
  for (const decl of functionDeclarations(document.getText())) {
    out.set(decl.name, decl);
  }
  return out;
}

function provideHover(document, position) {
  const range = document.getWordRangeAtPosition(position);
  if (!range) return undefined;
  const word = document.getText(range);
  if (!word) return undefined;
  const lineText = document.lineAt(position.line).text;

  // Builtins (all seventy: legacy + parity + stubs + the rejected trio) —
  // signature (placeholders stripped) + the full contract doc.
  const b = BUILTIN_DOCS[word] || LEGACY_HOVER_DOCS[word];
  if (b) {
    const md = new vscode.MarkdownString();
    md.appendCodeblock(hoverSig(b.sig), 'cstar');
    md.appendMarkdown(b.doc + '\n\n' + BUILTIN_HOVER_FOOTER);
    return new vscode.Hover(md, range);
  }

  // The twelve builtin exception classes.
  const exc = EXCEPTION_DOCS[word];
  if (exc) {
    const md = new vscode.MarkdownString();
    md.appendCodeblock(word, 'cstar');
    md.appendMarkdown(
      exc + '\n\n*cstar builtin exception class — catch with exact class identity\n' +
      '(`except ' + word + ' as e:`); instances expose `.type` and `.message`\n' +
      '(SPEC §6.10).*\n');
    return new vscode.Hover(md, range);
  }

  // Keywords — including True/False/None, global/nonlocal, the try family
  // and the retired `->` operator (still shown as an error helper).
  const k = KEYWORD_DOCS[word];
  if (k) {
    const md = new vscode.MarkdownString();
    md.appendMarkdown(k + '\n');
    return new vscode.Hover(md, range);
  }

  // `.method` right of a dot — every type's contract for that name.
  if (METHOD_DOCS[word] && /\.\s*$/.test(lineText.slice(0, range.start.character))) {
    const md = new vscode.MarkdownString();
    md.appendMarkdown('**Method of** `' + METHOD_DOCS[word].types + '`\n\n' +
                      METHOD_DOCS[word].doc + '\n');
    return new vscode.Hover(md, range);
  }

  // `variable.<qualifier>` — hover the `variable` token for the form help.
  if (word === 'variable' && /^\s*\./.test(lineText.slice(range.end.character))) {
    const md = new vscode.MarkdownString();
    md.appendMarkdown(VARQUAL_DOC + '\n');
    return new vscode.Hover(md, range);
  }

  // User functions and classes — their declaration signature + location.
  const d = userDecls(document).get(word);
  if (d) {
    const md = new vscode.MarkdownString();
    md.appendCodeblock(d.sig, 'cstar');
    let note;
    if (d.kind === 'method') {
      note = '*Declared at line ' + (d.line + 1) + '. Methods take an explicit `self`;\n' +
        'fields are dynamic; no inheritance (SPEC §8).*\n';
    } else if (d.kind === 'class') {
      note = '*Declared at line ' + (d.line + 1) + '. Classes are constructed with the\n' +
        'call operator (`Class(args)`); fields are dynamic; no inheritance (SPEC §8).*\n';
    } else {
      note = '*Declared at line ' + (d.line + 1) + '. Named `name():` / `name(params):`\n' +
        'declarations only — cstar has no lambdas; callbacks are passed as named\n' +
        'functions.*\n';
    }
    md.appendMarkdown(note);
    return new vscode.Hover(md, range);
  }

  return undefined;
}

module.exports = {
  activate,
  deactivate,
  // Internal helpers, exported only for test/smoke.js.
  _test: { setting, cliNames, cliCandidates, resolveModule },
};
