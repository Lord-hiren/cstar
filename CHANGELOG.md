# Changelog

All notable changes to Pentest. Dates and commit ids are from the repository
history. The format follows [Keep a Changelog](https://keepachangelog.com/);
versions are the strings reported by `pentest --version`. For the narrative see
[docs/history](docs/history/README.md).

## [Unreleased]

### Changed
- **The CLI is now `cstar`** (the rename from Pentest to cstar begins). The
  old names keep working until v0.4: `pentest` is an alias of the same driver,
  `.pen`/`.pent` sources are still accepted, and `$PENTEST_PATH` is still
  searched after the new `$CSTAR_PATH`.
- Source files use the **`.cst`** extension; modules resolve `.cst` first
  (`<rel>.cst`, then `.pen`/`.pent`, then `__init__.cst`/`.pen`/`.pent`).
- `cstar run`, `cstar build` and `cstar check` **without a file use
  `./index.cst`** (exit 2 if it does not exist).
- **`build` output has no extension**: `cstar build tools/scan.cst` writes
  `./scan` (previously `./scan.pentest`). If that name is an existing
  directory, `build` stops with exit 2 and asks for `-o`.
- `--version` prints `cstar 0.2.1`. The run cache moved to
  `$XDG_CACHE_HOME/cstar/` (`<key>.bin`); scratch folders are `cstar-*`. The
  old `~/.cache/pentest/` is no longer used and can be deleted.
- Error messages name the language **cstar**: `'eval' is not available in
  cstar: …`, `cstar supports only the UTF-8 encoding`, `pow() 3rd argument
  must be positive (cstar does not support negative moduli)`, `chr()
  surrogates are not allowed (cstar strings are UTF-8)`.
- The test programs use `.cst`: all 365 cases in `tests/cases/` and the module
  fixtures in `tests/cases/lib/` were renamed from `.pen`.
- **The VSCode extension is cstar**: id `cstar-lang.cstar-lang`, language id
  `cstar`, grammar `source.cstar`. It opens `.cst` files (and `.pen`/`.pent`
  until v0.4), runs `cstar check` and `cstar run` (falling back to a `pentest`
  CLI), and reads the new `cstar.executable`, `cstar.diagnostics` and
  `cstar.diagnosticsDelay` settings. The `pentest.*` settings and the
  `pentest.run` / `pentest.toggleLineComment` command ids still work until v0.4;
  the settings show as deprecated.
- The editor extension knows the SIMD vector builtins `fvec`, `ivec`,
  `fvec_fill`, `ivec_fill` and `dot` (highlighting, completion, hover, reserved
  names); it now covers all 78 builtins the compiler reserves.

### Tooling
- GitHub Actions CI (`.github/workflows/ci.yml`): a warning-free build,
  `make test`, the editor smoke test and the docs checker on Ubuntu 24.04, for
  pushes to `main`/`phase-*` and pull requests to `main`.
- `install-vscode-extension.sh` installs `cstar-lang.cstar-lang` and removes
  any other link to the same `editors/vscode` folder (such as the pre-rename
  `pentest-lang.pentest-lang`), which would load the extension twice; it never
  deletes real folders. On Windows it links with a directory junction (Git
  Bash's `ln -s` silently copied), and on macOS it uses `~/.vscode/extensions`
  (it used a folder VS Code does not read).

### Documentation
- New: [quick reference](docs/guide/quick-reference.md), the whole language and
  all 78 builtins on one page.
- New: building and testing from Windows through WSL
  ([getting started](docs/guide/getting-started.md#windows-wsl)). A new
  `.gitattributes` keeps line endings LF on every OS; CRLF checkouts broke the
  test harness.
- Corrected: suite counts (372 checks = 365 cases + 7; 1294 smoke checks) and
  builtin counts (78) in the README and guides; the "no third-party
  dependencies / no networking / only libc + libm" claims (programs link
  libcurl for the HTTP builtins), including SPEC §2, §11 and §12 wording; links
  to `examples/` and `extras/` files that are not in the repository (the
  README quick start and getting started now use inline programs). `build/`
  and `scan.log` are no longer tracked.
- Reorganised the documentation into a navigable tree under `docs/`
  (`guide/`, `reference/`, `contracts/`, `internals/`, `performance/`,
  `development/`, `history/`) with an index at [docs/README.md](docs/README.md).
- New: getting started, language tour (executable samples), CLI reference,
  builtins guide, errors and diagnostics, gotchas, specification map, known issues,
  architecture, code generation, runtime internals, performance overview, building
  and testing, benchmarking, editor-extension and documentation guides, history,
  and this changelog.
- Rewrote `README.md` as a short landing page. Updated the `SPEC.md` header and
  §1 status; section numbers are unchanged.
- Corrected: README test counts (320/316 → 368 checks = 361 program cases
  + 7 extra checks; the suite grew with the regression cases below);
  SPEC §6.10 claim that
  `except Exception:` catches everything; SPEC §10 `build` output location
  (current directory, not next to the source); `OPTIMIZATION` references to
  git-ignored local files.
- Moved documents (old → new), with every reference in sources, scripts, the
  editor extension and results files updated:
  `docs/M1-SEMANTICS.md`, `M2-2A/2B/2C-SEMANTICS.md` → `docs/contracts/m*-semantics.md`;
  `docs/VARIABLE-ACCESS.md` → `docs/reference/cross-scope-access.md`;
  `docs/OPTIMIZATION.md`, `docs/BASELINE-*.md` → `docs/performance/`;
  `docs/DESIGN.md`, `docs/M2-2B/2C-PROGRESS.md` → `docs/history/`.
- New [modules guide](docs/guide/modules.md); SPEC §14 is the normative
  modules/imports contract. Updated the language tour, CLI reference, errors
  catalogue, gotchas, spec map, architecture, code-generation, performance and
  history pages for imports and selective loading.

### Added
- **Modules and selective imports (Stage 2D, unreleased).** A program may be
  split across files with `import m`, `import m as x`, `from m import a` and
  `from m import a as b`; specifiers are dotted (`import lib.toolkit` binds
  `toolkit`). Resolution tries the importing file's directory, the working
  directory, then `$PENTEST_PATH`, accepting `<path>.pen`, `<path>.pent` and
  `<path>/__init__.pen`. The linker parses each module once, rejects circular
  imports, renames imported declarations to collision-free internal names,
  rewrites importer references, flattens top-level code in import order, and
  drops imported `main:` blocks. `run`/`build` prune imported declarations that
  are unreachable from the entry program **before** semantic analysis and
  code generation, so unused library code is never analysed, emitted or
  compiled; `check` is strict and validates everything. There is no runtime
  module object, lazy stub or lookup — imported calls are ordinary static
  calls, and a program without imports takes a zero-cost fast path. Run-cache
  keys include every imported module's source. Regression:
  `tests/cases/imp_*`, `lib/*`; suite **368 checks = 361 program cases + 7
  extra checks**.
- **First-class SIMD vector types `fvec` (float64 lanes) and `ivec`
  (int64 lanes)**, with five call-only builtins following the original-15
  pattern (arity checked at compile time, direct `pt_builtin_*` lowering):
  `fvec(items)`, `ivec(items)` (strict: int/bool elements only),
  `fvec_fill(n, v)`, `ivec_fill(n, v)`, `dot(a, b)`. Both types are full
  citizens of the object model: iteration, indexing (negative included),
  item assignment, elementwise `==`, truthiness, list-style display
  (`[1.0, 2.0]`), `type()`/`len()`, `sum`/`min`/`max` fast paths,
  `unhashable type: '<t>'`, and GC-leaf heap objects on 64-byte-aligned
  buffers (the sweep frees the lane buffer). Mixing rules: `fvec` combines
  with any numeric scalar (float64), `ivec` only with `int`/`bool`; a
  length mismatch raises `ValueError: fvec length mismatch` (likewise
  `ivec`) before any element is touched, and `fvec`+`ivec` raises the
  standard `unsupported operand types` TypeError. Regression:
  `tests/cases/fvec_basics.pen`, `fvec_ops.pen`, `ivec_ops.pen`,
  `fvec_errors.pen` (26 error surfaces); the suite is now
  **368 checks = 361 program cases + 7 extra checks**.
- `dot(a, b)` — lane-accumulated inner product (fvec: documented tree/FMA
  reduction; ivec: wrapping sum, associative so lane order is unobservable).
- Compile flag baseline raised: generated programs build with
  `clang -std=c11 -O3 -march=native` (was `-O2`). No `-ffast-math`, ever —
  bit-parity with Python is a contract, and the full suite is unchanged.
- `tests/check_docs.py` — runs every documentation code sample against
  `build/pentest` and checks all relative Markdown links and anchors.

### Changed
- **`slice_ops` and `mixed_num` gaps closed (55.95×/44.62× → 1.12×/1.17× C,
  REPS=15 on the gap-hunt machine and flags).** Slices now use copy-on-write
  `pt_seqbuf` views (`items`/`stride`, detach on write) instead of an
  element-by-element copy, with compact sequence objects, inline
  index/`len` fast paths and branchless index normalization. A whole-lifetime
  int-only list proof plus a flat-list sub-proof keeps `xs[i]` and the
  surrounding accumulator arithmetic in native `int64` lanes, and a lazy
  ephemeral-slice pass turns a slice read only through `target[idx]` in the
  same block into one normalized span plus fused direct reads (shared raw
  view fields, derived `ceil(count/step)` spans for equal bounds). Mixed
  numeric code keeps `float(<int64>)` as the exact cast, guards
  `int(<float>)` for the language's out-of-range `ValueError`, uses the exact
  int64/double ordering helper for mixed comparisons, and routes float
  multiply through `pt_nfmul`'s empty-asm barrier so no FMA contraction
  changes a rounding. `slice_ops` 192.5 → 3.06 ms (C 2.73 ms);
  `mixed_num` 1717 → 38.9 ms (C 33.25 ms); outputs unchanged (the slice twin
  auto-vectorizes, the mixed twin unrolls with FMA and an unchecked cast, so
  the residual is the language's bounds/type/error and rounding parity).
  Gates: 368/368 tests, 39/39
  speedtest pairs byte-identical, 115 lazy-slice differential programs and
  20 mixed-lane differential programs identical to their disabled-pass
  baselines; see `speedtest/results/GAP-CLOSURE-2026-10-21.md`.
- **Cold-start speed: the first `run`/`build` no longer recompiles the
  runtime.** `make` precompiles `runtime/pentest_rt.c` with exactly the flags
  the driver uses on demand (`clang -std=c11 -O3 -march=native -c`) into
  `build/pentest_rt.o` and embeds that object in `build/pentest`, next to the
  runtime sources. A cold cache installs and links it instead of spending
  ~6 s in clang on 8k lines of runtime: an empty-cache `run` drops from
  **6.5 s to 0.21 s** on the development machine (warm stays ~0.01 s), and
  cold `build` from ~6.5 s to ~0.23 s. Because `-march=native` machine code
  is CPU-specific, the build host's `/proc/cpuinfo` `flags` line is embedded
  too: any mismatch (a binary copied to another machine, kernel-masked
  features) silently falls back to the previous compile-and-cache path, and
  a missing `sha256sum`/unwritable cache degrades the same way. The
  runtime-object cache key now also includes the current CPU's `flags` line,
  so a cache shared across machines cannot serve an object built for another
  ISA. Cost: one ~6 s runtime compile per `make` and ~290 KB in the binary;
  no change to generated code (the 37 speedtest twins remain byte-identical).
- **BREAKING: `import` and `from` are now reserved keywords** (they begin an
  import declaration; §3.5). Programs using either as an identifier no longer
  parse. `as` was already reserved. This lands with the Stage 2D module syntax
  above.
- **BREAKING: a global variable and a global function may share a name
  (separate namespaces).** A bare `a` reads the variable and a call `a()`
  reads the function, so the two are syntactically disjoint and the M1
  single-namespace rule is retired: a bare
  read `a` resolves the **variable** first, a call `a(...)` resolves the
  **function** first, `global a` redirects only the variable side, a top-level
  `for` target may reuse a function's name, and `variable.global a` reaches
  the shared variable. `name 'f' is already declared as a function` now fires
  only for a `class` sharing a function's name (classes are constructed by
  calling them; a same-named function would shadow the constructor) and for nested
  declarations colliding with a parameter/local or being assigned (real
  storage collisions, unchanged). Reads/writes of the shared name compile to
  the same code as before for every previously-valid program — only the
  eagerly-rejected programs changed — so there is **no performance impact**;
  generated C for the existing corpus is unchanged. Hardened: a read-only
  `global x` (never assigned anywhere) now registers the module slot, so it
  trips the used-before-assignment guard at runtime instead of failing codegen
  with an undeclared `pt_g_x`. Regression: `tests/cases/ns_fn_var_shared.pen`
  (16-line matrix: both declaration orders, loop target, `global`,
  function values, in-function reads), `err_class_fn_shared_name.pen`;
  `err_conflict_function` / `err_func_loop_var` retired into the positive case.
- **BREAKING: function syntax overhauled — calls and declarations are
  parenthesised like Python, `in` is membership/binding, and the `function`
  keyword is gone.** Declarations are now `name():` (no parameters) or
  `name(p1, p2 = 1):`, class methods are `__init__(self, x):`, and the entry
  block is `main:` (was `main():`). Calls use parentheses with
  comma-separated arguments — `f(x, y)`, zero-arg `f()`, named
  `f(name=value)`, methods `p.step(i)`, builtins `print("hi")` /
  `isinstance(x, int)` — and chain right: `print(type(a))`. A bare name is a
  read, never a call, which is what lets one name be both a global variable
  and a global function (§5 above). The `->` spelling is retired as an
  operator entirely (it served as the pre-0.3 loop binder and then as the
  call operator) and now yields the targeted diagnostic
  `'->' is not an operator; call with 'f(args)'`. The old membership and
  `for`-loop binder `->` moved to `in` (`if x in range(5):`,
  `for i in range(3):`, `for item in collection:`, `x in y`).
  The AST (`Call`/`MethodCall`/`FuncDecl`), semantic analysis and code
  generation are byte-for-byte unchanged, so this is a front-end-only change
  with **no performance impact by construction**; the full corpus (368
  checks),
  all 37 speedtest C twins (byte-identical output), the specification, guides
  and the VS Code extension (grammar, snippets, completions, hover) were
  migrated together. Old syntax now yields targeted diagnostics
  (`'function' keyword was removed; declare with 'name():' or 'name(params):'`,
  `'name:' is not a declaration; declare with 'name():' (no parameters) or
  'name(params):'`, `expected 'in' after loop variable`,
  `expected ':' after 'main'`).
  **Recommended version bump: 0.2.1 → 0.3.0 (breaking).**
- **Vector lane buffers are recycled, not re-faulted.** Swept buffers of
  ≥ 64 KB are parked in an exact-size reuse pool and handed straight back
  out by the next `pt_vec_alloc` of the same capacity (removing the
  per-statement `mmap`/`munmap`/page-fault round-trip that dominated fused
  kernels on 64 MB working sets), and `pt_vec_alloc` no longer zero-fills
  the buffer — every producer fills `[0, len)` and nothing reads past
  `len`, so the extra full pass was dead work.
- **SIMD is now first-class, not hoped for.** The vector runtime operators
  and the new fused kernels are written as explicit clang vector-extension
  lanes (`pt_vd`/`pt_vi`, `PT_VW` = 8/4/2 int64/double lanes per
  AVX-512/AVX2/baseline, plus a scalar tail) instead of relying on the
  auto-vectorizer: one instruction stream per op covers every element, and
  per-element arithmetic is bit-for-bit the scalar operators' (wrap,
  division/modulo edges, same error classes and lines).
- **Expression fusion (Halide/XLA-style intermediate elimination).**
  `analyze_native_fvecs()` runs the same fixpoint discipline as the
  native-int lane and proves which slots only ever hold `fvec`/`ivec`
  values; an elementwise assignment over proven slots (`+ - *` and
  `+= -= *=`, scalar operands broadcast from the int lane or literals)
  then collapses to ONE fused kernel loop — operands hoisted and read
  exactly once, one result allocation, guaranteed lanes, scalar tail —
  instead of one heap temporary per operator. v1 scope: local and global
  slots; `/` `%`, params, cells and try-functions stay on the dynamic path
  (which is itself SIMD). Emission re-checks the proof against the final
  maps, so a slot that ever receives an unshaped write simply does not
  fuse. Regression: fused-vs-scalar bit parity in `fvec_ops.pen` /
  `ivec_ops.pen` (14 fused kernels across the two).
- SIMD primitives (`memcpy` lane loads/stores, `PT_VD_SPLAT`/`PT_VI_SPLAT`
  broadcasts) live in `runtime/pentest_rt.h`, so generated C uses the same
  splats as the runtime.
- **Native int-lane function twins (call-heavy code ~21× faster).** The
  native-int lane previously stopped at the function boundary: a param is a
  runtime-typed `pt_value` and a user call returns one, so a pure-int function
  like `fib` paid the full dynamic machinery per invocation (bound vector,
  local slots, tag dispatch, boxing). A function whose every binding the
  analysis proves int-only — params guaranteed int by the call site, all
  locals in the lane, all returns int-shaped, definite return — is now also
  emitted as `static int64_t pt_fn_<name>_i(int64_t …)` whose body is plain C
  int64 arithmetic; static call sites with all-int-shaped positional
  arguments call the twin, everything else (floats, strings, named args,
  defaults, first-class values) keeps the unchanged dynamic entry point, so
  semantics are bit-identical by construction. `fib(38)`: 15.2s → 0.26s
  (within ~10% of a clang `-O2` C twin; the old comparison ran C at `-O0`).
  A 10M-iteration in-function sum loop: 0.67s → 0.031s. Regression:
  `tests/cases/twin_int_lane.pen` (twin/dynamic parity across every routing).
- The shadow-stack frame and GC protect-stack push/pop
  (`pt_frame_begin/end`, `pt_gc_protect/unprotect`) moved from cross-object
  calls into `static inline` header fast paths (same treatment as
  `pt_guard`/`pt_truthy`); behaviour identical, ~10% off call-heavy code.
- **Text-chain fusion: formatted output builds in one buffer.** A `+`
  chain whose parts are *all provably text* — string literals and `str()`
  calls, nothing else (a bare operand could carry a non-text value, so
  the dynamic path keeps its exact operand-type error) — now compiles to
  one `pt_sb` stack buffer (`runtime/pentest_rt.h`): literals copy
  straight in, proven-int `str()` arguments format straight in via
  `pt_sb_puti` (the display path's digit code — `str(int)`'s exact
  spelling, no intermediate heap text), other `str()` results materialize
  once and append. The value tail (`s = a + b + c`) becomes ONE heap text
  (`pt_sb_finish`) instead of n−1 concatenations; the write tail
  (`f.write(<chain>)`) hands the buffer to `pt_file_write_buf` — one
  `fwrite`, zero heap traffic — with the receiver's checks in the dynamic
  branch's order and a non-file receiver routed through `pt_call_method`
  on the materialized text (every error byte-identical). An
  all-literal/all-int-lane chain is also alloc-free, so the loop
  back-edge safe point elides. Fused chains allocate fewer intermediate
  objects than the unfused form, so a program can only hit an
  out-of-memory condition at different points in its (unchanged) output.
  `io` (600k formatted records × write/close/reopen/read ×4 reps):
  3.95 s → 0.27 s — from 8.4× *behind* hand-written C (`fprintf` per
  record) to 1.3× *ahead of* the fastest C form (one preformatted
  `fwrite`).
- **Whole-file reads fill one registered block with one `fread`.** A
  `read()` on a seekable file sizes itself with `fstat`/`ftell` and reads
  directly into the destination block (no growing raw buffer, no second
  copy, no zeroing pass — the payload is fully written before any read);
  text mode then runs the unchanged validating decode. Where no byte can
  ever need pushing back (no newline stop, no code-point size bound) the
  old per-byte `getc` loop — one call + stdio lock *per byte* — became a
  chunked `fread`; `readline`/`read(size)` keep the precise byte loop,
  on `getc_unlocked`.
- **Instance fields are inline, one allocation per object.** The first
  two attributes of an instance live in the object's own GC block (a
  compact key+value tail beyond `pt_obj`, which stays small for every
  other kind) instead of two extra mallocs (`attrs.e` + `attrs.slots`):
  `pt_getattr`'s common path is a ≤2-entry linear scan whose keys compare
  by storage pointer (merged `.rodata` name literals), with no hash and
  no probe. The inline area is append-only — a delete tombstones and
  never reuses — so `vars()`/`dir()` keep exact insertion order across
  inline+spill; a third field spills into the ordinary table, marked and
  swept exactly as before. `ptrlist` (1M-node traversal): 2.83 s →
  1.50 s (cache-resident instruction cost 367 → ~122 ns/step).
- **Loop-body temp-clear DCE.** Emission is A-normalized — every temp is
  assigned before its next read — so the statement-tail `pt_unset()` is a
  dead store on a loop's back-edge path, and a shadow-frame slot holding
  a stale pointer is only ever *marked* at a collection, never
  swept-then-read. Inside loop bodies the eager clear is therefore
  skipped (§7.3 collection timing unchanged; at most one extra iteration
  of garbage is pinned). Six cross-object calls drop out of every hot
  loop in the language.
- **Cross-tag `==`/`!=` fast path** (`pt_eq_f`/`pt_ne_f`): any
  different-tag pair except the int/float mixture (which compares by
  value) answers inline — `cur != None` in a traversal loop is two tag
  loads and a branch — and `pt_value_eq` gained a same-storage fast path
  for text/bytes (identical buffer pointer + length skips the `memcmp`).
  `pt_gc_maybe` (the loop-back-edge safe point) is likewise a header
  `static inline` threshold check with the cold collector call out of
  line.
- **The speed waves: closing the speedtest gaps.** Eight waves of
  language/core fixes against the 37-case speedtest suite (each case
  has a hand-written C twin; every wave re-verified byte-identical
  output against C), documented in full in
  [docs/performance/optimization.md](docs/performance/optimization.md)
  §14. Highlights: an int-lane **joint fixpoint** (slot lanes and
  int-returning twin callees proven together) plus a native-int
  `switch` lowering put `divmod`/`call_direct`/`branchy` at C parity;
  an **instance-method inline cache** (four guarded resolution, cached
  entries pinned as GC roots) and **C-stack bound vectors** (≤ 64
  slots) took the hash/bound-method/malloc tax off every hot `p.step()`;
  the sweep now **parks dead GC blocks in exact-(size, kind) buckets**
  and `pt_gc_new_full` hands them straight back (no malloc/free round
  trip per cell or list object); text `s = s + chunk` loops concatenate
  **in place** with amortized growth and code-point queries are
  **memoized** per (payload, length, generation) — `text_build` 2.2×;
  a **float lane** (slots proven `PT_FLOAT` run as plain `double`)
  — `float_loop` 2.02×; a **definite-assignment guard elimination**
  drops used-before-assignment `pt_guard`s and used-flag stores the
  proof makes dead — `class_ops` −19%; statically-proven
  **instance-method inlining** (`analyze_method_inlines`/`emit_inline_method`,
  dynamic emission on re-entry) — `class_ops` 271.7 → 148.2 ms (1.83×,
  the C gap ~150× → ~38×); and **text/bytes blocks join the reuse
  pool** (TEXT/BYTES are now swept-parked, re-registered in the tset
  with a bumped generation on reuse, pool capped at 16 MB of parked
  bytes since text payloads are unbounded) — `text_ops` 1.35–1.65×
  (1.346× under `-falign-functions=64`, i.e. a behavioural win, not
  code layout), `sort_text` 1.23×. After all eight: `text_ops` ~15–17×
  from C (was ~24×), `class_ops` ~38× (was ~150×), `text_build` ~2.5×
  faster than the pre-wave compiler; every other case at parity or
  unchanged. Measurement methodology (interleaved min-of-N A/B on a
  pinned CPU, and the ±10% code-layout envelope that makes
  single-digit deltas unattributable) is documented in §9 of the same
  document.

### Fixed
- **Vector lane buffers were invisible to the GC, so arithmetic loops leaked
  them and re-faulted a fresh buffer every statement.** `pt_gc_bytes` only
  counted the ~250 B `pt_obj` header, never the (up to 64 MB) lane buffer, so
  a loop churning large vectors never crossed the 1 MiB collect floor: no
  collection ran, garbage vectors were never swept, their buffers never
  recycled — every statement leaked + `mmap`'d + page-zeroed a fresh 64 MB
  buffer inside the timed loop (~2 GB leaked by the end of a 30-rep run).
  A vector's buffer now counts as registered bytes (added in `pt_vec_alloc`,
  subtracted on sweep), so the collector sees the pressure and sweeps garbage
  vectors at safe points. Combined with the reuse pool (below), this takes
  fused `chain3` from ~1.4× behind hand-fused single-pass C to parity.
