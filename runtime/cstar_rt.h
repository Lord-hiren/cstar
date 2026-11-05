/* cstar_rt.h — runtime support library for cstar-generated C11 code.
 *
 * This file is part of the cstar M1 backend (see docs/contracts/m1-semantics.md §10),
 * extended in place for M2 Stage 2A (see docs/contracts/m2a-semantics.md §10) and
 * M2 Stage 2B (see docs/contracts/m2b-semantics.md §13). It is self-contained:
 * generated code only needs
 *
 *     #include "cstar_rt.h"
 *
 * and must be compiled together with cstar_rt.c using only libc + libm
 * (no pthread):
 *
 *     clang -std=c11 -O2 -o <out> gen.c cstar_rt.c -lm
 *
 * Value model (2B §2): unset / None / int (int64_t) / float (double) / bool /
 * text (pointer + length, UTF-8 code points, may contain NUL; the tag is
 * CS_STRING and the type name is now "text") / bytes (pointer + length, raw
 * bytes) / range (int64 start/stop/step, lazy — M2-2A §6), plus the M2-2B
 * heap objects list / map / set / tuple / function (cs_obj* behind a header
 * with a reserved, unused refcount field; 2B leaks them — no reclamation).
 * CS_MISSING is the §13 function-call binder's internal sentinel: it must
 * never escape into user operations (hitting one is an internal error).
 */
#ifndef CSTAR_RT_H
#define CSTAR_RT_H

#include <math.h>   /* lane float helpers (isfinite/floor in inline paths) */
#include <setjmp.h> /* M2-2C §2: cs_handler's jmp_buf */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>  /* FILE: struct cs_obj's file payload (internal ABI) */
#include <stdlib.h> /* realloc: cs_gc_protect fast path (static inline) */
#include <string.h> /* memcpy: cs_gc_protect fast path (static inline) */

#if defined(__GNUC__) || defined(__clang__)
#define CS_UNUSED __attribute__((unused))
#define CS_PRINTF(fmt_idx, first_arg) __attribute__((format(printf, fmt_idx, first_arg)))
#else
#define CS_UNUSED
#define CS_PRINTF(fmt_idx, first_arg)
#endif

