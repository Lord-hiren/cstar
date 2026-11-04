# Specification map

[`SPEC.md`](../../SPEC.md) is one long, normative document (about 2,600
lines). **Its section numbers are stable** — source comments, test headers and
the editor extension cite them (`SPEC §5.11.5`, `SPEC §8`, …). This page is the
table of contents organised by *question*, plus which contract document governs
a topic if the two ever disagree.

## By question

| I want to know… | Read |
|---|---|
| what the language includes and excludes | SPEC §1 |
| source file / executable extensions | SPEC §2 |
| how indentation, newlines and comments lex | SPEC §3.2–§3.4 (comments incl. the `//` footgun: §3.4) |
| the reserved words and reserved builtin names | SPEC §3.5 |
| number, text and bytes literal rules | SPEC §3.6 |
| the grammar (EBNF) and operator precedence | SPEC §4.1, §4.2 |
| how comma-join and destructuring parse | SPEC §4.4 |
| what types exist, and what each operator accepts | SPEC §5.1, §5.2 |
| integer wraparound, division and modulo | SPEC §5.3, §5.4 |
| equality and ordering rules | SPEC §5.5 |
| exactly how `print` and `str()` format each type | SPEC §5.6 |
| text/bytes semantics, collections, slicing | SPEC §5.7–§5.9 |
| the methods on `list`/`map`/`set`/`text`/`bytes` | SPEC §5.10 |
| every builtin — rules, original fifteen, Python-parity table, compiled form, deviations, stubs, exceptions | SPEC §5.11 (§5.11.1–§5.11.7) |
| classes, instances, attributes, bound methods | SPEC §5.12 |
| `is` / `is not` | SPEC §5.13 |
| truthiness, `and`/`or`/`not`, conditional expression | SPEC §6.1, §6.2 |
| membership `in`, `if`, `while`, `for`, `switch`, `break`/`continue`/`pass`, `range()` | SPEC §6.3–§6.9 |
| `try`/`except`/`finally`/`raise` and the exception classes | SPEC §6.10 |
| `import`/`from`, module resolution, selective loading, module initialization | SPEC §14 (unreleased Stage 2D) |
| script-style execution, `main:`, forward references | SPEC §7 |
| function calls, defaults, named/variadic arguments, closures | SPEC §7.1, §7.2 |
| the garbage collector as the language sees it | SPEC §7.3 |
| name resolution, `global`/`nonlocal`, `variable.` | SPEC §8 |
| every compile-time and runtime error message | SPEC §9.1 (lexer/parser), §9.2 (semantic), §9.3 (runtime), §9.4 (exit codes) |
| the command line | SPEC §10 |
| how the compiler is structured, the generated-C contract, the runtime's duties, the cache | SPEC §11 |
| what is not supported / roadmap | SPEC §12, §13 |
| worked examples | SPEC Appendix A |

## Section index

| § | Title |
|---|---|
| 1 | Status and scope |
| 2 | Source files and executables |
| 3 | Lexical structure — 3.1 encoding/identifiers, 3.2 indentation, 3.3 newlines, 3.4 comments, 3.5 keywords/reserved names, 3.6 literals, 3.7 tokens |
| 4 | Grammar — 4.1 EBNF, 4.2 precedence, 4.3 prose notes, 4.4 comma-join and destructuring |
| 5 | Values and operators — 5.1 tags, 5.2 operator table, 5.3 wraparound, 5.4 division/modulo, 5.5 comparison/equality, 5.6 `print` formatting, 5.7 text/bytes, 5.8 collections, 5.9 slicing, 5.10 builtin methods, 5.11 builtins, 5.12 classes, 5.13 identity |
| 6 | Control flow — 6.1 truthiness … 6.9 `range()`, 6.10 exceptions |
| 7 | Execution model — 7.1 calls, 7.2 nested declarations, 7.3 garbage collector |
| 8 | Scope and name resolution |
| 9 | Errors — 9.1 lexer/parser, 9.2 semantic, 9.3 runtime, 9.4 exit codes |
| 10 | Command-line interface |
| 11 | Implementation and architecture — 11.1 shape, 11.2 pipeline, 11.3 generated-C contract, 11.4 runtime duties, 11.5 source layout, 11.6 cache, 11.7 Approach B, 11.8 performance mechanisms |
| 12 | Known limitations |
| 13 | Future work and roadmap |
| 14 | Modules and imports (Stage 2D, unreleased) — 14.1 syntax, 14.2 resolution, 14.3 names/binding, 14.4 execution/initialization, 14.5 selective loading, 14.6 errors, 14.7 run cache |
| A | Appendix A — examples A.1–A.10 |

## Precedence between documents

1. **`SPEC.md`** and the **stage contracts** in [`../contracts/`](../contracts/README.md)
   are normative. If they disagree, *the contracts govern* — except that the
   unreleased v0.3 surface-syntax migration is front-end-only, so SPEC §4
   governs the surface form wherever a contract's examples predate it.
2. [`cross-scope-access.md`](cross-scope-access.md) is normative for `variable.`
   (SPEC §4.1/§8 summarise it).
3. Everything in `guide/`, `internals/`, `performance/`, `development/` and
   `history/` is **informative**. If one of them contradicts SPEC, SPEC wins —
   please fix the informative page.
4. [`known-issues.md`](known-issues.md) lists places where the *implementation*
   currently contradicts the normative text.

## Which contract introduced what

| Topic | Contract |
|---|---|
| strict M1 core: values, `print`, functions, `main:`, scopes, M1 error table, CLI | [m1-semantics](../contracts/m1-semantics.md) |
| control flow, `**`, bitwise, `()`, ternary, `range()`, compound assignment | [m2a-semantics](../contracts/m2a-semantics.md) |
| collections, text/bytes, slicing, destructuring, comma-join, defaults/named/variadic args, closures, `global`/`nonlocal` | [m2b-semantics](../contracts/m2b-semantics.md) |
| garbage collection, exceptions, classes, `is` | [m2c-semantics](../contracts/m2c-semantics.md) |
| `variable.<qualifier> <name>` | [cross-scope-access](cross-scope-access.md) |
| modules/imports and selective loading | SPEC §14 (Stage 2D — no separate contract) |
| Python-parity builtins, `input`/`time`/`clock`, performance lane | SPEC §5.11, §11.8 (no separate contract; [optimization](../performance/optimization.md) is the informative companion) |