- `fvec`/`ivec` values were missing from `pt_gc_mark_value`'s heap-tag
  switch when the types first landed: `collect()` could sweep live vectors
  and the next read observed freed lane buffers (caught by
  `fvec_basics.pen`'s GC section before release).
- **Open files and iterators were swept while still referenced.**
  `PT_FILE` and `PT_ITERATOR` values were missing from
  `pt_gc_mark_value`'s heap-tag switch: neither has members to trace (leaf
  kinds), but their *blocks* still need marking, and the sweep closes a
  file / frees an iterator's source when it reclaims the block. Any
  program that held an open file (or an `iter()` result) across a
  collection lost it — the file handle closed out from under it
  ("I/O operation on closed file"), the iterator's source was freed.
  Small programs never crossed the 1 MiB collect floor, which is why the
  suite never caught it; the hpcbench `io` workload did. Both tags now
  mark like every other heap object, with `tests/cases/gc_roots.pen` as
  the regression.
- **GC use-after-free on heap text/bytes (silent corruption).** `pt_concat`
  and every other string-producing path registered its buffer under the
  *value* tag (`PT_STRING`/`PT_BYTES`) instead of the *GC block* kind
  (`PT_GC_TEXT`/`PT_GC_BYTES`), so the collector's payload set never saw
  the buffer and any collection that ran while a concatenated string was
  still referenced **freed it**. Programs that allocated more than ~1 MiB
  of text (e.g. `s += "x"` loops) hit use-after-free; small programs stayed
  under the GC threshold, which is why the suite never caught it. All
  `pt_gc_buf` sites now pass the proper kind. Regression:
  `tests/cases/gc_text_liveness.pen` (segfaults on the old runtime).
