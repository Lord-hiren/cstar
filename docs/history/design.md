# Pentest Programming Language — Milestone 1 Design

> **Historical.** The original Milestone 1 plan, written before implementation.
> Superseded by the [stage contracts](../contracts/README.md) and
> [`SPEC.md`](../../SPEC.md) wherever they differ; kept for the vision, the
> canonical examples (SPEC Appendix A.1–A.4) and the compiler-approach decision.

**Document status:** Planning draft; no implementation started  
**Language name:** Pentest  
**Source extension:** `.pen`  
**Native executable naming convention:** `.pentest`

This document records the design decisions discussed so far for Pentest. It distinguishes agreed direction from details that still need decisions. The purpose of Milestone 1 is to define a small, consistent language that can later be implemented as a native compiler.

---

## 1. Vision and goals

Pentest is a custom programming language intended primarily for:

- Cybersecurity tools and utilities
- Automation scripts
- Network utilities
- Binary and text parsing
- General-purpose programming when needed

The language should have a readable, Python-like structure, but its syntax and behavior are designed specifically for Pentest rather than copied wholesale from Python.

### Core goals

1. **Simple source code.** Programs should be short, readable, and require little boilerplate.
2. **Native compilation.** The eventual compiler should produce native machine-code executables, not require a Python interpreter to execute every program.
3. **Competitive performance.** Aim for performance competitive with optimized C++ on relevant workloads. This is a goal, not a guarantee that Pentest will always match or beat C++ or assembly.
4. **Useful for security work.** Provide convenient libraries for networking, HTTP, DNS, byte buffers, process execution, file handling, and concurrency.
5. **Predictable execution.** Global statements, function declarations, and the special `main():` block must have clearly defined behavior.
6. **Incremental implementation.** Build a small working language first, then add advanced features without making the basic language unnecessarily complex.

---

## 2. Source files, executables, and command-line interface

### File extensions

- Pentest source files use **`.pen`**.
- Compiled native executables use **`.pentest`** by convention.
- Example source: `scanner.pen`
- Example executable: `scanner.pentest`

Linux does not technically require executables to have an extension, but `.pentest` will make compiled programs easy to distinguish from source files.

### Intended commands

The command-line program will be named `pentest`, similar in spirit to the `python` command.

Proposed command examples:

```bash
# Run a source file through the Pentest toolchain
pentest scanner.pen

# Build a standalone native executable
pentest build scanner.pen -o scanner.pentest

# Run the compiled executable on Linux
./scanner.pentest
```

The exact CLI grammar is **not finalized**. In particular, we still need to decide whether `pentest scanner.pen` compiles to a temporary native executable and runs it, uses a cached build, or uses another development workflow. The goal is for users to be able to run source files conveniently and also build standalone executables.

---

## 3. Basic syntax

Pentest will use indentation-based blocks, inspired by Python. A colon starts a block, and consistent indentation defines the block's contents.

### Variables

The chosen basic variable syntax is:

```python
count = 10
name = "Pentest"
enabled = True
print(count)
```

The primary form is `VARIABLE = VALUE`; explicit type annotations are not required in ordinary variable assignments. The type system and the rules for inferring types still need to be designed.

### Functions

Ordinary functions use the `function` keyword:

```python
function greet(name):
    print("Hello", name)

function do_nothing():
    return None
```

A function may have parameters or no parameters. For example, both `function greet(name):` and `function greet():` are valid forms in the intended design. The exact syntax for parameter types, default values, return types, and argument validation is not yet finalized.

### Special entry block: `main():`

The entry block uses a special form without the `function` keyword and has no parameters:

```python
main():
    print("Program started")
```

Pentest automatically executes this block when normal top-level execution reaches it. Programmers do not need to call `main()` manually.

`main():` is special syntax; it is not written as `function main():` in the current design.

### Function calls

Functions can be called from other functions, including functions declared later in the source file:

```python
main():
    helper()

function helper():
    print("Helper ran")
```

Expected output:

```text
Helper ran
```

The compiler must discover function declarations before execution so that forward function references can work.

### Built-ins

`print(...)` is expected to be a built-in function. The complete built-in function list is not yet defined.

---

## 4. Execution model

This is one of Pentest's defining features. The current intended behavior combines ordinary script execution with a special automatically executed `main():` block.

### Rule 1: Discover declarations before execution

Before executing statements, the compiler/runtime performs a declaration-discovery phase. At minimum, it discovers function declarations and class declarations so they can be referenced before their textual definition.

Discovery registers declarations; it does **not** execute every function body.

### Rule 2: Ordinary top-level statements run in source order

Statements outside function/class declarations execute from top to bottom, in the order they appear in the file.

