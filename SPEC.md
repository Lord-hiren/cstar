# Pentest Language Specification (version 0.2.1)

> **Where this fits.** This is the normative language specification. For a
> gentle introduction start with the [language tour](docs/guide/language-tour.md);
> for a map of every document see the [documentation index](docs/README.md);
> for this file's table of contents by topic see the
> [specification map](docs/reference/spec-map.md). **Section numbers
> (`§5.11.5`, `§8`, …) are cited from source comments, tests and the editor
> extension — they are stable; new material is appended, never renumbered.**

**Normative status:** This document specifies the Pentest language as of
Milestone 2 Stages 2A + 2B + 2C, the `0.2.1` performance push and
cross-scope variable access, plus the **unreleased** Stage 2D modules/imports
work (§14) and the v0.3 surface-syntax overhaul (`()` calls, `in`
membership/binding, `name():`/`name(params):` declarations, `main:`) — the
`pentest` in this working tree still reports version `0.2.1`
(`pentest --version` prints `pentest 0.2.1`). It is derived from
[docs/contracts/m2c-semantics.md](docs/contracts/m2c-semantics.md) (the Stage 2C contract),
[docs/contracts/m2b-semantics.md](docs/contracts/m2b-semantics.md) (the Stage 2B contract),
[docs/contracts/m2a-semantics.md](docs/contracts/m2a-semantics.md) (the Stage 2A contract),
[docs/reference/cross-scope-access.md](docs/reference/cross-scope-access.md) (cross-scope access,
normative) and [docs/contracts/m1-semantics.md](docs/contracts/m1-semantics.md) (the authoritative
M1 decisions, still normative wherever Stages 2A/2B/2C do not override them).
If this specification and those contracts disagree, the contracts govern.
**One carve-out:** the unreleased v0.3 *surface-syntax* overhaul
(`f(x)` calls, `in` membership/binding, `name():`/`name(params):` declarations,
`main:`, `import`/`from`) is front-end-only — AST, semantic analysis and
generated C are unchanged — so where a contract's examples predate it,
this document's §4 grammar governs the *surface form* while the contract
remains normative for semantics, error wording and behaviour.
[docs/history/design.md](docs/history/design.md) is the original planning document and
[docs/performance/optimization.md](docs/performance/optimization.md) explains the performance
mechanisms: informative, not normative.

**Audience:** people implementing the Pentest compiler, or writing tools that
read, analyze, or build `.pen` programs. (Learning the language? Read the
[language tour](docs/guide/language-tour.md) first.)

**Conformance note:** every error message quoted in this document is
normative. Tests match on these substrings; do not reword them.

---

## 1. Status and scope

Milestone 2 Stages 2A, 2B and 2C, the `0.2.1` drop, and the unreleased
Stage 2D modules/imports work deliver a small, natively compiled language:

- programs in `.pen` source, compiled to native Linux x86_64 executables
  (`.pentest`) — single-file or split across modules with `import`/`from`
  (§14);
- dynamically typed values — `int`, `float`, `bool`, `text`, `bytes`,
  `None`, `range` (§6.9), and the Stage 2B heap types `list`, `map`, `set`,
  `tuple`, `function` (§5.1) plus the Stage 2C heap types `class`,
  `instance` and `method` (§5.12) — assignment, augmented assignment (eleven
  forms, §4.3), arithmetic, bitwise, comparison and text/bytes operators,
  membership `in` (§6.3), the object-identity operators `is` / `is not`
  (§5.13), the logical operators `and`/`or`/`not` and the conditional
  expression (§6.2);
- collections with literals, indexing, slicing, index assignment,
  destructuring targets and Python-style methods (§5.8–§5.10);
- attribute access on instances (`obj.name` reads, `obj.name = v` writes,
  §5.12, §4.3) and user classes with methods (§5.12, §7.2);
- control flow: `if`/`elif`/`else`, `while`, `for ... in ...`,
  `switch`/`case` (with the `case *:` wildcard), `break`, `continue`,
  `pass`, and exceptions `try`/`except`/`finally`/`raise` (§6.10) (§6);
- functions with positional, defaulted, `*name` (varargs) and `**name`
  (kwargs) parameters, named arguments at call sites (including on bound
  methods), function values, nested functions and closures, and
  `global`/`nonlocal` declarations (§7.1, §8);
- the special parameterless `main:` block and the script-style execution
  model (§7);
- automatic memory management: a non-moving mark & sweep collector with a
  program-global root vector and safe points (§7.3);
- the builtins `print`, `range`, `len`, `str`, `int`, `float`, `bool`,
  `type`, `encode`, `decode`, `set`, `collect`, `input`, `time`, `clock`
  (§5.11, §6.9), plus the fifty-five Python-parity builtins — `abs`
  through `zip`, the nine always-failing stubs (`super`, `object`, …) and
  the compile-time-rejected `eval`/`exec`/`compile` (§5.11.3–§5.11.6);
- cross-scope variable access — `variable.global <name>`,
  `variable.parent <name>` and `variable.<function> <name>` read and
  write bindings in other scopes (§4.1, §8);
- **modules and imports (Stage 2D, unreleased in the current tree)** —
  `import m`, `import m as x`, `from m import a [as b]`, dotted specifiers,
  filesystem resolution, per-module names, one-time module initialization,
  and selective loading of only the declarations a program uses (§14);
- clear compile-time and runtime error reporting with defined exit codes;
  runtime errors are throwable exceptions with a defined class (§6.10, §9),
  and the uncaught output keeps the exact M1 byte format;
- a CLI `pentest` with `run`, `build`, `check`, `--help`, `--version`
  (the CLI contract and exit codes are unchanged from M1, §9.4, §10).

**Deliberately excluded (for now)** (reserved for later Milestone 2 stages;
see §13):

- **generators and the remaining Stage 2D scope** — imports, multi-file
  programs and reachability pruning are implemented in the unreleased
  tree (§14); generators are not;
- **inheritance, user exception hierarchies, class attributes,
  generators and weak references** — classes are methods-and-fields only
  (§5.12; 2C scope);
- **static typing** — no type annotations, no type syntax at all, no
  inference; values are tagged at runtime and reassignment may change type;
- **non-Linux platforms** — Linux x86_64 only;
- also absent: multiline string literals, string repetition, floor division
  (`//` is a comment, §3.4), the Python builtins `bytearray`, `complex`
  and `frozenset` (deferred, §13), and dynamic code execution
  (`eval`/`exec`/`compile` — rejected at compile time, §5.11.6).

## 2. Source files and executables

- Source files use the extension **`.cst`** (e.g. `scanner.cst`). The legacy
  extensions **`.pen`** and **`.pent`** are accepted everywhere a source path
  is given until v0.4. `.pentest` is never a source extension.
- Compiled native executables have **no extension** on Linux: `cstar build`
  names the output after the source basename (`scanner.cst` → `scanner`, §10).
- The CLI requires every source path argument to end in `.cst`, `.pen` or
  `.pent` (§10). `run`, `build` and `check` without a path use the default
  entry file **`./index.cst`**.
- A produced executable is standalone apart from the system C library,
  libm and libcurl — it does not need the `cstar` toolchain at run time.

## 3. Lexical structure

### 3.1 Encoding and identifiers

- Source is **UTF-8**.
- Identifiers are ASCII: `[A-Za-z_][A-Za-z0-9_]*`.
- Bytes that cannot begin any token produce the lex error
  `unexpected character 'x'` (the actual byte is shown).

### 3.2 Indentation

- Blocks are indentation-based (Python-style `INDENT`/`DEDENT` tokens).
- **Leading whitespace must be spaces only.** A tab anywhere in the leading
  whitespace of a line is a lex error:
  `tabs are not allowed for indentation`.
- A dedent must match a previously seen indentation level, otherwise the lex
  error `inconsistent indentation` is raised.
- Blank lines and comment-only lines do not affect indentation.

### 3.3 Newlines and statements

