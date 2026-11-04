# Stage contracts

These are the **working contracts** each milestone stage was built against.
They were written *before* the implementation, ratified during it, and remain
normative where [`SPEC.md`](../../SPEC.md) says so: the SPEC is derived from them,
and **if the SPEC and a contract disagree, the contract governs** — except for
one carve-out: the unreleased v0.3 *surface-syntax* overhaul (`()` calls, `in`
membership/binding, `name():`/`name(params):` declarations, `main:`,
`import`/`from`) is front-end-only, so where a contract's examples predate it,
SPEC §4 governs the surface form while the contract remains normative for
semantics and error wording.

| File | Stage | Version tag | Covers |
|---|---|---|---|
| [m1-semantics.md](m1-semantics.md) | M1 | `0.1.0-m1` | the strict core: values, `print`, functions, `main():`, scope rules, the first error tables, the CLI contract, the generated-C contract and source ownership |
| [m2a-semantics.md](m2a-semantics.md) | M2 Stage 2A | — | control flow (`if`/`while`/`for`/`switch`), `**`, bitwise operators, membership `->`, ternary, `and`/`or`/`not`, compound assignment, `range()` |
| [m2b-semantics.md](m2b-semantics.md) | M2 Stage 2B | `0.2.0-m2b` | collections, text vs bytes, slicing, destructuring, comma-join, builtin methods, function overhaul (defaults, named, `*args`/`**kwargs`, function values, closures, `global`/`nonlocal`) |
| [m2c-semantics.md](m2c-semantics.md) | M2 Stage 2C | `0.2.0-m2c` | garbage collection, exceptions, classes, object identity |

Cross-scope access is specified in
[`../reference/cross-scope-access.md`](../reference/cross-scope-access.md); the
Python-parity builtins and the performance work (version `0.2.1`) are specified
directly in SPEC §5.11 and §11.8.

## How to use them

* **Answering "what is the rule?"** — read SPEC first; it is the consolidated,
  current statement. Use the contract when you need the *reasoning*: each one
  opens with the user-approved decisions and the ratified design choices.
* **Older contracts are layered, not replaced.** M1 stays normative wherever 2A
  does not override it; 2A wherever 2B does not; and so on. A contract lists the
  specific behaviours it changes ("approved changes").
* **Do not edit a contract to match new behaviour silently.** The 2C contract
  states it directly: deviations require updating the contract first.
* The `*-progress.md` records of how each stage was landed live in
  [`../history/`](../history/README.md).

## Test obligations

Each contract ends with its acceptance plan (2A §13, 2B §15, 2C §4); the cases
live in `tests/cases/` and are described in
[building and testing](../development/building-and-testing.md).