Example:

```python
print("first line")

main():
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

The `main():` body executes automatically when execution reaches that block, then execution continues with later top-level statements.

### Rule 3: Function definitions do not run their bodies

Defining a function registers it. Its body runs only when the function is called, except for the special `main():` block, which is automatically executed when reached.

Example:

```python
function hello():
    print("hello world")

print("Script continues")
```

Expected output:

```text
Script continues
```

`hello()` is not called, so its body does not run.

### Rule 4: A file without `main():` works as a normal script

If no `main():` block exists, ordinary top-level statements execute from top to bottom. Function and class declarations are registered but their bodies are not executed merely because they appear in the file.

Example:

```python
a = 10
print(a)

function hello():
    print("hello world")
```

Expected output:

```text
10
```

### Rule 5: Only called function bodies execute

A function body runs when it is reached through a function call. This includes calls from another called function. Functions that are never called do not execute their bodies.

Example:

```python
function a():
    return None

function b():
    return None

function c():
    a()

main():
    c()
```

Here, `main()` executes, calls `c()`, and `c()` calls `a()`. The body of `b()` is never executed.

### Rule 6: Function declarations may appear before or after their callers

Because declarations are discovered before execution, this should work:

```python
main():
    dummy()

function dummy():
    print("dummy ran")
```

Expected output:

```text
dummy ran
```

### Rule 7: Variable assignments still happen in execution order

Function declarations can be discovered early, but global variable values are not automatically available before their assignments execute. The previously discussed idea of allowing a variable to be read before its assignment was set aside.

For example, this is the intended safe ordering:

```python
a = 10

main():
    print(a)
```

Expected output:

```text
10
```

But this ordering attempts to read `a` before it has been assigned:

```python
main():
    print(a)