- `print()` built a heap display buffer (malloc/free) per argument; the five
  common scalar cases (int/float/bool/None/text) now format without any
  allocation — ints through an `snprintf`-identical itoa, into a runtime-owned
  256 KB line buffer flushed with a raw `write(1)` (per-line flush on a TTY
  for Python `line_buffering` parity; `atexit` and pre-`input`/pre-crash
  flushes keep output order byte-identical). 1M `print(a)`: 0.21s → 0.069s on
  the reference machine — faster than a C `printf` reference loop. Complex
  types (list/map/…) keep the display-buffer path.
- A second (and every later) named `except` clause after a non-matching first
  one died with `except expects a class, got unset`: the later clauses'
  condition setup was emitted inside the previous clause's body instead of a
  lazily-evaluated `else` block (`Emitter::emit_try` in `src/codegen.cpp`, the
  same pattern `elif` already used). Later clauses are now only evaluated when
  every earlier clause failed. Regression: `tests/cases/try_multi_named.pen`
  (third-clause match, rethrow, bare tail, cond laziness).
- `int("x")` / `float("x")` bypassed `try`/`except` and skipped `finally` — the
  runtime printed to stderr and exited. Both now raise catchable `ValueError`s
  through the normal throw path; uncaught output is byte-identical and
  `finally` runs on the unwind edge. Regression: `bi_py_parse_catch.pen`,
  `rt_bi_parse_finally.pen`.