#ifdef __cplusplus
#define CS_NORETURN [[noreturn]]
extern "C" {
#else
#define CS_NORETURN _Noreturn
#endif

/* ---- value model ---------------------------------------------------- */

typedef enum cs_tag {
  CS_UNSET = 0, /* also the zero-initializer of static storage (§10) */
  CS_NONE,
  CS_INT,
  CS_FLOAT,
  CS_BOOL,
  CS_STRING, /* M2-2B: this is Text (UTF-8); the type name is "text" (was
              * "string"). Bytes has its own tag below. */
  CS_RANGE, /* M2-2A §6: lazy range value (start/stop/step payload) */
  /* ---- M2-2B additions (docs/contracts/m2b-semantics.md §2) ---- */
  CS_BYTES,    /* raw bytes (pointer + length, may contain NUL) */
  CS_LIST,     /* heap object: dynamic array */
  CS_MAP,      /* heap object: insertion-ordered key/value */
  CS_SET,      /* heap object: insertion-ordered unique elements */
  CS_TUPLE,    /* heap object: fixed ordered sequence */
  CS_FUNCTION, /* heap object: { cs_funmeta*, cs_cell** env } */
  CS_CLASS,    /* M2-2C §2/§3: class value { name, is_exc, methods } —
                * builtin exception classes (is_exc) and user classes */
  CS_EXCEPTION,/* M2-2C §2: exception instance { cls, msg, line } */
  CS_INSTANCE, /* M2-2C §3: user-class instance { cls, attrs } */
  CS_BOUND_METHOD, /* M2-2C §3: bound method { inst, fn } */
  CS_ITERATOR, /* SPEC §5.11.5: boxed iteration state (iter()/next()) */
  CS_FILE,     /* SPEC §5.11.5: open file object (open()) */
  /* ---- SIMD vector types (performance: guaranteed vectorization) ---- */
  CS_FVEC,     /* heap object: contiguous float64 vector (64-byte aligned) */
  CS_IVEC,     /* heap object: contiguous int64 vector (64-byte aligned) */
  CS_MISSING   /* §13 binder sentinel: omitted parameter slot. MUST NEVER
                * escape into user operations; ops hitting it report
                * `internal error: missing argument`. */
} cs_tag;

typedef struct cs_value {
  cs_tag tag;
  union {
    int64_t i;
    double d;
    bool b;
    struct {
      const char* p; /* may point at a static literal or malloc'd bytes */
      size_t len;    /* byte length; interior NULs are legal */
    } s;             /* M2-2B: shared by Text (UTF-8) and Bytes payloads */
    /* M2-2A §6: lazy range payload. The fields are named `start`, `stop`
     * and `step`; generated code reads them directly off the returned
     * cs_range3 value's union, either as `v.as.r.<field>` (named member)
     * or — because the same fields are also exposed through an anonymous
     * member (C11 only) — as `v.as.<field>`. Both spellings name the same
     * storage (identical layout / common initial sequence). */
    struct {
      int64_t start;
      int64_t stop;
      int64_t step;
    } r;
#ifndef __cplusplus
    struct {
      int64_t start;
      int64_t stop;
      int64_t step;
    };
#endif
    /* M2-2B §13: heap objects (list/map/set/tuple/function) are cs_obj*
     * behind a header with a reserved (unused) refcount field. 2B leaks
     * every heap object — no reclamation (scheduled for 2C). */
    struct cs_obj* o;
  } as;
} cs_value;

/* Scope kind for the used-before-assignment guard (§8). Params are locals. */
typedef enum cs_scope {
  CS_SCOPE_GLOBAL = 0,
  CS_SCOPE_LOCAL = 1
} cs_scope;

/* ---- constructors --------------------------------------------------- */
/* Zero-initialized static cs_value storage has tag CS_UNSET (§10). */

static inline cs_value cs_unset(void) {
  cs_value r = {CS_UNSET, {0}};
  return r;
}
static inline cs_value cs_none(void) {
  cs_value r = {CS_NONE, {0}};
  return r;
}
static inline cs_value cs_int(int64_t v) {
  cs_value r = {CS_INT, {0}};
  r.as.i = v;
  return r;
}
static inline cs_value cs_float(double v) {
  cs_value r = {CS_FLOAT, {0}};
  r.as.d = v;
  return r;
}
static inline cs_value cs_bool(bool v) {
  cs_value r = {CS_BOOL, {0}};
  r.as.b = v;
  return r;
}
/* String from raw bytes; `bytes` must outlive the value (literals are
 * static, concatenations are malloc'd and owned by the runtime). */
static inline cs_value cs_str(const char* bytes, size_t len) {
  cs_value r = {CS_STRING, {0}};
  r.as.s.p = bytes;
  r.as.s.len = len;
  return r;
}

/* Use ONLY with a C string literal: computes the byte length, including any
 * interior NUL bytes, at compile time. */
#define CS_STR(literal) cs_str((literal), sizeof(literal) - 1u)

/* M2-2B §5: Bytes from raw bytes; same lifetime rules as cs_str above. */
static inline cs_value cs_bytes_of(const char* bytes, size_t len) {
  cs_value r = {CS_BYTES, {0}};
  r.as.s.p = bytes;
  r.as.s.len = len;
  return r;
}

/* Use ONLY with a C bytes literal (e.g. b"..."): computes the byte length,
 * including any interior NUL bytes, at compile time. */
#define CS_BYTES(literal) cs_bytes_of((literal), sizeof(literal) - 1u)

/* §13 binder sentinel value: placed in the bound vector's omitted
 * default/vararg/kwarg slots. Never let it reach a user operation. */
static inline cs_value cs_missing(void) {
  cs_value r = {CS_MISSING, {0}};
  return r;
}

/* ---- program initialization ----------------------------------------- */

/* Stores the invoked source path (for §8 error prefixes), argv, and the
 * GC root vector (2C §1.3: one pointer per program global, emitted by
 * codegen as `&cs_g_<name>`). Called first thing from generated main();
 * roots may be NULL with nroots == 0. */
void cs_init(int argc, char** argv, const char* src_path,
             cs_value* const* roots, size_t nroots);

/* Accessors for the stored argv (cs_arg(0) == argv[0], ..., cs_arg(argc-1);
 * NULL when i is out of range). */
int cs_arg_count(void);
const char* cs_arg(int i);
/* The source path passed to cs_init (never NULL). */
const char* cs_src_path(void);

/* ---- M2-2C §1: garbage collection ------------------------------------ *
 * Non-moving mark&sweep over a registry of every heap allocation (object,
 * text/bytes buffer, closure cell). Collections run ONLY at the safe
 * points below, where every live value sits in a registered slot or a
 * registered root — never mid-runtime-operation. */

/* Shadow-stack frame: generated functions register the addresses of all
 * their local and temporary slots at entry (2C §1.3(2)), popping at exit
 * (every `return` path included). slots may be NULL with nslots == 0.
 * The frame/protect fast paths are static inline at the end of this
 * header; the state they touch is process-wide (cstar_rt.c). */
typedef struct cs_frame {
  struct cs_frame* prev;
  cs_value** slots;
  size_t nslots;
} cs_frame;

extern cs_frame* cs_gc_frames; /* shadow-stack head */
extern size_t cs_gc_nframes;   /* depth of cs_gc_frames (2C §2.5) */

/* Safe point: collect if the byte threshold has been crossed. Emitted at
 * function entries, loop back-edges, and top-level statement boundaries.
 * The threshold check itself runs INLINE in generated code (the state is
 * the runtime's, exported here exactly like cs_gc_frames): a cross-object
 * call per loop back-edge is pure overhead on allocation-free hot loops,
 * and the cold collector call only happens when the threshold really was
 * crossed. */
extern size_t cs_gc_bytes;     /* registered bytes live */
extern size_t cs_gc_limit;     /* collect at next safe point past this */
void cs_gc_collect_full(void); /* the full collection (cold) */

static inline void cs_gc_maybe(void) {
  if (cs_gc_bytes > cs_gc_limit) cs_gc_collect_full();
}

/* Protects values that live only in a runtime/call-site C local (the
 * callee's `bound` vector across its entry safe point: binder-built
 * vararg/kwarg lists and static-barr fresh values) so a collection cannot
 * sweep them before they are copied into registered slots. Pop with
 * cs_gc_unprotect(n) once the values are reachable from frames. */
extern cs_value* cs_gc_prot; /* protect stack (dynamic) */
extern size_t cs_gc_nprot;
extern size_t cs_gc_prot_cap;

/* Builtin `collect()` (2C §4 test hook): forces a full collection and
 * returns the number of reclaimed registry blocks (int). */
int64_t cs_builtin_collect(void);

/* ---- M2-2C §2: exceptions ------------------------------------------- */

/* One entry of the runtime handler stack (§2.5): the setjmp target, the
 * shadow-frame / protect-stack depths to restore when an exception
 * unwinds to this entry, the pending exception delivered to the landing
 * pad, and the entry kind (CS_H_CATCH with except clauses / CS_H_FINALLY
 * for a finally-only entry). */
typedef struct cs_handler {
  struct cs_handler* prev;
  jmp_buf env;
  size_t frame_depth;  /* cs_gc_nframes at cs_try_enter */
  size_t prot_depth;   /* cs_gc_nprot at cs_try_enter */
  size_t call_depth;   /* pending cs_call bound vectors (§2.5) */
  cs_value exc;        /* set by cs_throw just before the longjmp */
  int kind;
} cs_handler;

#define CS_H_CATCH 0
#define CS_H_FINALLY 1

/* Generated landing-pad protocol (setjmp must live in the generated
 * function — a setjmp'd frame must not have returned when longjmp runs):
 *
 *   cs_handler h;
 *   if (setjmp(h.env) == 0) {
 *     cs_try_enter(&h, CS_H_CATCH);
 *     ... try body ...
 *     cs_try_leave(&h);          // normal completion
 *   } else {
 *     cs_value e = h.exc;        // FIRST: copy before any call/safe point
 *     ... except matching ...    // no match: cs_rethrow(e)
 *   }
 *
 * cs_try_enter pushes the entry, recording the current depths. On an
 * empty stack cs_throw prints the uncaught exception in the 2B byte
 * format (`<src>:<line>: runtime error: <msg>`) and exits(1). */
void cs_try_enter(cs_handler* h, int kind);
void cs_try_leave(cs_handler* h);

/* Throw, or continue unwinding with, an exception instance (§2.3/§2.5). */
CS_NORETURN void cs_throw(cs_value exc);
CS_NORETURN void cs_rethrow(cs_value exc);

/* Validate per §2.3 and throw: an exception instance rethrows as-is, a
 * bare class constructs an empty-message instance; anything else raises
 * the TypeError `exceptions must be class or instance, got <t>`. */
CS_NORETURN void cs_raise(cs_value v, int line);

/* cs_runtime_error with a mapped builtin class (§2.2/§2.6 — used by
 * cs_guard from this header and by every reclassified error site).
 * Uncaught output is the same 2B byte format either way. An unknown
 * cls_name falls back to RuntimeError. */
CS_NORETURN void cs_runtime_error_as(const char* cls_name, int line,
                                     const char* fmt, ...);

/* Builtin exception classes (§2.2): singleton CS_CLASS values created by
 * cs_init. `cs_exc_class("ValueError")` returns the singleton for a known
 * name (unknown names return None — only codegen-emitted names are
 * looked up). */
cs_value cs_exc_class(const char* name);

/* Class construction through the dynamic call binder (cs_call, §2.2):
 * an exception instance of `cls` whose message is arg0 as text (displayed
 * when not already text; no arg = empty message), raised at `line`.
 * argc > 1 is a runtime TypeError. */
cs_value cs_exc_construct(cs_value cls, const cs_value* args, size_t argc,
                          int line);

/* §2.4 exact-identity matching (no inheritance): the pending exception's
 * class *is* `cls`. A non-class `cls` is a runtime TypeError
 * (`except expects a class, got <t>`). */
bool cs_exc_matches(cs_value pending, cs_value cls, int line);

/* ---- M2-2C §3: classes, instances, attributes, identity ---------------- */

/* A fresh user class named `name` (static string — never freed) with an
 * empty method table. Builtin exception classes are NOT created here (they
 * come from cs_init with is_exc set). */
cs_value cs_class_new(const char* name, int line);

/* Registers `fn` (a CS_FUNCTION value) under `name` in the class's method
 * table (duplicate names are a codegen invariant, not checked here). */
cs_value cs_class_add_method(cs_value cls, const char* name, cs_value fn,
                             int line);

/* Attribute read (§3.2): instance fields, then (exceptions) the builtin
 * `.type`/`.message` text fields, then methods (yielding a bound method);
 * anything else raises AttributeError `'<t>' object has no attribute
 * '<name>'`. */
cs_value cs_getattr(cs_value obj, const char* name, int line);

/* Attribute write (§3.2): instance fields only — any other receiver is a
 * runtime TypeError (`cannot assign attribute '<name>' on '<t>'`). */
void cs_setattr(cs_value obj, const char* name, cs_value val, int line);

/* `a is b` (§3.4): same heap object; text/bytes identical iff they share
 * storage; non-heap values (int/float/bool/none/range) identical iff same
 * tag and equal by value. */
bool cs_identical(cs_value a, cs_value b, int line);

/* ---- used-before-assignment guard (§6/§8) ---------------------------- */

/* cs_guard — static inline definition at the end of this header (hot path:
 * one call per guarded variable read in generated code; unset reads report
 * the global/local 'used before assignment' runtime error and exit(1)). */

/* ---- operators (§4) -------------------------------------------------- */
/* Every op that can fail takes the cstar source line of its AST node.
 * Semantics are normative; error messages are normative (§8).
 * int arithmetic wraps (two's complement, defined via uint64_t);
 * int `/` truncates toward zero; `%` is ints only (C sign rules);
 * `==`/`!=` are total and therefore take no line. */

cs_value cs_add(cs_value a, cs_value b, int line);
cs_value cs_sub(cs_value a, cs_value b, int line);
cs_value cs_mul(cs_value a, cs_value b, int line);
cs_value cs_div(cs_value a, cs_value b, int line);
cs_value cs_mod(cs_value a, cs_value b, int line);
cs_value cs_neg(cs_value a, int line);
cs_value cs_lt(cs_value a, cs_value b, int line);
cs_value cs_le(cs_value a, cs_value b, int line);
cs_value cs_gt(cs_value a, cs_value b, int line);
cs_value cs_ge(cs_value a, cs_value b, int line);
cs_value cs_eq(cs_value a, cs_value b);
cs_value cs_ne(cs_value a, cs_value b);

/* ---- amortized text concatenation (perf: `s = s + chunk` loops) --------- *
 * cs_concat always allocates an exact-size block and copies both parts —
 * correct, but O(n^2) copy volume for a build-up loop. cs_text_cat2 is the
 * capacity-aware form for codegen-PROVEN `name = name + expr` sites (the
 * proof — uniqueness/escape, heap-ness of the incoming value, no stale
 * alias, GC safety — lives in codegen.cpp's analyze_text_cats; any doubt
 * means the site gets may_grow=0):
 *   may_grow == 0  exactly cs_add's behavior (text/bytes -> cs_concat);
 *   may_grow == 1  the runtime APPENDS IN PLACE while `a` is a heap-owned
 *                  growable block with spare capacity (same pointer back)
 *                  and otherwise allocates a fresh block with DOUBLED
 *                  capacity — total copy volume O(final length), results
 *                  byte-identical to cs_concat. Non-text operands fall
 *                  back to cs_add (identical errors/lines). */
cs_value cs_text_cat2(cs_value a, cs_value b, int line, int may_grow);

/* ---- M2-2A additions (docs/contracts/m2a-semantics.md §3-§6, §10) ------------- */
/* Same discipline as the M1 operators above: every failing call reports the
 * cstar source line of its AST node via cs_runtime_error and exits(1);
 * int bit arithmetic wraps (uint64_t casts); type errors precede value
 * checks (M1 §8 ordering). */

/* range(start, stop, step) — validates the arguments (all must be int:
 * bool is NOT int in cstar; offending argument reported left-to-right,
 * M2-2A §9) and rejects step == 0, then returns the lazy range value.
 * Read start/stop/step directly off the returned value's union payload
 * (fields `start`, `stop`, `step` — see the cs_value declaration above).
 * Arity is checked at compile time (M2-2A §8). */
cs_value cs_range3(cs_value start, cs_value stop, cs_value step, int line);

/* cs_truthy — static inline definition at the end of this header (hot path:
 * every loop condition / boolean operator; §4 total truthiness — falsy =
 * None / int 0 / float 0.0 / empty text/bytes/list/map/set/tuple/range,
 * functions truthy; unset there would be a compiler bug). */

/* §3.1: any float operand → double via C pow(); int ** int with a
 * non-negative exponent → wrapping int64 (modular exponentiation);
 * int ** negative int → runtime error; non-numeric operand →
 * `unsupported operand types for **: ...` (type check first, then sign). */
cs_value cs_pow(cs_value a, cs_value b, int line);

/* §3.2: ints only (no bool/float/None/string coercion); wrapping bit ops,
 * `~a` == -a-1, `>>` arithmetic; a shift count outside 0..63 is a runtime
 * error (operand types checked first). */
cs_value cs_band(cs_value a, cs_value b, int line);
cs_value cs_bor(cs_value a, cs_value b, int line);
cs_value cs_bxor(cs_value a, cs_value b, int line);
cs_value cs_shl(cs_value a, cs_value b, int line);
cs_value cs_shr(cs_value a, cs_value b, int line);
cs_value cs_bnot(cs_value a, int line);

/* §3.4 membership `item -> container` → bool. Range membership is exact
 * for any int64 bounds (no overflow; stop is exclusive); string
 * containment is a byte-substring search (interior NULs respected, empty
 * needle → True). Any invalid operand pairing is a §9 type error. */
bool cs_contains(cs_value item, cs_value container, int line);

/* ---- iteration protocol (M2-2A §5/§6, §10; M2-2B §4 extension) --------- */
/* Kinds: range (successive int64 per §6; iteration stops before the next
 * index would leave int64 range — terminates for every valid range); text
 * (CS_ITER_STRING: successive 1-code-point Text values pointing into the
 * original bytes — no copying; M2-2B §5: code points, never split); bytes
 * (successive int byte values); list/tuple/set (successive elements);
 * map (successive keys, insertion order). The struct is fully defined
 * because generated code declares cs_iter locals; `kind` is a
 * cs_iter_kind (zero-initialized == exhausted). */

typedef enum cs_iter_kind {
  CS_ITER_DONE = 0, /* exhausted / not open; also the zero-initializer */
  CS_ITER_RANGE,
  CS_ITER_STRING, /* Text: 1-code-point Text values (M2-2B §5) */
  CS_ITER_BYTES,  /* M2-2B: int byte values */
  CS_ITER_LIST,   /* M2-2B: elements */
  CS_ITER_TUPLE,  /* M2-2B: elements */
  CS_ITER_SET,    /* M2-2B: elements (insertion order) */
  CS_ITER_MAP,    /* M2-2B: keys (insertion order) */
  CS_ITER_BOXED,  /* SPEC §5.11.5: `src` is a CS_ITERATOR object; next()
                    * advances the INNER state so `for x -> it` and
                    * next(it) share one cursor (Python semantics) */
  CS_ITER_FILE,   /* SPEC §5.11.5: `src` is a CS_FILE; yields one line
                    * (text, or bytes in binary mode) per next() */
  CS_ITER_FVEC,   /* fvec: successive float64 elements */
  CS_ITER_IVEC    /* ivec: successive int64 elements */
} cs_iter_kind;

typedef struct cs_iter {
  int kind; /* cs_iter_kind */
  /* CS_ITER_RANGE state: next index to emit, exclusive bound, nonzero step */
  int64_t cur;
  int64_t stop;
  int64_t step;
  /* CS_ITER_STRING/CS_ITER_BYTES state: cursor into the source bytes
   * (not copied) */
  const char* p;
  size_t len;
  size_t pos;
  /* M2-2B: sequence/map/set iteration state (src holds a reference to the
   * iterated heap object; idx is the entry cursor). */
  cs_value src;
  size_t idx;
} cs_iter;

/* Opens an iterator over a range, text, bytes, list, tuple, set or map
 * value; any other type → runtime error `'<t>' object is not iterable`. */
cs_iter cs_iter_open(cs_value v, int line);
/* Produces the next element; returns false at exhaustion (immediately for
 * empty inputs). `*out` is written only when true is returned. */
bool cs_iter_next(cs_iter* it, cs_value* out);

/* ---- print (§4 formatting, normative) -------------------------------- */

/* Prints n args (any n >= 0, NULL allowed when n == 0) separated by a single
 * space, terminated by '\n'. 0 args prints just the newline. float uses the
 * shortest round-trip representation; range prints as
 * `range(<start>, <stop>, <step>)` (all three numbers always, M2-2A §6);
 * `line` is the Call node's line. */
void cs_print(const cs_value* args, size_t n, int line);

/* ---- misc helpers ---------------------------------------------------- */

/* Normative type name: "unset" "None" "int" "float" "bool" "text" (the
 * CS_STRING tag is Text; M2-2B approved change — was "string") "bytes"
 * "range" "list" "map" "set" "tuple" "function". */
const char* cs_type_name(cs_value v);

/* ---- M2 Stage 2B additions (docs/contracts/m2b-semantics.md §2, §4-§9, §12,
 * §13, §15). Same discipline as above: every failing call reports the
 * cstar source line of its AST node via cs_runtime_error and exits(1).
 * All M1/2A behavior and wording is preserved; the only changes to
 * pre-existing ops are the approved 2B ones (CS_STRING is Text: type name
 * "text", code-point indexing/iteration) and the 2B truthiness/equality/
 * membership/display extensions for the new types.
 * -------------------------------------------------------------------- */

/* ---- heap cells and function values (§13 normative ABI) --------------- */

/* Captured-variable cell (§13): `{ cs_value v; }`, heap. A fresh cell is
 * initialized to unset so the M1 used-before-assignment guard applies. */
typedef struct cs_cell {
  cs_value v;
} cs_cell;

/* Heap object behind CS_LIST/CS_MAP/CS_SET/CS_TUPLE/CS_FUNCTION values
 * (§13): header with a reserved refcount field. Its payload layout is
 * runtime-private — generated code must go through the cs_* API. */
typedef struct cs_obj cs_obj;

cs_cell* cs_cell_alloc(void);

/* Immortal cell for a cross-scope "pinned" binding that is also captured
 * by a closure (docs/reference/cross-scope-access.md §4): a regular cell registered in
 * the collector's pinned-cell table, so it is rooted at every collection
 * and lives for the whole program. Created once per pinned binding, from
 * the generated program's entry — before any closure sharing it is hoisted. */
cs_cell* cs_cell_pinned(void);

/* Closure environment vector (§13): heap array of `n` cell slots,
 * zero-filled; generated code fills every slot immediately after creation.
 * A closure's env outlives the frame that created it (2B leaks by design). */
cs_cell** cs_env_new(size_t n);

/* Uniform function ABI (§13): every generated function has the signature
 * `cs_value cs_fn_x(cs_value* bound, cs_cell** env)`; top-level functions
 * are called with env = NULL. `bound` is a vector of n_params values; an
 * omitted parameter with a default arrives as CS_MISSING and is evaluated
 * by the callee's entry code, in parameter order (a default may reference
 * globals and earlier bound params). */
typedef cs_value (*cs_fn_abi)(cs_value* bound, cs_cell** env);

/* Per-function metadata carried by every function value (§13). Generated
 * code emits one static const instance per function declaration. Layout:
 *   name         display name (used in runtime binding errors)
 *   param_names  n_params entries: positional params, then the vararg
 *                name (when has_vararg), then the kwarg name (when
 *                has_kwarg)
 *   n_params     total slots in the bound vector (<= 64 for has_default)
 *   n_required   leading positional params with no default (vararg and
 *                kwarg slots never count as required)
 *   has_default  bitmask: bit i set when param i has a default expression
 *   has_vararg   extra positionals collect into a list in that slot
 *   has_kwarg    extra named arguments collect into a map in that slot
 *   fn           the uniform ABI entry point */
typedef struct cs_funmeta {
  const char* name;
  const char* const* param_names;
  size_t n_params;
  size_t n_required;
  uint64_t has_default;
  bool has_vararg;
  bool has_kwarg;
  cs_fn_abi fn;
} cs_funmeta;

/* Builds a function value ({meta, env}, tagged function) — §13.
 * cs_fun_new allocates the env storage itself (inlined into the object's
 * block for env_n <= 4) and returns the value ready to fill through
 * cs_fun_env — the preferred closure constructor (no temporary array).
 * cs_fun_value adopts a caller-built env array (cs_env_new): it copies it
 * into the block's inlined tail and frees the temporary. */
cs_value cs_fun_new(const cs_funmeta* meta, size_t env_n);
cs_value cs_fun_value(const cs_funmeta* meta, cs_cell** env, size_t env_n);
/* Accessors for the function value's halves (never NULL for a valid
 * function value). */
const cs_funmeta* cs_fun_meta(cs_value v);
cs_cell** cs_fun_env(cs_value v);

/* §13 dynamic call: binds and invokes fnv (tagged function; anything else
 * → `object of type '<t>' is not callable`). `posvals` holds npos + nnamed
 * values: the first npos are positional, and named argument i has name
 * argnames[i] and value posvals[npos + i]. Binding is positional first,
 * then by name; the §11 binding residue (unexpected name, duplicate
 * binding, missing required, too many positionals) is raised here at
 * runtime with the exact §11 wording. Omitted defaults stay CS_MISSING in
 * the bound vector (the callee evaluates them); vararg/kwarg slots are
 * filled with a list/map only when extra arguments exist (otherwise they
 * arrive as CS_MISSING and the callee initializes them empty). */
cs_value cs_call(cs_value fnv, const cs_value* posvals, size_t npos,
                 const char* const* argnames, size_t nnamed, int line);

/* §13/§15 method dispatch: obj.name(args...). Named arguments in a method
 * call → `method arguments must be positional`. Unknown method →
 * `'<t>' object has no method '<m>'`. Mutating methods return None. */
cs_value cs_call_method(cs_value obj, const char* name, const cs_value* args,
                        size_t nargs, const char* const* argnames,
                        size_t nnamed, int line);

/* ---- constructors (§2/§4) -------------------------------------------- */

cs_value cs_list_new(void);          /* empty list */
cs_value cs_list_of(const cs_value* items, size_t n);
cs_value cs_tuple_of(const cs_value* items, size_t n);
cs_value cs_map_new(void);           /* empty map */
/* n key/value pairs in source order; duplicate keys (total `==`) replace
 * the value but keep the first insertion position/key form. Unhashable key
 * → `unhashable type: '<t>'`. */
cs_value cs_map_of(const cs_value* keys, const cs_value* vals, size_t n,
                   int line);
cs_value cs_set_new(void);           /* empty set — the only empty-set form */
cs_value cs_set_of(const cs_value* items, size_t n, int line);

/* ---- length, indexing, slicing (§4/§5/§8) ----------------------------- */

/* §8 len(): text (code points), bytes, list, map, set, tuple; anything
 * else → `object of type '<t>' has no length()`. */
size_t cs_len(cs_value v, int line);
/* §4: negative indices count from the end. Non-int index on a sequence →
 * `<container-type> indices must be integers, not <t>`; out of range →
 * `<container-type> index out of range`; text index yields 1-code-point
 * Text, bytes index yields int; map key missing → `map key not found`;
 * non-subscriptable → `'<t>' object is not subscriptable`. */
cs_value cs_index_get(cs_value container, cs_value idx, int line);
/* §4/§12: list and map only. text/bytes/tuple →
 * `'text' does not support item assignment` (also bytes/tuple); other
 * types → `'<t>' object does not support item assignment`. */
void cs_index_set(cs_value container, cs_value idx, cs_value val, int line);
/* §4 slicing with full Python normalization on text/bytes/list/tuple,
 * returning a NEW object. An absent (open) bound is passed as CS_UNSET.
 * Non-int bound → `slice indices must be integers, not <t>`; zero step →
 * `slice step cannot be zero`; other types →
 * `'<t>' object does not support slicing`. Text slices operate on code
 * points and never split a code point. */
cs_value cs_slice(cs_value container, cs_value start, cs_value stop,
                  cs_value step, int line);

/* Bounds-only slice normalization: performs exactly cs_slice's container and
 * bound validation, then reports the selected (first, step, count) instead of
 * materializing the result. Used by the lazy-slice lowering so `s = xs[a:b]`
 * can keep the snapshot semantics proveable without a heap object. */
void cs_slice_bounds(cs_value container, cs_value start, cs_value stop,
                     cs_value step, int64_t* out_first, int64_t* out_step,
                     size_t* out_count, int line);

/* Slow/error paths of the lazy-slice element reads (the inline fast paths
 * live next to cs_seq_at below). */
cs_value cs_slice_get_slow(cs_value container, int64_t first, int64_t step,
                           size_t count, cs_value idx, int line);
int64_t cs_slice_iget_slow(cs_value container, int64_t first, int64_t step,
                           size_t count, int64_t idx, int line);
/* Cold out-of-range path: needs only the slice length, the index, the type
 * name, and the line (never first/step), so the hot inline reads keep their
 * normalized bounds in registers across the potential error edge. */
CS_NORETURN int64_t cs_slice_oob_named(const char* cname, size_t count,
                                       int64_t idx, int line);

/* ---- destructuring (§12) ---------------------------------------------- */

/* Element count of a destructure source: list/tuple length, text code
 * points, bytes length, map key count; non-iterable →
 * `'<t>' object is not iterable`. */
size_t cs_unpack_len(cs_value v, int line);
/* i'th element of a destructure source (text → 1-code-point Text, bytes →
 * int, map → key). i must be < cs_unpack_len(v) — internal error
 * otherwise (codegen contract). */
cs_value cs_unpack_get(cs_value v, size_t i, int line);
/* Elements [start, stop) of a destructure source as a fresh list (rest
 * target `*name` in a pattern with L leaves: the caller passes start = the
 * rest leaf's position and stop = element count minus the number of fixed
 * leaves after it, so a middle `a, *rest, b` spans exactly the middle;
 * a trailing rest passes stop = cs_unpack_len's result). */
cs_value cs_unpack_rest(cs_value v, size_t start, size_t stop, int line);
/* §12 count check for a target list of `expected` names given `got`
 * elements: `not enough values to unpack (expected N, got M)` /
 * `too many values to unpack (expected N, got M)`. */
void cs_unpack_check(size_t got, size_t expected, int line);

/* ---- comma-join (§7) -------------------------------------------------- */

/* Joins the display forms (§2) of n values with a single space into Text
 * (the unparenthesized-comma join of assignment RHS / return /
 * expression-statement positions). */
cs_value cs_join(const cs_value* args, size_t n, int line);

/* ---- builtins (§8) ---------------------------------------------------- */
/* Each is lowered as a direct runtime call (call-target-only names). */

cs_value cs_builtin_len(cs_value v, int line);
/* ---- fvec/ivec constructors (SIMD vector types) ----------------------- */
cs_value cs_builtin_fvec(cs_value items, int line);   /* list/tuple/range/
                                                       * fvec/ivec → fvec */
cs_value cs_builtin_ivec(cs_value items, int line);   /* int elements only */
cs_value cs_builtin_fvec_fill(cs_value n, cs_value v, int line);
cs_value cs_builtin_ivec_fill(cs_value n, cs_value v, int line);
cs_value cs_builtin_dot(cs_value a, cs_value b, int line); /* inner product */
cs_value cs_builtin_str(cs_value v, int line);   /* §2 display form as Text */
cs_value cs_builtin_int(cs_value v, int line);   /* text/bytes decimal parse
                                                  * (invalid → `invalid
                                                  * literal for int(): '<s>'`),
                                                  * float truncation, bool */
cs_value cs_builtin_float(cs_value v, int line); /* invalid → `invalid
                                                  * literal for float(): '<s>'` */
cs_value cs_builtin_bool(cs_value v, int line);
cs_value cs_builtin_type(cs_value v, int line);  /* type name as Text; note
                                                  * None renders "none" here
                                                  * (§8 list) while runtime
                                                  * error messages keep
                                                  * "None" (§12) */
cs_value cs_builtin_encode(cs_value v, int line); /* text → UTF-8 bytes */
cs_value cs_builtin_decode(cs_value v, int line); /* bytes → validating UTF-8
                                                   * text; invalid →
                                                   * `invalid UTF-8 in
                                                   * decode()` */
cs_value cs_builtin_set(cs_value* args, size_t n, int line); /* 0 or 1
                                                  * args; 1 iterable arg
                                                  * (map → keys) */

/* Post-2C basics: line read from stdin (None at end of input; optional
 * prompt printed+flushed first), epoch seconds, monotonic seconds. */
cs_value cs_builtin_input(cs_value prompt, int has_prompt, int line);
double cs_builtin_time(void);
double cs_builtin_clock(void);

/* ---- Python-parity builtins (SPEC §5.11.5) ---------------------------- *
 * One direct call per builtin — no dispatch, no lookup. Optional slots
 * arrive as CS_MISSING and are defaulted inside. Every implementation is
 * heap-free unless it builds a value (scalar paths allocate nothing). */

/* Reflection name-slot table (globals/locals/vars/dir): codegen emits one
 * entry per live global/local ONLY into programs that call these — an
 * unreferenced program carries zero table bytes, and nothing syncs at
 * runtime (the walk happens at call time over storage pointers).
 * kind: 0 = boxed cs_value* (CS_UNSET = not yet assigned → omitted),
 *       1 = native int64_t* with an assigned-flag byte (flag NULL =
 *           definitely assigned, always included),
 *       3 = native double* with an assigned-flag byte (the float lane;
 *           same flag rules as kind 1). */
typedef struct cs_name_slot {
  const char* name;
  signed char kind;
  void* ptr;
  unsigned char* flag;
} cs_name_slot;

cs_value cs_builtin_globals(const cs_name_slot* tbl, size_t n, int line);
cs_value cs_builtin_locals(const cs_name_slot* tbl, size_t n, int line);
/* vars()/dir(): obj present → instance fields (vars) / sorted field+method
 * names (dir); obj missing → the frame table (locals) / the globals table. */
cs_value cs_builtin_vars(cs_value obj, const cs_name_slot* tbl, size_t n,
                         int line);
cs_value cs_builtin_dir(cs_value obj, const cs_name_slot* tbl, size_t n,
                        int line);

/* Numeric/text/collection (see SPEC §5.11.5 for the Python-parity rules
 * and the documented deviations: int64 wrapping, C99 divmod, int-only
 * modulo, UTF-8-only files, lazy-skip of eval/exec/compile). */
cs_value cs_builtin_abs(cs_value x, int line);
cs_value cs_builtin_all(cs_value iterable, int line);
cs_value cs_builtin_any(cs_value iterable, int line);
cs_value cs_builtin_ascii(cs_value x, int line);
cs_value cs_builtin_bin(cs_value x, int line);
cs_value cs_builtin_bytes(cs_value source, cs_value encoding, cs_value errors,
                          int line);
cs_value cs_builtin_callable(cs_value obj, int line);
cs_value cs_builtin_chr(cs_value i, int line);
cs_value cs_builtin_delattr(cs_value obj, cs_value name, int line);
cs_value cs_builtin_dict(const cs_value* pos, size_t npos,
                         const cs_value* named_vals,
                         const char* const* named_names, size_t nnamed,
                         int line);
cs_value cs_builtin_divmod(cs_value a, cs_value b, int line);
cs_value cs_builtin_enumerate(cs_value iterable, cs_value start, int line);
cs_value cs_builtin_filter(cs_value fn, cs_value iterable, int line);
cs_value cs_builtin_format(cs_value v, cs_value spec, int line);
cs_value cs_builtin_getattr(cs_value obj, cs_value name, cs_value dflt,
                            int line);
cs_value cs_builtin_hasattr(cs_value obj, cs_value name, int line);
cs_value cs_builtin_hash(cs_value obj, int line);
cs_value cs_builtin_hex(cs_value x, int line);
cs_value cs_builtin_id(cs_value obj, int line);
cs_value cs_builtin_isinstance(cs_value obj, cs_value classinfo, int line);
cs_value cs_builtin_iter(cs_value obj, int line);
cs_value cs_builtin_list(cs_value iterable, int line);
cs_value cs_builtin_map(const cs_value* pos, size_t npos, int line);
cs_value cs_builtin_max(const cs_value* pos, size_t npos, cs_value key,
                        cs_value dflt, int line);
cs_value cs_builtin_min(const cs_value* pos, size_t npos, cs_value key,
                        cs_value dflt, int line);
cs_value cs_builtin_next(cs_value obj, cs_value dflt, int line);
cs_value cs_builtin_oct(cs_value x, int line);
cs_value cs_builtin_open(cs_value file, cs_value mode, cs_value encoding,
                         int line);
cs_value cs_builtin_ord(cs_value c, int line);
cs_value cs_builtin_pow(cs_value base, cs_value exp, cs_value mod, int line);
cs_value cs_builtin_repr(cs_value v, int line);
cs_value cs_builtin_reversed(cs_value sequence, int line);
cs_value cs_builtin_round(cs_value number, cs_value ndigits, int line);
cs_value cs_builtin_setattr(cs_value obj, cs_value name, cs_value value,
                            int line);
cs_value cs_builtin_slice(cs_value start, cs_value stop, cs_value step,
                          int line);
cs_value cs_builtin_sorted(cs_value iterable, cs_value key, cs_value reverse,
                           int line);
cs_value cs_builtin_sum(cs_value iterable, cs_value start, int line);
cs_value cs_builtin_tuple(cs_value iterable, int line);
cs_value cs_builtin_unsupported(const char* name, int line);
/* HTTP builtins (libcurl, persistent keep-alive handle):
 *   cs_builtin_http(url, body, headers)      — POST; headers = text of
 *     newline-separated "Name: value" lines, or None for JSON defaults.
 *   cs_builtin_http_get(url, headers)         — GET; headers likewise.
 * Both return "STATUS\nBODY" as Text (status 0 + curl error string when the
 * connection fails before any byte arrives). */
cs_value cs_builtin_http(cs_value url, cs_value body, cs_value headers,
                         int line);
cs_value cs_builtin_http_get(cs_value url, cs_value headers, int line);
/* url_encode(text): percent-encode for query strings / form bodies. */
cs_value cs_builtin_url_encode(cs_value text, int line);
cs_value cs_builtin_zip(const cs_value* pos, size_t npos, int line);

/* Attribute deletion (SPEC §5.11.5 delattr): removes a dynamic instance
 * field; a missing field raises AttributeError with §3.2's exact wording,
 * any non-instance receiver is an AttributeError like a failed read. */
void cs_delattr(cs_value obj, const char* name, int line);

/* Prints `<src_path>:<line>: runtime error: <formatted message>` to stderr
 * and exits with status 1 (§8). printf-style formatting. */
CS_NORETURN void cs_runtime_error(int line, const char* fmt, ...)
    CS_PRINTF(2, 3);

/* ---- internal object ABI (shared with generated code) ------------------
 * struct cs_obj, the field table it embeds and the inline-attribute tail
 * live here (they were runtime-private in cstar_rt.c) so the code
 * generator's literal-name attribute fast path — cs_getattr_lit /
 * cs_setattr_lit below — can scan an instance's inline tail directly.
 * Everything else stays runtime-private: generated code goes through the
 * cs_* API, and the two _lit helpers fall back to the full functions on
 * any miss (unknown receiver kind, spilled field, method lookup, error).
 *
 * Internals below — generated code must not touch them except through
 * the _lit helpers. */
typedef struct cs_entry {
  cs_value key;
  uint64_t hash;
  bool used; /* false = tombstone (keeps insertion order stable) */
} cs_entry; /* 48 B: the value MOVED OUT (§4) — maps/classes keep it in
             * cs_table.vals (same index), sets never carry one at all; a
             * 200 k-key table's probe footprint drops 12 -> 7.2 MB. */

typedef struct cs_table {
  cs_entry* e;       /* append-only entry array (insertion order) */
  size_t len;        /* entries used (incl. tombstones) */
  size_t cap;
  size_t* slots;     /* open-addressed index table: 0 empty, 1 deleted,
                      * entry index + 2 present */
  size_t slot_cap;   /* power of two */
  size_t tombstones; /* deleted slots in `e` */
  size_t live;       /* entries with used == true */
  size_t first;      /* smallest index with used == true while live > 0
                      * (maintained cursor — see cs_table_first) */
  cs_value* vals;    /* parallel VALUES array (entry index == this index)
                      * — allocated iff has_vals; a SET's is never
                      * allocated and never read (§4) */
  bool has_vals;     /* set by the owning constructor: map / class
                      * methods / instance attrs = true, set = false */
} cs_table;

struct cs_seqbuf; /* shared slice backing store (runtime-private) */

struct cs_obj {
  int tag; /* CS_LIST | CS_MAP | CS_SET | CS_TUPLE | CS_FUNCTION |
            * CS_CLASS | CS_EXCEPTION (M2-2C §2/§3) */
  union {
    struct {
      cs_value* items; /* first selected element (views may point inside
                        * buf->data, strided by `stride`) */
      size_t len;
      size_t cap;   /* in elements; a view's cap == len (pushes detach) */
      struct cs_seqbuf* buf; /* NULL = owns a plain malloc'd items array;
                              * non-NULL = refcounted shared slice buffer */
      ptrdiff_t stride; /* 0 = contiguous (+1); views can be negative or
                         * larger (xs[a:b:k] shares storage O(1)) */
    } seq; /* list (mutable) and tuple (immutable; cap unused) */
    struct {
      size_t len;  /* element count (the allocation is rounded up to the
                    * 64-byte lane multiple; len is the true count) */
      void* data;  /* 64-byte-aligned element buffer, zero-initialized */
    } vec; /* CS_FVEC (double) / CS_IVEC (int64_t) */
    cs_table table; /* map and set */
    struct {
      const cs_funmeta* meta;
      cs_cell** env;
      size_t env_n; /* cells in env (traced by the GC) */
    } fun; /* function value { meta, env } — §13 */
    struct {
      const char* name; /* static — never freed */
      bool is_exc;      /* builtin exception class (§2.2) vs user (§3) */
      cs_table methods; /* user classes: name → CS_FUNCTION value */
    } cls; /* CS_CLASS */
    struct {
      cs_value cls; /* the CS_CLASS value (§2.2) */
      cs_value msg; /* CS_STRING message text (GC-owned or static) */
      int line;     /* raise/construct site (§2.2 — uncaught reports it) */
    } exc; /* CS_EXCEPTION instance */
    struct {
      cs_value cls;   /* the CS_CLASS value (§3) */
      cs_table attrs; /* spilled dynamic fields (§3.2) — NULL-backed while
                       * the inline area below still suffices */
      uint32_t n_inl; /* inline entries used (append-only high-water: a
                       * delete marks used=false but never reuses the slot,
                       * so vars()/dir() keep exact insertion order across
                       * inline+spill) */
    } inst; /* CS_INSTANCE */
    struct {
      cs_value inst; /* the bound receiver (§3.2) */
      cs_value fn;   /* the CS_FUNCTION behind the method */
    } bnd; /* CS_BOUND_METHOD */
    struct {
      cs_iter* it; /* malloc'd iteration state (freed on sweep — only a
                    * program that calls iter() ever allocates one; the
                    * pointer keeps every OTHER object's size unchanged) */
    } itr; /* CS_ITERATOR — boxed iter()/next() state (SPEC §5.11.5) */
    struct {
      FILE* f;          /* stdio handle; NULL once closed */
      bool binary;      /* "b" mode: reads/writes stay bytes */
      bool writable;    /* "w"/"a" modes: write() is allowed */
      char* path;       /* raw C copy (the caller's text may be collected;
                         * kept past close() — f.name still reads, like
                         * Python — and freed only by the sweep) */
      struct cs_obj* next_open; /* open-file registry link (SPEC §5.11.5) */
    } file; /* CS_FILE */
  } as;
};

/* Inline instance fields (perf: pointer-chasing objects). The first
 * CS_INST_INLINE attributes live in the instance's OWN GC block, in a tail
 * sized at allocation (cs_obj_new_tag): one allocation and one dependent
 * cache miss per attribute read instead of three mallocs (obj + attrs.e +
 * attrs.slots) and as many serialized misses per traversal step. cs_obj
 * itself stays small for every OTHER kind — only CS_INSTANCE blocks carry
 * the tail. A class with more fields spills the rest into attrs (the
 * ordinary table) exactly as before.
 *
 * The inline slots use a COMPACT entry (key + value only — 64B instead of
 * cs_entry's 80B: no hash, no used flag; a zeroed key.tag == CS_UNSET is
 * the tombstone, and the scan never hashes anyway). Every line the
 * traversal hot path reads stays dense: n_inl, both keys and both values
 * span three cache lines instead of five, so the dependent load chain
 * per node costs one DRAM miss with the adjacent lines in tow. */
typedef struct cs_inl {
  cs_value key; /* CS_UNSET = tombstone (delattr) / empty (fresh block) */
  cs_value val;
} cs_inl;

#define CS_INST_INLINE 2u
#define CS_INST_INL(o) ((cs_inl*)((char*)(o) + sizeof(cs_obj)))

/* ---- literal-name attribute fast path (codegen's attr inlining) -------- *
 * Generated code knows the attribute name at compile time, so the hash of
 * cs_text_lit(name) + cs_table_find is pure overhead for the common shape
 * `p.<field>` on an instance: the field lives in the inline tail (first
 * CS_INST_INLINE attributes), which a linear scan of ≤ 2 compact entries
 * resolves without hashing, without touching the attrs table and without
 * the class-methods probe. These helpers are exactly that fast path:
 *   instance + inline hit   -> the tail entry (memcmp / pointer compare);
 *   anything else           -> the full cs_getattr / cs_setattr, which
 *                              re-scans the tail (tiny, cold) and then
 *                              handles spilled fields, methods (bound
 *                              method), non-instance receivers (exception
 *                              .type/.message, file .closed/.name), the
 *                              CS_MISSING sentinel and every error path.
 * Behavior is byte-identical to the full functions: the stored key is the
 * same literal pointer cs_text_lit would build (the caller passes its own
 * .rodata name), the append-vs-spill decision uses the same n_inl
 * high-water test, and cs_setattr's TypeError for non-instances lands in
 * the fallback. */
static inline cs_value cs_getattr_lit(cs_value obj, const char* name,
                                      int line) {
  if (obj.tag == CS_INSTANCE) {
    cs_obj* io = obj.as.o;
    size_t nlen = strlen(name); /* folded: name is a literal at every site */
    uint32_t k;
    for (k = 0; k < io->as.inst.n_inl; k++) {
      const cs_inl* e = &CS_INST_INL(io)[k];
      if (e->key.tag == CS_STRING && e->key.as.s.len == nlen &&
          (e->key.as.s.p == name || memcmp(e->key.as.s.p, name, nlen) == 0))
        return e->val; /* inline hit: no hash, no probe, no bound method */
    }
  }
  return cs_getattr(obj, name, line); /* everything else (cold for this site) */
}

static inline void cs_setattr_lit(cs_value obj, const char* name,
                                  cs_value val, int line) {
  if (obj.tag == CS_INSTANCE) {
    cs_obj* io = obj.as.o;
    size_t nlen = strlen(name);
    uint32_t k;
    for (k = 0; k < io->as.inst.n_inl; k++) {
      cs_inl* e = &CS_INST_INL(io)[k];
      if (e->key.tag == CS_STRING && e->key.as.s.len == nlen &&
          (e->key.as.s.p == name || memcmp(e->key.as.s.p, name, nlen) == 0)) {
        e->val = val;
        return;
      }
    }
    if (io->as.inst.n_inl < CS_INST_INLINE) { /* append (insertion order) */
      cs_inl* e = &CS_INST_INL(io)[io->as.inst.n_inl];
      io->as.inst.n_inl += 1u;
      e->key.tag = CS_STRING;
      e->key.as.s.p = name;
      e->key.as.s.len = nlen;
      e->val = val;
      return;
    }
  }
  cs_setattr(obj, name, val, line); /* spilled / non-instance / error paths */
}

/* ---- hot-path helpers --------------------------------------------------
 * Every guarded variable read compiles to cs_guard(...) and every loop
 * condition / boolean operator to cs_truthy(...). As separate-TU calls
 * each costs a cross-object call (~7-10 ns measured on the reference
 * machine — docs/performance/baseline-m2b.md); both bodies are tiny and pure, so they
 * live here as static inline and clang folds them into the generated code
 * (the unset/missing error paths are the only exits, and both are
 * CS_NORETURN). Moved verbatim from cstar_rt.c; behavior identical. */

static inline cs_value cs_guard(cs_value v, const char* name, cs_scope scope,
                                int line) {
  if (v.tag != CS_UNSET) return v;
  /* M2-2C §2.2: used-before-assignment is a catchable NameError. */
  if (scope == CS_SCOPE_GLOBAL)
    cs_runtime_error_as("NameError", line,
                        "global variable '%s' used before assignment", name);
  cs_runtime_error_as("NameError", line,
                      "local variable '%s' used before assignment", name);
}

/* Shadow-stack frame push/pop (moved verbatim from cstar_rt.c; the state
 * is the process-wide cs_gc_frames/cs_gc_nframes). */
static inline void cs_frame_begin(cs_frame* f, cs_value** slots,
                                  size_t nslots) {
  f->prev = cs_gc_frames;
  f->slots = slots;
  f->nslots = nslots;
  cs_gc_frames = f;
  cs_gc_nframes++;
}
static inline void cs_frame_end(cs_frame* f) {
  if (cs_gc_frames == f) {
    cs_gc_frames = f->prev;
    cs_gc_nframes--;
  }
}

/* Protect-stack push/pop (moved verbatim from cstar_rt.c). */
static inline void cs_gc_protect(const cs_value* vs, size_t n) {
  if (n == 0u) return;
  if (cs_gc_nprot + n > cs_gc_prot_cap) {
    size_t cap = (cs_gc_prot_cap != 0) ? cs_gc_prot_cap : 64u;
    while (cap < cs_gc_nprot + n) cap *= 2u;
    cs_gc_prot = (cs_value*)realloc(cs_gc_prot, cap * sizeof(cs_value));
    if (cs_gc_prot == NULL) cs_runtime_error(0, "out of memory");
    cs_gc_prot_cap = cap;
  }
  memcpy(cs_gc_prot + cs_gc_nprot, vs, n * sizeof(cs_value));
  cs_gc_nprot += n;
}
static inline void cs_gc_unprotect(size_t n) {
  if (n > cs_gc_nprot) cs_runtime_error(0, "internal error: protect underflow");
  cs_gc_nprot -= n;
}

/* ---- native int64 fast paths (post-2C perf: TODO.txt speed goal) --------
 *
 * Codegen proves which slots (globals / unboxed locals) hold only int64
 * values and keeps them as a native int64_t pair (value + used-before-
 * assignment flag); every read/write then compiles to straight C
 * arithmetic. These inline helpers replicate the cs_value operator
 * semantics EXACTLY — same two's-complement wrap, same division/modulo
 * edge cases, same error classes, wording and lines — so every observable
 * byte stays identical to the dynamic path. */
static inline int64_t cs_nread_l(int64_t v, char ok, int line,
                                 const char* name) {
  if (!ok)
    cs_runtime_error_as("NameError", line,
                        "local variable '%s' used before assignment", name);
  return v;
}

static inline int64_t cs_nread_g(int64_t v, char ok, int line,
                                 const char* name) {
  if (!ok)
    cs_runtime_error_as("NameError", line,
                        "global variable '%s' used before assignment", name);
  return v;
}

/* Wrapping two's-complement arithmetic via uint64_t (defined behavior) —
 * mirrors cs_wrap_add/cs_wrap_sub/cs_wrap_mul and cs_neg's int path. */
static inline int64_t cs_nwrap_add(int64_t x, int64_t y) {
  return (int64_t)((uint64_t)x + (uint64_t)y);
}
static inline int64_t cs_nwrap_sub(int64_t x, int64_t y) {
  return (int64_t)((uint64_t)x - (uint64_t)y);
}
static inline int64_t cs_nwrap_mul(int64_t x, int64_t y) {
  return (int64_t)((uint64_t)x * (uint64_t)y);
}
static inline int64_t cs_nwrap_neg(int64_t x) {
  return (int64_t)((uint64_t)0 - (uint64_t)x);
}

/* Bitwise operators' int path (M2-2A §3.2): the two's-complement bitwise
 * op over int64, value-identical to cs_band/cs_bor/cs_bxor with the
 * operand-type check skipped — the lane analysis proved both operands
 * int64 (exactly the proof cs_ndiv relies on to skip cs_div's check).
 * Branch-free, no error paths. */
static inline int64_t cs_nwrap_band(int64_t x, int64_t y) {
  return (int64_t)((uint64_t)x & (uint64_t)y);
}
static inline int64_t cs_nwrap_bor(int64_t x, int64_t y) {
  return (int64_t)((uint64_t)x | (uint64_t)y);
}
static inline int64_t cs_nwrap_bxor(int64_t x, int64_t y) {
  return (int64_t)((uint64_t)x ^ (uint64_t)y);
}

/* Shifts: the count range check is NOT skipped — cs_shl/cs_shr raise the
 * same ValueError (same message, same line) when the count leaves
 * 0..63, and the lane helpers must fire it identically; only the
 * operand-type check is proven away. `>>` is an arithmetic shift built
 * from the unsigned shift (no reliance on the implementation-defined
 * signed >>), mirroring cs_shr exactly. */
static inline int64_t cs_nwrap_shl(int64_t x, int64_t y, int line) {
  if (y < 0 || y > 63)
    cs_runtime_error_as("ValueError", line,
                        "shift count must be in range 0..63");
  return (int64_t)((uint64_t)x << (unsigned)y);
}
static inline int64_t cs_nwrap_shr(int64_t x, int64_t y, int line) {
  if (y < 0 || y > 63)
    cs_runtime_error_as("ValueError", line,
                        "shift count must be in range 0..63");
  if (x >= 0) return (int64_t)((uint64_t)x >> (unsigned)y);
  return (int64_t)~((~(uint64_t)x) >> (unsigned)y);
}

/* ~a == -a-1 as a uint64 complement (defined for INT64_MIN), mirroring
 * cs_bnot's int path with the tag check proven away. */
static inline int64_t cs_nbnot(int64_t x) { return (int64_t)(~(uint64_t)x); }

/* cs_div's int path (the line argument drives the error). */
static inline int64_t cs_ndiv(int64_t a, int64_t b, int line) {
  if (b == 0) cs_runtime_error_as("ZeroDivisionError", line, "division by zero");
  if (a == INT64_MIN && b == -1) return INT64_MIN; /* int arithmetic wraps */
  return a / b; /* C99: truncates toward zero */
}

/* cs_mod's int path: C semantics, sign follows the dividend. */
static inline int64_t cs_nmod(int64_t a, int64_t b, int line) {
  if (b == 0) cs_runtime_error_as("ZeroDivisionError", line, "modulo by zero");
  if (a == INT64_MIN && b == -1) return 0; /* would be UB */
  return a % b;
}

/* Python-parity builtin fast paths (SPEC §5.11.5) — the native-lane
 * forms of abs()/min()/max()/pow() over proven-int operands. Wrapping
 * matches the boxed runtime paths bit-for-bit; a lane `abs(INT64_MIN)`
 * wraps to INT64_MIN (two's-complement, like every other cstar int
 * op), and a negative pow exponent raises cs_pow's ValueError. */
static inline int64_t cs_nabs(int64_t x) { return x < 0 ? cs_nwrap_neg(x) : x; }
static inline int64_t cs_nmin(int64_t a, int64_t b) { return a < b ? a : b; }
static inline int64_t cs_nmax(int64_t a, int64_t b) { return a > b ? a : b; }
static inline int64_t cs_npow(int64_t base, int64_t e, int line) {
  uint64_t acc = 1u, b;
  if (e < 0) cs_runtime_error_as("ValueError", line, "negative exponent");
  b = (uint64_t)base;
  while (e != 0) { /* binary exponentiation, wrapping mod 2^64 */
    if ((e & 1) != 0) acc *= b;
    b *= b;
    e >>= 1;
  }
  return (int64_t)acc;
}

/* ---- native float64 lane (float twin of the int64 lane) -----------------
 * Codegen proves which slots hold only CS_FLOAT values and keeps them as a
 * native double pair (value + used-before-assignment flag), mirroring the
 * int64 lane above. The helpers replicate the cs_value operator semantics
 * EXACTLY — same IEEE arithmetic (strict, no fast-math, source order), same
 * division-by-zero check as cs_div_f/cs_div (a ±0.0 divisor raises the
 * same ZeroDivisionError), same NameError wording — so every observable
 * byte stays identical to the dynamic path. Boxed writes happen only at
 * observation points (cs_float() is a bit-copy of the double). */
static inline double cs_nfrd_l(double v, char ok, int line,
                               const char* name) {
  if (!ok)
    cs_runtime_error_as("NameError", line,
                        "local variable '%s' used before assignment", name);
  return v;
}

static inline double cs_nfrd_g(double v, char ok, int line,
                               const char* name) {
  if (!ok)
    cs_runtime_error_as("NameError", line,
                        "global variable '%s' used before assignment", name);
  return v;
}

/* cs_div's float path with the operand-type checks proven away (the lane
 * analysis proved both sides float): same y == 0.0 ZeroDivisionError at the
 * same line (a ±0.0 divisor raises; inf/nan divisors do not), same IEEE
 * division. */
static inline double cs_nfdiv(double x, double y, int line) {
  if (y == 0.0)
    cs_runtime_error_as("ZeroDivisionError", line, "division by zero");
  return x / y;
}

/* Lane multiply with FMA contraction blocked. The dynamic path evaluates
 * `a * b` and a consuming `+`/`-` as SEPARATE statements (each result is a
 * boxed cs_value temp), so the compiler can never fuse them: two IEEE
 * roundings. A lane expression like `(a * b) + c` is ONE C expression, and
 * clang at -O3 -march=native contracts it to vfmadd (ONE rounding) — a
 * different double in the last bits. The empty-asm barrier below is the
 * standard zero-instruction contraction blocker: the add's operand becomes
 * the opaque asm output, never a recognizable fmul node. Only `*` needs it
 * (FMA is mul+add only; +,-,/, comparisons and unary minus never fuse).
 * Non-IEEE targets without an FP-register constraint fall back to a volatile
 * round-trip (slower, but the no-fusion contract holds everywhere). */
static inline double cs_nfmul(double a, double b) {
  double p = a * b;
#if defined(__x86_64__) || defined(__i386__)
  __asm__("" : "+x"(p));
#elif defined(__aarch64__)
  __asm__("" : "+w"(p));
#else
  {
    volatile double vp = p;
    p = vp;
  }
#endif
  return p;
}

/* int(<proven float-lane value>): exactly cs_builtin_int's CS_FLOAT branch
 * — truncation toward zero, with the same NaN/inf/out-of-int64-range
 * ValueError at the same source line. The lane analysis proves the operand
 * is a CS_FLOAT, so no tag dispatch is needed; the range guard stays (it is
 * part of the language, not an operand-type check). */
static inline int64_t cs_nf2i(double d, int line) {
  /* The two range compares accept exactly the convertible doubles: NaN
   * fails both (unordered) and ±inf fail one, so no separate isfinite
   * test is needed — the rejected set and the ValueError are unchanged. */
  if (d >= -9223372036854775808.0 && d < 9223372036854775808.0)
    return (int64_t)d;
  cs_runtime_error_as("ValueError", line, "cannot convert float to int");
  return 0; /* not reached */
}

/* Exact int64-vs-double ordering for the lanes' mixed comparisons.
 * cs_cmp3 compares numeric operands through long double (exact for every
 * int64 and every double on x86_64). For |i| <= 2^53 the int64 -> double
 * conversion is exact, so a plain IEEE compare is bit-identical to that
 * long-double compare and costs one instruction; larger magnitudes keep
 * the exact long-double path. Return: -1 less, 0 equal, 1 greater,
 * 2 unordered (NaN). */
static inline int cs_ncmp_i64_d(int64_t i, double d) {
  if (i >= -9007199254740992LL && i <= 9007199254740992LL) {
    double di = (double)i;
    if (di < d) return -1;
    if (di > d) return 1;
    if (di == d) return 0;
    return 2; /* NaN */
  }
  {
    long double x = (long double)i;
    long double y = (long double)d;
    if (x < y) return -1;
    if (x > y) return 1;
    if (x == y) return 0;
    return 2; /* NaN */
  }
}
/* Operator wrappers: `int OP double` (the lane handles the reversed
 * operand order by swapping the operator at emission time). */
static inline bool cs_nlt_i_d(int64_t i, double d) {
  return cs_ncmp_i64_d(i, d) == -1;
}
static inline bool cs_nle_i_d(int64_t i, double d) {
  int c = cs_ncmp_i64_d(i, d);
  return c == -1 || c == 0;
}
static inline bool cs_ngt_i_d(int64_t i, double d) {
  return cs_ncmp_i64_d(i, d) == 1;
}
static inline bool cs_nge_i_d(int64_t i, double d) {
  int c = cs_ncmp_i64_d(i, d);
  return c == 1 || c == 0;
}
static inline bool cs_neq_i_d(int64_t i, double d) {
  return cs_ncmp_i64_d(i, d) == 0;
}
static inline bool cs_nne_i_d(int64_t i, double d) {
  return cs_ncmp_i64_d(i, d) != 0;
}

/* ---- text-chain builder (perf: formatted-output fusion) ----------------
 * The classic record-building idiom `f.write(str(i) + " " + str(v) + "\n")`
 * used to allocate one GC block per str() and per concatenation and copy
 * each growing prefix — five heap blocks per 15-byte record. The codegen
 * recognizes a `+` chain whose parts are provably text (string literals and
 * str() calls) and builds it in ONE buffer: literals are copied straight
 * in, proven-int str() arguments format straight in (no heap text at all),
 * and the tail either writes the buffer with ONE fwrite (f.write chains —
 * the file-side fprintf equivalent, zero heap traffic) or turns it into a
 * single heap text (a value chain — one block instead of n-1). The stack
 * buffer covers every short record; longer output spills to one malloc. */
#define CS_SB_STACK 256u
typedef struct cs_sb {
  char* p;
  size_t n;
  size_t cap;
  char stack_[CS_SB_STACK];
  int heap;
} cs_sb;

/* Fast int64 -> decimal: byte-identical to snprintf("%lld") (ASCII digits,
 * '-' prefix, the full range including INT64_MIN — the negation is done in
 * unsigned space). ~4ns vs snprintf's ~40-70ns; measured in the A/B for the
 * stdout fast path (docs/performance/optimization.md method). */
static inline int cs_itoa_ll(char* b, int64_t v) {
  char t[20];
  int i = 0;
  uint64_t u;
  if (v < 0) { b[0] = '-'; u = (uint64_t)(-(v + 1)) + 1u; } else u = (uint64_t)v;
  do { t[i++] = (char)('0' + (char)(u % 10u)); u /= 10u; } while (u != 0u);
  for (int j = 0; j < i; j++) b[(v < 0) + j] = t[i - 1 - j];
  return (v < 0) + i;
}

static inline void cs_sb_grow(cs_sb* b, size_t need) {
  size_t want = b->n + need;
  size_t cap = b->cap * 2u;
  char* np;
  while (cap < want) cap *= 2u;
  if (!b->heap) {
    np = (char*)malloc(cap);
    if (np != NULL) memcpy(np, b->p, b->n);
    b->heap = 1;
  } else {
    np = (char*)realloc(b->p, cap);
  }
  if (np == NULL) cs_runtime_error(0, "out of memory");
  b->p = np;
  b->cap = cap;
}

static inline void cs_sb_init(cs_sb* b) {
  b->p = b->stack_;
  b->n = 0u;
  b->cap = CS_SB_STACK;
  b->heap = 0;
}

static inline void cs_sb_putn(cs_sb* b, const char* s, size_t n) {
  if (n == 0u) return;
  if (b->n + n > b->cap) cs_sb_grow(b, n);
  memcpy(b->p + b->n, s, n);
  b->n += n;
}

/* Formats an int64 into the buffer with str(int)'s exact spelling
 * (cs_itoa_ll is byte-identical to the display path's snprintf). */
static inline void cs_sb_puti(cs_sb* b, int64_t v) {
  char buf[24];
  int k = cs_itoa_ll(buf, v);
  cs_sb_putn(b, buf, (size_t)k);
}

/* Appends a text value's bytes (a literal or a materialized str() result —
 * the codegen only fuses chains whose parts are provably text). */
static inline void cs_sb_putv(cs_sb* b, cs_value t) {
  cs_sb_putn(b, t.as.s.p, t.as.s.len);
}

static inline void cs_sb_free(cs_sb* b) {
  if (b->heap) free(b->p);
}

/* The two tails (runtime: both declared here, defined in cstar_rt.c).
 * cs_sb_finish copies the buffer into ONE registered heap text; the fused
 * f.write writes it out and returns the byte count like file.write does
 * (non-file receivers fall back to the dynamic method path — identical
 * errors, cold). */
cs_value cs_sb_finish(const cs_sb* b, int line);
cs_value cs_file_write_buf(cs_value file, const char* p, size_t n, int line);

/* ---- dynamic numeric fast paths (perf: TODO.txt "optimize this hard") ----
 * Generated code calls the _f operator variants instead of cs_add/cs_sub/
 * ... directly. Each one runs the runtime's own numeric fast path INLINE —
 * identical tag checks, identical wrap / division / modulo edge cases,
 * identical double conversion (cs_as_double) — and falls back to the
 * out-of-line operator for everything else (string/bytes concat, mixed
 * int/float comparisons, type errors, missing operands). Observable
 * behavior is byte-identical, but numeric hot loops pay NO cross-object
 * call: an opaque call in a loop body forces LLVM to assume every slot
 * global is clobbered, destroying register promotion and ok-flag
 * elimination — the "dynamic dispatch tax". With these inlined the
 * dynamic path compiles like the native int64 lane (tag check + ALU).
 * Mixed int/float ORDERING stays on the fallback on purpose: cs_cmp3
 * compares via long double, which a double conversion could not
 * reproduce for big int64 values. */
static inline double cs_dbl(cs_value v) {
  return v.tag == CS_FLOAT ? v.as.d : (double)v.as.i;
}

int64_t cs_il_iget_slow(cs_value c, int64_t i, int line);
/* Out-of-range read of a proven int-only list: raises the exact list
 * IndexError and never returns, so the hot inline path need not keep the
 * container/len live across a call. */
CS_NORETURN int64_t cs_il_oob_named(size_t count, int64_t idx, int line);
/* Element i of a list/tuple, honoring a copy-on-write view's stride.
 * (Defined here so generated code can inline the hot index fast path.) */
static inline cs_value cs_seq_at(const struct cs_obj* o, size_t i) {
  ptrdiff_t st = o->as.seq.stride;
  return (st == 0) ? o->as.seq.items[i]
                   : o->as.seq.items[(ptrdiff_t)i * st];
}

/* Branchless Python index normalization: negative indices count from the
 * end, then one unsigned compare tests [-n, n). The sign mask is all-ones
 * for i < 0, so it adds exactly n (not just bit 0 of n). */
static inline size_t cs_norm_idx(size_t n, int64_t i) {
  uint64_t u = (uint64_t)i;
  size_t mask = (size_t)0 - (size_t)(u >> 63);
  return (size_t)u + (mask & n);
}

/* Inline `container[idx]` fast path: list/tuple + in-range int. Every other
 * shape (including out-of-range, which must raise with the exact IndexError
 * wording) defers to the out-of-line operator. */
static inline cs_value cs_index_get_f(cs_value container, cs_value idx,
                                      int line) {
  if ((container.tag == CS_LIST || container.tag == CS_TUPLE) &&
      idx.tag == CS_INT) {
    size_t n = container.as.o->as.seq.len;
    size_t k = cs_norm_idx(n, idx.as.i);
    if (k < n) return cs_seq_at(container.as.o, k);
  }
  return cs_index_get(container, idx, line);
}

/* Inline `len(v)` fast path for the two sequence heap kinds. */
static inline cs_value cs_len_f(cs_value v, int line) {
  if (v.tag == CS_LIST || v.tag == CS_TUPLE)
    return cs_int((int64_t)v.as.o->as.seq.len);
  return cs_builtin_len(v, line);
}

/* Native read of a proven int-only list element. The fast path mirrors the
 * list branch of cs_index_get_f and returns the raw int64; every other shape
 * (out of range, non-list, a fact-inconsistent element) defers to the
 * out-of-line implementation so the dynamic error behavior is unchanged. */
static inline int64_t cs_il_iget(cs_value c, int64_t i, int line) {
  if (c.tag == CS_LIST) {
    size_t n = c.as.o->as.seq.len;
    size_t k = cs_norm_idx(n, i);
    if (k < n) return cs_seq_at(c.as.o, k).as.i;  // int_lists_: CS_INT
  }
  return cs_il_iget_slow(c, i, line);
}

/* Flat int-list read: the proof says seq.stride == 0, so no stride math. */
static inline int64_t cs_il_iget_flat(cs_value c, int64_t i, int line) {
  if (c.tag == CS_LIST) {
    size_t n = c.as.o->as.seq.len;
    size_t k = cs_norm_idx(n, i);
    if (k < n) return c.as.o->as.seq.items[k].as.i;  // int_lists_: CS_INT
    cs_il_oob_named(n, i, line);  // noreturn
  }
  return cs_il_iget_slow(c, i, line);
}

/* Direct int-list read with the source's stable view fields supplied as raw
 * locals. c is only touched on the cold (out-of-range / fact-violation) path,
 * so the hot path never reloads the source slot. */
static inline int64_t cs_il_iget_raw(cs_value* items, ptrdiff_t st, size_t n,
                                     int64_t i, int line) {
  size_t k = cs_norm_idx(n, i);
  if (k < n) {
    cs_value v = (st == 0) ? items[k] : items[(ptrdiff_t)k * st];
    return v.as.i;  // int_lists_ proved CS_INT
  }
  cs_il_oob_named(n, i, line);  // noreturn
}

/* Lazy-slice element read for the boxed world. The fast path mirrors
 * cs_slice(...)[idx] for list/tuple + int: cs_norm_index's normalization is
 * two branches, the source index is guaranteed in range by normalization,
 * and cs_seq_at honors a view stride. Every other shape (non-int index,
 * out of range, text/bytes/map) defers to the slow path so the exact
 * TypeError/IndexError wording and line are unchanged. */
static inline cs_value cs_slice_get(cs_value c, int64_t first, int64_t step,
                                    size_t count, cs_value idx, int line) {
  if ((c.tag == CS_LIST || c.tag == CS_TUPLE) && idx.tag == CS_INT) {
    size_t k = cs_norm_idx(count, idx.as.i);
    if (k < count) {
      uint64_t off = (uint64_t)k * (uint64_t)step;
      size_t si = (size_t)((uint64_t)first + off);
      return cs_seq_at(c.as.o, si);
    }
    cs_slice_oob_named(c.tag == CS_LIST ? "list" : "tuple", count, idx.as.i,
                       line);
  }
  return cs_slice_get_slow(c, first, step, count, idx, line);
}

/* Native twin: the caller proved the source is an int-only list, so the
 * selected element is CS_INT and the bounds are already normalized. */
static inline int64_t cs_slice_iget_obj(cs_obj* o, int64_t first, int64_t step,
                                        size_t count, int64_t idx, int line) {
  size_t k = cs_norm_idx(count, idx);
  if (k < count) {
    uint64_t off = (uint64_t)k * (uint64_t)step;
    size_t si = (size_t)((uint64_t)first + off);
    return cs_seq_at(o, si).as.i;  // int_lists_ proved CS_INT
  }
  cs_slice_oob_named("list", count, idx, line);  // noreturn
}

/* Same read with the source's stable (write-free after the snapshot) view
 * fields supplied directly, so the loop loads nothing from the heap. */
static inline int64_t cs_slice_iget_raw(cs_value* items, ptrdiff_t st,
                                        int64_t first, int64_t step,
                                        size_t count, int64_t idx, int line) {
  size_t k = cs_norm_idx(count, idx);
  if (k < count) {
    uint64_t off = (uint64_t)k * (uint64_t)step;
    size_t si = (size_t)((uint64_t)first + off);
    cs_value v = (st == 0) ? items[si] : items[(ptrdiff_t)si * st];
    return v.as.i;  // int_lists_ proved CS_INT
  }
  cs_slice_oob_named("list", count, idx, line);  // noreturn
}

static inline int64_t cs_slice_iget(cs_value c, int64_t first, int64_t step,
                                    size_t count, int64_t idx, int line) {
  return cs_slice_iget_obj(c.as.o, first, step, count, idx, line);
}

/* Bounds normalization for a source already proven to be an int-only list,
 * called only with a positive step. Returns the selected span as a value
 * struct (never via out-parameters), so the compiler can keep first/count in
 * registers across the fused element reads. The lazy-slice pass only admits
 * this shape; every other slice keeps the general cs_slice path. */
typedef struct {
  int64_t first;
  uint64_t count;
} cs_slice_span;

static inline cs_slice_span cs_slice_span_n(size_t n, int64_t start,
                                            int has_start, int64_t stop,
                                            int has_stop, int64_t step) {
  size_t len = n;
  int64_t b, e;
  if (has_start && has_stop && start >= 0 && stop >= 0 &&
      (uint64_t)start <= (uint64_t)len && (uint64_t)stop <= (uint64_t)len) {
    // Both explicit non-negative bounds are already inside the sequence;
    // every clamp below would be a no-op.
    b = start;
    e = stop;
  } else {
    int64_t nn = (len > (size_t)INT64_MAX) ? INT64_MAX : (int64_t)len;
    b = has_start ? start : 0;
    e = has_stop ? stop : nn;
    if (has_start) {
      if (b < 0) b += nn;
      if (b < 0) b = 0;
      if (b > nn) b = nn;
    }
    if (has_stop) {
      if (e < 0) e += nn;
      if (e < 0) e = 0;
      if (e > nn) e = nn;
    }
  }
  uint64_t cnt = 0u;
  if (e > b)
    cnt = ((uint64_t)e - (uint64_t)b + (uint64_t)step - 1u) / (uint64_t)step;
  cs_slice_span out;
  out.first = b;
  out.count = cnt;
  return out;
}

static inline cs_slice_span cs_slice_span_list_obj(cs_obj* o, int64_t start,
                                                   int has_start, int64_t stop,
                                                   int has_stop, int64_t step) {
  return cs_slice_span_n(o->as.seq.len, start, has_start, stop, has_stop, step);
}

static inline cs_slice_span cs_slice_span_list(cs_value c, int64_t start,
                                               int has_start, int64_t stop,
                                               int has_stop, int64_t step) {
  return cs_slice_span_list_obj(c.as.o, start, has_start, stop, has_stop, step);
}

static inline cs_value cs_add_f(cs_value a, cs_value b, int line) {
  if (a.tag == CS_INT && b.tag == CS_INT)
    return cs_int(cs_nwrap_add(a.as.i, b.as.i));
  if ((a.tag == CS_INT || a.tag == CS_FLOAT) &&
      (b.tag == CS_INT || b.tag == CS_FLOAT))
    return cs_float(cs_dbl(a) + cs_dbl(b));
  return cs_add(a, b, line);
}

static inline cs_value cs_sub_f(cs_value a, cs_value b, int line) {
  if (a.tag == CS_INT && b.tag == CS_INT)
    return cs_int(cs_nwrap_sub(a.as.i, b.as.i));
  if ((a.tag == CS_INT || a.tag == CS_FLOAT) &&
      (b.tag == CS_INT || b.tag == CS_FLOAT))
    return cs_float(cs_dbl(a) - cs_dbl(b));
  return cs_sub(a, b, line);
}

static inline cs_value cs_mul_f(cs_value a, cs_value b, int line) {
  if (a.tag == CS_INT && b.tag == CS_INT)
    return cs_int(cs_nwrap_mul(a.as.i, b.as.i));
  if ((a.tag == CS_INT || a.tag == CS_FLOAT) &&
      (b.tag == CS_INT || b.tag == CS_FLOAT))
    return cs_float(cs_dbl(a) * cs_dbl(b));
  return cs_mul(a, b, line);
}

static inline cs_value cs_div_f(cs_value a, cs_value b, int line) {
  if (a.tag == CS_INT && b.tag == CS_INT)
    return cs_int(cs_ndiv(a.as.i, b.as.i, line));
  if ((a.tag == CS_INT || a.tag == CS_FLOAT) &&
      (b.tag == CS_INT || b.tag == CS_FLOAT)) {
    double x = cs_dbl(a), y = cs_dbl(b);
    if (y == 0.0)
      cs_runtime_error_as("ZeroDivisionError", line, "division by zero");
    return cs_float(x / y);
  }
  return cs_div(a, b, line);
}

static inline cs_value cs_mod_f(cs_value a, cs_value b, int line) {
  if (a.tag == CS_INT && b.tag == CS_INT)
    return cs_int(cs_nmod(a.as.i, b.as.i, line));
  return cs_mod(a, b, line); /* bad types / missing → identical wording */
}

static inline cs_value cs_neg_f(cs_value a, int line) {
  if (a.tag == CS_INT) return cs_int(cs_nwrap_neg(a.as.i));
  if (a.tag == CS_FLOAT) return cs_float(-a.as.d);
  return cs_neg(a, line);
}

/* Ordering: int/int and float/float inline (IEEE compare matches cs_cmp3's
 * long double compare exactly, NaN unordered → all false); mixed numeric,
 * text/bytes and error cases fall back. */
static inline cs_value cs_lt_f(cs_value a, cs_value b, int line) {
  if (a.tag == CS_INT && b.tag == CS_INT)
    return cs_bool(a.as.i < b.as.i);
  if (a.tag == CS_FLOAT && b.tag == CS_FLOAT)
    return cs_bool(a.as.d < b.as.d);
  return cs_lt(a, b, line);
}
static inline cs_value cs_le_f(cs_value a, cs_value b, int line) {
  if (a.tag == CS_INT && b.tag == CS_INT)
    return cs_bool(a.as.i <= b.as.i);
  if (a.tag == CS_FLOAT && b.tag == CS_FLOAT)
    return cs_bool(a.as.d <= b.as.d);
  return cs_le(a, b, line);
}
static inline cs_value cs_gt_f(cs_value a, cs_value b, int line) {
  if (a.tag == CS_INT && b.tag == CS_INT)
    return cs_bool(a.as.i > b.as.i);
  if (a.tag == CS_FLOAT && b.tag == CS_FLOAT)
    return cs_bool(a.as.d > b.as.d);
  return cs_gt(a, b, line);
}
static inline cs_value cs_ge_f(cs_value a, cs_value b, int line) {
  if (a.tag == CS_INT && b.tag == CS_INT)
    return cs_bool(a.as.i >= b.as.i);
  if (a.tag == CS_FLOAT && b.tag == CS_FLOAT)
    return cs_bool(a.as.d >= b.as.d);
  return cs_ge(a, b, line);
}

/* == / != are total (M1 §4): int/int and float/float inline with the same
 * IEEE semantics as cs_value_eq; every other pairing falls back. */
static inline cs_value cs_eq_f(cs_value a, cs_value b) {
  if (a.tag == CS_INT && b.tag == CS_INT)
    return cs_bool(a.as.i == b.as.i);
  if (a.tag == CS_FLOAT && b.tag == CS_FLOAT)
    return cs_bool(a.as.d == b.as.d);
  /* Cross-tag: only int/float mixtures compare by value (1 == 1.0 is
   * True) — every other tag pair is unequal by construction, so instance
   * loops like `cur != None` answer here without the out-of-line
   * operator (tag checks only, no call, no argument shuffle). */
  if (a.tag != b.tag &&
      !((a.tag == CS_INT || a.tag == CS_FLOAT) &&
        (b.tag == CS_INT || b.tag == CS_FLOAT)))
    return cs_bool(false);
  return cs_eq(a, b);
}
static inline cs_value cs_ne_f(cs_value a, cs_value b) {
  if (a.tag == CS_INT && b.tag == CS_INT)
    return cs_bool(a.as.i != b.as.i);
  if (a.tag == CS_FLOAT && b.tag == CS_FLOAT)
    return cs_bool(a.as.d != b.as.d);
  if (a.tag != b.tag &&
      !((a.tag == CS_INT || a.tag == CS_FLOAT) &&
        (b.tag == CS_INT || b.tag == CS_FLOAT)))
    return cs_bool(true);
  return cs_ne(a, b);
}

/* Collection truthiness needs `struct cs_obj`, whose layout is private to
 * cstar_rt.c — those four tags delegate; every scalar/tag case is
 * decided right here without a call. */
bool cs_truthy_obj(cs_value v, int line);

static inline bool cs_truthy(cs_value v, int line) {
  switch (v.tag) {
    case CS_NONE: return false;
    case CS_INT: return v.as.i != 0;
    case CS_FLOAT: return v.as.d != 0.0; /* -0.0 falsy; NaN truthy */
    case CS_BOOL: return v.as.b;
    case CS_STRING: return v.as.s.len != 0;
    case CS_BYTES: return v.as.s.len != 0;       /* M2-2B §9 */
    case CS_LIST: return cs_truthy_obj(v, line); /* M2-2B §9 */
    case CS_TUPLE: return cs_truthy_obj(v, line);
    case CS_SET: return cs_truthy_obj(v, line);
    case CS_MAP: return cs_truthy_obj(v, line);
    case CS_FVEC: return cs_truthy_obj(v, line); /* SIMD vectors: len != 0 */
    case CS_IVEC: return cs_truthy_obj(v, line);
    case CS_FUNCTION: return true; /* functions are truthy */
    case CS_CLASS:
    case CS_EXCEPTION:
    case CS_INSTANCE:
    case CS_BOUND_METHOD: return true; /* M2-2C §2/§3: truthy objects */
    case CS_ITERATOR:
    case CS_FILE: return true; /* SPEC §5.11.5: iterators and open files
                                * are always truthy (Python: no __bool__) */
    case CS_RANGE:
      /* Exact emptiness for any int64 bounds — this test never overflows:
       * step>0 → start < stop; step<0 → start > stop. */
      if (v.as.r.step > 0) return v.as.r.start < v.as.r.stop;
      return v.as.r.start > v.as.r.stop;
    case CS_MISSING:
      /* M2-2B §13: sentinel never escapes — ops that can observe a
       * bound-vector slot report this and exit. */
      cs_runtime_error(line, "internal error: missing argument");
      return false; /* not reached */
    case CS_UNSET:
    default:
      /* Unreachable in well-formed programs: every read is guarded before
       * it can observe an unset value (§6/§8). Reaching here means
       * generated code skipped a guard — a compiler bug, not a user error. */
      cs_runtime_error(line, "internal error: unset value in cs_truthy");
  }
}

/* ---- fvec/ivec: first-class SIMD vectors ---------------------------------
 * A float64/int64 vector is a heap object with a 64-byte-aligned data
 * buffer. The lane types below make vectorization GUARANTEED, not hoped
 * for: fused elementwise kernels (emitted by codegen) and the runtime
 * elementwise ops process whole lanes at the host's widest vector
 * extension, with a scalar tail for the remainder. Width selection follows
 * the compile-time CPU macros (the CLI compiles with -march=native); on a
 * baseline host the kernels fall back to a 2-wide lane and stay correct. */
#if defined(__AVX512F__)
typedef double cs_vd __attribute__((vector_size(64)));
typedef int64_t cs_vi __attribute__((vector_size(64)));
#define CS_VW 8
#define CS_VD_SPLAT(x)                                        \
  __builtin_shufflevector((cs_vd){(x)}, (cs_vd){(x)}, 0, 0, 0, 0, 0, 0, 0, 0)
#define CS_VI_SPLAT(x)                                        \
  __builtin_shufflevector((cs_vi){(x)}, (cs_vi){(x)}, 0, 0, 0, 0, 0, 0, 0, 0)
#elif defined(__AVX2__)
typedef double cs_vd __attribute__((vector_size(32)));
typedef int64_t cs_vi __attribute__((vector_size(32)));
#define CS_VW 4
#define CS_VD_SPLAT(x) \
  __builtin_shufflevector((cs_vd){(x)}, (cs_vd){(x)}, 0, 0, 0, 0)
#define CS_VI_SPLAT(x) \
  __builtin_shufflevector((cs_vi){(x)}, (cs_vi){(x)}, 0, 0, 0, 0)
#else
typedef double cs_vd __attribute__((vector_size(16)));
typedef int64_t cs_vi __attribute__((vector_size(16)));
#define CS_VW 2
#define CS_VD_SPLAT(x) __builtin_shufflevector((cs_vd){(x)}, (cs_vd){(x)}, 0, 0)
#define CS_VI_SPLAT(x) __builtin_shufflevector((cs_vi){(x)}, (cs_vi){(x)}, 0, 0)
#endif

/* Value constructors for the vector tags (the object itself comes from
 * cs_fvec_new/cs_ivec_new below). */
static inline cs_value cs_fvec_val(cs_obj* o) {
  cs_value r;
  r.tag = CS_FVEC;
  memset(&r.as, 0, sizeof(r.as));
  r.as.o = o;
  return r;
}
static inline cs_value cs_ivec_val(cs_obj* o) {
  cs_value r;
  r.tag = CS_IVEC;
  memset(&r.as, 0, sizeof(r.as));
  r.as.o = o;
  return r;
}

/* Runtime half of the vector API (GC registration, 64-byte-aligned zeroed
 * allocation, typed accessors). struct cs_obj's layout stays private to
 * cstar_rt.c — generated code goes through these only. */
cs_obj* cs_fvec_new(size_t n, int line);
cs_obj* cs_ivec_new(size_t n, int line);
size_t cs_vec_len_of(cs_value v);
double* cs_fvec_data_of(cs_value v);
int64_t* cs_ivec_data_of(cs_value v);
/* Length agreement check for elementwise kernels: `other` must carry
 * exactly n elements, else ValueError 'fvec length mismatch' / 'ivec
 * length mismatch' (raised with the kernel's source line). */
void cs_fvec_expect_len(cs_value other, size_t n, int line);
void cs_ivec_expect_len(cs_value other, size_t n, int line);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CSTAR_RT_H */
