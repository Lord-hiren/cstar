# Cross-scope access (`variable.`) performance baseline

Pair: `bench/cases/varq/cross_scope.pen` vs its C twin `cross_scope.cpp`
- same seed, same arithmetic, byte-identical output, exec-only timing,
  9 reps, pinned to one core.

| metric | pentest (`variable.` access) | C twin |
|---|---|---|
| min wall ms | 18.293 | 8.944 |
| median wall ms | 19.137 | 12.122 |
| cold compile ms | 562 | n/a (clang direct) |

Target (`docs/reference/cross-scope-access.md` §6): pentest < 20 ms OR >= C.  
under 20 ms: **yes** — not slower than C: **no** → **PASS**