- Every statement ends at a newline.
- Newlines inside `(`/`)` (bracket depth > 0) are ignored: no `NEWLINE` token
  is produced and no indent processing happens for them. This is the only way
  to span lines — **there is no line-continuation syntax** (e.g. a trailing
  `\` is just an unexpected character).
- A missing final newline at EOF is tolerated (it is synthesized).
- An empty file is valid: it produces an empty program, exits 0, and prints
  nothing.

### 3.4 Comments

Comments are never recognized inside string literals, and `//` wins over `/`
(it always starts a comment, never an operator). There are exactly four
forms():

| Form | Extent | Notes |
|---|---|---|
| `# ...` | to end of line | A shebang on the first line (`#!/usr/bin/env pentest`) is just a `#` comment. |
| `// ...` | to end of line | **Always a comment.** See footgun below. |
| `/* ... */` | to matching `*/` | **Non-nesting**: an inner `/*` has no effect. Unterminated → lex error `unterminated block comment`. |
| `""" ... """` | to closing `"""` | Multiline **comment** (never a string literal; `'''` is unsupported). Unterminated → lex error `unterminated triple-quoted comment`. |

> **The `//` footgun (normative).** `//` is *always* a line comment; there is
> no floor-division operator spelled `//` (Stage 2A did not add one — `//`
> remains locked as a comment, §1). Everything from `//` to the end of
> the line is discarded. So `x = 7 // 2` assigns `7` (not `3`). The comment
> runs to end-of-line **even inside unclosed parentheses**, so the one-liner
> `print(7 // 2)` is a syntax error (`unclosed '('` — the comment
> swallowed the `)`); closing the paren on the next line prints `7`. Use a
> single `/` for division: `print(7 / 2)` prints `3` (int division
> truncates toward zero, §5.4).

### 3.5 Keywords and reserved names

- **Keywords** (reserved; cannot be identifiers): `main`, `return`, `True`,
  `False`, `None`, `global`, `nonlocal`, plus the
  thirteen Stage 2A keywords
  `if`, `elif`, `else`, `for`, `while`, `break`, `continue`, `switch`,
  `case`, `pass`, `and`, `or`, `not`, plus the seven Stage 2C keywords
  `try`, `except`, `finally`, `raise`, `as` (§6.10) and `class`, `is`
  (§5.12, §5.13), the membership/binding keyword `in`, and the Stage 2D
  module keywords `import` and `from` (§14). `function` is still a reserved
  word so that old programs fail with the targeted parser error
  `'function' keyword was removed; declare with 'name():' or 'name(params):'`
  — it no longer introduces a declaration.
- **`variable` is not a keyword** (0.2.1): it stays an ordinary
  identifier. Only the exact 4-token shape `variable <qualifier> <name>`
  is read as cross-scope access (§4.1 `qual_ref`, §8); everywhere else
  `variable` is an ordinary name, so existing programs are unaffected.
- **`main`**: because it is a keyword, `main` as a bare identifier is always
  a syntax error. A declaration `function main:` → targeted parser error
  `'function' keyword was removed; declare with 'name():' or 'name(params):'`;
  a call target `main()` → `expected ':' after 'main'`; a read
  `x = main` → `unexpected token 'main'`; a nested `main:` block →
  `nested 'main' block is not allowed`. The only legal use is the block form
  `main:` (§7).
- **Reserved builtin names**: the fifteen original builtins `print`,
  `range`, `len`, `str`, `int`, `float`, `bool`, `type`, `encode`,
  `decode`, `set`, `collect`, `input`, `time`, `clock` (§5.11, §6.9),
  together with the fifty-five Python-parity names of §5.11.3–§5.11.6 —
  the 43 implemented entries (`abs` … `zip`, table in §5.11.3), the nine
  always-failing stubs (`breakpoint`, `classmethod`, `help`,
  `issubclass`, `memoryview`, `object`, `property`, `staticmethod`,
  `super`) and the compile-time-rejected trio `eval`, `exec`,
  `compile`. None of the seventy can be assigned, used as a parameter,
  declared as a function, or used as a loop variable; each may only
  appear as a call target (`print(...)`, `abs(...)`). Violations are
  semantic errors (§9.2); a bare read of any of them is reported as
  `undefined variable '<name>'`. `set`, `collect` and `range` (and every
  Python-parity name) are ordinary identifiers syntactically — not
  keywords — but share this reserved-name treatment (§8).

### 3.6 Literals

**Integer**

- Decimal digits only (`0`–`9`): no `0x`/`0b`/`0o` prefixes, no underscores,
  no exponent, no sign (the unary `-` is an operator).
- Must fit in signed 64-bit; otherwise the lex error
  `integer literal out of range`.
- Literal overflow is a *lex* error; integer *arithmetic* wraps (§5.3).

**Float**

- Form: `DIGITS '.' DIGITS` (e.g. `1.5`, `0.0`). Both sides require digits:
  `.5` and `5.` are not float literals.
- No exponent form (`1e10` does not lex as a number).

**String (text)**

- Double-quoted, single line. The value is a **`text`** value (Stage 2A
  called the type `string`; as of 2B it displays and reports as `text`,
  §5.1).
- Escapes: `\n`, `\t`, `\r`, `\\`, `\"`, `\0`.
- Unknown escape → lex error `unknown escape sequence '\x'` (the offending
  character is shown).
- A newline or EOF inside the quotes → lex error `unterminated string literal`.
- The value is the decoded byte sequence held as UTF-8 text (`print`
  writes it back verbatim); interior NUL (U+0000) is legal.

**Bytes**

- `b"..."` — double-quoted, single line, recognized only when `b` is
  **immediately** followed by `"` (no space: `b "x"` is not a bytes
  literal). Same escapes as String **plus `\xHH`** (a two-hex-digit byte).
- The value is a **`bytes`** value: raw bytes, no encoding implied.
- A newline or EOF inside the quotes → lex error
  `unterminated bytes literal`.

### 3.7 Tokens

The token set is: `NEWLINE`, `INDENT`, `DEDENT`, `IDENT`, `INT`, `FLOAT`,
`STRING`, `BSTRING` (the `b"..."` literal of §3.6), the keywords (§3.5),
the punctuation
`( ) [ ] { } . : , = == != < <= > >= + - * / % **(& | ^ ~ << >>)`, the
eleven
compound-assignment operators `+= -= *= /= %= **= &= |= ^= <<= >>=`, and
`EOF`.

- Single `=` (assignment) and `==` (equality) are distinct tokens; the lexer
  never splits `==`. The same maximal-munch rule governs the Stage 2A
  operators: `**=` is checked before `**`, `<<=`/`>>=` before `<<`/`>>`,
  `&=`/`|=`/`^=` before `&`/`|`/`^`, and `-` is checked before `->` (a
  hyphen followed by `>` lexes as the one token `->`, which is **not an
  operator** — it exists only so the parser can say `'->' is not an
  operator; call with 'f(args)'`; `a - > b` is two tokens and a syntax
  error).
- Semicolons are not supported (they produce a generic syntax error).

## 4. Grammar

### 4.1 EBNF (normative)

```ebnf
program       := { NL | import_decl | top_stmt | func_decl | main_decl }
import_decl   := "import" module_name [ "as" IDENT ]
               | "from" module_name "import" import_name
                                                 { "," import_name }
module_name   := IDENT { "." IDENT }              (* dotted specifier, §14 *)
import_name   := IDENT [ "as" IDENT ]
func_decl     := IDENT "(" [ params ] ")" ":" NL INDENT block DEDENT
                 (* always parenthesised: `name():` for zero parameters;
                    also legal inside a function body or main: — §7.2 *)
main_decl     := "main" ":" NL INDENT block DEDENT
params        := [ param { "," param } [ "," vararg ] [ "," kwarg ]
                 | vararg [ "," kwarg ] | kwarg ]
param         := IDENT [ "=" expr ]                  (* default value *)
vararg        := "*" IDENT                           (* extra positionals yield a list *)
kwarg         := "**" IDENT                          (* extra named arguments yield a map *)
block         := { NL } stmt { NL | stmt }           (* >= 1 statement;
                                                        empty is an error *)
top_stmt      := stmt

stmt          := compound_stmt | simple_stmt NL

compound_stmt :=
      "if" expr ":" suite { "elif" expr ":" suite } [ "else" ":" suite ]
    | "while" expr ":" suite
    | "for" IDENT "in" expr ":" NL INDENT block DEDENT  (* block form only *)
    | "switch" expr ":" NL INDENT { case_clause } DEDENT
    | try_stmt                                        (* M2-2C §6.10 *)
    | class_decl                                      (* M2-2C §5.12 *)
suite         := NL INDENT block DEDENT | simple_stmt   (* single-line body *)

case_clause   := "case" case_label ":" NL INDENT block DEDENT
               | "case" "*" ":" NL INDENT block DEDENT
case_label    := INT | "-" INT | FLOAT | "-" FLOAT
               | STRING | "True" | "False" | "None"

try_stmt      := "try" ":" NL INDENT block DEDENT
                 { except_clause } [ "finally" ":" suite ]
               | "try" ":" NL INDENT block DEDENT "finally" ":" suite
except_clause := "except" [ ternary ] [ "as" IDENT ] ":" suite
                 (* >= 1 except or finally required; at most one 'finally';
                    bare `except:` catches everything *)
class_decl    := "class" IDENT ":" NL INDENT { func_decl } DEDENT
                 (* block form only; `class C(Base):` is a syntax error (no
                    inheritance); the body accepts function declarations and
                    `pass`, validated semantically (§9.2) *)

simple_stmt   := target_list ("=" | augop) expr     (* augop: the 11 forms, §3.7;
                                                       >= 2 targets => destructuring *)
               | "return" [ expr ]
               | "break" | "continue" | "pass"
               | "global" IDENT { "," IDENT }
               | "nonlocal" IDENT { "," IDENT }
               | "raise" expr                        (* M2-2C §6.10 *)
               | expr                            (* expression statement; may be a Join *)

target_list   := target { "," target }               (* >= 2 targets => Tuple pattern *)
target        := qual_ref                            (* cross-scope (0.2.1), §8 *)
               | IDENT { "[" subscript "]" | "." IDENT }
                                                        (* tail selects Name / Index /
                                                           Attr; a slice tail is an
                                                           error, §4.3 *)
               | "*" IDENT                           (* rest leaf, anywhere in a pattern *)
               | "(" target_list [ "," ] ")"         (* nested pattern; a single target
                                                        needs the trailing comma *)
subscript     := [ expr ] ":" [ expr ] [ ":" expr ]  (* slice — never an assignment target *)
               | expr                                (* index *)

expr        := ternary { "," ternary }    (* the JOIN level — produced only at
                                             full-expression positions: assignment
                                             RHS, `return`, expression statement
                                             (§4.4). Precedence, low to high *)
ternary     := or_expr [ "if" or_expr "else" ternary ]   (* else-branch nests *)
or_expr     := and_expr { "or" and_expr }                (* left-assoc *)
and_expr    := not_expr { "and" not_expr }               (* left-assoc *)
not_expr    := "not" not_expr | cmp_expr
cmp_expr    := bitor_expr { cmp_op bitor_expr }          (* left-assoc *)
cmp_op      := "==" | "!=" | "<" | "<=" | ">" | ">=" | "in"
             | "is" | "is not"                           (* M2-2C §5.13 *)
bitor_expr  := bitxor_expr { "|" bitxor_expr }           (* left-assoc *)
bitxor_expr := bitand_expr { "^" bitand_expr }           (* left-assoc *)
bitand_expr := shift_expr { "&" shift_expr }             (* left-assoc *)
shift_expr  := add_expr { ("<<" | ">>") add_expr }       (* left-assoc *)
add_expr    := mul_expr { ("+" | "-") mul_expr }         (* left-assoc *)
mul_expr    := unary { ("*" | "/" | "%") unary }         (* left-assoc *)
unary       := ("-" | "~") unary | power
power       := postfix [ "**" unary ]                    (* right-assoc *)
postfix     := primary { "[" subscript "]"          (* index / slice *)
                         | "." IDENT                (* attribute read / bound method *)
                         | "(" [ args ] ")" }       (* call — bare, indirect,
                                                       chained; `f()` is the
                                                       zero-argument form *)
args        := arg { "," arg }
arg         := [ IDENT "=" ] ternary             (* named arg when IDENT "=" *)
primary     := INT | FLOAT | STRING | BSTRING | "True" | "False" | "None"
             | list_lit | set_or_map_lit | "(" [ tuple_or_group ] ")"
             | qual_ref                               (* cross-scope read, §8 *)
             | IDENT                                      (* variable read *)
qual_ref    := "variable" "." qual IDENT             (* cross-scope access, §8:
                                                         qual := IDENT | "global";
                                                         all 4 tokens on one
                                                         line — no NL between *)
list_lit    := "[" [ ternary { "," ternary } ] "]"
set_or_map_lit := "{" "}"                            (* empty MAP *)
               | "{" ternary ":" ternary { "," ternary ":" ternary } "}"  (* map *)
               | "{" ternary { "," ternary } "}"        (* set *)
tuple_or_group := expr                               (* (e) grouping *)
               | ternary "," [ ternary { "," ternary } ]  (* tuple / 1-tuple *)
```

Arguments inside `f(...)`, list/set/map elements and tuple elements parse at
`ternary` level — they are never joins (§4.4).

### 4.2 Precedence (normative)

Levels bind loosest (1) to tightest (14); operators on the same level
associate left unless stated otherwise:

| Level | Operators / form | Associativity |
|---|---|---|
| 1 | conditional expression `a if c else b` | else-branch nests |
| 2 | `or` | left |
| 3 | `and` | left |
| 4 | `not` (prefix) | — |
| 5 | `==` `!=` `<` `<=` `>` `>=` `in` `is` `is not` | left; **no chaining** |
| 6 | `\|` | left |
| 7 | `^` | left |
| 8 | `&` | left |
| 9 | `<<` `>>` | left |
| 10 | `+` `-` | left |
| 11 | `*` `/` `%` | left |
| 12 | unary `-`, `~` (prefix) | — |
| 13 | `**` | **right** |
| 14 | literals, names, cross-scope refs (`variable.…`, §4.1), `( expr )`, calls `f(x)`, indexing/slicing, attribute reads (`primary` + `postfix`, §4.1) | — |

Consequences (normative):

- **Arithmetic groups by parentheses, then `**`, unary `-`/`~`, then
  `*` `/` `%`, then `+` `-`**: `(1 + 2) * 3` is `9`, `1 + 2 * 3` is `7`,
  `8 / 4 / 2` is `1`, `10 - 3 - 2` is `5`. Operators on one level associate
  **left to right**, so `6 / 2 * 3` is `(6 / 2) * 3` == `9`, not
  `6 / (2 * 3)` — even though `*` and `/` bind tighter than `+`/`-`;
- `-2 ** 2` is `-(2 ** 2)` == **-4**, and `~2 ** 2` is `~(2 ** 2)` == `-5`;
- `2 ** 3 ** 2` is `2 ** (3 ** 2)` == **512**; `2 ** -1` parses as
  `2 ** (-1)` and fails at runtime with `negative exponent` (§9.3) because
  both operands are `int` (use `2.0 ** -1` for the float path, §5.2);
- `not` binds looser than comparisons: `not a == b` parses as
  `not (a == b)`;
- `x = a if b else c` is the conditional expression used as an assignment;
  statement-level `if` is the statement form — there is no ambiguity because
  the statement dispatcher looks at the first token only;
- comparisons, `in` and `is`/`is not` do not chain specially (plain
  left-associative, as in M1): `a is b == c` parses `(a is b) == c`;
- **the comma is not an operator**: `expr := ternary { "," ternary }` (the
  join level) sits below every row above and is produced only at
  full-expression positions (assignment RHS, `return`, expression
  statement) — see §4.4. `()` call arguments and collection elements parse
  at `ternary` level, so `f(a, b)` passes two arguments and `[a, b]`
  builds a two-element list, never a join; a comma-separated parenthesized
  list is a tuple (`(a, b)`, §4.1), not a join.

### 4.3 Prose notes

- **Assignment is a statement, never an expression.** `x = (y = 1)` is a
  syntax error; `a = b = 1` does not exist. The left-hand side is a
  `target_list` (§4.1): a single target (`name = expr` or
  `name[index] = expr`) or a comma-separated **destructuring** pattern
  (§4.4). Augmented assignment comes in the eleven forms
  `+= -= *= /= %= **= &= |= ^= <<= >>=` (§3.7). Each augmented form behaves
  exactly like `target = target <base-op> expr` with the base operator's
  runtime semantics and error messages (§5.2); the target is read, the
  operation applied, the result assigned, and the tag may change. Index
  targets (`xs[i] += v`, §5.8) evaluate the container and the index
  exactly once; a slice is never an assignment target
  (`cannot assign to slice`). Attribute targets (`obj.name = v`,
  `obj.name += v`, §5.12) evaluate the object exactly once; a compound form
  reads the field first (at the statement's line), then the RHS, and
  assignment requires an instance (`cannot assign attribute '<n>' on '<t>'`
  otherwise).
- **Cross-scope access is a contextual 4-token atom (0.2.1).** `variable`
  is **not** a reserved word: `variable <qualifier> <name>` (exactly
  `Ident("variable")` `.` qual `ident`, §4.1 `qual_ref`) is recognized
  only in that shape — as an expression (§8) or as the head of an
  assignment target (then the postfix `[…]`/`.attr` chain of `target`
  applies to it, and it is never a binding form). An attribute read
  `variable.x` followed by anything but a bare identifier still parses as
  an ordinary attribute read, so no existing program changes meaning; a
  bare `variable` remains an ordinary identifier.
  `qual_ref` may not be a `for` loop variable or an `except … as`
  binding — those positions take a plain `IDENT` (§6.6, §6.10).
- **Calls and function values (2B).** The callee of a call may be any
  expression: a bare name (`f(x)`), an indexed/stored function value
  (`fs[0](x)`), or a chained call (`f(x)(y)`). Arguments sit between the
  parentheses, separated by commas (`f(a, b)`); `f()` is the
  zero-argument call, and a named argument is written `f(name=value)`.
  Functions are first-class
  values (§8): they can be assigned, passed, returned and stored in
  collections; `print(f)` prints `<function f>`; `f + 1` is a runtime
  `unsupported operand types` error, not a syntax error. The builtins of
  §5.11/§6.9 remain call-target-only and cannot be read as values.
  `obj.name(args)` is the method-call form (§5.10, §5.12); a bare
  `obj.name` without a call is a legal **attribute read** (2C, §5.12) whose
  value is a bound method (or field) — it fails at runtime with
  `'…' object has no attribute '…'` only when no such attribute exists.
- **`main` is only valid as the block header** `main:`; anywhere else it is
  a syntax error (`unexpected token 'main'`, `expected ':' after 'main'`, or
  `nested 'main' block is not allowed`).
- **Nested `function` declarations are legal (2B)** inside a function body
  and inside `main:`, in any statement position — they define closures
  that are created (hoisted) when the enclosing function is entered
  (§7.2). Top-level declarations keep the M1 discovery rule (§7).
- **Blocks must contain at least 1 statement.** An empty indented block →
  parser error `expected an indented block after ':'`. Use `pass` as a
  placeholder statement.
- **Single-line suites** are allowed for `if`, `elif`, `else` and `while`
  only (`if x > 0: print(x)`); the body is exactly one `simple_stmt`. A
  compound or missing statement after the colon →
  `expected a simple statement on the same line after ':'`. `for` and
  `switch` require indented blocks; `for ...: <stmt>` on one line reports
  `expected an indented block after ':'`.
- **`for` requires the `in` binding form**: a loop header whose variable is
  not followed by `in` (e.g. `for i:`, `for a, b in x:`) →
  `expected 'in' after loop variable`.
- **`switch` requires at least one `case`.** A `switch` block that does not
  start with `case` → `expected at least one case in 'switch'`. Case labels
  are constant literals only (§4.1); `case *:` must be last. An `if`
  statement consumes its whole `elif`/`else` chain; a following orphan
  `elif`/`else` at statement level is a parser error (§9.1).
- **Newlines inside parentheses are ignored**, so a call or parenthesized
  expression may span lines; there is no other form of line continuation.
- **Statement kinds:** assignment (plain or augmented, incl.
  destructuring, name/index/attribute targets), `return`, `break`,
  `continue`, `pass`, `global`, `nonlocal`, `raise`, expression statement
  (its value is discarded), function declarations (top level, `main:`,
  nested — §7.2), class declarations (top level and `main:` only, §5.12),
  and the five compound statements `if`/`while`/`for`/`switch`/`try`. A
  function body that falls off the end without `return` returns `None`.
- **An empty file is valid** (exit 0, no output).
- **Notation.** A call is written `f(args)` (§4.1) both in prose and in code;
  nothing else stands for a call. A bare `f` is a read, never a call.

### 4.4 Comma-join and destructuring (2B)

**Comma-join.** Wherever a full `expr` is parsed — an assignment RHS, a
`return` value, or an expression statement — `a, b` is not a tuple: it
evaluates each operand and joins their **display forms** (§5.6) with a
single space, producing **`text`** (2B contract §7):

- `m = "hello", "world"` → `text` `hello world`;
- `return 1, 2` → `text` `1 2`;
- `x = (1, 2), 3` → `text` `(1, 2) 3` (tuple display, no flattening);
- `print(a, b)` passes **two arguments** (print space-separates them — the
  visual result is the same); `f(a, b)` passes two arguments;
  `[a, b]` and `(a, b)` are a list and a tuple.

Tuples at these positions need parentheses or brackets:
`pair = (1, 2)` / `pair = [1, 2]`.

**Destructuring.** The left-hand side of an assignment may be a
`target_list` (§4.1) with two or more targets:

- flat: `a, b = (1, 2)` or `a, b = [1, 2]` (the RHS must be parenthesized
  or bracketed — **`a, b = 1, 2` is wrong**: the RHS parses as a join and
  becomes the `text` `1 2`, which is not a 2-tuple);
- nested: `(a, b), c = ([1, 2], 3)` and `x, (y, z) = (1, (2, 3))`; a
  parenthesized single target needs a trailing comma (`(x,)`);
- rest: `first, *rest = xs` — the `*` leaf may appear **anywhere** (leading
  `*h, t = xs`, middle `a, *m, b = xs`, trailing `f, *r = xs`, or alone
  `*p = xs`) and binds the remaining elements as a **fresh `list`** (empty
  if none remain);
- index leaves: `xs[0], m["k"] = …` assign through the leaf's index target.

The source is any iterable (§6.6): list, tuple, set, map (**keys**), text
(**code points**), bytes (**int bytes**), range. Counting: a rest-free
pattern needs **exactly** as many elements as it has leaves (too few →
`not enough values to unpack (expected N, got M)`, too many →
`too many values to unpack (expected N, got M)`); a pattern with a rest
leaf needs at least `L - 1` elements (`L` = leaf count, reported as
`expected`) and never errors on extras. A non-iterable source →
`'<t>' object is not iterable`. Leaf targets follow the normal scoping
rules of §8.

## 5. Values and operators

### 5.1 Runtime tags

Values carry one of sixteen dynamic tags: `unset`, `None`, `int` (64-bit
signed), `float` (IEEE-754 double), `bool`, `text`, `bytes`, `range`
(§6.9), the Stage 2B heap types `list`, `map`, `set`, `tuple`,
`function` (§5.8, §8), and the Stage 2C heap types `class`, `instance`
and `method` (§5.12).
Reassignment always works and may change the tag of a variable.

**Approved 2B change:** the type that Stage 2A called `string` is displayed
and reported as **`text`**; `bytes` is new. `text`/`bytes` are immutable
(pointer+length; `text` stores UTF-8, may contain NUL); `list`, `map`,
`set`, `tuple`, `function`, `range`, `class`, `instance` and `method` are
**heap objects** referenced by pointer (§7.3: reclaimed by the mark &
sweep collector once unreachable).

Arithmetic is performed on **evaluated values**; there are **no implicit
conversions between categories** — notably no int→text and no bool→int.
These type names are exactly the ones used in error messages: `int`,
`float`, `bool`, `text`, `bytes`, `range`, `list`, `map`, `set`, `tuple`,
`function`, `class`, `method`, `None`, `unset` (an `instance` reports its
**class name** as its type — §5.12). The `type()` builtin (§5.11) uses the
same spellings except that `None` is reported as `none`; `type()` of an
exception or instance is its class name (`"ValueError"`, `"Point"`), of a
class `"class"` and of a bound method `"method"`.

`unset` may never be observed as a value except by the
used-before-assignment check (§8, §9.3).

### 5.2 Operator table

`<t1>` is the left operand's tag, `<t2>` the right's.

| Operator | Operands | Result |
|---|---|---|
| `+` | `int`, `int` | `int`, wrapping (§5.3) |
| `+` | `float` mixed with `int`/`float` | `float` |
| `+` | `text`, `text` | concatenation |
| `+` | `bytes`, `bytes` | concatenation (result stays `bytes`) |
| `+` | anything else (incl. mixed `text`/`bytes`) | runtime error `unsupported operand types for +: <t1> and <t2>` |
| `-`, `*` | `int`, `int` | `int`, wrapping (§5.3) |
| `-`, `*` | `float` mixed with `int`/`float` | `float` |
| `-`, `*` | anything else (incl. any `text` or `bytes` operand) | runtime error `unsupported operand types for -: <t1> and <t2>` (resp. `*`). **No string repetition.** |
| `/` | `int`, `int` | `int`, truncating **toward zero** (§5.4) |
| `/` | `float` mixed with `int`/`float` | `float` |
| `/` | numeric, divisor equal to zero (`0` or `0.0`) | runtime error `division by zero` |
| `/` | anything else | runtime error `unsupported operand types for /: <t1> and <t2>` |
| `%` | `int`, `int` | `int`, C semantics: sign follows the dividend (§5.4) |
| `%` | numeric, divisor equal to zero | runtime error `modulo by zero` |
| `%` | anything else (any `float` operand) | runtime error `unsupported operand types for %: <t1> and <t2>` — e.g. `unsupported operand types for %: int and float` for `5 % 2.0` |
| `**` | `int`, `int`, exponent >= 0 | `int`, wrapping (§5.3) |
| `**` | any `float` operand (`float ** int`, `int ** float`, `float ** float`) | `float` (C `pow()`) |
| `**` | `int` with a negative `int` exponent | runtime error `negative exponent` (operand types are checked first) |
| `**` | anything else (e.g. any `text`/`bytes` operand) | runtime error `unsupported operand types for **: <t1> and <t2>` |
| unary `-` | `int` | `int`, wrapping |
| unary `-` | `float` | `float` |
| unary `-` | anything else | runtime error `unsupported operand type for unary -: <t>` |
| unary `~` | `int` | `int`, `~a == -a - 1` |
| unary `~` | anything else | runtime error `unsupported operand type for unary ~: <t>` |
| `&`, `\|`, `^` | `int`, `int` | `int`, bitwise, wrapping (§5.3) |
| `<<`, `>>` | `int`, `int`, shift count 0..63 | `int`; `<<` shifts left, `>>` is arithmetic (sign-propagating) |
| `<<`, `>>` | `int`, `int`, shift count outside 0..63 | runtime error `shift count must be in range 0..63` (no silent masking) |
| `&`, `\|`, `^`, `<<`, `>>` | anything else (any `bool`, `float`, `None`, `text`, `bytes` or heap-object operand) | runtime error `unsupported operand types for &: <t1> and <t2>` (resp. `\|`, `^`, `<<`, `>>`) — **no bool→int promotion** |
| `<`, `<=`, `>`, `>=` | `int`/`float` (mixed allowed) | `bool` |
| `<`, `<=`, `>`, `>=` | `text`, `text` or `bytes`, `bytes` | `bool`, byte-wise lexicographic; on a prefix tie the shorter value is less |
| `<`, `<=`, `>`, `>=` | any other mix (incl. any `bool`/`None` operand, mixed `text`/`bytes`, collections) | runtime error `unsupported operand types for <: <t1> and <t2>` (resp. `<=`, `>`, `>=`) |
| `in` | right operand a `range`, `text`, `bytes`, `list`, `tuple`, `map` or `set` (left operand any) | `bool`, membership test (§6.3) |
| `in` | anything else (right operand of any other type, e.g. `int`/`float`) | runtime error `unsupported operand types for(): <t1> and <t2>` (the internal operator name is still `()`) |
| `is`, `is not` | anything (including `unset`, which raises the usual guards first) | `bool`, object identity — never a runtime error otherwise (§5.13) |
| `==`, `!=` | anything | `bool`, total — **never a runtime error** (§5.5) |

Both operands of a binary operator (and every call argument) are evaluated
exactly once; this specification does not normatively fix which of the two
operand subtrees is visited first. The eleven augmented assignment forms
(§4.3) apply exactly these semantics and messages for their base operators.

### 5.3 Integer wraparound

Integer literals must fit in signed 64-bit (lex error otherwise), but integer
**arithmetic wraps**: two's-complement wraparound is defined behavior, not
undefined behavior. For example `9223372036854775807 + 1` evaluates to
`-9223372036854775808`. `int ** int` with a non-negative exponent wraps the
same way (the result is congruent modulo 2^64, computed with unsigned
modular exponentiation).

### 5.4 Division and modulo

- `/` on two `int`s performs integer division **truncating toward zero**:
  `7 / 2 == 3` and `-7 / 2 == -3`.
- `/` with any `float` operand performs double division.
- Division by a zero divisor (int `0` or float `0.0`) is a runtime error
  `division by zero` — floats never produce `inf`/`nan` from division by
  zero. Operand types are checked first: `"a" / 0` reports the type error
  (§5.2), not `division by zero`.
- `%` accepts **two `int`s only**, with C semantics (the result takes the
  sign of the dividend): `7 % 2 == 1`, `-7 % 2 == -1`, `7 % -2 == 1`.
- A zero divisor for `%` is a runtime error `modulo by zero`.
- A `float` operand of `%` is a runtime error (§5.2).
- There is **no floor division**: `//` always starts a comment (§3.4) and
  `7 // 2` is just `7`. Single `/` is the only division operator; int `/`
  truncates toward zero as described above.

### 5.5 Comparison and equality

- `<`, `<=`, `>`, `>=` work on numbers (int/float, mixed allowed) and on
  `text`/`bytes` (byte-wise lexicographic, same tag only). They reject
  `bool`, `None`, collection and mixed `text`/`bytes` operands
  with `unsupported operand types for <op>: <t1> and <t2>`.
- `==` and `!=` are **total**: they never raise a runtime error, for any
  pair of values.
  - Different tags → `==` is `False`, `!=` is `True` — **except** numeric
    mixing: `int` and `float` compare by mathematical value, so
    `1 == 1.0` is `True`. (A `bool` is its own tag, not a number:
    `True == 1` is `False`.)
  - `None == None` → `True`. Bools compare with bools (`True == True` →
    `True`).
  - `text`/`bytes` of equal tag compare byte-wise; `text` and `bytes` are
    different tags, so `"ab" == b"ab"` is `False`.
  - `list` and `tuple` compare **structurally** (same tag, same length,
    element-wise `==`); `map` and `set` compare structurally without
    regard to insertion order (same live count, same key/value pairs).
  - `range` values compare structurally: `==` is `True` iff start, stop and
    step are all equal (`range(3) == range(3, 5, 1)` is `False`).
  - `function` values are equal iff they are the same value or belong to
    the same function declaration (meta identity) — two closures created
    from one declaration compare equal.
  - `1 == "1"` → `False`.
  - `class`, `instance` and bound `method` values (2C) compare **by
    identity**: `==` is `True` only for the same object — two instances of
    the same class with equal fields are still `!=` (§5.12), and two
    exception instances with equal class and message are `!=`.

### 5.6 `print` formatting (normative)

`print(...)` accepts any number of arguments, including zero (§3.5):

- 0 arguments → just a newline.
- Arguments are separated by a single space; output is terminated by `\n`.
- `int` → decimal; `bool` → `True`/`False`; `None` → `None`;
  `text` → raw bytes verbatim (no quotes).
- `bytes` → `b"..."`: printable ASCII (0x20–0x7E except `"` and `\`) is
  shown literally, everything else as lowercase `\xHH`, and `"`/`\` are
  escaped — e.g. `print(b"a\n")` prints `b"a\x0a"`.
- `list` → `[e1, e2]` (a space after each comma); `tuple` → `(e1, e2)`,
  a 1-tuple → `(e,)`, the empty tuple → `()`; `set` → `{e1, e2}`
  (insertion order); `map` → `{k1: v1, k2: v2}` (insertion order; keys and
  values use their own display forms — a `text` key shows without quotes).
  The empty set also displays as `{}` — **documented collision** with the
  empty map (`type()`/`set()`/`{}` disambiguate, §5.8).
- `function` → `<function name>`.
- `class` → `<class Name>`; an `instance` → `<Name instance>`; a bound
  `method` → `<method 'm' of Name>` — all deterministic (2C, §5.12; no
  addresses are ever shown). An exception instance with a message →
  `Class: message`; with an empty message → just `Class` (§6.10).
- `range` → `range(<start>, <stop>, <step>)` — start, stop and step are
  always all shown (§6.9), e.g. `print(range(3))` prints `range(0, 3, 1)`.
- Nested values recurse (a list of tuples of maps displays fully).
  Cycles are not detected — displaying a self-referential structure is
  undefined (§12).
- `float` → the **shortest representation that round-trips**: try `%.15g`,
  then `%.16g`, then `%.17g`; use the first whose `strtod` result equals the
  original double exactly. Then, if that string contains no `.`, `e`, or `n`
  character, append `.0`. Consequences: `5.0` prints `5.0`, `1.5` prints
  `1.5`, and `inf`/`nan`-style spellings (which contain `n`) are left
  unchanged.

The same table is the **display form** used by `str(x)` (§5.11), by
comma-join (§4.4), and inside every nested display above.

### 5.7 Text and Bytes (2B)

- **`text`** is UTF-8. `len(t)` counts **code points**; `t[i]` yields a
  **1-code-point `text`** (never a partial UTF-8 sequence);
  `for c in t` yields 1-code-point `text` values; slicing is code-point
  based (§5.9). `"..."` literals produce `text` (§3.6); `+` concatenates
  `text + text`; `<`/`<=`/`>`/`>=` compare byte-wise; `in` is substring
  containment (empty needle → `True`).
- **`bytes`** is raw. `len(b)` counts bytes; `b[i]` yields an **`int`
  0–255**; `for x in b` yields ints; slicing is byte-based (§5.9).
  `b"..."` literals produce `bytes` (§3.6, plus `\xHH`); `+` concatenates
  `bytes + bytes`; comparisons are byte-wise; `in` is subbytes
  containment.
- Mixed `text`/`bytes` operands of `+`, `<`-family comparisons, etc. are
  type errors (§5.2). `encode(text)` yields `bytes` (UTF-8) and
  `decode(bytes)` yields `text` — they convert between the two (§5.11);
  `decode` validates UTF-8.

### 5.8 Collections (2B)

**Literals** (§4.1):

| Form | Type |
|---|---|
| `[]`, `[a, b]` | `list` (ordered, mutable, duplicates allowed) |
| `{}`, `{"k": v, "j": w}` | `map` (insertion-ordered key→value) — **`{}` is an empty map** |
| `{1, 2, 3}` | `set` (unique, insertion-ordered) |
| `set()` | empty `set` — the only empty-set literal (an empty set *displays* as `{}`, §5.6) |
| `()`, `(x,)`, `(a, b)` | `tuple` (ordered, immutable); `(x)` is a group, not a tuple |

Elements evaluate left-to-right as written; a map literal evaluates
key-before-value, pair by pair.

**Reference semantics (2B ratified):** assigning a `list`/`map`/`set` to
another variable copies the **reference** — mutating through one is visible
through the other (`b = a; b.append(1)` also changes `a`). `tuple`,
`text` and `bytes` are immutable (value-like); `function` values are
references.

**Indexing `c[i]`** (list, tuple, text, bytes; maps use `m[k]`):

- negative `i` counts from the end (Python): `xs[-1]` is the last element;
- out of range → `list index out of range` / `tuple index out of range` /
  `text index out of range` / `bytes index out of range`;
- non-`int` index → `<container> indices must be integers, not <t>`
  (e.g. `list indices must be integers, not text`);
- `m[k]` accepts any **hashable** key; a missing key →
  `map key not found`; indexing a non-subscriptable value →
  `'<t>' object is not subscriptable`.

**Index assignment** exists on `list` and `map` only: `xs[i] = v`
(replace), `m[k] = v` (insert or replace). Out-of-range/negative indices
normalize exactly like reads; a non-`int` list index reports the same
`list indices must be integers, not <t>` error. Assignment to `text`,
`bytes` or `tuple` → `'text' does not support item assignment` (likewise
`'bytes'`/`'tuple'`); other types →
`'<t>' object does not support item assignment`. Compound index assignment
(`xs[i] += v`, all eleven forms, §4.3) evaluates the container and index
**exactly once** and reads the element before the right-hand side.

**Iteration** (`for x in c`, §6.6): list/tuple → elements; map → **keys**
(insertion order); set → elements (insertion order); text → 1-code-point
`text`; bytes → int; range → int. Mutating a collection **during its own
iteration is undefined** (no error is guaranteed).

**Membership `v in c`** (§6.3): list/tuple → element `==`; map → key;
set → element; text → substring; bytes → subbytes; range → int-in-range.
The result is always `bool`.

**Hashing (map keys / set elements):** hashable — `int`, `float`, `bool`,
`None`, `text`, `bytes`, and `tuple` whose elements are all hashable.
Unhashable — `list`, `map`, `set`, `range`, `function` →
`unhashable type: '<t>'`. Equality/hashing follow the total `==` (§5.5):
`1` and `1.0` are the **same key** (the first insertion's value and type
are retained for that key); `True` is a **distinct** key from `1`
(`True == 1` is `False`); `text` and `bytes` with equal bytes are
**distinct** keys; tuple keys hash element-wise.

### 5.9 Slicing (2B)

`c[a:b:c]` is accepted on **`text`, `bytes`, `list` and `tuple` only**
(a map or set → `'<t>' object does not support slicing`; any other type
likewise). Python normalization:

- any bound may be omitted: `[:4]`, `[2:]`, `[:]`, `[::2]`; bounds are
  `int` (a non-int bound → `slice indices must be integers, not <t>`);
- negative bounds count from the end; out-of-range bounds clamp (no
  error); the stop is **exclusive**; step may be negative;
- step `0` → runtime error `slice step cannot be zero`;
- the result is always a **new object** (a new list/tuple, new text, new
  bytes) — mutating it never affects the original;
- `text` slices by code point and never split a UTF-8 sequence.

### 5.10 Methods on builtin values (2B)

Methods are called as `obj.name(args)`; a bare `obj.name` is a legal read
(§5.12) that yields no value on builtin receivers — lists, maps, sets,
tuples, text and bytes expose no attributes, so it raises
`'<t>' object has no attribute '<n>'` at runtime. Arguments on builtin
methods are **positional only** (named → runtime error
`method arguments must be positional`, §9.3); user-defined methods
accept named arguments (§5.12). Dispatch is by the
receiver's type; **mutating methods return `None`**. This is the complete
builtin method surface (user classes and user-defined methods: §5.12):

| Type | Method | Args | Behavior |
|---|---|---|---|
| `list` | `append(v)` | 1 | append; returns `None` |
| | `pop()` / `pop(i)` / `pop(i, default)` | 0–2 | remove + return element `i` (default `-1`); empty `pop()` or out-of-range without default → `list index out of range`; with default, out-of-range returns the default; `i` must be `int` |
| | `insert(i, v)` | 2 | Python-style clamped insert; returns `None` |
| | `index(v)` | 1 | first matching index (`int`), else runtime error `value not in list` |
| | `remove(v)` | 1 | remove first match (else `value not in list`); returns `None` |
| | `reverse()` / `clear()` | 0 | in place; returns `None` |
| `map` | `get(k)` / `get(k, default)` | 1–2 | value, or default, or `None` if absent (no error) |
| | `keys()` / `values()` / `items()` | 0 | fresh `list` — `items()` yields `(k, v)` tuples, in insertion order |
| | `pop(k)` / `pop(k, default)` | 1–2 | remove + return; missing without default → `map key not found` |
| | `clear()` | 0 | empties the map; returns `None` |
| `set` | `add(v)` | 1 | insert (no-op if present); returns `None` |
| | `remove(v)` | 1 | remove, else runtime error `value not in set`; returns `None` |
| | `discard(v)` | 1 | remove if present, silent otherwise; returns `None` |
| | `pop()` | 0 | remove + return the first element (insertion order); empty → `pop from an empty set` |
| | `clear()` | 0 | empties the set; returns `None` |
| `text` | `upper()` / `lower()` | 0 | case mapping (**ASCII letters only** — documented 2B limitation) |
| | `split(sep)` | 1 | `sep` must be non-empty `text` (else `empty separator in split()`); → `list` of `text` |
| | `join(iterable)` | 1 | joins with the receiver as separator; a non-`text` element → `'text.join() expects text elements, got <t>'` |
| | `find(needle)` | 1 | first occurrence as a **code-point index** (`int`), or `-1` |
| | `replace(old, new)` | 2 | non-overlapping replacement; `old` must be non-empty (else `empty pattern in replace()`) |
| | `strip()` | 0 | trims ASCII whitespace from both ends |
| | `starts_with(p)` / `ends_with(p)` | 1 | `bool` |
| `bytes` | `decode()` | 0 | validating UTF-8 decode → `text` (else `invalid UTF-8 in decode()`) |

Method errors (runtime, §9.3): a method the receiver's type does not have
→ `'<t>' object has no method '<m>'` (e.g. `'int' object has no method
'append'`); a wrong argument count for an exact-arity method →
`'<t>.<m>() takes N argument(s), got M` — **normative quirk: this message
has no closing apostrophe**, exactly as quoted in the contract
(`'list.append() takes 1 argument(s), got 0`); a 0–2-argument method
outside its range →
`'<t>.<m>() takes between N and M argument(s), got K'` (closing apostrophe
present); a non-`text` argument where `text` is required →
`'<t>.<m>() expects text, got <u>` (also no closing apostrophe).

### 5.11 Builtins (2B/2C/0.2.1)

#### 5.11.1 Scope, call-only rule and arity checks

There are **seventy** builtins (§3.5): the fifteen original Pentest
builtins (§5.11.2), forty-three Python-parity builtins (§5.11.3), nine
always-failing stubs (§5.11.6) and the compile-time-rejected
`eval`/`exec`/`compile` trio (§5.11.6). Every one is **call-target-only**:
it cannot be read as a value, assigned, used as a parameter or loop
variable, or redeclared — so `callable(print)`, `type(abs)` or
`f = sorted` are impossible by construction (§3.5, §8). Callbacks
(`key=`, `map`/`filter` functions) are therefore passed as ordinary named
`function` declarations — Pentest has no lambdas.

**Arity and named arguments are checked at compile time** (§9.2):

- fixed-slot builtins: `'len' expects 1 argument(s), got <n>`,
  `'input' expects 0 to 1 argument(s), got <n>`, and the Python-shaped
  required form `slice() missing 1 required positional argument: 'start'`
  (comma-joined `'a', 'b'` above one missing slot);
- variadic builtins (`map`, `zip`, `max`, `min`):
  `'map' expects at least 2 argument(s), got <n>`;
- extra positionals: `'round' expects at most 2 argument(s), got <n>`;
- named binding: `'<n>' has no named argument '<a>'` and
  `'<n>' got multiple values for argument '<slot>'`; `dict` alone takes
  **arbitrary** named keys (`dict(a=1, b=2)`), with at most one
  positional source (`'dict' expects at most 1 positional argument, got
  <n>` — the count **excludes** the named arguments);
- the original fifteen take **no** named arguments at all
  (`builtin '<n>' does not take named arguments`).

Unfilled optional slots arrive at the runtime as `pt_missing()` and are
defaulted there, exactly like Python's `None`-sentinel conventions.

#### 5.11.2 The original fifteen

- `len(x)` → `int`: `text` (code points), `bytes`, `list`, `map`, `set`,
  `tuple`; anything else → runtime error
  `object of type '<t>' has no length()`.
- `str(x)` → `text`: the display form of §5.6, for **every** type
  (including `bytes` → `b"..."` and `function` → `<function name>`).
- `int(x)` → `int`: `int` itself; `float` truncated toward zero (a
  non-finite or out-of-int64-range float → `cannot convert float to
  int`); `bool` → 0/1; `text`/`bytes` parsed as a strict decimal integer
  (invalid → `invalid literal for int(): '<s>'`); anything else →
  `int() expects text, bytes, int, float or bool, got <t>`.
- `float(x)` → `float`: `float` itself; `int`/`bool` converted;
  `text`/`bytes` parsed (invalid → `invalid literal for float(): '<s>'`);
  anything else → `float() expects text, bytes, int, float or bool, got <t>`.
- `bool(x)` → `bool`: §6.1 truthiness (never a runtime error).
- `type(x)` → `text`: one of `"int" "float" "bool" "text" "bytes" "none"
  "list" "map" "set" "tuple" "range" "function" "class" "method"` — note
  `None` is spelled `none` here, while error messages and `print` use
  `None`; an exception instance or a user instance reports its **class
  name** (`"ValueError"`, `"Point"`, §5.12, §6.10).
- `encode(t)` → `bytes`: UTF-8 bytes of `t`; a non-`text` argument →
  runtime error `encode() expects text, got <t>`.
- `decode(b)` → `text`: validating UTF-8 decode (§5.7); a non-`bytes`
  argument → `decode() expects bytes, got <t>`; invalid UTF-8 →
  `invalid UTF-8 in decode()`.
- `set()` → the empty `set`; `set(iterable)` → a `set` of the iterable's
  elements (a `map` contributes its **keys**); a non-iterable argument →
  `'<t>' object is not iterable`.
- `collect()` → `int` (2C): forces a full garbage collection (§7.3) and
  returns the number of heap blocks freed by that collection. It is the
  only observability of the collector; programs never depend on its value.
- `input()` → `text` or `None` (0.2.1): reads one line from standard
  input up to and including the trailing newline, which is stripped. With
  an argument, the prompt is printed first with display (`str()`)
  semantics — no newline — and flushed, so interactive reads can ask
  before blocking. An empty line returns `""`; end of input returns
  `None` (never blocks, and `""` a line vs `None` no line are distinct).
  The line's raw bytes are taken as `text` as-is (no UTF-8 validation on
  this path).
- `time()` → `float` (0.2.1): `CLOCK_REALTIME` seconds since the Unix
  epoch (wall clock).
- `clock()` → `float` (0.2.1): `CLOCK_MONOTONIC` seconds from an
  arbitrary origin — for measuring intervals; never goes backwards.

#### 5.11.3 The Python-parity builtins (0.2.1)

Forty-three of Python's builtin names, each compiled to **one direct
`pt_builtin_*` call** (§5.11.4, docs/performance/optimization.md). Slot names double
as named-argument keys (`round(number=1.5, ndigits=0)`); `…?` marks an
optional slot. `max`/`min` accept an unbounded positional list plus
named `key`/`default`; `map`/`zip` take positionals only; `dict` takes
one optional positional source plus arbitrary named keys; `slice`'s one
positional binds **stop** (§5.11.5.10).

| Call | Semantics |
|---|---|
| `abs(x)` | `int`/`float` magnitude; `abs(INT64_MIN)` wraps (§5.3) |
| `all(iterable)` | `bool`; empty → `True`; short-circuits on the first falsy |
| `any(iterable)` | `bool`; empty → `False`; short-circuits on the first truthy |
| `ascii(x)` | like `repr`, escaping every non-ASCII code point: `\xNN` (< 0x100), `\uNNNN` (< 0x10000), else `\UNNNNNNNN` |
| `bin(x)` | `0b…` text for a non-negative `int`, `-0b…` for a negative; non-int → `TypeError` `'<t>' object cannot be interpreted as an integer` |
| `bytes(source?, encoding?, errors?)` | see below — `bytes()` empty, `bytes(n)` n zero bytes, `bytes(iterable)` of 0..255, `bytes(bytes)` copy, `bytes(text, encoding)` UTF-8 encode |
| `callable(obj)` | `bool` — `True` for function values, bound methods, class and exception classes; `False` for everything else (never raises) |
| `chr(i)` | one-code-point `text` for 0..0x10FFFF; out of range → `ValueError`; surrogates → `ValueError` `chr() surrogates are not allowed (cstar strings are UTF-8)` |
| `delattr(obj, name)` | `None` — removes one **dynamic instance field**; class method → `AttributeError` `'<C>' attribute '<m>' belongs to its class and cannot be deleted`; missing field or non-instance → `AttributeError` `'<t>' object has no attribute '<n>'` |
| `dict(source?, **pairs)` | a `map`; ≤ 1 positional source (a `map` copies, an iterable of 2-item pairs / a `zip` list unpacks; element errors mirror CPython: `dictionary update sequence element #0 is not a pair`, `… has length 3; 2 is required`) |
| `dir(obj?)` | no argument → **sorted** `text` names of the globals table (§5.11.5.4); with an instance → sorted union of its dynamic field and method names; anything else → `TypeError` `'<t>' object has no attributes` |
| `divmod(a, b)` | `(quotient, remainder)` using Pentest's C99 truncating `/` and `%` (§5.4) — **not** Python's floor semantics (§5.11.5.1); `b == 0` → `ZeroDivisionError` `division by zero`; `INT64_MIN / -1` → `(INT64_MIN, 0)` |
| `enumerate(iterable, start=0)` | **eager** `list` of `(index, item)` (§5.11.5.7); `start` must be an `int` → `TypeError` `enumerate() start must be an int, not '<t>'` |
| `filter(function, iterable)` | **eager** `list` of the elements whose item is truthy; `function` may be `None` (filters truthy, like Python); non-callable → `TypeError` `filter() arg 1 must be callable or None` |
| `format(value, format_spec?)` | the mini-language `[[fill]align][sign][#][0][width][,][.precision][type]` with types `d i u b o x X c e E f F g G % s r`; a typeless int formats as `d` (so `format(42, ">7")` is `"     42"`); typeless int + precision → `ValueError` `Precision not allowed in integer format specifier`; grouping counts from the right (`format(314159, ",")` → `314,159`); `'='`-align pads after the sign |
| `getattr(obj, name, default?)` | instance field/method; exception `.type`/`.message`; file `.closed`/`.name`; on a hit returns exactly what `obj.name` would (§5.12); on a miss returns `default` if given, else `AttributeError` `'<t>' object has no attribute '<n>'`; `name` must be `text` → `TypeError` `getattr(): attribute name must be str, not '<t>'` |
| `globals()` | `map` of the globals table snapshotted at the call site: name → current value; not-yet-assigned names are skipped; top-level functions appear as function values |
| `hasattr(obj, name)` | `bool` — true **iff** `getattr` without a default would not raise (one shared probe) |
| `hash(obj)` | `int` (§5.3 wrap): the runtime's value hash as `int64`; unhashable (`list`/`map`/`set`, instances) → `TypeError` `unhashable type: '<t>'` |
| `hex(x)` | `0x…` text, `-0x…` for negatives; same `TypeError` as `bin` |
| `id(obj)` | `int`: heap objects → their address with bit 63 set (a disambiguating sign fold); immediates (`int`/`float`/`bool`/`None`) → a deterministic value-tagged fold, so `id(1) == id(1)` while `id(True) != id(1)` and `id(1) != id(1.0)`; identical static text literals may share storage and therefore an id |
| `isinstance(obj, classinfo)` | `bool` with **exact type matching** (§5.11.5.5); `classinfo` is a bare builtin-type name (rewritten at compile time — `int`, `text`, `type` → the type-name string, …), a type-name string (`"int"`), an exception class or user class, or a tuple of those; a shadowing parameter/local of that name wins over the rewrite |
| `iter(obj)` | a boxed iterator: `text`/`bytes` by code point, `list`/`tuple`/`set` by element, `map` by key, `range`, `file` by line; `iter(iterator)` is the argument unchanged (Python: `iter(it) is it`); non-iterable → `TypeError` `'<t>' object is not iterable` |
| `list(iterable?)` | eager `list` copy; no argument → `[]` |
| `locals()` | `map` of the current frame's table (parameters + locals, in declaration order — the globals table at top level); unassigned names skipped |
| `map(function, iterable…)` | **eager** `list`, one call per index, stopping with the shortest input (like Python) |
| `max(…, key?, default?)` | largest element — one iterable positional or many positionals; empty without `default` → `ValueError` `max() arg is an empty sequence`; `key` must be callable or `None` (`max() argument 'key' must be callable or None`); with a `key`, only the best key's element is retained (no per-element list) |
| `min(…, key?, default?)` | mirror of `max` (`min() arg is an empty sequence`) |
| `next(obj, default?)` | advances a boxed iterator one step; a `file` yields its next line **including** the trailing newline (Python); exhaustion with `default` → `default`, without → `StopIteration`; non-iterator → `TypeError` `'<t>' object is not an iterator`, non-iterable → `'<t>' object is not iterable` |
| `oct(x)` | `0o…` text; same `TypeError` as `bin` |
| `open(file, mode="r", encoding?)` | a `file` value; `mode` is `r`/`w`/`a` plus optional `t` or `b` (`x` → `ValueError` `open(): unsupported mode '<m>'`); UTF-8 only (`encoding` other → `ValueError` `cstar supports only the UTF-8 encoding`; on `b` → `ValueError` `binary mode doesn't take an encoding argument`); missing file → `FileNotFoundError` `[Errno 2] No such file or directory: '<path>'` |
| `ord(c)` | `int` code point of a one-character `text`; empty → `TypeError` `ord() expected a character, but string of length 0 found`; longer → `TypeError` `ord() expected a character, but string of length <n> found`; non-`text` → `TypeError` `ord() expected string of length 1, but <t> found` |
| `pow(base, exp, mod?)` | two-argument: wrapping int power (§5.3) or `float`; three-argument: `int` only with an `int` modulus — `mod ≤ 0` → `ValueError`, negative exponent → `ValueError` `pow() 3-argument form does not support negative exponents (no modular inverse)` (computed with `__int128` modular exponentiation) |
| `repr(x)` | the quoted/escaped form of §5.6: `'…'` for text with `\n \t \\ \' \xNN`-style escapes (interior `"` kept raw), quoted inner text inside collections, `b"…"` for bytes; non-text → its display form |
| `reversed(sequence)` | **eager** `list`, reversed in place in one buffer; accepts `range`, `text`, `bytes`, `list`, `tuple` and `map` (keys, like Python); sets, iterators and everything else → `TypeError` `'<t>' object is not reversible` |
| `round(number, ndigits?)` | banker's half-even rounding; `int` + `ndigits ≥ 0` → the value unchanged (identity); `int` + negative `ndigits` → `int` rounded to that power of ten; `float` + `ndigits` → `float`; no `ndigits` → `int` (`float` half-even to a whole) |
| `setattr(obj, name, value)` | `None` — sets a **dynamic instance field**; on a non-instance raises `TypeError` `cannot assign attribute '<n>' on '<t>'` — the same class and message as `obj.attr = v` (§5.11.5.3) |
| `slice(start?, stop?, step?)` | the `(start, stop, step)` **tuple** (missing → `None`); one positional binds **stop**: `slice(5)` → `(None, 5, None)`; components are stored unvalidated (like Python 3.14); inert — indexing with the result does not slice (§5.11.5.10) |
| `sorted(iterable, key?, reverse?)` | a new sorted `list`; stable merge sort (equal keys keep input order); `key` callable or `None` (`sorted(): 'key' must be callable or None`); `reverse` is truth-tested (any truthy value reverses, like Python); incomparable elements → the `§5.5` comparison `TypeError` |
| `sum(iterable, start=0)` | left fold with the `+` operator (§5.2): `int`/`float` arithmetic, **text `start` concatenates** (§5.11.5.12) |
| `tuple(iterable?)` | eager `tuple` copy; no argument → `()` |
| `vars(obj?)` | no argument → `map` of the current frame's table (like `locals()`); an instance → a `map` copy of its dynamic fields; anything else → `TypeError` `'<t>' object has no attributes` |
| `zip(iterables…)` | **eager** `list` of tuples, one tuple per index, stopping with the shortest input; no arguments → `[]` |

**`bytes(source, encoding, errors)`** in detail: `bytes()` → empty;
`bytes(n)` → `n` zero bytes (a `bool` source counts as 0/1 —
`bytes(True)` is `b"\x00"`), negative → `ValueError` `negative count`;
`bytes(iterable)` → one byte per element — non-int (a `bool` counts as
0/1) → `TypeError` `'<t>' object cannot be interpreted as an integer`,
out of range → `ValueError` `bytes must be in range(0, 256)`;
`bytes(bytes)` → a fresh copy; `bytes(text, …)` **requires** an encoding
(omitted → `TypeError` `string argument without an encoding`), accepts
only `utf-8`/`utf8` (else `ValueError` `cstar supports only the UTF-8
encoding`), and `encoding`/`errors` must be `text` (`bytes() argument
'encoding' must be str, not <t>` / `'errors'` likewise) — passing either
to a non-text source → `TypeError` `encoding without a string argument` /
`errors without a string argument`. The `errors` handler name is accepted
but never consulted (Pentest strings are valid UTF-8, so encoding cannot
fail). A non-iterable, non-int source → `TypeError` `cannot convert
'<t>' object to bytes`.

#### 5.11.4 Compiled form (performance contract)

Every §5.11.3 call lowers to a single direct C call with constant
arguments — no dispatch table, no dynamic lookup, nothing emitted for
builtins the program does not use (docs/performance/optimization.md):

- fixed-slot calls pass `(pt_value){slot…}` positionally after compile-time
  binding; variadics (`map`/`zip`/`max`/`min`) pass a C11 compound-literal
  `(const pt_value[]){…}` plus `(npos, npos, key, default, line)`;
- `globals`/`locals`/`vars`/`dir` emit a `pt_name_slot` compound-literal
  **at the call site** (kind 0 boxed value — `PT_UNSET` skipped, kind 1
  native `int64` + assignment-guard flag, kind 2 top-level function meta)
  — no reflection over live frames is ever performed;
- a set of **allocation-free** builtins is elided from GC safe points
  (`abs`, `callable`, `delattr`, `hasattr`, `hash`, `id`, `isinstance`,
  `ord`, `pow`, `round`, `clock`, `time`, and `max`/`min` with ≥ 2
  positionals and no `key`/`default`): they cannot allocate, so no
  collector probe is emitted around them;
- native-int lanes keep pure-`int` subexpressions unboxed end to end:
  `abs(x)` → `pt_nabs(x)`, two-argument `pow` → `pt_npow(a, b, line)`,
  two-positional `min`/`max` → `pt_nmin`/`pt_nmax`, and one-argument
  `round(x)` compiles to **its argument unchanged** (an `int` with no
  `ndigits` is the identity) — no call, no boxing, no allocation;
- only `sorted`/`map`/`filter`/`max`/`min` root intermediates (the two
  max/min roots hold just the best key, and only when `key` is given).

#### 5.11.5 Deviations from Python

Every observable difference from CPython (3.14) is listed here; each is
either a consequence of an earlier normative Pentest rule or an explicit
design decision. Error-message wording matches CPython except for
Pentest's type names (`text`, `None`, `map`, `class` — §5.1) and the
documented cases below.

1. **`divmod` truncates, Python floors.** Pentest's `/` and `%` are C99
   (§5.4), so `divmod(-7, 2)` is `(-3, -1)` where Python gives `(-4, 1)`
   — the same deviation `/` and `%` already have. `divmod(INT64_MIN, -1)`
   returns `(INT64_MIN, 0)` instead of trapping.
2. **`round` is half-even; `int` inputs are exact.** `round(x)` ties go
   to the even neighbour (like Python), but `round(3.1415, 2)` on an
   `int` with non-negative `ndigits` returns the value unchanged rather
   than round-tripping through `float`.
3. **`setattr` on a non-instance raises `TypeError`, not
   `AttributeError`.** The message is byte-identical to `obj.attr = v`'s
   (`cannot assign attribute '<n>' on '<t>'`, §5.12) so both spellings
   fail the same way; Python raises `AttributeError`.
4. **`dir()` always lists the globals table.** Python's `dir()` with no
   argument lists the *local* scope inside a function; Pentest's lists
   the program's globals in every scope (there is no builtin
   namespace — the table contains program globals: user functions,
   classes, assigned names, the readable exception classes and nothing
   else). `globals()`/`locals()`/`vars()` return maps whose **display**
   follows Pentest's `{k: v}` form, not Python's `{'k': 'v'}`.
5. **`isinstance` matches exactly — there is no subclassing.**
   `isinstance(True, int)` is `False` (§5.12), `isinstance(5, float)` is
   `False`, and only the listed type names/classes/tuples are accepted
   (a `text` type name — `isinstance(x, "int")` — is a Pentest
   *extension*: Python raises there). Bare builtin-type names in
   `classinfo` are rewritten to type-name strings at compile time
   (shadowing locals win); `isinstance(None, type)` is `False` (no
   metaclasses).
6. **Builtins are call-only**, so `callable(print)`, `type(abs)`,
   `map(len, …)`-style calls and any builtin-as-value idiom are compile
   errors; `key=`/`function=` arguments must be named `function`
   declarations (no lambdas exist).
7. **`zip`/`enumerate`/`filter`/`map`/`reversed` return eager lists**,
   not lazy iterators: `len(z)`, `z[i]` and repeated iteration all work,
   and their side effects (the callback calls) happen at the call, not
   during consumption.
8. **`sum` concatenates text.** `sum(["a", "b"], "")` → `"ab"`; Python
   refuses (`sum() can't sum strings`).
9. **Item ordering comparisons are numeric/text only** (§5.5), so
   `max([1, 2], [3])` or `sorted([[2], [1]])` without a `key` raise
   `unsupported operand types for <…` where Python orders lexicographically.
   With `key=` (which returns a scalar) sorting works unchanged.
10. **`slice` produces an inert tuple.** `slice(a, b, c)` returns the
    `(a, b, c)` tuple — not a slice object — so `xs[slice_obj]` does not
    slice (a tuple subscript → `TypeError`); use `xs[a:b:c]` (§5.9).
    `slice()` with no arguments is a compile-time arity error (Python
    raises at runtime).
11. **`bytes` display uses Pentest's locked form** `b"…"` (double
    quotes) where Python's repr uses `b'…'`; `bytes(text)` supports only
    UTF-8 (`encoding="ascii"` → `ValueError cstar supports only the
    UTF-8 encoding`) and `errors=` is accepted but never consulted.
    `chr()` rejects surrogates (UTF-8 cannot represent them).