- `float([])` raised `RuntimeError`; it now raises `TypeError`, matching
  `int([])` and SPEC §6.10's builtin argument-type errors.
- A missing `sha256sum` killed the driver with SIGPIPE (exit 141) mid-compile
  and left an empty `pentest-hash-*` file behind. The cache-key write now
  ignores SIGPIPE, so the write fails with `EPIPE`, caching is silently
  disabled and every run compiles (SPEC §10). CLI check (e) `no_sha256sum` in
  `tests/run_tests.sh`.
- `format()` thousands grouping counted from the left (`format(1234567, ",")`
  → `123,456,7`; correct only when the digit count was a multiple of three),
  and the `,` option was rejected after an explicit width
  (`">12,"` → `Unknown format code ','`). Grouping now counts from the right
  and `,` parses in the grammar position `[width][,][.precision][type]`
  (SPEC §5.11.3). Regression: `bi_py_format_group.pen`, outputs identical to
  Python.
- The editor extension's hover documented `Exception` as the "base catchable"
  class and `RuntimeError` as "catch-all"; both now state exact-class matching
  per SPEC §6.10. Smoke suite: 1230 → 1234 checks.
- `examples/functions.pen` compiled again: its helper `sum` collided with the
  builtin `sum` reserved in `0.2.1`; renamed to `add`.
