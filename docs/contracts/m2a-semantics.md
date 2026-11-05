# cstar M2 Stage 2A — Normative Semantics and Decisions

> Part of the [stage contracts](README.md) (normative; they govern over
> [`SPEC.md`](../../SPEC.md) if the two differ).

Authoritative contract for Milestone 2 Stage 2A (control flow, expressions,
baseline performance). All components must conform. Error wording here is
normative: tests match on these substrings. M1 behavior
(`docs/contracts/m1-semantics.md`) remains normative wherever this document does not
override it; the full M1 test suite must stay green.

## 0. Stage 2A decisions (user-approved)

| Question | Decision |
|---|---|
| Operator scope | `**` (exponent) + full bitwise set `& \| ^ ~ << >>`. No `is` (object identity is 2C). **No floor division** — `//` is a locked M1 comment; impossible as an operator. |
| switch/case | Strict set: constant-literal labels only; duplicate case = compile error; `case *:` optional, must be last; no fall-through; `break` is **loop-only** (error outside a loop; inside a loop it breaks the loop even if lexically inside a switch). |
| Single-line forms | **Allowed** for `if` / `elif` / `else` / `while` (`if x > 0: print(x)`). `for` and `switch` require indented blocks. A single-line body is exactly one `simple_stmt`. |
| Membership `->` | A normal binary expression (comparison precedence), usable anywhere an expression is allowed, equivalent to Python's `in`: `needle -> haystack`. In a `for` header it is the binding form. 2A supports `range()` and `string` targets; lists/maps/sets arrive with 2B through the same runtime entry point. |
| Milestone pace | 2A lands fully, then work continues autonomously into 2B. |
| pass | Included: placeholder statement, valid anywhere a statement is. Blocks still require >= 1 statement (`pass` for empty). |
| `range` | New reserved builtin name, treated exactly like `print` (call-only; reserved-name rules apply). |

## 1. Lexical additions

New keywords (reserved, cannot be identifiers):
`if elif else for while break continue switch case pass and or not`.

New reserved builtin: `range` — an IDENT (so `range(3)` parses as a call)
governed by the same reserved-name machinery as `print`; every check that
applies to `print` applies to `range` with the name substituted.

New tokens (maximal munch, checked in this order at each first char):

| Lexeme | Token | Notes |
|---|---|---|
| `->` | Arrow | `-` followed by `>`; `a - > b` is two tokens (syntax error) |
| `**` | StarStar | `*` followed by `*` |
| `**=` | StarStarAssign | 3-char munch, checked before `**` |
| `<<` `>>` | LtLt GtGt | |
| `<<=` `>>=` | LtLtAssign GtGtAssign | 3-char munch, checked before `<<`/`>>` |
| `&` `\|` `^` `~` | Amp Pipe Caret Tilde | |
| `&=` `\|=` `^=` | AmpAssign PipeAssign CaretAssign | |
| `+=` `-=` `*=` `/=` `%=` | PlusAssign etc. | `-` followed by `=`; `=` alone remains Assign |

Integration amendment (ratified): because the exponent/bitwise base operators now exist, the spec's invitation ("more forms can be considered if their base operators exist") is accepted — **all eleven compound forms** `+= -= *= /= %= **= &= |= ^= <<= >>=` are supported; each behaves as `target = target <base-op> expr` with the base operator's exact runtime semantics and error messages. No other compound forms exist.
| keywords above | Kw* | |

No other lexical changes. `//` remains a comment. Literals, strings,
indentation and comment rules are unchanged from M1 §2.

## 2. Grammar (EBNF, normative additions)

```ebnf
program       := { NL | top_stmt | func_decl | main_decl }
func_decl     := "function" IDENT "(" [ params ] ")" ":" NL INDENT block DEDENT
main_decl     := "main" "(" ")" ":" NL INDENT block DEDENT
params        := IDENT { "," IDENT }
block         := { NL } stmt { NL | stmt }              (* >= 1 statement *)
top_stmt      := stmt

stmt          := compound_stmt | simple_stmt NL

compound_stmt :=
      "if" expr ":" suite { "elif" expr ":" suite } [ "else" ":" suite ]
    | "while" expr ":" suite
    | "for" IDENT "->" expr ":" NL INDENT block DEDENT  (* block form only *)
    | "switch" expr ":" NL INDENT { case_clause } DEDENT
suite         := NL INDENT block DEDENT | simple_stmt   (* single-line body *)

case_clause   := "case" case_label ":" NL INDENT block DEDENT
               | "case" "*" ":" NL INDENT block DEDENT
case_label    := INT | "-" INT | FLOAT | "-" FLOAT
               | STRING | "True" | "False" | "None"

simple_stmt   := IDENT ("=" | augop) expr                (* augop: see §1 table *)
               | "return" [ expr ]
               | "break" | "continue" | "pass"
               | expr
```