12. **Files:** `seek()` allows end-relative seeks in text mode (Python's
    text wrapper forbids them); mode `"x"` is unsupported (`ValueError`)
    rather than exclusive-create; writing to a read-only file raises
    `OSError file not open for writing` (Python: `io.UnsupportedOperation`);
    an unknown method on a file raises `TypeError` `'file' object has no
    method '<m>'` (Python: `AttributeError`). Line methods keep Python's
    bytes exactly (`readline()` includes the newline, `read(-1)` reads
    everything, `next(file)` reads one line).
13. **`id()` of two identical static text literals may be equal** (shared
    static storage — like CPython's interning, but not guaranteed by the
    same rules); `hash()` returns an `int64` (§5.3 wrap) where Python's
    hash is an unbounded `int`.
14. **`eval`/`exec`/`compile` fail at compile time**, not at runtime
    (§5.11.6).
15. **`vars()`/`dir()` on a class object raise** `TypeError '<t>' object
    has no attributes` — class objects expose no attributes in Pentest
    (§5.12), so Python's `dir(SomeClass)` method listing has no
    equivalent.

#### 5.11.6 Stubs and unavailable builtins

**Nine stubs** are reserved names with no runtime concept behind them.
Arguments are evaluated normally (so side effects still happen), then
the call always raises `RuntimeError` with its own reason — nothing else
about them is checked (no arity policing):