- `editors/vscode/test/smoke.js`: the snippet-description check now looks for the
  renamed doc (`cross-scope-access`).

### Known issues
See [docs/reference/known-issues.md](docs/reference/known-issues.md): the six
issues found during the documentation pass are fixed (above); two low-severity
corners remain recorded — `format()` zero-padding plus grouping pads the
already-grouped string instead of regrouping a padded digit run, and a run that
cannot cache (no `sha256sum`, unwritable cache) leaves its compiled `program`
in `$TMPDIR` by exec-in-place design.

## [0.2.1] — 2026-10-15 … 2026-10-16
Commits `86deffe`, `c1aa8a7`, `db2d029`.

### Added
- **Optimisation lane**: native-int specialisation (straight-C loops, no-flag
  reads, `for … -> range()` targets in the lane), dynamic numeric fast paths,
  GC safe-point elision for allocation-free loops, run/`rt-*.o` artifact caches,
  pin poisoning.
- **Cross-scope variable access**: `variable.global a`, `variable.parent a`,
  `variable.<function> a` — read, write, compound-assign and destructure bindings
  in other scopes with one shared storage per binding.
- **Builtins**: `input()`, `time()`, `clock()`; and the Python-parity set — 43
  implemented names (`abs` … `zip`, including `open()` with file methods, `iter`/`next`,
  `bytes`, reflection, `format`, `sorted`, `round`, `pow`, `divmod`), nine
  always-failing stubs, and compile-time rejection of `eval`/`exec`/`compile`.
