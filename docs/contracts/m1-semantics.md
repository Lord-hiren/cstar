# Pentest M1 — Normative Semantics and Decisions

> Part of the [stage contracts](README.md) (normative; they govern over
> [`SPEC.md`](../../SPEC.md) if the two differ).

This is the authoritative contract for Milestone 1 implementation. All compiler
components must conform to this document. Error-message wording listed here is
normative: tests match on these substrings.

## 1. Locked decisions (from design doc Section 10)

| Question | M1 decision |
|---|---|
| Type system | Dynamic: values are tagged at runtime (unset/None/int/float/bool/string). Reassignment may change type. Static typing deferred. |
| Compiler language | C++17 (compiler driver + frontend + codegen), built with `clang++ -std=c++17 -Wall -Wextra`. |
| Backend | Approach A: generate C11, compile with `clang -std=c11 -O2 ... -lm`. |
| Control flow | **Strict M1**: no if/else, no loops, no break/continue, no and/or/not. |
| Target platform | Linux x86_64 only. |
| CLI grammar | `pentest <file.pen>`, `pentest run <file.pen>`, `pentest build <file.pen> [-o out]`, `pentest check <file.pen>`, `--help`, `--version`. |
| Reassignment | Always allowed, including type change. |
| Params | Types optional → no type syntax at all in M1. No defaults, no named args, no variadics. Fixed arity, checked at compile time. |
| Return | `return expr` and bare `return` (= None). `return` only inside `function` bodies. Not allowed at top level nor inside `main():`. Falling off the end of a function returns `None`. `None` is an official keyword/value. |
| main() uniqueness | At most one `main():` block per file; executes when top-level execution reaches its textual position. |
| Modules/imports | Not in M1 (single file). |
| Classes | Not in M1. |
| Triple quotes | `"""..."""` is a multiline **comment** only (not a string literal). `'''` unsupported. |
| Comments | `#` line, `//` line (ALWAYS a comment: `x = 7 // 2` is `x = 7`), `/* */` (non-nesting), `""" """` multiline. A comment runs to EOL even inside unclosed parentheses, so it can swallow a closing `)`. |
| Errors | First lexer/parser error aborts; semantic errors are collected (all reported); runtime errors print and exit 1. |
| CLI workflow | `run` compiles with content-hash cache and executes; `build` emits standalone `.pentest`; `check` parses+analyzes only. |

## 2. Lexical structure

- **Encoding:** UTF-8 source; identifiers are ASCII `[A-Za-z_][A-Za-z0-9_]*`.
- **Indentation:** leading spaces only. A tab anywhere in leading whitespace →
  lex error `tabs are not allowed for indentation`.
  Python-style INDENT/DEDENT tokens. Dedent must match a previously seen
  indentation level, otherwise `inconsistent indentation`.
- **Newlines:** every statement ends at a newline. Newlines inside `(`/`)`
  (bracket depth > 0) are ignored (no NEWLINE, no indent processing).
  A missing final newline at EOF is tolerated (synthesized).
  Blank and comment-only lines do not affect indentation.
- **Comments** (never recognized inside string literals; `//` wins over `/`):
  - `# ... EOL`
  - `// ... EOL` — always a comment; M1 has no `//` division operator.
    `x = 7 // 2` is `x = 7` plus a comment. Because the comment runs to EOL
    even inside unclosed parentheses, the one-liner `print(7 // 2)` is an
    `unclosed '('` error (the comment eats the `)`); with the paren closed on
    a following line it prints `7`. `print(7 / 2)` prints `3`.
  - `/* ... */` — non-nesting; unterminated → `unterminated block comment`.
  - `""" ... """` — multiline comment; unterminated → `unterminated triple-quoted comment`.
  - A shebang first line is just a `#` comment.
- **Keywords (reserved, cannot be identifiers):** `function`, `main`, `return`,
  `True`, `False`, `None`.
- **Reserved identifier:** `print` — builtin; cannot be assigned, used as a
  parameter, or declared as a function. It can only appear as a call target.
  `main` as a bare identifier is always a syntax error (it is a keyword);
  in particular `function main():` and calling `main()` are syntax errors
  (parser emits `'main' must be declared as the special block 'main():'`).