| Call | Message |
|---|---|
| `super()` | `super() is not supported: classes have no inheritance (SPEC §5.11.6)` |
| `property(x)` | `property() is not supported: there are no descriptors or decorators` |
| `classmethod(f)` / `staticmethod(f)` | `…() is not supported: there are no decorators` |
| `issubclass(a, b)` | `issubclass() is not supported: classes have no inheritance` |
| `breakpoint()` | `breakpoint() is not supported: there is no interactive debugger` |
| `help()` | `help() is not supported: there is no interactive help (SPEC.md is the reference)` |
| `memoryview(b)` | `memoryview() is not supported: bytes are immutable and have no buffer views` |
| `object()` | `object() is not supported: define your own class — every class is already a root` |

**The dynamic-code trio** `eval`, `exec`, `compile` is not part of the
language at all: Pentest compiles ahead of time and the runtime never
compiles source — no embedded parser, no interpreter, no megabyte of
compiler in every binary. Calling one is a **compile-time error**
(the message names the call site and §5.11.6):

```
'eval' is not available in cstar: programs are compiled ahead of time
and the runtime never compiles source (see SPEC.md §5.11.6)
```

No runtime entry exists for the trio; they still count as reserved
names (§3.5) — `f = eval` reports `undefined variable 'eval'`.

#### 5.11.7 Exceptions raised by the builtins

The catchable class set gained three members (twelve total, §6.10):
`StopIteration` (`next()` exhausted with no default — empty message),
`FileNotFoundError` (`open()` on a missing path), and `OSError`
(file write/seek/tell/flush failures and `file not open for writing`).
Everything else raised by §5.11.3 uses the existing nine classes:
`TypeError` (argument shapes and conversions), `ValueError` (bad
values/ranges), `AttributeError` (getattr/delattr misses),
`ZeroDivisionError` (`divmod`/`pow`-adjacent division), `KeyError` (map
subscript misses), `RuntimeError` (the nine stubs).

#### 5.11.8 Network builtins: `http_get`, `http_post`, `url_encode` (unreleased)

Three libcurl-backed builtins give programs HTTP access. Like the original
fifteen (§5.11.2) they are call-target-only, take **no** named arguments
(`builtin '<n>' does not take named arguments`) and have their arity checked
at compile time:

- `http_get(url[, headers])` — **1 to 2 arguments** → `text`. Performs an
  HTTP GET against `url` and returns `"STATUS\nBODY"`: the decimal HTTP
  status code, a newline, then the raw response body. On a connection/TLS
  failure before any response byte is received, the status is `0` and BODY
  is the curl error string. `headers` is `None` or a `text` of
  newline-separated `Name: value` lines (blank lines are skipped). Runtime
  type errors: `http_get() expects text url, got <t>` and
  `http_get() expects text or None headers, got <t>`.
- `http_post(url, body[, headers])` — **2 to 3 arguments** → `text`, the same
  `"STATUS\nBODY"` return shape. `body` is `text` or `None` (`None` sends an
  empty body). A POST with a `text` body and no caller-supplied
  `Content-Type` sends `Content-Type: application/json`; a caller-supplied
  `Content-Type` suppresses that default. `Accept: application/json` and
  `User-Agent: jev-pipeline/1.0 (pentest)` are always appended. Runtime type
  errors: `http_post() expects text url, got <t>`,
  `http_post() expects text or None body, got <t>`,
  `http_post() expects text or None headers, got <t>`.
- `url_encode(text)` — **1 argument** → `text`: percent-encoding for query
  strings and form bodies. The unreserved characters `A`–`Z`, `a`–`z`,
  `0`–`9`, `-`, `_`, `.`, `~` stay literal; every other byte of the UTF-8
  text becomes `%XX` with uppercase hex digits. Runtime type error:
  `url_encode() expects text, got <t>`.

**Compile-time arity errors** (wording is exact, §9.2):
`'http_get' expects 1 to 2 argument(s), got <n>`,
`'http_post' expects 2 to 3 argument(s), got <n>`,
`'url_encode' expects 1 argument(s), got <n>`.

Both HTTP builtins share **one persistent libcurl handle** per process:
connections stay alive across calls, so every call after the first skips
the TCP + TLS handshake; HTTP/3 is preferred with an HTTP/2 fallback (ALPN);
TCP fast open is enabled; each request has a 30-second timeout.

**SQL injection in Pentest.** UNION-based GET SQLi and Burp-style raw-POST
templating are done in `.pen` via `http_get`/`http_post` + `url_encode` —
see the worked walkthroughs in [docs/guide/sqli.md](docs/guide/sqli.md),
which run against a deliberately vulnerable local lab on
`http://127.0.0.1:8080` (the lab and the full scripts are not included in the
repository).

### 5.12 Classes, instances and attributes (2C)

**Declaration.** `class Name:` is a block-form compound statement whose
body holds **function declarations and `pass` only** — anything else →
semantic error `only function declarations are allowed in a class body`.
Classes may be declared **at top level or directly in `main:`** — inside
a function → `classes must be declared at the top level`; a duplicate name
→ `duplicate class 'Name'`. The class name is an ordinary guarded global
(§8): reading it before its declaration statement executes →
`global variable 'Name' used before assignment` (§9.3). There is no
inheritance: `class C(Base):` is a syntax error (§4.1), and class
attributes do not exist — methods only.

**Methods.** Every method is declared `name(self, …):` (or `name():` for a
parameterless method, which is unusual) with
`self` as an ordinary **explicit** first parameter (no injection, no name
enforcement). A duplicate method name in one class →
`duplicate method 'm' in class 'C'`. Methods are **not bare names**: a
bare read (`f = move`) or call (`move(p, 1)`) of a method → the normal
`undefined variable` / `undefined function` errors (§8) — they are reached
only through an object (`obj.m`, §5.12) or their class. Method
parameters, defaults, `*varargs` and `**kwargs` behave exactly like
functions' (§7.1).

**Construction.** `C(args…)` evaluates to a freshly allocated `instance`:
- `__init__` is found in the method table and invoked as
  `__init__(instance, args…)`; its return value is discarded.
- No `__init__` **and** no arguments → a bare instance.
- No `__init__` **with** arguments → runtime error
  `class 'C' takes arguments but has no __init__`.
- Argument binding errors (count, unknown/duplicate names, missing
  required) are the ordinary binder's (§7.1), evaluated in source order.

**Attribute assignment** `obj.name = v` / `obj.name <op>= v` (§4.3)
requires an `instance` (the object is evaluated exactly once; a compound
form reads the field before the RHS). Otherwise → runtime error
`cannot assign attribute '<name>' on '<t>'` where `<t>` is the target's
type name (`'int'`, `'class'`, `'text'`, …).

**Attribute read** `obj.name`:
1. on an `instance`: the instance field, else
2. the class's method table → a bound **`method`** value, else
3. runtime error `AttributeError: '<Class>' object has no attribute '<name>'`.

On any other value the read fails at step 3 with the same message shape
(`<t>` = type/class name of the object — `'<t>' object has no attribute
'<name>'`). Fields are dynamic: created by assignment, holding any value,
replaced freely; there is no deletion syntax.

**Bound methods.** A `method` value holds {receiver, function}. `obj.m`
(bare) displays as `<method 'm' of C>` and `type(obj.m)` is `"method"`;
`obj.m(args)` — and calling a stored `method` value — invokes the function
with the receiver prepended positionally. **Named arguments work on bound
methods** (they bind through the ordinary binder, §7.1); the builtin
container methods of §5.10 stay positional-only (runtime
`method arguments must be positional`).

**Exception classes as values (§6.10)** behave like classes: they
construct instances when called, display as `<class ValueError>` and are
readable as ordinary globals. All the flow rules above apply.