- Exception classes `StopIteration`, `FileNotFoundError`, `OSError` (twelve total).
- VSCode extension: hover provider, builtin/qualifier completions, snippets.

## [0.2.0-m2c] — 2026-10-15
Commits `e506be8` … `dcf1c71`.

### Added
- **Garbage collection**: non-moving mark & sweep with shadow-stack roots and safe
  points; `collect()` builtin. Peak RSS on leak-heavy benchmarks falls from up to
  344 MB to the ~12 MB floor.
- **Exceptions**: `try`/`except`/`finally`/`raise`/`as`; nine builtin classes;
  every runtime error is a catchable instance with its message unchanged; uncaught
  output byte-identical to earlier releases.
- **Classes**: `class` with methods, `__init__`, dynamic attributes, bound methods
  (named arguments supported), `is`/`is not`, and `<class C>` / `<C instance>` /
  `<method 'm' of C>` displays.
- 23 acceptance cases; VSCode extension parity.

## [0.2.0-m2b] — 2026-10-14
Commits `59666cd` … `21e665d`.

### Added
- The VSCode extension (highlighting, completion, snippets, live diagnostics, Run button) first appears in the checkpoint that starts this entry (`59666cd`).
- **Collections**: `list`, `map`, `set`, `tuple` with literals, indexing, slicing,
  index assignment, iteration, membership and Python-style methods.