- **Literals:**
  - Integer: decimal digits only (`0`–`9`), no `0x`, no underscores, no
    exponent. Must fit in signed 64-bit, else `integer literal out of range`.
  - Float: `DIGITS '.' DIGITS` (e.g. `1.5`). No exponent form in M1.
  - String: double-quoted, single line. Escapes: `\n \t \r \\ \" \0`.
    Unknown escape → `unknown escape sequence '\x'`. Newline or EOF inside →
    `unterminated string literal`.
- **Tokens:** NEWLINE, INDENT, DEDENT, IDENT, INT, FLOAT, STRING, keywords,
  punctuation `( ) : , = == != < <= > >= + - * / %`, EOF.
  Single `=` and `==` are distinct tokens. Semicolons are not supported
  (generic syntax error).
- Other unexpected bytes → `unexpected character 'x'`.

## 3. Grammar (EBNF, normative)

```ebnf
program     := { NL | top_stmt | func_decl | main_decl }
func_decl   := "function" IDENT "(" [ params ] ")" ":" NL INDENT block DEDENT
main_decl   := "main" "(" ")" ":" NL INDENT block DEDENT
params      := IDENT { "," IDENT }
block       := { NL } stmt { NL | stmt }        (* >= 1 statement; empty -> error *)
top_stmt    := stmt
stmt        := IDENT "=" expr NL                (* assignment; lexer makes "==" one token *)
             | "return" [ expr ] NL
             | expr NL                          (* expression statement; value discarded *)
expr        := cmp                              (* precedence, low -> high *)
cmp         := add { ("==" | "!=" | "<" | "<=" | ">" | ">=") add }   (* left-assoc *)
add         := mul { ("+" | "-") mul }                                (* left-assoc *)
mul         := unary { ("*" | "/" | "%") unary }                      (* left-assoc *)
unary       := "-" unary | primary
primary     := INT | FLOAT | STRING | "True" | "False" | "None"
             | IDENT "(" [ expr { "," expr } ] ")"     (* call: bare name only *)
             | IDENT "(" ")"                            (* zero-arg call *)
             | IDENT                                    (* variable read *)
             | "(" expr ")"
```

Rules:
- Assignment is a statement, never an expression (`print(a = 1)` → syntax error).
- Calls target a bare identifier only. Functions are not values in M1
  (`f(1)(2)` and `f + 1` are syntax errors; `print(f)` where `f` is a
  function is a semantic error, see §7).
- Nested `function` declarations → parser error
  `nested function declarations are not allowed`.
- Blocks must contain >= 1 statement → parser error
  `expected an indented block after ':'`.
- An empty file is valid (produces an empty program, exit 0, no output).

## 4. Values and operators

Runtime tags: `unset`, `None`, `int` (int64), `float` (double), `bool`, `string`.

- Arithmetic is done on the **evaluated values**; no implicit conversions
  between categories (notably no int→string, no bool→int).