**Unhashable (2C):** classes, instances, exception instances and bound
methods are **unhashable** — using one as a map key or set element raises
`unhashable type: '<t>'` (`'Point'` for an instance of `Point`, `class`,
`method`). Identity comparisons use `==`/`is` instead (§5.13).

### 5.13 Object identity: `is` / `is not` (2C)

`a is b` sits at comparison precedence (level 5, left-associative, no
special chaining — §4.2); `a is not b` is the negation of the same test
(parsed as one operator — `not` after `is` is not the prefix `not`).
Operands evaluate once, left then right, like every binary operator;
`unset` operands raise the usual used-before-assignment guards first
(§9.3). `is` is **never** constant-folded: identity is a runtime fact.

The test is true iff:

- **heap objects** — `list`, `map`, `set`, `tuple`, `function`, `class`,
  `instance`, exception instances, bound `method`: it is **the same
  object**. Equal-but-distinct objects (two `Point(1)` instances, two
  `ValueError("x")` instances, two identical lists) are not identical.
- **`text` / `bytes`**: they **share storage** — same buffer, same
  length. Content built separately (`"ab" + "cd"` twice, or `base + ""`)
  is `==` but not `is`. (Whether identical source literals share storage
  is an implementation detail of sharing, not a language guarantee.)
- **non-heap values** — `int`, `float`, `bool`, `None`, `range`: the same
  tag **and** equal value: `1 is 1` → `True`, `1.5 is 1.5` → `True`,
  `None is None` → `True`, `1 is 1.0` → `False` (different tags).
- different tags → `False`, always.

It is never a runtime error otherwise.

## 6. Control flow and logical operators

### 6.1 Truthiness

Truthiness is **total**: converting any value to a truth value never raises a
runtime error.

| Value | Truthiness |
|---|---|
| `False` | falsy |
| `None` | falsy |
| `int` `0` | falsy |
| `float` `0.0` | falsy |
| `text` of 0 code points (empty `text`) | falsy |
| `bytes` of 0 bytes | falsy |
| empty `list`, empty `map`, empty `set`, empty `tuple` | falsy (2B) |
| `range` with no representable member | falsy |
| `class`, `instance`, bound `method`, exception instance, `function` | truthy (2C) |
| everything else (`True`, nonzero numbers, nonempty text/bytes/collections, nonempty ranges) | truthy |

### 6.2 `and`, `or`, `not`, and the conditional expression

Logical operators return **`bool`** (never their operands — a deliberate
difference from Python) while short-circuiting:

- `a and b` — evaluate `a`; if `a` is falsy the result is `False` and `b` is
  **not** evaluated; otherwise the result is `bool(b)`.
- `a or b` — evaluate `a`; if `a` is truthy the result is `True` and `b` is
  **not** evaluated; otherwise the result is `bool(b)`.
- `not a` — `bool` negation of `a`'s truthiness: `not 0` is `True`.

The conditional expression `a if c else b` (level 1, §4.2): `c` is evaluated
exactly once, then exactly one branch is evaluated. `and`, `or` and `not` are
keywords and cannot be identifiers (§3.5).

### 6.3 Membership `in`

`a in b` is an ordinary binary expression at comparison precedence (level 5,
§4.2), usable anywhere an expression is allowed; it evaluates to `bool`. It
is equivalent to Python's `in`, spelled `needle in haystack` (it replaced the
retired Stage 2A `->` spelling in v0.3):

- `b` a `range` value → integer-in-range test using exact arithmetic (no
  overflow, §6.9): `10 in range(3, 10)` is `False` (stop is exclusive).
- `b` a `text` → substring containment: `"ell" in "hello"` is `True`;
  an empty needle is `True`.
- `b` a `bytes` → subbytes containment (same empty-needle rule).
- `b` a `list` or `tuple` → is any element `==` to `a`; a `map` → is `a`
  a key; a `set` → is any element `==` to `a` (all 2B, §5.8).
- anything else (including every other right-operand type, e.g. `int` or
  `float`) → runtime error
  `unsupported operand types for(): <t1> and <t2>`. **The message keeps the
  internal operator spelling `()`** — it is normative and the tests match on
  it — even though the surface operator is `in`.

`in` targets are all of `range`, `text`, `bytes`, `list`, `tuple`,
`map` and `set`. In a `for` header (§6.6), `in` is the binding keyword.

### 6.4 `if` / `elif` / `else`

- Shape: `if expr: suite`, then zero or more `elif expr: suite`, then an
  optional `else: suite`.
- Conditions are evaluated in order until one is truthy (each condition at
  most once); exactly the first truthy branch runs. If no condition is
  truthy, the `else` branch runs when present, otherwise nothing runs.
- Suites may be indented blocks or single-line bodies (§4.3).
- An `if` statement consumes its whole `elif`/`else` chain; a following
  orphan `elif`/`else` at statement level is a parser error (§9.1).

### 6.5 `while`

- `while expr: body` is a **pre-test** loop: the condition is checked before
  every iteration, so a `while` may run zero times.
- The condition uses §6.1 truthiness; the suite may be a single-line body
  (§4.3).
- `break` and `continue` apply to the innermost loop (§6.8).

### 6.6 `for ... in ...`

- Shape: `for var in iterable: body` with an indented block (no single-line
  form); `var` must be a bare identifier immediately followed by `in`
  (§4.3).
- Iteration:
  - a `range` value (§6.9) — each iteration binds `var` to the next `int`;
  - `list`, `tuple` — elements; `set` — elements (insertion order);
    `map` — **keys** (insertion order);
  - `text` — each iteration binds `var` to a **1-code-point `text`**
    (2B: `text` is UTF-8 code points, never a raw byte);
  - `bytes` — each iteration binds `var` to an `int` byte (0–255);
  - anything else → runtime error `'<t>' object is not iterable`, raised at
    loop entry.
- `var` is assigned before the body runs on each iteration.
- The loop variable **leaks** after the loop (Python-like): it keeps its
  last binding — at top level / in `main:` as a global, inside a function
  as a local (§8).
- Mutating a collection **during its own iteration is undefined** (2B — no
  error is guaranteed); strings and ranges are immutable, so the 2A case
  cannot arise.

### 6.7 `switch` / `case`

- Shape: `switch expr:` followed by an indented block of one or more `case`
  clauses (§4.1); a `switch` with no cases is a parser error (§9.1).
- Case labels are **constant literals only**: `INT`, `-INT`, `FLOAT`,
  `-FLOAT`, `STRING`, `True`, `False`, `None`. Expressions, names and other
  non-constants are parser errors (§9.1).
- A non-wildcard arm matches iff `scrutinee == label` under M1's **total
  `==`** (§5.5): `case 1:` matches int `1` and float `1.0`;
  `case True:` does **not** match int `1`.
- Arms are tried in source order; the **first match wins** and the switch
  **ends automatically** — there is no fall-through and no `break` is needed
  (`break` is loop-only, §6.8).
- No arm matches → the `case *:` wildcard arm runs if present, else nothing
  runs. The wildcard is optional and must be the last case (§9.1).
- Two cases with the same value under total `==` → compile error
  `duplicate case value in switch` (§9.2).

### 6.8 `break`, `continue`, `pass`

- `break` exits the innermost loop; `continue` skips to the innermost loop's
  next iteration.
- Both are compile-time errors outside any loop: `'break' outside of a loop`
  and `'continue' outside of a loop` (§9.2). A `switch` does **not** count
  as a loop: a `break` written inside a switch inside a loop breaks the
  **loop**.
- `pass` is a no-op statement, valid anywhere a statement is. Blocks still
  require at least one statement (§4.3) — `pass` is the placeholder for a
  deliberately empty body.

### 6.9 `range()`

`range` is a reserved builtin name governed by exactly the same rules as
`print` (§3.5). Forms: `range(stop)`, `range(start, stop)`,
`range(start, stop, step)` — 1 to 3 arguments, all `int` (defaults
`start = 0`, `step = 1`; `range(5)` is `range(0, 5, 1)`):

- Argument-count errors are compile-time (§9.2): 0 or more than 3 arguments
  → `function 'range' expects 1 to 3 argument(s), got <n>`.
- Float/bool/text argument → runtime error
  `range() expects int arguments, got <t>`.
- `step == 0` → runtime error `range() arg 3 must not be zero`.
- **The stop is exclusive.** With `step > 0` iteration continues while
  `i < stop`; with `step < 0` while `i > stop`. A range is empty when it
  contains no `int64`.
- Ranges are **values**: lazy (never materialized), usable in `for` (§6.6)
  and `in` (§6.3), printable (§5.6: `print(range(3))` →
  `range(0, 3, 1)`), and compared structurally with total `==`/`!=` (§5.5).
- Iteration must never exhibit undefined behavior: when the next index would
  leave `int64` range, iteration **stops** (so even a huge range
  terminates). Membership uses exact arithmetic for any int64 bounds.

### 6.10 Exceptions: `try` / `except` / `finally` / `raise` (2C)

**Syntax** (§4.1): the `try` body is block-form only (like `for`/`switch`);
`except`/`finally` bodies are suite form (single-line bodies legal, like
`if`/`while`). At least one `except` or `finally` clause is required —
otherwise parser error
`try block requires at least one 'except' or 'finally' clause`; a second
`finally` → `duplicate 'finally' clause`. An except clause is bare
(`except:` — catches everything), or `except <expr>:` /
`except <expr> as <name>:`; the expression parses at `ternary` level and
stops before `as`/`:`. The `as` binding is scoped like other statement
bindings (§8) and lives only for the clause body.

**Builtin exception classes.** Exactly twelve exist at program start, as
readable class values: `Exception`, `RuntimeError`, `TypeError`,
`ValueError`, `NameError`, `ZeroDivisionError`, `IndexError`, `KeyError`,
`AttributeError`, plus the three added with the Python-parity builtins
(§5.11.7): `StopIteration`, `FileNotFoundError`, `OSError`. Calling one
constructs an instance from its argument
(≤ 1 argument; a non-`text` argument is displayed into the message;
0 arguments → empty message). They display as `<class ValueError>`,
`type(ValueError)` is `"class"`, and instances display
`ValueError: division by zero` (empty message → bare class name),
`type(e)` = `"ValueError"`, with `.type` / `.message` text attributes
(§5.12). Instance equality/`is` is identity (§5.13).

**Runtime errors are throws.** Every runtime error of §9.3 is raised as
an instance of the mapped class with its message **unchanged**; the
message strings of §9.3 are byte-identical to 2B. The mapping (the
"closest Python-style class" rule):

| Class | What raises it (§9.3 examples) |
|---|---|
| `NameError` | used-before-assignment guards: `global variable '…' used before assignment`, `local variable '…' used before assignment` |
| `TypeError` | unsupported operand types, not callable / not iterable / not subscriptable, no method, arity and argument-name binding errors, non-integer indices/slices, unhashable keys, builtin argument-type errors, `except expects a class, got <t>`, `method arguments must be positional`, `cannot assign attribute '…' on '<t>'`, `exceptions must be class or instance, got <t>` |
| `ValueError` | bad value for a conversion or range: `int()`/`float()` parse failures, `negative exponent`, `shift count must be in range 0..63`, `slice step cannot be zero`, `range() arg 3 must not be zero`, unpack count errors, `invalid UTF-8 in decode()` |
| `ZeroDivisionError` | `division by zero`, `modulo by zero` |
| `IndexError` | `list index out of range`, `'<t>' index out of range` (text/bytes/tuple) |
| `KeyError` | `map key not found`, `value not in set`, `pop from an empty set` |
| `AttributeError` | `'<t>' object has no attribute '<n>'` (§5.12), `getattr`/`delattr` misses |
| `StopIteration` | `next()` exhausted with no default (empty message — §5.11.3) |
| `FileNotFoundError` | `open()` on a missing path: `[Errno 2] No such file or directory: '<path>'` (§5.11.3) |
| `OSError` | file operation failures: `file not open for writing`, `seek failed`, `tell failed`, `flush failed` (§5.11.3) |
| `RuntimeError` | internal fallbacks (out of memory inside error reporting, unknown-class default) and the nine always-failing stubs (§5.11.6) |
| `Exception` | the generic class for a user `raise Exception("…")`; never raised by the runtime itself. Matching is exact (see **Matching**), so `except Exception:` catches **only** exceptions whose class is exactly `Exception` — use a bare `except:` to catch everything |

**Matching.** `except X:` runs its clause when the pending exception's
class **is** `X` — exact identity (§5.13); there is no inheritance, so
`except TypeError:` does **not** catch a `ValueError`. `except:` catches
everything. A non-class except expression at runtime →
`TypeError: except expects a class, got <t>`. If no clause matches, the
exception continues unwinding — after `finally` (below).

**`raise`.** `raise <expr>` requires an exception **instance** (re-raises
it as-is — re-raise via the `as` binding: `raise e`) or a **bare
exception class** (constructs an instance with an empty message).
Anything else → `TypeError: exceptions must be class or instance, got
<t>` — this includes user classes (`got class`) and user instances
(`got Point`); user classes are not raisable in 2C (§5.12). There is no
bare `raise` statement (syntax error — the grammar requires an
expression).

**`finally`.** Runs exactly once per entry path: normal completion,
`break`/`continue`/`return` out of the `try`, and exception unwind. A
`break`/`continue`/`return` crossing a `finally` runs the block first.
An exception raised inside an `except` clause or during unwinding
**replaces** the in-flight exception (chaining is not implemented —
documented). Errors in except-matching expressions propagate normally.

**Uncaught.** An exception that reaches the bottom of the handler stack
prints `<src>:<line>: runtime error: <message>` to stderr and exits 1 —
byte-identical to the M1/2B fatal-error format (the class name is **not**
part of the uncaught output; §9.4).

## 7. Execution model

**Unchanged from M1**, except for the Stage 2B amendments in §7.1/§7.2
below. Seven rules (restated from docs/history/design.md §4 with the M1 decisions
applied):

1. **Discover declarations before execution.** Before any statement runs,
   the compiler performs a declaration-discovery phase: every top-level
   `function`
   declaration in the file and the optional `main:` block are registered.
   Discovery registers declarations only — it never executes a body.
   Because discovery precedes execution, forward references work (rule 6).
   **2B:** nested `function` declarations inside a function body or
   `main:` are *not* part of this phase — they are created when their
   enclosing function is entered (§7.2). **2C:** class names are also
   discovered in advance (so a class name is a known global from the
   start), but the class **statement** executes at its declaration point —
   a read before that point runs the normal used-before-assignment guard
   (§5.12).
2. **Top-level statements run in source order, and `main:` runs in
   place.** Statements outside function declarations execute top to bottom.
   When execution reaches the `main:` block, its body executes immediately
   at that textual position — in global scope (§8) — and then top-level
   execution continues with the statements following the block.
3. **Function definitions do not execute their bodies.** Defining a function
   registers it; the body runs only when the function is called. The
   `main:` block is not a function — its body runs when top-level
   execution reaches it (rule 2).
4. **A file without `main:` is a normal script.** Top-level statements
   execute from top to bottom; nothing more is required.
5. **Only called function bodies run.** A body runs when reached through a
   call chain starting at top-level execution (or `main:`), including
   transitive calls. Functions that are never called never execute.
6. **Declarations may appear before or after their callers.** Thanks to
   rule 1, a function may be called from code that appears earlier in the
   file — from top level, from `main:`, or from another function,
   including mutual recursion.
7. **Variables are not hoisted; assignments happen in execution order.**
   Every global exists from program start with tag `unset`; its value
   appears only when its assignment executes. Reading a global whose
   assignment has not yet executed is a **runtime error**
   `global variable '<name>' used before assignment` (DESIGN left open
   whether this could be caught at compile time; M1 decides: runtime error,
   §9.3). Locals behave the same way (§8).

Uniqueness: at most one `main:` block per file (a second one is a semantic
error), and `main:` cannot be called explicitly — any non-block use of
`main` is a syntax error (§3.5).

### 7.1 Function entry and calls (2B)

**Parameter list** (§4.1): `[positional…] [*vararg] [**kwarg]` — required
parameters first, then defaulted ones, then at most one `*name`, then at
most one `**name` (parser errors: `positional parameter cannot follow
'*name'`, `only one '*' parameter is allowed`, `'*name' cannot follow
'**name'`, and the `**` analogues). A default after a required parameter
out of order → semantic error
`parameter with default must come after required parameters`.

**Call binding.** Positional arguments fill positional parameters left to
right; named arguments (`f(x=1, y=2)`) fill by name. A named argument
whose name **is** a parameter always binds that parameter (even when the
function has `**kwargs`); named arguments that match no parameter are
collected into `**kwargs` as a fresh `map`; extra positionals beyond the
fixed parameters are collected into `*vararg` as a fresh `list`; an unused
vararg/kwarg is an empty `list`/`map`. Binding errors are compile-time
whenever the callee declaration is known statically (§9.2); otherwise the
same checks run at call time, and calling a non-function →
`object of type '<t>' is not callable`.

**Defaults are evaluated at call time, fresh for each omitted argument,
in parameter order** — never shared between calls (so a mutable default
cannot leak state from one call into the next). A default expression may
read globals and earlier parameters of the same call.

**Function values.** Reading a function's name (top-level or nested)
yields a **function value**; it may be assigned, passed as an argument,
returned, stored in a collection, compared with `==` (§5.5) and called
indirectly (`f(x)(y)`, `(get_fn())(1)`). Indirect calls bind dynamically
(§9.2/§9.3 static-vs-runtime checks). `print`/`range` and the other
builtins remain call-target-only and never yield values.

**Recursion** is unchanged (M1/2A): a function may call itself, directly
or through a function value, and mutual recursion works via discovery.
**Recursion-depth safety:** deep recursion may overflow the
native C stack — a documented limitation (§12); a controlled runtime
failure is acceptable (no guaranteed catch in 2B).

### 7.2 Nested declarations and hoisting (2B)

1. A nested function declaration (`name():` or `name(params):`) is legal
   **inside a function body and inside `main:`** (any statement position,
   including inside `if`/`for`/`while` bodies). Top-level (outside `main:`)
   declarations keep the M1 discovery rule (§7).
2. **Name binding:** a nested declaration's name becomes a **local** of the
   enclosing function, initialized to its closure at entry; a declaration
   in `main:` or at top level registers an ordinary **global function**
   (M1 behavior). A nested declaration whose name collides with a
   parameter or an existing local is a compile error using the existing
   M1 wording (`name 'f' is already declared as a function`), and a later
   assignment to a nested function's name is the same error — never a
   silent rebind.