- **Text and bytes**: code-point semantics for `text`, `b"…"` literals, `encode`/
  `decode`; the type previously called `string` is now `text`.
- Destructuring (including `*rest`) and comma-join.
- **Function overhaul**: defaults (evaluated per call), named arguments,
  `*args`/`**kwargs`, function values, nested functions and closures,
  `global`/`nonlocal`, a uniform function ABI.
- Builtins `len str int float bool type encode decode set`.
- 74 collection and 35 function acceptance cases; VSCode extension parity.

### Changed
- `string` renamed `text` in `type()` and messages (approved change); using a
  function name as a value is now legal (the M1 error was retired).

## Stage 2A — 2026-10-13
Landed inside the first M2 checkpoint (`59666cd`).

### Added
- `if`/`elif`/`else`, `while`, `for … -> …`, `switch`/`case` (with `case *:`),
  `break`, `continue`, `pass`; `**`, `& | ^ ~ << >>`, membership `->`, the
  conditional expression, `and`/`or`/`not`, eleven compound assignments;
  the `range()` builtin.

## [0.1.0-m1] — 2026-10-13
Commit `37c194a`.

### Added
- The strict core: `.pen` scripts compiled through C to native executables; `int`,
  `float`, `bool`, `string`, `None`; arithmetic and comparison; functions, `return`,
  forward references; the special `main():` block and the script-style execution
  model; the four comment forms; `print`; compile-time errors with carets and
  runtime errors with line numbers and exit codes; the `run`/`build`/`check` CLI with
  a content-hash cache; the test harness and benchmark harness.