- `+`: int+int → int (wrapping two's-complement, defined behavior);
  float involved (int+float, float+int, float+float) → float;
  string+string → concatenation; anything else → runtime error
  `unsupported operand types for +: <t1> and <t2>`.
- `-`, `*`: numeric only; int/int → int (wrapping); float involved → float;
  string operand → runtime error `unsupported operand types for -: ...` (resp. `*`).
  No string repetition in M1.
- `/`: int/int → integer division truncating **toward zero** (`7/2 == 3`,
  `-7/2 == -3`); float involved → double; division by zero (int or float
  divisor equal to zero) → runtime error `division by zero`.
- `%`: **ints only**; C semantics (sign follows dividend); zero divisor →
  runtime error `modulo by zero`; any float operand → runtime error
  `unsupported operand types for %: int and float` style message.
- Operand type compatibility is checked **before** the divisor is examined:
  `"a" / 0` reports the type error, not `division by zero`.
- Unary `-`: numbers only; ints wrap. Non-number →
  `unsupported operand type for unary -: <t>`.
- `< <= > >=`: int/float (mixed OK), strings lexicographic (byte-wise,
  shorter-is-less on prefix tie). bool/None operand →
  `unsupported operand types for <: <t1> and <t2>` (resp. `<=`, `>`, `>=`).
- `==` / `!=` are **total**, never a runtime error:
  - different tags → `==` is False, `!=` is True — except numeric mixing:
    int/float compare by mathematical value (`1 == 1.0` → True).
  - `None == None` → True; bools compare with bools.
  - `1 == "1"` → False.
- `unset` may never be observed as a value except by the used-before-assignment
  check (§6/§8).
- Int literal overflow is a lex error; int arithmetic wraps.

### print formatting (normative)

`print(...)` accepts any number of args (including 0):
- 0 args → just a newline.
- Args separated by a single space; terminated by `\n`.
- int → decimal; bool → `True`/`False`; None → `None`; string → raw bytes.
- float → shortest representation that round-trips: try `%.15g`, `%.16g`,
  `%.17g`, first whose `strtod` round-trips exactly; then, if the result
  contains no `.`, `e`, or `n` character, append `.0`
  (so `5.0` prints `5.0`, `1.5` prints `1.5`).

## 5. Execution model

1. **Declaration discovery first.** Before any execution, the compiler
   collects every `function` declaration and the optional `main():` block.
   Forward references to functions work from anywhere.
2. **Top-level statements run in source order.** When execution reaches the
   `main():` block, its body executes immediately at that point, then
   top-level execution continues with the following statements.
3. **Function definitions do not execute their bodies.** A body runs only
   when called (directly or transitively). Uncalled functions never run.
4. **A file without `main():` is a normal script.**
5. **Variables are not hoisted.** Reading a global before its top-level
   assignment has executed is a runtime error (§8). Globals exist from
   program start with tag `unset`; assignment changes them.
6. At most one `main():` block; it cannot be called explicitly.

## 6. Scope and name resolution

- **Top level and the `main():` block share global scope.** Every assignment
  there creates/updates a global. The `main():` block is *not* a function.
- **Functions (Python-style):**
  - Parameters are locals.
  - A name assigned **anywhere** in the function body is local for the whole
    body (reading it before its first executed assignment → runtime error
    `local variable 'x' used before assignment`).
  - Assignments inside a function are therefore **always local/param**;
    M1 has no `global` keyword — a function cannot modify a global
    (known limitation, planned for M2).
  - Any other name reads the corresponding global.
- **Resolution phases (semantic pass):**
  1. Scan top level: register all `function` decls, the `main():` block,
     collect global names (top-level/main-block assignments).
  2. Resolve top-level and main-block statements: reads → Global (or error §7);
     assignments → Global.
  3. Resolve each function body: params → Param; assigned names → Local;
     else global → Global; else error.
- **`Program.globals`** = global names in first-assignment (source) order —
  includes names assigned at top level and names resolved as Global in
  function bodies.

## 7. Compile-time errors (semantic pass — report ALL, then fail)

Prefix `<file>:<line>:<col>: error: `. Wording is normative (substring match):

| Condition | Message |
|---|---|
| duplicate function name | `duplicate function 'f'` |
| duplicate parameter | `duplicate parameter 'a' in function 'f'` |
| second `main:` block | `only one 'main:' block is allowed per file` |
| param named `print` | `cannot use reserved name 'print' as parameter` |
| assignment to `print` (any scope) | `cannot assign to reserved name 'print'` |
| `print():` | `cannot declare function with reserved name 'print'` |
| top-level/main assign where name is also a function | **allowed since 0.3** (separate namespaces — see SPEC §8; was `name 'f' is already declared as a function` under M1) |
| `class` name that is also a function name | `name 'f' is already declared as a function` |
| call to unknown name | `undefined function 'missing'` |
| wrong arg count (non-print) | `function 'add' expects 2 argument(s), got 1` |
| call to `print` is always valid (any count ≥ 0) | — |
| variable read of undefined name | `undefined variable 'zzz'` |
| variable read where name is a function | `function 'f' cannot be used as a variable` |
| `return` at top level | `'return' outside of a function` |
| `return` inside `main:` | `'return' is not allowed inside 'main:'` |
| assigning to a param inside its function | allowed (updates the param) |

Lexer/parser report only the **first** error and stop (with caret display);
the semantic pass collects and prints every error before returning failure.
Any compile error → exit code 1.

## 8. Runtime errors

Format (stderr, then exit 1): `<file>:<line>: runtime error: <message>`
where `<file>` is the path as invoked and `<line>` is the Pentest source line
of the failing expression.

| Condition | Message |
|---|---|
| read of unset global | `global variable 'a' used before assignment` |
| read of a local/param before its first executed assignment | `local variable 'v' used before assignment` |
| division by zero | `division by zero` |
| modulo by zero | `modulo by zero` |
| bad binary operands | `unsupported operand types for <op>: <t1> and <t2>` |
| bad unary operand | `unsupported operand type for unary -: <t>` |

Type names in messages: `int`, `float`, `bool`, `string`, `None`, `unset`.

## 9. CLI contract

```
pentest <file.pen> [args...]        # = run
pentest run <file.pen> [args...]    # compile (cached) + exec, pass [args] through
pentest build <file.pen> [-o out]   # native executable, default: <base>.pentest
pentest check <file.pen>            # parse + semantic only; silent on success
pentest --help | -h
pentest --version | -v              # prints: pentest 0.1.0-m1
```

- Source path must end in `.pen` or `.pent`, else usage error (exit 2).
  Both extensions are accepted everywhere a source file is given;
  `.pen` is the canonical spelling, `.pent` an accepted alias.
- `build` output default: source basename with `.pen` or `.pent` replaced
  by `.pentest`.
- `run` cache: `$XDG_CACHE_HOME/pentest/` (fallback `~/.cache/pentest/`),
  key = sha256 of (source bytes + version string), invoked via `sha256sum`
  through a pipe; if `sha256sum` is unavailable → no caching, always compile.
  On cache hit, exec the cached binary directly (exit code propagates).
- Compile pipeline: write generated `gen.c` plus the embedded runtime sources
  `cstar_rt.h`/`cstar_rt.c` (embedded in the `cstar` binary via
  `build/runtime_embed.h`) into a temp dir, then run
  `clang -std=c11 -O2 -o <out> gen.c cstar_rt.c -lm`.
  If `clang` is missing → clear error, exit 1.
- Exit codes: 0 success; 1 compile/semantic/runtime/clang errors;
  2 usage errors.
- Extra args after the file path are passed to the program as `argv[1..]`.
- Linux x86_64 only for M1.

## 10. Generated-C contract

`generate_c(program, out_c_path)` writes C11 that includes `"cstar_rt.h"`
and, when compiled together with `cstar_rt.c`, produces the program.
Required structure:

```c
#include "cstar_rt.h"
static cs_value cs_g_<name>;              /* one per Program.globals; zero-init = unset */
static cs_value cs_fn_<name>(cs_value cs_l_<param>, ...);   /* forward decls, ALL functions */
static cs_value cs_fn_<name>(...) { ... }                    /* definitions, decl order */
static void cs_main_block(void) { ... }                      /* if Program.has_main */
int main(int argc, char** argv) {
    cs_init(argc, argv, "<file path as invoked>");           /* stores path + args */
    /* top-level flow: statements in source order; at main()'s position: cs_main_block(); */
    return 0;
}
```

- Name mangling is done by the semantic pass (`FuncDecl.c_name`).
- Locals: C parameters are named `cs_l_<name>` directly; other locals are
  declared at function entry as `cs_value cs_l_<name> = <unset value>;`.
- `print` calls emit a local array + `cs_print(arr, n, line)`.
- Every operation that can fail at runtime must receive the Pentest source
  line number of the corresponding AST node.
- Reads of Global/Local nodes must be guarded by the used-before-assignment
  check with the proper name and scope kind.
- All function forward declarations appear before any definition so forward
  calls and mutual recursion work.
- The runtime library owns `cs_value` layout and the exact `cs_*` API
  (implementation freedom), but must provide: constructors for None/int/
  float/bool/string-literal, the arithmetic/comparison ops above with line
  reporting, `cs_print`, the used-before-assignment guard,
  `cs_init(argc, argv, src_path)`, and the runtime error reporter that
  prints §8's format and exits 1. Runtime requires only libc + libm.
- Float-to-string must implement §4's shortest-round-trip rule.
- Strings are immutable; literals may be static; concatenation may leak
  (no GC in M1 — documented limitation).

## 11. Fixed source file layout (ownership)

```
src/ast.h              src/diagnostics.h      src/diagnostics.cpp   (core contract)
src/frontend.h         src/codegen.h          (interface contract)
src/lexer.{h,cpp} src/parser.{h,cpp} src/semantic.{h,cpp}   (frontend)
src/codegen.cpp        runtime/cstar_rt.{h,c}             (backend)
src/main.cpp           Makefile  tools/embed.cpp  tests/**  examples/**   (CLI/build/tests)
SPEC.md                README.md              (docs)
build/pentest          (the CLI binary)
```