3. **Hoisting:** nested closures are created in the **enclosing function's
   prologue**, before any body statement runs: heap cells are allocated
   for every captured local (initialized to `unset`, so the M1
   used-before-assignment guard still applies), the closures are built
   over those cells, and they are stored into their named locals.
   Consequences: a nested function is callable before its source line
   (forward references work — matching M1's discovery philosophy); a
   declaration's body never executes at its declaration; and a nested
   function defined under a condition is still created at entry —
   **declaration position controls nothing** (documented divergence from
   Python).
4. **Capture:** a nested function captures by reference every name that
   resolves to an enclosing function's local/parameter, through heap cells
   that outlive the frame; all reads and writes of the owner go through
   the cell, so rebinding is visible in both directions. Capture is
   **transitive**: if `b` contains `c` and `c` mentions `a`'s local `x`,
   then `b` captures `x` too and hands it down. Globals are static storage
   and are never captured.
5. Calls to a nested function by bare name go through the local's function
   value (dynamic call); when the callee declaration is known — including
   nested callees — arity and named-argument checks stay **static**.

### 7.3 Memory management: the garbage collector (2C)

Heap objects (§5.1) — `text`, `bytes`, `list`, `map`, `set`, `tuple`,
`function`, `class`, `instance`, `method` and exception instances — are
allocated through a registry that a **non-moving mark & sweep** collector
reclaims (contract §1; 2B's "everything leaks" limitation is gone, §12):

- **No movement.** Survivors keep their addresses; `is`/`is not` identity
  (§5.13) is stable for every live object, and interior pointers into
  text/bytes buffers stay valid (the collector tracks buffer bases, so a
  `text` value holding a slice shares the buffer's block).
- **Roots.** Exactly one pointer per program global (the codegen emits a
  root vector per global, initialized before any code runs), plus the
  live shadow frames (locals and temp slots), pending call argument
  vectors, the collector protect stack (used across e.g. `__init__`
  dispatch), the in-flight exception handlers' values, and the builtin
  exception-class singletons.
- **Safe points.** Collection happens only at function entries, loop
  back-edges and statement boundaries; generated temps are cleared at
  statement end so dropped cycles become collectible. Between safe points
  the program never sees a freed object.
- **Cycles** (mutual `list`/`map` references, instances in fields of each
  other) are collected like any other garbage.
- **Observability.** `collect()` (§5.11) forces a full collection and
  returns the number of blocks freed. Which of several program points
  actually observes a given collection is unspecified — programs must not
  depend on the timing, only on memory staying bounded.
- **Not provided:** finalizers, weak references, pinned/non-moving escape
  hatches beyond the guarantee above, and any user-facing heap
  introspection (§13).

Mutation of a collection during its own iteration remains undefined (2B).

## 8. Scope and name resolution

- **Top level and the `main:` block share global scope.** Every assignment
  in either place creates or updates a global. The `main:` block is *not*
  a function — it has no locals, no parameters, and its body resolves names
  exactly like top-level code.
- **Variables and functions are separate namespaces (0.3).** Because calls
  use `f(…)` and reads use the bare name `f`, one name may be both a
  global variable and a global function. Each site picks its namespace by
  usage: a bare read resolves the **variable** first (locals, parameters and
  enclosing bindings as usual, then the global variable, and only a
  function-only name yields a function value, §7.1); a call resolves the
  **function** first (falling back to a dynamic call through a global
  variable that holds a function value). `global a` inside a function
  redirects assignments and reads to the variable namespace while `a()`
  still calls a same-named function. Class names are the exception: a class
  shares its name with neither variables nor functions (§5.12) — classes are
  constructed with the same `()` call syntax, so a same-named function
  would shadow the constructor.
- **Functions are Python-style:**
  - Parameters are locals.
  - A name assigned **anywhere** in the function body is local for the whole
    body. Reading it before its first executed assignment is a runtime
    error: `local variable 'x' used before assignment`.
  - Consequently, **without a declaration** assignments inside a function
    are **always** to a local or a parameter — a function cannot *silently*
    modify a global (a `global` declaration can, below). A local may
    share a name with a global — inside the function, the name refers to
    the local.
  - Any other name reads the corresponding global (with the usual
    used-before-assignment guard: `global variable 'x' used before
    assignment` if the global's assignment has not executed).
  - Assigning to a parameter inside its function is allowed and updates the
    parameter.
- **`global` and `nonlocal` (2B):** both are keywords; a declaration lists
  one or more names (`global count` / `global a, b`). `global name` makes
  the function's subsequent assignments target the module global;
  `nonlocal name` targets the **nearest enclosing function's** binding of
  that name. Reading a global needs no declaration. **Placement rule:** a
  declaration for a name must appear before any binding or use of that
  name in the function — otherwise the compile error
  `name 'x' is used before 'global' declaration` (resp.
  `... 'nonlocal' ...`). `nonlocal` with no enclosing binding →
  `no binding for nonlocal 'x' found`. Assigning to a name that resolves to
  an enclosing function's local **without** `nonlocal` →
  `name 'x' is a local of an enclosing function; declare
  'nonlocal x' to rebind it`. Mutating a mutable object's *contents*
  (`xs.append(1)`, `m["k"] = 1`) is **not** a rebind and needs no
  declaration. `global` at top level or in `main:` is allowed but
  redundant (both places already are global scope).
- **Nested functions and closures (2B):** a `function` declared inside
  another function captures the enclosing function's locals **by
  reference** through heap cells that outlive the frame — reading and
  rebinding are visible in both directions, deep nesting captures from the
  correct enclosing frame, and capture is transitive (§7.2). Nested
  closures are created in the enclosing function's prologue, so forward
  references to them work and a declaration's position (e.g. under a
  condition) controls nothing.
- **Loop variables (Stage 2A):** a `for` loop variable follows M1's
  assigned-anywhere rule — local for the whole enclosing function, global at
  top level / in `main:` — and it **leaks** after the loop: it keeps its
  last binding (§6.6). Loop targets participate in every reserved-name rule:
  `cannot use reserved name 'print' as loop variable` (the same message
  with any builtin's name, §3.5). A top-level `for` target whose name is
  also a function is allowed (§8): the loop variable is the variable
  namespace, `name()` still calls the function.
- **Classes, methods and exception classes (2C):** a top-level or
  `main:` `class` name is an ordinary guarded global, registered in the
  declaration scan (§5.12, §7 rule 1); reading it before its statement
  executes runs the usual guard. **Method names are never names in any
  scope** — a bare read (`f = move`) → `undefined variable 'move'`, a bare
  call (`move(p, 1)`) → `undefined function 'move'`; methods resolve only
  through `obj.m` (§5.12). The twelve builtin exception classes are
  ordinary globals pre-bound at program start (§6.10): they are never
  used-before-assignment, and a user `class ValueError:` rebinds the name
  at its declaration point like any global assignment.
- **Every reserved builtin mirrors `print`** throughout the reserved-name
  machinery (§3.5): `cannot assign to reserved name '<b>'`,
  `cannot use reserved name '<b>' as parameter`,
  `cannot declare function with reserved name '<b>'`, and a bare read is
  `undefined variable '<b>'` — for all seventy names of §5.11 (the
  legacy fifteen, the forty-three parity builtins, the nine stubs and
  the `eval`/`exec`/`compile` trio).
- **Cross-scope variable access (`0.2.1`, §4.1 `qual_ref`).** Outside
  ordinary name resolution, a `variable.<qualifier> <name>` atom names a
  binding in another scope directly:
  - `variable.global <name>` — a module global (created by top-level /
    `main:` assignment or a `global` declaration anywhere); never needs
    a declaration at the site. Function names and builtins are rejected.
  - `variable.parent <name>` — the **nearest enclosing function** that
    binds `<name>` (searching outward; the current function is skipped).
    `global`-declared names in enclosing functions do not count as
    bindings — use `variable.global`.
  - `variable.<fn> <name>` — a param (including `*args`/`**kwargs`) or
    local of another function **sharing the site's scope**: a top-level
    function when the site is at top level or in `main:`, otherwise a
    nested function of the site's own enclosing function (a function may
    name itself; reach the enclosing function with `variable.parent`).

  Each form is an ordinary expression (a read) and may also be an
  assignment target — plain or augmented (§4.3) — or a leaf of a
  destructuring target (§4.4), possibly followed by the `[…]`/`.attr`
  postfix chain. It may **not** be a `for` target or an `except … as`
  binding (§4.3).

  Resolution is **declaration-order independent**: qualified sites are
  resolved in a final semantic phase after every function's
  params/locals/boxing flags are final, so a site may target a function
  declared later in the file. Failures are compile errors naming the
  exact form: `no binding for 'variable.<q> <n>' found` or
  `'<fn>' is not a function sharing this scope`.

  **One storage per binding.** The moment any site references a binding
  through `variable.`, that binding is *pinned*: its storage becomes a
  single program-lifetime slot shared by **every** access — the owner's
  own reads/writes included. A pinned slot starts unset (reading it
  before the owner's first assignment → the usual
  `used before assignment` runtime error) and keeps its value across
  owner invocations (C `static`-local shape) — re-entering the owner does
  not reset it. Recursive activations share the one slot. If a closure
  also captures the binding, the cell is immortal (rooted for the whole
  program), so closures from every invocation share it. The binding is
  excluded from the native-int lane (one dynamic storage everywhere —
  see [docs/performance/optimization.md](docs/performance/optimization.md) §7). Full model,
  lowering and lookup details: [docs/reference/cross-scope-access.md](docs/reference/cross-scope-access.md).
- **Resolution phases (semantic pass):**
  1. Scan the top level: register all top-level `function` declarations,
     the `main:` block and the top-level `class` declarations; collect
     global names from top-level and main-block assignments.
  2. Resolve top-level and main-block statements: reads → Global (or error,
     §9.2); assignments → Global.
  3. Resolve each function body (chained against its enclosing function's
     scope): parameters → Param; `global`/`nonlocal`-declared names →
     redirected per the declarations above; names assigned anywhere in the
     body → Local (a nested `function` name counts as a local, §7.2);
     captured names resolve to the owning enclosing frame's binding; else,
     if the name is a known global → Global; else error.
  4. Resolve every `variable.` qualified site (the cross-scope rules
     above) once all bindings are final — this phase also marks each
     referenced binding as pinned (shared storage, native-lane poison).

  A name that is only ever assigned inside functions never becomes a global;
  a function body that reads a name which is neither a parameter, nor
  assigned in that body, nor a capture, nor assigned at top level /
  in `main:`, gets
  `undefined variable '<name>'`.
- **`Program.globals`** (visible to the backend) holds global names in
  first-assignment (source) order: names assigned at top level or in the
  `main:` block, plus names resolved as Global in function bodies.

Example (a function reading a global):

```python
a = 10

show():
    print(a)

main:
    show()
```

Expected output:

```text
10
```

Example (writing a global without a declaration — this program fails at
runtime):

```python
count = 0

bump():
    count = count + 1

main:
    bump()
```

`count` is assigned inside `bump`, so it is local for the whole body; the
read on the right-hand side happens before any executed assignment:

```text
<bump.pen>:4: runtime error: local variable 'count' used before assignment
```

(exit code 1). Reading a global from a function works without any
declaration; *writing* one requires `global` (2B):

```python
count = 0

bump():
    global count
    count = count + 1

main:
    bump()
    bump()
    print(count)
```

Expected output:

```text
2
```

## 9. Errors

All compile-time diagnostics print to stderr in the form

```text
<file>:<line>:<col>: error: <message>
<source line>
  <caret line>
```

where `<file>` is the path as invoked. Any compile error → exit code 1.

### 9.1 Lexer/parser errors

The lexer and parser report only the **first** error and stop.

| Condition | Message |
|---|---|
| tab in leading whitespace | `tabs are not allowed for indentation` |
| dedent to a never-seen level | `inconsistent indentation` |
| unterminated `/* ... */` | `unterminated block comment` |
| unterminated `""" ... """` | `unterminated triple-quoted comment` |
| integer literal > int64 range | `integer literal out of range` |
| bad string escape | `unknown escape sequence '\x'` |
| newline/EOF inside a string | `unterminated string literal` |
| newline/EOF inside a `b"..."` bytes literal | `unterminated bytes literal` |
| bad `\xHH` escape in a bytes literal | `invalid \x escape` |
| byte that starts no token | `unexpected character 'x'` |
| `try:` with no `except`/`finally` clause | `try block requires at least one 'except' or 'finally' clause` |
| second `finally:` on one `try` | `duplicate 'finally' clause` |
| slice used as an assignment target (`xs[1:2] = v`) | `cannot assign to slice` |
| positional parameter after `*name` / `**name` | `positional parameter cannot follow '*name'` (resp. `'**name'`) |
| second `*name` parameter | `only one '*' parameter is allowed` |
| `*name` after `**name` | `'*name' cannot follow '**name'` |
| second `**name` parameter | `only one '**' parameter is allowed` |
| empty/missing indented block after `:` (all block forms) | `expected an indented block after ':'` |
| single-line suite whose body is compound or absent | `expected a simple statement on the same line after ':'` |
| `for` target not followed by `in` (e.g. `for i:`, `for a, b in x:`) | `expected 'in' after loop variable` |
| `switch` with zero case clauses (block not starting with `case`) | `expected at least one case in 'switch'` |
| non-constant case label (a name, an expression, …) | `case label must be a constant literal` |
| `case *:` followed by another case | `'case *' must be the last case in a switch` |
| statement starting with `case` outside a `switch` | `'case' outside of 'switch'` |
| orphan `elif` (no matching `if`) | `'elif' without a matching 'if'` |
| orphan `else` (no matching `if`) | `'else' without a matching 'if'` |
| `function main:` or calling/reading `main` (any non-block use of `main`) | `'function' keyword was removed; declare with 'name():' or 'name(params):'` / `expected ':' after 'main'` / `unexpected token 'main'` / `nested 'main' block is not allowed` |
| everything else (semicolons, `x = (y = 1)`, unclosed brackets, bad tokens, …) | a generic syntax error (first error only, with caret) |

An `if` statement consumes its whole `elif`/`else` chain, so a following
orphan `elif`/`else` triggers the messages above.

### 9.2 Semantic errors

The semantic pass **collects and reports every error**, then fails. Prefix:
`<file>:<line>:<col>: error: `. Wording is normative (substring match).

| Condition | Message |
|---|---|
| duplicate function name | `duplicate function 'f'` |
| duplicate parameter | `duplicate parameter 'a' in function 'f'` |
| second `main:` block | `only one 'main:' block is allowed per file` |
| parameter named a builtin (any reserved builtin name of §3.5) | `cannot use reserved name 'print' as parameter` (message carries the actual builtin's name, §3.5) |
| assignment to a builtin name, any scope | `cannot assign to reserved name 'print'` (likewise for every builtin) |
| `print:` (any builtin) | `cannot declare function with reserved name 'print'` (likewise for every builtin) |
| `for` loop variable named a builtin | `cannot use reserved name 'print' as loop variable` (likewise for every builtin) |
| top-level/main assignment or top-level `for` target where the name is also a function | **allowed** (0.3 — separate namespaces, §8; reads name the variable, calls the function) |
| a `class` name that is also a function name; a nested declaration colliding with a parameter/local; assignment to a nested function's name | `name 'f' is already declared as a function` |
| call to unknown name | `undefined function 'missing'` |
| wrong argument count — fixed function, called positionally, known callee | `function 'add' expects 2 argument(s), got 1` |
| too many positionals with no `*vararg` (known callee) | `function 'f' expects at most <n> argument(s), got <m>` |
| missing required argument with defaults/varargs (known callee) | `function 'f' expects at least <n> argument(s), got <m>` |
| unknown named argument (known callee, no `**kwargs`) | `unexpected argument name 'foo' in call to 'f'` |
| named argument duplicating a positional binding, or named twice | `argument 'x' got multiple values in call to 'f'` |
| `range()` called with 0 or more than 3 arguments | `function 'range' expects 1 to 3 argument(s), got <n>` |
| `len`/`str`/`int`/`float`/`bool`/`type`/`encode`/`decode` called with ≠ 1 argument | `'len' expects 1 argument(s), got <n>` (message carries the actual builtin's name) |
| `set()` called with 2+ arguments | `'set' expects 0 to 1 argument(s), got <n>` |
| `collect()` called with 1+ arguments | `'collect' expects 0 argument(s), got <n>` |
| `input()` called with 2+ arguments | `'input' expects 0 to 1 argument(s), got <n>` |
| `time()` / `clock()` called with 1+ arguments | `'time' expects 0 argument(s), got <n>` (message carries the actual builtin's name) |
| named argument to one of the original fifteen builtins | `builtin 'print' does not take named arguments` (message carries the actual builtin's name) |
| Python-parity builtin called with too many positionals | `'round' expects at most 2 argument(s), got <n>` (§5.11.1) |
| variadic Python-parity builtin (`map`/`zip`/`max`/`min`) with too few positionals | `'map' expects at least 2 argument(s), got <n>` (§5.11.1) |
| fixed-slot Python-parity builtin missing a required slot | `slice() missing 1 required positional argument: 'start'` (comma-joined `'a', 'b'` above one; §5.11.1) |
| unknown named argument to a Python-parity builtin (or any name to `map`/`zip`) | `'abs' has no named argument 'y'` (message carries the actual builtin's name) |
| named argument binding a slot already filled positionally | `'round' got multiple values for argument 'ndigits'` (message carries the actual builtin's name) |
| `dict()` called with 2+ positionals | `'dict' expects at most 1 positional argument, got <n>` (named arguments excluded from the count) |
| `eval(...)`, `exec(...)` or `compile(...)` called anywhere | `'eval' is not available in cstar: programs are compiled ahead of time and the runtime never compiles source (see SPEC.md §5.11.6)` (message carries the actual name; §5.11.6) |
| call to `print` (any count ≥ 0); call to `range` (1–3 args); call to `set` (0–1); call to `collect` (0 args) | never an error |
| variable read of undefined name | `undefined variable 'zzz'` |
| qualified reference whose qualifier/binding doesn't resolve | `no binding for 'variable.global zzz' found` (message carries the actual `variable.<qual> <name>`, §8) |
| `variable.<fn> <name>` where `<fn>` doesn't share the site's scope | `'missing' is not a function sharing this scope` (message carries the actual qualifier, §8) |
| bare read of a builtin as a value | `undefined variable 'print'` (message carries the actual builtin's name) |
| `class` declaration inside a function body | `classes must be declared at the top level` |
| non-function statement in a class body (besides `pass`) | `only function declarations are allowed in a class body` |
| duplicate class declaration | `duplicate class 'Point'` |
| duplicate method in one class | `duplicate method 'go' in class 'Dupe'` |
| `global`/`nonlocal` name used (bound or read) before its declaration in the same function | `name 'x' is used before 'global' declaration` (resp. `...'nonlocal'...`) |
| `nonlocal` naming a name no enclosing function binds | `no binding for nonlocal 'x' found` |
| assignment to an enclosing function's local without `nonlocal` | `name 'x' is a local of an enclosing function; declare 'nonlocal x' to rebind it` |
| defaulted parameter written before a required one | `parameter with default must come after required parameters` |
| `return` at top level | `'return' outside of a function` |
| `return` inside `main:` | `'return' is not allowed inside 'main:'` |
| `break` outside any loop | `'break' outside of a loop` |
| `continue` outside any loop | `'continue' outside of a loop` |
| duplicate case value (same under total `==`) | `duplicate case value in switch` |
| assigning to a parameter inside its function | **allowed** (updates the parameter) |
| reading a function name as a value | **allowed** (2B — yields a function value; §7.1). *Approved change:* the M1 error `function 'f' cannot be used as a variable` is retired |

Two `case *:` arms never reach the duplicate rule: the first wildcard is
not last, so the parser reports `'case *' must be the last case in a switch`
first (§9.1). The `duplicate case value in switch` rule applies to value
labels only.

### 9.3 Runtime errors

Printed to stderr, then exit 1, in the exact format:

```text
<file>:<line>: runtime error: <message>
```

`<file>` is the source path as invoked (baked into the program at compile
time) and `<line>` is the Pentest source line of the failing expression.
There is no column in runtime errors.

**2C:** every runtime error below is *raised* as an exception **instance**
of the class mapped in §6.10, carrying its message unchanged. Inside a
`try` (§6.10) it is catchable and execution continues; only when an
exception reaches the bottom of the handler stack is the format above
printed and exit code 1 used — byte-identical to M1/2B, and the class
name never appears in it.

| Condition | Message |
|---|---|
| read of a global before its assignment executed | `global variable 'a' used before assignment` |
| read of a local or parameter before its assignment executed | `local variable 'v' used before assignment` |
| numeric division with a zero divisor | `division by zero` |
| `%` with a zero divisor | `modulo by zero` |
| binary operator applied to unsupported types | `unsupported operand types for <op>: <t1> and <t2>` |
| unary `-` applied to a non-number | `unsupported operand type for unary -: <t>` |
| `**` with a non-numeric operand (type check precedes the exponent-sign check) | `unsupported operand types for **: <t1> and <t2>` |
| `int ** int` with a negative exponent | `negative exponent` |
| `&`, `\|`, `^`, `<<`, `>>` with any non-`int` operand | `unsupported operand types for <op>: <t1> and <t2>` |
| `~` applied to a non-`int` | `unsupported operand type for unary ~: <t>` |
| shift count (right operand) outside 0..63 | `shift count must be in range 0..63` |
| `in` where the right operand is not a supported container (`range`, `text`, `bytes`, `list`, `tuple`, `map`, `set`) | `unsupported operand types for(): <t1> and <t2>` (the message keeps the internal `()` spelling) |
| `for` or unpacking over a non-iterable | `'<t>' object is not iterable` |
| `range()` with a non-`int` argument | `range() expects int arguments, got <t>` |
| `range()` with a zero step | `range() arg 3 must not be zero` |
| list/tuple/text/bytes index out of range (incl. negative) | `list index out of range` / `tuple index out of range` / `text index out of range` / `bytes index out of range` |
| non-`int` index on a list/tuple/text/bytes | `<container> indices must be integers, not <t>` (e.g. `list indices must be integers, not text`) |
| indexing a non-subscriptable value (not list/tuple/text/bytes/map) | `'<t>' object is not subscriptable` |
| missing map key (`m[k]`, or `map.pop(k)` without default) | `map key not found` |
| slice step `0` | `slice step cannot be zero` |
| non-`int` slice bound | `slice indices must be integers, not <t>` |
| slice on a map/set/other non-sliceable | `'<t>' object does not support slicing` |
| unhashable map key / set element (`list`, `map`, `set`, `range`, `function`, `class`, `instance`, `method` — instances are **not** usable as keys, §5.12) | `unhashable type: '<t>'` |
| index assignment on `text`/`bytes`/`tuple` | `'text' does not support item assignment` (likewise `'bytes'`/`'tuple'`) |
| index assignment on another non-assignable type (e.g. `set`) | `'<t>' object does not support item assignment` |
| unknown method for the receiver's type | `'<t>' object has no method '<m>'` (e.g. `'int' object has no method 'append'`) |
| wrong argument count for an exact-arity method | `'<t>.<m>() takes N argument(s), got M` — **normative quirk: no closing apostrophe** (`'list.append() takes 1 argument(s), got 0`) |
| argument count outside a 0–2 method's range | `'<t>.<m>() takes between N and M argument(s), got K'` |
| non-`text` argument where a method requires `text` | `'<t>.<m>() expects text, got <u>` (no closing apostrophe) |
| `list.index`/`list.remove` value absent | `value not in list` |
| `set.remove` value absent | `value not in set` |
| `set.pop()` on an empty set | `pop from an empty set` |
| `text.split("")` | `empty separator in split()` |
| `text.replace("", …)` | `empty pattern in replace()` |
| non-`text` element passed to `text.join()` | `'text.join() expects text elements, got <t>'` |
| `encode()` of a non-`text` value | `encode() expects text, got <t>` |
| `decode()` of a non-`bytes` value | `decode() expects bytes, got <t>` |
| `decode()` of invalid UTF-8 | `invalid UTF-8 in decode()` |
| `int()`/`float()` of unparseable text/bytes | `invalid literal for int(): '<s>'` / `invalid literal for float(): '<s>'` |
| `int()` of a non-finite or out-of-range float | `cannot convert float to int` |
| `int()`/`float()` of a wrong-typed argument | `int() expects text, bytes, int, float or bool, got <t>` (resp. `float()`) |
| `len()` of a value with no length (`int`, `float`, `bool`, `None`, `range`, `function`, `class`, `instance`, `method`) | `object of type '<t>' has no length()` |
| destructuring with too few elements | `not enough values to unpack (expected N, got M)` |
| destructuring with too many elements (rest-free pattern) | `too many values to unpack (expected N, got M)` |
| calling a non-function value (`f()` where `f` is not a function) | `object of type '<t>' is not callable` |
| reading `obj.name` where no field/method exists (on any value) | `'<t>' object has no attribute '<n>'` — `<t>` is the instance's **class name** (`'Point' object has no attribute 'missing'`), else the plain type name; class = `AttributeError` |
| assigning `obj.name = v` on a non-instance | `cannot assign attribute '<n>' on '<t>'` |
| calling a class with arguments but no `__init__` method | `class '<C>' takes arguments but has no __init__` |
| named argument passed to a builtin container method (§5.10) | `method arguments must be positional` (user bound methods accept named arguments, §5.12) |
| except clause expression evaluates to a non-class | `except expects a class, got <t>` |
| `raise` of a non-class, non-exception-instance (incl. user classes: `got class`; user instances: `got Point`) | `exceptions must be class or instance, got <t>` |

Type names used in messages: `int`, `float`, `bool`, `text`, `bytes`,
`range`, `list`, `map`, `set`, `tuple`, `function`, `class`, `method`,
`None`, `unset` (the 2B rename `string` → `text` is an approved change;
`type()` spells `None` as `none`, §5.11). An **instance** reports its
class name in `<t>` — `'Point' object has no attribute …`,
`cannot assign attribute 'x' on 'int'` uses the target's type.

### 9.4 Exit codes

Unchanged from M1:

| Code | Meaning |
|---|---|
| `0` | success (including an empty program) |
| `1` | lexer/parser error, semantic error, uncaught runtime exception (runtime error), missing `clang` |
| `2` | usage errors (missing source argument, path not ending in `.pen`/`.pent`, unrecognized option/subcommand) |

## 10. Command-line interface

**The `cstar` command.** It was called `pentest` before v0.3; `pentest`
remains as an alias of the same driver until v0.4.

```text
cstar <file.cst> [args...]              # = run
cstar run [<file.cst> [args...]]        # compile (cached) + exec, pass [args] through
cstar build [<file.cst>] [-o out]       # native executable, default: ./<base>
cstar check [<file.cst>]                # parse + semantic only; silent on success
cstar --help | -h
cstar --version | -v                    # prints: cstar 0.2.1
```

- `cstar <file.cst>` with no subcommand is shorthand for `run`. Bare `cstar`
  with no arguments is a usage error (exit 2).
- **Default entry:** `run`, `build` and `check` without a source path use
  `./index.cst`. If it does not exist, that is a usage error (exit 2,
  `no source file given and ./index.cst does not exist`).
- The source path **must end in `.cst`** (or the legacy `.pen` / `.pent`,
  accepted until v0.4), otherwise it is a usage error (exit 2); `.pentest` is
  rejected as a source path.
- `build` output default: the source basename **without its extension**,
  **written to the current working directory** (e.g. `tools/scan.cst` →
  `./scan`, `scan.pent` → `./scan`); `-o out` overrides it. If the default
  name is an existing directory (e.g. `lib.cst` next to a `lib/` module
  folder), `build` stops with a usage error (exit 2) that asks for `-o`.
  `build` does not run the program.
- `check` runs only the lexer/parser and the semantic pass — no code
  generation, no `clang`. It is silent on success; errors → exit 1.
- **`run` cache:** the cache lives in `$XDG_CACHE_HOME/cstar/`, falling
  back to `~/.cache/cstar/`. The key is the sha256 of (source bytes +
  version string + toolchain identity — the size and mtime of the running
  `cstar` binary, so a rebuilt toolchain automatically re-keys every
  entry and stale executables are never served), computed by invoking
  `sha256sum` through a pipe. If
  `sha256sum` is unavailable, caching is disabled and every `run` compiles.
  On a cache hit the cached binary is executed directly and its exit code
  propagates.
- **Compile pipeline:** the driver writes the generated `gen.c` plus the
  embedded runtime sources `pentest_rt.h`/`pentest_rt.c` (embedded in the
  `cstar` binary via `build/runtime_embed.h`) into a temp dir, then runs
  `clang -std=c11 -O3 -march=native -o <out> gen.c pentest_rt.c -lm -lcurl`. The
  runtime translation unit is identical for every program, so `make`
  precompiles it with those same flags and embeds the object (plus the build
  host's `/proc/cpuinfo` `flags` line) in the binary: a first-ever
  `run`/`build` links that object instead of recompiling the runtime when the
  host's ISA fingerprint matches. On a mismatch the driver compiles the
  runtime once and caches it as `rt-<sha>.o` under the cache directory above
  (keyed by runtime content, compile flags and current CPU fingerprint), then
  links it; this preserves the observable behaviour while making the first
  compile fast. If `clang` is missing → clear error, exit 1.
- **Arguments:** extra args after the file path are passed to the program as
  `argv[1..]`. The generated program hands them to the runtime via
  `pt_init(argc, argv, <source path as invoked>, pt_g_roots, N)` (§11.3);
  no builtin reads them (none of the seventy builtins takes `argv`).
- Linux x86_64 only.

## 11. Implementation and architecture

### 11.1 Overall shape (Approach A)

The toolchain uses **Approach A** from docs/history/design.md §7: the compiler translates
Pentest to C11 and lets `clang` do native code generation and optimization.

- **Compiler driver + frontend:** C++17, built with
  `clang++ -std=c++17 -Wall -Wextra`.
- **Backend:** generates C11, compiled with `clang -std=c11 -O3 -march=native ... -lm`.
- **Runtime:** a small C11 library linked into every generated program.

(Approach B — generating LLVM IR directly — is the documented future path;
see §11.7.)

### 11.2 Pipeline

1. Read the source file (`.pen`/`.pent`, UTF-8).
2. **Lexer** — tokens, INDENT/DEDENT, comments, literals; first error stops.
3. **Parser** — EBNF of §4 → AST (`src/ast.h`); first error stops.
4. **Import linker** (only when the entry file imports something) — parse
   every imported module, resolve specifiers and detect cycles (§14), rename
   imported declarations to collision-free internal names, rewrite
   importer-side references, splice module top-level code in import order
   (dropping imported `main:` blocks), and — in `run`/`build` mode — prune
   imported declarations unreachable from the entry program. The result is
   one flattened AST; programs without imports skip this step entirely
   (§14.4).
5. **Semantic pass** — declaration discovery + name resolution +
   validation
   (§6, §7, §8, §9.2), including parameter/default/variadic checks,
   static call-binding checks, `global`/`nonlocal` placement, class/method
   discovery and validation (§5.12), `try`/`raise` clause checks (§6.10),
   and transitive capture analysis (boxing) for nested functions. Collects
   *all* errors. A final phase resolves every `variable.` qualified site
   and pins the bindings it references (§8). Annotates the AST: mangled C
   names (`FuncDecl.c_name`), per-function local lists, capture lists and
   boxing flags, `Var`/`Assign`
   resolutions (Param/Local/Global), `Program.functions`,
   `Program.globals`, `Program.has_main`, the builtin
   flags for the fifteen legacy builtins (§3.5), and the reserved-name
   set of the Python-parity builtins from `src/builtins.h` (§5.11.3).
6. **Code generation** — `generate_c(program, out_c_path)` writes `gen.c`.
7. **`clang`** — compiles `gen.c` + `pentest_rt.c` to the executable.
8. **Cache/exec** — for `run`, the binary is stored under the content-hash
   key and exec'd (§10). The key covers the entry source **and every
   imported module's source** (§11.6).

### 11.3 Generated-C contract

`generate_c` writes C11 that includes `"pentest_rt.h"` and, compiled together
with `pentest_rt.c`, produces the program. Required structure:

```c
#include "pentest_rt.h"
static pt_value pt_g_<name>;              /* one per Program.globals; zero-init = unset */
static pt_value pt_g_roots[N];            /* 2C: one root pointer per global, &pt_g_<name> */
/* 2B uniform function ABI (M2-2B contract §13): */
static pt_value pt_fn_<name>(pt_value* bound, pt_cell** env);  /* forward decls, ALL functions */
static pt_value pt_fn_<name>(pt_value* bound, pt_cell** env) { ... }  /* definitions, decl order */
static void pt_main_block(void) { ... }                      /* if Program.has_main */
int main(int argc, char** argv) {
    pt_init(argc, argv, "<file path as invoked>", pt_g_roots, N);  /* path + args + GC roots */
    /* top-level flow: statements in source order; at main()'s position: pt_main_block(); */
    return 0;
}
```

Rules():

- Name mangling is done by the semantic pass (`FuncDecl.c_name`); the import
  linker has already renamed imported declarations to internal
  `__ptm<id>_<name>` identifiers (§14.4), so the generated C is oblivious to
  module boundaries.
- Imported modules are **flattened** into the entry AST before code
  generation: module top-level code runs in import order from `main`, and an
  imported module's `main:` block is not emitted.
- **Every function** has the uniform ABI `pt_value f(pt_value* bound,
  pt_cell** env)`: `bound[i]` holds the pre-bound argument for parameter
  `i` or the `PT_MISSING` sentinel; a call site with a known callee
  stack-allocates and fills `bound`, and the callee's entry code
  materializes defaults (in parameter order, **at call time**) and the
  empty vararg/kwarg list/map before the body runs. Top-level,
  env-less functions are called with `env = NULL`; nested functions
  receive their closure's `env` (heap cells, §7.2).
- Function values carry a `pt_funmeta` (display name, parameter names,
  required count, default mask, vararg/kwarg flags, entry pointer) plus
  the `env` vector; dynamic calls bind through `pt_call(fnv, …)` and raise
  the §9.3 binding errors when the callee was not statically known.
- Locals: bound parameters are materialized into `pt_value pt_l_<name>`;
  other locals are
  declared at function entry as `pt_value pt_l_<name> = <unset value>;`;
  captured (boxed) locals live in heap `pt_cell`s.
- `print` calls emit a local array plus `pt_print(arr, n, line)`;
  collections/bytes literals, indexing, slicing, destructuring, comma-join
  and the §5.11 builtins lower to their `pt_*` runtime operations; method
  calls lower to `pt_call_method(obj, name, args, nargs, argnames, nnamed,
  line)` (the positional-only form passes no names, §5.12); attribute reads
  and writes lower to `pt_getattr` / `pt_setattr`; `is`/`is not` lower to
  `pt_bool(±pt_identical(a, b, line))`.
- A `class` statement lowers to `pt_class_new(name, line)` bound to the
  global, then one `pt_class_add_method(cls, name, fn, line)` per method
  (methods are ordinary functions in the global function table, reached
  only through the class).
- Control-flow constructs lower to C control flow: conditions are tested
  with a total runtime truthiness check, `break`/`continue` map 1:1, a
  `for` over a direct `range(...)` call is open-coded without signed-overflow
  UB (any other iterable uses the runtime iteration protocol), `and`/`or`/
  the conditional expression keep their lazy right side, `switch` lowers
  to a first-match chain (M2-2A contract §11), and `try`/`except`/
  `finally` lowers to a C scope holding a `pt_handler` (a `setjmp` landing
  pad) — nested try-blocks compose the CATCH/FINALLY pairs (§6.10).
- Generated temporaries are cleared at statement boundaries so the
  collector can reclaim dropped values (§7.3).
- **Every operation that can fail at runtime must receive the Pentest source
  line number** of the corresponding AST node (that is how §9.3's `<line>`
  is produced).
- Reads of Global/Local nodes must be guarded by the used-before-assignment
  check with the proper name and scope kind.
- All function forward declarations appear before any definition so forward
  calls and mutual recursion work.

### 11.4 Runtime library responsibilities

`runtime/pentest_rt.{h,c}` owns the `pt_value` layout and the exact `pt_*`
API (implementation freedom inside that contract). It must provide:

- constructors for `None`/`int`/`float`/`bool`/text-literal/bytes-literal/
  `range`
  values, and heap constructors for `list`/`map`/`set`/`tuple`/`function`
  values and closure cells/environments (§5.8, §7.2), plus class,
  instance, bound-method and exception-class values (§5.12, §6.10);
- the arithmetic, bitwise, power, comparison, membership and truthiness
  operations of §5–§6, with line reporting, and the identity test
  `pt_identical` (§5.13);
- indexing (negative + range checks), index assignment, Python-normalized
  slicing, the iteration protocol over every iterable, total equality,
  hashing + insertion-ordered map/set storage, and the display/`str()`
  forms of §5.6;
- the §5.10 method dispatch `pt_call_method(...)` (positional **and**
  named arguments), attribute get/set `pt_getattr`/`pt_setattr`, class
  construction/dispatch (`pt_class_new`/`pt_class_add_method`/
  `pt_class_call`), and the §5.11 builtins;
- the dynamic-call binder `pt_call(...)` (positional/named/vararg/kwarg
  binding with the §9.3 messages) behind indirect calls, class
  construction and bound-method invocation;
- `pt_print` implementing §5.6 exactly (including the shortest-round-trip
  float rule and the `range(...)` format);
- the used-before-assignment guard (NameError messages of §9.3);
- the runtime iteration protocol behind `for` and unpacking (§6.6, §4.4);
- `pt_init(argc, argv, src_path, roots, nroots)` — stores the path + args
  and registers the program-global GC root vector (§7.3), then creates the
  twelve builtin exception-class singletons;
- the exception machinery of §6.10: the handler stack
  (`pt_try_enter`/`pt_throw`/rethrow), bound-vector cleanup on unwind, the
  `pt_runtime_error_as(cls, line, ...)` raiser (all 9.3 sites are mapped
  to a class), and the uncaught printer that emits
  `<file>:<line>: runtime error: <message>` and exits 1;
- the mark & sweep collector of §7.3: block registry, root marking,
  safe-point triggering, the protect stack, interior-pointer tracking for
  text/bytes buffers, and `collect()`.

The runtime requires **only libc, libm and libcurl** (libcurl backs the HTTP
builtins, §5.11.8). `text`/`bytes` are immutable and
literals may be static; every heap object — concatenated text, collections,
function values, closure cells, classes, instances — flows through the
registry and is reclaimed by the collector once unreachable (§7.3).

### 11.5 Source layout (ownership)

| Path | Role |
|---|---|
| `src/ast.h` | shared AST contract (frontend ↔ backend) |
| `src/diagnostics.h`, `src/diagnostics.cpp` | error reporting with caret display |
| `src/frontend.h` | driver ↔ frontend interface |
| `src/codegen.h` | driver ↔ C backend interface |
| `src/lexer.{h,cpp}`, `src/parser.{h,cpp}`, `src/semantic.{h,cpp}` | frontend |
| `src/modules.{h,cpp}` | import resolution, flattening, mangling, selective-loading prune (§14) |
| `src/codegen.cpp` | C code generator (backend) |
| `runtime/pentest_rt.{h,c}` | C11 runtime library (backend) |
| `src/main.cpp` | CLI driver |
| `Makefile`, `tools/embed.cpp` | build; precompiles the runtime and embeds it with the sources into `build/runtime_embed.h` (pure C++, no other tooling) |
| `tests/**` | test harness + cases |
| `examples/**` | local example programs (git-ignored, not in the repository) |
| `editors/vscode/**` | VSCode extension: grammar, autocomplete, snippets, diagnostics |
| `SPEC.md`, `README.md`, `CHANGELOG.md`, `docs/**` | documentation (index: [docs/README.md](docs/README.md)) |
| `build/pentest` | the CLI binary |

### 11.6 Cache

`run` caches compiled executables as `<key>.bin` under
`$XDG_CACHE_HOME/cstar/` (fallback `~/.cache/cstar/`), keyed by sha256(entry
source bytes + every imported module's source bytes + version string +
toolchain identity: the size/mtime of the running `cstar` binary — a `make`
or toolchain update re-keys the cache instead of serving stale executables)
computed
via `sha256sum` through a pipe. Editing an imported module therefore
invalidates the cache exactly like editing the entry file (§14.6).
No `sha256sum` → no caching. Hit → exec the
cached binary directly; exit code propagates (§10).

### 11.7 Approach B (future path)

docs/history/design.md §7 also describes **Approach B**: frontend → LLVM IR → LLVM
optimization/codegen → link runtime. Approach B gives direct control over
code generation but requires a heavier frontend/backend integration. The
toolchain does not use it; it is the documented path if Pentest later needs
more control over code generation (§13).

### 11.8 Performance mechanisms (0.2.1)

There is no separate optimizer pass: every mechanism below is a lowering
decision made where code is emitted, so behavior is fixed by construction
and the byte-compat contract holds. The dedicated chapter —
[docs/performance/optimization.md](docs/performance/optimization.md) — explains each one, why it
cannot change results, and how it is validated. In brief:

- **Native-int specialization** — proven-int slots become `int64_t`
  pairs with straight-C loops and definite-assignment-proven reads that
  skip the guard flag; `for` targets over open-coded `range()` join the
  lane too (the bind writes the induction variable directly — 14.4× on
  a 20M-iteration `abs`/`%` loop), with exact edge parity (wrap, div/mod
  signs, `MIN/-1`) part of the contract.
- **Dynamic numeric fast paths** — static-inline `pt_add_f` … `pt_ne_f`
  operators in the runtime header, identical tag checks with out-of-line
  fallback (~35% faster dynamic loops).
- **GC safe-point elision** — allocation-free loop bodies emit no
  back-edge collection check (`alloc_free_expr` / `body_alloc_free`).
- **Artifact caches** — the `run` binary cache (§11.6) plus an `rt-<sha>.o`
  runtime object cache, so repeat builds skip recompiling the runtime.
- **Pin poisoning** — bindings pinned by cross-scope access exit the
  native lane so both access paths share one dynamic storage (§8).
- **Zero-cost selective imports** — a program without imports returns from
  the linker before any traversal, and imported declarations that are
  unreachable from the entry program are pruned before semantic analysis
  and code generation, so their bodies cost nothing at compile time or
  run time (§14.4).

## 12. Known limitations (as of `0.2.1`)

- **The collector is non-moving, mark & sweep, with no finalizers or weak
  references.** Reclamation timing is observable only through `collect()`
  (§7.3); unreachable cycles are collected, but there is no way to be
  notified of an object's death, and no memory introspection beyond
  `collect()`'s count.
- **No cycle detection in display.** A self-referential collection (e.g. a
  list containing itself) is legal to build but displaying it is undefined
  (§5.6) — this can recurse until the C stack overflows.
- **Deep recursion may overflow the native C stack** — there is no
  guaranteed catch; a controlled runtime failure is acceptable (§7.1).
- **Mutating a collection during its own iteration is undefined** (no
  error is guaranteed, §5.8).
- **Linux x86_64 only.** No other host/target is supported or tested.
- **File I/O and HTTP only.** File I/O exists only through `open()` and
  the file methods (§5.11.3); network I/O is limited to the HTTP client
  builtins (§5.11.8); there is no `argv` builtin,
  and `print` is the only output outside files. Programs may span multiple
  files (§14; unreleased in this tree), but the compiled result is still one
  standalone executable.
- **Class system (2C scope, §13):** no inheritance or MRO, no user
  exception hierarchies (user classes cannot subclass `Exception` or be
  raised, §6.10), no class attributes or deletion syntax, and no
  generators. Exception matching is exact identity only (§6.10);
  exception chaining is not implemented — a raise during unwinding
  replaces the in-flight exception.
- **Known display quirks (documented, not bugs):** an empty set and an
  empty map both display as `{}` (use `type()` / literal form to tell them
  apart); `text.upper()`/`lower()` map **ASCII letters only**; the
  fixed-arity method error omits its closing apostrophe
  (§5.10 — normative, matches the contract).
- Also worth remembering: `//` is a comment, not floor division; `"""` is a
  comment, not a string; int arithmetic wraps (including `**`); `%` and the
  bitwise operators are ints-only; logical operators return `bool`;
  unparenthesized commas at full-expression positions join into `text`
  instead of building a tuple (§4.4).

## 13. Future work and roadmap

Milestone 2 proceeds in stages. **Stages 2A, 2B and 2C (this
specification) are released**, and the post-2C `0.2.1` drop ships the
performance push (§11.8), cross-scope variable access (§4.1, §8), the
`input()`/`time()`/`clock()` builtins, and the Python-parity builtin
set — the 43 implemented entries, nine stubs and the rejected
`eval`/`exec`/`compile` trio of §5.11.3–§5.11.6. The **unreleased** Stage 2D
work in this tree adds modules/imports with selective loading (§14) and the
v0.3 surface-syntax overhaul; generators are still to come:

| Stage | Scope |
|---|---|
| **2A — done** | Control flow (`if`/`elif`/`else`, `while`, `for`, `switch`/`case`), expressions (`**`, bitwise set, membership `in`, ternary, `and`/`or`/`not`, eleven compound assignments), `range()`, baseline performance. |
| **2B — done** | Collections (`list`/`map`/`set`/`tuple`) with indexing, slicing, index assignment, iteration and membership; destructuring and comma-join; `text`/`bytes` with code-point semantics and `encode`/`decode`; Python-style builtin methods; the nine builtins beyond `print`/`range`; default/named/`*args`/`**kwargs` parameters; function values; nested functions and closures; `global`/`nonlocal`. |
| **2C — done** | Classes with methods and dynamic attributes, attribute read/write, bound methods, object identity `is`/`is not`; exceptions (`try`/`except`/`finally`/`raise`/`as`, nine builtin exception classes, runtime errors as catchable instances); non-moving mark & sweep garbage collection with `collect()`. |
| **2D — partly** | Modules/imports, multi-file programs and selective-loading reachability analysis are implemented (§14; unreleased in the current tree). Generators remain. |
| **2E** | Tooling and performance: diagnostics polish, editor support, benchmarks, and an LLVM backend (Approach B, §11.7) if direct control over code generation becomes necessary. |

Still open beyond those stages:

- **Class system depth:** inheritance/MRO, user exception hierarchies
  (subclassing `Exception`), class attributes, weak references and
  finalizers — all deliberately out of 2C scope (§12).
- **Types:** a static typing layer with inference — docs/history/design.md §10 Q1 is
  still open beyond the dynamic decision.
- **Standard library modules** from docs/history/design.md §8: `net`, `web`, `bytes`,
  `parallel`, `proc`, `parse`, plus file/command-line/environment utilities.
- **Multiline strings** (`'''`, or triple quotes dual-purposed) if the
  comment-only rule proves limiting.
- **Deferred Python builtins:** `bytearray` (a mutable `bytes` needs a
  real buffer type first), `complex` (no operator surface for it yet)
  and `frozenset` (needs immutable-set semantics distinct from `set`).
  Everything else in the Python builtin list ships in `0.2.1` (§5.11).
- **Portability** beyond Linux x86_64.

---

## 14. Modules and imports (Stage 2D)

> **Status:** implemented and normative in the unreleased tree
> (`pentest --version` still prints `0.2.1`). This section was appended after
> §13, so existing section numbers are unchanged.

Imports are resolved, expanded and renamed **before semantic analysis and
code generation**. The result is one flat program: no module objects exist at
run time, imported calls are ordinary static calls, and a program without
imports pays no cost for the feature.

### 14.1 Syntax

The grammar is in §4.1 (`import_decl`):

```text
import module_name [ "as" IDENT ]
from module_name "import" import_name { "," import_name }
module_name := IDENT { "." IDENT }
import_name := IDENT [ "as" IDENT ]
```

- `import m` binds the **last dotted component** of `m` — `import a.b` binds
  `b` — or the `as` alias when present.
- `from m import a, b as c` binds each named declaration (or its alias).
- Imports are **top level only**: inside a function or `main:` body the parser
  reports `'import' is only allowed at the top level`.
- Malformed forms report `expected module name`,
  `expected 'import' after module name`, `expected a name to import` or
  `expected alias after 'as'`.
- `import` and `from` are keywords (§3.5); `as` was already a keyword.

### 14.2 Resolution (normative)

For a specifier `a.b`, the relative path is `a/b`. The linker searches these
roots in order, taking the first existing candidate:

1. the **directory of the importing file** (its canonical parent directory);
2. the **current working directory** of the `cstar` process;
3. each colon-separated entry of **`$CSTAR_PATH`** (empty entries are
   skipped);
4. each colon-separated entry of the legacy **`$PENTEST_PATH`** (accepted
   until v0.4; empty entries are skipped).

At each root it tries, in order (`.cst` first; the legacy spellings until
v0.4):

```text
<root>/<rel>.cst
<root>/<rel>.pen
<root>/<rel>.pent
<root>/<rel>/__init__.cst
<root>/<rel>/__init__.pen
<root>/<rel>/__init__.pent
```

Paths are canonicalised with `realpath`; a module reached through different
spellings is a single module. Nothing is read at run time: resolution and
reading happen at compile time, and a module that cannot be found is a
compile error (`cannot find module '<spec>'`), never a runtime failure.

**Cycles.** A module that (transitively) imports a module currently being
loaded is a compile error at the offending import:
`circular import: '<spec>' is already being loaded` (or
`circular import involving module '<spec>'` when a previously failed load is
re-entered).

### 14.3 Names and binding (normative)

- Every top-level binding of a module — functions, classes, and globals
  assigned at module top level or in its `main:` (which is not executed, see
  §14.4, but still declares names) — is renamed to a module-unique internal
  name `__ptm<id>_<name>`. References inside the module are rewritten
  consistently, so modules never collide with each other or with the entry
  file. This is invisible in generated diagnostics, which keep original
  names.
- `module.name` in the importer is rewritten to the internal name of a
  top-level declaration of that module. The module specifier is not a
  namespace object: after `import a.b` the usable name is `b`, and
  `a.b.name` is an `undefined variable 'a'` error.
- Only declarations **made in that file** are importable. A name a module
  itself imported is **not** re-exported: `from mid import x` succeeds only
  when `mid` declares `x` itself.
- Names beginning with `_` are module-private: they cannot be imported
  (`cannot import private name '_x' from module 'm'`) and cannot be read or
  written as attributes (`'_x' is private in module '<path>'`).
- A missing declaration reports `module '<spec>' has no declaration 'x'`
  (`from` form; the qualified form names the module path instead).
- The importer's bindings may not collide with its own declarations:
  `import alias 'x' conflicts with a declaration in this module`,
  `imported name 'x' conflicts with a declaration in this module`,
  `import alias 'x' conflicts with an imported name`,
  `imported name 'x' conflicts with a module alias`,
  `imported name 'x' is already imported from another module` (the same name
  from a *different* module; the same module may be imported repeatedly and
  under several aliases), and `two modules are imported with the alias 'x'`.
- A module alias is **not a value**: using it bare reports
  `module alias 'x' cannot be used as a value; write x.name`.
- `from`-imported names are **read-only**:
  `cannot assign to imported name 'x'` and
  `cannot declare imported name 'x' global`. A module global *is* writable
  through its qualified name (`m.x = v` updates the module global that the
  module's own functions read); assigning through an alias that is not a
  module is `cannot assign to module alias 'x'`.
- A local or parameter of the same name shadows an import inside a function,
  exactly like any other outer binding (§8).

### 14.4 Execution and initialization (normative)

Imports are statements at their textual position in the importing file:

1. The first time a module is loaded, its top-level statements execute
   **once**, in source order, at that import statement's position — before
   the statements that follow the import in the importer.
2. Further imports of the same module (from any file, under any alias) run
   nothing and bind the same declarations.
3. Imported declarations are registered before execution (like top-level
   declarations of the entry file), so forward references inside and across
   modules work. A module may import other modules; the graph is flattened
   depth-first in import order.
4. An imported module's `main:` block is **ignored**; only the entry file's
   `main:` runs. Module initialization is its top-level statement code.
5. Module top-level code is retained for the compiled program even when the
   program uses none of the module's declarations.

### 14.5 Selective loading (normative)

The compiler must not analyse, emit or compile imported declarations that
cannot run:

- In `run` and `build` mode, after flattening, the linker performs one
  reachability pass over the top-level declarations. Entry-file items are
  roots. An imported item is kept when it is reachable from a root or from
  another kept imported item (references inside function bodies, defaults,
  class methods and nested functions are followed transitively).
- An imported item is *eligible* for removal only when it declares bindings
  that have no effect at load time: a function declaration, a class
  declaration, or a plain `x = <pure expression>` global assignment, where
  "pure" means built from literals, names, unary/binary/logical/conditional
  operators and collection literals. Any other top-level statement — calls,
  method calls, attribute/index reads, loops, `if`, `try`, imports — is
  module initialization and is always kept.
- Removing items must not change observable behaviour: kept declarations have
  identical semantics, and all module initialization (including the impure
  statements of a module whose declarations are all unused) still runs.
- In `check` mode the pass is **strict**: no imported declaration is
  removed, so every declaration of every loaded module is validated and may
  produce diagnostics. A program whose unused imported code is invalid
  therefore passes `run`/`build` but fails `check`; this is intentional
  (documented in [docs/guide/modules.md](docs/guide/modules.md)).
- The pass runs before semantic analysis and code generation; it adds **no
  runtime instructions**. Programs without imports skip the linker entirely
  after a linear scan of the top-level items.

### 14.6 Errors (normative)

| Trigger | Message (prefix `error: `, with location) |
|---|---|
| no candidate file | `cannot find module '<spec>'` |
| cycle | `circular import: '<spec>' is already being loaded` / `circular import involving module '<spec>'` |
| unreadable module file | `cannot read module '<spec>' (<path>): <strerror>` |
| `from`-importing a private name | `cannot import private name '_x' from module 'm'` |
| qualifying a private name | `'_x' is private in module '<path>'` |
| unknown declaration (`from`) | `module 'm' has no declaration 'x'` |
| unknown declaration (qualified) | `module '<path>' has no declaration 'x'` |
| alias collides with a declaration | `import alias 'x' conflicts with a declaration in this module` |
| imported name collides with a declaration | `imported name 'x' conflicts with a declaration in this module` |
| alias collides with an import | `import alias 'x' conflicts with an imported name` |
| import collides with an alias | `imported name 'x' conflicts with a module alias` |
| same name from two modules | `imported name 'x' is already imported from another module` |
| same alias for two modules | `two modules are imported with the alias 'x'` |
| alias used as a value | `module alias 'x' cannot be used as a value; write x.name` |
| assigning an alias | `cannot assign to module alias 'x'` |
| assigning an imported name | `cannot assign to imported name 'x'` |
| `global` on an imported name | `cannot declare imported name 'x' global` |
| import inside a function or `main:` | `'import' is only allowed at the top level` |
| malformed import | `expected module name` / `expected 'import' after module name` / `expected a name to import` / `expected alias after 'as'` |

Diagnostics carry the offending file's path and the exact source location
(§9.1); a diagnostic inside an imported module names that module's canonical
path, not the entry file that imported it.

### 14.7 Run cache (normative)

When a program has imports, the `run` cache key material includes, in load
order, the canonical path and full contents of every loaded module (§11.6).
Editing an imported file therefore re-keys the cached program; the module
files themselves are never needed to run the compiled executable.

---

## Appendix A. Examples

The first four are the canonical examples from docs/history/design.md §11, verbatim.

### A.1 Ordinary script, no `main:` block (DESIGN §11.A)

```python
a = 10
print(a)

hello():
    print("hello world")
```

Expected output:

```text
10
```

### A.2 Top-level statements around `main:` (DESIGN §11.B)

```python
print("first line")

main:
    print("second line")

// outside main
print("third line")
```

Expected output:

```text
first line
second line
third line
```

### A.3 `main:` calls a function declared later (DESIGN §11.C)

```python
main:
    dummy()

dummy():
    print("dummy ran")
```

Expected output:

```text
dummy ran
```

### A.4 Only called function bodies execute (DESIGN §11.D)

```python
a():
    return None

b():
    return None

c():
    a()

main:
    c()
```

The bodies of `main`, `c`, and `a` execute. The body of `b` does not.

Expected output: *(none — the program prints nothing and exits 0.)*

### A.5 Operators and `print` formatting

```python
print(7 / 2, -7 / 2, 7 % 2, -7 % 2)
print(1 == 1.0, 1 == "1", True == 1)
print(5.0, 1.0 / 3.0)
```

Expected output:

```text
3 -3 1 -1
True False False
5.0 0.3333333333333333
```

### A.6 The `//` footgun

```python
x = 7 // 2
print(x)
print(7 / 2)
```

Expected output:

```text
7
3
```

`//` starts a comment that runs to the end of the line — it is never
division, so `x = 7 // 2` is just `x = 7`. Note that a trailing comment can
swallow a closing paren: `print(7 // 2)` on one line is an
`unclosed '('` syntax error. Division is a single `/`.

### A.7 Forward reference plus arithmetic and `return`

```python
main:
    print(add(2, 3))
    print(scale(add(2, 3)))

add(a, b):
    return a + b

scale(x):
    return x * 10
```

Expected output:

```text
5
50
```

Both `add` and `scale` are declared after their call sites; declaration
discovery (§7, rule 1) makes that legal.

### A.8 Control flow: `if`/`elif`/`else`, `for` over `range()`, `switch`

```python
classify(n):
    if n < 0:
        return "negative"
    elif n == 0:
        return "zero"
    else:
        return "positive"

main:
    for i in range(4):
        print(i, classify(i - 2))
    switch 3:
        case 1:
            print("one")
        case 3:
            print("three")
        case *:
            print("other")
```

Expected output:

```text
0 negative
1 negative
2 zero
3 positive
three
```

The `for` loop binds `i` to `0..3` (stop exclusive, §6.9); the `switch`
matches `case 3:` under total `==` and ends without falling through, so
`case 1:` and `case *:` never run (§6.7).

### A.9 Stage 2B: collections, methods, destructuring, comma-join

```python
main:
    xs = [3, 1, 2]
    xs.append(4)
    print(xs, len(xs))
    first, *rest = xs
    print(first, rest)
    ports = {"http": 80, "https": 443}
    ports["ssh"] = 22
    for k in ports:
        print(k, ports[k])
    msg = "hello", "world"
    print(msg)
```

Expected output:

```text
[3, 1, 2, 4] 4
3 [1, 2, 4]
http 80
https 443
ssh 22
hello world
```

`append` mutates the list in place (§5.10); `first, *rest = xs` destructures
with a trailing rest leaf binding a fresh `list` (§4.4); iterating a `map`
yields **keys** in insertion order (§6.6); and the unparenthesized
`"hello", "world"` on the assignment RHS is a **comma-join**, not a tuple
(§4.4).

### A.10 Stage 2B: fresh defaults, named arguments, function values, closures

```python
greet(name, greeting = "hello"):
    return greeting + ", " + name

add_item(item, bucket = []):
    bucket.append(item)
    return bucket

make_counter(step):
    n = 0
    tick(by = 1):
        nonlocal n
        n = n + by
        return n * step
    return tick

main:
    print(greet("ana"))
    print(greet("bob", greeting = "hi"))
    print(greet)
    fns = [greet]
    print(fns[0]("zoe"))
    print(add_item("a"))
    print(add_item("b"))
    c = make_counter(10)
    print(c())
    print(c(5))
```

Expected output:

```text
hello, ana
hi, bob
<function greet>
hello, zoe
[a]
[b]
10
60
```

Defaults are evaluated **fresh at each call** (§7.1): the two `add_item`
calls build independent lists, so the output is `[a]` then `[b]` — never
`[a, b]`. A function name read as a value yields a function value that can
be stored in a collection and called indirectly (§7.1). `tick` is a nested
function that captures `step` and, through `nonlocal n`, rebinds the
enclosing `n` across calls (§7.2, §8); its `by = 1` default is also
call-time.

**Stage 2C example — classes, named arguments, exceptions, `is`,
`collect()`:**

```python
class Point:
    __init__(self, x, y):
        self.x = x
        self.y = y

    move(self, dx, dy):
        self.x = self.x + dx
        self.y = self.y + dy
        return self

main:
    p = Point(2, 3)
    p.move(dx=1, dy=1)
    print(p, p.x, p.y)
    try:
        p.nope
    except AttributeError as e:
        print(e, type(e))
    try:
        print(10 / 0)
    except ZeroDivisionError as e:
        print(e)
    print(p is p, p is Point(2, 3))
    n = collect()
    print(n >= 0)
```

Expected output:

```text
<Point instance> 3 4
AttributeError: 'Point' object has no attribute 'nope' AttributeError
ZeroDivisionError: division by zero
True False
True
```

`p.move(dx=1, dy=1)` binds named arguments through the ordinary binder
(§5.12); `p.nope` is a bare attribute read (§5.12) raising `AttributeError`,
caught with its `as` binding (§6.10); `p is p` is identity while the fresh
`Point(2, 3)` is not (§5.13); and `collect()` reports a freed-block count
(§7.3).