a = 10
```

The recommended behavior is an error rather than silently using an unpredictable value. Whether such cases can always be detected at compile time or sometimes require a runtime error remains to be specified.

### Global statement versus declaration summary

| Source construct | Behavior |
|---|---|
| `a = 10` | Executes as a top-level statement when reached; establishes/updates a global value according to the eventual assignment rules. |
| `print(a)` at top level | Executes in source order. |
| `function helper(): ...` | Registers the function; does not execute its body. |
| `main(): ...` | Executes its body automatically when top-level execution reaches it. |
| `class Name: ...` | Class declarations are intended to be discovered in advance; exact class-body and initialization semantics are still open. |
| `helper()` | Calls `helper` and executes its body. |

---

## 5. Scope and visibility

The intended model distinguishes global declarations/variables from things in local or more restricted scopes. The design discussion also mentioned private and protected scope, especially in relation to classes.

The following details are not finalized yet:

- How local variables are declared and accessed
- Whether function-local variables can shadow global names
- The exact syntax and behavior of class members
- How `private` and `protected` visibility are expressed
- Whether nested functions are supported in the first milestone
- How global reassignment works and whether a separate declaration is needed to modify a global from inside a function

For the first working version, a simpler scope model is preferable: module-level globals, function-local variables, and global function declarations. More advanced class visibility can be added after basic execution works.

---

## 6. Comments

The intended comment forms are:

### Single-line comments

```python
# Hash-style comment
// Slash-style comment
```

### Block comments

```text
/*
   Block comment spanning multiple lines
*/
```

### Triple-quoted multiline comments

Triple quotes were also requested as a supported multiline comment form. The precise rules for triple quotes are still open, particularly whether the same syntax can also represent multiline string literals.

Comment syntax must be specified in the lexer so that comment markers inside strings are not accidentally treated as comments.

---

## 7. Native compilation and performance

Pentest is intended to generate native executables. The goal is to combine concise source code with performance competitive with optimized C++ in suitable workloads.

This requires more than a short syntax. The compiler must produce efficient code, use appropriate native libraries, and define types and memory behavior in ways that the optimizer can understand.

### Compiler implementation approach: still undecided

Two approaches have been discussed:

**Approach A — Easier first prototype**

1. Build a Pentest lexer/parser and semantic checks.
2. Translate Pentest source into C.
3. Compile the generated C using Clang.
4. Produce a native executable.

This lets the first version reuse a mature C compiler for native code generation and optimization.

**Approach B — Direct native compiler backend**

1. Build the lexer/parser and semantic analysis.
2. Generate LLVM intermediate representation (LLVM IR).
3. Use LLVM to optimize and generate native machine code.
4. Link the required runtime and libraries.

This provides more direct control over code generation but requires a more involved frontend/backend integration.

The choice is **not finalized**. A practical plan is to start with the simpler C-generation route and later replace it with a direct LLVM backend if Pentest needs more control.

### Performance principles

- Do not assume that a custom syntax alone makes a language fast.
- Resolve types and operation behavior clearly enough for compile-time optimization.
- Keep unnecessary runtime dispatch and allocation out of performance-critical code where possible.
- Prefer efficient byte-buffer and slice handling for packet and binary parsing.
- Measure real programs against optimized C++ instead of relying on assumptions.
- Keep network latency, file I/O, algorithm choice, and CPU-bound computation separate when benchmarking.

Pentest should not claim that it is always faster than C++ or assembly. The realistic target is competitive performance with simpler source code for the workloads that matter to its users.

---

## 8. Intended cybersecurity and automation direction

Networking and security utilities are the primary use cases. These are goals for the language and its standard library; not all are required for the first minimal compiler.

Potential standard-library modules include:

- **`net`** — TCP/UDP sockets, IP addresses, ports, DNS and timeouts
- **`web`** — HTTP clients, TLS-backed requests and response handling
- **`bytes`** — raw byte buffers, slices, endian conversion, hex encoding and decoding
- **`parallel`** — bounded concurrency, task pools, cancellation and timeouts
- **`proc`** — process execution with structured arguments and captured output
- **`parse`** — JSON and common text/binary parsing helpers
- **File and command-line utilities** — reading files, command-line arguments and environment variables

These modules are future-facing design goals. Their exact APIs should be designed after the base language and compiler can handle variables, functions, control flow and errors.

---

## 9. Proposed Milestone 1 scope

The first implementation should be deliberately small. Its purpose is to validate Pentest's syntax and execution model, not to deliver the full cybersecurity standard library.

### Core features to target

1. Read a `.pen` source file.
2. Tokenize and parse the chosen syntax.
3. Support basic values and variable assignments, such as `a = 10`.
4. Support basic arithmetic and expressions.
5. Support calls to a built-in `print(...)` function.
6. Support function declarations with zero or more parameters.
7. Discover function declarations before execution, allowing forward function references.
8. Support the special parameterless `main():` block.
9. Follow the execution rules described in Section 4.
10. Report syntax and execution errors clearly.
11. Generate and run a native executable through the chosen initial backend.
12. Provide a CLI named `pentest` that accepts `.pen` files and can build `.pentest` executables.

Conditions, loops, classes, return-value semantics, richer types, networking and advanced comments need a precise specification before or during implementation. Their priority in the first prototype still needs to be decided.

---

## 10. Decisions still needed

The following questions should be answered before the language specification is treated as final:

1. **Type system:** Are variables statically typed with type inference, dynamically typed, or a mix? Can a variable change from an integer to a string later?
2. **Reassignment:** Is `a = 20` after `a = 10` always allowed? How are global assignments inside functions handled?
3. **Function parameters:** Are parameter types optional? Will Pentest support default parameter values, named arguments, or variadic parameters?
4. **Return values:** What are the exact rules for `return`, and is `None` an official value/keyword?
5. **`main()` uniqueness and placement:** Is at most one `main():` block allowed per file? The current intended behavior is that it executes when source-order execution reaches it.
6. **Multiple modules/files:** How are imports and cross-file function declarations discovered?
7. **Classes:** What is the class declaration syntax, and when/how is a class body executed or initialized?
8. **Comments and strings:** Are triple-quoted constructs comments, multiline strings, or both with different rules?
9. **Control flow:** What are the exact `if`, `else`, loop, and range syntaxes?
10. **Errors:** Which problems are compile-time errors, and which are runtime errors?
11. **Compiler backend:** Start by generating C and using Clang, or generate LLVM IR directly?
12. **CLI behavior:** Should `pentest file.pen` compile on every run, cache generated output, or use another development workflow?
13. **Target platform:** Is Linux the only initial target, or should Windows/macOS support be considered from the beginning?

---

## 11. Canonical examples

### A. Ordinary script, no `main()`

```python
a = 10
print(a)

function hello():
    print("hello world")
```

Expected output:

```text
10
```

### B. Top-level statements around `main()`

```python
print("first line")

main():
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

### C. `main()` calls a function declared later

```python
main():
    dummy()

function dummy():
    print("dummy ran")
```

Expected output:

```text
dummy ran
```

### D. Only called function bodies execute

```python
function a():
    return None

function b():
    return None

function c():
    a()

main():
    c()
```

The bodies of `main`, `c`, and `a` execute. The body of `b` does not.

---

## 12. Guiding principle

Pentest should remain easy to write, predictable to execute, and capable of producing efficient native binaries. For Milestone 1, correctness and a coherent execution model matter more than having many features. Nail the grammar and semantics first; then implement the smallest complete compiler that follows them.