Expression grammar (low to high precedence):

```ebnf
expr        := ternary
ternary     := or_expr [ "if" or_expr "else" ternary ]   (* else-branch nests *)
or_expr     := and_expr { "or" and_expr }                (* left-assoc *)
and_expr    := not_expr { "and" not_expr }               (* left-assoc *)
not_expr    := "not" not_expr | cmp_expr
cmp_expr    := bitor_expr { cmp_op bitor_expr }          (* left-assoc *)
cmp_op      := "==" | "!=" | "<" | "<=" | ">" | ">=" | "->"
bitor_expr  := bitxor_expr { "|" bitxor_expr }           (* left-assoc *)
bitxor_expr := bitand_expr { "^" bitand_expr }           (* left-assoc *)
bitand_expr := shift_expr { "&" shift_expr }             (* left-assoc *)
shift_expr  := add_expr { ("<<" | ">>") add_expr }       (* left-assoc *)
add_expr    := mul_expr { ("+" | "-") mul_expr }         (* left-assoc *)
mul_expr    := power { ("*" | "/" | "%") power }         (* left-assoc *)
unary       := ("-" | "~") unary | power
power       := primary [ "**" unary ]                    (* right-assoc *)
primary     := INT | FLOAT | STRING | "True" | "False" | "None"
             | IDENT "(" [ expr { "," expr } ] ")"       (* call *)
             | IDENT "(" ")"                             (* zero-arg call *)
             | IDENT                                     (* variable read *)
             | "(" expr ")"
```

Consequences (normative): `-2 ** 2` is `-(2 ** 2)` == -4; `2 ** -1` and
`2 ** 3 ** 2` == 2**9 parse as expected; `x = a if b else c` is the ternary
assignment (statement-level `if` is the statement form — no ambiguity, the
statement dispatcher looks at the first token only); comparisons and `->` do
not chain specially (left-associative like M1 comparisons).

Parser rules and their normative messages (first syntax error wins, caret):

Ratified integrator clarifications (agents' contract questions, now normative):
- `switch x:` followed by NEWLINE with no INDENT block → `expected an indented block after ':'`; an INDENT block that does not start with `case` → `expected at least one case in 'switch'`.
- Expression structure is `mul_expr := unary {("*"|"/"|"%") unary}`, `unary := ("-"|"~") unary | power`, `power := primary ["**" unary]` (the §2 EBNF's `mul_expr := power` was a typo; this yields the normative `-2 ** 2 == -4` and `2 ** -1`).

| Condition | Message |
|---|---|
| `for` target not followed by `->` (incl. `for i:` , `for a, b -> x:`) | `expected '->' after loop variable` |
| single-line suite whose statement is compound or absent | `expected a simple statement on the same line after ':'` |
| colon followed by NEWLINE with no INDENT block (all forms) | M1: `expected an indented block after ':'` |
| `switch` with zero case clauses | `expected at least one case in 'switch'` |
| non-constant case label (IDENT, expression, `case *` not last) | `case label must be a constant literal` |
| `case *:` followed by another case | `'case *' must be the last case in a switch` |
| statement starting with `case` outside a switch | `'case' outside of 'switch'` |
| `elif` with no matching `if` | `'elif' without a matching 'if'` |
| `else` with no matching `if` | `'else' without a matching 'if'` |
| nested `function` | M1: `nested function declarations are not allowed` |

An `if` statement consumes its whole `elif`/`else` chain; a following orphan
`elif`/`else` at statement level triggers the messages above. `pass` parses as
an ordinary statement. Assignment remains a statement, never an expression.

## 3. Operator semantics

All M1 operator rules are preserved (wrapping two's-complement int
arithmetic, toward-zero int division, total `==`/`!=`, string `+`,
lexicographic comparisons, type-error-before-zero-divisor ordering).

### 3.1 `**` (power)

- Any float operand (`float ** int/int ** float/float ** float`) → double via
  C `pow()` (float result).
- `int ** int` with non-negative exponent → int, wrapping (defined via
  uint64 modular exponentiation; result congruent mod 2^64).
- `int ** negative int` → runtime error `negative exponent`.
- Non-numeric operand → `unsupported operand types for **: <t1> and <t2>`
  (type error precedes the exponent sign check).

### 3.2 Bitwise `& | ^ << >> ~`

- **ints only.** bool/float/None/string operand → runtime error
  `unsupported operand types for <op>: <t1> and <t2>` (`~`:
  `unsupported operand type for unary ~: <t>`). No bool→int promotion
  (M1: `1 == True` is False; bools are not ints).
- `& | ^` are ordinary wrapping int64 bit ops; `~a` == `-a - 1`.
- `>>` is arithmetic (sign-propagating) shift.
- Shift counts: negative → runtime error
  `shift count must be in range 0..63`; >= 64 → same error. (No silent
  masking.)

### 3.3 Compound assignment

`target op= expr` where target is a bare IDENT in 2A (index/attribute targets
arrive in 2B with a single-evaluation rule). Supported forms (integration
amendment, §1): `+= -= *= /= %= **= &= |= ^= <<= >>=`. Semantics identical to
`target = target op expr` for the corresponding base operator (§3.1/§3.2 for
the exponent/bitwise forms); the operator's error messages are unchanged. The
target is read, the operation applied, the result assigned; type may change.

### 3.4 Membership `->`

`a -> b` evaluates to bool.

- `b` a range → integer-in-range test (§5 arithmetic, no overflow).
- `b` a string → byte-substring containment (`"ell" -> "hello"` → True;
  empty needle → True).
- anything else (including non-range/string types in 2B order of arrival) →
  runtime error `unsupported operand types for ->: <t1> and <t2>`.

## 4. Truthiness, `and`, `or`, `not`

Truthiness is **total** (never an error): falsy = `False`, `None`, int `0`,
float `0.0`, empty string (0 bytes), empty range (no representable member);
everything else truthy (`True`, nonzero numbers, nonempty strings/ranges).

Logical operators return **bool** (user decision — not Python's
operand-returning `and`/`or`) while short-circuiting:

- `a and b` — evaluate `a`; falsy → `False` (`b` not evaluated); else
  `bool(b)`.
- `a or b` — evaluate `a`; truthy → `True` (`b` not evaluated); else
  `bool(b)`.
- `not a` — `bool(!truthy(a))`.

Ternary `a if c else b` — evaluate `c` once, then exactly one branch.
`and`/`or`/`not` are keywords and cannot be identifiers.

## 5. Control flow

- `if`/`elif`/`else`: zero or more `elif`, optional `else`; exactly the first
  truthy branch runs; conditions evaluated in order, once each until true.
- `while cond: body` — pre-test; may be zero iterations; `break`/`continue`
  apply to the innermost loop.
- `for var -> iterable: body` — iterates:
  - a `range()` value (§6) — each iteration binds `var` to the next int;
  - a string — each iteration binds `var` to a **1-byte string** (raw byte;
    2A strings are raw bytes; UTF-8 character iteration is part of the 2B
    Text decision);
  - anything else → runtime error `'<t>' object is not iterable` raised at
    loop entry.
  `var` is assigned before the body runs each iteration. The loop variable
  leaks after the loop (§7). Mutation-during-iteration is undefined until 2B
  defines it; 2A strings/ranges are immutable, so it cannot arise.
- `break` exits the innermost loop; `continue` skips to its next iteration.
  Both are compile-time errors outside any loop:
  `'break' outside of a loop` / `'continue' outside of a loop`.
  A `switch` does not count as a loop; `break` inside a switch inside a loop
  breaks the **loop**.
- `switch scrutinee:` — arms in source order; each non-wildcard arm matches
  iff `scrutinee == label` under M1's total `==` (so `case 1:` matches int 1
  and float 1.0; `case True:` does NOT match int 1). First match executes and
  the switch **ends automatically** (no fall-through, no `break` needed). No
  match → wildcard arm if present, else nothing. Case labels are constants
  only (grammar §2); duplicates are a semantic error (§8).
- `pass` — no-op statement.
- Single-line suites (§2) execute their single statement exactly like a
  one-statement block. `return` remains function-only with M1's messages
  through any nesting depth.

## 6. `range()` (reserved builtin)

`range(stop)`, `range(start, stop)`, `range(start, stop, step)` — all int
arguments (default start=0, step=1; `range(5)` == `range(0, 5, 1)`).

- Float/bool/string argument → runtime error
  `range() expects int arguments, got <t>`.
- `step == 0` → runtime error `range() arg 3 must not be zero`.
- Stop is exclusive. step>0 iterates while `i < stop`; step<0 while
  `i > stop`. Empty when the range contains no int64.
- Ranges are **values** (lazy; never materialized) usable in `for` and `->`.
  `print(range(3))` prints `range(0, 3, 1)` (start, stop, step always shown).
- Iteration/index arithmetic must never exhibit undefined behavior: when the
  next index would leave int64 range, iteration **stops**. Membership uses
  exact arithmetic (no overflow) for any int64 bounds.

## 7. Scope additions

M1 scope rules stand; additions:

- A `for` loop variable is assigned-its-name: local for the whole enclosing
  function (M1's assigned-anywhere rule), global at top level / in `main():`.
  Loop variables **leak** after the loop (Python-like).
- Loop targets participate in all reserved-name rules:
  `cannot use reserved name 'print' as loop variable` (resp. `range`).
  A top-level `for` over a name that is a function is allowed since 0.3
  (separate namespaces — SPEC §8): the loop variable is the variable
  namespace, `name()` still calls the function.
- `range` mirrors `print` for: assignment
  (`cannot assign to reserved name 'range'`), parameter
  (`cannot use reserved name 'range' as parameter`), function declaration
  (`cannot declare function with reserved name 'range'`), bare read
  (same treatment `print` receives for a bare read).

## 8. Compile-time errors (semantic — report ALL; M1 §7 unchanged plus)

| Condition | Message |
|---|---|
| duplicate case value (same under total `==`) | `duplicate case value in switch` |
| two `case *:` arms | always caught earlier by the §2 ordering rule — the first wildcard is not last → `'case *' must be the last case in a switch` (parser, first error wins; the §8 duplicate rule is unreachable for wildcards and applies to value labels only) |
| `break` outside any loop | `'break' outside of a loop` |
| `continue` outside any loop | `'continue' outside of a loop` |
| `range()` called with 0 or more than 3 arguments | `function 'range' expects 1 to 3 argument(s), got <n>` |

Parser errors (§2 table) abort at first occurrence with caret display.
Any compile error → exit 1.

## 9. Runtime errors (M1 §8 table plus)

Format unchanged: `<file>:<line>: runtime error: <message>`.

| Condition | Message |
|---|---|
| bad `**` operands | `unsupported operand types for **: <t1> and <t2>` |
| int ** negative int | `negative exponent` |
| bad bitwise operands | `unsupported operand types for <op>: <t1> and <t2>` (`& \| ^ << >>`) |
| bad `~` operand | `unsupported operand type for unary ~: <t>` |
| shift count outside 0..63 | `shift count must be in range 0..63` |
| bad `->` RHS/operand | `unsupported operand types for ->: <t1> and <t2>` |
| `for` over non-iterable | `'<t>' object is not iterable` |
| bad `range()` args | `range() expects int arguments, got <t>` |
| zero step | `range() arg 3 must not be zero` |

Type names: `unset None int float bool string` (+ `range` when 2B adds more).

## 10. Runtime API additions (runtime/cstar_rt.{h,c})

All M1 APIs unchanged. Additions (C11, libc+libm only):

```c
/* CS_RANGE tag: value holds int64 start/stop/step (contract exposes fields). */
cs_value cs_range3(cs_value start, cs_value stop, cs_value step, int line);
/* Validates args (int-only, nonzero step) with §9 messages. */

bool cs_truthy(cs_value v, int line);            /* §4; total */
cs_value cs_pow(cs_value a, cs_value b, int line);      /* §3.1 */
cs_value cs_band(cs_value a, cs_value b, int line);     /* §3.2 */
cs_value cs_bor(cs_value a, cs_value b, int line);
cs_value cs_bxor(cs_value a, cs_value b, int line);
cs_value cs_shl(cs_value a, cs_value b, int line);
cs_value cs_shr(cs_value a, cs_value b, int line);
cs_value cs_bnot(cs_value a, int line);

bool cs_contains(cs_value item, cs_value container, int line);  /* §3.4 */

/* Minimal iteration protocol for `for` (range + string in 2A; 2B extends). */
typedef struct cs_iter { /* kind + range state + string cursor */ int kind; } cs_iter;
cs_iter cs_iter_open(cs_value v, int line);        /* §5 not-iterable error */
bool cs_iter_next(cs_iter* it, cs_value* out);     /* false at exhaustion */
```

`cs_print` gains the `range` format `range(<start>, <stop>, <step>)`.
Range values compare with total `==`/`!=` **structurally** (start, stop and
step all equal) — ratified integrator decision; `cs_type_name` of a range is
`range`.
All arithmetic stays free of C undefined behavior (wrap via uint64_t as M1).

## 11. Generated-C contract additions (docs/contracts/m1-semantics.md §10 extended)

Structure, mangling, guards, line passing and compile command are unchanged.
New requirements:

- `if`/`elif`/`else` → C `if`/`else if`/`else` on `cs_truthy(cond, line)`.
- `while` → C `while (cs_truthy(cond, line))`; `break`/`continue` map 1:1.
- `for` over a **direct `range(...)` call** is open-coded: validate via
  `cs_range3`, then iterate int64 induction variables with **no signed
  overflow UB** (unsigned accumulation or explicit overflow guard; loop must
  terminate per §6). Any other iterable → `cs_iter_open`/`cs_iter_next`.
- `and`/`or`/ternary → C conditional expressions (lazy right side):
  `and` → `(cs_truthy(l,lno) ? cs_bool(cs_truthy(r,lno)) : cs_bool(false))`;
  `or` → `(cs_truthy(l,lno) ? cs_bool(true) : cs_bool(cs_truthy(r,lno)))`;
  ternary → `(cs_truthy(c,lno) ? t : e)`.
- `switch` → if/else chain of `cs_eq(scrut, label_value, ...)` in source
  order (optimization later; semantics: first match ends).
- Compound assign → read target, apply op runtime call, write back.
- Loop variable read guards: the loop assigns before each body execution;
  codegen may skip the used-before-assignment guard for reads of the loop
  variable inside its own body (optional optimization).
- Every new runtime call receives its AST node's cstar source line.

## 12. Fixed source file layout (ownership — Stage 2A agents)

```
docs/contracts/m2a-semantics.md src/ast.h src/frontend.h src/codegen.h
src/diagnostics.{h,cpp}                                   (contract — integrator only)
src/lexer.{h,cpp} src/parser.{h,cpp}                      (agent: frontend syntax)
src/semantic.{h,cpp}                                      (agent: semantic pass)
src/codegen.cpp                                           (agent: C backend)
runtime/cstar_rt.{h,c}                                  (agent: runtime)
src/main.cpp Makefile tools/embed.cpp                     (integrator, if needed)
tests/** examples/** bench/** docs/performance/baseline-m1.md          (test/bench agents, later)
SPEC.md README.md docs/contracts/m1-semantics.md                    (integrator, at the end)
editors/vscode/**                                         (integrator, at the end)
```

Agents must not run `make` in the project root (build/cstar is shared);
syntax-check single files with
`clang++ -std=c++17 -Wall -Wextra -fsyntax-only -Isrc src/<file>.cpp`
(respectively `clang -std=c11 -fsyntax-only runtime/cstar_rt.c`) and keep
scratch work in /tmp. Contract files are read-only for agents: report
suspected contract bugs instead of editing.

## 13. Test plan (Stage 2A acceptance)

M1 suite (60 checks) stays green. New cases (harness conventions unchanged):
if/elif/else chains + nesting; truthiness of every value type (incl. in
`and`/`or`/`not`/ternary/`while`); short-circuit proof via print side
effects; `for` over range 1/2/3-arg, negative step, exclusive stop,
zero-step error, huge-range termination; `for` over string (1-byte values);
while zero-iterations; nested loops with break/continue (incl. break inside
switch inside loop); switch first-match/no-fallthrough/wildcard/no-match/
dup-case error/wildcard-not-last error/`case 1` matching `1.0`; ternary
nesting; compound assign all five ops + overflow + type errors; membership
on range/string + bad-type error; `**` (int wrap, negative exp error, float
paths) and all bitwise ops + errors + shift range; `pass`; single-line
if/elif/else/while forms and their error cases; loop-variable leak;
`range` reserved-name errors; orphan elif/else, `case` outside switch,
`for` without `->` parser errors. Every runtime-error test asserts the full
`<file>:<line>: runtime error: <message>` form and exit code 1.
