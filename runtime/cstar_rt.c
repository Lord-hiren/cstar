/* cstar_rt.c — runtime support library for cstar-generated C11 code.
 *
 * Implements docs/contracts/m1-semantics.md §4 (values/operators/print formatting),
 * §8 (runtime error format) and §10 (runtime API contract), plus the
 * M2 Stage 2A additions of docs/contracts/m2a-semantics.md §3-§6, §9, §10
 * (range values, truthiness, `**`, bitwise ops, `->`, iteration protocol),
 * plus the M2 Stage 2B additions of docs/contracts/m2b-semantics.md §2, §4-§9,
 * §12, §13, §15 (collections, Text/Bytes, hashing, indexing, slicing,
 * iteration, membership, builtins, comma-join, destructuring helpers,
 * function values + dynamic-call binder, method dispatch).
 * M1 behavior and error wording are preserved exactly; the only changes to
 * pre-existing operations are the user-approved 2B behavior changes: the
 * CS_STRING tag is Text (type name "text", was "string") and Text
 * indexing/iteration yields whole UTF-8 code points.
 * C11, libc + libm only, no pthread. Linux x86_64.
 */
/* clock_gettime/time (input/time/clock builtins) is POSIX; the compile
 * line is strict -std=c11, which hides POSIX declarations by default. */
#define _POSIX_C_SOURCE 200809L

#include "cstar_rt.h"

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <errno.h>    /* SPEC §5.11.5: open() failure classification */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strncasecmp: header-field matching (CURL headers) */
#include <sys/stat.h> /* fstat/S_ISREG: whole-file read sizing (read()) */
#include <time.h>
#include <unistd.h> /* write/isatty: the stdout fast path (cs_out_*) */
#include <curl/curl.h> /* HTTP builtin (cs_builtin_http): libcurl */

/* ---- process state --------------------------------------------------- */

static const char* cs_state_path = "<program>";
static int cs_state_argc = 0;
static char** cs_state_argv = NULL;

/* ---- M2-2C §1: GC state ---------------------------------------------- */

static cs_value* const* cs_gc_globals = NULL; /* codegen-emitted roots */
static size_t cs_gc_nglobals = 0;
/* Shadow-stack + protect-stack state: file-scope (not static) because the
 * header's static-inline fast paths (cs_frame_begin/end, cs_gc_protect/
 * unprotect — the same treatment cs_guard/cs_truthy get) touch them from
 * the generated TU; a single process-wide instance either way. */
cs_frame* cs_gc_frames = NULL; /* shadow-stack head (header type) */
size_t cs_gc_nframes = 0;      /* depth of cs_gc_frames (2C §2.5) */
cs_value* cs_gc_prot = NULL;   /* protect stack (dynamic, cs_call-era
                                * binder values / callee bound) */
size_t cs_gc_nprot = 0;
size_t cs_gc_prot_cap = 0;

/* ---- lane-buffer reuse pool (vector fast paths) -----------------------
 * Sweeping a vector used to free() its aligned_alloc block, so every
 * transient of a vector arithmetic chain paid mmap + munmap + page-fault
 * costs on the next statement — on 64 MB working sets that dominated the
 * fused kernel (the kernel itself runs at DRAM speed; re-faulting a
 * fresh buffer every pass does not). Large buffers (>= 64 KB — the
 * page-granule sizes where the syscall/fault round-trip matters) are now
 * parked in exact-size buckets and handed straight back out by
 * cs_vec_alloc; glibc's bins already recycle small blocks.
 *
 * A parked buffer may hold STALE lane values. That is safe because every
 * producer fills [0, len) before anything can read the vector
 * (constructors, operator kernels, fused emission) and nothing ever
 * reads past len (lane loops and tails are bounded by it); reachability
 * is decided at explicit safe points only (cs_obj_new/cs_gc_new never
 * collect), so a pooled buffer is provably unreferenced. The pool's
 * total is capped so a program touching many distinct large sizes
 * cannot pin memory. */
#define CS_VP_MIN (64u << 10)     /* pool buffers >= 64 KB */
#define CS_VP_MAX_BYTES (512u << 20) /* park at most 512 MB */
#define CS_VP_SLOTS 32

static struct { size_t sz; void* head; } cs_vp[CS_VP_SLOTS];
static size_t cs_vp_bytes = 0u;

/* Park a swept lane buffer (exact byte size — must match what
 * cs_vec_alloc asks for). A free buffer's first 8 bytes hold the
 * intrusive next pointer. */
static void cs_vec_buf_free(void* d, size_t sz) {
  int i, free_slot = -1;
  if (d == NULL) return;
  if (sz < CS_VP_MIN || cs_vp_bytes + sz > CS_VP_MAX_BYTES) {
    free(d);
    return;
  }
  for (i = 0; i < CS_VP_SLOTS; i++) {
    if (cs_vp[i].sz == sz) break;
    if (free_slot < 0 && cs_vp[i].sz == 0u) free_slot = i;
  }
  if (i == CS_VP_SLOTS) { /* no bucket for this size yet */
    if (free_slot < 0) { free(d); return; } /* table full: not cacheable */
    i = free_slot;
    cs_vp[i].sz = sz;
    cs_vp[i].head = NULL;
  }
  *(void**)d = cs_vp[i].head;
  cs_vp[i].head = d;
  cs_vp_bytes += sz;
}

/* Pop a parked buffer of exactly sz bytes, or NULL to allocate fresh. */
static void* cs_vec_buf_get(size_t sz) {
  int i;
  if (sz >= CS_VP_MIN) {
    for (i = 0; i < CS_VP_SLOTS; i++) {
      if (cs_vp[i].sz == sz && cs_vp[i].head != NULL) {
        void* d = cs_vp[i].head;
        cs_vp[i].head = *(void**)d;
        cs_vp_bytes -= sz;
        return d;
      }
    }
  }
  return NULL;
}

/* ---- M2-2C §2: exception state ---------------------------------------- */

/* Defined with the exception core below; referenced early: cs_runtime_error
 * (top of file) funnels into cs_throw_message, and cs_init creates the
 * builtin class singletons. */
static CS_NORETURN void cs_throw_message(cs_value cls, int line,
                                         const char* fmt, va_list ap);
static void cs_exc_init_classes(void);
static cs_value cs_exc_named(const char* name);

/* Handler stack (§2.5): head = innermost active try entry (generated code
 * owns the cs_handler objects — they live on the landing function's C
 * stack; only this head pointer is runtime state). */
static cs_handler* cs_exc_top = NULL;

/* Builtin exception class singletons (§2.2), created by cs_init and
 * always marked by the collector (reachable from generated code through
 * cs_exc_class and produced internally by cs_raise/cs_throw_message). */
enum {
  CS_EXC_EXCEPTION = 0,
  CS_EXC_RUNTIMEERROR,
  CS_EXC_TYPEERROR,
  CS_EXC_VALUEERROR,
  CS_EXC_NAMEERROR,
  CS_EXC_ZERODIVISIONERROR,
  CS_EXC_INDEXERROR,
  CS_EXC_KEYERROR,
  CS_EXC_ATTRIBUTEERROR,
  /* SPEC §5.11.7: Python-parity additions — next() exhaustion, open()
   * failures (ENOENT → FileNotFoundError, everything else → OSError). */
  CS_EXC_STOPITERATION,
  CS_EXC_FILENOTFOUNDERROR,
  CS_EXC_OSError,
  CS_EXC_N
};
static const char* const cs_exc_cls_names[CS_EXC_N] = {
    "Exception",      "RuntimeError", "TypeError",      "ValueError",
    "NameError",      "ZeroDivisionError", "IndexError", "KeyError",
    "AttributeError", "StopIteration", "FileNotFoundError", "OSError"};
static cs_value cs_exc_cls[CS_EXC_N];

/* Pending cs_call bound vectors (§2.5): a longjmp out of a callee must
 * not leak the caller's bound vector — cs_throw frees every vector pushed
 * after the landing entry. Each entry carries a kind tag: HEAP vectors
 * (malloc'd by the binder / cs_bound_call / cs_class_call) are freed on
 * pop or unwind; C-STACK vectors (the allocation-free method-dispatch fast
 * path) are only dropped — the longjmp discards the frame that owns them,
 * and free()ing them would corrupt the heap. The vectors are never GC
 * roots (callee code copies what it needs out of them at entry). */
typedef struct cs_call_rec {
  void* p;
  bool on_stack;
} cs_call_rec;
static cs_call_rec* cs_call_bounds = NULL;
static size_t cs_ncalls = 0;
static size_t cs_call_cap = 0;

/* ---- stdout fast path (docs/performance/optimization.md) ---------------
 * print() builds whole lines in a runtime-owned buffer and flushes with
 * one write(2) per 256 KiB — or per line when stdout is a TTY (matching
 * Python's line_buffering=True, so interactive output stays prompt). The
 * stdio FILE* layer is bypassed per argument (its per-call locking and
 * the snprintf-based formatting it encouraged dominated hot print loops);
 * output bytes are identical. Flush points: buffer full, end of every
 * print on a TTY, input() (prompt semantics — SPEC §5.11.5), the uncaught
 * error printer (so a crash message cannot jump ahead of pending output),
 * and process exit (atexit — covers main's return and exit(1)). */
#define CS_OUT_CAP (1u << 18)
static char cs_out[CS_OUT_CAP];
static size_t cs_out_n = 0u;
static int cs_out_tty = 0;

static void cs_out_flush(void) {
  size_t o = 0u;
  while (o < cs_out_n) {
    ssize_t w = write(1, cs_out + o, cs_out_n - o);
    if (w <= 0) {
      if (w < 0 && errno == EINTR) continue;
      break; /* EPIPE etc: like stdio, the pending bytes are dropped */
    }
    o += (size_t)w;
  }
  cs_out_n = 0u;
}

static void cs_out_add(const char* s, size_t n) {
  if (n == 0u) return;
  if (n >= CS_OUT_CAP) { /* a single display larger than the buffer */
    size_t o = 0u;
    cs_out_flush();
    while (o < n) {
      ssize_t w = write(1, s + o, n - o);
      if (w <= 0) { if (w < 0 && errno == EINTR) continue; break; }
      o += (size_t)w;
    }
    return;
  }
  if (cs_out_n + n > CS_OUT_CAP) cs_out_flush();
  memcpy(cs_out + cs_out_n, s, n);
  cs_out_n += n;
}

void cs_init(int argc, char** argv, const char* src_path,
             cs_value* const* roots, size_t nroots) {
  cs_state_argc = argc;
  cs_state_argv = argv;
  cs_state_path = (src_path != NULL) ? src_path : "";
  cs_gc_globals = roots;
  cs_gc_nglobals = nroots;
  cs_out_tty = isatty(1); /* TTY: print flushes per line (Python parity) */
  atexit(cs_out_flush);   /* every exit() path drains pending output */
  cs_exc_init_classes(); /* 2C §2.2: after roots so the singletons mark */
}

int cs_arg_count(void) { return cs_state_argc; }

const char* cs_arg(int i) {
  if (cs_state_argv == NULL || i < 0 || i >= cs_state_argc) return NULL;
  return cs_state_argv[i];
}

const char* cs_src_path(void) { return cs_state_path; }

/* ---- error reporting (§8) -------------------------------------------- */

/* M2-2C §2.6: keeps its 2B signature and exit semantics, but now *throws*
 * a RuntimeError instance (catchable; uncaught output byte-identical). */
CS_NORETURN void cs_runtime_error(int line, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  cs_throw_message(cs_exc_cls[CS_EXC_RUNTIMEERROR], line, fmt, ap);
}

CS_NORETURN void cs_runtime_error_as(const char* cls_name, int line,
                                     const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  cs_throw_message(cs_exc_named(cls_name), line, fmt, ap);
}

/* M2-2B §13: CS_MISSING must never escape into user operations; ops that
 * can observe a bound-vector slot report this and exit. */
static void cs_missing_error(int line) {
  cs_runtime_error(line, "internal error: missing argument");
}

static void cs_no_missing(cs_value v, int line) {
  if (v.tag == CS_MISSING) cs_missing_error(line);
}

static void cs_no_missing2(cs_value a, cs_value b, int line) {
  if (a.tag == CS_MISSING || b.tag == CS_MISSING) cs_missing_error(line);
}

/* M2-2B §4: total-equality hash (defined with the collection machinery
 * below; raises `unhashable type: '<t>'` for list/map/set/range/function
 * and the internal CS_MISSING error for the sentinel). */
static uint64_t cs_hash_value(cs_value v, int line);

/* Open-file registry close (SPEC §5.11.5; defined with the Python-parity
 * builtins below): idempotent fclose + unlink + path free. The GC sweep
 * and close()/exit() all funnel through it. */
static void cs_file_close_obj(cs_obj* o);
/* One line from an open file (has_size → at most `size` chars in text
 * mode / bytes in binary): the line keeps its trailing '\n' when present;
 * CS_NONE at EOF. Drives next()/for-over-file and readline(). */
static cs_value cs_file_readline(cs_value filev, int has_size, int64_t size,
                                 int line);
/* File methods (read/readline/readlines/write/writelines/seek/tell/
 * flush/close — SPEC §5.11.5); dispatch target of cs_call_method. */
static cs_value cs_method_file(cs_value obj, const char* m,
                               const cs_value* args, size_t n, int line);

/* Reports `unsupported operand types for <op>: <t1> and <t2>` and exits. */
static void cs_bad_binary(int line, const char* op, cs_value a, cs_value b) {
  cs_runtime_error_as("TypeError", line, "unsupported operand types for %s: %s and %s", op,
                   cs_type_name(a), cs_type_name(b));
}

/* ---- used-before-assignment guard (§6/§8) ----------------------------- */
/* cs_guard is static inline at the end of cstar_rt.h (hot path). */

/* ---- helpers ---------------------------------------------------------- */

static int cs_is_numeric(cs_value v) { return v.tag == CS_INT || v.tag == CS_FLOAT; }

static double cs_as_double(cs_value v) {
  return v.tag == CS_FLOAT ? v.as.d : (double)v.as.i;
}

/* Exact conversion for comparisons: (long double) is lossless for every
 * int64_t and every double on x86_64, so mixed int/float comparisons are
 * mathematically exact (9223372036854775807 == 9223372036854775808.0 is
 * False). Never route an int through double first. */
static long double cs_as_ld(cs_value v) {
  return v.tag == CS_INT ? (long double)v.as.i : (long double)v.as.d;
}

/* Wrapping two's-complement arithmetic via uint64_t (defined behavior). */
static int64_t cs_wrap_add(int64_t x, int64_t y) {
  return (int64_t)((uint64_t)x + (uint64_t)y);
}
static int64_t cs_wrap_sub(int64_t x, int64_t y) {
  return (int64_t)((uint64_t)x - (uint64_t)y);
}
static int64_t cs_wrap_mul(int64_t x, int64_t y) {
  return (int64_t)((uint64_t)x * (uint64_t)y);
}

/* Byte-wise lexicographic compare; on prefix tie the shorter string is less. */
static int cs_str_cmp(cs_value a, cs_value b) {
  size_t n = a.as.s.len < b.as.s.len ? a.as.s.len : b.as.s.len;
  int c = (n > 0) ? memcmp(a.as.s.p, b.as.s.p, n) : 0;
  if (c != 0) return c < 0 ? -1 : 1;
  if (a.as.s.len == b.as.s.len) return 0;
  return a.as.s.len < b.as.s.len ? -1 : 1;
}

/* ---- M2-2C §1: GC registry ------------------------------------------- *
 * Every heap allocation that outlives its creating operation is a
 * registered block: prefix {links, size, kind, mark} + max-aligned
 * payload. Kinds CS_LIST..CS_FUNCTION (object tags) trace their members;
 * CS_GC_TEXT/CS_GC_BYTES are leaves; CS_GC_CELL traces one cs_value.
 * Static string literals and builtin singletons are never registered. */

typedef union cs_gcblk {
  struct {
    union cs_gcblk* next;
    union cs_gcblk* prev;
    size_t sz;            /* header + payload bytes */
    unsigned char kind;   /* CS_* object tag, or CS_GC_* below */
    unsigned char marked;
    unsigned char grow;   /* TEXT/BYTES only: capacity-aware concatenation
                           * (cs_text_cat2) created this block WITH spare
                           * capacity for in-place appends; only a value
                           * born from such a concat (never aliased — the
                           * codegen's may_grow proof) may append into it.
                           * Fits the existing header padding (sizeof is
                           * unchanged: 8+8+8+3 -> 32, max_align_t 16). */
    uint32_t gen;         /* TEXT/BYTES only: allocation generation — a
                           * monotonically increasing stamp bumped by every
                           * TEXT/BYTES allocation (perf: code-point memo,
                           * below). Lets a cache keyed on a payload pointer
                           * detect that the block died and a DIFFERENT
                           * buffer now lives at the same address with the
                           * same length (see cs_cp_bind). Also 4 bytes of
                           * the same padding: 8+8+8+1+1+1+(pad)+4 = 32. */
  } h;
  max_align_t align_;     /* keeps the payload max-aligned */
} cs_gcblk;

/* Header size is load-bearing (block sizes, parking buckets, byte
 * accounting): the extra fields must fit the padding that was always
 * there, or every allocation would grow. */
_Static_assert(sizeof(cs_gcblk) == 32, "cs_gcblk header must stay 32 bytes");

#define CS_GC_TEXT 100u
#define CS_GC_BYTES 101u
#define CS_GC_CELL 102u

static cs_gcblk* cs_gc_head = NULL;
static cs_gcblk* cs_gc_tail = NULL;
size_t cs_gc_bytes = 0;        /* registered bytes live */
size_t cs_gc_limit = 1048576u; /* collect at next safe point past this */

/* ---- code-point query memo state (perf: unicode_scan / text_find) ------ *
 * Text indexing (t[i]), len(t) and find()'s byte-offset → code-point
 * conversion are answered from ONE process-wide memo naming a single
 * (payload, length) view of a registered heap text, so the O(len)
 * re-decode-per-query machinery becomes O(1) (or one amortized forward
 * walk) for hot loops. Soundness rules — all enforced at *query* time by
 * cs_cp_bind (defined next to the UTF-8 helpers below):
 *   - the memo only ever NAMES registered TEXT/BYTES payloads (tset hit),
 *     never literals or plain-malloc buffers — those take the direct
 *     path, exactly the pre-memo code;
 *   - identity = payload pointer + byte length + the block's allocation
 *     generation (h.gen), bumped on every TEXT/BYTES allocation, so a
 *     freed-and-reused address can never impersonate the captured
 *     buffer; a generation wrap invalidates the memo outright;
 *   - text payloads are immutable except cs_text_cat2's in-place
 *     APPEND, which changes the length — part of the key, so an append
 *     can never alias a cached fact either;
 *   - every cached fact is a true statement about those exact bytes:
 *     a full code-point count (total), a forward offset cursor and a
 *     prefix-count cursor, each walked only from a position previously
 *     established under the same bound. */
typedef struct cs_cp_memo {
  const char* base;  /* key: registered payload pointer (NULL = invalid) */
  size_t len;        /* key: byte length of the named view               */
  uint32_t gen;      /* key: h.gen of the named block                    */
  size_t total;      /* code points in [0, len), valid iff total_ok      */
  size_t it_cp;      /* offset cursor: code point index at byte it_off   */
  size_t it_off;
  size_t pr_cp;      /* prefix cursor: code points in [0, pr_off)        */
  size_t pr_off;
  unsigned char total_ok;
  unsigned char valid;
} cs_cp_memo;

static cs_cp_memo cs_cp = {0};
static uint32_t cs_gc_text_gen = 0; /* bumped per TEXT/BYTES allocation */

/* ---- swept-block parking (exact size + kind) -------------------------- *
 * The sweep parks dead blocks of the churn-prone kinds in exact-
 * (size, kind) buckets — the cs_vp[] lane-buffer precedent, generalized to
 * GC blocks — and cs_gc_new_full hands them straight back out instead of
 * paying a malloc/free round trip per cell / list object / function
 * object / text buffer. Soundness rules (all enforced below):
 *   - only kinds whose sweep already frees (or never had) member arrays
 *     are parked, so a parked block owns nothing. TEXT/BYTES payloads are
 *     inline in the block and additionally registered in the tset; the
 *     tset staleness is closed by ORDERING: the sweep unlinks and parks
 *     dead blocks INSIDE the loop, then rebuilds the tset from the main
 *     list AFTER the loop — so a parked payload is out of the tset from
 *     the rebuild onward, and nothing consults the tset inside the sweep
 *     loop (the loop only free()s members and relinks). On reuse,
 *     cs_gc_new_full re-inserts the payload into the tset and bumps the
 *     allocation generation (TEXT/BYTES branch), so a memo keyed on the
 *     old (payload, length, generation) can never be impersonated by the
 *     block's next life; cs_cp_named/cs_cp_bind consult the tset BEFORE
 *     dereferencing the header, so a parked-or-freed payload is never
 *     read through a stale entry;
 *   - a parked block is UNLINKED from the main list — the tracer and the
 *     sweep never see it (it is dead: post-sweep-unreachable by
 *     construction; mark handles cycles; nothing can resurrect it — every
 *     shadow-frame slot is a mark root, so any block a value still points
 *     at is marked, never parked);
 *   - popping re-initializes the whole header (links, size, kind, mark=0,
 *     grow=0 — a popped text block never inherits a stale spare-capacity
 *     flag, so cat_grow's in-place append cannot write past what the
 *     exact-size bucket match guarantees) and re-links the block as if
 *     freshly malloc'd; zeroed allocations memset the payload like a
 *     fresh block, and the !zero path's contract is "caller fills the
 *     whole payload before any read", which stale bytes satisfy exactly
 *     like fresh malloc garbage does;
 *   - pinned cells are rooted forever (the mark phase marks them), so
 *     they are never swept and can never be parked.
 * The parked pool is capped by BOTH block count and total bytes so a
 * program that churns many sizes — including large texts — and then stops
 * cannot pin memory (text payloads are unbounded, so a count cap alone
 * would be insufficient). */
#define CS_BP_SLOTS 24
#define CS_BP_MAX_BLOCKS 65536u /* hard cap on parked block count */
#define CS_BP_MAX_BYTES (16u << 20) /* hard cap on parked payload bytes */

static struct {
  size_t sz;
  unsigned char kind;
  cs_gcblk* head;
} cs_bp[CS_BP_SLOTS];
static size_t cs_bp_blocks = 0u;
static size_t cs_bp_bytes = 0u; /* sum of parked b->h.sz, count-cap partner */

/* Park an already-unlinked, already-accounted dead block. */
static void cs_bp_park(cs_gcblk* b) {
  int i, free_slot = -1;
  if (cs_bp_blocks >= CS_BP_MAX_BLOCKS ||
      cs_bp_bytes + b->h.sz > CS_BP_MAX_BYTES) {
    free(b);
    return;
  }
  for (i = 0; i < CS_BP_SLOTS; i++) {
    if (cs_bp[i].head != NULL && cs_bp[i].sz == b->h.sz &&
        cs_bp[i].kind == b->h.kind)
      break;
    if (free_slot < 0 && cs_bp[i].head == NULL) free_slot = i;
  }
  if (i == CS_BP_SLOTS) { /* no bucket for this (size, kind) yet */
    if (free_slot < 0) {
      free(b); /* table full: not cacheable */
      return;
    }
    i = free_slot;
    cs_bp[i].sz = b->h.sz;
    cs_bp[i].kind = b->h.kind;
    cs_bp[i].head = NULL;
  }
  b->h.next = cs_bp[i].head;
  cs_bp[i].head = b;
  cs_bp_blocks++;
  cs_bp_bytes += b->h.sz;
}

/* Pop a parked block of exactly (kind, size), or NULL to allocate fresh. */
static cs_gcblk* cs_bp_pop(unsigned char kind, size_t sz) {
  int i;
  for (i = 0; i < CS_BP_SLOTS; i++) {
    if (cs_bp[i].head != NULL && cs_bp[i].sz == sz && cs_bp[i].kind == kind) {
      cs_gcblk* b = cs_bp[i].head;
      cs_bp[i].head = b->h.next;
      cs_bp_blocks--;
      cs_bp_bytes -= b->h.sz;
      return b;
    }
  }
  return NULL;
}

/* Kinds the sweep may park (the sweep frees/never-had their members
 * first; reuse re-initializes everything — see the parking docs; TEXT/
 * BYTES additionally ride the tset, closed by park-before-rebuild). */
#define CS_BP_PARKABLE(kind)                                                  \
  ((kind) == CS_GC_CELL || (kind) == CS_LIST || (kind) == CS_TUPLE ||         \
   (kind) == CS_FUNCTION || (kind) == CS_ITERATOR || (kind) == CS_FVEC ||     \
   (kind) == CS_IVEC || (kind) == CS_GC_TEXT || (kind) == CS_GC_BYTES)

static void cs_gc_tset_insert(void* p); /* payload set, defined below */
static int cs_gc_tset_has(const void* p); /* payload set, defined below */
static cs_gcblk* cs_gc_blk_of(const void* payload); /* defined below */

static void* cs_gc_new_full(unsigned char kind, size_t payload, int line,
                            int zero) {
  size_t sz = sizeof(cs_gcblk) + payload;
  cs_gcblk* b = cs_bp_pop(kind, sz);
  if (b == NULL) {
    b = (cs_gcblk*)malloc(sz);
    if (b == NULL) cs_runtime_error(line, "out of memory");
  }
  if (zero) memset((void*)(b + 1), 0, payload); /* zeroed like the old calloc */
  b->h.next = NULL;
  b->h.prev = cs_gc_tail;
  b->h.sz = sz;
  b->h.kind = kind;
  b->h.marked = 0;
  b->h.grow = 0; /* spare-capacity flag: set only by cs_text_cat_grow */
  if (kind == CS_GC_TEXT || kind == CS_GC_BYTES) {
    /* Fresh allocation generation: any code-point memo fact captured for
     * this ADDRESS (old block, now swept) is thereby stale — cs_cp_bind's
     * gen check rejects it before a single byte is read. On 2^32 wrap the
     * memo is dropped outright, so equal generations can never collide. */
    if (cs_gc_text_gen == 0xffffffffu) { /* next increment would be 0 */
      cs_gc_text_gen = 1u;
      cs_cp.valid = 0;
    } else {
      cs_gc_text_gen += 1u;
    }
    b->h.gen = cs_gc_text_gen;
  } else {
    b->h.gen = 0; /* never read (tset names TEXT/BYTES payloads only) */
  }
  if (cs_gc_tail != NULL) cs_gc_tail->h.next = b;
  else cs_gc_head = b;
  cs_gc_tail = b;
  cs_gc_bytes += b->h.sz;
  if (kind == CS_GC_TEXT || kind == CS_GC_BYTES) cs_gc_tset_insert((void*)(b + 1));
  return (void*)(b + 1);
}

static void* cs_gc_new(unsigned char kind, size_t payload, int line) {
  return cs_gc_new_full(kind, payload, line, 1);
}

/* Heap text/bytes buffer with NUL terminator (payload len+1). */
static char* cs_gc_buf(unsigned char kind, size_t len, int line) {
  return (char*)cs_gc_new(kind, len + 1u, line);
}

/* Same, but the CALLER fills every payload byte before any read (the fread
 * read path below): skips the zeroing pass — a pure-cost full write for a
 * multi-megabyte buffer that is about to be overwritten anyway. */
static char* cs_gc_buf_filled(unsigned char kind, size_t len, int line) {
  return (char*)cs_gc_new_full(kind, len + 1u, line, 0);
}

/* string + string concatenation. Heap buffers are GC-registered (2C §1). */
static cs_value cs_concat(cs_value a, cs_value b, int line) {
  size_t la = a.as.s.len, lb = b.as.s.len;
  char* p;
  cs_value r;
  /* The GC block kind is NOT the value tag: text buffers are CS_GC_TEXT /
   * CS_GC_BYTES (registered in the tset, so CS_STRING values pointing into
   * them mark the block). Passing a.tag here made every concatenated
   * string invisible to the collector — swept while still live (use-after-
   * free once a collection ran mid-program). Both parts and the NUL are
   * written below, so the zeroing memset is pure waste — the caller-fill
   * contract of cs_gc_buf_filled. */
  p = cs_gc_buf_filled(a.tag == CS_STRING ? (unsigned char)CS_GC_TEXT
                                          : (unsigned char)CS_GC_BYTES,
                       la + lb, line);
  if (la > 0) memcpy(p, a.as.s.p, la);
  if (lb > 0) memcpy(p + la, b.as.s.p, lb);
  p[la + lb] = '\0';
  /* M2-2B §5: `+` concatenates text+text or bytes+bytes, and the result
   * keeps the operand kind — both callers pass same-tag operands. */
  r = a;
  r.as.s.p = p;
  r.as.s.len = la + lb;
  return r;
}

/* ---- amortized capacity-aware concatenation (perf: text_build) ---------- *
 * `s = s + chunk` in a loop used to allocate an EXACT-size block per
 * append and copy the whole prefix every time — O(n^2) copy volume. The
 * codegen proves a statement pattern (`name = name + expr`, or `name +=
 * expr`) uniquely owns the destination buffer (see codegen.cpp's
 * analyze_text_cats for the proof) and calls cs_text_cat2 with may_grow=1;
 * the runtime then appends IN PLACE while the block has spare capacity and
 * reallocates geometrically (doubling) when it does not, making the total
 * copy volume O(final length) — the C realloc idiom.
 *
 * may_grow is a PERMISSION, not a promise: the runtime re-checks that `a`
 * really is a heap-allocated text/bytes block (exact-payload tset probe —
 * a literal points at static storage and is registered nowhere) whose
 * header carries the grow flag (h.grow: a block a previous cat2 created
 * WITH spare capacity). A literal or any unflagged block takes the fresh-
 * block path with doubled capacity, so the loop's first iteration over a
 * literal seed is correct and the loop amortizes from the second on. The
 * tset probe answers heap-ness WITHOUT touching `a`'s storage — reading h
 * before a literal would be invalid memory.
 *
 * Safety of the in-place step under may_grow=1 (what the codegen proof
 * guarantees, re-verified at runtime by the flag):
 *   - the buffer is reachable only from the assigned slot, so no alias can
 *     observe the appended bytes; the slot's own NUL invariant is restored
 *     before return, so cs_text_cstr's fast path stays byte-identical;
 *   - the self-concat case `s = s + s` is the ONLY way `b` can share `a`'s
 *     buffer under the proof (same p, same len — shorter/longer aliasing
 *     views are excluded): src [p, p+len) and dst [p+len, p+2len) are
 *     adjacent and disjoint; the copy uses memmove regardless;
 *   - the append path never allocates and never reaches a GC safe point
 *     (no collection can run between reading a.p and storing the result);
 *     the grow path allocates a NEW block and leaves the old one for the
 *     collector to PARK (exact-size reuse at the next same-size text
 *     allocation; amortized garbage is bounded by the geometric series of
 *     old capacities, <= 2x live text size, and the pool's own block/byte
 *     caps bound what parking retains);
 *   - results are byte-identical to cs_concat's: same bytes, same NUL, same
 *     tag — only the buffer identity/size differ, and nothing observable
 *     can hold the old buffer. */
static cs_value cs_text_cat_grow(cs_value a, cs_value b, unsigned char kind,
                                 int line) {
  size_t la = a.as.s.len, lb = b.as.s.len;
  const char* p = a.as.s.p;
  size_t oldcap = 0;
  int heap = (p != NULL) && cs_gc_tset_has((const void*)p);
  if (heap) oldcap = cs_gc_blk_of((const void*)p)->h.sz - sizeof(cs_gcblk);
  if (heap && cs_gc_blk_of((const void*)p)->h.grow && la + lb + 1u <= oldcap) {
    /* Fits (content + NUL): append in place, same pointer back. */
    char* w = (char*)p;
    if (lb > 0u) memmove(w + la, b.as.s.p, lb);
    w[la + lb] = '\0';
    a.as.s.len = la + lb;
    return a;
  }
  /* Grow / non-growable entry: one fresh block with DOUBLED capacity (at
   * least 2 * (len + 1)), so the next appends amortize. The old block (if
   * any) becomes garbage; the collector frees it at the next safe point. */
  {
    size_t need = la + lb + 1u; /* content + NUL */
    size_t cap = need * 2u;
    char* np;
    cs_value r;
    if (oldcap > need && oldcap * 2u > cap) cap = oldcap * 2u;
    np = (char*)cs_gc_new_full(kind, cap, line, 0); /* filled below */
    if (la > 0u) memcpy(np, p, la);
    if (lb > 0u) memcpy(np + la, b.as.s.p, lb);
    np[la + lb] = '\0';
    cs_gc_blk_of(np)->h.grow = 1;
    r = a;
    r.as.s.p = np;
    r.as.s.len = la + lb;
    return r;
  }
}

/* Capacity-aware `+` for codegen-proven `name = name + expr` sites.
 * may_grow == 0 behaves exactly like cs_add (which for same-tag text/bytes
 * IS cs_concat). may_grow == 1 permits the in-place append described above.
 * Everything else — mixed tags, ints, floats, vectors, missing operands —
 * falls through to cs_add, byte-identical to the cs_add_f fallback the
 * codegen would otherwise have emitted (cs_add_f's inline numeric fast
 * paths are exactly cs_add's numeric paths). */
cs_value cs_text_cat2(cs_value a, cs_value b, int line, int may_grow) {
  if (may_grow && a.tag == CS_STRING && b.tag == CS_STRING)
    return cs_text_cat_grow(a, b, (unsigned char)CS_GC_TEXT, line);
  if (may_grow && a.tag == CS_BYTES && b.tag == CS_BYTES)
    return cs_text_cat_grow(a, b, (unsigned char)CS_GC_BYTES, line);
  return cs_add(a, b, line);
}

/* The value tail of the text-chain fusion: one registered heap text from
 * the builder's buffer (the fused `+` chain — one block instead of the n-1
 * intermediate concatenations). Always text: fused chains only contain
 * string literals and str() results. */
cs_value cs_sb_finish(const cs_sb* b, int line) {
  char* p = NULL;
  if (b->n > 0u) {
    p = cs_gc_buf_filled(CS_GC_TEXT, b->n, line); /* full write below */
    memcpy(p, b->p, b->n);
    p[b->n] = '\0';
  }
  return cs_str(p, b->n);
}

/* ---- M2-2B §2: heap object layout ------------------------------------ *
 * list/map/set/tuple/function are cs_obj* behind this header. The
 * refcount field is reserved for 2C and unused in 2B (2B leaks every heap
 * object — documented limitation). Map and set share one insertion-ordered
 * hash-table representation (set values are unused); list and tuple share
 * the sequential-array representation (tuple.cap is unused). Internals
 * below are runtime-private: generated code must go through the cs_* API.
 * The cs_entry / cs_table / struct cs_obj / cs_inl definitions moved to
 * cstar_rt.h (internal ABI): the code generator's literal-name attribute
 * fast path (cs_getattr_lit / cs_setattr_lit) scans an instance's inline
 * tail and needs them.
 */

/* Defined with the collection machinery below; declared here because
 * cs_contains (above) probes membership through it. */
static size_t cs_table_find(cs_table* t, cs_value key, uint64_t h);
static uint64_t cs_table_hash(cs_table* t, cs_value v, int line);

/* The registry header of a block is the cs_gcblk immediately BEFORE its
 * payload: blk_of(payload) == (cs_gcblk*)payload - 1. Objects therefore
 * keep their original layout (no embedded links); cs_obj_new allocates
 * through cs_gc_new. */
static cs_gcblk* cs_gc_blk_of(const void* payload) {
  return ((cs_gcblk*)payload) - 1;
}





/* ---- instance-method inline cache (M2-2C §3.2 dispatch) ---------------- *
 * cs_call_method (and cs_class_call for __init__) resolve a method call
 * through this cache BEFORE touching the getattr/bound-method machinery, so
 * a hot `p.step(i)` pays no hash lookup, no bound-method allocation and no
 * malloc for the self+args vector. The resolution is sound because:
 *
 *   - Method tables are written ONLY during class-statement execution
 *     (cs_class_add_method — a class body holds nothing but method
 *     declarations, enforced by semantic.cpp's phase-1 scan; the class
 *     statement itself runs cs_class_new + cs_class_add_method back to back
 *     with no user code in between), and cs_setattr refuses non-instances.
 *     So for a given class OBJECT the name → CS_FUNCTION mapping never
 *     changes once any instance of it can exist.
 *
 * Four guards keep a cached resolution exact (all in the runtime's call
 * sites; see cs_call_method / cs_class_call):
 *
 *   1. entry.cls == receiver's class — classes with identically-named
 *      methods never cross-contaminate (direct-mapped eviction only costs
 *      a re-resolve);
 *   2. strcmp(entry.name, name) — gen.c does NOT pointer-merge equal
 *      string literals, so a site must prove its name matches the cached
 *      one rather than compare literal addresses;
 *   3. the receiver has no field of that name — cs_getattr serves instance
 *      fields (inline area, then the spilled attrs table) BEFORE the
 *      methods table, so `p.step = 42` must keep reading/calling through
 *      the attribute path; the probe mirrors that order exactly;
 *   4. entries are GC roots (cs_gc_collect_full marks cls and fn) — the
 *      collector frees unreachable class objects, and a NEW class could be
 *      allocated at the recycled address, which would otherwise let a
 *      stale entry pass guard 1. Rooting pins the cached objects for the
 *      life of the entry (≤ CS_MIC_SIZE of each) and makes the cached
 *      pointers safe to dereference.
 *
 * Every miss or guard failure re-resolves through the cs_getattr order and
 * re-fills the slot; errors stay byte-identical because the fallback is the
 * same getattr + §13 binder machinery as before. */
#define CS_MIC_SIZE 8u /* power of two — direct-mapped */
typedef struct cs_mic_entry {
  cs_obj* cls;      /* NULL = empty slot */
  const char* name; /* the call site's literal (static storage) */
  cs_obj* fn;       /* the CS_FUNCTION object behind methods[name] */
} cs_mic_entry;
static cs_mic_entry cs_mic[CS_MIC_SIZE];

/* (Defined here — after the complete cs_obj: CS_INSTANCE/CS_EXCEPTION
 * wording carries the class name.) */
const char* cs_type_name(cs_value v) {
  switch (v.tag) {
    case CS_UNSET: return "unset";
    case CS_NONE: return "None";
    case CS_INT: return "int";
    case CS_FLOAT: return "float";
    case CS_BOOL: return "bool";
    case CS_STRING: return "text"; /* M2-2B: was "string" (approved change) */
    case CS_RANGE: return "range"; /* M2-2A §9 */
    case CS_BYTES: return "bytes"; /* M2-2B §12 */
    case CS_LIST: return "list";
    case CS_MAP: return "map";
    case CS_SET: return "set";
    case CS_TUPLE: return "tuple";
    case CS_FUNCTION: return "function";
    case CS_CLASS: return "class"; /* M2-2C §2/§3 */
    case CS_EXCEPTION: return "exception"; /* M2-2C §2 (type() shows the
                                            * class name instead) */
    case CS_INSTANCE: { /* M2-2C §3: the class name — error wording reads
                         * `'Point' object has no ...` */
      const char* n = v.as.o->as.inst.cls.as.o->as.cls.name;
      return n;
    }
    case CS_BOUND_METHOD: return "method"; /* M2-2C §3 */
    case CS_ITERATOR: return "iterator"; /* SPEC §5.11.5 */
    case CS_FILE: return "file";         /* SPEC §5.11.5 */
    case CS_FVEC: return "fvec";         /* SIMD float64 vector */
    case CS_IVEC: return "ivec";         /* SIMD int64 vector */
    case CS_MISSING: return "unset"; /* internal sentinel; never observable */
  }
  return "unset";
}

/* ---- M2-2C §1: mark & sweep ------------------------------------------- *
 * cs_gc_mark_value only marks+enqueues a block; the drain loop does all
 * member tracing iteratively (no C-stack recursion on deep structures). */

static cs_gcblk** cs_gc_work = NULL; /* mark worklist (owned, unregistered) */
static size_t cs_gc_work_n = 0;
static size_t cs_gc_work_cap = 0;
static int64_t cs_gc_last_freed = 0;

/* Immortal cells (docs/reference/cross-scope-access.md §4): a binding that is both
 * cross-scope "pinned" and closure-captured keeps ONE heap cell for the
 * whole program. Registered here so every collection roots them (mark
 * phase below) — they are program-lifetime storage like module globals,
 * never collected. Owned array, grown monotonically. */
static cs_cell** cs_gc_pinned = NULL;
static size_t cs_gc_pinned_n = 0;
static size_t cs_gc_pinned_cap = 0;

static void cs_gc_push(cs_gcblk* b) {
  if (b == NULL || b->h.marked) return;
  b->h.marked = 1;
  if (cs_gc_work_n == cs_gc_work_cap) {
    size_t cap = (cs_gc_work_cap != 0) ? cs_gc_work_cap * 2u : 256u;
    cs_gc_work = (cs_gcblk**)realloc(cs_gc_work, cap * sizeof(cs_gcblk*));
    if (cs_gc_work == NULL) cs_runtime_error(0, "out of memory");
    cs_gc_work_cap = cap;
  }
  cs_gc_work[cs_gc_work_n++] = b;
}

/* Payload-pointer set of the registered TEXT/BYTES blocks: a CS_STRING
 * value cannot tell a static literal from a heap buffer by inspection, so
 * marking asks the set. Only inserts happen between collections (nothing
 * is freed outside a sweep), so the set is rebuilt from the live registry
 * after each sweep — no tombstones. */
static void** cs_gc_tset = NULL;
static size_t cs_gc_tset_cap = 0; /* power of two; NULL slot = empty */
static size_t cs_gc_tset_n = 0;

static void cs_gc_tset_insert(void* p) {
  size_t mask, i;
  if (cs_gc_tset == NULL || (cs_gc_tset_n + 1u) * 10u >= cs_gc_tset_cap * 7u) {
    size_t cap = (cs_gc_tset_cap != 0) ? cs_gc_tset_cap * 2u : 1024u;
    void** nt;
    size_t j;
    while ((cs_gc_tset_n + 1u) * 10u >= cap * 7u) cap *= 2u;
    nt = (void**)calloc(cap, sizeof(void*));
    if (nt == NULL) cs_runtime_error(0, "out of memory");
    for (j = 0; j < cs_gc_tset_cap; j++) {
      void* q = cs_gc_tset[j];
      if (q != NULL) {
        size_t k = (size_t)(((uintptr_t)q >> 4) * 2654435761u) & (cap - 1u);
        while (nt[k] != NULL) k = (k + 1u) & (cap - 1u);
        nt[k] = q;
      }
    }
    free(cs_gc_tset);
    cs_gc_tset = nt;
    cs_gc_tset_cap = cap;
  }
  mask = cs_gc_tset_cap - 1u;
  i = (size_t)(((uintptr_t)p >> 4) * 2654435761u) & mask;
  while (cs_gc_tset[i] != NULL) {
    if (cs_gc_tset[i] == p) return;
    i = (i + 1u) & mask;
  }
  cs_gc_tset[i] = p;
  cs_gc_tset_n++;
}

static int cs_gc_tset_has(const void* p) {
  size_t mask, i;
  if (cs_gc_tset_cap == 0) return 0;
  mask = cs_gc_tset_cap - 1u;
  i = (size_t)(((uintptr_t)p >> 4) * 2654435761u) & mask;
  while (cs_gc_tset[i] != NULL) {
    if (cs_gc_tset[i] == p) return 1;
    i = (i + 1u) & mask;
  }
  return 0;
}

/* Mark the block behind a value, if it has one (immediates are never
 * registered; static text literals are registered nowhere — the payload
 * set decides for CS_STRING/CS_BYTES). */
static void cs_gc_mark_value(cs_value v) {
  switch (v.tag) {
    case CS_LIST:
    case CS_MAP:
    case CS_SET:
    case CS_TUPLE:
    case CS_FUNCTION:
    case CS_CLASS:    /* M2-2C §2: heap singleton / user class */
    case CS_EXCEPTION: /* M2-2C §2: instance */
    case CS_INSTANCE: /* M2-2C §3 */
    case CS_BOUND_METHOD: /* M2-2C §3 */
    case CS_FVEC:   /* SIMD vectors: heap object, leaf payload (data is
                     * freed by the sweep, never traced) */
    case CS_IVEC:
    case CS_ITERATOR: /* SPEC §5.11.5: boxed iteration state — the sweep
                       * frees its source iterator, so a live one that is
                       * never marked dies at the first collection */
    case CS_FILE:     /* SPEC §5.11.5: open handle — the sweep closes the
                       * file, so a reachable file must be marked or it
                       * closes out from under its last reference */
      if (v.as.o != NULL) cs_gc_push(cs_gc_blk_of(v.as.o));
      break;
    case CS_STRING:
    case CS_BYTES:
      if (v.as.s.p != NULL && cs_gc_tset_has(v.as.s.p))
        cs_gc_push(cs_gc_blk_of(v.as.s.p));
      break;
    default:
      break;
  }
}

/* ---- shared slice storage (copy-on-write views) -----------------------
 *
 * Slicing a list/tuple used to copy every selected element. A slice now
 * shares the source's element array: the source adopts a tiny refcounted
 * header (cs_seqbuf) and the view points into it with (items, stride),
 * O(1) regardless of length or step. Views are indistinguishable from
 * ordinary lists: cs_seq_at() reads through the stride, and EVERY
 * mutation first detaches (cs_seq_detach) when the buffer is shared, when
 * the object is a view, or when it does not own the whole allocation.
 * That is exact copy-on-write semantics:
 *
 *   - mutating the source copies it; live views keep the old array;
 *   - mutating a view copies the view and unrefs; the source is untouched;
 *   - a raw list (never sliced) keeps its old layout (buf == NULL) and
 *     pays nothing at all;
 *   - the buffer is freed when the last object referencing it goes.
 *
 * The GC traces only each object's own view range: elements outside every
 * live view/owner range are unreachable through any list, so not marking
 * them is correct (the array itself lives until the last ref drops).
 * Buffers are plain malloc storage (not GC blocks), matching the old
 * items arrays, and are freed from the sweep via cs_seqbuf_unref. */
typedef struct cs_seqbuf {
  size_t refs;     /* owning cs_obj count (base + views) */
  size_t cap;      /* data capacity in cs_value elements */
  cs_value* data;  /* element array (never moved while refs > 1) */
} cs_seqbuf;

static void cs_seqbuf_unref(cs_seqbuf* b) {
  if (b == NULL) return;
  if (--b->refs == 0u) {
    free(b->data);
    free(b);
  }
}

/* Turn a plain (buf == NULL) sequence into a shared-buffer owner without
 * copying: the existing items array becomes the buffer's data. */
static cs_seqbuf* cs_seqbuf_adopt(struct cs_obj* o) {
  cs_seqbuf* b = o->as.seq.buf;
  if (b != NULL) return b;
  b = (cs_seqbuf*)malloc(sizeof *b);
  if (b == NULL) cs_runtime_error(0, "out of memory");
  b->refs = 1u;
  b->cap = o->as.seq.cap;
  b->data = o->as.seq.items;
  o->as.seq.buf = b;
  return b;
}

/* Copy a view (or shared owner) into a private plain array of at least
 * `mincap` elements, then release the shared buffer. */
static void cs_seq_detach(struct cs_obj* o, size_t mincap) {
  size_t i, n = o->as.seq.len;
  size_t cap = (mincap > n) ? mincap : n;
  cs_value* items = NULL;
  cs_seqbuf* old = o->as.seq.buf;
  if (cap > 0u) {
    items = (cs_value*)malloc(cap * sizeof(cs_value));
    if (items == NULL) cs_runtime_error(0, "out of memory");
    for (i = 0; i < n; i++) items[i] = cs_seq_at(o, i);
  }
  o->as.seq.items = items;
  o->as.seq.len = n;
  o->as.seq.cap = cap;
  o->as.seq.stride = 0;
  o->as.seq.buf = NULL;
  cs_seqbuf_unref(old);
}

/* Make `o` safe to write in place: it must be the sole owner of a
 * contiguous whole-buffer allocation. Shared owners and views detach. */
static void cs_seq_ensure_owner(struct cs_obj* o, size_t mincap) {
  if (o->as.seq.buf == NULL) return;
  if (o->as.seq.buf->refs == 1u && o->as.seq.stride == 0 &&
      o->as.seq.items == o->as.seq.buf->data &&
      o->as.seq.cap == o->as.seq.buf->cap)
    return;
  cs_seq_detach(o, mincap);
}

/* Sweep: reclaim unmarked blocks (freeing owned member arrays per kind)
 * and clear marks on survivors. */
static void cs_gc_sweep(void) {
  cs_gcblk* b = cs_gc_head;
  int64_t freed = 0;
  while (b != NULL) {
    cs_gcblk* next = b->h.next;
    if (!b->h.marked) {
      void* payload = (void*)(b + 1);
      unsigned char kind = b->h.kind;
      cs_gc_bytes -= b->h.sz;
      if (kind == CS_LIST || kind == CS_TUPLE) {
        cs_obj* so = (cs_obj*)payload;
        if (so->as.seq.buf != NULL) cs_seqbuf_unref(so->as.seq.buf);
        else free(so->as.seq.items);
      } else if (kind == CS_MAP || kind == CS_SET) {
        free(((cs_obj*)payload)->as.table.e);
        free(((cs_obj*)payload)->as.table.vals); /* NULL for sets */
        free(((cs_obj*)payload)->as.table.slots);
      } else if (kind == CS_INSTANCE) {
        free(((cs_obj*)payload)->as.inst.attrs.e);
        free(((cs_obj*)payload)->as.inst.attrs.vals);
        free(((cs_obj*)payload)->as.inst.attrs.slots);
      } else if (kind == CS_CLASS &&
                 !((cs_obj*)payload)->as.cls.is_exc) {
        free(((cs_obj*)payload)->as.cls.methods.e);
        free(((cs_obj*)payload)->as.cls.methods.vals);
        free(((cs_obj*)payload)->as.cls.methods.slots);
      } else if (kind == CS_FUNCTION) {
        /* Free the env array unless it is the inlined tail (cs_fun_value's
         * interior-pointer convention: env == ((char*)o + sizeof(cs_obj))
         * means the array lives inside this very block — an external heap
         * array can never equal the object's own interior address). */
        cs_cell** fenv = ((cs_obj*)payload)->as.fun.env;
        if (fenv != NULL && fenv != (cs_cell**)((char*)payload + sizeof(cs_obj)))
          free(fenv);
      } else if (kind == CS_ITERATOR) {
        free(((cs_obj*)payload)->as.itr.it); /* NULL-safe */
      } else if (kind == CS_FVEC || kind == CS_IVEC) {
        /* park the lane buffer for reuse instead of munmap'ing it —
         * exact capacity size, the key cs_vec_alloc asks for */
        cs_obj* vo = (cs_obj*)payload;
        if (vo->as.vec.data != NULL) {
          size_t vsz = (((vo->as.vec.len + 7u) / 8u) * 8u) * 8u;
          cs_gc_bytes -= vsz; /* balance cs_vec_alloc's accounting */
          cs_vec_buf_free(vo->as.vec.data, vsz);
        }
      } else if (kind == CS_FILE) {
        /* SPEC §5.11.5: a swept open file still closes its handle and
         * unlinks from the registry (cs_file_close_obj is idempotent);
         * only the sweep frees the path copy — close() keeps f.name
         * readable like Python. */
        cs_file_close_obj((cs_obj*)payload);
        free(((cs_obj*)payload)->as.file.path);
        ((cs_obj*)payload)->as.file.path = NULL;
      }
      if (b->h.prev != NULL) b->h.prev->h.next = next;
      else cs_gc_head = next;
      if (next != NULL) next->h.prev = b->h.prev;
      else cs_gc_tail = b->h.prev;
      if (CS_BP_PARKABLE(kind)) cs_bp_park(b); /* recycled at the next
        * allocation of this exact (size, kind) — see the parking docs */
      else free(b);
      freed++;
    } else {
      b->h.marked = 0;
    }
    b = next;
  }
  cs_gc_last_freed = freed;
  /* Rebuild the text/bytes payload set from the survivors (only inserts
   * happen between collections, so a rebuild is the whole delete). */
  cs_gc_tset_n = 0;
  if (cs_gc_tset_cap != 0)
    memset(cs_gc_tset, 0, cs_gc_tset_cap * sizeof(void*));
  for (b = cs_gc_head; b != NULL; b = b->h.next)
    if (b->h.kind == CS_GC_TEXT || b->h.kind == CS_GC_BYTES)
      cs_gc_tset_insert((void*)(b + 1));
}

void cs_gc_collect_full(void) {
  size_t i;
  cs_frame* f;
  /* Roots: globals, builtin class singletons (2C §2.2), shadow frames,
   * protect stack, handler-stack pendings (2C §2.5). */
  if (cs_gc_globals != NULL)
    for (i = 0; i < cs_gc_nglobals; i++) cs_gc_mark_value(*cs_gc_globals[i]);
  for (i = 0; i < CS_EXC_N; i++) cs_gc_mark_value(cs_exc_cls[i]);
  /* Immortal pinned cells (docs/reference/cross-scope-access.md §4): rooted forever. */
  for (i = 0; i < cs_gc_pinned_n; i++)
    cs_gc_push(cs_gc_blk_of(cs_gc_pinned[i]));
  for (f = cs_gc_frames; f != NULL; f = f->prev)
    for (i = 0; i < f->nslots; i++) cs_gc_mark_value(*f->slots[i]);
  for (i = 0; i < cs_gc_nprot; i++) cs_gc_mark_value(cs_gc_prot[i]);
  { /* Instance-method inline cache entries (guard 4 in the cache docs):
     * pin the cached class and method function objects so a swept class's
     * address can never be recycled under a live entry (and the cached
     * pointers stay dereferenceable). At most CS_MIC_SIZE of each. */
    size_t j;
    for (j = 0; j < CS_MIC_SIZE; j++) {
      if (cs_mic[j].cls != NULL) {
        cs_gc_push(cs_gc_blk_of(cs_mic[j].cls));
        cs_gc_push(cs_gc_blk_of(cs_mic[j].fn));
      }
    }
  }
  {
    cs_handler* h;
    for (h = cs_exc_top; h != NULL; h = h->prev) cs_gc_mark_value(h->exc);
  }
  /* Drain: trace members of every enqueued block (children get enqueued
   * by cs_gc_push and are drained in turn). */
  while (cs_gc_work_n > 0) {
    cs_gcblk* b = cs_gc_work[--cs_gc_work_n];
    void* payload = (void*)(b + 1);
    switch (b->h.kind) {
      case CS_GC_CELL: { /* leaf-with-value (cs_cell_alloc): trace v — a
                          * value reachable only through a captured cell
                          * must survive the collection that reclaims the
                          * frame that boxed it */
        cs_gc_mark_value(((cs_cell*)payload)->v);
        break;
      }
      case CS_LIST:
      case CS_TUPLE: {
        cs_obj* o = (cs_obj*)payload;
        for (i = 0; i < o->as.seq.len; i++)
          cs_gc_mark_value(cs_seq_at(o, i));
        break;
      }
      case CS_MAP:
      case CS_SET: {
        cs_obj* o = (cs_obj*)payload;
        for (i = 0; i < o->as.table.len; i++) {
          if (!o->as.table.e[i].used) continue;
          cs_gc_mark_value(o->as.table.e[i].key);
          if (o->as.table.vals != NULL) /* set tables carry none (§4) */
            cs_gc_mark_value(o->as.table.vals[i]);
        }
        break;
      }
      case CS_FUNCTION: {
        cs_obj* o = (cs_obj*)payload;
        for (i = 0; i < o->as.fun.env_n; i++) {
          cs_cell* c = o->as.fun.env[i];
          if (c != NULL) cs_gc_push(cs_gc_blk_of(c)); /* cells are blocks */
        }
        break;
      }
      case CS_EXCEPTION: { /* M2-2C §2: class value + message text */
        cs_obj* o = (cs_obj*)payload;
        cs_gc_mark_value(o->as.exc.cls);
        cs_gc_mark_value(o->as.exc.msg);
        break;
      }
      case CS_CLASS: { /* M2-2C §3: user-class method table */
        cs_obj* o = (cs_obj*)payload;
        if (o->as.cls.is_exc) break; /* builtin: leaf (static name) */
        for (i = 0; i < o->as.cls.methods.len; i++) {
          if (!o->as.cls.methods.e[i].used) continue;
          cs_gc_mark_value(o->as.cls.methods.e[i].key);
          if (o->as.cls.methods.vals != NULL)
            cs_gc_mark_value(o->as.cls.methods.vals[i]);
        }
        break;
      }
      case CS_INSTANCE: { /* M2-2C §3: class + instance fields */
        cs_obj* o = (cs_obj*)payload;
        uint32_t k;
        cs_gc_mark_value(o->as.inst.cls);
        for (k = 0; k < o->as.inst.n_inl; k++) { /* inline fields */
          if (CS_INST_INL(o)[k].key.tag == CS_UNSET) continue;
          cs_gc_mark_value(CS_INST_INL(o)[k].key);
          cs_gc_mark_value(CS_INST_INL(o)[k].val);
        }
        for (i = 0; i < o->as.inst.attrs.len; i++) {
          if (!o->as.inst.attrs.e[i].used) continue;
          cs_gc_mark_value(o->as.inst.attrs.e[i].key);
          if (o->as.inst.attrs.vals != NULL)
            cs_gc_mark_value(o->as.inst.attrs.vals[i]);
        }
        break;
      }
      case CS_BOUND_METHOD: { /* M2-2C §3: receiver + function */
        cs_obj* o = (cs_obj*)payload;
        cs_gc_mark_value(o->as.bnd.inst);
        cs_gc_mark_value(o->as.bnd.fn);
        break;
      }
      case CS_ITERATOR: { /* SPEC §5.11.5: the iterated source (a BOXED
                           * iterator's src IS the inner iterator obj) */
        cs_obj* o = (cs_obj*)payload;
        if (o->as.itr.it != NULL) cs_gc_mark_value(o->as.itr.it->src);
        break;
      }
      default:
        break; /* leaf kinds (text/bytes/cell/CS_FILE: no traced members) */
    }
  }
  cs_gc_sweep();
  /* Next threshold: double the surviving bytes, never below 1 MiB. */
  cs_gc_limit = cs_gc_bytes * 2u;
  if (cs_gc_limit < 1048576u) cs_gc_limit = 1048576u;
}

/* cs_gc_maybe is the header's static inline (threshold check + cold call). */

int64_t cs_builtin_collect(void) {
  cs_gc_collect_full();
  return cs_gc_last_freed;
}

/* ---- shadow stack & protect stack -------------------------------------
 * cs_frame_begin/end and cs_gc_protect/unprotect moved to cstar_rt.h as
 * static inline (like cs_guard/cs_truthy): each is a few stores, and as
 * cross-object calls they cost ~15-20ns per function invocation — a large
 * tax on call-heavy code. Behavior identical. */

/* ---- arithmetic (§4) --------------------------------------------------- */

/* ---- fvec/ivec elementwise ops (SIMD vector types) ----------------------
 * Defined in the vector section below; every operator routes vector
 * operands here first (invalid pairings fall through to cs_bad_binary
 * with the standard wording). */
static cs_value cs_vec_binop(cs_value a, cs_value b, int line, char op);

#define CS_IF_VEC(a, b) \
  ((a).tag == CS_FVEC || (a).tag == CS_IVEC || (b).tag == CS_FVEC || \
   (b).tag == CS_IVEC)

cs_value cs_add(cs_value a, cs_value b, int line) {
  cs_no_missing2(a, b, line); /* M2-2B §13 */
  if (CS_IF_VEC(a, b)) return cs_vec_binop(a, b, line, '+');
  if (a.tag == CS_STRING && b.tag == CS_STRING) return cs_concat(a, b, line);
  if (a.tag == CS_BYTES && b.tag == CS_BYTES) return cs_concat(a, b, line);
  if (!cs_is_numeric(a) || !cs_is_numeric(b)) cs_bad_binary(line, "+", a, b);
  if (a.tag == CS_INT && b.tag == CS_INT) return cs_int(cs_wrap_add(a.as.i, b.as.i));
  return cs_float(cs_as_double(a) + cs_as_double(b));
}

cs_value cs_sub(cs_value a, cs_value b, int line) {
  cs_no_missing2(a, b, line);
  if (CS_IF_VEC(a, b)) return cs_vec_binop(a, b, line, '-');
  if (!cs_is_numeric(a) || !cs_is_numeric(b)) cs_bad_binary(line, "-", a, b);
  if (a.tag == CS_INT && b.tag == CS_INT) return cs_int(cs_wrap_sub(a.as.i, b.as.i));
  return cs_float(cs_as_double(a) - cs_as_double(b));
}

cs_value cs_mul(cs_value a, cs_value b, int line) {
  cs_no_missing2(a, b, line);
  if (CS_IF_VEC(a, b)) return cs_vec_binop(a, b, line, '*');
  if (!cs_is_numeric(a) || !cs_is_numeric(b)) cs_bad_binary(line, "*", a, b);
  if (a.tag == CS_INT && b.tag == CS_INT) return cs_int(cs_wrap_mul(a.as.i, b.as.i));
  return cs_float(cs_as_double(a) * cs_as_double(b));
}

cs_value cs_div(cs_value a, cs_value b, int line) {
  cs_no_missing2(a, b, line);
  if (CS_IF_VEC(a, b)) return cs_vec_binop(a, b, line, '/');
  if (!cs_is_numeric(a) || !cs_is_numeric(b)) cs_bad_binary(line, "/", a, b);
  if (a.tag == CS_INT && b.tag == CS_INT) {
    if (b.as.i == 0) cs_runtime_error_as("ZeroDivisionError", line, "division by zero");
    /* INT64_MIN / -1 would overflow; int arithmetic wraps. */
    if (a.as.i == INT64_MIN && b.as.i == -1) return cs_int(INT64_MIN);
    return cs_int(a.as.i / b.as.i); /* C99: truncates toward zero */
  }
  {
    double x = cs_as_double(a), y = cs_as_double(b);
    if (y == 0.0) cs_runtime_error_as("ZeroDivisionError", line, "division by zero");
    return cs_float(x / y);
  }
}

cs_value cs_mod(cs_value a, cs_value b, int line) {
  cs_no_missing2(a, b, line);
  if (CS_IF_VEC(a, b)) return cs_vec_binop(a, b, line, '%');
  if (a.tag != CS_INT || b.tag != CS_INT) cs_bad_binary(line, "%", a, b);
  if (b.as.i == 0) cs_runtime_error_as("ZeroDivisionError", line, "modulo by zero");
  if (a.as.i == INT64_MIN && b.as.i == -1) return cs_int(0); /* would be UB */
  return cs_int(a.as.i % b.as.i); /* C semantics: sign follows dividend */
}

cs_value cs_neg(cs_value a, int line) {
  cs_no_missing(a, line);
  if (a.tag == CS_INT) return cs_int((int64_t)((uint64_t)0 - (uint64_t)a.as.i));
  if (a.tag == CS_FLOAT) return cs_float(-a.as.d);
  if (a.tag == CS_FVEC || a.tag == CS_IVEC) {
    size_t i, n = a.as.o->as.vec.len;
    if (a.tag == CS_FVEC) {
      cs_obj* r = cs_fvec_new(n, line);
      double* rd = (double*)r->as.vec.data;
      const double* ad = (const double*)a.as.o->as.vec.data;
      for (i = 0; i < n; i++) rd[i] = -ad[i];
      return cs_fvec_val(r);
    } else {
      cs_obj* r = cs_ivec_new(n, line);
      int64_t* rd = (int64_t*)r->as.vec.data;
      const int64_t* ad = (const int64_t*)a.as.o->as.vec.data;
      for (i = 0; i < n; i++) rd[i] = (int64_t)((uint64_t)0 - (uint64_t)ad[i]);
      return cs_ivec_val(r);
    }
  }
  cs_runtime_error_as("TypeError", line, "unsupported operand type for unary -: %s", cs_type_name(a));
}

/* ---- ordering comparisons (§4) ---------------------------------------- */

typedef enum cs_ord {
  CS_ORD_LESS = 0,
  CS_ORD_EQUAL = 1,
  CS_ORD_GREATER = 2,
  CS_ORD_UNORDERED = 3 /* NaN operands: < <= > >= are all False */
} cs_ord;

static cs_ord cs_cmp3(cs_value a, cs_value b, int line, const char* op) {
  cs_no_missing2(a, b, line); /* M2-2B §13 */
  if (a.tag == CS_INT && b.tag == CS_INT) {
    if (a.as.i < b.as.i) return CS_ORD_LESS;
    if (a.as.i > b.as.i) return CS_ORD_GREATER;
    return CS_ORD_EQUAL;
  }
  if (cs_is_numeric(a) && cs_is_numeric(b)) {
    /* long double is exact for every int64 and every double on x86_64, so
     * mixed int/float comparison is mathematically exact (1 vs 1.0 etc.). */
    long double x = cs_as_ld(a);
    long double y = cs_as_ld(b);
    if (x < y) return CS_ORD_LESS;
    if (x > y) return CS_ORD_GREATER;
    if (x == y) return CS_ORD_EQUAL;
    return CS_ORD_UNORDERED;
  }
  if ((a.tag == CS_STRING || a.tag == CS_BYTES) && a.tag == b.tag) {
    /* Text and Bytes both compare byte-wise lexicographically (M1 string
     * rule; M2-2B §5 extends it to Bytes). Mixed text/bytes → type error. */
    int c = cs_str_cmp(a, b);
    return c < 0 ? CS_ORD_LESS : (c > 0 ? CS_ORD_GREATER : CS_ORD_EQUAL);
  }
  cs_bad_binary(line, op, a, b);
  return CS_ORD_UNORDERED; /* not reached: cs_bad_binary exits */
}

cs_value cs_lt(cs_value a, cs_value b, int line) {
  return cs_bool(cs_cmp3(a, b, line, "<") == CS_ORD_LESS);
}
cs_value cs_le(cs_value a, cs_value b, int line) {
  cs_ord o = cs_cmp3(a, b, line, "<=");
  return cs_bool(o == CS_ORD_LESS || o == CS_ORD_EQUAL);
}
cs_value cs_gt(cs_value a, cs_value b, int line) {
  return cs_bool(cs_cmp3(a, b, line, ">") == CS_ORD_GREATER);
}
cs_value cs_ge(cs_value a, cs_value b, int line) {
  cs_ord o = cs_cmp3(a, b, line, ">=");
  return cs_bool(o == CS_ORD_GREATER || o == CS_ORD_EQUAL);
}

/* ---- total equality (§4) ---------------------------------------------- */

static int cs_value_eq(cs_value a, cs_value b) {
  /* M2-2B §13: the sentinel is never observable in user operations. */
  if (a.tag == CS_MISSING || b.tag == CS_MISSING)
    cs_runtime_error(0, "internal error: missing argument");
  /* Numeric mixing compares by mathematical value: 1 == 1.0 is True. */
  if ((a.tag == CS_INT && b.tag == CS_FLOAT) ||
      (a.tag == CS_FLOAT && b.tag == CS_INT)) {
    long double x = cs_as_ld(a);
    long double y = cs_as_ld(b);
    return x == y;
  }
  if (a.tag != b.tag) return 0; /* different tags: 1 == "1", 1 == True */
  switch (a.tag) {
    case CS_UNSET: return 1; /* unreachable: unset is never observable */
    case CS_NONE: return 1;
    case CS_INT: return a.as.i == b.as.i;
    case CS_FLOAT: return a.as.d == b.as.d;
    case CS_BOOL: return a.as.b == b.as.b;
    case CS_STRING:
    case CS_BYTES: /* text vs bytes are distinct tags → never equal */
      if (a.as.s.p == b.as.s.p && a.as.s.len == b.as.s.len)
        return 1; /* same storage (merged .rodata name literals behind
                   * cs_text_lit): skip the memcmp entirely — the instance
                   * attribute scan compares its inline keys this way */
      return a.as.s.len == b.as.s.len &&
             (a.as.s.len == 0 || memcmp(a.as.s.p, b.as.s.p, a.as.s.len) == 0);
    case CS_RANGE: /* M2-2A: two ranges are equal iff start/stop/step match */
      return a.as.r.start == b.as.r.start && a.as.r.stop == b.as.r.stop &&
             a.as.r.step == b.as.r.step;
    /* ---- M2-2B §4 total equality for the new types ---- */
    case CS_LIST:
    case CS_TUPLE: {
      size_t i;
      if (a.as.o->as.seq.len != b.as.o->as.seq.len) return 0;
      for (i = 0; i < a.as.o->as.seq.len; i++)
        if (!cs_value_eq(cs_seq_at(a.as.o, i), cs_seq_at(b.as.o, i)))
          return 0;
      return 1;
    }
    case CS_FVEC: { /* same-tag elementwise (NaN != NaN, like floats) */
      size_t i, n = a.as.o->as.vec.len;
      const double* ad = (const double*)a.as.o->as.vec.data;
      const double* bd = (const double*)b.as.o->as.vec.data;
      if (n != b.as.o->as.vec.len) return 0;
      for (i = 0; i < n; i++)
        if (ad[i] != bd[i]) return 0;
      return 1;
    }
    case CS_IVEC: {
      size_t i, n = a.as.o->as.vec.len;
      const int64_t* ad = (const int64_t*)a.as.o->as.vec.data;
      const int64_t* bd = (const int64_t*)b.as.o->as.vec.data;
      if (n != b.as.o->as.vec.len) return 0;
      for (i = 0; i < n; i++)
        if (ad[i] != bd[i]) return 0;
      return 1;
    }
    case CS_SET: {
      const cs_table* ta;
      const cs_table* tb;
      size_t i;
      if (a.as.o == b.as.o) return 1;
      ta = &a.as.o->as.table;
      tb = &b.as.o->as.table;
      if (ta->live != tb->live) return 0;
      for (i = 0; i < ta->len; i++) {
        size_t j;
        if (!ta->e[i].used) continue;
        for (j = 0; j < tb->len; j++) {
          if (tb->e[j].used &&
              cs_value_eq(ta->e[i].key, tb->e[j].key))
            break;
        }
        if (j == tb->len) return 0;
      }
      return 1;
    }
    case CS_MAP: {
      const cs_table* ta;
      const cs_table* tb;
      size_t i;
      if (a.as.o == b.as.o) return 1;
      ta = &a.as.o->as.table;
      tb = &b.as.o->as.table;
      if (ta->live != tb->live) return 0;
      for (i = 0; i < ta->len; i++) {
        size_t j;
        if (!ta->e[i].used) continue;
        for (j = 0; j < tb->len; j++) {
          if (tb->e[j].used && cs_value_eq(ta->e[i].key, tb->e[j].key)) {
            if (!cs_value_eq(ta->vals[i], tb->vals[j])) return 0;
            break;
          }
        }
        if (j == tb->len) return 0; /* key missing in b */
      }
      return 1;
    }
    case CS_FUNCTION: /* §13: function equality is by meta identity */
      return a.as.o == b.as.o ||
             a.as.o->as.fun.meta == b.as.o->as.fun.meta;
    case CS_CLASS:        /* M2-2C §2: classes compare by identity */
    case CS_EXCEPTION:    /* M2-2C §2: instances compare by identity */
    case CS_INSTANCE:     /* M2-2C §3 */
    case CS_BOUND_METHOD: /* M2-2C §3 */
    case CS_ITERATOR:     /* SPEC §5.11.5: iterators/files by identity */
    case CS_FILE:
      return a.as.o == b.as.o;
    case CS_MISSING:
    default:
      return 0; /* unreachable: handled at entry */
  }
}

cs_value cs_eq(cs_value a, cs_value b) { return cs_bool(cs_value_eq(a, b) != 0); }
cs_value cs_ne(cs_value a, cs_value b) { return cs_bool(cs_value_eq(a, b) == 0); }

/* ---- print (§4, normative formatting) --------------------------------- */

/* Shortest round-trip: first of %.15g / %.16g / %.17g whose output parses
 * back to the identical double; then ".0" when the text has no '.', 'e' or
 * 'n' character (the 'n' covers "inf"/"nan"), so 5.0 prints `5.0`. */
static void cs_fmt_float(double v, char* buf, size_t cap) {
  if (isfinite(v)) {
    static const int precisions[3] = {15, 16, 17};
    size_t i;
    for (i = 0; i < sizeof(precisions) / sizeof(precisions[0]); i++) {
      char* end = NULL;
      double back;
      snprintf(buf, cap, "%.*g", precisions[i], v);
      back = strtod(buf, &end);
      if (end != buf && *end == '\0' && back == v) break; /* round-trips */
    }
    /* The loop always accepts (at worst %.17g is exact for doubles). */
  } else {
    snprintf(buf, cap, "%.17g", v); /* "inf", "-inf", "nan", "-nan" */
  }
  if (strpbrk(buf, ".en") != NULL) return;
  {
    size_t len = strlen(buf);
    if (len + 3 <= cap) {
      buf[len] = '.';
      buf[len + 1] = '0';
      buf[len + 2] = '\0';
    }
  }
}

/* ---- M2-2B §5: UTF-8 helpers ----------------------------------------- */

/* Strict UTF-8 validation for decode(): rejects truncated sequences,
 * overlong encodings, surrogates (U+D800..U+DFFF) and values above
 * U+10FFFF. Interior NUL bytes are legal. */
static bool cs_utf8_valid(const char* p, size_t len) {
  size_t i = 0;
  while (i < len) {
    unsigned char c = (unsigned char)p[i];
    if (c < 0x80u) {
      i += 1;
    } else if (c >= 0xc2u && c <= 0xdfu) {
      if (i + 1 >= len || ((unsigned char)p[i + 1] & 0xc0u) != 0x80u) return false;
      i += 2;
    } else if (c == 0xe0u) {
      if (i + 2 >= len) return false;
      if ((unsigned char)p[i + 1] < 0xa0u || (unsigned char)p[i + 1] > 0xbfu ||
          ((unsigned char)p[i + 2] & 0xc0u) != 0x80u)
        return false;
      i += 3;
    } else if (c >= 0xe1u && c <= 0xecu) {
      if (i + 2 >= len || ((unsigned char)p[i + 1] & 0xc0u) != 0x80u ||
          ((unsigned char)p[i + 2] & 0xc0u) != 0x80u)
        return false;
      i += 3;
    } else if (c == 0xedu) { /* excludes UTF-16 surrogates */
      if (i + 2 >= len || (unsigned char)p[i + 1] < 0x80u ||
          (unsigned char)p[i + 1] > 0x9fu ||
          ((unsigned char)p[i + 2] & 0xc0u) != 0x80u)
        return false;
      i += 3;
    } else if (c >= 0xeeu && c <= 0xefu) {
      if (i + 2 >= len || ((unsigned char)p[i + 1] & 0xc0u) != 0x80u ||
          ((unsigned char)p[i + 2] & 0xc0u) != 0x80u)
        return false;
      i += 3;
    } else if (c == 0xf0u) {
      if (i + 3 >= len || (unsigned char)p[i + 1] < 0x90u ||
          (unsigned char)p[i + 1] > 0xbfu ||
          ((unsigned char)p[i + 2] & 0xc0u) != 0x80u ||
          ((unsigned char)p[i + 3] & 0xc0u) != 0x80u)
        return false;
      i += 4;
    } else if (c >= 0xf1u && c <= 0xf3u) {
      if (i + 3 >= len || ((unsigned char)p[i + 1] & 0xc0u) != 0x80u ||
          ((unsigned char)p[i + 2] & 0xc0u) != 0x80u ||
          ((unsigned char)p[i + 3] & 0xc0u) != 0x80u)
        return false;
      i += 4;
    } else if (c == 0xf4u) {
      if (i + 3 >= len || (unsigned char)p[i + 1] > 0x8fu ||
          ((unsigned char)p[i + 1] & 0xc0u) != 0x80u ||
          ((unsigned char)p[i + 2] & 0xc0u) != 0x80u ||
          ((unsigned char)p[i + 3] & 0xc0u) != 0x80u)
        return false;
      i += 4;
    } else {
      return false;
    }
  }
  return true;
}

/* Decodes the code point starting at p+pos. All text sources are
 * validated at construction, so this always succeeds; as a defensive
 * fallback an invalid or truncated lead byte is emitted verbatim as a
 * 1-byte pseudo code point (never splits, never reads out of bounds).
 * *adv is set to the consumed byte count (>= 1). */
static uint32_t cs_utf8_step(const char* p, size_t len, size_t pos,
                             size_t* adv) {
  unsigned char c = (unsigned char)p[pos];
  size_t rem = len - pos;
  if (c < 0x80u) {
    *adv = 1;
    return c;
  }
  if (c >= 0xc2u && c <= 0xdfu && rem >= 2 &&
      ((unsigned char)p[pos + 1] & 0xc0u) == 0x80u) {
    *adv = 2;
    return (uint32_t)(((uint32_t)(c & 0x1fu) << 6) |
                      (uint32_t)((unsigned char)p[pos + 1] & 0x3fu));
  }
  if (rem >= 3 && ((unsigned char)p[pos + 2] & 0xc0u) == 0x80u) {
    unsigned char c1 = (unsigned char)p[pos + 1];
    if ((c1 & 0xc0u) == 0x80u) {
      uint32_t cp;
      bool ok = false;
      if (c == 0xe0u)
        ok = c1 >= 0xa0u && c1 <= 0xbfu;
      else if (c == 0xedu)
        ok = c1 >= 0x80u && c1 <= 0x9fu; /* no surrogates */
      else if (c >= 0xe1u && c <= 0xefu)
        ok = true;
      if (ok) {
        cp = (uint32_t)((c & 0x0fu) << 12) |
             (uint32_t)((c1 & 0x3fu) << 6) |
             (uint32_t)((unsigned char)p[pos + 2] & 0x3fu);
        *adv = 3;
        return cp;
      }
    }
  }
  if (rem >= 4 && ((unsigned char)p[pos + 3] & 0xc0u) == 0x80u &&
      ((unsigned char)p[pos + 2] & 0xc0u) == 0x80u) {
    unsigned char c1 = (unsigned char)p[pos + 1];
    if ((c1 & 0xc0u) == 0x80u) {
      uint32_t cp;
      bool ok = false;
      if (c == 0xf0u)
        ok = c1 >= 0x90u && c1 <= 0xbfu;
      else if (c == 0xf4u)
        ok = c1 >= 0x80u && c1 <= 0x8fu;
      else if (c >= 0xf1u && c <= 0xf3u)
        ok = true;
      if (ok) {
        cp = (uint32_t)((c & 0x07u) << 18) |
             (uint32_t)((c1 & 0x3fu) << 12) |
             (uint32_t)((unsigned char)p[pos + 2] & 0x3fu) << 6 |
             (uint32_t)((unsigned char)p[pos + 3] & 0x3fu);
        *adv = 4;
        return cp;
      }
    }
  }
  *adv = 1; /* defensive: emit the raw byte */
  return c;
}

/* Full code-point count of [0, len). ASCII runs are skipped in bulk: a
 * byte < 0x80 is ALWAYS its own code point (cs_utf8_step's first branch
 * is context-free, and a continuation byte is never < 0x80, so no
 * multi-byte sequence can start or hide inside the run) — the partition
 * is bit-for-bit the one the step loop produces, byte for byte. */
static size_t cs_utf8_count(const char* p, size_t len) {
  size_t pos = 0, n = 0;
  while (pos < len) {
    if ((unsigned char)p[pos] < 0x80u) {
      size_t k = 0;
      while (pos + k < len && (unsigned char)p[pos + k] < 0x80u) k++;
      pos += k;
      n += k;
      continue;
    }
    {
      size_t adv;
      (void)cs_utf8_step(p, len, pos, &adv);
      pos += adv;
      n++;
    }
  }
  return n;
}

/* Byte offset of the i'th code point; i may equal the code point count
 * (the byte length) — used for code-point-index → byte-offset mapping.
 * ASCII runs advance both counters in bulk (same partition argument as
 * cs_utf8_count above; the run is clipped to the remaining i - n code
 * points, so the result stops exactly where the step loop would). */
static size_t cs_utf8_byte(const char* p, size_t len, size_t i) {
  size_t pos = 0, n = 0;
  while (pos < len && n < i) {
    if ((unsigned char)p[pos] < 0x80u) {
      size_t run = len - pos;
      size_t room = i - n;
      size_t k = 0;
      if (run > room) run = room;
      while (k < run && (unsigned char)p[pos + k] < 0x80u) k++;
      if (k > 0u) {
        pos += k;
        n += k;
        continue;
      }
    }
    {
      size_t adv;
      (void)cs_utf8_step(p, len, pos, &adv);
      pos += adv;
      n++;
    }
  }
  return pos;
}

/* ---- memoized code-point queries (perf: unicode_scan / text_find) ------ *
 * The three O(len)-per-query entry points — full count (len(t)), byte
 * offset of the i'th code point (t[i], slices, unpack) and the prefix
 * count find() turns its byte hit into — all funnel through the single
 * memo declared with the GC state above. Correctness contract:
 *   - only REGISTERED heap payloads are ever named (cs_cp_bind), and only
 *     while payload pointer + length + allocation generation all match;
 *     literals and plain-malloc buffers keep the direct, pre-memo code;
 *   - text bytes are immutable except cat2's in-place append, which
 *     changes the length — part of the key — so a named view's bytes can
 *     never change underneath a cached fact;
 *   - each cursor only ever advances over positions it established
 *     itself under the same bound (offset cursor: always bound = len of
 *     the named view; prefix cursor: established only by prefix walks,
 *     re-entered only at the identical offset or at a boundary >= 4
 *     bytes behind, where any bound disagreement could first occur —
 *     never in validated UTF-8). A mismatch falls back to the exact old
 *     walk, so worst case == baseline, never a wrong byte. */
#define CS_CP_DIRECT_MAX 32u /* below this, scan directly: probing costs more */

/* True when the memo names exactly (p, len) — same payload, same length,
 * same allocation generation (no sweep-and-reuse since capture). Order
 * matters: the cheap key compares run before the tset probe. */
static bool cs_cp_named(const char* p, size_t len) {
  return cs_cp.valid != 0u && cs_cp.base == p && cs_cp.len == len &&
         cs_gc_tset_has((const void*)p) &&
         cs_gc_blk_of((const void*)p)->h.gen == cs_cp.gen;
}

/* Point the memo at (p, len) with fresh (unknown) facts, iff p is a
 * registered heap text payload; otherwise leave the memo alone and
 * return false (literal / unknown buffer → caller scans directly).
 * Reads only header state behind a tset hit, never p's bytes. */
static bool cs_cp_bind(const char* p, size_t len) {
  if (cs_cp_named(p, len)) return true;
  if (p != NULL && cs_gc_tset_has((const void*)p)) {
    cs_cp.base = p;
    cs_cp.len = len;
    cs_cp.gen = cs_gc_blk_of((const void*)p)->h.gen;
    cs_cp.total = 0;
    cs_cp.total_ok = 0;
    cs_cp.it_cp = 0;
    cs_cp.it_off = 0;
    cs_cp.pr_cp = 0;
    cs_cp.pr_off = 0;
    cs_cp.valid = 1;
    return true;
  }
  return false;
}

/* Code-point count of a text value's whole byte range: the per-query
 * cost of every len(t) and the implicit length inside each t[i]. */
static size_t cs_text_count(const char* p, size_t len) {
  size_t n;
  if (len == 0u) return 0u;
  if (len > CS_CP_DIRECT_MAX && cs_cp_bind(p, len)) {
    if (cs_cp.total_ok) return cs_cp.total;
    n = cs_utf8_count(p, len);
    cs_cp.total = n;
    cs_cp.total_ok = 1;
    return n;
  }
  return cs_utf8_count(p, len);
}

/* Byte offset of the i'th code point of a text value (i may equal the
 * code point count → len). The monotone cursor turns a sequential
 * t[i] loop into one linear walk instead of a full re-decode per index;
 * a random-access step backwards restarts exactly where the old code
 * did — at byte 0. */
static size_t cs_text_byte(const char* p, size_t len, size_t i) {
  size_t pos, n, adv;
  if (len <= CS_CP_DIRECT_MAX || !cs_cp_bind(p, len))
    return cs_utf8_byte(p, len, i);
  pos = 0;
  n = 0;
  if (i >= cs_cp.it_cp) { /* resume the established truth point */
    pos = cs_cp.it_off;
    n = cs_cp.it_cp;
  }
  while (pos < len && n < i) {
    if ((unsigned char)p[pos] < 0x80u) {
      size_t run = len - pos;
      size_t room = i - n;
      size_t k = 0;
      if (run > room) run = room;
      while (k < run && (unsigned char)p[pos + k] < 0x80u) k++;
      if (k > 0u) {
        pos += k;
        n += k;
        continue;
      }
    }
    (void)cs_utf8_step(p, len, pos, &adv);
    pos += adv;
    n++;
  }
  cs_cp.it_cp = n; /* truth point: prefix [0, pos) holds exactly n code points */
  cs_cp.it_off = pos;
  return pos;
}

/* Code points in [0, off) — find()'s byte-hit → index conversion. The
 * repeated-same-offset case (searching one haystack in a loop) answers
 * from the cursor in O(1); a forward step walks from the cursor, a
 * backward/tail step restarts from byte 0 with the queried bound —
 * byte-identical to the old cs_utf8_count(p, off) by construction. */
static size_t cs_text_prefix(const char* p, size_t len, size_t off) {
  bool named;
  size_t n, pos, adv;
  if (off == 0u) return 0u;
  named = (len > CS_CP_DIRECT_MAX) && cs_cp_bind(p, len);
  if (named) {
    if (off == cs_cp.pr_off) return cs_cp.pr_cp;
    if (off > cs_cp.pr_off && off - cs_cp.pr_off >= 4u) {
      pos = cs_cp.pr_off;
      n = cs_cp.pr_cp;
      while (pos < off) {
        (void)cs_utf8_step(p, off, pos, &adv);
        pos += adv;
        n++;
      }
      cs_cp.pr_cp = n;
      cs_cp.pr_off = off;
      return n;
    }
  }
  n = cs_utf8_count(p, off);
  if (named) {
    cs_cp.pr_cp = n;
    cs_cp.pr_off = off;
  }
  return n;
}

/* ---- M2-2B §2: display buffer ---------------------------------------- */

typedef struct cs_buf {
  char* p;
  size_t len;
  size_t cap;
} cs_buf;

static void cs_buf_reserve(cs_buf* b, size_t extra, int line) {
  if (b->len + extra + 1 > b->cap) {
    size_t cap = (b->cap != 0) ? b->cap : 32u;
    while (cap < b->len + extra + 1) cap *= 2u;
    b->p = (char*)realloc(b->p, cap);
    if (b->p == NULL) cs_runtime_error(line, "out of memory");
    b->cap = cap;
  }
}

static void cs_buf_addn(cs_buf* b, const char* s, size_t n, int line) {
  if (n == 0u) return;
  /* No single append can reach PTRDIFF_MAX (9 EB — no such object can
   * exist on this target), so a size that large can only come from a
   * length underflow upstream. Die loudly (noreturn: GCC then sees the
   * memcpy bound as at most PTRDIFF_MAX — the object-size limit — and
   * keeps every clone of this helper warning-free). */
  if (n > (size_t)PTRDIFF_MAX)
    cs_runtime_error(line, "buffer size out of range");
  cs_buf_reserve(b, n, line);
  memcpy(b->p + b->len, s, n);
  b->len += n;
}

static void cs_buf_addc(cs_buf* b, char c, int line) {
  cs_buf_addn(b, &c, 1u, line);
}

static void cs_buf_end(cs_buf* b, int line) {
  cs_buf_reserve(b, 0u, line);
  b->p[b->len] = '\0';
}

/* ---- M2-2C §1: GC-registered text results ----------------------------- *
 * Transient buffers are runtime-internal; RESULTS must live in
 * registered blocks so the collector can reclaim them, and text that
 * points INTO another text's storage must be copied (an interior pointer
 * would dangle once the source block is swept). */

static cs_value cs_text_from_buf(cs_buf* b, int line) {
  size_t len = b->len;
  char* p = NULL;
  if (len > 0u) {
    p = cs_gc_buf_filled(CS_GC_TEXT, len, line); /* full write below */
    memcpy(p, b->p, len);
    p[len] = '\0';
  }
  free(b->p);
  b->p = NULL;
  b->len = 0;
  b->cap = 0;
  return cs_str(p, len);
}

static cs_value cs_text_copy(const char* src, size_t len, int line) {
  char* p = NULL;
  if (len > 0u) {
    p = cs_gc_buf_filled(CS_GC_TEXT, len, line); /* full write below */
    memcpy(p, src, len);
    p[len] = '\0';
  }
  return cs_str(p, len);
}

/* §2 bytes display: b"..." with printable ASCII (0x20-0x7E except " and
 * \) literal, `"` → `\"`, `\` → `\\`, everything else → \xHH (lowercase
 * hex). */
static void cs_display_bytes(cs_buf* b, cs_value v, int line) {
  size_t i;
  cs_buf_addn(b, "b\"", 2u, line);
  for (i = 0; i < v.as.s.len; i++) {
    unsigned char c = (unsigned char)v.as.s.p[i];
    if (c == (unsigned char)'\"')
      cs_buf_addn(b, "\\\"", 2u, line);
    else if (c == (unsigned char)'\\')
      cs_buf_addn(b, "\\\\", 2u, line);
    else if (c >= 0x20u && c <= 0x7eu)
      cs_buf_addc(b, (char)c, line);
    else {
      char hex[5];
      snprintf(hex, sizeof(hex), "\\x%02x", (unsigned)c);
      cs_buf_addn(b, hex, 4u, line);
    }
  }
  cs_buf_addc(b, '\"', line);
}

/* §2 recursive display / str() form for every type. */
static void cs_display_buf(cs_buf* b, cs_value v, int line) {
  size_t i;
  switch (v.tag) {
    case CS_INT: {
      char buf[32];
      /* cs_itoa_ll is byte-identical to snprintf("%lld") — this is the
       * same substitution the print fast path already runs (§11/§13);
       * itoa + the known length, no second strlen pass. */
      cs_buf_addn(b, buf, (size_t)cs_itoa_ll(buf, v.as.i), line);
      break;
    }
    case CS_FLOAT: {
      char buf[64];
      cs_fmt_float(v.as.d, buf, sizeof(buf));
      cs_buf_addn(b, buf, strlen(buf), line);
      break;
    }
    case CS_BOOL:
      cs_buf_addn(b, v.as.b ? "True" : "False", v.as.b ? 4u : 5u, line);
      break;
    case CS_NONE:
      cs_buf_addn(b, "None", 4u, line);
      break;
    case CS_STRING:
      cs_buf_addn(b, v.as.s.p, v.as.s.len, line);
      break;
    case CS_BYTES:
      cs_display_bytes(b, v, line);
      break;
    case CS_ITERATOR: /* SPEC §5.11.5: stable form — no address leak */
      cs_buf_addn(b, "<iterator>", 10u, line);
      break;
    case CS_FILE: /* SPEC §5.11.5: <file 'path'> */
      cs_buf_addn(b, "<file '", 7u, line);
      cs_buf_addn(b, v.as.o->as.file.path, strlen(v.as.o->as.file.path),
                  line);
      cs_buf_addn(b, "'>", 2u, line);
      break;
    case CS_RANGE:
      cs_buf_addn(b, "range(", 6u, line);
      cs_display_buf(b, cs_int(v.as.r.start), line);
      cs_buf_addn(b, ", ", 2u, line);
      cs_display_buf(b, cs_int(v.as.r.stop), line);
      cs_buf_addn(b, ", ", 2u, line);
      cs_display_buf(b, cs_int(v.as.r.step), line);
      cs_buf_addc(b, ')', line);
      break;
    case CS_LIST:
      cs_buf_addc(b, '[', line);
      for (i = 0; i < v.as.o->as.seq.len; i++) {
        if (i > 0) cs_buf_addn(b, ", ", 2u, line);
        cs_display_buf(b, cs_seq_at(v.as.o, i), line);
      }
      cs_buf_addc(b, ']', line);
      break;
    case CS_FVEC: { /* renders list-style: [1.0, 2.0] */
      size_t k, n = v.as.o->as.vec.len;
      const double* d = (const double*)v.as.o->as.vec.data;
      cs_buf_addc(b, '[', line);
      for (k = 0; k < n; k++) {
        if (k > 0) cs_buf_addn(b, ", ", 2u, line);
        cs_display_buf(b, cs_float(d[k]), line);
      }
      cs_buf_addc(b, ']', line);
      break;
    }
    case CS_IVEC: {
      size_t k, n = v.as.o->as.vec.len;
      const int64_t* d = (const int64_t*)v.as.o->as.vec.data;
      cs_buf_addc(b, '[', line);
      for (k = 0; k < n; k++) {
        if (k > 0) cs_buf_addn(b, ", ", 2u, line);
        cs_display_buf(b, cs_int(d[k]), line);
      }
      cs_buf_addc(b, ']', line);
      break;
    }
    case CS_TUPLE:
      if (v.as.o->as.seq.len == 1u) { /* 1-tuple: `(e,)` */
        cs_buf_addc(b, '(', line);
        cs_display_buf(b, cs_seq_at(v.as.o, 0), line);
        cs_buf_addn(b, ",)", 2u, line);
        break;
      }
      cs_buf_addc(b, '(', line);
      for (i = 0; i < v.as.o->as.seq.len; i++) {
        if (i > 0) cs_buf_addn(b, ", ", 2u, line);
        cs_display_buf(b, cs_seq_at(v.as.o, i), line);
      }
      cs_buf_addc(b, ')', line);
      break;
    case CS_SET: {
      const cs_table* t = &v.as.o->as.table;
      bool first = true;
      cs_buf_addc(b, '{', line);
      for (i = 0; i < t->len; i++) {
        if (!t->e[i].used) continue;
        if (!first) cs_buf_addn(b, ", ", 2u, line);
        first = false;
        cs_display_buf(b, t->e[i].key, line);
      }
      cs_buf_addc(b, '}', line);
      break;
    }
    case CS_MAP: {
      const cs_table* t = &v.as.o->as.table;
      bool first = true;
      cs_buf_addc(b, '{', line);
      for (i = 0; i < t->len; i++) {
        if (!t->e[i].used) continue;
        if (!first) cs_buf_addn(b, ", ", 2u, line);
        first = false;
        cs_display_buf(b, t->e[i].key, line);
        cs_buf_addn(b, ": ", 2u, line);
        cs_display_buf(b, t->vals[i], line);
      }
      cs_buf_addc(b, '}', line);
      break;
    }
    case CS_FUNCTION:
      cs_buf_addn(b, "<function ", 10u, line);
      cs_buf_addn(b, v.as.o->as.fun.meta->name,
                  strlen(v.as.o->as.fun.meta->name), line);
      cs_buf_addc(b, '>', line);
      break;
    case CS_CLASS: /* M2-2C §2/§3: `<class Point>`, `<class ValueError>` */
      cs_buf_addn(b, "<class ", 7u, line);
      cs_buf_addn(b, v.as.o->as.cls.name, strlen(v.as.o->as.cls.name), line);
      cs_buf_addc(b, '>', line);
      break;
    case CS_EXCEPTION: { /* M2-2C §2.2: `ValueError: division by zero`;
                          * empty message renders the bare class name. */
      const char* cn = v.as.o->as.exc.cls.as.o->as.cls.name;
      const cs_value* m = &v.as.o->as.exc.msg;
      cs_buf_addn(b, cn, strlen(cn), line);
      if (m->as.s.len > 0u) {
        cs_buf_addn(b, ": ", 2u, line);
        cs_buf_addn(b, m->as.s.p, m->as.s.len, line);
      }
      break;
    }
    case CS_INSTANCE: /* M2-2C §3.3: `<Point instance>` */
      cs_buf_addn(b, "<", 1u, line);
      cs_buf_addn(b, v.as.o->as.inst.cls.as.o->as.cls.name,
                  strlen(v.as.o->as.inst.cls.as.o->as.cls.name), line);
      cs_buf_addn(b, " instance>", 10u, line);
      break;
    case CS_BOUND_METHOD: { /* M2-2C §3.3: `<method 'move' of Point>` */
      const cs_value* fn = &v.as.o->as.bnd.fn;
      const char* cn =
          v.as.o->as.bnd.inst.as.o->as.inst.cls.as.o->as.cls.name;
      cs_buf_addn(b, "<method '", 9u, line);
      cs_buf_addn(b, fn->as.o->as.fun.meta->name,
                  strlen(fn->as.o->as.fun.meta->name), line);
      cs_buf_addn(b, "' of ", 5u, line);
      cs_buf_addn(b, cn, strlen(cn), line);
      cs_buf_addc(b, '>', line);
      break;
    }
    case CS_UNSET:
      /* Unreachable in well-formed programs: reads are guarded (§4). */
      cs_runtime_error(line, "unset value in print");
      break;
    case CS_MISSING:
    default:
      cs_missing_error(line);
  }
}

/* §8 display text for str() — malloc'd, runtime-owned (leaks, like M1
 * concatenation). */
static cs_value cs_display_value(cs_value v, int line) {
  cs_buf b;
  b.p = NULL;
  b.len = 0;
  b.cap = 0;
  cs_display_buf(&b, v, line);
  cs_buf_end(&b, line);
  return cs_text_from_buf(&b, line);
}

/* Fast int64 -> decimal lives in the header (cs_itoa_ll) — the text-chain
 * builder's puti formats through it too. */

void cs_print(const cs_value* args, size_t n, int line) {
  size_t i;
  for (i = 0; i < n; i++) {
    cs_value v = args[i];
    if (i > 0) cs_out_add(" ", 1u);
    if (v.tag == CS_UNSET) {
      /* Unreachable in well-formed programs: reads are guarded (§4).
       * M1 wording preserved exactly. */
      cs_runtime_error(line, "unset value in print");
    }
    /* Scalar fast path (perf: no per-argument heap cs_buf, no per-call
     * stdio). The five common cases format exactly as cs_display_buf
     * would — cs_itoa_ll is byte-identical to snprintf("%lld"), and
     * cs_fmt_float and the literal spellings are unchanged — and append
     * to the runtime-owned stdout line buffer; only the complex types
     * (list/map/…) still build a display buffer. Byte-identical output
     * is covered by the full suite. */
    switch (v.tag) {
      case CS_INT: {
        char buf[32];
        cs_out_add(buf, (size_t)cs_itoa_ll(buf, v.as.i));
        break;
      }
      case CS_FLOAT: {
        char buf[64];
        cs_fmt_float(v.as.d, buf, sizeof(buf));
        cs_out_add(buf, strlen(buf));
        break;
      }
      case CS_BOOL:
        cs_out_add(v.as.b ? "True" : "False", v.as.b ? 4u : 5u);
        break;
      case CS_NONE:
        cs_out_add("None", 4u);
        break;
      case CS_STRING:
        cs_out_add(v.as.s.p, v.as.s.len);
        break;
      default: {
        cs_buf b;
        b.p = NULL;
        b.len = 0;
        b.cap = 0;
        cs_display_buf(&b, v, line);
        cs_out_add(b.p, b.len);
        free(b.p);
      }
    }
  }
  cs_out_add("\n", 1u);
  if (cs_out_tty) cs_out_flush();
}

/* ---- input / time / clock (post-2C basics: TODO.txt) ------------------- *
 *
 * input([prompt]) — reads one line from stdin. Returns the line WITHOUT
 * its trailing newline, or None at end of input (the builtin set has no
 * EOFError class, so EOF is a value, not a raise). A prompt argument is
 * printed first with str() semantics — no newline — and flushed, so an
 * interactive read can ask before blocking. An empty line returns "" (a
 * line read), which keeps "" distinct from None (no line at all).
 *
 * time()    — CLOCK_REALTIME seconds since the Unix epoch, as float
 *             (Python time.time() shape: wall-clock stamps).
 * clock()   — CLOCK_MONOTONIC seconds from an arbitrary origin, as float;
 *             never goes backwards — differences are elapsed time, the
 *             right basis for loop timeouts. */
cs_value cs_builtin_input(cs_value prompt, int has_prompt, int line) {
  cs_buf b;
  int c = EOF;
  if (has_prompt) {
    cs_value p = cs_display_value(prompt, line);
    cs_out_add(p.as.s.p, p.as.s.len);
  }
  /* Drain everything pending (prompt and any earlier prints) before
   * blocking on stdin — the SPEC §5.11.5 "flushed" prompt guarantee,
   * now over the runtime-owned buffer instead of stdio's. */
  cs_out_flush();
  b.p = NULL;
  b.len = 0;
  b.cap = 0;
  for (;;) {
    c = fgetc(stdin);
    if (c == EOF || c == '\n') break;
    cs_buf_addc(&b, (char)c, line);
  }
  if (c == EOF && b.len == 0u) {
    free(b.p);
    return cs_none(); /* end of input — no line, no chars */
  }
  cs_buf_end(&b, line);
  return cs_text_from_buf(&b, line);
}

double cs_builtin_time(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

double cs_builtin_clock(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ======================================================================= *
 * M2 Stage 2A additions (docs/contracts/m2a-semantics.md §3-§6, §9, §10).
 * M1 behavior and wording above are unchanged.
 * ======================================================================= */

/* ---- range values (M2-2A §6) ----------------------------------------- */

cs_value cs_range3(cs_value start, cs_value stop, cs_value step, int line) {
  cs_value r = {CS_RANGE, {0}};
  /* All three arguments must be int; bool is NOT int (M1 §4). The FIRST
   * offending argument, left to right, is reported (§9). Arity is checked
   * at compile time (§8), so all three values always arrive here. */
  if (start.tag != CS_INT)
    cs_runtime_error_as("TypeError", line, "range() expects int arguments, got %s",
                     cs_type_name(start));
  if (stop.tag != CS_INT)
    cs_runtime_error_as("TypeError", line, "range() expects int arguments, got %s",
                     cs_type_name(stop));
  if (step.tag != CS_INT)
    cs_runtime_error_as("TypeError", line, "range() expects int arguments, got %s",
                     cs_type_name(step));
  if (step.as.i == 0) cs_runtime_error_as("ValueError", line, "range() arg 3 must not be zero");
  r.as.r.start = start.as.i;
  r.as.r.stop = stop.as.i;
  r.as.r.step = step.as.i;
  return r;
}

/* ---- truthiness (M2-2A §4) ------------------------------------------- */
/* cs_truthy is static inline at the end of cstar_rt.h (hot path: every
 * loop condition and boolean operator in generated code). The four
 * collection tags land here because struct cs_obj is private to this
 * translation unit. */
bool cs_truthy_obj(cs_value v, int line) {
  switch (v.tag) {
    case CS_LIST:
    case CS_TUPLE:
      return v.as.o->as.seq.len != 0; /* M2-2B §9 */
    case CS_FVEC:
    case CS_IVEC:
      return v.as.o->as.vec.len != 0;
    case CS_SET:
    case CS_MAP:
      return v.as.o->as.table.live != 0;
    default:
      /* Only list/tuple/set/map/fvec/ivec can reach this (the header
       * switch filters every other tag, including unset, before
       * delegating). */
      cs_runtime_error(line, "internal error: unset value in cs_truthy");
  }
}

/* ---- exponent `**` (M2-2A §3.1) --------------------------------------- */

cs_value cs_pow(cs_value a, cs_value b, int line) {
  /* Operand types are validated before the exponent sign is examined
   * (M1 §8 ordering discipline). */
  cs_no_missing2(a, b, line); /* M2-2B §13 */
  if (!cs_is_numeric(a) || !cs_is_numeric(b)) cs_bad_binary(line, "**", a, b);
  if (a.tag == CS_FLOAT || b.tag == CS_FLOAT)
    return cs_float(pow(cs_as_double(a), cs_as_double(b)));
  if (b.as.i < 0) cs_runtime_error_as("ValueError", line, "negative exponent");
  {
    /* Modular exponentiation in uint64_t: the result is congruent mod
     * 2^64, i.e. the wrapping two's-complement int64 (M1 §4). b >= 0 here,
     * so the conversion to uint64 is exact. */
    uint64_t base = (uint64_t)a.as.i;
    uint64_t acc = 1u;
    uint64_t e = (uint64_t)b.as.i;
    while (e != 0u) {
      if ((e & 1u) != 0u) acc *= base;
      base *= base;
      e >>= 1;
    }
    return cs_int((int64_t)acc);
  }
}

/* ---- bitwise operators (M2-2A §3.2) ----------------------------------- */

/* ints only — no bool→int promotion (M1 §4). Operand types are checked
 * before any shift-count range examination. */
static void cs_require_ints(cs_value a, cs_value b, const char* op, int line) {
  cs_no_missing2(a, b, line); /* M2-2B §13 */
  if (a.tag != CS_INT || b.tag != CS_INT) cs_bad_binary(line, op, a, b);
}

cs_value cs_band(cs_value a, cs_value b, int line) {
  cs_require_ints(a, b, "&", line);
  return cs_int((int64_t)((uint64_t)a.as.i & (uint64_t)b.as.i));
}

cs_value cs_bor(cs_value a, cs_value b, int line) {
  cs_require_ints(a, b, "|", line);
  return cs_int((int64_t)((uint64_t)a.as.i | (uint64_t)b.as.i));
}

cs_value cs_bxor(cs_value a, cs_value b, int line) {
  cs_require_ints(a, b, "^", line);
  return cs_int((int64_t)((uint64_t)a.as.i ^ (uint64_t)b.as.i));
}

cs_value cs_shl(cs_value a, cs_value b, int line) {
  cs_require_ints(a, b, "<<", line);
  if (b.as.i < 0 || b.as.i > 63)
    cs_runtime_error_as("ValueError", line, "shift count must be in range 0..63");
  return cs_int((int64_t)((uint64_t)a.as.i << (unsigned)b.as.i));
}

cs_value cs_shr(cs_value a, cs_value b, int line) {
  cs_require_ints(a, b, ">>", line);
  if (b.as.i < 0 || b.as.i > 63)
    cs_runtime_error_as("ValueError", line, "shift count must be in range 0..63");
  /* Arithmetic (sign-propagating) shift without relying on the
   * implementation-defined signed >>: for negative a use ~((~a) >> b),
   * which is exact two's-complement arithmetic shift. */
  if (a.as.i >= 0)
    return cs_int((int64_t)((uint64_t)a.as.i >> (unsigned)b.as.i));
  return cs_int((int64_t)~((~(uint64_t)a.as.i) >> (unsigned)b.as.i));
}

cs_value cs_bnot(cs_value a, int line) {
  cs_no_missing(a, line); /* M2-2B §13 */
  if (a.tag != CS_INT)
    cs_runtime_error_as("TypeError", line, "unsupported operand type for unary ~: %s",
                     cs_type_name(a));
  /* ~a == -a-1, computed as a uint64 complement (defined for INT64_MIN). */
  return cs_int((int64_t)(~(uint64_t)a.as.i));
}

/* ---- membership `->` (M2-2A §3.4) ------------------------------------- */

/* Byte substring containment (memmem-style, no POSIX dependency): interior
 * NULs are ordinary bytes; an empty needle is always contained.
 * memchr-skips to the needle's first byte — the exact candidate set the
 * old byte-by-byte scan walked over (it accepted precisely the positions
 * where hay[i] == needle[0] and memcmp then agreed), so the predicate is
 * unchanged while the worst case drops from O(len*qlen) byte+memcmp
 * attempts to one memchr pass plus the candidates. Same skip
 * cs_text_find / cs_text_find_sep already use. */
static bool cs_str_contains(cs_value hay, cs_value needle) {
  const char* p = hay.as.s.p;
  size_t len = hay.as.s.len, qlen = needle.as.s.len, pos = 0, last;
  if (qlen == 0u) return true;
  if (qlen > len) return false;
  last = len - qlen;
  while (pos <= last) {
    const char* f =
        (const char*)memchr(p + pos, needle.as.s.p[0], last - pos + 1u);
    if (f == NULL) return false;
    pos = (size_t)(f - p);
    if (memcmp(p + pos, needle.as.s.p, qlen) == 0) return true;
    pos += 1u;
  }
  return false;
}

/* Exact integer membership in a range for any int64 bounds (§6): every
 * difference below is computed in uint64_t, where |item - start| (<= the
 * 2^64-1 span of int64) and |step| always fit exactly. Stop is exclusive. */
static bool cs_range_contains(cs_value rng, int64_t item) {
  int64_t start = rng.as.r.start, stop = rng.as.r.stop, step = rng.as.r.step;
  uint64_t diff, s;
  if (step > 0) {
    if (item < start || item >= stop) return false;
    diff = (uint64_t)item - (uint64_t)start; /* exact: item >= start */
    s = (uint64_t)step;
  } else {
    if (item > start || item <= stop) return false;
    diff = (uint64_t)start - (uint64_t)item; /* exact: item <= start */
    s = 0u - (uint64_t)step; /* |step| as uint64; defined for INT64_MIN */
  }
  return diff % s == 0u;
}

bool cs_contains(cs_value item, cs_value container, int line) {
  /* M2-2B §4: membership over every container type. Operand types are
   * validated for the pairing first (M1: types first). */
  cs_no_missing2(item, container, line);
  switch (container.tag) {
    case CS_RANGE:
      if (item.tag != CS_INT) cs_bad_binary(line, "->", item, container);
      return cs_range_contains(container, item.as.i);
    case CS_STRING: /* text: substring (M1 byte search, cp-transparent) */
      if (item.tag != CS_STRING) cs_bad_binary(line, "->", item, container);
      return cs_str_contains(container, item);
    case CS_BYTES: /* bytes: subbytes */
      if (item.tag != CS_BYTES) cs_bad_binary(line, "->", item, container);
      return cs_str_contains(container, item);
    case CS_LIST:
    case CS_TUPLE: { /* element `==` */
      size_t i, n = container.as.o->as.seq.len;
      for (i = 0; i < n; i++)
        if (cs_value_eq(cs_seq_at(container.as.o, i), item)) return true;
      return false;
    }
    case CS_MAP:   /* key */
    case CS_SET: { /* element */
      cs_table* t = &container.as.o->as.table;
      uint64_t h = cs_table_hash(t, item, line); /* raises unhashable */
      /* Hash-based slot-table probe with the identical match condition
       * (used && hash == h && total-eq) that cs_table_find/cs_set_add/
       * cs_map_set already use. This case used to scan the whole entry
       * array, making every `x -> s` / `k -> m` O(size): set_ops
       * measured 23.3 s for 120,000 membership tests after the two
       * drain-quadratics were fixed (docs/performance/baseline-m2b.md). */
      return cs_table_find(t, item, h) != (size_t)-1;
    }
    default:
      cs_bad_binary(line, "->", item, container);
      return false; /* not reached */
  }
}

/* ---- iteration protocol (M2-2A §5/§6) --------------------------------- */

cs_iter cs_iter_open(cs_value v, int line) {
  cs_iter it;
  it.kind = CS_ITER_DONE; /* also makes every field deterministic */
  it.cur = 0;
  it.stop = 0;
  it.step = 1;
  it.p = NULL;
  it.len = 0;
  it.pos = 0;
  it.src = cs_unset();
  it.idx = 0;
  cs_no_missing(v, line); /* M2-2B §13 */
  switch (v.tag) {
    case CS_RANGE:
      it.kind = CS_ITER_RANGE;
      it.cur = v.as.r.start;
      it.stop = v.as.r.stop;
      it.step = v.as.r.step; /* nonzero: cs_range3 rejects step == 0 */
      break;
    case CS_STRING: /* M2-2B §5: Text — 1-code-point values */
    case CS_BYTES:  /* int byte values */
      it.kind = (v.tag == CS_STRING) ? CS_ITER_STRING : CS_ITER_BYTES;
      it.p = v.as.s.p; /* no copy: lifetime tied to the source value (M1) */
      it.len = v.as.s.len;
      it.pos = 0;
      break;
    case CS_LIST:
    case CS_TUPLE:
    case CS_SET:
    case CS_MAP:
      it.kind = (v.tag == CS_LIST)  ? CS_ITER_LIST
                : (v.tag == CS_TUPLE) ? CS_ITER_TUPLE
                : (v.tag == CS_SET)   ? CS_ITER_SET
                                      : CS_ITER_MAP;
      it.src = v; /* holds a reference (no copy) */
      it.idx = 0;
      break;
    case CS_FVEC:
    case CS_IVEC:
      it.kind = (v.tag == CS_FVEC) ? CS_ITER_FVEC : CS_ITER_IVEC;
      it.src = v; /* holds a reference (no copy) */
      it.idx = 0;
      break;
    case CS_ITERATOR:
      /* SPEC §5.11.5: iterating an iterator shares its cursor — `for x ->
       * it` and next(it) advance the SAME state (Python semantics). */
      it.kind = CS_ITER_BOXED;
      it.src = v; /* holds a reference to the boxed iterator */
      break;
    case CS_FILE:
      /* SPEC §5.11.5: for line -> f reads one line per step. */
      it.kind = CS_ITER_FILE;
      it.src = v;
      break;
    default:
      cs_runtime_error_as("TypeError", line, "'%s' object is not iterable", cs_type_name(v));
  }
  return it;
}

bool cs_iter_next(cs_iter* it, cs_value* out) {
  switch (it->kind) {
    case CS_ITER_RANGE: {
      /* Empty range fails this bound check on the very first call. */
      bool more = (it->step > 0) ? (it->cur < it->stop) : (it->cur > it->stop);
      if (!more) return false;
      *out = cs_int(it->cur);
      /* Advance without signed-overflow UB: when the next index would
       * leave int64 range (§6) no further member can exist, so stop.
       * Both guard expressions are overflow-free for their step sign. */
      if (it->step > 0) {
        if (it->cur > INT64_MAX - it->step) it->kind = CS_ITER_DONE;
        else it->cur += it->step;
      } else {
        if (it->cur < INT64_MIN - it->step) it->kind = CS_ITER_DONE;
        else it->cur += it->step;
      }
      return true;
    }
    case CS_ITER_STRING: { /* M2-2B §5: whole code points, never split */
      size_t adv;
      if (it->pos >= it->len) return false;
      (void)cs_utf8_step(it->p, it->len, it->pos, &adv);
      *out = cs_text_copy(it->p + it->pos, adv, 0); /* copy: source may be
                                                     * collected (2C §1) */
      it->pos += adv;
      return true;
    }
    case CS_ITER_BYTES:
      if (it->pos >= it->len) return false; /* empty bytes: immediate */
      *out = cs_int((unsigned char)it->p[it->pos]);
      it->pos += 1;
      return true;
    case CS_ITER_LIST:
    case CS_ITER_TUPLE: {
      const struct cs_obj* o = it->src.as.o;
      if (it->idx >= o->as.seq.len) return false;
      *out = cs_seq_at(o, it->idx);
      it->idx += 1;
      return true;
    }
    case CS_ITER_FVEC: {
      const struct cs_obj* o = it->src.as.o;
      if (it->idx >= o->as.vec.len) return false;
      *out = cs_float(((const double*)o->as.vec.data)[it->idx]);
      it->idx += 1;
      return true;
    }
    case CS_ITER_IVEC: {
      const struct cs_obj* o = it->src.as.o;
      if (it->idx >= o->as.vec.len) return false;
      *out = cs_int(((const int64_t*)o->as.vec.data)[it->idx]);
      it->idx += 1;
      return true;
    }
    case CS_ITER_SET:
    case CS_ITER_MAP: { /* map yields keys (M2-2B §4) */
      const struct cs_obj* o = it->src.as.o;
      while (it->idx < o->as.table.len) {
        /* Walk the entry IN PLACE (§11): the step used to copy the whole
         * 80-byte cs_entry onto the C stack just to read `key` — three
         * cache lines of traffic per yielded element whose only effect was
         * on iteration throughput. Key, order and tombstone skipping are
         * exactly the old loop's. */
        const cs_entry* e = &o->as.table.e[it->idx];
        it->idx += 1;
        if (!e->used) continue; /* tombstone (popped key) */
        *out = e->key;
        return true;
      }
      return false;
    }
    case CS_ITER_BOXED: /* SPEC §5.11.5: advance the shared inner cursor */
      if (it->src.as.o->as.itr.it == NULL) return false;
      return cs_iter_next(it->src.as.o->as.itr.it, out);
    case CS_ITER_FILE: { /* SPEC §5.11.5: one line per step */
      cs_value lv = cs_file_readline(it->src, 0, 0, 0);
      if (lv.tag == CS_NONE) return false; /* EOF */
      *out = lv;
      return true;
    }
    case CS_ITER_DONE:
    default:
      return false;
  }
}

/* ======================================================================= *
 * M2 Stage 2B additions (docs/contracts/m2b-semantics.md §2, §4-§9, §12, §13, §15).
 * Collections, Text/Bytes, hashing, indexing, slicing, iteration,
 * membership, builtins, comma-join, destructuring helpers, function
 * values + dynamic-call binder, method dispatch. M1/2A behavior and
 * wording above are unchanged (the only pre-existing changes are the
 * approved 2B ones: CS_STRING is Text, code-point indexing/iteration).
 * ======================================================================= */

/* ---- hashing (§4) ---------------------------------------------------- */

/* FNV-1a 64-bit. */
#define CS_HASH_BASIS 1469598103934665603ULL
#define CS_HASH_PRIME 1099511628211ULL

static uint64_t cs_hash_bytes(const char* p, size_t len, uint64_t seed) {
  uint64_t h = seed;
  size_t i;
  for (i = 0; i < len; i++) {
    h ^= (unsigned char)p[i];
    h *= CS_HASH_PRIME;
  }
  return h;
}

/* Total-equality hash (§4): int 1 and float 1.0 hash identically (exact
 * mathematical equality, no epsilon); True/False are distinct from 1/0;
 * text vs bytes with equal bytes are distinct; tuples hash element-wise;
 * list/map/set/range/function are unhashable →
 * `unhashable type: '<t>'`. */
static uint64_t cs_hash_value(cs_value v, int line) {
  switch (v.tag) {
    case CS_NONE: {
      uint64_t h = CS_HASH_BASIS ^ (uint64_t)0x9e3779b97f4a7c15ULL;
      return h * CS_HASH_PRIME;
    }
    case CS_INT: {
      uint64_t h = (CS_HASH_BASIS ^ (uint64_t)3) * 1u;
      h = (h ^ (uint64_t)v.as.i) * CS_HASH_PRIME;
      return h;
    }
    case CS_BOOL: {
      uint64_t h = CS_HASH_BASIS ^ (uint64_t)5;
      h = (h ^ (v.as.b ? (uint64_t)1 : (uint64_t)0)) * CS_HASH_PRIME;
      return h;
    }
    case CS_FLOAT: {
      double d = v.as.d;
      if (isfinite(d) && d == floor(d) && d >= -9223372036854775808.0 &&
          d < 9223372036854775808.0) {
        /* Integral float: hash as the mathematically equal int64, so
         * int 1 and float 1.0 are the SAME key (§4). The path depends
         * only on the value, so equal values always hash equally. */
        uint64_t h = (CS_HASH_BASIS ^ (uint64_t)3) * 1u;
        h = (h ^ (uint64_t)(int64_t)d) * CS_HASH_PRIME;
        return h;
      }
      { /* Non-integral (or out of int64 range): == can only hold for
         * bitwise-identical doubles, so hash the bits (NaN never compares
         * equal — it can be stored but is never found again; documented). */
        uint64_t bits;
        uint64_t h;
        memcpy(&bits, &d, sizeof(bits));
        h = CS_HASH_BASIS ^ (uint64_t)7;
        return (h ^ bits) * CS_HASH_PRIME;
      }
    }
    case CS_STRING: /* text */
      return cs_hash_bytes(v.as.s.p, v.as.s.len, CS_HASH_BASIS ^ (uint64_t)11);
    case CS_BYTES:
      return cs_hash_bytes(v.as.s.p, v.as.s.len, CS_HASH_BASIS ^ (uint64_t)13);
    case CS_TUPLE: {
      uint64_t h = CS_HASH_BASIS ^ (uint64_t)17;
      size_t i;
      for (i = 0; i < v.as.o->as.seq.len; i++) {
        uint64_t e = cs_hash_value(cs_seq_at(v.as.o, i), line);
        h = (h ^ e) * CS_HASH_PRIME;
      }
      return h;
    }
    case CS_LIST:
    case CS_MAP:
    case CS_SET:
    case CS_RANGE:
    case CS_FUNCTION:
    case CS_FVEC:
    case CS_IVEC:
      cs_runtime_error_as("TypeError", line, "unhashable type: '%s'", cs_type_name(v));
      return 0; /* not reached */
    case CS_MISSING:
      cs_missing_error(line);
      return 0; /* not reached */
    default:
      cs_runtime_error_as("TypeError", line, "unhashable type: '%s'", cs_type_name(v));
      return 0; /* not reached */
  }
}

/* ---- insertion-ordered hash table (map + set) (§4) -------------------- */

/* Entries live in an append-only array (insertion order); the slot table
 * is an open-addressed (linear probing) index into it. Deleting an entry
 * leaves a tombstone in both structures so the insertion order of the
 * remaining entries is stable; tombstones are dropped whenever the slot
 * table is rebuilt. */

static cs_obj* cs_obj_new_tag(cs_tag tag, size_t tail, int line) {
  /* Registered GC block: the header sits immediately before the payload
   * (cs_gc_blk_of), so the cs_obj layout itself is unchanged. blk.h.kind
   * (== `tag`) drives member tracing and sweeping. `tail` extends the
   * payload past the struct (inline instance fields — CS_INST_INL). */
  cs_obj* o =
      (cs_obj*)cs_gc_new((unsigned char)tag, sizeof(cs_obj) + tail, line);
  o->tag = (int)tag;
  return o;
}

static cs_obj* cs_obj_new(cs_tag tag, int line) {
  return cs_obj_new_tag(tag, 0u, line);
}

/* Lists/tuples only ever touch `tag` and `as.seq`; allocating just that
 * prefix (48 B) instead of the whole cs_obj union (~104 B, sized by the map
 * table) halves their GC footprint and first-touch cost. The prefix layout
 * is bit-identical to cs_obj's, so the GC/sweep casts stay valid. */
#define CS_SEQ_PAYLOAD (offsetof(cs_obj, as.seq) + sizeof(((cs_obj*)0)->as.seq))

static cs_obj* cs_seq_new(cs_tag tag, int line) {
  cs_obj* o = (cs_obj*)cs_gc_new((unsigned char)tag, CS_SEQ_PAYLOAD, line);
  o->tag = (int)tag;
  return o;
}

static cs_value cs_obj_value(cs_obj* o, cs_tag tag) {
  cs_value r;
  r.tag = tag;
  memset(&r.as, 0, sizeof(r.as));
  r.as.o = o;
  return r;
}

/* ---- M2-2C §2: exceptions --------------------------------------------- *
 * setjmp/longjmp handler stack (§2.5), builtin class singletons (§2.2),
 * raise/construct/match validation (§2.3/§2.4), and the throw path every
 * cs_runtime_error site funnels into. The uncaught printer keeps the 2B
 * byte format: `<src>:<line>: runtime error: <msg>` + exit(1). */

static cs_value cs_exc_named(const char* name) {
  cs_value v = cs_exc_class(name);
  return (v.tag == CS_CLASS) ? v : cs_exc_cls[CS_EXC_RUNTIMEERROR];
}

static void cs_exc_init_classes(void) {
  int i;
  for (i = 0; i < CS_EXC_N; i++) {
    cs_obj* o = cs_obj_new(CS_CLASS, 0);
    o->as.cls.name = cs_exc_cls_names[i];
    o->as.cls.is_exc = true; /* §2.2: constructed via cs_exc_construct (the
                              * method table of a singleton stays empty) */
    cs_exc_cls[i] = cs_obj_value(o, CS_CLASS);
  }
}

cs_value cs_exc_class(const char* name) {
  int i;
  for (i = 0; i < CS_EXC_N; i++)
    if (strcmp(name, cs_exc_cls_names[i]) == 0) return cs_exc_cls[i];
  return cs_none(); /* only codegen-emitted known names are looked up */
}

/* An exception instance of `cls` with message `msg` raised at `line`. */
static cs_value cs_exc_make(cs_value cls, cs_value msg, int line) {
  cs_obj* o = cs_obj_new(CS_EXCEPTION, line);
  o->as.exc.cls = cls;
  o->as.exc.msg = msg;
  o->as.exc.line = line;
  return cs_obj_value(o, CS_EXCEPTION);
}

cs_value cs_exc_construct(cs_value cls, const cs_value* args, size_t argc,
                          int line) {
  cs_value msg;
  if (cls.tag != CS_CLASS) {
    fprintf(stderr, "internal error: constructing a non-class\n");
    exit(1);
  }
  if (argc > 1u)
    cs_runtime_error_as("TypeError", line,
                        "class constructor expects at most 1 argument, got %zu",
                        argc);
  if (argc == 0u || args == NULL) {
    msg = cs_str("", 0); /* static empty literal — immortal */
  } else if (args[0].tag == CS_STRING) {
    /* Copy: the argument text may be static, an interior slice pointer,
     * or a GC block the instance must own by reachability (§2.2). */
    msg = cs_text_copy(args[0].as.s.p, args[0].as.s.len, line);
  } else {
    msg = cs_display_value(args[0], line);
  }
  return cs_exc_make(cls, msg, line);
}

bool cs_exc_matches(cs_value pending, cs_value cls, int line) {
  if (cls.tag != CS_CLASS)
    cs_runtime_error_as("TypeError", line, "except expects a class, got %s",
                        cs_type_name(cls)); /* §2.4 */
  /* Exact identity: the pending exception's CLASS *is* `cls` (§2.4) —
   * compare the instance's stored class against the candidate class. */
  return pending.tag == CS_EXCEPTION &&
         pending.as.o->as.exc.cls.as.o == cls.as.o;
}

/* -- handler stack (§2.5) ------------------------------------------------ */

void cs_try_enter(cs_handler* h, int kind) {
  h->prev = cs_exc_top;
  h->frame_depth = cs_gc_nframes;
  h->prot_depth = cs_gc_nprot;
  h->call_depth = cs_ncalls;
  h->kind = kind;
  h->exc = cs_unset();
  cs_exc_top = h;
}

void cs_try_leave(cs_handler* h) {
  if (cs_exc_top != h) {
    fprintf(stderr, "internal error: handler stack mismatch\n");
    exit(1);
  }
  cs_exc_top = h->prev;
}

CS_NORETURN void cs_throw(cs_value exc) {
  cs_handler* h;
  if (exc.tag != CS_EXCEPTION) {
    fprintf(stderr, "internal error: throw of non-exception (%s)\n",
            cs_type_name(exc));
    exit(1);
  }
  if (cs_exc_top == NULL) {
    /* Uncaught: exactly the 2B cs_runtime_error output (§2.2/§2.6). */
    const cs_value* m = &exc.as.o->as.exc.msg;
    cs_out_flush(); /* pending prints keep their place before the error */
    fprintf(stderr, "%s:%d: runtime error: %.*s\n", cs_state_path,
            exc.as.o->as.exc.line, (int)m->as.s.len, m->as.s.p);
    exit(1);
  }
  h = cs_exc_top;
  cs_exc_top = h->prev;
  /* Restore every root layer the entry recorded (§2.5). */
  while (cs_gc_frames != NULL && cs_gc_nframes > h->frame_depth) {
    cs_gc_frames = cs_gc_frames->prev;
    cs_gc_nframes--;
  }
  cs_gc_nprot = h->prot_depth;
  /* §2.5: drop every bound vector pushed after the landing entry — heap
   * vectors are freed; C-stack vectors (the method-dispatch fast path) are
   * only dropped: the longjmp below discards the frame that owns them, and
   * free()ing them would corrupt the heap. */
  while (cs_ncalls > h->call_depth) {
    cs_call_rec e = cs_call_bounds[--cs_ncalls];
    if (!e.on_stack) free(e.p);
  }
  h->exc = exc;
  longjmp(h->env, 1);
}

CS_NORETURN void cs_rethrow(cs_value exc) { cs_throw(exc); }

CS_NORETURN void cs_raise(cs_value v, int line) {
  if (v.tag == CS_EXCEPTION) cs_throw(v); /* §2.3: re-raise the instance */
  if (v.tag == CS_CLASS) {
    /* §2.3: only a BARE EXCEPTION class constructs an empty-message
     * exception (§3.3: user classes are not raisable in 2C). */
    if (v.as.o->as.cls.is_exc)
      cs_throw(cs_exc_construct(v, NULL, 0, line));
  }
  cs_runtime_error_as("TypeError", line,
                      "exceptions must be class or instance, got %s",
                      cs_type_name(v)); /* §2.3 (+ §3.3: user class/instance) */
}

/* Formats the message into a GC text block and throws an instance of
 * `cls`. The reentry guard stops OOM-inside-error-reporting recursion: it
 * falls back to the original direct 2B print (no allocation). */
static CS_NORETURN void cs_throw_message(cs_value cls, int line,
                                         const char* fmt, va_list ap) {
  static int reentry = 0;
  va_list ap2;
  char* buf;
  int n;
  if (reentry || cls.tag != CS_CLASS) {
    cs_out_flush(); /* pending prints keep their place before the error */
    fprintf(stderr, "%s:%d: runtime error: ", cs_state_path, line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    exit(1);
  }
  va_copy(ap2, ap);
  n = vsnprintf(NULL, 0, fmt, ap);
  if (n < 0) n = 0;
  reentry = 1;
  buf = (char*)cs_gc_new(CS_GC_TEXT, (size_t)n + 1u, line);
  reentry = 0;
  vsnprintf(buf, (size_t)n + 1u, fmt, ap2);
  va_end(ap2);
  {
    cs_value msg;
    msg.tag = CS_STRING;
    msg.as.s.p = buf;
    msg.as.s.len = (size_t)n;
    cs_throw(cs_exc_make(cls, msg, line));
  }
}

static void cs_table_rehash(cs_table* t, size_t want, int line) {
  size_t cap = 16u;
  size_t i;
  while (cap < (want + 1u) * 2u) cap <<= 1u;
  free(t->slots);
  t->slots = (size_t*)calloc(cap, sizeof(size_t));
  if (t->slots == NULL) cs_runtime_error(line, "out of memory");
  t->slot_cap = cap;
  t->tombstones = 0;
  for (i = 0; i < t->len; i++) {
    size_t j;
    if (!t->e[i].used) continue;
    j = (size_t)t->e[i].hash & (cap - 1u);
    while (t->slots[j] != 0u) j = (j + 1u) & (cap - 1u);
    t->slots[j] = i + 2u;
  }
}

/* ---- §12: one-slot hash memo ----------------------------------------- *
 *
 * The FNV for a text/bytes key is recomputed by EVERY operation (`get`
 * then `set` of the same word hashes it twice per read-modify-write). One
 * process-wide slot remembers the LAST successful probe:
 *
 *   consult (cs_table_hash) requires ALL of:
 *     - cs_hm_t == t  — the memo was taken from THIS table object, which
 *       is live right now (we are executing an operation on it);
 *     - payload pointer + length + tag identical;
 *     - cs_hm_idx within t->len and e[idx].used;
 *     - the stored entry's payload pointer/length identical to the
 *       query's, and e->hash == memo hash.
 *
 * FRESHNESS PROOF (why the cached hash may replace a rehash):
 *   (a) cs_table_find only notes a probe where e->hash == h already held,
 *       so the memo hash is the hash the table itself accepted for its
 *       stored key;
 *   (b) entries are append-only and never re-keyed or re-used, so
 *       e[cs_hm_idx] still IS that stored key while it is `used`;
 *   (c) a live table roots every stored key (the GC marks e[i].key for
 *       map/set/class tables), so the stored key's payload cannot be
 *       freed while the table lives — hence a query payload whose
 *       POINTER equals the stored key's pointer is the SAME allocation,
 *       not a freed-and-reused one;
 *   (d) payload bytes are immutable while any value points at them (the
 *       same invariant the gen-stamped cs_cp_bind code-point memo relies
 *       on: a block is only ever parked AFTER it is unreachable — no
 *       value points at a swept block — and a popped block's bytes are
 *       only written while its new owner builds it, under a fresh
 *       allocation generation); and identical tag means identical hash
 *       seed.
 * Under (a)-(d) cs_hash_value is a pure function of (bytes, tag) and the
 * cached value IS the recomputed value; any violated precondition fails
 * one of the pointer/len/hash guards and falls back to cs_hash_value. */
static cs_table* cs_hm_t;
static const char* cs_hm_p;
static size_t cs_hm_len;
static uint64_t cs_hm_h;
static size_t cs_hm_idx;

static void cs_table_note(cs_table* t, cs_value key, uint64_t h, size_t idx) {
  if (key.tag == CS_STRING || key.tag == CS_BYTES) {
    cs_hm_t = t;
    cs_hm_p = key.as.s.p;
    cs_hm_len = key.as.s.len;
    cs_hm_h = h;
    cs_hm_idx = idx;
  }
}

/* Entry index of `key` (hash `h`), or (size_t)-1. */
static size_t cs_table_find(cs_table* t, cs_value key, uint64_t h) {
  size_t j = (size_t)h & (t->slot_cap - 1u);
  const bool both_int = (key.tag == CS_INT);
  const bool both_str = (key.tag == CS_STRING);
  for (;;) {
    size_t s = t->slots[j];
    if (s == 0u) return (size_t)-1;
    if (s >= 2u) {
      const cs_entry* e = &t->e[s - 2u];
      /* Two inline probes (§4): int==int and the text byte-compare are
       * copies of cs_value_eq's own arms (same-storage check, then length
       * + exact memcmp) — no call, no tag switch on the hot path. Every
       * other pairing (mixed numeric, bytes, tuples, ...) still routes
       * through cs_value_eq, so `1 == 1.0` keying, MISSING handling and
       * all error behaviour are untouched. */
      if (e->used && e->hash == h) {
        if (both_int && e->key.tag == CS_INT) {
          if (e->key.as.i == key.as.i) cs_table_note(t, key, h, s - 2u);
        return s - 2u;
        } else if (both_str && e->key.tag == CS_STRING) {
          if (e->key.as.s.p == key.as.s.p && e->key.as.s.len == key.as.s.len)
            cs_table_note(t, key, h, s - 2u);
        return s - 2u;
          if (e->key.as.s.len == key.as.s.len &&
              (key.as.s.len == 0u || memcmp(e->key.as.s.p, key.as.s.p, key.as.s.len) == 0))
            cs_table_note(t, key, h, s - 2u);
        return s - 2u;
        } else if (cs_value_eq(e->key, key)) {
          cs_table_note(t, key, h, s - 2u);
        return s - 2u;
        }
      }
    }
    j = (j + 1u) & (t->slot_cap - 1u);
  }
}

/* Hash `v` for table `t`, consulting the one-slot memo first (§12 proof
 * above); identical result and identical raise-on-unhashable behaviour —
 * only CS_STRING/CS_BYTES can be memoized and both are hashable. */
static uint64_t cs_table_hash(cs_table* t, cs_value v, int line) {
  if (v.tag == CS_STRING || v.tag == CS_BYTES) {
    if (cs_hm_t == t && cs_hm_p == v.as.s.p && cs_hm_len == v.as.s.len &&
        cs_hm_idx < t->len) {
      const cs_entry* e = &t->e[cs_hm_idx];
      if (e->used && e->hash == cs_hm_h && e->key.tag == v.tag &&
          e->key.as.s.p == v.as.s.p && e->key.as.s.len == v.as.s.len)
        return cs_hm_h;
    }
  }
  return cs_hash_value(v, line);
}

/* Appends a new entry for `key` and returns its index. The caller must
 * have checked absence with cs_table_find. */
static size_t cs_table_insert(cs_table* t, cs_value key, cs_value val,
                              uint64_t h, int line) {
  if (t->len + t->tombstones + 1u > t->slot_cap * 3u / 4u)
    cs_table_rehash(t, t->len + 1u, line);
  if (t->len == t->cap) {
    size_t cap = (t->cap != 0u) ? t->cap * 2u : 8u;
    t->e = (cs_entry*)realloc(t->e, cap * sizeof(cs_entry));
    if (t->e == NULL) cs_runtime_error(line, "out of memory");
    if (t->has_vals) { /* values ride alongside the entries (same index) */
      t->vals = (cs_value*)realloc(t->vals, cap * sizeof(cs_value));
      if (t->vals == NULL) cs_runtime_error(line, "out of memory");
    }
    t->cap = cap;
  }
  {
    size_t j = (size_t)h & (t->slot_cap - 1u);
    for (;;) {
      size_t s = t->slots[j];
      if (s == 0u || s == 1u) {
        if (s == 1u) t->tombstones -= 1u;
        t->slots[j] = t->len + 2u;
        t->e[t->len].key = key;
        if (t->has_vals) t->vals[t->len] = val;
        t->e[t->len].hash = h;
        t->e[t->len].used = true;
        t->len += 1u;
        t->live += 1u;
        if (t->live == 1u) t->first = t->len - 1u; /* was empty: this entry
                                                    * is the first */
        return t->len - 1u;
      }
      j = (j + 1u) & (t->slot_cap - 1u);
    }
  }
}

/* Clears one slot of the slot table and re-homes the rest of its probe
 * cluster (standard linear-probing deletion without shifting entries, so
 * the insertion order of the entry array is untouched). */
/* Tombstones one slot (keeps the insertion-order array stable).
 *
 * No forward re-home walk is needed: cs_table_find skips tombstones
 * (slots == 1) and slots whose entry is dead (used == false), and
 * cs_table_insert probes from the key's home slot reusing only
 * tombstones — a deleted slot therefore never breaks a probe chain, and
 * no empty slot can appear mid-chain (the cs_table_insert rehash guard
 * keeps >= 25% of slots empty, so find always terminates). The previous
 * implementation re-homed every present entry up to the next empty slot;
 * on a densely packed table that walk spans the whole live run, making a
 * front-draining pop() loop quadratic — set_ops measured 46.7 s (with a
 * second, identical Σk scan in cs_table_first) then 26.6 s after the
 * first fix, all of it this walk; docs/performance/baseline-m2b.md. */
static void cs_table_clear_slot(cs_table* t, size_t slot_j) {
  t->slots[slot_j] = 1u; /* deleted */
  t->tombstones += 1u;
}

/* Tombstones the entry (keeps the insertion-order array stable) and
 * removes it from the slot table. */
static void cs_table_delete(cs_table* t, size_t ei) {
  size_t j = (size_t)t->e[ei].hash & (t->slot_cap - 1u);
  t->e[ei].used = false;
  t->live -= 1u;
  if (ei == t->first) {
    /* The cursor must advance past the deleted head. Entries are
     * append-only (indices never move, tombstones never reuse a slot in
     * `e`), so every index this scan passes stays dead forever — total
     * scan steps over the table's life are O(len): amortized O(1). */
    size_t k = ei + 1u;
    while (k < t->len && !t->e[k].used) k++;
    t->first = k; /* == len when drained; gated by live == 0 */
  }
  for (;;) {
    size_t s = t->slots[j];
    if (s == ei + 2u) {
      cs_table_clear_slot(t, j);
      return;
    }
    if (s == 0u) return; /* not found in slots: internal inconsistency */
    j = (j + 1u) & (t->slot_cap - 1u);
  }
}

/* First live entry index in insertion order, or (size_t)-1. O(1): `first`
 * is a maintained cursor (set by insert when the table was empty, advanced
 * forward on head deletion). Previously this scanned from index 0 on every
 * call, making front-draining pop() loops quadratic — measured: set_ops
 * 46.7 s for 120,000 pops (docs/performance/baseline-m2b.md). */
static size_t cs_table_first(const cs_table* t) {
  if (t->live == 0u) return (size_t)-1;
  return t->first;
}

/* ---- collection constructors (§2/§4) --------------------------------- */

cs_value cs_list_new(void) {
  return cs_obj_value(cs_seq_new(CS_LIST, 0), CS_LIST);
}

static void cs_list_push(cs_value listv, cs_value v, int line) {
  struct cs_obj* o = listv.as.o;
  size_t newcap = (o->as.seq.len == o->as.seq.cap)
                      ? (o->as.seq.cap != 0u ? o->as.seq.cap * 2u : 4u)
                      : o->as.seq.cap;
  /* Any append to a shared buffer must detach first: after a pop the new
   * slot can lie inside a live view's range, so even a spare-capacity
   * write cannot touch shared storage. */
  if (o->as.seq.buf != NULL && (o->as.seq.buf->refs != 1u ||
                                o->as.seq.stride != 0 ||
                                o->as.seq.items != o->as.seq.buf->data ||
                                o->as.seq.cap != o->as.seq.buf->cap))
    cs_seq_detach(o, newcap);
  if (o->as.seq.len == o->as.seq.cap) {
    if (o->as.seq.buf != NULL) { /* sole owner: in-place grow */
      cs_seqbuf* b = o->as.seq.buf;
      cs_value* p = (cs_value*)realloc(b->data, newcap * sizeof(cs_value));
      if (p == NULL) cs_runtime_error(line, "out of memory");
      b->data = p;
      b->cap = newcap;
      o->as.seq.items = p;
    } else {
      cs_value* p =
          (cs_value*)realloc(o->as.seq.items, newcap * sizeof(cs_value));
      if (p == NULL) cs_runtime_error(line, "out of memory");
      o->as.seq.items = p;
    }
    o->as.seq.cap = newcap;
  }
  o->as.seq.items[o->as.seq.len] = v;
  o->as.seq.len += 1u;
}

cs_value cs_list_of(const cs_value* items, size_t n) {
  /* Exact-size one-shot build: literal lists (and the list-returning
   * builtins) know their length up front — one allocation instead of the
   * append path's log2(n) growth reallocations. */
  cs_value v = cs_list_new();
  if (n > 0u) {
    v.as.o->as.seq.items = (cs_value*)malloc(n * sizeof(cs_value));
    if (v.as.o->as.seq.items == NULL) cs_runtime_error(0, "out of memory");
    memcpy(v.as.o->as.seq.items, items, n * sizeof(cs_value));
    v.as.o->as.seq.len = n;
    v.as.o->as.seq.cap = n;
  }
  return v;
}

cs_value cs_tuple_of(const cs_value* items, size_t n) {
  cs_obj* o = cs_seq_new(CS_TUPLE, 0);
  if (n > 0u) {
    o->as.seq.items = (cs_value*)malloc(n * sizeof(cs_value));
    if (o->as.seq.items == NULL) cs_runtime_error(0, "out of memory");
    memcpy(o->as.seq.items, items, n * sizeof(cs_value));
  }
  o->as.seq.len = n;
  o->as.seq.cap = n;
  return cs_obj_value(o, CS_TUPLE);
}

/* ---- fvec/ivec: first-class SIMD vector types ----------------------------
 * A vector is a heap object holding n contiguous elements in a 64-byte-
 * aligned zeroed buffer (the allocation rounds up to whole vector lanes).
 * Elementwise operators process CS_VW elements per iteration with explicit
 * vector-extension lanes — GUARANTEED SIMD, not auto-vectorizer hope —
 * plus a scalar tail. Per-element semantics are exactly the scalar
 * operators' (two's-complement wrap for int64, the same division/modulo
 * edge cases and error classes at the same source line). The length of a
 * two-vector op must agree ('fvec length mismatch' / 'ivec length
 * mismatch'); a numeric scalar broadcasts. fvec mixes with any numeric
 * (promoted to float64); ivec mixes only with int/bool. */

static cs_obj* cs_vec_alloc(int is_fvec, size_t n, int line) {
  cs_obj* o = cs_obj_new(is_fvec ? CS_FVEC : CS_IVEC, line);
  size_t cap = ((n + 7u) / 8u) * 8u; /* whole 64-byte lanes */
  void* d = NULL;
  if (cap != 0u) {
    size_t sz = cap * 8u;
    d = cs_vec_buf_get(sz); /* pooled: stale bytes, safe — every producer
                             * fills [0, n) and nothing reads past len */
    if (d == NULL) {
      d = aligned_alloc(64u, sz);
      if (d == NULL) cs_runtime_error(line, "out of memory");
    }
    cs_gc_bytes += sz; /* the lane buffer IS GC pressure: the ~250 B
                        * cs_obj header alone is invisible, so a
                        * 64 MB-buffer arithmetic loop never crossed the
                        * 1 MB collect floor, garbage vectors were never
                        * swept, and every statement leaked + re-faulted a
                        * fresh buffer (a full page-zero pass per rep). */
  }
  o->as.vec.len = n;
  o->as.vec.data = d;
  return o;
}

cs_obj* cs_fvec_new(size_t n, int line) { return cs_vec_alloc(1, n, line); }
cs_obj* cs_ivec_new(size_t n, int line) { return cs_vec_alloc(0, n, line); }
size_t cs_vec_len_of(cs_value v) { return v.as.o->as.vec.len; }
double* cs_fvec_data_of(cs_value v) { return (double*)v.as.o->as.vec.data; }
int64_t* cs_ivec_data_of(cs_value v) { return (int64_t*)v.as.o->as.vec.data; }
void cs_fvec_expect_len(cs_value other, size_t n, int line) {
  if (other.as.o->as.vec.len != n)
    cs_runtime_error_as("ValueError", line, "fvec length mismatch");
}
void cs_ivec_expect_len(cs_value other, size_t n, int line) {
  if (other.as.o->as.vec.len != n)
    cs_runtime_error_as("ValueError", line, "ivec length mismatch");
}

static const char* cs_vec_op_str(char op) {
  switch (op) {
    case '+': return "+";
    case '-': return "-";
    case '*': return "*";
    case '/': return "/";
    default: return "%";
  }
}

/* fvec elementwise kernel: explicit lanes, scalar tail, `restrict` fresh
 * result (no aliasing with any operand — the result is a new object).
 * bd == NULL selects scalar mode: the right side is the broadcast s. */
static void cs_fv_kernel(double* restrict rd, const double* restrict ad,
                         const double* restrict bd, double s, size_t n,
                         char op) {
  size_t i = 0;
  int sc = (bd == NULL);
  for (; i + CS_VW <= n; i += CS_VW) {
    cs_vd va, vo;
    memcpy(&va, ad + i, sizeof va);
    if (sc) {
      switch (op) {
        case '+': vo = va + s; break; /* scalar op broadcasts */
        case '-': vo = va - s; break;
        case '*': vo = va * s; break;
        default:  vo = va / s; break;
      }
    } else {
      cs_vd vb;
      memcpy(&vb, bd + i, sizeof vb);
      switch (op) {
        case '+': vo = va + vb; break;
        case '-': vo = va - vb; break;
        case '*': vo = va * vb; break;
        default:  vo = va / vb; break;
      }
    }
    memcpy(rd + i, &vo, sizeof vo);
  }
  for (; i < n; i++) {
    double y = sc ? s : bd[i];
    switch (op) {
      case '+': rd[i] = ad[i] + y; break;
      case '-': rd[i] = ad[i] - y; break;
      case '*': rd[i] = ad[i] * y; break;
      default:  rd[i] = ad[i] / y; break;
    }
  }
}

/* scalar-LEFT kernel (s <op> vec). */
static void cs_fv_kernel_sl(double* restrict rd, double s,
                            const double* restrict bd, size_t n, char op) {
  size_t i = 0;
  for (; i + CS_VW <= n; i += CS_VW) {
    cs_vd vb, vo;
    memcpy(&vb, bd + i, sizeof vb);
    switch (op) {
      case '-': vo = s - vb; break;
      case '/': vo = s / vb; break;
      case '+': vo = s + vb; break;
      default:  vo = s * vb; break;
    }
    memcpy(rd + i, &vo, sizeof vo);
  }
  for (; i < n; i++) {
    switch (op) {
      case '-': rd[i] = s - bd[i]; break;
      case '/': rd[i] = s / bd[i]; break;
      case '+': rd[i] = s + bd[i]; break;
      default:  rd[i] = s * bd[i]; break;
    }
  }
}

/* ivec elementwise kernel. +,-,* are lane-wide (two's-complement wrap is
 * exactly cs_wrap_* semantics). / and % keep the scalar semantics element
 * by element (zero divisor and INT64_MIN op -1 raise/resolve exactly like
 * cs_div/cs_mod at the same line). */
static void cs_iv_kernel(int64_t* restrict rd, const int64_t* restrict ad,
                         const int64_t* restrict bd, const int64_t* restrict bp,
                         int64_t s, size_t n, char op, int swap, int line) {
  size_t i = 0;
  if (op == '/' || op == '%') {
    for (; i < n; i++) {
      int64_t x = swap ? s : ad[i];
      int64_t y = swap ? ad[i] : (bp != NULL ? s : bd[i]);
      if (op == '/') {
        if (y == 0) cs_runtime_error_as("ZeroDivisionError", line, "division by zero");
        if (x == INT64_MIN && y == -1) { rd[i] = INT64_MIN; continue; }
        rd[i] = x / y;
      } else {
        if (y == 0) cs_runtime_error_as("ZeroDivisionError", line, "modulo by zero");
        if (x == INT64_MIN && y == -1) { rd[i] = 0; continue; }
        rd[i] = x % y;
      }
    }
    return;
  }
  for (; i + CS_VW <= n; i += CS_VW) {
    cs_vi va, vo;
    memcpy(&va, ad + i, sizeof va);
    if (bp != NULL) {
      cs_vi vb = CS_VI_SPLAT(s); /* scalar broadcast (C has no scalar→vec
                                  * cast; the splat macro is width-safe) */
      switch (op) {
        case '+': vo = va + vb; break;
        case '-': vo = swap ? vb - va : va - vb; break;
        default:  vo = va * vb; break;
      }
    } else {
      cs_vi vb;
      memcpy(&vb, bd + i, sizeof vb);
      switch (op) {
        case '+': vo = va + vb; break;
        case '-': vo = va - vb; break; /* swap is impossible here */
        default:  vo = va * vb; break;
      }
    }
    memcpy(rd + i, &vo, sizeof vo);
  }
  for (; i < n; i++) {
    int64_t y = (bp != NULL) ? s : bd[i];
    switch (op) {
      case '+': rd[i] = (int64_t)((uint64_t)ad[i] + (uint64_t)y); break;
      case '-': rd[i] = swap ? (int64_t)((uint64_t)y - (uint64_t)ad[i])
                             : (int64_t)((uint64_t)ad[i] - (uint64_t)y);
                break;
      default:  rd[i] = (int64_t)((uint64_t)ad[i] * (uint64_t)y); break;
    }
  }
}

static cs_value cs_vec_binop(cs_value a, cs_value b, int line, char op) {
  int fv;
  int swap = 0;
  cs_value vec;
  cs_value sca;
  size_t n;
  cs_obj* r;
  sca.tag = CS_UNSET;
  /* Classify: one vector operand (its kind is the result kind), the other
   * side scalar or a same-kind vector. Validation happens before any
   * allocation. */
  if (a.tag == CS_FVEC || a.tag == CS_IVEC) {
    vec = a;
    fv = (a.tag == CS_FVEC);
    if (b.tag == CS_FVEC || b.tag == CS_IVEC) {
      if ((b.tag == CS_FVEC) != (fv != 0))
        cs_bad_binary(line, cs_vec_op_str(op), a, b);
      if (b.as.o->as.vec.len != a.as.o->as.vec.len)
        cs_runtime_error_as("ValueError", line,
                            fv ? "fvec length mismatch" : "ivec length mismatch");
    } else {
      sca = b;
    }
  } else {
    vec = b;
    fv = (b.tag == CS_FVEC);
    sca = a;
    swap = 1;
  }
  n = vec.as.o->as.vec.len;
  r = cs_vec_alloc(fv, n, line);
  if (fv) {
    const double* ad = (const double*)vec.as.o->as.vec.data;
    double* rd = (double*)r->as.vec.data;
    if (sca.tag != CS_UNSET) {
      double s;
      if (!cs_is_numeric(sca)) cs_bad_binary(line, cs_vec_op_str(op), a, b);
      s = cs_as_double(sca);
      if (op == '/' && swap) {
        /* scalar/vec: the VECTOR is the divisor — pre-scan keeps the
         * kernel trap-free. */
        size_t k;
        for (k = 0; k < n; k++)
          if (ad[k] == 0.0)
            cs_runtime_error_as("ZeroDivisionError", line, "division by zero");
      }
      if (op == '/' && !swap && s == 0.0)
        cs_runtime_error_as("ZeroDivisionError", line, "division by zero");
      if (swap) cs_fv_kernel_sl(rd, s, ad, n, op);
      else cs_fv_kernel(rd, ad, NULL, s, n, op);
    } else {
      const double* bd = (const double*)b.as.o->as.vec.data;
      if (op == '/') {
        size_t k;
        for (k = 0; k < n; k++)
          if (bd[k] == 0.0)
            cs_runtime_error_as("ZeroDivisionError", line, "division by zero");
      }
      cs_fv_kernel(rd, ad, bd, 0.0, n, op);
    }
  } else {
    const int64_t* ad = (const int64_t*)vec.as.o->as.vec.data;
    int64_t* rd = (int64_t*)r->as.vec.data;
    if (sca.tag != CS_UNSET) {
      int64_t s;
      if (sca.tag != CS_INT && sca.tag != CS_BOOL)
        cs_bad_binary(line, cs_vec_op_str(op), a, b);
      s = sca.tag == CS_BOOL ? (sca.as.b ? 1 : 0) : sca.as.i;
      cs_iv_kernel(rd, ad, NULL, &s, s, n, op, swap, line);
    } else {
      const int64_t* bd = (const int64_t*)b.as.o->as.vec.data;
      cs_iv_kernel(rd, ad, bd, NULL, 0, n, op, 0, line);
    }
  }
  return fv ? cs_fvec_val(r) : cs_ivec_val(r);
}

/* ---- vector constructors (builtins) ------------------------------------ */

/* fvec(items): from list/tuple/range/fvec/ivec — numeric elements (int/
 * bool promote to float64); anything else raises TypeError. ivec(items):
 * from list/tuple/range/ivec of ints (int-valued floats are rejected —
 * the vector type is exact). */
cs_value cs_builtin_fvec(cs_value items, int line) {
  cs_iter it;
  cs_value item;
  cs_obj* r;
  size_t i = 0, n;
  cs_no_missing(items, line);
  /* Count first (no size hints on ranges beyond stop-start). */
  it = cs_iter_open(items, line); /* non-iterable → TypeError */
  n = 0;
  while (cs_iter_next(&it, &item)) n++;
  r = cs_vec_alloc(1, n, line);
  {
    double* d = (double*)r->as.vec.data;
    it = cs_iter_open(items, line);
    while (cs_iter_next(&it, &item)) {
      if (item.tag == CS_INT || item.tag == CS_FLOAT || item.tag == CS_BOOL)
        d[i++] = item.tag == CS_FLOAT   ? item.as.d
                 : item.tag == CS_BOOL  ? (item.as.b ? 1.0 : 0.0)
                                        : (double)item.as.i;
      else
        cs_runtime_error_as("TypeError", line,
                            "fvec() elements must be numbers, got %s",
                            cs_type_name(item));
    }
  }
  return cs_fvec_val(r);
}

cs_value cs_builtin_ivec(cs_value items, int line) {
  cs_iter it;
  cs_value item;
  cs_obj* r;
  size_t i = 0, n;
  cs_no_missing(items, line);
  it = cs_iter_open(items, line);
  n = 0;
  while (cs_iter_next(&it, &item)) n++;
  r = cs_vec_alloc(0, n, line);
  {
    int64_t* d = (int64_t*)r->as.vec.data;
    it = cs_iter_open(items, line);
    while (cs_iter_next(&it, &item)) {
      if (item.tag == CS_INT || item.tag == CS_BOOL)
        d[i++] = item.tag == CS_BOOL ? (item.as.b ? 1 : 0) : item.as.i;
      else
        cs_runtime_error_as("TypeError", line,
                            "ivec() elements must be ints, got %s",
                            cs_type_name(item));
    }
  }
  return cs_ivec_val(r);
}

/* fvec_fill(n, v) / ivec_fill(n, v): n copies of the scalar v. */
cs_value cs_builtin_fvec_fill(cs_value n_, cs_value v, int line) {
  cs_obj* r;
  size_t i, n;
  cs_no_missing2(n_, v, line);
  if (n_.tag != CS_INT || n_.as.i < 0)
    cs_runtime_error_as("ValueError", line, "fvec_fill: count must be a non-negative int");
  if (!cs_is_numeric(v))
    cs_runtime_error_as("TypeError", line,
                        "fvec_fill() element must be numeric, got %s",
                        cs_type_name(v));
  n = (size_t)n_.as.i;
  r = cs_vec_alloc(1, n, line);
  {
    double* d = (double*)r->as.vec.data;
    double s = cs_as_double(v);
    for (i = 0; i < n; i++) d[i] = s;
  }
  return cs_fvec_val(r);
}

cs_value cs_builtin_ivec_fill(cs_value n_, cs_value v, int line) {
  cs_obj* r;
  size_t i, n;
  cs_no_missing2(n_, v, line);
  if (n_.tag != CS_INT || n_.as.i < 0)
    cs_runtime_error_as("ValueError", line, "ivec_fill: count must be a non-negative int");
  if (v.tag != CS_INT && v.tag != CS_BOOL)
    cs_runtime_error_as("TypeError", line,
                        "ivec_fill() element must be an int, got %s",
                        cs_type_name(v));
  n = (size_t)n_.as.i;
  r = cs_vec_alloc(0, n, line);
  {
    int64_t* d = (int64_t*)r->as.vec.data;
    int64_t s = v.tag == CS_BOOL ? (v.as.b ? 1 : 0) : v.as.i;
    for (i = 0; i < n; i++) d[i] = s;
  }
  return cs_ivec_val(r);
}

/* dot(a, b): the inner product. fvec: lane-wide multiply-accumulate (the
 * `acc += a[i] * b[i]` expression contracts to FMA under the CLI's
 * default -ffp-contract=on — documented fvec semantics), then a fixed-
 * order horizontal sum. ivec: wrapping int64 sum (addition is
 * associative modulo 2^64, so the lane order is unobservable). Length
 * mismatch raises ValueError. */
cs_value cs_builtin_dot(cs_value a, cs_value b, int line) {
  size_t i, n;
  cs_no_missing2(a, b, line);
  if ((a.tag != CS_FVEC && a.tag != CS_IVEC) ||
      (b.tag != CS_FVEC && b.tag != CS_IVEC) || a.tag != b.tag)
    cs_runtime_error_as("TypeError", line,
                        "dot() expects two fvecs or two ivec, got %s and %s",
                        cs_type_name(a), cs_type_name(b));
  n = a.as.o->as.vec.len;
  if (b.as.o->as.vec.len != n)
    cs_runtime_error_as("ValueError", line,
                        a.tag == CS_FVEC ? "fvec length mismatch"
                                         : "ivec length mismatch");
  if (a.tag == CS_FVEC) {
    const double* restrict ad = (const double*)a.as.o->as.vec.data;
    const double* restrict bd = (const double*)b.as.o->as.vec.data;
    cs_vd acc = {0};
    double s = 0.0;
    size_t j = 0;
    for (; j + CS_VW <= n; j += CS_VW) {
      cs_vd va, vb;
      memcpy(&va, ad + j, sizeof va);
      memcpy(&vb, bd + j, sizeof vb);
      acc += va * vb;
    }
    for (i = 0; i < CS_VW; i++) s += acc[i];
    for (; j < n; j++) s += ad[j] * bd[j];
    return cs_float(s);
  } else {
    const int64_t* restrict ad = (const int64_t*)a.as.o->as.vec.data;
    const int64_t* restrict bd = (const int64_t*)b.as.o->as.vec.data;
    uint64_t acc = 0;
    size_t j = 0;
    for (; j + CS_VW <= n; j += CS_VW) {
      cs_vi va, vb;
      memcpy(&va, ad + j, sizeof va);
      memcpy(&vb, bd + j, sizeof vb);
      cs_vi vo = va * vb;
      for (i = 0; i < CS_VW; i++) acc += (uint64_t)vo[i];
    }
    for (; j < n; j++) acc += (uint64_t)((int64_t)((uint64_t)ad[j] * (uint64_t)bd[j]));
    return cs_int((int64_t)acc);
  }
}

cs_value cs_map_new(void) {
  cs_obj* o = cs_obj_new(CS_MAP, 0);
  o->as.table.has_vals = true; /* maps carry values (sets do not) */
  cs_table_rehash(&o->as.table, 0, 0);
  return cs_obj_value(o, CS_MAP);
}

cs_value cs_set_new(void) {
  cs_obj* o = cs_obj_new(CS_SET, 0);
  cs_table_rehash(&o->as.table, 0, 0);
  return cs_obj_value(o, CS_SET);
}

static void cs_map_set(cs_value mapv, cs_value key, cs_value val, int line) {
  cs_table* t = &mapv.as.o->as.table;
  uint64_t h = cs_table_hash(t, key, line); /* raises unhashable (§4) */
  size_t i = cs_table_find(t, key, h);
  if (i != (size_t)-1) {
    /* §4: same key (1 vs 1.0) — the first insertion's key form and
     * position are kept; the value is replaced (Python-style). */
    t->vals[i] = val;
    return;
  }
  (void)cs_table_insert(t, key, val, h, line);
}

static bool cs_set_add(cs_value setv, cs_value item, int line) {
  cs_table* t = &setv.as.o->as.table;
  uint64_t h = cs_table_hash(t, item, line); /* raises unhashable (§4) */
  if (cs_table_find(t, item, h) != (size_t)-1) return false;
  (void)cs_table_insert(t, item, cs_none(), h, line);
  return true;
}

/* ---- M2-2C §3: classes, instances, bound methods, attributes, `is` ----- *
 * A class value carries its static name, is_exc (§2.2 singletons vs §3.1
 * user classes) and a method table (name → CS_FUNCTION). An instance is
 * { class, dynamic fields }; attribute reads walk fields → (exceptions)
 * the builtin .type/.message → methods, yielding a bound method (§3.2). */

/* Track a caller-owned vector on the unwind stack (§2.5): cs_throw drops
 * every entry pushed after the landing entry, freeing the HEAP ones;
 * normal returns pop. The method-dispatch fast path builds its self+args
 * vector on the C stack instead of malloc'ing it — such an entry carries
 * on_stack=true so pop/unwind never free()s it (the longjmp discards the
 * owning frame). */
static void cs_call_push(void* p, bool on_stack, int line) {
  if (cs_ncalls == cs_call_cap) {
    size_t cap = (cs_call_cap != 0) ? cs_call_cap * 2u : 64u;
    cs_call_rec* nb =
        (cs_call_rec*)realloc(cs_call_bounds, cap * sizeof(cs_call_rec));
    if (nb == NULL) cs_runtime_error(line, "out of memory");
    cs_call_bounds = nb;
    cs_call_cap = cap;
  }
  cs_call_bounds[cs_ncalls].p = p;
  cs_call_bounds[cs_ncalls].on_stack = on_stack;
  cs_ncalls++;
}

static void cs_call_push_bound(void* p, int line) { cs_call_push(p, false, line); }
static void cs_call_push_stack(void* p, int line) { cs_call_push(p, true, line); }

/* Pop the top entry, freeing it iff it is a heap vector (§2.5). */
static void cs_call_pop_free(void) {
  cs_call_rec e = cs_call_bounds[--cs_ncalls];
  if (!e.on_stack) free(e.p);
}

/* A static text value over a C literal (attribute names, "__init__" and
 * class names are string literals — never freed). */
static cs_value cs_text_lit(const char* s) {
  cs_value v;
  v.tag = CS_STRING;
  v.as.s.p = s;
  v.as.s.len = strlen(s);
  return v;
}

/* The owner name used in attribute error messages: the class name for
 * instances and exceptions, else the type name (§3.2). */
static const char* cs_attr_owner(cs_value obj) {
  if (obj.tag == CS_INSTANCE) return obj.as.o->as.inst.cls.as.o->as.cls.name;
  if (obj.tag == CS_EXCEPTION) return obj.as.o->as.exc.cls.as.o->as.cls.name;
  return cs_type_name(obj);
}

static cs_value cs_instance_new(cs_value cls, int line) {
  /* Inline fields, one block: no attrs table until a THIRD field arrives
   * (cs_setattr spills then). The zeroed payload gives n_inl = 0 and
   * CS_UNSET keys (empty slots) for the inline entries. */
  cs_obj* o = cs_obj_new_tag(CS_INSTANCE, CS_INST_INLINE * sizeof(cs_inl),
                             line);
  o->as.inst.cls = cls;
  o->as.inst.attrs.has_vals = true; /* spilled fields: name -> value (§3.2) */
  return cs_obj_value(o, CS_INSTANCE);
}

cs_value cs_class_new(const char* name, int line) {
  cs_obj* o = cs_obj_new(CS_CLASS, line);
  o->as.cls.name = name; /* static — gen.c literal (§3.1) */
  o->as.cls.is_exc = false;
  o->as.cls.methods.has_vals = true; /* name -> CS_FUNCTION (§3.1) */
  cs_table_rehash(&o->as.cls.methods, 0, line);
  return cs_obj_value(o, CS_CLASS);
}

cs_value cs_class_add_method(cs_value cls, const char* name, cs_value fn,
                             int line) {
  cs_table* t = &cls.as.o->as.cls.methods;
  cs_value key = cs_text_lit(name);
  uint64_t h = cs_hash_value(key, line);
  size_t i = cs_table_find(t, key, h);
  if (i != (size_t)-1)
    t->vals[i] = fn; /* duplicate methods are a semantic error (§3.1) */
  else
    (void)cs_table_insert(t, key, fn, h, line);
  return cls;
}

cs_value cs_getattr(cs_value obj, const char* name, int line) {
  cs_value key = cs_text_lit(name);
  uint64_t h;
  size_t i;
  cs_no_missing(obj, line);
  if (obj.tag == CS_INSTANCE) {
    cs_obj* io = obj.as.o;
    cs_table* attrs = &io->as.inst.attrs;
    cs_table* meths = &io->as.inst.cls.as.o->as.cls.methods;
    uint32_t k;
    /* Inline fields first: a linear scan over ≤ CS_INST_INLINE entries —
     * no hash, no probe (the name literals behind cs_text_lit merge to one
     * .rodata pointer per program, so cs_value_eq's storage fast path is a
     * pointer compare). */
    for (k = 0; k < io->as.inst.n_inl; k++) {
      const cs_inl* e = &CS_INST_INL(io)[k];
      if (e->key.tag != CS_UNSET && cs_value_eq(e->key, key)) return e->val;
    }
    h = cs_hash_value(key, line);
    if (attrs->slots != NULL) { /* spilled fields (3rd+ attribute) */
      i = cs_table_find(attrs, key, h);
      if (i != (size_t)-1) return attrs->vals[i];
    }
    i = cs_table_find(meths, key, h);
    if (i != (size_t)-1) { /* §3.2: methods yield a bound method */
      cs_obj* o = cs_obj_new(CS_BOUND_METHOD, line);
      o->as.bnd.inst = obj;
      o->as.bnd.fn = meths->vals[i];
      return cs_obj_value(o, CS_BOUND_METHOD);
    }
    cs_runtime_error_as("AttributeError", line,
                        "'%s' object has no attribute '%s'",
                        cs_attr_owner(obj), name);
    return cs_none(); /* not reached */
  }
  if (obj.tag == CS_EXCEPTION) {
    if (strcmp(name, "type") == 0) /* §3.2: `.type` (text) — the class name */
      return cs_text_lit(obj.as.o->as.exc.cls.as.o->as.cls.name);
    if (strcmp(name, "message") == 0) /* §3.2: `.message` (text) */
      return obj.as.o->as.exc.msg;
  }
  if (obj.tag == CS_FILE) {
    /* SPEC §5.11.5: the two observable file attributes (methods go
     * through the method-call dispatch, like every other receiver). */
    if (strcmp(name, "closed") == 0)
      return cs_bool(obj.as.o->as.file.f == NULL);
    if (strcmp(name, "name") == 0)
      return cs_str(obj.as.o->as.file.path, strlen(obj.as.o->as.file.path));
  }
  cs_runtime_error_as("AttributeError", line,
                      "'%s' object has no attribute '%s'", cs_attr_owner(obj),
                      name);
  return cs_none(); /* not reached */
}

void cs_setattr(cs_value obj, const char* name, cs_value val, int line) {
  cs_value key = cs_text_lit(name);
  cs_table* t;
  uint64_t h;
  size_t i;
  cs_no_missing(obj, line);
  if (obj.tag != CS_INSTANCE)
    cs_runtime_error_as("TypeError", line,
                        "cannot assign attribute '%s' on '%s'", name,
                        cs_attr_owner(obj));
  t = &obj.as.o->as.inst.attrs;
  {
    /* Inline fields first (see cs_getattr); the inline area is
     * append-only, so insertion order survives to vars()/dir(). */
    cs_obj* io = obj.as.o;
    uint32_t k;
    for (k = 0; k < io->as.inst.n_inl; k++) {
      cs_inl* e = &CS_INST_INL(io)[k];
      if (e->key.tag != CS_UNSET && cs_value_eq(e->key, key)) {
        e->val = val;
        return;
      }
    }
    if (io->as.inst.n_inl < CS_INST_INLINE) {
      cs_inl* e = &CS_INST_INL(io)[io->as.inst.n_inl];
      io->as.inst.n_inl += 1u;
      e->key = key;
      e->val = val;
      return;
    }
  }
  h = cs_hash_value(key, line);
  if (t->slots != NULL) {
    i = cs_table_find(t, key, h);
    if (i != (size_t)-1) {
      t->vals[i] = val;
      return;
    }
  }
  (void)cs_table_insert(t, key, val, h, line);
}

bool cs_identical(cs_value a, cs_value b, int line) {
  cs_no_missing2(a, b, line);
  if (a.tag != b.tag) return false;
  switch (a.tag) {
    case CS_STRING:
    case CS_BYTES: /* §3.4: identical iff they share storage */
      return a.as.s.p == b.as.s.p && a.as.s.len == b.as.s.len;
    case CS_LIST:
    case CS_MAP:
    case CS_SET:
    case CS_TUPLE:
    case CS_FUNCTION:
    case CS_CLASS:
    case CS_EXCEPTION:
    case CS_INSTANCE:
    case CS_BOUND_METHOD: /* same heap object */
    case CS_ITERATOR:     /* SPEC §5.11.5 */
    case CS_FILE:
      return a.as.o == b.as.o;
    default: /* int/float/bool/none/range: same tag, equal by value */
      return cs_value_eq(a, b) != 0;
  }
}

/* ---- method-dispatch fast path helpers (inline cache, see the cache
 * docs by CS_MIC_SIZE at struct cs_obj) ------------------------------- */

static size_t cs_mic_index(const cs_obj* cls, const char* name) {
  /* Byte-hash of the name (short static literals) mixed with the class
   * pointer: the SAME name at several call sites maps to one slot, so
   * sites cooperate instead of evicting each other. The guard's strcmp —
   * not this hash — proves the name; this only picks the slot. */
  uintptr_t h = ((uintptr_t)cls >> 4) * 2654435761u;
  const unsigned char* p = (const unsigned char*)name;
  while (*p != '\0') h = h * 31u + (uintptr_t)*p++;
  h ^= h >> 7;
  return (size_t)(h & (uintptr_t)(CS_MIC_SIZE - 1u));
}

/* Cache guard 3: does the instance carry its OWN field `name`? Mirrors
 * cs_getattr's instance lookup order exactly (inline area, then the
 * spilled attrs table) without allocating; false lets the methods table
 * decide. */
static bool cs_inst_field_shadowed(cs_obj* io, const char* name, int line) {
  cs_value key = cs_text_lit(name);
  cs_table* attrs = &io->as.inst.attrs;
  uint32_t k;
  for (k = 0; k < io->as.inst.n_inl; k++) {
    const cs_inl* e = &CS_INST_INL(io)[k];
    if (e->key.tag != CS_UNSET && cs_value_eq(e->key, key)) return true;
  }
  if (attrs->slots != NULL) { /* spilled fields (3rd+ attribute) */
    uint64_t h = cs_hash_value(key, line);
    if (cs_table_find(attrs, key, h) != (size_t)-1) return true;
  }
  return false;
}

/* Resolve like cs_getattr's instance branch, but hand back the METHOD
 * function value instead of a bound method (no allocation): CS_UNSET when
 * the name is an instance field or does not exist — the caller then runs
 * the exact slow path, which produces the attribute / callable errors. */
static cs_value cs_inst_method(cs_obj* io, const char* name, int line) {
  cs_value key = cs_text_lit(name);
  cs_table* meths = &io->as.inst.cls.as.o->as.cls.methods;
  cs_table* attrs = &io->as.inst.attrs;
  uint64_t h;
  size_t i;
  uint32_t k;
  for (k = 0; k < io->as.inst.n_inl; k++) {
    const cs_inl* e = &CS_INST_INL(io)[k];
    if (e->key.tag != CS_UNSET && cs_value_eq(e->key, key)) return cs_unset();
  }
  h = cs_hash_value(key, line);
  if (attrs->slots != NULL) { /* spilled fields (3rd+ attribute) */
    i = cs_table_find(attrs, key, h);
    if (i != (size_t)-1) return cs_unset();
  }
  i = cs_table_find(meths, key, h);
  if (i != (size_t)-1) return meths->vals[i];
  return cs_unset();
}

/* §3.2: `Point(1, 2)` — allocate the instance, then run
 * `__init__(self, ...)`. The __init__ resolution goes through the method
 * inline cache (same guards as cs_call_method — the method table is
 * immutable, see the cache docs), and an exact positional call with no
 * vararg/kwarg invokes the method's uniform ABI directly with a C-stack
 * self vector: no hash lookup, no per-construction malloc. Everything
 * else keeps the ordinary binder path (named arguments pass through) with
 * a heap self vector, and the instance is protected across the call. */
static cs_value cs_class_call(cs_value cls, const cs_value* posvals,
                              size_t npos, const char* const* argnames,
                              size_t nnamed, int line) {
  cs_value key = cs_text_lit("__init__");
  cs_value inst, fn;
  cs_value* arr;
  cs_obj* cbo = cls.as.o;
  size_t i;
  fn = cs_unset();
  {
    cs_mic_entry* e = &cs_mic[cs_mic_index(cbo, "__init__")];
    if (e->cls == cbo && strcmp(e->name, "__init__") == 0) {
      fn = cs_obj_value(e->fn, CS_FUNCTION); /* guard 1 + 2 satisfied */
    } else {
      uint64_t h = cs_hash_value(key, line);
      i = cs_table_find(&cbo->as.cls.methods, key, h);
      if (i != (size_t)-1) {
        fn = cbo->as.cls.methods.vals[i];
        /* Fill the slot: value/name first, class last (a collection can
         * only happen at an allocation point, but keep the entry's
         * rooted-ness monotone for the marker regardless). */
        e->name = "__init__";
        e->fn = fn.as.o;
        e->cls = cbo;
      }
    }
  }
  inst = cs_instance_new(cls, line);
  if (fn.tag != CS_FUNCTION) {
    if (npos > 0u || nnamed > 0u) /* §3.2: missing __init__ + args */
      cs_runtime_error_as("TypeError", line,
                          "class '%s' takes arguments but has no __init__",
                          cls.as.o->as.cls.name);
    return inst;
  }
  if (nnamed == 0u && !fn.as.o->as.fun.meta->has_vararg &&
      !fn.as.o->as.fun.meta->has_kwarg &&
      npos + 1u == fn.as.o->as.fun.meta->n_params) {
    /* Exact positional arity: every bound slot is filled by the prepended
     * self and the arguments, so no MISSING slot, default, duplicate or
     * required-residue check in the §13 binder could fire — build the
     * vector on the C stack and invoke the uniform ABI directly. An
     * exception inside __init__ still unwinds cleanly: cs_throw drops
     * stack entries without freeing them (the longjmp discards the frame).
     * The callee roots `bound` (and hence the instance) at its entry safe
     * point exactly as the binder path does; inst is protected across the
     * call like before. */
    const cs_funmeta* meta = fn.as.o->as.fun.meta;
    cs_value bnd[meta->n_params]; /* C11 VLA; n_params ≥ 1 (npos+1 == it) */
    cs_value r;
    bnd[0] = inst;
    for (i = 0; i < npos; i++) bnd[i + 1u] = posvals[i];
    cs_call_push_stack(bnd, line);
    cs_gc_protect(&inst, 1u);
    r = meta->fn(bnd, fn.as.o->as.fun.env);
    cs_gc_unprotect(1u);
    cs_call_pop_free();
    (void)r;
    return inst; /* __init__'s return value is discarded (§3.2) */
  }
  /* posvals carries npos + nnamed values (named i at posvals[npos+i], §13);
   * self is prepended, so the copy spans the whole vector. */
  arr = (cs_value*)malloc((npos + nnamed + 1u) * sizeof(cs_value));
  if (arr == NULL) cs_runtime_error(line, "out of memory");
  arr[0] = inst;
  for (i = 0; i < npos + nnamed; i++) arr[i + 1u] = posvals[i];
  cs_call_push_bound(arr, line);
  cs_gc_protect(&inst, 1u);
  (void)cs_call(fn, arr, npos + 1u, argnames, nnamed, line);
  cs_gc_unprotect(1u);
  cs_call_pop_free();
  return inst; /* __init__'s return value is discarded (§3.2) */
}

/* §3.2: a bound method re-dispatches through the ordinary call machinery
 * with self prepended (so named arguments/kwargs work on methods). */
static cs_value cs_bound_call(cs_value bnd, const cs_value* posvals,
                              size_t npos, const char* const* argnames,
                              size_t nnamed, int line) {
  /* posvals carries npos + nnamed values (§13); self is prepended, so the
   * copy spans the whole vector (named i lands at arr[npos+1+i] — exactly
   * where cs_call looks for posvals'[npos'+i] with npos' = npos + 1). */
  cs_value* arr = (cs_value*)malloc((npos + nnamed + 1u) * sizeof(cs_value));
  cs_value r;
  size_t i;
  if (arr == NULL) cs_runtime_error(line, "out of memory");
  arr[0] = bnd.as.o->as.bnd.inst;
  for (i = 0; i < npos + nnamed; i++) arr[i + 1u] = posvals[i];
  cs_call_push_bound(arr, line);
  r = cs_call(bnd.as.o->as.bnd.fn, arr, npos + 1u, argnames, nnamed, line);
  cs_call_pop_free();
  return r;
}

cs_value cs_map_of(const cs_value* keys, const cs_value* vals, size_t n,
                   int line) {
  cs_value v = cs_map_new();
  size_t i;
  for (i = 0; i < n; i++) cs_map_set(v, keys[i], vals[i], line);
  return v;
}

cs_value cs_set_of(const cs_value* items, size_t n, int line) {
  cs_value v = cs_set_new();
  size_t i;
  for (i = 0; i < n; i++) (void)cs_set_add(v, items[i], line);
  return v;
}

/* ---- length (§8) ------------------------------------------------------ */

size_t cs_len(cs_value v, int line) {
  cs_no_missing(v, line);
  switch (v.tag) {
    case CS_STRING: return cs_text_count(v.as.s.p, v.as.s.len);
    case CS_BYTES: return v.as.s.len;
    case CS_LIST:
    case CS_TUPLE: return v.as.o->as.seq.len;
    case CS_FVEC:
    case CS_IVEC: return v.as.o->as.vec.len;
    case CS_MAP:
    case CS_SET: return v.as.o->as.table.live;
    default:
      cs_runtime_error_as("TypeError", line, "object of type '%s' has no length()",
                       cs_type_name(v));
      return 0; /* not reached */
  }
}

cs_value cs_builtin_len(cs_value v, int line) {
  return cs_int((int64_t)cs_len(v, line));
}

/* ---- indexing (§4/§12) ----------------------------------------------- */

/* Length of an indexable sequence (list/tuple/bytes → element count,
 * text → code point count). */
static size_t cs_index_len(cs_value c) {
  switch (c.tag) {
    case CS_STRING: return cs_text_count(c.as.s.p, c.as.s.len);
    case CS_BYTES: return c.as.s.len;
    case CS_FVEC:
    case CS_IVEC: return c.as.o->as.vec.len;
    default: return c.as.o->as.seq.len;
  }
}

/* Normalizes a (possibly negative) int index for a sequence of length n;
 * reports §12 out-of-range errors. */
static size_t cs_norm_index(int64_t i, size_t n, const char* cname, int line) {
  int64_t nn = (n > (size_t)INT64_MAX) ? INT64_MAX : (int64_t)n;
  if (i < 0) i += nn;
  if (i < 0 || i >= nn) cs_runtime_error_as("IndexError", line, "%s index out of range", cname);
  return (size_t)i;
}

cs_value cs_index_get(cs_value container, cs_value idx, int line) {
  cs_no_missing2(container, idx, line);
  switch (container.tag) {
    case CS_LIST:
    case CS_TUPLE:
    case CS_STRING:
    case CS_BYTES:
    case CS_FVEC:
    case CS_IVEC: {
      const char* cname = cs_type_name(container);
      size_t i;
      if (idx.tag != CS_INT)
        cs_runtime_error_as("TypeError", line, "%s indices must be integers, not %s", cname,
                         cs_type_name(idx));
      i = cs_norm_index(idx.as.i, cs_index_len(container), cname, line);
      switch (container.tag) {
        case CS_LIST:
        case CS_TUPLE:
          return cs_seq_at(container.as.o, i);
        case CS_FVEC:
          return cs_float(((const double*)container.as.o->as.vec.data)[i]);
        case CS_IVEC:
          return cs_int(((const int64_t*)container.as.o->as.vec.data)[i]);
        case CS_BYTES:
          return cs_int((unsigned char)container.as.s.p[i]);
        default: { /* text: 1-code-point Text (§5) */
          const char* p = container.as.s.p;
          size_t len = container.as.s.len;
          size_t adv;
          size_t a = cs_text_byte(p, len, i);
          /* The old `cs_utf8_byte(i + 1)` was the same walk one step
           * further from THIS boundary — take it directly (a == len is
           * the out-of-range defensive shape: an empty slice, unchanged;
           * never dereferenced). */
          if (a < len) (void)cs_utf8_step(p, len, a, &adv);
          else adv = 0;
          return cs_text_copy(p + a, (a < len) ? adv : 0u, line);
        }
      }
    }
    case CS_MAP: {
      cs_table* t = &container.as.o->as.table;
      uint64_t h = cs_table_hash(t, idx, line); /* raises unhashable (§4) */
      size_t i = cs_table_find(t, idx, h);
      if (i == (size_t)-1) cs_runtime_error_as("KeyError", line, "map key not found");
      return t->vals[i];
    }
    default:
      cs_runtime_error_as("TypeError", line, "'%s' object is not subscriptable",
                       cs_type_name(container));
      return cs_none(); /* not reached */
  }
}

void cs_index_set(cs_value container, cs_value idx, cs_value val, int line) {
  cs_no_missing2(container, idx, line);
  cs_no_missing(val, line);
  switch (container.tag) {
    case CS_LIST: {
      size_t i;
      if (idx.tag != CS_INT)
        cs_runtime_error_as("TypeError", line, "list indices must be integers, not %s",
                         cs_type_name(idx));
      i = cs_norm_index(idx.as.i, container.as.o->as.seq.len, "list", line);
      cs_seq_ensure_owner(container.as.o, 0u);
      container.as.o->as.seq.items[i] = val; /* reference semantics §7.5 */
      return;
    }
    case CS_MAP:
      cs_map_set(container, idx, val, line);
      return;
    case CS_FVEC: {
      size_t i;
      if (idx.tag != CS_INT)
        cs_runtime_error_as("TypeError", line, "fvec indices must be integers, not %s",
                         cs_type_name(idx));
      i = cs_norm_index(idx.as.i, container.as.o->as.vec.len, "fvec", line);
      if (!cs_is_numeric(val))
        cs_runtime_error_as("TypeError", line, "cannot store %s in fvec",
                         cs_type_name(val));
      ((double*)container.as.o->as.vec.data)[i] = cs_as_double(val);
      return;
    }
    case CS_IVEC: {
      size_t i;
      if (idx.tag != CS_INT)
        cs_runtime_error_as("TypeError", line, "ivec indices must be integers, not %s",
                         cs_type_name(idx));
      i = cs_norm_index(idx.as.i, container.as.o->as.vec.len, "ivec", line);
      if (val.tag != CS_INT && val.tag != CS_BOOL)
        cs_runtime_error_as("TypeError", line, "cannot store %s in ivec",
                         cs_type_name(val));
      ((int64_t*)container.as.o->as.vec.data)[i] =
          val.tag == CS_BOOL ? (val.as.b ? 1 : 0) : val.as.i;
      return;
    }
    case CS_STRING: /* §12: immutable kinds */
    case CS_BYTES:
    case CS_TUPLE:
      cs_runtime_error_as("TypeError", line, "'%s' does not support item assignment",
                       cs_type_name(container));
      return;
    default:
      cs_runtime_error_as("TypeError", line, "'%s' object does not support item assignment",
                       cs_type_name(container));
      return;
  }
}

/* ---- slicing (§4) ----------------------------------------------------- */

/* Python slice normalization: computes the selected index sequence
 * (first, step, count) for a sequence of length n. Bounds arrive as
 * possibly-unset int values. */
static void cs_slice_norm(cs_value start, cs_value stop, cs_value step,
                          size_t n, int64_t* out_first, int64_t* out_step,
                          size_t* out_count, int line) {
  int64_t st = 1, b, e;
  int64_t nn = (n > (size_t)INT64_MAX) ? INT64_MAX : (int64_t)n;
  if (step.tag == CS_UNSET) st = 1;
  else if (step.tag == CS_INT) st = step.as.i;
  else cs_runtime_error_as("TypeError", line, "slice indices must be integers, not %s",
                        cs_type_name(step));
  if (st == 0) cs_runtime_error_as("ValueError", line, "slice step cannot be zero");
  if (start.tag == CS_UNSET) {
    b = (st > 0) ? 0 : nn - 1;
  } else {
    if (start.tag != CS_INT)
      cs_runtime_error_as("TypeError", line, "slice indices must be integers, not %s",
                       cs_type_name(start));
    b = start.as.i;
    if (b < 0) b += nn;
    if (st > 0) {
      if (b < 0) b = 0;
      if (b > nn) b = nn;
    } else {
      if (b < 0) b = -1;
      if (b >= nn) b = nn - 1;
    }
  }
  if (stop.tag == CS_UNSET) {
    e = (st > 0) ? nn : -1;
  } else {
    if (stop.tag != CS_INT)
      cs_runtime_error_as("TypeError", line, "slice indices must be integers, not %s",
                       cs_type_name(stop));
    e = stop.as.i;
    if (e < 0) e += nn;
    if (st > 0) {
      if (e < 0) e = 0;
      if (e > nn) e = nn;
    } else {
      if (e < 0) e = -1;
      if (e >= nn) e = nn - 1;
    }
  }
  /* Count = ceil((e - b) / st), computed in uint64 (overflow-free for
   * every int64 step, including INT64_MIN). */
  {
    uint64_t count = 0u;
    if (st > 0) {
      if (e > b) {
        uint64_t d = (uint64_t)e - (uint64_t)b;
        count = (d + (uint64_t)st - 1u) / (uint64_t)st;
      }
    } else {
      if (b > e) {
        uint64_t d = (uint64_t)b - (uint64_t)e;
        uint64_t mag = 0u - (uint64_t)st; /* |st|, defined for INT64_MIN */
        count = (d + mag - 1u) / mag;
      }
    }
    *out_first = b;
    *out_step = st;
    *out_count = (size_t)count;
  }
}

CS_NORETURN int64_t cs_il_oob_named(size_t count, int64_t idx, int line) {
  cs_norm_index(idx, count, "list", line);
  __builtin_unreachable();
}

CS_NORETURN int64_t cs_slice_oob_named(const char* cname, size_t count,
                                       int64_t idx, int line) {
  cs_norm_index(idx, count, cname, line);
  __builtin_unreachable();
}

cs_value cs_slice(cs_value container, cs_value start, cs_value stop,
                  cs_value step, int line) {
  int64_t first, st;
  size_t count, k;
  cs_no_missing(container, line);
  switch (container.tag) {
    case CS_STRING:
    case CS_BYTES:
    case CS_LIST:
    case CS_TUPLE:
      break;
    default:
      cs_runtime_error_as("TypeError", line, "'%s' object does not support slicing",
                       cs_type_name(container));
      return cs_none(); /* not reached */
  }
  cs_slice_norm(start, stop, step, cs_index_len(container), &first, &st,
                &count, line);
  switch (container.tag) {
    case CS_LIST:
    case CS_TUPLE: {
      /* O(1) copy-on-write view: share the source's element buffer and
       * select through (items + first*stride, stride*step). Mutations
       * detach first (cs_seq_detach), so copy semantics are unchanged.
       * count == 0 returns a fresh empty object (nothing to reference). */
      cs_tag tag = container.tag;
      if (count == 0u)
        return (tag == CS_LIST) ? cs_list_new() : cs_tuple_of(NULL, 0u);
      {
        cs_obj* src = container.as.o;
        ptrdiff_t ss = (src->as.seq.stride != 0) ? src->as.seq.stride : 1;
        ptrdiff_t ns = (count > 1u) ? ss * (ptrdiff_t)st : 1;
        cs_obj* o = cs_seq_new(tag, line);
        cs_seqbuf* b = cs_seqbuf_adopt(src);
        b->refs += 1u;
        o->as.seq.items = src->as.seq.items + (ptrdiff_t)first * ss;
        o->as.seq.len = count;
        o->as.seq.cap = count; /* views never have spare room */
        o->as.seq.buf = b;
        o->as.seq.stride = (ns == 1) ? 0 : ns;
        return cs_obj_value(o, tag);
      }
    }
    case CS_BYTES: {
      cs_value out;
      char* p = NULL;
      int64_t cur = first;
      if (count > 0u) {
        p = cs_gc_buf(CS_GC_BYTES, count, line);
        for (k = 0; k < count; k++) {
          p[k] = container.as.s.p[(size_t)cur];
          if (k + 1u < count) cur += st;
        }
      }
      out = cs_bytes_of(p, count);
      return out;
    }
    default: { /* CS_STRING: slices are code-point based and never split a
                * code point (§4/§5). */
      cs_buf b;
      int64_t cur = first;
      b.p = NULL;
      b.len = 0;
      b.cap = 0;
      for (k = 0; k < count; k++) {
        size_t a = cs_text_byte(container.as.s.p, container.as.s.len,
                                (size_t)cur);
        size_t c = cs_text_byte(container.as.s.p, container.as.s.len,
                                (size_t)cur + 1u);
        cs_buf_addn(&b, container.as.s.p + a, c - a, line);
        if (k + 1u < count) cur += st;
      }
      cs_buf_end(&b, line);
      return cs_text_from_buf(&b, line);
    }
  }
}


void cs_slice_bounds(cs_value container, cs_value start, cs_value stop,
                     cs_value step, int64_t* out_first, int64_t* out_step,
                     size_t* out_count, int line) {
  cs_no_missing(container, line);
  switch (container.tag) {
    case CS_STRING:
    case CS_BYTES:
    case CS_LIST:
    case CS_TUPLE:
      break;
    default:
      cs_runtime_error_as("TypeError", line, "'%s' object does not support slicing",
                       cs_type_name(container));
      return;
  }
  cs_slice_norm(start, stop, step, cs_index_len(container), out_first, out_step,
                out_count, line);
}

cs_value cs_slice_get_slow(cs_value container, int64_t first, int64_t step,
                           size_t count, cs_value idx, int line) {
  cs_no_missing2(container, idx, line);
  const char* cname = cs_type_name(container);
  if (idx.tag != CS_INT)
    cs_runtime_error_as("TypeError", line, "%s indices must be integers, not %s",
                     cname, cs_type_name(idx));
  size_t i = cs_norm_index(idx.as.i, count, cname, line);
  int64_t src = first + (int64_t)i * step;
  return cs_index_get(container, cs_int(src), line);
}

int64_t cs_slice_iget_slow(cs_value container, int64_t first, int64_t step,
                           size_t count, int64_t idx, int line) {
  const char* cname = cs_type_name(container);
  size_t i = cs_norm_index(idx, count, cname, line);
  int64_t src = first + (int64_t)i * step;
  return cs_il_iget(container, src, line);
}

int64_t cs_il_iget_slow(cs_value c, int64_t i, int line) {
  cs_value v = cs_index_get(c, cs_int(i), line);
  if (v.tag != CS_INT)
    cs_runtime_error_as("TypeError", line,
                     "expected an int list element, got %s", cs_type_name(v));
  return v.as.i;
}

/* ---- destructuring (§12) ---------------------------------------------- */

size_t cs_unpack_len(cs_value v, int line) {
  cs_no_missing(v, line);
  switch (v.tag) {
    case CS_LIST:
    case CS_TUPLE: return v.as.o->as.seq.len;
    case CS_STRING: return cs_text_count(v.as.s.p, v.as.s.len);
    case CS_BYTES: return v.as.s.len;
    case CS_MAP: return v.as.o->as.table.live; /* keys */
    case CS_SET: return v.as.o->as.table.live; /* elements (insertion order) */
    case CS_RANGE: {
      /* Exact member count (M2-2A §6 semantics): step>0 iterates while
       * i < stop, step<0 while i > stop. The distance never exceeds 2^64,
       * so unsigned difference arithmetic is exact for any int64 bounds. */
      int64_t st = v.as.r.start, sp = v.as.r.step;
      uint64_t udiff, ustep;
      if (sp > 0) {
        if (st >= v.as.r.stop) return 0;
        udiff = (uint64_t)v.as.r.stop - (uint64_t)st;
        ustep = (uint64_t)sp;
      } else {
        if (st <= v.as.r.stop) return 0;
        udiff = (uint64_t)st - (uint64_t)v.as.r.stop;
        /* -(int64)sp in unsigned form: correct even for INT64_MIN. */
        ustep = (uint64_t)(-(sp + 1)) + 1u;
      }
      return (size_t)(udiff / ustep + (udiff % ustep != 0u ? 1u : 0u));
    }
    default:
      cs_runtime_error_as("TypeError", line, "'%s' object is not iterable", cs_type_name(v));
      return 0; /* not reached */
  }
}

/* i'th element of a destructure source, skipping map tombstones. */
cs_value cs_unpack_get(cs_value v, size_t i, int line) {
  cs_no_missing(v, line);
  switch (v.tag) {
    case CS_LIST:
    case CS_TUPLE:
      if (i >= v.as.o->as.seq.len)
        cs_runtime_error(line, "internal error: unpack index out of range");
      return cs_seq_at(v.as.o, i);
    case CS_STRING: {
      /* Same boundary-adjacent step as the old cs_utf8_byte(i + 1). */
      size_t adv;
      size_t a = cs_text_byte(v.as.s.p, v.as.s.len, i);
      if (a < v.as.s.len) (void)cs_utf8_step(v.as.s.p, v.as.s.len, a, &adv);
      else adv = 0;
      return cs_text_copy(v.as.s.p + a, (a < v.as.s.len) ? adv : 0u, line);
    }
    case CS_BYTES:
      return cs_int((unsigned char)v.as.s.p[i]);
    case CS_MAP: {
      const cs_table* t = &v.as.o->as.table;
      size_t j, seen = 0;
      for (j = 0; j < t->len; j++) {
        if (!t->e[j].used) continue;
        if (seen == i) return t->e[j].key;
        seen += 1u;
      }
      cs_runtime_error(line, "internal error: unpack index out of range");
      return cs_none(); /* not reached */
    }
    case CS_SET: { /* i'th element in insertion order (element lives in .key) */
      const cs_table* t = &v.as.o->as.table;
      size_t j, seen = 0;
      for (j = 0; j < t->len; j++) {
        if (!t->e[j].used) continue;
        if (seen == i) return t->e[j].key;
        seen += 1u;
      }
      cs_runtime_error(line, "internal error: unpack index out of range");
      return cs_none(); /* not reached */
    }
    case CS_RANGE: {
      /* member = start + i*step; i < cs_unpack_len keeps the true value
       * inside int64, so two's-complement wrapping arithmetic is exact. */
      uint64_t off = (uint64_t)i * (uint64_t)v.as.r.step;
      return cs_int((int64_t)((uint64_t)v.as.r.start + off));
    }
    default:
      cs_runtime_error_as("TypeError", line, "'%s' object is not iterable", cs_type_name(v));
      return cs_none(); /* not reached */
  }
}

/* Elements [start, stop) as a fresh list (rest target `*name` in a pattern
 * with L leaves: the caller passes start = the rest leaf's position and
 * stop = element count minus the number of fixed leaves after it, so a
 * middle `a, *rest, b` spans exactly the middle; a trailing rest passes
 * stop = cs_unpack_len's result). stop is clamped to the element count. */
cs_value cs_unpack_rest(cs_value v, size_t start, size_t stop, int line) {
  size_t n = cs_unpack_len(v, line);
  if (stop > n) stop = n;
  cs_value out = cs_list_new();
  size_t i;
  for (i = start; i < stop; i++) cs_list_push(out, cs_unpack_get(v, i, line), line);
  return out;
}

void cs_unpack_check(size_t got, size_t expected, int line) {
  if (got < expected)
    cs_runtime_error_as("ValueError", line, "not enough values to unpack (expected %zu, got %zu)",
                     expected, got);
  if (got > expected)
    cs_runtime_error_as("ValueError", line, "too many values to unpack (expected %zu, got %zu)",
                     expected, got);
}

/* ---- comma-join (§7) --------------------------------------------------- */

cs_value cs_join(const cs_value* args, size_t n, int line) {
  cs_buf b;
  size_t i;
  b.p = NULL;
  b.len = 0;
  b.cap = 0;
  for (i = 0; i < n; i++) {
    if (i > 0u) cs_buf_addc(&b, ' ', line);
    cs_display_buf(&b, args[i], line);
  }
  cs_buf_end(&b, line);
  return cs_text_from_buf(&b, line);
}

/* ---- builtins (§8) ----------------------------------------------------- */

cs_value cs_builtin_str(cs_value v, int line) {
  cs_no_missing(v, line);
  return cs_display_value(v, line);
}

cs_value cs_builtin_bool(cs_value v, int line) {
  return cs_bool(cs_truthy(v, line));
}

cs_value cs_builtin_type(cs_value v, int line) {
  const char* s;
  cs_no_missing(v, line);
  /* M2-2C §2.2: type(e) of an exception is its class name ("ValueError");
   * §8: the builtin renders None as "none" (runtime error messages keep
   * the M1 "None" spelling — §12). */
  if (v.tag == CS_EXCEPTION) {
    const char* n = v.as.o->as.exc.cls.as.o->as.cls.name;
    return cs_str(n, strlen(n));
  }
  s = (v.tag == CS_NONE) ? "none" : cs_type_name(v);
  return cs_str(s, strlen(s));
}

/* Strict decimal int parse ([+-]?[0-9]+, no whitespace); overflow of the
 * int64 range also counts as invalid (cstar ints are int64). */
static bool cs_parse_int_text(const char* p, size_t len, int64_t* out) {
  size_t i = 0;
  bool neg = false;
  uint64_t acc = 0;
  uint64_t limit;
  if (i < len && (p[i] == '+' || p[i] == '-')) {
    neg = (p[i] == '-');
    i += 1;
  }
  if (i >= len) return false;
  limit = neg ? (uint64_t)9223372036854775808ULL : (uint64_t)9223372036854775807ULL;
  for (; i < len; i++) {
    unsigned char c = (unsigned char)p[i];
    uint64_t d;
    if (c < '0' || c > '9') return false;
    d = (uint64_t)(c - '0');
    if (acc > (limit - d) / 10u) return false; /* int64 overflow */
    acc = acc * 10u + d;
  }
  *out = neg ? (int64_t)(0u - acc) : (int64_t)acc;
  return true;
}

/* Strict strtod parse of the whole span (no surrounding whitespace);
 * "inf"/"nan" spellings are accepted, as by C strtod / Python float(). */
static bool cs_parse_float_text(const char* p, size_t len, double* out) {
  char* buf;
  char* end = NULL;
  double d;
  if (len == 0u) return false;
  if (p[len] == '\0') {
    /* Every text payload and literal is NUL-terminated at its byte
     * length (cs_gc_buf / cs_concat / CS_STR invariants — the same one
     * cs_text_cstr's fast path relies on), so strtod sees EXACTLY the
     * byte sequence the copied buffer held: same parse, same end checks,
     * zero allocations (perf: float() parse used to malloc+memcpy+free a
     * scratch buffer per call). */
    d = strtod(p, &end);
    if (end == p || *end != '\0') return false;
    *out = d;
    return true;
  }
  buf = (char*)malloc(len + 1u);
  if (buf == NULL) return false; /* caller reports invalid literal */
  memcpy(buf, p, len);
  buf[len] = '\0';
  d = strtod(buf, &end);
  if (end == buf || *end != '\0') {
    free(buf);
    return false;
  }
  free(buf);
  *out = d;
  return true;
}

/* Bound for echoing a raw source span inside a message (keeps `%.*s`
 * precision in int range and matches the pre-throw output length). */
#define CS_RAW_SPAN_MAX 1000000

cs_value cs_builtin_int(cs_value v, int line) {
  cs_no_missing(v, line);
  switch (v.tag) {
    case CS_INT: return v;
    case CS_BOOL: return cs_int(v.as.b ? 1 : 0);
    case CS_FLOAT: {
      double d = v.as.d;
      /* Truncate toward zero; non-finite or out-of-int64-range values
       * cannot convert (documented wording). */
      if (!isfinite(d) || d < -9223372036854775808.0 ||
          d >= 9223372036854775808.0)
        cs_runtime_error_as("ValueError", line, "cannot convert float to int");
      return cs_int((int64_t)d);
    }
    case CS_STRING:
    case CS_BYTES: {
      int64_t out;
      if (!cs_parse_int_text(v.as.s.p, v.as.s.len, &out))
        /* Catchable ValueError (SPEC §6.10 / §9.3): same message and same
         * uncaught bytes as the old print-and-exit path, but `try`/
         * `except`/`finally` now see it. NUL bytes cut the echo exactly as
         * the old `%.*s`-to-stderr write did. */
        cs_runtime_error_as("ValueError", line,
                            "invalid literal for int(): '%.*s'",
                            (int)((v.as.s.len > (size_t)CS_RAW_SPAN_MAX)
                                      ? (size_t)CS_RAW_SPAN_MAX
                                      : v.as.s.len),
                            v.as.s.p);
      return cs_int(out);
    }
    default:
      cs_runtime_error_as("TypeError", line, "int() expects text, bytes, int, float or bool, got %s",
                       cs_type_name(v));
      return cs_none(); /* not reached */
  }
}

cs_value cs_builtin_float(cs_value v, int line) {
  cs_no_missing(v, line);
  switch (v.tag) {
    case CS_FLOAT: return v;
    case CS_INT: return cs_float((double)v.as.i);
    case CS_BOOL: return cs_float(v.as.b ? 1.0 : 0.0);
    case CS_STRING:
    case CS_BYTES: {
      double out;
      if (!cs_parse_float_text(v.as.s.p, v.as.s.len, &out))
        /* Catchable ValueError — see cs_builtin_int's parse branch. */
        cs_runtime_error_as("ValueError", line,
                            "invalid literal for float(): '%.*s'",
                            (int)((v.as.s.len > (size_t)CS_RAW_SPAN_MAX)
                                      ? (size_t)CS_RAW_SPAN_MAX
                                      : v.as.s.len),
                            v.as.s.p);
      return cs_float(out);
    }
    default:
      /* TypeError like int()'s default branch (SPEC §6.10 "builtin
       * argument-type errors") — was cs_runtime_error → RuntimeError. */
      cs_runtime_error_as("TypeError", line,
                          "float() expects text, bytes, int, float or bool, got %s",
                          cs_type_name(v));
      return cs_none(); /* not reached */
  }
}

cs_value cs_builtin_encode(cs_value v, int line) {
  char* p;
  cs_no_missing(v, line);
  if (v.tag != CS_STRING)
    cs_runtime_error_as("TypeError", line, "encode() expects text, got %s", cs_type_name(v));
  p = NULL;
  if (v.as.s.len > 0u) {
    p = cs_gc_buf(CS_GC_BYTES, v.as.s.len, line);
    memcpy(p, v.as.s.p, v.as.s.len);
  }
  return cs_bytes_of(p, v.as.s.len);
}

cs_value cs_builtin_decode(cs_value v, int line) {
  char* p;
  cs_no_missing(v, line);
  if (v.tag != CS_BYTES)
    cs_runtime_error_as("TypeError", line, "decode() expects bytes, got %s", cs_type_name(v));
  if (!cs_utf8_valid(v.as.s.p, v.as.s.len))
    cs_runtime_error_as("ValueError", line, "invalid UTF-8 in decode()");
  p = NULL;
  if (v.as.s.len > 0u) {
    p = cs_gc_buf(CS_GC_TEXT, v.as.s.len, line);
    memcpy(p, v.as.s.p, v.as.s.len);
  }
  return cs_str(p, v.as.s.len);
}

cs_value cs_builtin_set(cs_value* args, size_t n, int line) {
  cs_value s;
  if (n > 1u)
    cs_runtime_error_as("TypeError", line, "set() takes 0 or 1 argument(s), got %zu", n);
  s = cs_set_new();
  if (n == 1u) {
    /* Any iterable; a map iterates its keys (§8). cs_iter_open raises
     * the §12 not-iterable error for non-iterables. */
    cs_iter it = cs_iter_open(args[0], line);
    cs_value e;
    while (cs_iter_next(&it, &e)) (void)cs_set_add(s, e, line);
  }
  return s;
}

/* ---- function values and the dynamic-call binder (§13) ---------------- */

cs_cell* cs_cell_alloc(void) {
  /* Registered GC block (leaf-with-value: CS_GC_CELL traces c->v). */
  cs_cell* c = (cs_cell*)cs_gc_new(CS_GC_CELL, sizeof(cs_cell), 0);
  c->v = cs_unset(); /* used-before-assignment guard still applies */
  return c;
}

/* Immortal cell for a cross-scope "pinned" binding that is also captured
 * by a closure (docs/reference/cross-scope-access.md §4): a regular cell, registered in
 * the pinned-cell table so the collector roots it at every collection.
 * Called once per pinned binding, from the generated program's entry. */
cs_cell* cs_cell_pinned(void) {
  cs_cell* c = cs_cell_alloc();
  if (cs_gc_pinned_n == cs_gc_pinned_cap) {
    size_t cap = (cs_gc_pinned_cap != 0) ? cs_gc_pinned_cap * 2u : 8u;
    cs_cell** np = (cs_cell**)realloc(cs_gc_pinned, cap * sizeof(cs_cell*));
    if (np == NULL) cs_runtime_error(0, "out of memory");
    cs_gc_pinned = np;
    cs_gc_pinned_cap = cap;
  }
  cs_gc_pinned[cs_gc_pinned_n++] = c;
  return c;
}

cs_cell** cs_env_new(size_t n) {
  cs_cell** e = (cs_cell**)calloc(n ? n : 1u, sizeof(cs_cell*));
  if (e == NULL) cs_runtime_error(0, "out of memory");
  return e; /* owned by the function value it is stored in (2C §1.2) */
}

/* Closure env vectors with at most this many cells are inlined into the
 * function object's own GC block (a tail past cs_obj, like the instance
 * inline fields): one allocation instead of obj + calloc, one fewer
 * dependent cache miss per env read, and no separate free at sweep. */
#define CS_FUN_ENV_INL 4u

/* Uniform-ABI function value with `env_n` capture slots, ready to fill
 * (cs_fun_new): the preferred closure constructor — it allocates the env
 * storage itself (inlined in the block for the common small case), so
 * generated code writes the cells straight into cs_fun_env(fn)[j] with no
 * temporary env array to allocate, copy and free. The tail (like every
 * freshly allocated payload) is zero-filled, so the slots read as NULL —
 * trace-safe even before the fills run. */
cs_value cs_fun_new(const cs_funmeta* meta, size_t env_n) {
  cs_obj* o;
  if (env_n > 0u && env_n <= CS_FUN_ENV_INL) {
    o = cs_obj_new_tag(CS_FUNCTION, env_n * sizeof(cs_cell*), 0);
    o->as.fun.env = (cs_cell**)((char*)o + sizeof(cs_obj)); /* zeroed tail */
  } else if (env_n > 0u) {
    o = cs_obj_new(CS_FUNCTION, 0);
    o->as.fun.env = (cs_cell**)calloc(env_n, sizeof(cs_cell*));
    if (o->as.fun.env == NULL) cs_runtime_error(0, "out of memory");
  } else {
    o = cs_obj_new(CS_FUNCTION, 0);
    o->as.fun.env = NULL; /* top-level, env-less function */
  }
  o->as.fun.meta = meta;
  o->as.fun.env_n = env_n;
  return cs_obj_value(o, CS_FUNCTION);
}

cs_value cs_fun_value(const cs_funmeta* meta, cs_cell** env, size_t env_n) {
  if (env != NULL && env_n > 0u && env_n <= CS_FUN_ENV_INL) {
    /* Inline the env pointer array into the block's tail and take over
     * the caller's array (cs_env_new's allocation — its ownership is the
     * function value's by the §13 contract, so freeing it here is the one
     * place it dies). The inline convention: as.fun.env points INSIDE the
     * object's own block at ((char*)o + sizeof(cs_obj)). The sweep skips
     * free() exactly for that address (an external heap array can never
     * equal the object's interior), and the GC trace needs no change —
     * it only reads through env[i], wherever the array lives. */
    cs_obj* o = cs_obj_new_tag(CS_FUNCTION, env_n * sizeof(cs_cell*), 0);
    o->as.fun.meta = meta;
    o->as.fun.env = (cs_cell**)((char*)o + sizeof(cs_obj));
    memcpy(o->as.fun.env, env, env_n * sizeof(cs_cell*));
    free(env);
    o->as.fun.env_n = env_n; /* GC trace bound */
    return cs_obj_value(o, CS_FUNCTION);
  }
  {
    cs_obj* o = cs_obj_new(CS_FUNCTION, 0);
    o->as.fun.meta = meta;
    o->as.fun.env = env;     /* NULL for top-level, env-less functions */
    o->as.fun.env_n = env_n; /* GC trace bound */
    return cs_obj_value(o, CS_FUNCTION);
  }
}

const cs_funmeta* cs_fun_meta(cs_value v) {
  if (v.tag != CS_FUNCTION) cs_runtime_error(0, "internal error: not a function value");
  return v.as.o->as.fun.meta;
}

cs_cell** cs_fun_env(cs_value v) {
  if (v.tag != CS_FUNCTION) cs_runtime_error(0, "internal error: not a function value");
  return v.as.o->as.fun.env;
}

/* The §13 binder core over an already-initialized `bound` vector (all
 * meta->n_params slots filled with CS_MISSING, plus one spare slot — the
 * historical heap vector's shape): steps 1 (positionals), 2 (named
 * arguments / kwargs map), 3 (required residue) and the uniform-ABI
 * invoke. `bound`'s storage (heap vs C stack) and unwind tracking belong
 * to the caller — this function's checks, errors and evaluation order are
 * byte-identical whichever storage the vector uses. */
static cs_value cs_call_bind_invoke(const cs_funmeta* meta, cs_value* bound,
                                    cs_cell** env, const cs_value* posvals,
                                    size_t npos, const char* const* argnames,
                                    size_t nnamed, size_t vararg_idx,
                                    size_t kwarg_idx, size_t nfixed,
                                    int line) {
  size_t i;
  bool has_vararg = meta->has_vararg;
  bool has_kwarg = meta->has_kwarg;

  /* 1. Positional arguments, left to right. */
  if (npos > nfixed && !has_vararg)
    cs_runtime_error_as("TypeError", line, "function '%s' expects at most %zu argument(s), got %zu",
                     meta->name, nfixed, npos);
  for (i = 0; i < npos; i++) {
    if (i < nfixed) {
      bound[i] = posvals[i];
    } else {
      /* Extra positionals collect into the vararg slot's list (§6). The
       * slot may already hold a list when named args bound it — they
       * cannot (vararg names are not namable), so it is always MISSING. */
      if (bound[vararg_idx].tag == CS_MISSING) bound[vararg_idx] = cs_list_new();
      cs_list_push(bound[vararg_idx], posvals[i], line);
    }
  }

  /* 2. Named arguments (value i is posvals[npos + i]). */
  for (i = 0; i < nnamed; i++) {
    const char* nm = argnames[i];
    size_t j, found = (size_t)-1;
    cs_value nv = posvals[npos + i];
    for (j = 0; j < meta->n_params; j++) {
      if (has_vararg && j == vararg_idx) continue; /* not namable */
      if (has_kwarg && j == kwarg_idx) continue;    /* not namable */
      if (strcmp(meta->param_names[j], nm) == 0) {
        found = j;
        break;
      }
    }
    if (found == (size_t)-1) {
      /* With **kwargs, names outside the plain parameter list collect
       * into the kwargs map below instead of erroring (§6). */
      if (has_kwarg) continue;
      cs_runtime_error_as("TypeError", line, "unexpected argument name '%s' in call to '%s'", nm,
                       meta->name);
    }
    if (bound[found].tag != CS_MISSING)
      cs_runtime_error_as("TypeError", line, "argument '%s' got multiple values in call to '%s'",
                       nm, meta->name);
    bound[found] = nv;
  }
  if (has_kwarg && bound[kwarg_idx].tag == CS_MISSING) {
    /* Named extras were bound directly above only for plain params; build
     * the kwargs map from any leftover named args. */
    cs_value kw = cs_map_new();
    for (i = 0; i < nnamed; i++) {
      const char* nm = argnames[i];
      size_t j;
      bool is_param = false;
      for (j = 0; j < meta->n_params; j++) {
        if (has_vararg && j == vararg_idx) continue;
        if (j == kwarg_idx) continue;
        if (strcmp(meta->param_names[j], nm) == 0) {
          is_param = true;
          break;
        }
      }
      if (!is_param) cs_map_set(kw, cs_str(nm, strlen(nm)), posvals[npos + i], line);
    }
    bound[kwarg_idx] = kw;
  }

  /* 3. Required-parameter residue (§11 wording, checked at runtime). */
  for (i = 0; i < meta->n_required; i++) {
    if (bound[i].tag == CS_MISSING)
      cs_runtime_error_as("TypeError", line, "function '%s' expects at least %zu argument(s), got %zu",
                       meta->name, meta->n_required, npos + nnamed);
  }

  /* 4. Vararg/kwarg slots left CS_MISSING are initialized empty by the
   * callee's entry code (§13); defaults stay MISSING for the callee.
   * Slot materialization copies everything it needs out of `bound` (the
   * callee protects `bound` across its own entry safe point — 2C §1.3(4)),
   * so the vector itself is transient. */
  return meta->fn(bound, env);
}

/* Bound-vector storage cap for the stack fast paths (§13 binder): at or
 * below this many slots the bound vector is built on the C stack instead
 * of heap-allocated per call — the exact-arity path skips the binder
 * entirely, the binder path keeps every check byte-identical with only
 * the storage location changed. Wider functions keep the heap vector:
 * a deep recursion with a large VLA per frame could overflow the C
 * stack. */
#define CS_CALL_STACK_MAX 64u

cs_value cs_call(cs_value fnv, const cs_value* posvals, size_t npos,
                 const char* const* argnames, size_t nnamed, int line) {
  const cs_funmeta* meta;
  cs_cell** env;
  cs_value* bound;
  size_t i, nfixed, vararg_idx = 0, kwarg_idx = 0;
  bool has_vararg, has_kwarg;

  if (fnv.tag == CS_CLASS) {
    /* M2-2C §2.2/§3.2: calling a class constructs an instance — builtin
     * exception classes build exceptions, user classes run __init__. */
    if (fnv.as.o->as.cls.is_exc) {
      if (nnamed > 0u)
        cs_runtime_error_as(
            "TypeError", line, "class constructor got unexpected argument name '%s'",
            argnames[0]);
      return cs_exc_construct(fnv, posvals, npos, line);
    }
    return cs_class_call(fnv, posvals, npos, argnames, nnamed, line);
  }
  if (fnv.tag == CS_BOUND_METHOD) /* M2-2C §3.2: self prepended below */
    return cs_bound_call(fnv, posvals, npos, argnames, nnamed, line);
  if (fnv.tag != CS_FUNCTION)
    cs_runtime_error_as("TypeError", line, "object of type '%s' is not callable",
                     cs_type_name(fnv));
  meta = fnv.as.o->as.fun.meta;
  env = fnv.as.o->as.fun.env;
  has_vararg = meta->has_vararg;
  has_kwarg = meta->has_kwarg;
  if (has_kwarg) kwarg_idx = meta->n_params - 1u;
  if (has_vararg) vararg_idx = meta->n_params - 1u - (has_kwarg ? 1u : 0u);
  nfixed = meta->n_params - (has_vararg ? 1u : 0u) - (has_kwarg ? 1u : 0u);

  if (!has_vararg && !has_kwarg &&
      meta->n_params + 1u <= CS_CALL_STACK_MAX) {
    /* Stack-bound vector (storage-only change — see CS_CALL_STACK_MAX). */
    cs_value bound_s[meta->n_params + 1u]; /* VLA ≥ 1 element */
    cs_value r;
    /* Exact-arity fast path (§13): every parameter is provided
     * positionally (nnamed == 0, npos == n_params), so every binder check
     * is provably unable to fire:
     *   - "expects at most" (npos > nfixed && !has_vararg): nfixed ==
     *     n_params == npos here, so it is false;
     *   - positional binding fills every slot i < n_params with
     *     posvals[i] (the vararg branch is unreachable — no vararg);
     *   - the named-argument loops and the kwargs map need nnamed > 0;
     *   - required residue: bound[0..n_required) are all filled
     *     (n_required <= n_params by the §13 meta contract).
     * The one value that could still change behavior is a caller-passed
     * CS_MISSING (the residue check treats it like an omitted argument):
     * scanning for it keeps even that case byte-identical — such a call
     * falls through to the binder below, which reproduces the historical
     * error. With no MISSING present the bound vector is pure data, and
     * copying the arguments onto the C stack and invoking the uniform ABI
     * directly is observably identical: the callee copies what it needs
     * out of `bound` at entry (rooting it via the protect stack), and
     * cs_throw drops on-stack unwind entries without free()ing them. */
    if (nnamed == 0u && npos == meta->n_params) {
      bool exact = true;
      for (i = 0; i < npos; i++) {
        if (posvals[i].tag == CS_MISSING) { exact = false; break; }
      }
      if (exact) {
        for (i = 0; i < npos; i++) bound_s[i] = posvals[i];
        cs_call_push_stack(bound_s, line);
        r = meta->fn(bound_s, env);
        cs_call_pop_free();
        return r;
      }
    }
    for (i = 0; i < meta->n_params; i++) bound_s[i] = cs_missing();
    cs_call_push_stack(bound_s, line);
    r = cs_call_bind_invoke(meta, bound_s, env, posvals, npos, argnames,
                            nnamed, vararg_idx, kwarg_idx, nfixed, line);
    cs_call_pop_free();
    return r;
  }

  bound = (cs_value*)malloc((meta->n_params + 1u) * sizeof(cs_value));
  if (bound == NULL) cs_runtime_error(line, "out of memory");
  /* Track the vector for unwinding (2C §2.5): cs_throw frees every vector
   * pushed after the landing entry, so a longjmp out of the callee cannot
   * leak it. Normal completion pops it below. */
  cs_call_push_bound(bound, line);
  for (i = 0; i < meta->n_params; i++) bound[i] = cs_missing();
  {
    cs_value r = cs_call_bind_invoke(meta, bound, env, posvals, npos,
                                     argnames, nnamed, vararg_idx, kwarg_idx,
                                     nfixed, line);
    cs_call_pop_free();
    return r;
  }
}

/* ---- method dispatch (§15) -------------------------------------------- *
 *
 * §10: every builtin-method rung used to gate itself with a libc strcmp
 * call — cs_call_method's compiled image carried 42 `call strcmp@PLT`
 * sites, i.e. EVERY runtime method dispatch paid a PLT round trip plus the
 * string compare no matter how short the name was. Each rung now proves the
 * name with the receiver's first byte and inline byte compares against the
 * literal (a rung that cannot match costs one byte test), preserving
 * EXACTLY the old ladder: the same names in the same order, arity checks
 * still only after the name matched, and every TypeError/arity/argument
 * message byte-identical (the messages interpolate `m` verbatim, which
 * this proof never rewrites). Safe address-wise: byte k (k <= strlen(m))
 * is read only when bytes 1..k-1 already matched and were non-NUL, so the
 * scan never passes the terminating NUL of a short name.
 */

static void cs_arity_fixed(const char* t, const char* m, size_t want,
                           size_t got, int line) {
  /* Wording is normative (M2-2B §12): `'<t>.<m>() takes N argument(s)` —
   * the contract quotes it without a closing apostrophe; do not "fix". */
  if (got != want)
    cs_runtime_error_as("TypeError", line, "'%s.%s() takes %zu argument(s), got %zu", t, m,
                     want, got);
}

static void cs_arity_range(const char* t, const char* m, size_t lo, size_t hi,
                           size_t got, int line) {
  if (got < lo || got > hi)
    cs_runtime_error_as("TypeError", line, "'%s.%s() takes between %zu and %zu argument(s), got %zu'",
                     t, m, lo, hi, got);
}

static void cs_expect_text(const char* t, const char* m, cs_value v, int line) {
  if (v.tag != CS_STRING)
    cs_runtime_error_as("TypeError", line, "'%s.%s() expects text, got %s", t, m,
                     cs_type_name(v));
}

/* ---- list methods (§15) ---- */

static cs_value cs_method_list(cs_value obj, const char* m,
                               const cs_value* args, size_t n, int line) {
  static const char* T = "list";
  struct cs_obj* o = obj.as.o;
  if (m[0] == 'a' && m[1] == 'p' && m[2] == 'p' && m[3] == 'e' && m[4] == 'n' && m[5] == 'd' && m[6] == '\0') {
    cs_arity_fixed(T, m, 1u, n, line);
    cs_list_push(obj, args[0], line);
    return cs_none();
  }
  if (m[0] == 'p' && m[1] == 'o' && m[2] == 'p' && m[3] == '\0') { /* pop() / pop(i) / pop(i, default) */
    cs_arity_range(T, m, 0u, 2u, n, line);
    {
      int64_t i = (int64_t)o->as.seq.len - 1;
      cs_value out;
      if (n == 0u) {
        if (o->as.seq.len == 0u) cs_runtime_error_as("IndexError", line, "list index out of range");
      } else {
        if (args[0].tag != CS_INT)
          cs_runtime_error_as("TypeError", line, "list indices must be integers, not %s",
                           cs_type_name(args[0]));
        i = args[0].as.i;
        if (i < 0) i += (int64_t)o->as.seq.len;
        if (i < 0 || i >= (int64_t)o->as.seq.len) {
          if (n == 2u) return args[1]; /* out of range → default */
          cs_runtime_error_as("IndexError", line, "list index out of range");
        }
      }
      out = cs_seq_at(o, (size_t)i);
      cs_seq_ensure_owner(o, 0u);
      memmove(&o->as.seq.items[i], &o->as.seq.items[i + 1u],
              (o->as.seq.len - (size_t)i - 1u) * sizeof(cs_value));
      o->as.seq.len -= 1u;
      return out;
    }
  }
  if (m[0] == 'i' && m[1] == 'n' && m[2] == 's' && m[3] == 'e' && m[4] == 'r' && m[5] == 't' && m[6] == '\0') { /* Python-style clamped insert */
    cs_arity_fixed(T, m, 2u, n, line);
    if (args[0].tag != CS_INT)
      cs_runtime_error_as("TypeError", line, "list indices must be integers, not %s",
                       cs_type_name(args[0]));
    {
      int64_t i = args[0].as.i;
      cs_list_push(obj, cs_none(), line); /* grow by one, then shift */
      if (i < 0) i += (int64_t)o->as.seq.len - 1;
      if (i < 0) i = 0;
      if (i > (int64_t)o->as.seq.len - 1) i = (int64_t)o->as.seq.len - 1;
      memmove(&o->as.seq.items[i + 1u], &o->as.seq.items[i],
              ((size_t)(int64_t)o->as.seq.len - 1u - (size_t)i) * sizeof(cs_value));
      o->as.seq.items[(size_t)i] = args[1];
      return cs_none();
    }
  }
  if (m[0] == 'i' && m[1] == 'n' && m[2] == 'd' && m[3] == 'e' && m[4] == 'x' && m[5] == '\0') { /* first matching element, else error */
    size_t i;
    cs_arity_fixed(T, m, 1u, n, line);
    for (i = 0; i < o->as.seq.len; i++)
      if (cs_value_eq(cs_seq_at(o, i), args[0])) return cs_int((int64_t)i);
    cs_runtime_error_as("ValueError", line, "value not in list");
  }
  if (m[0] == 'r' && m[1] == 'e' && m[2] == 'm' && m[3] == 'o' && m[4] == 'v' && m[5] == 'e' && m[6] == '\0') {
    size_t i;
    cs_arity_fixed(T, m, 1u, n, line);
    for (i = 0; i < o->as.seq.len; i++) {
      if (cs_value_eq(cs_seq_at(o, i), args[0])) {
        cs_seq_ensure_owner(o, 0u);
        memmove(&o->as.seq.items[i], &o->as.seq.items[i + 1u],
                (o->as.seq.len - i - 1u) * sizeof(cs_value));
        o->as.seq.len -= 1u;
        return cs_none();
      }
    }
    cs_runtime_error_as("ValueError", line, "value not in list");
  }
  if (m[0] == 'r' && m[1] == 'e' && m[2] == 'v' && m[3] == 'e' && m[4] == 'r' && m[5] == 's' && m[6] == 'e' && m[7] == '\0') {
    size_t i, n2 = o->as.seq.len;
    cs_arity_fixed(T, m, 0u, n, line);
    cs_seq_ensure_owner(o, 0u);
    for (i = 0; i < n2 / 2u; i++) {
      cs_value tmp = o->as.seq.items[i];
      o->as.seq.items[i] = o->as.seq.items[n2 - 1u - i];
      o->as.seq.items[n2 - 1u - i] = tmp;
    }
    return cs_none();
  }
  if (m[0] == 'c' && m[1] == 'l' && m[2] == 'e' && m[3] == 'a' && m[4] == 'r' && m[5] == '\0') {
    cs_arity_fixed(T, m, 0u, n, line);
    o->as.seq.len = 0;
    return cs_none();
  }
  cs_runtime_error_as("TypeError", line, "'%s' object has no method '%s'", T, m);
  return cs_none(); /* not reached */
}

/* ---- map methods (§15) ---- */

static cs_value cs_method_map(cs_value obj, const char* m, const cs_value* args,
                              size_t n, int line) {
  static const char* T = "map";
  cs_table* t = &obj.as.o->as.table;
  if (m[0] == 'g' && m[1] == 'e' && m[2] == 't' && m[3] == '\0') { /* get(k) / get(k, default → None) */
    uint64_t h;
    size_t i;
    cs_arity_range(T, m, 1u, 2u, n, line);
    h = cs_table_hash(t, args[0], line);
    i = cs_table_find(t, args[0], h);
    if (i != (size_t)-1) return t->vals[i];
    return (n == 2u) ? args[1] : cs_none();
  }
  if ((m[0] == 'k' && m[1] == 'e' && m[2] == 'y' && m[3] == 's' && m[4] == '\0') ||
      (m[0] == 'v' && m[1] == 'a' && m[2] == 'l' && m[3] == 'u' && m[4] == 'e' && m[5] == 's' && m[6] == '\0') ||
      (m[0] == 'i' && m[1] == 't' && m[2] == 'e' && m[3] == 'm' && m[4] == 's' && m[5] == '\0')) {
    bool keys = (m[1] == 'e' && m[2] == 'y' && m[3] == 's' && m[4] == '\0');
    bool items = (m[1] == 't' && m[2] == 'e' && m[3] == 'm' && m[4] == 's' && m[5] == '\0');
    cs_value out;
    size_t i;
    cs_arity_fixed(T, m, 0u, n, line);
    out = cs_list_new();
    for (i = 0; i < t->len; i++) {
      if (!t->e[i].used) continue;
      if (items) {
        cs_value pair[2];
        pair[0] = t->e[i].key;
        pair[1] = t->vals[i];
        cs_list_push(out, cs_tuple_of(pair, 2u), line);
      } else {
        cs_list_push(out, keys ? t->e[i].key : t->vals[i], line);
      }
    }
    return out;
  }
  if (m[0] == 'p' && m[1] == 'o' && m[2] == 'p' && m[3] == '\0') { /* pop(k) / pop(k, default); missing without
                                * default → `map key not found` */
    uint64_t h;
    size_t i;
    cs_value out;
    cs_arity_range(T, m, 1u, 2u, n, line);
    h = cs_table_hash(t, args[0], line);
    i = cs_table_find(t, args[0], h);
    if (i == (size_t)-1) {
      if (n == 2u) return args[1];
      cs_runtime_error_as("KeyError", line, "map key not found");
    }
    out = t->vals[i];
    cs_table_delete(t, i);
    return out;
  }
  if (m[0] == 'c' && m[1] == 'l' && m[2] == 'e' && m[3] == 'a' && m[4] == 'r' && m[5] == '\0') {
    size_t i;
    cs_arity_fixed(T, m, 0u, n, line);
    for (i = 0; i < t->len; i++) t->e[i].used = false;
    t->live = 0;
    t->tombstones = t->len;
    return cs_none();
  }
  cs_runtime_error_as("TypeError", line, "'%s' object has no method '%s'", T, m);
  return cs_none(); /* not reached */
}

/* ---- set methods (§15) ---- */

static cs_value cs_method_set(cs_value obj, const char* m, const cs_value* args,
                              size_t n, int line) {
  static const char* T = "set";
  cs_table* t = &obj.as.o->as.table;
  if (m[0] == 'a' && m[1] == 'd' && m[2] == 'd' && m[3] == '\0') {
    cs_arity_fixed(T, m, 1u, n, line);
    (void)cs_set_add(obj, args[0], line);
    return cs_none();
  }
  if (m[0] == 'r' && m[1] == 'e' && m[2] == 'm' && m[3] == 'o' && m[4] == 'v' && m[5] == 'e' && m[6] == '\0') {
    uint64_t h;
    size_t i;
    cs_arity_fixed(T, m, 1u, n, line);
    h = cs_table_hash(t, args[0], line);
    i = cs_table_find(t, args[0], h);
    if (i == (size_t)-1) cs_runtime_error_as("KeyError", line, "value not in set");
    cs_table_delete(t, i);
    return cs_none();
  }
  if (m[0] == 'd' && m[1] == 'i' && m[2] == 's' && m[3] == 'c' && m[4] == 'a' && m[5] == 'r' && m[6] == 'd' && m[7] == '\0') {
    uint64_t h;
    size_t i;
    cs_arity_fixed(T, m, 1u, n, line);
    h = cs_table_hash(t, args[0], line);
    i = cs_table_find(t, args[0], h);
    if (i != (size_t)-1) cs_table_delete(t, i);
    return cs_none();
  }
  if (m[0] == 'p' && m[1] == 'o' && m[2] == 'p' && m[3] == '\0') { /* remove + return the first element */
    size_t i;
    cs_value out;
    cs_arity_fixed(T, m, 0u, n, line);
    i = cs_table_first(t);
    if (i == (size_t)-1) cs_runtime_error_as("KeyError", line, "pop from an empty set");
    out = t->e[i].key;
    cs_table_delete(t, i);
    return out;
  }
  if (m[0] == 'c' && m[1] == 'l' && m[2] == 'e' && m[3] == 'a' && m[4] == 'r' && m[5] == '\0') {
    size_t i;
    cs_arity_fixed(T, m, 0u, n, line);
    for (i = 0; i < t->len; i++) t->e[i].used = false;
    t->live = 0;
    t->tombstones = t->len;
    return cs_none();
  }
  cs_runtime_error_as("TypeError", line, "'%s' object has no method '%s'", T, m);
  return cs_none(); /* not reached */
}

/* ---- text methods (§15; ASCII-only case mapping, documented) ---- */

static bool cs_ascii_ws(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

/* Case maps ASCII letters only (documented 2B limitation). The result
 * buffer is fully written (loop + explicit NUL) — no zeroing memset. */
static cs_value cs_text_case(cs_value v, bool upper, int line) {
  char* p = NULL;
  size_t i;
  if (v.as.s.len > 0u) {
    p = cs_gc_buf_filled(CS_GC_TEXT, v.as.s.len, line); /* full write below */
    for (i = 0; i < v.as.s.len; i++) {
      char c = v.as.s.p[i];
      if (upper && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
      else if (!upper && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      p[i] = c;
    }
    p[v.as.s.len] = '\0';
  }
  return cs_str(p, v.as.s.len);
}

/* Code-point index of the byte offset `off` in `v` — find()'s byte hit
 * converted to the value it returns. Memoized through cs_text_prefix
 * (whole-view length is the memo identity): searching the same haystack
 * 20,000 times converts the hit once, then answers O(1). */
static int64_t cs_text_cp_index(cs_value v, size_t off) {
  return (int64_t)cs_text_prefix(v.as.s.p, v.as.s.len, off);
}

/* Next occurrence of the non-empty needle `s` at byte offset >= `pos` in
 * [p, p+len), or `len` when there is none. memchr-skips to the needle's
 * first byte — the same match positions the byte-by-byte memcmp scan
 * found (the scan is a pure function of the immutable inputs, so the
 * two-pass split/replace below re-derive identical positions). */
static size_t cs_text_find_sep(const char* p, size_t len, size_t pos,
                               const char* s, size_t qlen) {
  while (pos + qlen <= len) {
    const char* f = (const char*)memchr(p + pos, s[0], len - pos);
    if (f == NULL) return len;
    pos = (size_t)(f - p);
    if (pos + qlen > len) return len;
    if (memcmp(p + pos, s, qlen) == 0) return pos;
    pos += 1u;
  }
  return len;
}

static cs_value cs_text_split(cs_value v, cs_value sep, int line) {
  const char* p = v.as.s.p;
  size_t len = v.as.s.len, qlen = sep.as.s.len;
  size_t start = 0, pos = 0, np = 1u, i = 0;
  cs_value out;
  cs_value* items;
  /* Pieces are independent GC texts (the runtime's lifetime rule: text
   * pointing INTO another text's storage must be copied — an interior
   * pointer would dangle once the source block is swept; cf. the
   * cs_iter_next text case). Two pure scans: count pieces (pre-size the
   * list exactly — no doubling reallocs), then copy each piece into its
   * own filled block (no zeroing memset). */
  while ((pos = cs_text_find_sep(p, len, pos, sep.as.s.p, qlen)) < len) {
    np++;
    pos += qlen;
  }
  out = cs_list_new();
  items = (cs_value*)malloc(np * sizeof(cs_value));
  if (items == NULL) cs_runtime_error(line, "out of memory");
  out.as.o->as.seq.items = items;
  out.as.o->as.seq.len = np;
  out.as.o->as.seq.cap = np;
  pos = 0;
  while ((pos = cs_text_find_sep(p, len, pos, sep.as.s.p, qlen)) < len) {
    items[i++] = cs_text_copy(p + start, pos - start, line);
    pos += qlen;
    start = pos;
  }
  items[i] = cs_text_copy(p + start, len - start, line);
  return out;
}

static cs_value cs_text_join(cs_value sep, cs_value iterv, int line) {
  cs_iter it = cs_iter_open(iterv, line);
  cs_value e;
  size_t total = 0, off, n;
  char* p;
  bool first;
  /* Two passes over a SIDE-EFFECT-FREE source (list/tuple/set/map/range/
   * text/bytes — cs_iter_next for those is a pure cursor walk): pass 1
   * validates + measures, pass 2 fills ONE exact-size block — no temp
   * buffer and no second copy. Boxed iterators and files have consumable
   * cursors (a second pass would observe/advance them), so they keep the
   * single-pass temp-buffer path below. */
  if (it.kind == CS_ITER_BOXED || it.kind == CS_ITER_FILE) {
    cs_buf b;
    bool firstb = true;
    b.p = NULL;
    b.len = 0;
    b.cap = 0;
    while (cs_iter_next(&it, &e)) {
      if (e.tag != CS_STRING) {
        free(b.p);
        cs_runtime_error_as("TypeError", line, "'text.join() expects text elements, got %s",
                         cs_type_name(e));
      }
      if (!firstb) cs_buf_addn(&b, sep.as.s.p, sep.as.s.len, line);
      firstb = false;
      cs_buf_addn(&b, e.as.s.p, e.as.s.len, line);
    }
    cs_buf_end(&b, line);
    return cs_text_from_buf(&b, line);
  }
  n = 0;
  while (cs_iter_next(&it, &e)) {
    if (e.tag != CS_STRING)
      cs_runtime_error_as("TypeError", line, "'text.join() expects text elements, got %s",
                       cs_type_name(e));
    if (n > 0) total += sep.as.s.len;
    total += e.as.s.len;
    n++;
  }
  if (total == 0u) return cs_str(NULL, 0); /* empty / all-empty: no block */
  p = cs_gc_buf_filled(CS_GC_TEXT, total, line); /* full write below */
  it = cs_iter_open(iterv, line);
  first = true;
  off = 0;
  while (cs_iter_next(&it, &e)) {
    if (!first) {
      memcpy(p + off, sep.as.s.p, sep.as.s.len);
      off += sep.as.s.len;
    }
    first = false;
    memcpy(p + off, e.as.s.p, e.as.s.len);
    off += e.as.s.len;
  }
  p[total] = '\0';
  return cs_str(p, total);
}

/* First occurrence of needle in text; returns the CODE POINT index or -1. */
static int64_t cs_text_find(cs_value hay, cs_value needle, int line) {
  size_t i, last;
  (void)line;
  if (needle.as.s.len == 0u) return 0;
  if (needle.as.s.len > hay.as.s.len) return -1;
  last = hay.as.s.len - needle.as.s.len;
  i = 0;
  for (;;) { /* memchr-skip to the needle's first byte — same matches */
    const char* f = (const char*)memchr(hay.as.s.p + i, needle.as.s.p[0],
                                        last - i + 1u);
    if (f == NULL) return -1;
    i = (size_t)(f - hay.as.s.p);
    if (memcmp(hay.as.s.p + i, needle.as.s.p, needle.as.s.len) == 0)
      return cs_text_cp_index(hay, i);
    i += 1u;
  }
}

static cs_value cs_text_replace(cs_value v, cs_value oldv, cs_value newv,
                                int line) {
  size_t olen = oldv.as.s.len, nlen = newv.as.s.len;
  size_t len = v.as.s.len;
  const char* p = v.as.s.p;
  size_t pos = 0, nhit = 0, total, off, start;
  char* out;
  /* Pass 1 (pure scan over the immutable source text): count matches. */
  while ((pos = cs_text_find_sep(p, len, pos, oldv.as.s.p, olen)) < len) {
    nhit++;
    pos += olen;
  }
  if (nhit == 0u) return cs_text_copy(p, len, line); /* value-identical */
  /* Exact result size up front: one filled block, no temp buffer. The
   * non-overlapping replace semantics (advance by olen past a match) are
   * re-run by the identical scan in pass 2. */
  total = len - nhit * olen + nhit * nlen;
  out = cs_gc_buf_filled(CS_GC_TEXT, total, line); /* full write below */
  pos = 0;
  off = 0;
  start = 0;
  while ((pos = cs_text_find_sep(p, len, pos, oldv.as.s.p, olen)) < len) {
    if (pos > start) {
      memcpy(out + off, p + start, pos - start);
      off += pos - start;
    }
    if (nlen > 0) {
      memcpy(out + off, newv.as.s.p, nlen);
      off += nlen;
    }
    pos += olen;
    start = pos;
  }
  if (len > start) {
    memcpy(out + off, p + start, len - start);
    off += len - start;
  }
  out[total] = '\0';
  return cs_str(out, total);
}

static cs_value cs_text_strip(cs_value v, int line) {
  size_t a = 0, b2 = v.as.s.len;
  while (a < b2 && cs_ascii_ws(v.as.s.p[a])) a += 1u;
  while (b2 > a && cs_ascii_ws(v.as.s.p[b2 - 1u])) b2 -= 1u;
  {
    cs_buf b;
    b.p = NULL;
    b.len = 0;
    b.cap = 0;
    cs_buf_addn(&b, v.as.s.p + a, b2 - a, line);
    cs_buf_end(&b, line);
    return cs_text_from_buf(&b, line);
  }
}

static cs_value cs_method_text(cs_value obj, const char* m,
                               const cs_value* args, size_t n, int line) {
  static const char* T = "text";
  if (m[0] == 'u' && m[1] == 'p' && m[2] == 'p' && m[3] == 'e' && m[4] == 'r' && m[5] == '\0') {
    cs_arity_fixed(T, m, 0u, n, line);
    return cs_text_case(obj, true, line);
  }
  if (m[0] == 'l' && m[1] == 'o' && m[2] == 'w' && m[3] == 'e' && m[4] == 'r' && m[5] == '\0') {
    cs_arity_fixed(T, m, 0u, n, line);
    return cs_text_case(obj, false, line);
  }
  if (m[0] == 's' && m[1] == 'p' && m[2] == 'l' && m[3] == 'i' && m[4] == 't' && m[5] == '\0') { /* split(sep): 1 non-empty text arg */
    cs_arity_fixed(T, m, 1u, n, line);
    cs_expect_text(T, m, args[0], line);
    if (args[0].as.s.len == 0u) cs_runtime_error_as("ValueError", line, "empty separator in split()");
    return cs_text_split(obj, args[0], line);
  }
  if (m[0] == 'j' && m[1] == 'o' && m[2] == 'i' && m[3] == 'n' && m[4] == '\0') {
    cs_arity_fixed(T, m, 1u, n, line);
    return cs_text_join(obj, args[0], line);
  }
  if (m[0] == 'f' && m[1] == 'i' && m[2] == 'n' && m[3] == 'd' && m[4] == '\0') {
    cs_arity_fixed(T, m, 1u, n, line);
    cs_expect_text(T, m, args[0], line);
    return cs_int(cs_text_find(obj, args[0], line));
  }
  if (m[0] == 'r' && m[1] == 'e' && m[2] == 'p' && m[3] == 'l' && m[4] == 'a' && m[5] == 'c' && m[6] == 'e' && m[7] == '\0') {
    cs_arity_fixed(T, m, 2u, n, line);
    cs_expect_text(T, m, args[0], line);
    cs_expect_text(T, m, args[1], line);
    if (args[0].as.s.len == 0u)
      cs_runtime_error_as("ValueError", line, "empty pattern in replace()");
    return cs_text_replace(obj, args[0], args[1], line);
  }
  if (m[0] == 's' && m[1] == 't' && m[2] == 'r' && m[3] == 'i' && m[4] == 'p' && m[5] == '\0') {
    cs_arity_fixed(T, m, 0u, n, line);
    return cs_text_strip(obj, line);
  }
  if ((m[0] == 's' && m[1] == 't' && m[2] == 'a' && m[3] == 'r' && m[4] == 't' && m[5] == 's' && m[6] == '_' && m[7] == 'w' && m[8] == 'i' && m[9] == 't' && m[10] == 'h' && m[11] == '\0') ||
      (m[0] == 'e' && m[1] == 'n' && m[2] == 'd' && m[3] == 's' && m[4] == '_' && m[5] == 'w' && m[6] == 'i' && m[7] == 't' && m[8] == 'h' && m[9] == '\0')) {
    bool starts = (m[1] == 't' && m[2] == 'a' && m[3] == 'r' && m[4] == 't' && m[5] == 's' && m[6] == '_' && m[7] == 'w' && m[8] == 'i' && m[9] == 't' && m[10] == 'h' && m[11] == '\0');
    cs_value p;
    cs_arity_fixed(T, m, 1u, n, line);
    cs_expect_text(T, m, args[0], line);
    p = args[0];
    if (p.as.s.len > obj.as.s.len) return cs_bool(false);
    if (starts)
      return cs_bool(memcmp(obj.as.s.p, p.as.s.p, p.as.s.len) == 0);
    return cs_bool(memcmp(obj.as.s.p + obj.as.s.len - p.as.s.len, p.as.s.p,
                          p.as.s.len) == 0);
  }
  cs_runtime_error_as("TypeError", line, "'%s' object has no method '%s'", T, m);
  return cs_none(); /* not reached */
}

/* ---- bytes methods (§15) ---- */

static cs_value cs_method_bytes(cs_value obj, const char* m,
                                const cs_value* args, size_t n, int line) {
  static const char* T = "bytes";
  (void)args; /* decode() takes no arguments */
  if (m[0] == 'd' && m[1] == 'e' && m[2] == 'c' && m[3] == 'o' && m[4] == 'd' && m[5] == 'e' && m[6] == '\0') {
    cs_arity_fixed(T, m, 0u, n, line);
    return cs_builtin_decode(obj, line); /* §5 validating UTF-8 */
  }
  cs_runtime_error_as("TypeError", line, "'%s' object has no method '%s'", T, m);
  return cs_none(); /* not reached */
}

/* ---- dispatch (§13/§15) ---- */

cs_value cs_call_method(cs_value obj, const char* name, const cs_value* args,
                        size_t nargs, const char* const* argnames,
                        size_t nnamed, int line) {
  cs_no_missing(obj, line);
  if (obj.tag == CS_INSTANCE) {
    /* M2-2C §3.2: methods resolve through the instance-method inline
     * cache (see the cache docs by CS_MIC_SIZE): a hit plus the
     * field-shadow guard dispatches straight to the method's uniform ABI
     * with a C-stack self vector — no bound method, no hash lookup, no
     * malloc. A non-exact call (defaults/vararg/named args) still skips
     * the bound-method allocation but keeps the §13 binder: the self
     * vector is heap-tracked and cs_call produces the exact binding
     * errors. Instance fields and missing names fall back to the ordinary
     * getattr path, byte-identical to before. */
    cs_obj* io = obj.as.o;
    cs_obj* cls = io->as.inst.cls.as.o;
    cs_mic_entry* e = &cs_mic[cs_mic_index(cls, name)];
    cs_value fnv;
    if (e->cls == cls && strcmp(e->name, name) == 0 &&
        !cs_inst_field_shadowed(io, name, line)) {
      /* Guards 1+2+3: same class, same name, no shadowing field. */
      fnv = cs_obj_value(e->fn, CS_FUNCTION);
    } else {
      fnv = cs_inst_method(io, name, line); /* full getattr-order resolve */
      if (fnv.tag == CS_FUNCTION) {         /* fill/replace the cache slot */
        e->name = name; /* value+name first, class last (see cs_class_call) */
        e->fn = fnv.as.o;
        e->cls = cls;
      }
    }
    if (fnv.tag == CS_FUNCTION) {
      const cs_funmeta* meta = fnv.as.o->as.fun.meta;
      if (nnamed == 0u && !meta->has_vararg && !meta->has_kwarg &&
          nargs + 1u == meta->n_params) {
        /* Exact positional arity (and no shadowing field — the guard
         * above): every bound slot is filled by the prepended self and
         * the arguments, so no MISSING slot, default, duplicate or
         * required-residue check in the §13 binder could fire — build
         * the vector on the C stack and invoke the uniform ABI directly.
         * The callee roots `bound` (and hence the receiver) at its entry
         * safe point exactly as the binder path does, and an exception
         * unwinds cleanly (cs_throw drops stack entries without freeing
         * them). */
        cs_value bound[meta->n_params]; /* C11 VLA; n_params ≥ 1 */
        cs_value r;
        size_t j;
        bound[0] = obj;
        for (j = 0; j < nargs; j++) bound[j + 1u] = args[j];
        cs_call_push_stack(bound, line);
        r = meta->fn(bound, fnv.as.o->as.fun.env);
        cs_call_pop_free();
        return r;
      }
      /* Not exact (defaults/vararg/kwarg/named): self-prepended heap
       * vector + the ordinary binder — same binding, same errors. */
      {
        cs_value* arr = (cs_value*)malloc((nargs + nnamed + 1u) * sizeof(cs_value));
        cs_value r;
        size_t j;
        if (arr == NULL) cs_runtime_error(line, "out of memory");
        arr[0] = obj;
        for (j = 0; j < nargs + nnamed; j++) arr[j + 1u] = args[j];
        cs_call_push_bound(arr, line);
        r = cs_call(fnv, arr, nargs + 1u, argnames, nnamed, line);
        cs_call_pop_free();
        return r;
      }
    }
    /* Instance field or missing name: getattr reproduces the attribute /
     * callable errors exactly (and serves a field that happens to hold a
     * bound method, function or class). */
    {
      cs_value b = cs_getattr(obj, name, line);
      return cs_call(b, args, nargs, argnames, nnamed, line);
    }
  }
  if (nnamed > 0u) cs_runtime_error_as("TypeError", line, "method arguments must be positional");
  switch (obj.tag) {
    case CS_LIST: return cs_method_list(obj, name, args, nargs, line);
    case CS_MAP: return cs_method_map(obj, name, args, nargs, line);
    case CS_SET: return cs_method_set(obj, name, args, nargs, line);
    case CS_STRING: return cs_method_text(obj, name, args, nargs, line);
    case CS_BYTES: return cs_method_bytes(obj, name, args, nargs, line);
    case CS_FILE: return cs_method_file(obj, name, args, nargs, line);
    default:
      cs_runtime_error_as("TypeError", line, "'%s' object has no method '%s'",
                       cs_type_name(obj), name);
      return cs_none(); /* not reached */
  }
}

/* ====================================================================== *
 * Python-parity builtins (SPEC §5.11.5) — the `dir(builtins)` port.     *
 * ====================================================================== *
 * One direct cs_builtin_* call per builtin (src/builtins.h drives the
 * semantic arity/named-argument checks and codegen's slot layout). The
 * design contract (docs/performance/optimization.md §11): scalar paths allocate
 * NOTHING, collection builders allocate exactly once per result, and the
 * native-int lane absorbs abs/min/max/pow/round over proven-int operands
 * (cs_nabs/cs_nmin/cs_nmax/cs_npow in the header). Documented deviations
 * from CPython are normative in SPEC §5.11.5: int64 wrapping, C99
 * divmod, int-only `%`, eager list results for enumerate/zip/filter/map/
 * reversed, UTF-8-only files, no descriptors/inheritance. */

/* ---- shared helpers --------------------------------------------------- */

static bool cs_is_callable(cs_value v) {
  return v.tag == CS_FUNCTION || v.tag == CS_BOUND_METHOD ||
         v.tag == CS_CLASS;
}

static cs_value cs_call1(cs_value fn, cs_value arg, int line) {
  return cs_call(fn, &arg, 1u, NULL, 0u, line);
}

/* UTF-8 encode one code point; dst must hold 4 bytes. */
static size_t cs_utf8_put(char* dst, uint32_t cp) {
  if (cp < 0x80u) {
    dst[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800u) {
    dst[0] = (char)(0xC0u | (cp >> 6));
    dst[1] = (char)(0x80u | (cp & 0x3Fu));
    return 2;
  }
  if (cp < 0x10000u) {
    dst[0] = (char)(0xE0u | (cp >> 12));
    dst[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    dst[2] = (char)(0x80u | (cp & 0x3Fu));
    return 3;
  }
  dst[0] = (char)(0xF0u | (cp >> 18));
  dst[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
  dst[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
  dst[3] = (char)(0x80u | (cp & 0x3Fu));
  return 4;
}

/* bin/oct/hex over the exact uint64 magnitude (INT64_MIN is fine). */
static cs_value cs_base_convert(cs_value x, int base, const char* prefix,
                                int line) {
  static const char* kDigits = "0123456789abcdef";
  char buf[72]; /* sign + 64 base-2 digits + NUL */
  uint64_t mag;
  size_t n = 0, i;
  bool neg;
  cs_no_missing(x, line); /* the error below matches Python's exact wording */
  if (x.tag != CS_INT)
    cs_runtime_error_as("TypeError", line,
                        "'%s' object cannot be interpreted as an integer",
                        cs_type_name(x));
  neg = x.as.i < 0;
  mag = neg ? (uint64_t)(0u - (uint64_t)x.as.i) : (uint64_t)x.as.i;
  do {
    buf[n++] = kDigits[mag % (uint64_t)base];
    mag /= (uint64_t)base;
  } while (mag != 0u);
  {
    cs_buf b;
    b.p = NULL;
    b.len = 0;
    b.cap = 0;
    if (neg) cs_buf_addc(&b, '-', line);
    cs_buf_addn(&b, prefix, strlen(prefix), line);
    for (i = n; i > 0; i--) cs_buf_addc(&b, buf[i - 1], line);
    return cs_text_from_buf(&b, line);
  }
}

/* ---- repr / ascii (SPEC §5.11.5) -------------------------------------- *
 * repr() = the display form with PYTHON QUOTING: text values are quoted
 * and escaped everywhere (str() keeps the legacy display form — byte
 * contract), bytes render b'...' with \xNN escapes, and objects render
 * their Python shapes minus the address (repr never leaks ASLR state).
 * ascii() is repr with every non-ASCII code point escaped (\u/\U). */

static void cs_repr_escape(cs_buf* b, const char* p, size_t len, char quote,
                           bool ascii_only, bool is_bytes, int line) {
  size_t pos = 0;
  while (pos < len) {
    size_t adv = 1;
    uint32_t cp;
    char tmp[16];
    if (is_bytes) {
      cp = (unsigned char)p[pos]; /* bytes repr is byte-wise */
    } else {
      cp = cs_utf8_step(p, len, pos, &adv);
    }
    if (cp == (uint32_t)(unsigned char)quote) {
      cs_buf_addn(b, "\\", 1u, line);
      cs_buf_addc(b, quote, line);
    } else if (cp == (uint32_t)'\\') {
      cs_buf_addn(b, "\\\\", 2u, line);
    } else if (cp == (uint32_t)'\n') {
      cs_buf_addn(b, "\\n", 2u, line);
    } else if (cp == (uint32_t)'\r') {
      cs_buf_addn(b, "\\r", 2u, line);
    } else if (cp == (uint32_t)'\t') {
      cs_buf_addn(b, "\\t", 2u, line);
    } else if (cp == (uint32_t)'\f') {
      cs_buf_addn(b, "\\f", 2u, line);
    } else if (cp == (uint32_t)'\a') {
      cs_buf_addn(b, "\\a", 2u, line);
    } else if (cp == (uint32_t)'\b') {
      cs_buf_addn(b, "\\b", 2u, line);
    } else if (cp < 0x20u || cp == 0x7Fu) {
      snprintf(tmp, sizeof(tmp), "\\x%02x", (unsigned)cp);
      cs_buf_addn(b, tmp, 4u, line);
    } else if (is_bytes && cp >= 0x80u) {
      snprintf(tmp, sizeof(tmp), "\\x%02x", (unsigned)cp);
      cs_buf_addn(b, tmp, 4u, line);
    } else if (ascii_only && cp >= 0x80u) {
      if (cp < 0x100u) {
        snprintf(tmp, sizeof(tmp), "\\x%02x", (unsigned)cp);
        cs_buf_addn(b, tmp, 4u, line);
      } else if (cp < 0x10000u) {
        snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)cp);
        cs_buf_addn(b, tmp, 6u, line);
      } else {
        snprintf(tmp, sizeof(tmp), "\\U%08x", (unsigned)cp);
        cs_buf_addn(b, tmp, 10u, line);
      }
    } else if (is_bytes) {
      cs_buf_addc(b, p[pos], line); /* raw printable byte */
    } else {
      cs_buf_addn(b, p + pos, adv, line); /* raw code point bytes */
    }
    pos += adv;
  }
}

/* Python's quote choice: single quotes unless that costs escapes. */
static char cs_repr_quote(const char* p, size_t len) {
  size_t i;
  bool has_sq = false, has_dq = false;
  for (i = 0; i < len; i++) {
    if (p[i] == '\'') has_sq = true;
    else if (p[i] == '"') has_dq = true;
  }
  return (has_sq && !has_dq) ? '"' : '\'';
}

static void cs_repr_buf(cs_buf* b, cs_value v, bool ascii_only, int line) {
  size_t i;
  switch (v.tag) {
    case CS_INT: {
      char tmp[32];
      cs_buf_addn(b, tmp, (size_t)cs_itoa_ll(tmp, v.as.i), line); /* == %lld */
      break;
    }
    case CS_FLOAT: {
      char tmp[64];
      cs_fmt_float(v.as.d, tmp, sizeof(tmp));
      cs_buf_addn(b, tmp, strlen(tmp), line);
      break;
    }
    case CS_BOOL:
      cs_buf_addn(b, v.as.b ? "True" : "False", v.as.b ? 4u : 5u, line);
      break;
    case CS_NONE:
      cs_buf_addn(b, "None", 4u, line);
      break;
    case CS_STRING: {
      char q = cs_repr_quote(v.as.s.p, v.as.s.len);
      cs_buf_addc(b, q, line);
      cs_repr_escape(b, v.as.s.p, v.as.s.len, q, ascii_only, false, line);
      cs_buf_addc(b, q, line);
      break;
    }
    case CS_BYTES: {
      char q = cs_repr_quote(v.as.s.p, v.as.s.len);
      cs_buf_addc(b, 'b', line);
      cs_buf_addc(b, q, line);
      cs_repr_escape(b, v.as.s.p, v.as.s.len, q, true, true, line);
      cs_buf_addc(b, q, line);
      break;
    }
    case CS_RANGE: {
      char tmp[32];
      cs_buf_addn(b, "range(", 6u, line);
      cs_buf_addn(b, tmp, (size_t)cs_itoa_ll(tmp, v.as.r.start), line);
      cs_buf_addn(b, ", ", 2u, line);
      cs_buf_addn(b, tmp, (size_t)cs_itoa_ll(tmp, v.as.r.stop), line);
      cs_buf_addn(b, ", ", 2u, line);
      cs_buf_addn(b, tmp, (size_t)cs_itoa_ll(tmp, v.as.r.step), line);
      cs_buf_addc(b, ')', line);
      break;
    }
    case CS_LIST:
      cs_buf_addc(b, '[', line);
      for (i = 0; i < v.as.o->as.seq.len; i++) {
        if (i > 0) cs_buf_addn(b, ", ", 2u, line);
        cs_repr_buf(b, cs_seq_at(v.as.o, i), ascii_only, line);
      }
      cs_buf_addc(b, ']', line);
      break;
    case CS_TUPLE:
      if (v.as.o->as.seq.len == 1u) {
        cs_buf_addc(b, '(', line);
        cs_repr_buf(b, cs_seq_at(v.as.o, 0), ascii_only, line);
        cs_buf_addn(b, ",)", 2u, line);
        break;
      }
      cs_buf_addc(b, '(', line);
      for (i = 0; i < v.as.o->as.seq.len; i++) {
        if (i > 0) cs_buf_addn(b, ", ", 2u, line);
        cs_repr_buf(b, cs_seq_at(v.as.o, i), ascii_only, line);
      }
      cs_buf_addc(b, ')', line);
      break;
    case CS_SET: {
      const cs_table* t = &v.as.o->as.table;
      bool first = true;
      cs_buf_addc(b, '{', line);
      for (i = 0; i < t->len; i++) {
        if (!t->e[i].used) continue;
        if (!first) cs_buf_addn(b, ", ", 2u, line);
        first = false;
        cs_repr_buf(b, t->e[i].key, ascii_only, line);
      }
      cs_buf_addc(b, '}', line);
      break;
    }
    case CS_MAP: {
      const cs_table* t = &v.as.o->as.table;
      bool first = true;
      cs_buf_addc(b, '{', line);
      for (i = 0; i < t->len; i++) {
        if (!t->e[i].used) continue;
        if (!first) cs_buf_addn(b, ", ", 2u, line);
        first = false;
        cs_repr_buf(b, t->e[i].key, ascii_only, line);
        cs_buf_addn(b, ": ", 2u, line);
        cs_repr_buf(b, t->vals[i], ascii_only, line);
      }
      cs_buf_addc(b, '}', line);
      break;
    }
    case CS_FUNCTION: /* Python shape, minus the address (SPEC §5.11.5) */
      cs_buf_addn(b, "<function ", 10u, line);
      cs_buf_addn(b, v.as.o->as.fun.meta->name,
                  strlen(v.as.o->as.fun.meta->name), line);
      cs_buf_addn(b, ">", 1u, line);
      break;
    case CS_CLASS: /* Python renders classes exactly like this */
      cs_buf_addn(b, "<class '", 8u, line);
      cs_buf_addn(b, v.as.o->as.cls.name, strlen(v.as.o->as.cls.name), line);
      cs_buf_addn(b, "'>", 2u, line);
      break;
    case CS_EXCEPTION: {
      const char* cn = v.as.o->as.exc.cls.as.o->as.cls.name;
      cs_buf_addn(b, cn, strlen(cn), line);
      cs_buf_addn(b, "('", 2u, line);
      if (v.as.o->as.exc.msg.tag == CS_STRING)
        cs_repr_escape(b, v.as.o->as.exc.msg.as.s.p,
                       v.as.o->as.exc.msg.as.s.len, '\'', ascii_only, false,
                       line);
      cs_buf_addn(b, "')", 2u, line);
      break;
    }
    case CS_INSTANCE: /* Python shape without the address */
      cs_buf_addn(b, "<", 1u, line);
      {
        const char* cn = v.as.o->as.inst.cls.as.o->as.cls.name;
        cs_buf_addn(b, cn, strlen(cn), line);
      }
      cs_buf_addn(b, " object>", 8u, line);
      break;
    case CS_BOUND_METHOD: {
      const char* cls =
          v.as.o->as.bnd.inst.tag == CS_INSTANCE
              ? v.as.o->as.bnd.inst.as.o->as.inst.cls.as.o->as.cls.name
              : cs_type_name(v.as.o->as.bnd.inst);
      const char* fn = v.as.o->as.bnd.fn.as.o->as.fun.meta->name;
      cs_buf_addn(b, "<bound method ", 14u, line);
      cs_buf_addn(b, cls, strlen(cls), line);
      cs_buf_addc(b, '.', line);
      cs_buf_addn(b, fn, strlen(fn), line);
      cs_buf_addn(b, ">", 1u, line);
      break;
    }
    case CS_ITERATOR:
      cs_buf_addn(b, "<iterator>", 10u, line);
      break;
    case CS_FILE:
      cs_buf_addn(b, "<file '", 7u, line);
      cs_buf_addn(b, v.as.o->as.file.path, strlen(v.as.o->as.file.path), line);
      cs_buf_addn(b, "'>", 2u, line);
      break;
    default: /* CS_UNSET / CS_MISSING are unreachable behind cs_no_missing */
      break;
  }
}

static cs_value cs_repr_value(cs_value v, bool ascii_only, int line) {
  cs_buf b;
  cs_no_missing(v, line);
  b.p = NULL;
  b.len = 0;
  b.cap = 0;
  cs_repr_buf(&b, v, ascii_only, line);
  return cs_text_from_buf(&b, line);
}

/* ---- open files (SPEC §5.11.5) ---------------------------------------- *
 * Registry of open handles: every CS_FILE links in on open() and leaves
 * on close(); the GC sweep closes whatever a program forgot (Python
 * closes on collection — cstar closes at collection or exit). The only
 * unconditional footprint is one pointer; the atexit hook installs on the
 * FIRST open, so a program that never opens a file pays nothing. */

static cs_obj* cs_open_files = NULL;
static char cs_files_atexit_installed = 0;

static void cs_file_exit_close(void) {
  while (cs_open_files != NULL) cs_file_close_obj(cs_open_files);
}

static void cs_file_close_obj(cs_obj* o) {
  cs_obj** pp;
  if (o->as.file.f != NULL) {
    fclose(o->as.file.f);
    o->as.file.f = NULL;
  }
  for (pp = &cs_open_files; *pp != NULL; pp = &(*pp)->as.file.next_open) {
    if (*pp == o) {
      *pp = o->as.file.next_open;
      o->as.file.next_open = NULL;
      break;
    }
  }
}

/* Raw byte reader behind read()/readline()/for-over-file: optionally at
 * most `size` code points (text) or bytes (binary), optionally stopping
 * after a '\n' (inclusive). Returns a NUL-terminated buffer (NULL + len 0
 * at EOF). The caller converts it and frees it UNLESS *borrowed says this
 * call returned the process-lifetime getdelim scratch (newline-stop path:
 * one getdelim buffer is reused for every line of every file, so the
 * 100k-line readline loop does zero per-line malloc/free — the line is
 * always copied into a GC block before the next call can reclaim it). */
static char* cs_file_read_raw(FILE* f, int stop_nl, int has_size,
                              int64_t size, bool binary, size_t* out_len,
                              int* borrowed, int line) {
  size_t cap = 4096, len = 0, cps = 0;
  char* p;
  /* Newline-stop without a size bound (readline()/for-over-file): one
   * getdelim() stops exactly after the next '\n' — stdio drains its own
   * buffer in chunk-sized calls internally, so there is no per-byte loop,
   * no capacity check/code-point classification per byte, and NO read-ahead
   * to push back (getdelim consumes precisely through the delimiter).
   * Semantics are the old loop's: the returned line INCLUDES its '\n', a
   * final line without one is returned as-is, and EOF with nothing consumed
   * yields len 0 (caller reports EOF). POSIX 2008 (_POSIX_C_SOURCE). */
  if (stop_nl && !has_size) {
    static char* lb = NULL; /* reused across calls (single-threaded* runtime;
                             * see the borrowed contract above) */
    static size_t lcap = 0;
    ssize_t got;
    *borrowed = 1;
    got = getdelim(&lb, &lcap, '\n', f);
    if (got <= 0) {
      *out_len = 0;
      return NULL;
    }
    *out_len = (size_t)got;
    return lb;
  }
  *borrowed = 0;
  /* Chunked fread path — safe exactly when no byte ever has to be pushed
   * back into the stream: no newline stop (readline/for-over-file consume
   * byte-at-a-time below, so the first '\n' ends the read precisely at the
   * right stream position) and, for text, no code-point size bound
   * (read(size) may stop mid-stream — the next code point is ungetc'd).
   * binary+size bounds the read by the exact byte count; the whole-file
   * cases run to EOF — nothing is ever overread. The old per-byte getc loop
   * cost a call + stdio lock per byte (8.3MB file = 8.3M calls); one fread
   * per buffer refill is the whole read-side fix. */
  if (!stop_nl && (binary || !has_size)) {
    p = (char*)malloc(cap);
    if (p == NULL) cs_runtime_error(line, "out of memory");
    for (;;) {
      size_t want, got;
      int64_t remain = -1;
      if (has_size) {
        remain = size - (int64_t)len;
        if (remain <= 0) break;
      }
      if (len + 2u > cap) {
        char* np;
        cap *= 2u;
        np = (char*)realloc(p, cap);
        if (np == NULL) {
          free(p);
          cs_runtime_error(line, "out of memory");
        }
        p = np;
      }
      want = cap - len - 1u; /* one byte of slack for the NUL */
      if (remain >= 0 && (int64_t)want > remain) want = (size_t)remain;
      got = fread(p + len, 1u, want, f);
      len += got;
      if (got < want) break; /* EOF/error — fread waits out short reads */
    }
    p[len] = '\0';
    *out_len = len;
    return p;
  }
  p = (char*)malloc(cap);
  if (p == NULL) cs_runtime_error(line, "out of memory");
  for (;;) {
    int c;
    int is_cont;
    if (has_size && (int64_t)(binary ? len : cps) >= size) break;
    c = getc_unlocked(f); /* single-threaded reader: the lock is pure cost */
    if (c == EOF) break;
    is_cont = ((c & 0xC0) == 0x80);
    if (has_size && !binary && !is_cont && (int64_t)cps >= size) {
      ungetc(c, f); /* the next code point is not wanted */
      break;
    }
    if (len + 2u > cap) {
      char* np;
      cap *= 2u;
      np = (char*)realloc(p, cap);
      if (np == NULL) {
        free(p);
        cs_runtime_error(line, "out of memory");
      }
      p = np;
    }
    p[len++] = (char)c;
    if (!is_cont) cps++;
    if (stop_nl && c == '\n') break;
  }
  p[len] = '\0';
  *out_len = len;
  return p;
}

static cs_value cs_file_readline(cs_value filev, int has_size, int64_t size,
                                 int line) {
  cs_obj* o = filev.as.o;
  size_t len = 0;
  char* raw;
  int borrowed = 0;
  if (o->as.file.f == NULL)
    cs_runtime_error_as("ValueError", line, "I/O operation on closed file");
  raw = cs_file_read_raw(o->as.file.f, 1, has_size, size, o->as.file.binary,
                         &len, &borrowed, line);
  if (len == 0u) {
    if (!borrowed) free(raw);
    return cs_none(); /* EOF */
  }
  if (o->as.file.binary) {
    char* p = NULL;
    p = cs_gc_buf(CS_GC_BYTES, len, line);
    memcpy(p, raw, len);
    if (!borrowed) free(raw);
    return cs_bytes_of(p, len);
  }
  /* Text mode decodes (validating) exactly like decode() — a line with
   * broken UTF-8 fails with the same normative message. Fast path: when
   * the raw line validates (the overwhelmingly common case), build the
   * ONE result block directly — old path did raw malloc + BYTES block +
   * memcpy + free + a second TEXT block + a second memcpy for the same
   * bytes. Invalid lines keep the exact old chain so the ValueError text
   * and its delivery point are untouched. */
  if (cs_utf8_valid(raw, len)) {
    char* p = cs_gc_buf_filled(CS_GC_TEXT, len, line); /* full write below */
    memcpy(p, raw, len);
    p[len] = '\0';
    if (!borrowed) free(raw);
    return cs_str(p, len);
  }
  {
    char* p = cs_gc_buf(CS_GC_BYTES, len, line);
    cs_value as_bytes;
    memcpy(p, raw, len);
    if (!borrowed) free(raw);
    as_bytes = cs_bytes_of(p, len);
    return cs_builtin_decode(as_bytes, line);
  }
}

/* ---- format mini-language (SPEC §5.11.5) ------------------------------ */

/* Align already-formatted text inside `width` (align: -1 left, 0 center,
 * 1 right; fill defaults to ' '). */
static void cs_pad_align(cs_buf* out, const char* s, size_t n, char fill,
                         int align, size_t width, int line) {
  size_t pad = (width > n) ? width - n : 0u;
  size_t i;
  if (align > 0) { /* right */
    for (i = 0; i < pad; i++) cs_buf_addc(out, fill, line);
    cs_buf_addn(out, s, n, line);
  } else if (align < 0) { /* left */
    cs_buf_addn(out, s, n, line);
    for (i = 0; i < pad; i++) cs_buf_addc(out, fill, line);
  } else { /* center */
    size_t lpad = pad / 2u;
    for (i = 0; i < lpad; i++) cs_buf_addc(out, fill, line);
    cs_buf_addn(out, s, n, line);
    for (i = lpad; i < pad; i++) cs_buf_addc(out, fill, line);
  }
}

/* Insert ',' every 3 digits of the INTEGER part, counting from the RIGHT
 * (Python: format(314159, ",") is "314,159"). Fraction and exponent are
 * copied untouched. */
static void cs_add_grouping(cs_buf* out, const char* s, size_t n, int line) {
  size_t k, d = 0, from_right;
  for (k = 0; k < n && s[k] != '.' && s[k] != 'e' && s[k] != 'E'; k++)
    if (s[k] >= '0' && s[k] <= '9') d++;
  from_right = d;
  for (k = 0; k < n; k++) {
    bool is_digit = (s[k] >= '0' && s[k] <= '9');
    if (s[k] == '.' || s[k] == 'e' || s[k] == 'E') from_right = 0; /* stop */
    /* Comma before each digit whose remaining count (itself included) is a
     * multiple of three — measured from the RIGHT of the integer part.
     * `from_right < d` skips the very first digit (no leading comma even
     * when the digit count is a multiple of three). */
    if (is_digit && from_right > 0u && k > 0u && from_right < d &&
        (from_right % 3u) == 0u && s[k - 1] != '-' && s[k - 1] != '+')
      cs_buf_addc(out, ',', line);
    if (is_digit && from_right > 0u) from_right--;
    cs_buf_addc(out, s[k], line);
  }
}

/* The format(value, spec) engine — SPEC §5.11.5 documents the supported
 * subset of Python's mini-language: [[fill]align][sign][#][0][width][,]
 * [.precision][type] with types d i u b o x X c e E f F g G % s r. */
static cs_value cs_format_apply(cs_value v, const char* sp, size_t slen,
                                int line) {
  size_t i = 0;
  char fill = ' ';
  int align = -2; /* -2: unset (numbers default right, text left) */
  char sign = 0;
  bool alt = false, zero = false, grouped = false;
  size_t width = 0;
  bool have_prec = false;
  int64_t prec = 0;
  char typec = 0;
  char num[160];
  cs_buf out;
  const char* vtype = cs_type_name(v);

  /* [[fill]align] comes first in Python's grammar — before the sign and
   * flags, so a leading '0' followed by '=' is the FILL, not the zero
   * flag. align 2 is '=' (sign-aware zero padding, handled by the numeric
   * paths). */
  if (i < slen && (sp[i] == '<' || sp[i] == '>' || sp[i] == '^' ||
                   sp[i] == '=')) {
    align = (sp[i] == '<') ? -1 : (sp[i] == '>') ? 1 : (sp[i] == '^') ? 0 : 2;
    i++;
  } else if (i + 1u < slen && (sp[i + 1] == '<' || sp[i + 1] == '>' ||
                               sp[i + 1] == '^' || sp[i + 1] == '=')) {
    fill = sp[i];
    align = (sp[i + 1] == '<') ? -1 : (sp[i + 1] == '>') ? 1
                                 : (sp[i + 1] == '^') ? 0 : 2;
    i += 2;
  }
  /* [[sign][#][0]] — the flags. The ',' grouping option is NOT a flag: it
   * comes after the width (SPEC §5.11.3 grammar), accepted below. */
  while (i < slen) {
    char c = sp[i];
    if (c == '+' || c == '-' || c == ' ') { sign = c; i++; continue; }
    if (c == '#') { alt = true; i++; continue; }
    if (c == '0') { zero = true; i++; continue; }
    break;
  }
  while (i < slen && sp[i] >= '0' && sp[i] <= '9') {
    width = width * 10u + (size_t)(sp[i] - '0');
    i++;
  }
  if (i < slen && sp[i] == ',') { grouped = true; i++; } /* [width][,] */
  if (i < slen && sp[i] == '.') {
    i++;
    if (i < slen && sp[i] >= '0' && sp[i] <= '9') {
      have_prec = true;
      while (i < slen && sp[i] >= '0' && sp[i] <= '9') {
        prec = prec * 10 + (sp[i] - '0');
        i++;
      }
    } else {
      cs_runtime_error_as("ValueError", line,
                          "Format specifier missing precision");
    }
  }
  if (i < slen) {
    typec = sp[i];
    i++;
    if (i != slen)
      cs_runtime_error_as("ValueError", line, "Invalid format specifier");
  }
  if (typec == 0 && v.tag == CS_INT) {
    /* Typeless int renders as decimal (Python: format(42, ">7") is
     * "     42", never the float display) and, like Python, rejects a
     * precision that only codes like 'f' would take. */
    if (have_prec)
      cs_runtime_error_as("ValueError", line,
                          "Precision not allowed in integer format specifier");
    typec = 'd';
  }

  out.p = NULL;
  out.len = 0;
  out.cap = 0;

  if (typec == 's' || typec == 'r' ||
      (typec == 0 && !cs_is_numeric(v))) {
    /* Text-ish: str() for 's'/typeless, repr() for 'r'; precision
     * truncates (Python does the same for strings). */
    cs_value t = (typec == 'r') ? cs_repr_value(v, false, line)
                                : cs_display_value(v, line);
    const char* p = t.as.s.p;
    size_t n = t.as.s.len;
    if (have_prec && (int64_t)n > prec) n = (size_t)prec;
    cs_pad_align(&out, p, n, fill, (align == -2) ? -1 : align, width, line);
    return cs_text_from_buf(&out, line);
  }

  if (typec == 'c') {
    char ch[8];
    size_t cn;
    if (v.tag != CS_INT)
      cs_runtime_error_as("ValueError", line,
                          "Unknown format code 'c' for object of type '%s'",
                          vtype);
    if (v.as.i < 0 || v.as.i > 0x10FFFF)
      cs_runtime_error_as("ValueError", line,
                          "'%lld' is not a valid Unicode code point",
                          (long long)v.as.i);
    cn = cs_utf8_put(ch, (uint32_t)v.as.i);
    cs_pad_align(&out, ch, cn, fill, (align == -2) ? 1 : align, width, line);
    return cs_text_from_buf(&out, line);
  }

  if (typec == 'e' || typec == 'E' || typec == 'f' || typec == 'F' ||
      typec == 'g' || typec == 'G' || typec == '%' ||
      (typec == 0 && cs_is_numeric(v))) {
    double x;
    int nn;
    if (!cs_is_numeric(v))
      cs_runtime_error_as("ValueError", line,
                          "Unknown format code '%c' for object of type '%s'",
                          typec ? typec : 'd', vtype);
    x = cs_as_double(v);
    if (grouped && typec != 'f' && typec != 'F' && typec != 0)
      cs_runtime_error_as("ValueError", line,
                          "Format specifier ',' requires type 'd' or 'f'");
    if (!have_prec) prec = 6;
    switch (typec) {
      case 'e': nn = snprintf(num, sizeof(num), "%.*e", (int)prec, x); break;
      case 'E':
        nn = snprintf(num, sizeof(num), "%.*E", (int)prec, x);
        break;
      case 'f':
      case 0: {
        if (typec == 0 && !have_prec) {
          /* Typeless float: the display form (shortest round-trip). */
          cs_fmt_float(x, num, sizeof(num));
          nn = (int)strlen(num);
        } else if (alt) {
          nn = snprintf(num, sizeof(num), "%#.*f", (int)prec, x);
        } else {
          nn = snprintf(num, sizeof(num), "%.*f", (int)prec, x);
        }
        break;
      }
      case 'F':
        nn = alt ? snprintf(num, sizeof(num), "%#.*F", (int)prec, x)
                 : snprintf(num, sizeof(num), "%.*F", (int)prec, x);
        break;
      case 'g':
        nn = snprintf(num, sizeof(num), "%.*g", (int)prec, x);
        break;
      case 'G':
        nn = snprintf(num, sizeof(num), "%.*G", (int)prec, x);
        break;
      default: /* '%' scales by 100 */
        nn = snprintf(num, sizeof(num), "%.*f%%", (int)prec, x * 100.0);
        break;
    }
    if (nn < 0 || (size_t)nn >= sizeof(num))
      cs_runtime_error(line, "out of memory");
    {
      cs_buf numbuf;
      numbuf.p = NULL;
      numbuf.len = 0;
      numbuf.cap = 0;
      if (grouped) cs_add_grouping(&numbuf, num, (size_t)nn, line);
      else cs_buf_addn(&numbuf, num, (size_t)nn, line);
      /* sign — before the digits (Python: format(3.5, "+.1f") == "+3.5") */
      if ((sign == '+' || sign == ' ') && numbuf.len > 0 &&
          numbuf.p[0] != '-' && numbuf.p[0] != '+') {
        cs_buf s2;
        s2.p = NULL;
        s2.len = 0;
        s2.cap = 0;
        cs_buf_addc(&s2, sign, line);
        cs_buf_addn(&s2, numbuf.p, numbuf.len, line);
        free(numbuf.p);
        numbuf = s2;
      }
      /* Sign-aware padding ('0' flag or '=' align) — only when alignment
       * is unset/right/'='; explicit left/center goes through cs_pad_align
       * with the fill. '=' pads with the fill char, '0' with '0'. */
      if ((zero || align == 2) && (align == -2 || align == 1 || align == 2) &&
          width > numbuf.len) {
        char zc = (align == 2) ? fill : '0';
        cs_buf z;
        z.p = NULL;
        z.len = 0;
        z.cap = 0;
        if (numbuf.len > 0 && (numbuf.p[0] == '+' || numbuf.p[0] == '-' ||
                               numbuf.p[0] == ' ')) {
          cs_buf_addc(&z, numbuf.p[0], line);
          cs_buf_addn(&z, numbuf.p + 1, numbuf.len - 1u, line);
        } else {
          cs_buf_addn(&z, numbuf.p, numbuf.len, line);
        }
        {
          cs_buf fin;
          size_t pad = width - z.len, k;
          fin.p = NULL;
          fin.len = 0;
          fin.cap = 0;
          if (z.len > 0 && z.p[0] == '+' && sign == '+') {
            cs_buf_addc(&fin, '+', line);
            for (k = 0; k < pad; k++) cs_buf_addc(&fin, zc, line);
            cs_buf_addn(&fin, z.p + 1, z.len - 1u, line);
          } else {
            for (k = 0; k < pad; k++) cs_buf_addc(&fin, zc, line);
            cs_buf_addn(&fin, z.p, z.len, line);
          }
          free(z.p);
          free(numbuf.p);
          return cs_text_from_buf(&fin, line);
        }
      }
      cs_pad_align(&out, numbuf.p, numbuf.len, fill,
                   (align == -2) ? 1 : align, width, line);
      free(numbuf.p);
      return cs_text_from_buf(&out, line);
    }
  }

  /* Integer types: d i u b o x X (typeless int behaves like 'd'). */
  {
    uint64_t mag;
    bool neg;
    const char* prefix = "";
    size_t nn = 0;
    int base = 10;
    bool is_int_type = (typec == 'd' || typec == 'i' || typec == 'u' ||
                        typec == 'b' || typec == 'o' || typec == 'x' ||
                        typec == 'X' || typec == 0);
    if (!is_int_type)
      cs_runtime_error_as("ValueError", line,
                          "Unknown format code '%c' for object of type '%s'",
                          typec, vtype);
    if (v.tag != CS_INT)
      cs_runtime_error_as("ValueError", line,
                          "Unknown format code '%c' for object of type '%s'",
                          typec ? typec : 'd', vtype);
    if (grouped && typec != 'd' && typec != 'i' && typec != 'u')
      cs_runtime_error_as("ValueError", line,
                          "Format specifier ',' requires type 'd' or 'f'");
    neg = v.as.i < 0;
    mag = neg ? (uint64_t)(0u - (uint64_t)v.as.i) : (uint64_t)v.as.i;
    switch (typec) {
      case 'b': base = 2; prefix = alt ? "0b" : ""; break;
      case 'o': base = 8; prefix = alt ? "0o" : ""; break;
      case 'x': base = 16; prefix = alt ? "0x" : ""; break;
      case 'X': base = 16; prefix = alt ? "0X" : ""; break;
      default: base = 10; break;
    }
    {
      static const char* kDig = "0123456789abcdef";
      char dbuf[72];
      size_t dn = 0;
      do {
        unsigned d = (unsigned)(mag % (uint64_t)base);
        dbuf[dn++] = (typec == 'X') ? (char)((d < 10) ? ('0' + d)
                                                      : ('A' + d - 10))
                                    : kDig[d];
        mag /= (uint64_t)base;
      } while (mag != 0u);
      /* sign + prefix + digits (+ grouping) into num[] */
      if (neg) num[nn++] = '-';
      else if (sign == '+') num[nn++] = '+';
      else if (sign == ' ') num[nn++] = ' ';
      {
        size_t pl = strlen(prefix), k;
        for (k = 0; k < pl && nn + 1u < sizeof(num); k++) num[nn++] = prefix[k];
      }
      {
        size_t k;
        for (k = dn; k > 0 && nn + 1u < sizeof(num); k--) num[nn++] = dbuf[k - 1];
      }
      num[nn] = '\0';
    }
    {
      cs_buf numbuf;
      numbuf.p = NULL;
      numbuf.len = 0;
      numbuf.cap = 0;
      if (grouped) cs_add_grouping(&numbuf, num, nn, line);
      else cs_buf_addn(&numbuf, num, nn, line);
      if ((zero || align == 2) && (align == -2 || align == 1 || align == 2) &&
          width > numbuf.len) {
        /* '='/'0' placement: sign/prefix stay before the pad. */
        char zc = (align == 2) ? fill : '0';
        size_t off = 0;
        if (numbuf.len > 0 && (numbuf.p[0] == '+' || numbuf.p[0] == '-' ||
                               numbuf.p[0] == ' '))
          off = 1;
        if (numbuf.len >= off + 2u && numbuf.p[off] == '0' &&
            (numbuf.p[off + 1] == 'x' || numbuf.p[off + 1] == 'X' ||
             numbuf.p[off + 1] == 'b' || numbuf.p[off + 1] == 'o'))
          off += 2;
        {
          cs_buf fin;
          size_t pad = width - numbuf.len, k;
          fin.p = NULL;
          fin.len = 0;
          fin.cap = 0;
          cs_buf_addn(&fin, numbuf.p, off, line);
          for (k = 0; k < pad; k++) cs_buf_addc(&fin, zc, line);
          cs_buf_addn(&fin, numbuf.p + off, numbuf.len - off, line);
          free(numbuf.p);
          return cs_text_from_buf(&fin, line);
        }
      }
      cs_pad_align(&out, numbuf.p, numbuf.len, fill,
                   (align == -2) ? 1 : align, width, line);
      free(numbuf.p);
      return cs_text_from_buf(&out, line);
    }
  }
}

cs_value cs_builtin_format(cs_value v, cs_value spec, int line) {
  cs_no_missing(v, line);
  if (spec.tag == CS_MISSING)
    return cs_display_value(v, line); /* format(v) == str(v) */
  if (spec.tag == CS_NONE) /* format(v, None) == str(v) (Python) */
    return cs_display_value(v, line);
  if (spec.tag != CS_STRING)
    cs_runtime_error_as("TypeError", line,
                        "format() argument 2 must be str, not '%s'",
                        cs_type_name(spec));
  return cs_format_apply(v, spec.as.s.p, spec.as.s.len, line);
}

/* ---- attribute reflection (SPEC §5.11.5) ------------------------------ */

/* NUL-terminated view of a text value for the C-string APIs (cs_getattr
 * takes `const char*`). Texts built by the runtime are always
 * NUL-terminated (cs_text_from_buf / cs_concat / literals), so the common
 * case allocates nothing; only an interior slice needs a scratch copy. */
typedef struct cs_cstr {
  char* owned;   /* free() after use, or NULL when not needed */
  const char* p; /* the NUL-terminated bytes */
} cs_cstr;

static cs_cstr cs_text_cstr(cs_value t, int line) {
  cs_cstr r;
  r.owned = NULL;
  if (t.as.s.len == 0u) {
    r.p = "";
    return r;
  }
  r.p = t.as.s.p;
  if (t.as.s.p[t.as.s.len] != '\0') { /* interior slice of a longer text */
    r.owned = (char*)malloc(t.as.s.len + 1u);
    if (r.owned == NULL) cs_runtime_error(line, "out of memory");
    memcpy(r.owned, t.as.s.p, t.as.s.len);
    r.owned[t.as.s.len] = '\0';
    r.p = r.owned;
  }
  return r;
}

/* Existence probe mirroring cs_getattr exactly (SPEC §5.11.5): hasattr is
 * true iff a bare attribute read would not raise, and getattr's default
 * path never guesses. Reads the same tables cs_getattr reads, without the
 * exception machinery. */
static bool cs_attr_probe(cs_value obj, const char* name, int line) {
  if (obj.tag == CS_INSTANCE) {
    cs_value key = cs_text_lit(name);
    cs_obj* io = obj.as.o;
    cs_table* attrs = &io->as.inst.attrs;
    cs_table* meths = &io->as.inst.cls.as.o->as.cls.methods;
    uint64_t h;
    uint32_t k;
    for (k = 0; k < io->as.inst.n_inl; k++) { /* inline fields first */
      const cs_inl* e = &CS_INST_INL(io)[k];
      if (e->key.tag != CS_UNSET && cs_value_eq(e->key, key)) return true;
    }
    h = cs_hash_value(key, line);
    if (attrs->slots != NULL && cs_table_find(attrs, key, h) != (size_t)-1)
      return true;
    if (cs_table_find(meths, key, h) != (size_t)-1) return true;
    return false;
  }
  if (obj.tag == CS_EXCEPTION)
    return strcmp(name, "type") == 0 || strcmp(name, "message") == 0;
  if (obj.tag == CS_FILE)
    return strcmp(name, "closed") == 0 || strcmp(name, "name") == 0;
  return false;
}

cs_value cs_builtin_getattr(cs_value obj, cs_value name, cs_value dflt,
                            int line) {
  cs_cstr c;
  cs_no_missing(obj, line);
  if (name.tag != CS_STRING)
    cs_runtime_error_as("TypeError", line,
                        "getattr(): attribute name must be str, not '%s'",
                        cs_type_name(name));
  c = cs_text_cstr(name, line);
  if (cs_attr_probe(obj, c.p, line)) {
    cs_value v = cs_getattr(obj, c.p, line);
    free(c.owned);
    return v;
  }
  if (dflt.tag != CS_MISSING) { /* Python: return the default on a miss */
    free(c.owned);
    return dflt;
  }
  cs_getattr(obj, c.p, line); /* raises §3.2's exact AttributeError */
  free(c.owned);
  return cs_none(); /* not reached */
}

cs_value cs_builtin_hasattr(cs_value obj, cs_value name, int line) {
  cs_cstr c;
  cs_value r;
  cs_no_missing(obj, line);
  if (name.tag != CS_STRING)
    cs_runtime_error_as("TypeError", line,
                        "hasattr(): attribute name must be str, not '%s'",
                        cs_type_name(name));
  c = cs_text_cstr(name, line);
  r = cs_bool(cs_attr_probe(obj, c.p, line));
  free(c.owned);
  return r;
}

cs_value cs_builtin_setattr(cs_value obj, cs_value name, cs_value value,
                            int line) {
  cs_cstr c;
  cs_no_missing(obj, line);
  if (name.tag != CS_STRING)
    cs_runtime_error_as("TypeError", line,
                        "setattr(): attribute name must be str, not '%s'",
                        cs_type_name(name));
  c = cs_text_cstr(name, line);
  cs_setattr(obj, c.p, value, line);
  free(c.owned);
  return cs_none();
}

void cs_delattr(cs_value obj, const char* name, int line) {
  cs_value key;
  cs_table* t;
  uint64_t h;
  size_t i;
  cs_no_missing(obj, line);
  if (obj.tag != CS_INSTANCE)
    cs_runtime_error_as("AttributeError", line,
                        "'%s' object has no attribute '%s'",
                        cs_attr_owner(obj), name);
  t = &obj.as.o->as.inst.attrs;
  key = cs_text_lit(name);
  {
    /* Inline fields first (see cs_getattr); a delete only tombstones
     * (and drops both references — mark skips CS_UNSET keys, so the
     * tombstone pins nothing). */
    cs_obj* io = obj.as.o;
    uint32_t k;
    for (k = 0; k < io->as.inst.n_inl; k++) {
      cs_inl* e = &CS_INST_INL(io)[k];
      if (e->key.tag != CS_UNSET && cs_value_eq(e->key, key)) {
        e->key = cs_unset();
        e->val = cs_unset();
        return;
      }
    }
  }
  h = cs_hash_value(key, line);
  if (t->slots != NULL) {
    i = cs_table_find(t, key, h);
    if (i != (size_t)-1) {
      cs_table_delete(t, i);
      return;
    }
  }
  {
    cs_table* meths = &obj.as.o->as.inst.cls.as.o->as.cls.methods;
    if (cs_table_find(meths, key, h) != (size_t)-1)
      cs_runtime_error_as("AttributeError", line,
                          "'%s' attribute '%s' belongs to its class and "
                          "cannot be deleted",
                          cs_attr_owner(obj), name);
    cs_runtime_error_as("AttributeError", line,
                        "'%s' object has no attribute '%s'",
                        cs_attr_owner(obj), name);
  }
}

cs_value cs_builtin_delattr(cs_value obj, cs_value name, int line) {
  cs_cstr c;
  cs_no_missing(obj, line);
  if (name.tag != CS_STRING)
    cs_runtime_error_as("TypeError", line,
                        "delattr(): attribute name must be str, not '%s'",
                        cs_type_name(name));
  c = cs_text_cstr(name, line);
  cs_delattr(obj, c.p, line);
  free(c.owned);
  return cs_none();
}

/* ---- identity and type tests ------------------------------------------ */

cs_value cs_builtin_callable(cs_value obj, int line) {
  cs_no_missing(obj, line);
  return cs_bool(cs_is_callable(obj)); /* instances have no __call__ */
}

static bool cs_isinstance1(cs_value obj, cs_value ci, int line) {
  size_t i;
  if (ci.tag == CS_STRING) {
    /* type-name form: the semantic layer rewrites bare builtin type names
     * in classinfo position to exactly these (SPEC §5.11.5). */
    const char* tn = cs_type_name(obj);
    return strlen(tn) == ci.as.s.len &&
           memcmp(tn, ci.as.s.p, ci.as.s.len) == 0;
  }
  if (ci.tag == CS_CLASS) {
    if (obj.tag == CS_INSTANCE)
      return cs_identical(obj.as.o->as.inst.cls, ci, line);
    if (obj.tag == CS_EXCEPTION)
      return cs_identical(obj.as.o->as.exc.cls, ci, line);
    return false;
  }
  if (ci.tag == CS_TUPLE) {
    for (i = 0; i < ci.as.o->as.seq.len; i++)
      if (cs_isinstance1(obj, cs_seq_at(ci.as.o, i), line)) return true;
    return false;
  }
  cs_runtime_error_as("TypeError", line,
                      "isinstance() arg 2 must be a type or tuple of types, "
                      "not '%s'",
                      cs_type_name(ci));
  return false; /* not reached */
}

cs_value cs_builtin_isinstance(cs_value obj, cs_value classinfo, int line) {
  cs_no_missing2(obj, classinfo, line);
  return cs_bool(cs_isinstance1(obj, classinfo, line));
}

cs_value cs_builtin_id(cs_value obj, int line) {
  cs_no_missing(obj, line);
  switch (obj.tag) {
    case CS_LIST:
    case CS_MAP:
    case CS_SET:
    case CS_TUPLE:
    case CS_FUNCTION:
    case CS_CLASS:
    case CS_EXCEPTION:
    case CS_INSTANCE:
    case CS_BOUND_METHOD:
    case CS_ITERATOR:
    case CS_FILE:
      /* heap objects: the pointer with bit 63 set (SPEC §5.11.5) — no two
       * live heap objects share one. */
      return cs_int((int64_t)((uint64_t)(uintptr_t)obj.as.o | (1ULL << 63)));
    default: {
      /* immediates: a value-derived fold in the disjoint bit-62 space
       * (bit 63 clear), mixed by tag so True / 1 / 1.0 never collide. */
      uint64_t payload = 0;
      if (obj.tag == CS_INT) payload = (uint64_t)obj.as.i;
      else if (obj.tag == CS_FLOAT) memcpy(&payload, &obj.as.d, sizeof payload);
      else if (obj.tag == CS_BOOL) payload = obj.as.b ? 1u : 0u;
      else if (obj.tag == CS_STRING || obj.tag == CS_BYTES)
        payload = (uint64_t)(uintptr_t)obj.as.s.p ^ ((uint64_t)obj.as.s.len << 17);
      else if (obj.tag == CS_RANGE)
        payload = (uint64_t)obj.as.r.start ^ ((uint64_t)obj.as.r.stop << 21) ^
                  (uint64_t)obj.as.r.step;
      payload ^= (uint64_t)obj.tag * 0x9E3779B97F4A7C15ULL;
      payload ^= payload >> 29;
      payload *= 0xBF58476D1CE4E5B9ULL;
      payload ^= payload >> 32;
      return cs_int((int64_t)((1ULL << 62) | (payload & ((1ULL << 62) - 1))));
    }
  }
}

cs_value cs_builtin_hash(cs_value obj, int line) {
  cs_no_missing(obj, line);
  return cs_int((int64_t)cs_hash_value(obj, line)); /* unhashable → TypeError */
}

/* ---- globals / locals / vars / dir ------------------------------------ *
 * Codegen passes the compile-time name table of the enclosing function
 * (§5.11.5): one entry per live name, pointing at the storage the name
 * currently reads. Boxed names whose slot is still CS_UNSET and native
 * names whose guard flag is 0 are absent — exactly like a name that has
 * not been assigned yet. */

static cs_value cs_name_table_map(const cs_name_slot* tbl, size_t n,
                                  int line) {
  cs_value m = cs_map_new();
  size_t i;
  for (i = 0; i < n; i++) {
    cs_value v;
    if (tbl[i].kind == 0) {
      v = *(cs_value*)tbl[i].ptr;
      if (v.tag == CS_UNSET) continue; /* not yet assigned */
    } else if (tbl[i].kind == 2) { /* top-level function meta */
      v = cs_fun_value((const cs_funmeta*)tbl[i].ptr, NULL, 0u);
    } else if (tbl[i].kind == 3) { /* native float-lane slot (CS_FLOAT) */
      if (tbl[i].flag != NULL && *tbl[i].flag == 0) continue;
      v = cs_float(*(double*)tbl[i].ptr);
    } else {
      if (tbl[i].flag != NULL && *tbl[i].flag == 0) continue;
      v = cs_int(*(int64_t*)tbl[i].ptr);
    }
    cs_map_set(m, cs_text_lit(tbl[i].name), v, line);
  }
  return m;
}

static cs_value cs_name_table_names(const cs_name_slot* tbl, size_t n,
                                    int line) {
  size_t i, k, cnt = 0;
  const char** names;
  cs_value out;
  names = (n > 0u) ? (const char**)malloc(n * sizeof(const char*)) : NULL;
  if (n > 0u && names == NULL) cs_runtime_error(line, "out of memory");
  for (i = 0; i < n; i++) {
    if (tbl[i].kind == 0) {
      if (((cs_value*)tbl[i].ptr)->tag == CS_UNSET) continue;
    } else if ((tbl[i].kind == 1 || tbl[i].kind == 3) &&
               tbl[i].flag != NULL && *tbl[i].flag == 0)
      continue; /* native slot (int64/float lane) not yet assigned */
    names[cnt++] = tbl[i].name;
  }
  /* insertion sort: n is the name count of one function (small), and it
   * keeps dir()'s output sorted like Python's without a sort call. */
  for (i = 1; i < cnt; i++) {
    const char* key = names[i];
    k = i;
    while (k > 0u && strcmp(names[k - 1u], key) > 0) {
      names[k] = names[k - 1u];
      k--;
    }
    names[k] = key;
  }
  out = cs_list_new();
  for (i = 0; i < cnt; i++) cs_list_push(out, cs_text_lit(names[i]), line);
  free(names);
  return out;
}

cs_value cs_builtin_globals(const cs_name_slot* tbl, size_t n, int line) {
  return cs_name_table_map(tbl, n, line);
}

cs_value cs_builtin_locals(const cs_name_slot* tbl, size_t n, int line) {
  return cs_name_table_map(tbl, n, line);
}

cs_value cs_builtin_vars(cs_value obj, const cs_name_slot* tbl, size_t n,
                         int line) {
  if (obj.tag == CS_MISSING) return cs_name_table_map(tbl, n, line);
  cs_no_missing(obj, line);
  if (obj.tag == CS_INSTANCE) { /* copy of the dynamic fields */
    cs_obj* io = obj.as.o;
    cs_table* t = &io->as.inst.attrs;
    cs_value m = cs_map_new();
    size_t i;
    uint32_t k;
    for (k = 0; k < io->as.inst.n_inl; k++) /* inline fields, insertion
                                             * order first */
      if (CS_INST_INL(io)[k].key.tag != CS_UNSET)
        cs_map_set(m, CS_INST_INL(io)[k].key, CS_INST_INL(io)[k].val, line);
    for (i = 0; i < t->len; i++)
      if (t->e[i].used) cs_map_set(m, t->e[i].key, t->vals[i], line);
    return m;
  }
  cs_runtime_error_as("TypeError", line, "'%s' object has no attributes",
                      cs_type_name(obj));
  return cs_none(); /* not reached */
}

/* Union of an instance's field names and class method names, sorted. */
static cs_value cs_dir_instance(cs_value obj, int line) {
  cs_obj* io = obj.as.o;
  cs_table* attrs = &io->as.inst.attrs;
  cs_table* meths = &io->as.inst.cls.as.o->as.cls.methods;
  size_t na = 0, nm = 0, i, k, cnt;
  uint32_t u;
  const char** names;
  cs_value out;
  for (u = 0; u < io->as.inst.n_inl; u++) /* inline fields */
    if (CS_INST_INL(io)[u].key.tag != CS_UNSET) na++;
  for (i = 0; i < attrs->len; i++)
    if (attrs->e[i].used) na++;
  for (i = 0; i < meths->len; i++)
    if (meths->e[i].used) nm++;
  cnt = na + nm;
  names = (cnt > 0u) ? (const char**)malloc(cnt * sizeof(const char*)) : NULL;
  if (cnt > 0u && names == NULL) cs_runtime_error(line, "out of memory");
  k = 0;
  for (u = 0; u < io->as.inst.n_inl; u++)
    if (CS_INST_INL(io)[u].key.tag != CS_UNSET)
      names[k++] = CS_INST_INL(io)[u].key.as.s.p;
  for (i = 0; i < attrs->len; i++)
    if (attrs->e[i].used) names[k++] = attrs->e[i].key.as.s.p;
  for (i = 0; i < meths->len; i++)
    if (meths->e[i].used) names[k++] = meths->e[i].key.as.s.p;
  for (i = 1; i < cnt; i++) { /* insertion sort (names are unique) */
    const char* key = names[i];
    k = i;
    while (k > 0u && strcmp(names[k - 1u], key) > 0) {
      names[k] = names[k - 1u];
      k--;
    }
    names[k] = key;
  }
  out = cs_list_new();
  for (i = 0; i < cnt; i++) cs_list_push(out, cs_text_lit(names[i]), line);
  free(names);
  return out;
}

cs_value cs_builtin_dir(cs_value obj, const cs_name_slot* tbl, size_t n,
                        int line) {
  if (obj.tag == CS_MISSING) return cs_name_table_names(tbl, n, line);
  cs_no_missing(obj, line);
  if (obj.tag == CS_INSTANCE) return cs_dir_instance(obj, line);
  cs_runtime_error_as("TypeError", line, "'%s' object has no attributes",
                      cs_type_name(obj));
  return cs_none(); /* not reached */
}

/* ---- iteration (SPEC §5.11.5) ----------------------------------------- */

cs_value cs_builtin_iter(cs_value obj, int line) {
  cs_iter it;
  cs_obj* o;
  cs_no_missing(obj, line);
  if (obj.tag == CS_ITERATOR) return obj; /* Python: iter(it) is it */
  it = cs_iter_open(obj, line); /* not iterable → TypeError */
  /* Pin the source: text/bytes iteration keeps an interior cursor with no
   * reference of its own, which would dangle once the source collects. */
  it.src = obj;
  o = cs_obj_new(CS_ITERATOR, line);
  o->as.itr.it = (cs_iter*)malloc(sizeof(cs_iter));
  if (o->as.itr.it == NULL) cs_runtime_error(line, "out of memory");
  *o->as.itr.it = it;
  return cs_obj_value(o, CS_ITERATOR);
}

cs_value cs_builtin_next(cs_value obj, cs_value dflt, int line) {
  cs_value out;
  cs_no_missing(obj, line);
  if (obj.tag == CS_FILE) { /* Python: files are iterators — one line per
                             * next(); the FILE* cursor lives in the object,
                             * so a transient iter state suffices. */
    cs_iter it;
    it.kind = CS_ITER_FILE;
    it.cur = 0;
    it.stop = 0;
    it.step = 0;
    it.p = NULL;
    it.len = 0u;
    it.pos = 0u;
    it.idx = 0u;
    it.src = obj;
    if (cs_iter_next(&it, &out)) return out;
    if (dflt.tag != CS_MISSING) return dflt;
    cs_runtime_error_as("StopIteration", line, "");
  }
  if (obj.tag != CS_ITERATOR)
    cs_runtime_error_as("TypeError", line, "'%s' object is not an iterator",
                        cs_type_name(obj));
  if (obj.as.o->as.itr.it != NULL && cs_iter_next(obj.as.o->as.itr.it, &out))
    return out;
  if (dflt.tag != CS_MISSING) return dflt; /* Python: the default at EOF */
  cs_runtime_error_as("StopIteration", line, "");
  return cs_none(); /* not reached */
}

/* ---- files (SPEC §5.11.5) --------------------------------------------- */

cs_value cs_builtin_open(cs_value file, cs_value mode, cs_value encoding,
                         int line) {
  cs_cstr pc;
  cs_obj* o;
  FILE* f;
  char m0;
  bool binary, writable;
  char fmode[4];
  cs_no_missing(file, line);
  if (file.tag != CS_STRING && file.tag != CS_BYTES)
    cs_runtime_error_as("TypeError", line,
                        "open(): argument 1 must be str or bytes, not '%s'",
                        cs_type_name(file));
  if (mode.tag == CS_MISSING) mode = CS_STR("r");
  if (mode.tag != CS_STRING)
    cs_runtime_error_as("TypeError", line,
                        "open(): 'mode' must be str, not '%s'",
                        cs_type_name(mode));
  if (mode.as.s.len < 1u || mode.as.s.len > 2u ||
      (mode.as.s.p[0] != 'r' && mode.as.s.p[0] != 'w' && mode.as.s.p[0] != 'a'))
    cs_runtime_error_as("ValueError", line, "open(): unsupported mode '%.*s'",
                        (int)mode.as.s.len, mode.as.s.p);
  m0 = mode.as.s.p[0];
  writable = (m0 != 'r');
  binary = false;
  if (mode.as.s.len == 2u) {
    char m1 = mode.as.s.p[1];
    if (m1 == 'b') binary = true;
    else if (m1 != 't')
      cs_runtime_error_as("ValueError", line, "open(): unsupported mode '%.*s'",
                          (int)mode.as.s.len, mode.as.s.p);
  }
  if (encoding.tag != CS_MISSING && encoding.tag != CS_NONE) {
    if (encoding.tag != CS_STRING)
      cs_runtime_error_as("TypeError", line,
                          "open(): 'encoding' must be str, not '%s'",
                          cs_type_name(encoding));
    if (binary)
      cs_runtime_error_as("ValueError", line,
                          "binary mode doesn't take an encoding argument");
    if (!(encoding.as.s.len == 5u &&
          memcmp(encoding.as.s.p, "utf-8", 5u) == 0) &&
        !(encoding.as.s.len == 4u && memcmp(encoding.as.s.p, "utf8", 4u) == 0))
      cs_runtime_error_as("ValueError", line,
                          "cstar supports only the UTF-8 encoding");
  }
  pc = cs_text_cstr(file, line);
  fmode[0] = m0;
  fmode[1] = 'b'; /* binary stdio: no CRLF translation on any mode */
  fmode[2] = '\0';
  f = fopen(pc.p, fmode);
  if (f == NULL) {
    int e = errno;
    if (e == ENOENT && m0 == 'r') {
      cs_runtime_error_as("FileNotFoundError", line, "[Errno %d] %s: '%s'", e,
                          strerror(e), pc.p);
    }
    cs_runtime_error_as("OSError", line, "[Errno %d] %s: '%s'", e,
                        strerror(e), pc.p);
  }
  o = cs_obj_new(CS_FILE, line);
  o->as.file.f = f;
  o->as.file.binary = binary;
  o->as.file.writable = writable;
  o->as.file.path = (char*)malloc(pc.p != NULL ? strlen(pc.p) + 1u : 1u);
  if (o->as.file.path == NULL) cs_runtime_error(line, "out of memory");
  memcpy(o->as.file.path, pc.p, strlen(pc.p) + 1u);
  o->as.file.next_open = cs_open_files;
  cs_open_files = o;
  if (!cs_files_atexit_installed) { /* first open pays the atexit hook */
    atexit(cs_file_exit_close);
    cs_files_atexit_installed = 1;
  }
  free(pc.owned);
  return cs_obj_value(o, CS_FILE);
}

static void cs_file_expect_open(cs_obj* o, int line) {
  if (o->as.file.f == NULL)
    cs_runtime_error_as("ValueError", line, "I/O operation on closed file");
}

static void cs_file_expect_writable(cs_obj* o, int line) {
  cs_file_expect_open(o, line);
  if (!o->as.file.writable)
    cs_runtime_error_as("OSError", line, "file not open for writing");
}

/* Remaining bytes of a seekable regular file; -1 when not determinable
 * (pipe/socket/device, or a failed ftell). Drives the whole-file read
 * fast path: one fread straight into the registered block. */
static long long cs_file_remaining(FILE* f) {
  struct stat st;
  long pos;
  if (fstat(fileno(f), &st) != 0) return -1;
  if (!S_ISREG(st.st_mode)) return -1;
  pos = ftell(f);
  if (pos < 0) return -1;
  return (long long)st.st_size - (long long)pos;
}

/* read(): raw bytes → bytes value, or (text mode) a validating UTF-8
 * decode exactly like decode() — the normative broken-UTF-8 message. */
static cs_value cs_file_read(cs_obj* o, int has_size, int64_t size, int line) {
  size_t len = 0;
  char* raw;
  int borrowed = 0; /* whole-file reads never borrow (stop_nl == 0) */
  cs_file_expect_open(o, line);
  /* Whole-file read of a seekable file: size it up front and fill ONE
   * registered block with ONE fread — no growing raw buffer, no second
   * copy, no zeroing pass (the payload is fully written before any read).
   * Text mode then runs the unchanged validating decode over the block. */
  if (!has_size) {
    long long rem = cs_file_remaining(o->as.file.f);
    if (rem >= 0) {
      char* p;
      if (rem == 0)
        return o->as.file.binary ? cs_bytes_of(NULL, 0u) : CS_STR("");
      p = cs_gc_buf_filled(CS_GC_BYTES, (size_t)rem, line);
      len = fread(p, 1u, (size_t)rem, o->as.file.f);
      if (len == 0u)
        return o->as.file.binary ? cs_bytes_of(NULL, 0u)
                                 : CS_STR(""); /* raced truncate: the unfilled
                                   block has no references and is swept */
      p[len] = '\0';
      if (o->as.file.binary) return cs_bytes_of(p, len);
      return cs_builtin_decode(cs_bytes_of(p, len), line);
    }
  }
  raw = cs_file_read_raw(o->as.file.f, 0, has_size, size, o->as.file.binary,
                         &len, &borrowed, line);
  if (len == 0u) {
    free(raw);
    return o->as.file.binary ? cs_bytes_of(NULL, 0u) : CS_STR("");
  }
  if (o->as.file.binary) {
    char* p = cs_gc_buf(CS_GC_BYTES, len, line);
    memcpy(p, raw, len);
    free(raw);
    return cs_bytes_of(p, len);
  }
  {
    char* p = cs_gc_buf(CS_GC_BYTES, len, line);
    cs_value as_bytes;
    memcpy(p, raw, len);
    free(raw);
    as_bytes = cs_bytes_of(p, len);
    return cs_builtin_decode(as_bytes, line);
  }
}

static cs_value cs_method_file(cs_value obj, const char* m,
                               const cs_value* args, size_t n, int line) {
  static const char* T = "file";
  cs_obj* o = obj.as.o;
  if (m[0] == 'r' && m[1] == 'e' && m[2] == 'a' && m[3] == 'd' && m[4] == '\0') {
    int64_t sz = 0;
    int has = 0;
    cs_arity_range(T, m, 0u, 1u, n, line);
    if (n == 1u) {
      if (args[0].tag != CS_INT)
        cs_runtime_error_as("TypeError", line,
                            "file.read(): size must be an int");
      if (args[0].as.i >= 0) {
        sz = args[0].as.i;
        has = 1;
      }
      /* negative size: Python reads everything (until EOF) */
    }
    return cs_file_read(o, has, sz, line);
  }
  if (m[0] == 'r' && m[1] == 'e' && m[2] == 'a' && m[3] == 'd' && m[4] == 'l' && m[5] == 'i' && m[6] == 'n' && m[7] == 'e' && m[8] == '\0') {
    int64_t sz = 0;
    int has = 0;
    cs_value lv;
    cs_arity_range(T, m, 0u, 1u, n, line);
    if (n == 1u) {
      if (args[0].tag != CS_INT)
        cs_runtime_error_as("TypeError", line,
                            "file.readline(): size must be an int");
      if (args[0].as.i >= 0) {
        sz = args[0].as.i;
        has = 1;
      }
      /* negative size: Python reads with no size cap (still one line) */
    }
    lv = cs_file_readline(obj, has, sz, line);
    if (lv.tag == CS_NONE) return CS_STR(""); /* Python: '' at EOF */
    return lv;
  }
  if (m[0] == 'r' && m[1] == 'e' && m[2] == 'a' && m[3] == 'd' && m[4] == 'l' && m[5] == 'i' && m[6] == 'n' && m[7] == 'e' && m[8] == 's' && m[9] == '\0') {
    cs_value out = cs_list_new();
    cs_value lv;
    cs_arity_fixed(T, m, 0u, n, line);
    for (;;) { /* no user code runs — no safe point — no rooting needed */
      lv = cs_file_readline(obj, 0, 0, line);
      if (lv.tag == CS_NONE) break;
      cs_list_push(out, lv, line);
    }
    return out;
  }
  if (m[0] == 'w' && m[1] == 'r' && m[2] == 'i' && m[3] == 't' && m[4] == 'e' && m[5] == '\0') {
    size_t nw;
    cs_arity_fixed(T, m, 1u, n, line);
    cs_file_expect_writable(o, line);
    if (o->as.file.binary) {
      if (args[0].tag != CS_BYTES)
        cs_runtime_error_as("TypeError", line,
                            "write() argument must be bytes-like, not '%s'",
                            cs_type_name(args[0]));
    } else if (args[0].tag != CS_STRING) {
      cs_runtime_error_as("TypeError", line,
                          "write() argument must be str, not '%s'",
                          cs_type_name(args[0]));
    }
    nw = fwrite(args[0].as.s.p, 1u, args[0].as.s.len, o->as.file.f);
    return cs_int((int64_t)nw); /* Python: bytes written */
  }
  if (m[0] == 'w' && m[1] == 'r' && m[2] == 'i' && m[3] == 't' && m[4] == 'e' && m[5] == 'l' && m[6] == 'i' && m[7] == 'n' && m[8] == 'e' && m[9] == 's' && m[10] == '\0') {
    cs_iter it;
    cs_value item;
    cs_arity_fixed(T, m, 1u, n, line);
    cs_file_expect_writable(o, line);
    it = cs_iter_open(args[0], line);
    while (cs_iter_next(&it, &item)) {
      if (o->as.file.binary) {
        if (item.tag != CS_BYTES)
          cs_runtime_error_as("TypeError", line,
                              "writelines() argument must be bytes-like, not "
                              "'%s'",
                              cs_type_name(item));
      } else if (item.tag != CS_STRING) {
        cs_runtime_error_as("TypeError", line,
                            "writelines() argument must be str, not '%s'",
                            cs_type_name(item));
      }
      (void)fwrite(item.as.s.p, 1u, item.as.s.len, o->as.file.f);
    }
    return cs_none();
  }
  if (m[0] == 's' && m[1] == 'e' && m[2] == 'e' && m[3] == 'k' && m[4] == '\0') {
    int whence = 0;
    cs_arity_range(T, m, 1u, 2u, n, line);
    if (args[0].tag != CS_INT)
      cs_runtime_error_as("TypeError", line,
                          "file.seek(): offset must be an int");
    if (n == 2u) {
      if (args[1].tag != CS_INT)
        cs_runtime_error_as("TypeError", line,
                            "file.seek(): whence must be an int");
      if (args[1].as.i < 0 || args[1].as.i > 2)
        cs_runtime_error_as("ValueError", line,
                            "file.seek(): whence must be 0, 1 or 2");
      whence = (int)args[1].as.i;
    }
    cs_file_expect_open(o, line);
    if (fseek(o->as.file.f, (long)args[0].as.i, whence) != 0)
      cs_runtime_error_as("OSError", line, "seek failed");
    return cs_int((int64_t)ftell(o->as.file.f));
  }
  if (m[0] == 't' && m[1] == 'e' && m[2] == 'l' && m[3] == 'l' && m[4] == '\0') {
    long p;
    cs_arity_fixed(T, m, 0u, n, line);
    cs_file_expect_open(o, line);
    p = ftell(o->as.file.f);
    if (p < 0) cs_runtime_error_as("OSError", line, "tell failed");
    return cs_int((int64_t)p);
  }
  if (m[0] == 'f' && m[1] == 'l' && m[2] == 'u' && m[3] == 's' && m[4] == 'h' && m[5] == '\0') {
    cs_arity_fixed(T, m, 0u, n, line);
    cs_file_expect_open(o, line);
    if (fflush(o->as.file.f) != 0)
      cs_runtime_error_as("OSError", line, "flush failed");
    return cs_none();
  }
  if (m[0] == 'c' && m[1] == 'l' && m[2] == 'o' && m[3] == 's' && m[4] == 'e' && m[5] == '\0') {
    cs_arity_fixed(T, m, 0u, n, line);
    cs_file_close_obj(o); /* idempotent — the path stays readable (f.name) */
    return cs_none();
  }
  cs_runtime_error_as("TypeError", line, "'%s' object has no method '%s'", T,
                      m);
  return cs_none(); /* not reached */
}

/* The write tail of the text-chain fusion (perf: formatted-output fusion).
 * Generated code for `f.write(<text chain>)` where every chain part is
 * provably text builds ONE stack buffer and calls this — the record goes
 * to the stdio buffer with a single fwrite, no heap text is materialized,
 * and the returned count matches file.write's bytes-written. The checks
 * mirror cs_method_file's write branch in the same order (writable, then
 * the binary/str type check); a non-file receiver keeps the full dynamic
 * dispatch and its exact errors by rebuilding the text and going through
 * cs_call_method — cold, and byte-identical. */
cs_value cs_file_write_buf(cs_value file, const char* p, size_t n, int line) {
  if (file.tag != CS_FILE) {
    cs_value t;
    char* q = NULL;
    if (n > 0u) {
      q = cs_gc_buf(CS_GC_TEXT, n, line);
      memcpy(q, p, n);
    }
    t = cs_str(q, n);
    return cs_call_method(file, "write", &t, 1u, NULL, 0u, line);
  }
  {
    cs_obj* o = file.as.o;
    cs_file_expect_writable(o, line);
    if (o->as.file.binary)
      cs_runtime_error_as("TypeError", line,
                          "write() argument must be bytes-like, not 'text'");
    return cs_int((int64_t)fwrite(p, 1u, n, o->as.file.f));
  }
}

/* ---- all / any -------------------------------------------------------- */

cs_value cs_builtin_all(cs_value iterable, int line) {
  cs_iter it;
  cs_value item;
  cs_no_missing(iterable, line);
  it = cs_iter_open(iterable, line);
  while (cs_iter_next(&it, &item))
    if (!cs_truthy(item, line)) return cs_bool(false);
  return cs_bool(true);
}

cs_value cs_builtin_any(cs_value iterable, int line) {
  cs_iter it;
  cs_value item;
  cs_no_missing(iterable, line);
  it = cs_iter_open(iterable, line);
  while (cs_iter_next(&it, &item))
    if (cs_truthy(item, line)) return cs_bool(true);
  return cs_bool(false);
}

/* ---- collection builders ---------------------------------------------- *
 * The loops below call no user code (cs_iter_next and the cs_* builders
 * never reach a GC safe point), so a partially built result needs no
 * rooting. The key/fn-calling builders (sorted/filter/map/max/min) protect
 * their result across callbacks — user loop bodies do hit safe points. */

cs_value cs_builtin_list(cs_value iterable, int line) {
  cs_iter it;
  cs_value item;
  cs_value out;
  if (iterable.tag == CS_MISSING) return cs_list_new(); /* list() */
  cs_no_missing(iterable, line);
  it = cs_iter_open(iterable, line);
  out = cs_list_new();
  while (cs_iter_next(&it, &item)) cs_list_push(out, item, line);
  return out;
}

cs_value cs_builtin_tuple(cs_value iterable, int line) {
  cs_value lv;
  if (iterable.tag == CS_MISSING) return cs_tuple_of(NULL, 0u);
  lv = cs_builtin_list(iterable, line);
  return cs_tuple_of(lv.as.o->as.seq.items, lv.as.o->as.seq.len);
}

cs_value cs_builtin_dict(const cs_value* pos, size_t npos,
                         const cs_value* named_vals,
                         const char* const* named_names, size_t nnamed,
                         int line) {
  cs_value m = cs_map_new();
  size_t i;
  if (npos > 1u)
    cs_runtime_error_as("TypeError", line,
                        "dict() takes at most 1 positional argument");
  if (npos == 1u) {
    cs_value src = pos[0];
    if (src.tag == CS_MAP) {
      const cs_table* t = &src.as.o->as.table;
      for (i = 0; i < t->len; i++)
        if (t->e[i].used) cs_map_set(m, t->e[i].key, t->vals[i], line);
    } else {
      cs_iter it = cs_iter_open(src, line); /* not iterable → TypeError */
      cs_value item;
      size_t idx = 0;
      while (cs_iter_next(&it, &item)) {
        if (item.tag != CS_TUPLE && item.tag != CS_LIST)
          cs_runtime_error_as("TypeError", line,
                              "dictionary update sequence element #%zu is not "
                              "a pair",
                              idx);
        if (item.as.o->as.seq.len != 2u)
          cs_runtime_error_as("TypeError", line,
                              "dictionary update sequence element #%zu has "
                              "length %zu; 2 is required",
                              idx, item.as.o->as.seq.len);
        cs_map_set(m, cs_seq_at(item.as.o, 0), cs_seq_at(item.as.o, 1),
                   line);
        idx++;
      }
    }
  }
  for (i = 0; i < nnamed; i++)
    cs_map_set(m, cs_text_lit(named_names[i]), named_vals[i], line);
  return m;
}

cs_value cs_builtin_zip(const cs_value* pos, size_t npos, int line) {
  cs_iter* itv;
  cs_value* row;
  cs_value out;
  size_t k;
  if (npos == 0u) return cs_list_new(); /* Python: zip() -> [] */
  itv = (cs_iter*)malloc(npos * sizeof(cs_iter));
  if (itv == NULL) cs_runtime_error(line, "out of memory");
  for (k = 0; k < npos; k++) itv[k] = cs_iter_open(pos[k], line);
  row = (cs_value*)malloc(npos * sizeof(cs_value));
  if (row == NULL) cs_runtime_error(line, "out of memory");
  out = cs_list_new();
  for (;;) {
    bool done = false;
    for (k = 0; k < npos; k++)
      if (!cs_iter_next(&itv[k], &row[k])) {
        done = true;
        break;
      }
    if (done) break; /* shortest input wins (Python 3) */
    cs_list_push(out, cs_tuple_of(row, npos), line);
  }
  free(itv);
  free(row);
  return out;
}

cs_value cs_builtin_enumerate(cs_value iterable, cs_value start, int line) {
  int64_t i;
  cs_iter it;
  cs_value item;
  cs_value out;
  cs_no_missing(iterable, line);
  if (start.tag == CS_MISSING) i = 0;
  else if (start.tag == CS_INT) i = start.as.i;
  else
    cs_runtime_error_as("TypeError", line,
                        "enumerate() start must be an int, not '%s'",
                        cs_type_name(start));
  it = cs_iter_open(iterable, line);
  out = cs_list_new();
  while (cs_iter_next(&it, &item)) {
    cs_value pair[2];
    pair[0] = cs_int(i);
    pair[1] = item;
    cs_list_push(out, cs_tuple_of(pair, 2u), line);
    i = (int64_t)((uint64_t)i + 1u); /* modular: wraps like the rest of int64 */
  }
  return out;
}

cs_value cs_builtin_reversed(cs_value sequence, int line) {
  cs_iter it;
  cs_value item;
  cs_value out;
  cs_value* a;
  size_t n, i;
  cs_no_missing(sequence, line);
  switch (sequence.tag) {
    case CS_RANGE:
    case CS_STRING:
    case CS_BYTES:
    case CS_LIST:
    case CS_TUPLE:
    case CS_MAP: /* like Python dicts — reverse iterates the keys */
      break;
    default: /* Python 3.8+: sets are NOT reversible; iterators are not
              * sequences (their cursor would be shared, not reversed). */
      cs_runtime_error_as("TypeError", line, "'%s' object is not reversible",
                          cs_type_name(sequence));
  }
  it = cs_iter_open(sequence, line);
  out = cs_list_new();
  while (cs_iter_next(&it, &item)) cs_list_push(out, item, line);
  /* in-place reversal: one pass, no second buffer */
  a = out.as.o->as.seq.items;
  n = out.as.o->as.seq.len;
  for (i = 0; i < n / 2u; i++) {
    cs_value t = a[i];
    a[i] = a[n - 1u - i];
    a[n - 1u - i] = t;
  }
  return out;
}

cs_value cs_builtin_filter(cs_value fn, cs_value iterable, int line) {
  cs_iter it;
  cs_value item;
  cs_value out;
  bool is_none;
  cs_no_missing(iterable, line);
  is_none = (fn.tag == CS_MISSING || fn.tag == CS_NONE);
  if (!is_none && !cs_is_callable(fn))
    cs_runtime_error_as("TypeError", line,
                        "filter() arg 1 must be callable or None");
  it = cs_iter_open(iterable, line);
  out = cs_list_new();
  if (is_none) { /* None → keep the truthy elements */
    while (cs_iter_next(&it, &item))
      if (cs_truthy(item, line)) cs_list_push(out, item, line);
    return out;
  }
  cs_gc_protect(&out, 1u); /* fn runs user code → safe points can collect */
  while (cs_iter_next(&it, &item))
    if (cs_truthy(cs_call1(fn, item, line), line)) cs_list_push(out, item, line);
  cs_gc_unprotect(1u);
  return out;
}

cs_value cs_builtin_map(const cs_value* pos, size_t npos, int line) {
  cs_value fn;
  cs_iter* itv;
  cs_value* row;
  cs_value out;
  size_t k, nit;
  if (npos < 2u) return cs_list_new(); /* semantic enforces map(f, it, ...) */
  fn = pos[0];
  if (!cs_is_callable(fn))
    cs_runtime_error_as("TypeError", line, "map() arg 1 must be callable");
  nit = npos - 1u;
  itv = (cs_iter*)malloc(nit * sizeof(cs_iter));
  if (itv == NULL) cs_runtime_error(line, "out of memory");
  for (k = 0; k < nit; k++) itv[k] = cs_iter_open(pos[1u + k], line);
  row = (cs_value*)malloc(nit * sizeof(cs_value));
  if (row == NULL) cs_runtime_error(line, "out of memory");
  out = cs_list_new();
  cs_gc_protect(&out, 1u); /* fn runs user code → safe points can collect */
  for (;;) {
    bool done = false;
    for (k = 0; k < nit; k++)
      if (!cs_iter_next(&itv[k], &row[k])) {
        done = true;
        break;
      }
    if (done) break;
    cs_list_push(out, cs_call(fn, row, nit, NULL, 0u, line), line);
  }
  cs_gc_unprotect(1u);
  free(itv);
  free(row);
  return out;
}

/* Stable sort over (key, item) records — `sorted()`'s engine (≥ §5).
 *
 * Structure (perf: sort_ops / sort_text / sort_small, §5):
 *   - ping-pong bottom-up merge: each level merges src -> dst and swaps,
 *     instead of memcpy'ing the whole array back after EVERY level; one
 *     final copy only if the finished run sits in the spare buffer.
 *     (An index-array variant — 8-B moves instead of 64-B — was measured
 *     and REJECTED: the random dec[idx] indirection per compare cost more
 *     than the saved bandwidth, 259 ms vs 153 ms. Do not re-try without a
 *     new measurement.)
 *   - record width: sorted() WITHOUT key= stores ONE 32-byte value per
 *     element (the key IS the item), so those merges move half the bytes
 *     of the 64-byte decorated pair. key= arrays keep the 64-byte record.
 *   - monotone-run probe: when EVERY key carries the same non-raising tag
 *     (int / text / bytes — comparing equal tags can never raise and has
 *     no side effect), one O(n) pass recognises fully ascending / fully
 *     descending inputs and answers without merging at all; a reversed
 *     result is produced by reversing the records and reversing the
 *     equal-key runs back, which keeps ties in input order exactly like
 *     the stable merge. Arrays whose keys are NOT tag-uniform (int vs
 *     text, bool vs int, mixed numbers) skip the probe AND the insertion
 *     cutoff: they run the classic merge with the classic comparison
 *     sequence, so the first-raised TypeError pair and operand order
 *     (pinned by tests/cases/sorted_edges) are bit-for-bit legacy.
 *   - insertion cutoff (tag-uniform keys only, n <= 16): one pass, no
 *     scratch allocation, ties never swapped (the merge's take-left rule),
 *     and the first compared pair is (a[0], a[1]) — the same first pair
 *     the w=1 level compares. sorted()-small stops paying merge setup.
 *   - all-int keys (the sort_ops shape): order is computed on 16-byte
 *     (key, original index) pairs — contiguous 16-B merge records, a plain
 *     int64 compare, no tag dispatch — and the result is emitted by
 *     indexing the decor array through the sorted index order, so the
 *     64-B records are never merged at all. Ties go to the LEFT index,
 *     i.e. the original order: the permutation is exactly the stable merge
 *     rearrangement (verified against the pinned stability cases).
 *
 * All fast paths preserve the OBSERVABLES: stable order in both
 * directions, exact TypeError text (only tag-uniform key sets take the
 * new paths, where no comparison can raise), and `key=` is called before
 * this engine runs — exactly once per element, in input order (§ pinned).
 */
typedef struct cs_decor {
  cs_value key;
  cs_value item;
} cs_decor;

/* 16-byte merge record for the all-int path: key + original position. */
typedef struct cs_ik {
  int64_t k;
  int64_t i;
} cs_ik;

#define CS_KEYDEC(r) ((r).key)
#define CS_KEYVAL(r) (r)

/* ---- monotone probe + record reversal (tag-uniform keys only) -------- */

/* kind: 1 = every key CS_INT, 2 = every CS_STRING, 3 = every CS_BYTES,
 * 0 = anything else (mixed / bool / float / missing ...): 0 means "the
 * comparisons could raise or differ, use only the classic sequence". */
static int cs_keys_kind(const void* base, size_t rec, size_t n) {
  const char* cur = (const char*)base;
  int kind;
  size_t i;
  switch (((const cs_value*)cur)->tag) {
    case CS_INT: kind = 1; break;
    case CS_STRING: kind = 2; break;
    case CS_BYTES: kind = 3; break;
    default: return 0;
  }
  for (i = 1; i < n; i++) {
    cur += rec;
    if (((const cs_value*)cur)->tag != (kind == 1 ? CS_INT
                                                  : (kind == 2 ? CS_STRING : CS_BYTES)))
      return 0;
  }
  return kind;
}

static int cs_key_cmp3_ord(const cs_value* a, const cs_value* b, int kind) {
  if (kind == 1) return (a->as.i < b->as.i) ? -1 : ((a->as.i > b->as.i) ? 1 : 0);
  return cs_str_cmp(*a, *b);
}

/* Reverse the records, then reverse each equal-key run back: the result of
 * "opposite ordering" for a monotone input with ties still in input order
 * (probe kinds only — those comparisons cannot raise). */
static void cs_records_revfix(void* base, size_t rec, size_t n, int kind) {
  char* b = (char*)base;
  size_t i, j;
  for (i = 0, j = n; i + 1 < j; i++, j--) { /* reverse all */
    char* p = b + i * rec;
    char* q = b + (j - 1) * rec;
    for (size_t k = 0; k < rec; k++) { char t = p[k]; p[k] = q[k]; q[k] = t; }
  }
  /* equal-key runs (now adjacent) flip back to input order */
  i = 0;
  while (i < n) {
    size_t e = i + 1;
    while (e < n && cs_key_cmp3_ord((const cs_value*)(b + i * rec),
                                    (const cs_value*)(b + e * rec), kind) == 0)
      e++;
    if (e - i > 1u) {
      for (size_t a2 = i, b2 = e; a2 + 1 < b2; a2++, b2--) { /* flip run */
        char* p = b + a2 * rec;
        char* q = b + (b2 - 1) * rec;
        for (size_t k = 0; k < rec; k++) { char t = p[k]; p[k] = q[k]; q[k] = t; }
      }
    }
    i = e;
  }
}

/* O(n) monotone classification. -1 = neither (needs a sort), 0 = both
 * (all keys equal: input order IS the answer), 1 = ascending only,
 * 2 = descending only. */
static int cs_record_order(const void* base, size_t rec, size_t n, int kind) {
  const char* cur = (const char*)base;
  bool asc = true, desc = true;
  size_t i;
  for (i = 1; i < n; i++) {
    const cs_value* prev = (const cs_value*)cur;
    cur += rec;
    int c = cs_key_cmp3_ord(prev, (const cs_value*)cur, kind);
    if (c > 0) asc = false;  /* prev > cur breaks ascending */
    else if (c < 0) desc = false; /* prev < cur breaks descending */
    if (!asc && !desc) return -1;
  }
  return (asc && desc) ? 0 : (asc ? 1 : 2);
}

/* ---- generic merge (classic comparison sequence) ---------------------- *
 * REC record with key expression KOF(p). Comparison pairs, operands and
 * phase order are exactly the classic loop's, so first-raised
 * TypeError/UNORDERABLE/stability output is unchanged. */
#define CS_DECL_MERGE(NAME, REC, KOF)                                        \
  static void NAME(REC* a, REC* tmp, size_t n, bool rev, int line) {         \
    size_t w;                                                                \
    REC* src = a;                                                            \
    REC* dst = tmp;                                                          \
    for (w = 1u; w < n; w *= 2u) {                                           \
      size_t i;                                                              \
      for (i = 0; i < n; i += 2u * w) {                                      \
        size_t l = i;                                                        \
        size_t m = (i + w < n) ? i + w : n;                                  \
        size_t r = (i + 2u * w < n) ? i + 2u * w : n;                        \
        size_t p = l, q = m, k = l;                                          \
        while (p < m && q < r) {                                             \
          cs_ord c;                                                          \
          cs_value ka = KOF(src[p]);                                         \
          cs_value kb = KOF(src[q]);                                         \
          if (ka.tag == CS_INT && kb.tag == CS_INT) {                        \
            int64_t x = ka.as.i, y = kb.as.i;                                \
            c = (x < y) ? CS_ORD_LESS : ((x > y) ? CS_ORD_GREATER : CS_ORD_EQUAL); \
          } else {                                                           \
            c = cs_cmp3(ka, kb, line, "<");                                  \
          }                                                                  \
          /* ties always take p → the sort is stable in both directions */   \
          bool take_p = rev ? (c != CS_ORD_LESS) : (c != CS_ORD_GREATER);    \
          dst[k++] = take_p ? src[p++] : src[q++];                           \
        }                                                                    \
        while (p < m) dst[k++] = src[p++];                                   \
        while (q < r) dst[k++] = src[q++];                                   \
      }                                                                      \
      {                                                                      \
        REC* sw = src;                                                       \
        src = dst;                                                           \
        dst = sw;                                                            \
      }                                                                      \
    }                                                                        \
    if (src != a) memcpy(a, src, n * sizeof(REC)); /* spare holds result */  \
  }

CS_DECL_MERGE(cs_sort_decor, cs_decor, CS_KEYDEC)
CS_DECL_MERGE(cs_sort_plain, cs_value, CS_KEYVAL)

/* ---- insertion cutoff, tag-uniform keys only (n <= 16) --------------- *
 * Same take-left rule as the merge (ties never move), same first compared
 * pair (a[0], a[1]); comparisons may only use the tag-uniform kinds, so
 * none of them can raise. */
#define CS_DECL_INSERT(NAME, REC, KOF)                                       \
  static void NAME(REC* a, size_t n, bool rev, int kind, int line) {         \
    size_t i;                                                                \
    (void)line;                                                              \
    for (i = 1; i < n; i++) {                                                \
      size_t j = i;                                                          \
      while (j > 0) {                                                        \
        cs_value x = KOF(a[j - 1]), y = KOF(a[j]);                           \
        int c = (kind == 1) ? ((x.as.i < y.as.i) ? -1 : ((x.as.i > y.as.i) ? 1 : 0))  \
                            : cs_str_cmp(x, y);                              \
        bool swap2 = rev ? (c < 0) : (c > 0);                                \
        if (!swap2) break;                                                   \
        { REC t = a[j - 1]; a[j - 1] = a[j]; a[j] = t; }                     \
        j--;                                                                 \
      }                                                                      \
    }                                                                        \
  }

CS_DECL_INSERT(cs_insert_decor, cs_decor, CS_KEYDEC)
CS_DECL_INSERT(cs_insert_plain, cs_value, CS_KEYVAL)

/* ---- all-int path: merge 16-B (key, index) pairs ---------------------- */
static void cs_sort_iks(cs_ik* a, cs_ik* tmp, size_t n, bool rev) {
  size_t w;
  cs_ik* src = a;
  cs_ik* dst = tmp;
  for (w = 1u; w < n; w *= 2u) {
    size_t i;
    for (i = 0; i < n; i += 2u * w) {
      size_t l = i;
      size_t m = (i + w < n) ? i + w : n;
      size_t r = (i + 2u * w < n) ? i + 2u * w : n;
      size_t p = l, q = m, k = l;
      while (p < m && q < r) {
        int64_t x = src[p].k, y = src[q].k;
        bool take_p = rev ? (!(x < y)) : (!(x > y)); /* ties: left == stable */
        dst[k++] = take_p ? src[p++] : src[q++];
      }
      while (p < m) dst[k++] = src[p++];
      while (q < r) dst[k++] = src[q++];
    }
    {
      cs_ik* sw = src;
      src = dst;
      dst = sw;
    }
  }
  if (src != a) memcpy(a, src, n * sizeof(cs_ik));
}

cs_value cs_builtin_sorted(cs_value iterable, cs_value key, cs_value reverse,
                           int line) {
  cs_iter it;
  cs_value item;
  cs_value out;
  cs_value keys; /* roots the key results while callbacks can collect */
  void* dec = NULL; /* cs_decor[n] with key=, cs_value[n] otherwise */
  void* tmp = NULL;
  cs_ik* pairs = NULL; /* all-int path: (key, index) records */
  cs_ik* pairtmp = NULL;
  size_t cnt = 0, cap = 0, i;
  size_t rec;
  bool rev = false, use_key, sort_needed = true, pair_mode = false;
  int kind;
  cs_no_missing(iterable, line);
  if (reverse.tag != CS_MISSING) rev = cs_truthy(reverse, line);
  use_key = (key.tag != CS_MISSING && key.tag != CS_NONE);
  if (use_key && !cs_is_callable(key))
    cs_runtime_error_as("TypeError", line,
                        "sorted(): 'key' must be callable or None");
  it = cs_iter_open(iterable, line);
  keys = cs_list_new();
  if (use_key) cs_gc_protect(&keys, 1u);
  rec = use_key ? sizeof(cs_decor) : sizeof(cs_value);
  while (cs_iter_next(&it, &item)) {
    if (cnt == cap) {
      size_t ncap = (cap != 0u) ? cap * 2u : 16u;
      void* nd = realloc(dec, ncap * rec);
      if (nd == NULL) cs_runtime_error(line, "out of memory");
      dec = nd;
      cap = ncap;
    }
    if (use_key) {
      cs_decor* d = (cs_decor*)((char*)dec + cnt * rec);
      cs_value kv = cs_call1(key, item, line);
      cs_list_push(keys, kv, line); /* the list marks it during callbacks */
      d->key = kv;
      d->item = item;
    } else {
      *(cs_value*)((char*)dec + cnt * rec) = item;
    }
    cnt++;
  }
  if (use_key) cs_gc_unprotect(1u);
  if (cnt > 1u) {
    const size_t n = cnt;
    kind = cs_keys_kind(dec, rec, n);
    if (kind != 0) {
      /* Tag-uniform keys: order probe first — a monotone input needs no
       * sort at all (and its reversed answer is a revfix, not a merge). */
      int ord = cs_record_order(dec, rec, n, kind);
      if (ord == 0) { /* all keys equal: input order is already stable */
        sort_needed = false;
      } else if (ord == 1) { /* ascending */
        if (rev) cs_records_revfix(dec, rec, n, kind);
        sort_needed = false;
      } else if (ord == 2) { /* descending */
        if (!rev) cs_records_revfix(dec, rec, n, kind);
        sort_needed = false;
      } else if (n <= 16u) {
        if (rec == sizeof(cs_decor)) cs_insert_decor((cs_decor*)dec, n, rev, kind, line);
        else cs_insert_plain((cs_value*)dec, n, rev, kind, line);
        sort_needed = false;
      } else if (kind == 1) {
        pair_mode = true; /* all-int: sort 16-B pairs, emit via index */
        sort_needed = true;
      } else {
        tmp = malloc(n * rec);
        if (tmp == NULL) cs_runtime_error(line, "out of memory");
        if (rec == sizeof(cs_decor)) cs_sort_decor((cs_decor*)dec, (cs_decor*)tmp, n, rev, line);
        else cs_sort_plain((cs_value*)dec, (cs_value*)tmp, n, rev, line);
        free(tmp);
        tmp = NULL;
        sort_needed = false;
      }
    }
    if (pair_mode) {
      pairs = (cs_ik*)malloc(n * sizeof(cs_ik));
      pairtmp = (cs_ik*)malloc(n * sizeof(cs_ik));
      if (pairs == NULL || pairtmp == NULL) cs_runtime_error(line, "out of memory");
      {
        const char* cur = (const char*)dec;
        for (i = 0; i < n; i++) {
          pairs[i].k = ((const cs_value*)cur)->as.i;
          pairs[i].i = (int64_t)i;
          cur += rec;
        }
      }
      cs_sort_iks(pairs, pairtmp, n, rev);
      free(pairtmp);
      pairtmp = NULL;
      sort_needed = false;
    } else if (sort_needed && kind == 0) {
      /* Legacy path: mixed/other keys — classic merge, classic sequence
       * (no probe, no cutoff, no pairs: the TypeError pins depend on it). */
      tmp = malloc(n * rec);
      if (tmp == NULL) cs_runtime_error(line, "out of memory");
      if (rec == sizeof(cs_decor)) cs_sort_decor((cs_decor*)dec, (cs_decor*)tmp, n, rev, line);
      else cs_sort_plain((cs_value*)dec, (cs_value*)tmp, n, rev, line);
      free(tmp);
      tmp = NULL;
      sort_needed = false;
    }
  }
  out = cs_list_new();
  if (pair_mode) {
    const char* base = (const char*)dec;
    for (i = 0; i < cnt; i++) {
      const char* p = base + (size_t)pairs[i].i * rec;
      cs_list_push(out, rec == sizeof(cs_decor) ? ((const cs_decor*)p)->item
                                                : *(const cs_value*)p, line);
    }
    free(pairs);
  } else if (rec == sizeof(cs_decor)) {
    const cs_decor* d = (const cs_decor*)dec;
    for (i = 0; i < cnt; i++) cs_list_push(out, d[i].item, line);
  } else {
    const cs_value* v = (const cs_value*)dec;
    for (i = 0; i < cnt; i++) cs_list_push(out, v[i], line);
  }
  free(dec);
  return out;
}
cs_value cs_builtin_sum(cs_value iterable, cs_value start, int line) {
  cs_iter it;
  cs_value item;
  cs_value acc;
  cs_no_missing(iterable, line);
  /* SIMD fast path over fvec/ivec. ivec: wrapping int64 add — addition is
   * associative modulo 2^64, so the lane order is unobservable. fvec:
   * lane-order accumulation, the documented fvec reduction semantics (it
   * may differ in the last bits from a sequential loop). A supplied scalar
   * start is added to the finished sum; ivec with a non-int start falls
   * back to the exact sequential loop. */
  if (iterable.tag == CS_FVEC || iterable.tag == CS_IVEC) {
    size_t n = iterable.as.o->as.vec.len;
    size_t j = 0;
    if (iterable.tag == CS_IVEC) {
      const int64_t* d = (const int64_t*)iterable.as.o->as.vec.data;
      uint64_t u = 0;
      if (start.tag != CS_MISSING && start.tag != CS_INT) goto seq;
      for (; j + CS_VW <= n; j += CS_VW) {
        cs_vi va;
        size_t k;
        memcpy(&va, d + j, sizeof va);
        for (k = 0; k < CS_VW; k++) u += (uint64_t)va[k];
      }
      for (; j < n; j++) u += (uint64_t)d[j];
      if (start.tag == CS_MISSING) return cs_int((int64_t)u);
      return cs_add(start, cs_int((int64_t)u), line);
    } else {
      const double* d = (const double*)iterable.as.o->as.vec.data;
      cs_vd vacc = {0};
      double s = 0.0;
      size_t k;
      for (; j + CS_VW <= n; j += CS_VW) {
        cs_vd va;
        memcpy(&va, d + j, sizeof va);
        vacc += va;
      }
      for (k = 0; k < CS_VW; k++) s += vacc[k];
      for (; j < n; j++) s += d[j];
      if (start.tag == CS_MISSING) return cs_float(s);
      return cs_add(start, cs_float(s), line);
    }
  }
seq:
  acc = (start.tag == CS_MISSING) ? cs_int(0) : start;
  it = cs_iter_open(iterable, line);
  while (cs_iter_next(&it, &item)) acc = cs_add(acc, item, line);
  return acc;
}

/* max/min: the variadic lane takes no arg_slots (codegen passes positionals
 * as the array and key/default as fixed trailing parameters). A one-element
 * list carries the current best key so the protect stack stays a single
 * push across key callbacks. */
static cs_value cs_minmax(const cs_value* pos, size_t npos, cs_value key,
                          cs_value dflt, int line, bool want_max) {
  const char* nm = want_max ? "max" : "min";
  bool use_key = (key.tag != CS_MISSING && key.tag != CS_NONE);
  cs_value best_item;
  cs_value best_key;
  cs_value keep = cs_unset(); /* 1-slot holder for the best key (lazy) */
  bool have = false;
  size_t k;
  if (npos == 0u)
    cs_runtime_error_as("TypeError", line, "'%s' expected at least 1 argument",
                        nm);
  if (use_key && !cs_is_callable(key))
    cs_runtime_error_as("TypeError", line, "%s() argument 'key' must be "
                                           "callable or None",
                        nm);
  if (use_key) {
    keep = cs_list_new();
    cs_list_push(keep, cs_none(), line);
    cs_gc_protect(&keep, 1u); /* key callbacks can hit safe points */
  }
  if (npos == 1u) {
    /* SIMD fast path: min/max over a non-empty fvec/ivec with no key —
     * same boxes and same cs_cmp3 comparisons as the iteration path
     * below, so first-wins tie semantics are identical. Empty vectors
     * fall through (they follow the standard default/ValueError route). */
    size_t vn = pos[0].as.o != NULL ? pos[0].as.o->as.vec.len : 0u;
    if (!use_key && (pos[0].tag == CS_FVEC || pos[0].tag == CS_IVEC) &&
        vn > 0u) {
      bool fv = pos[0].tag == CS_FVEC;
      const double* fd = (const double*)pos[0].as.o->as.vec.data;
      const int64_t* id = (const int64_t*)pos[0].as.o->as.vec.data;
      size_t j;
      cs_value best = fv ? cs_float(fd[0]) : cs_int(id[0]);
      for (j = 1; j < vn; j++) {
        cs_value cur = fv ? cs_float(fd[j]) : cs_int(id[j]);
        cs_ord c = cs_cmp3(cur, best, line, "<");
        bool better = want_max ? (c == CS_ORD_GREATER) : (c == CS_ORD_LESS);
        if (better) best = cur;
      }
      return best;
    }
    cs_iter it = cs_iter_open(pos[0], line); /* not iterable → TypeError */
    cs_value item;
    while (cs_iter_next(&it, &item)) {
      cs_value kv = use_key ? cs_call1(key, item, line) : item;
      if (!have) {
        best_item = item;
        best_key = kv;
        if (use_key) keep.as.o->as.seq.items[0] = kv;
        have = true;
        continue;
      }
      {
        cs_ord c = cs_cmp3(kv, best_key, line, "<");
        bool better = want_max ? (c == CS_ORD_GREATER) : (c == CS_ORD_LESS);
        if (better) {
          best_item = item;
          best_key = kv;
          if (use_key) keep.as.o->as.seq.items[0] = kv;
        }
      }
    }
    if (!have) {
      if (dflt.tag != CS_MISSING) {
        if (use_key) cs_gc_unprotect(1u);
        return dflt;
      }
      cs_runtime_error_as("ValueError", line, "%s() arg is an empty sequence",
                          nm);
    }
    if (use_key) cs_gc_unprotect(1u);
    return best_item;
  }
  /* variadic: every positional is an operand (already rooted by the frame) */
  for (k = 0; k < npos; k++) {
    cs_value kv = use_key ? cs_call1(key, pos[k], line) : pos[k];
    if (!have) {
      best_item = pos[k];
      best_key = kv;
      if (use_key) keep.as.o->as.seq.items[0] = kv;
      have = true;
      continue;
    }
    {
      cs_ord c = cs_cmp3(kv, best_key, line, "<");
      bool better = want_max ? (c == CS_ORD_GREATER) : (c == CS_ORD_LESS);
      if (better) {
        best_item = pos[k];
        best_key = kv;
        if (use_key) keep.as.o->as.seq.items[0] = kv;
      }
    }
  }
  if (use_key) cs_gc_unprotect(1u);
  return best_item;
}

cs_value cs_builtin_max(const cs_value* pos, size_t npos, cs_value key,
                        cs_value dflt, int line) {
  return cs_minmax(pos, npos, key, dflt, line, true);
}

cs_value cs_builtin_min(const cs_value* pos, size_t npos, cs_value key,
                        cs_value dflt, int line) {
  return cs_minmax(pos, npos, key, dflt, line, false);
}

/* ---- numeric (SPEC §5.11.5) ------------------------------------------- */

cs_value cs_builtin_abs(cs_value x, int line) {
  cs_no_missing(x, line);
  if (x.tag == CS_INT) return cs_int(cs_nabs(x.as.i));
  if (x.tag == CS_FLOAT) return cs_float(fabs(x.as.d));
  cs_runtime_error_as("TypeError", line,
                      "bad operand type for abs(): '%s'", cs_type_name(x));
  return cs_none(); /* not reached */
}

cs_value cs_builtin_divmod(cs_value a, cs_value b, int line) {
  cs_value pair[2];
  cs_no_missing2(a, b, line);
  if (a.tag != CS_INT || b.tag != CS_INT)
    cs_runtime_error_as("TypeError", line,
                        "divmod() expects int operands, got '%s' and '%s'",
                        cs_type_name(a), cs_type_name(b));
  if (b.as.i == 0) cs_runtime_error_as("ZeroDivisionError", line,
                                       "division by zero");
  if (a.as.i == INT64_MIN && b.as.i == -1) { /* the only overflow case */
    pair[0] = cs_int(INT64_MIN);
    pair[1] = cs_int(0);
  } else {
    pair[0] = cs_int(a.as.i / b.as.i); /* C99 truncation, like / and % */
    pair[1] = cs_int(a.as.i % b.as.i);
  }
  return cs_tuple_of(pair, 2u);
}

cs_value cs_builtin_pow(cs_value base, cs_value exp, cs_value mod, int line) {
  cs_no_missing2(base, exp, line);
  if (mod.tag == CS_MISSING) return cs_pow(base, exp, line); /* base ** exp */
  /* 3-argument modular form: int-only (cstar's modulo is int-only) */
  if (base.tag != CS_INT || exp.tag != CS_INT || mod.tag != CS_INT)
    cs_runtime_error_as("TypeError", line,
                        "pow() 3-argument form expects int operands");
  if (mod.as.i == 0)
    cs_runtime_error_as("ValueError", line, "pow() 3rd argument cannot be 0");
  if (mod.as.i < 0)
    cs_runtime_error_as("ValueError", line,
                        "pow() 3rd argument must be positive (cstar does "
                        "not support negative moduli)");
  if (exp.as.i < 0)
    cs_runtime_error_as("ValueError", line,
                        "pow() 3-argument form does not support negative "
                        "exponents (no modular inverse)");
  {
    /* exact modular exponentiation: __int128 keeps every product in range
     * (m < 2^63 → m*m < 2^126) */
    unsigned __int128 r = 1u;
    unsigned __int128 b = (unsigned __int128)(uint64_t)base.as.i %
                          (unsigned __int128)(uint64_t)mod.as.i;
    uint64_t e = (uint64_t)exp.as.i;
    unsigned __int128 m = (unsigned __int128)(uint64_t)mod.as.i;
    while (e != 0u) {
      if ((e & 1u) != 0u) r = (r * b) % m;
      b = (b * b) % m;
      e >>= 1;
    }
    return cs_int((int64_t)r);
  }
}

cs_value cs_builtin_round(cs_value number, cs_value ndigits, int line) {
  bool has_nd;
  int64_t nd = 0;
  cs_no_missing(number, line);
  has_nd = (ndigits.tag != CS_MISSING && ndigits.tag != CS_NONE);
  if (has_nd) {
    if (ndigits.tag != CS_INT)
      cs_runtime_error_as("TypeError", line,
                          "round(): 'ndigits' must be an int, not '%s'",
                          cs_type_name(ndigits));
    nd = ndigits.as.i;
  }
  if (number.tag == CS_INT) {
    if (!has_nd || nd >= 0) return number; /* int + non-negative: identity */
    { /* negative ndigits: round to a multiple of 10^-nd, half-to-even */
      uint64_t k = (uint64_t)(-(nd + 1)) + 1u; /* -nd without overflowing */
      if (k >= 39u) return cs_int(0); /* 5·10^38 already exceeds int64 */
      {
        unsigned __int128 scale = 10u, q, rem, half;
        unsigned __int128 mag;
        bool neg = number.as.i < 0;
        size_t j;
        for (j = 1u; j < k; j++) scale *= 10u;
        mag = neg ? (unsigned __int128)(0u - (uint64_t)number.as.i)
                  : (unsigned __int128)(uint64_t)number.as.i;
        q = mag / scale;
        rem = mag % scale;
        half = scale / 2u;
        if (rem > half || (rem == half && ((q & 1u) != 0u))) q += 1u;
        {
          unsigned __int128 res = q * scale;
          unsigned __int128 lim =
              (unsigned __int128)9223372036854775807ULL + (neg ? 1u : 0u);
          if (res > lim)
            cs_runtime_error_as("ValueError", line,
                                "round(): result out of int64 range");
          return cs_int(neg ? (int64_t)(0u - (uint64_t)res) : (int64_t)res);
        }
      }
    }
  }
  if (number.tag == CS_FLOAT) {
    double x = number.as.d;
    if (!has_nd) { /* → int, half-to-even (rint uses that mode) */
      double r;
      if (isnan(x))
        cs_runtime_error_as("ValueError", line,
                            "cannot convert float NaN to integer");
      if (isinf(x))
        cs_runtime_error_as("ValueError", line,
                            "cannot convert float infinity to integer");
      r = rint(x);
      if (r < -9223372036854775808.0 || r >= 9223372036854775808.0)
        cs_runtime_error_as("ValueError", line,
                            "round(): result out of int64 range");
      return cs_int((int64_t)r);
    }
    if (nd >= 0) { /* float result, correctly rounded (%.*f, half-even) */
      char buf[1100];
      int nn;
      if (isinf(x) || isnan(x)) return number;
      if (nd > 340) nd = 340; /* past binary precision, the value is itself */
      nn = snprintf(buf, sizeof buf, "%.*f", (int)nd, x);
      if (nn < 0 || (size_t)nn >= sizeof buf)
        cs_runtime_error(line, "round(): format buffer overrun");
      return cs_float(strtod(buf, NULL));
    }
    { /* negative ndigits: round to a multiple of 10^-nd */
      uint64_t k = (uint64_t)(-(nd + 1)) + 1u;
      if (k >= 309u) return cs_float(0.0); /* |x| < 5·10^308 always */
      {
        double scale = pow(10.0, (double)k);
        return cs_float(rint(x / scale) * scale);
      }
    }
  }
  cs_runtime_error_as("TypeError", line,
                      "round(): unsupported operand type '%s'",
                      cs_type_name(number));
  return cs_none(); /* not reached */
}

/* ---- text and numbers ------------------------------------------------- */

cs_value cs_builtin_ord(cs_value c, int line) {
  cs_no_missing(c, line);
  if (c.tag == CS_STRING || c.tag == CS_BYTES) {
    const char* what = (c.tag == CS_STRING) ? "string" : "bytes";
    if (c.as.s.len == 0u)
      cs_runtime_error_as("TypeError", line,
                          "ord() expected a character, but %s of length 0 "
                          "found",
                          what);
    if (c.tag == CS_BYTES) {
      if (c.as.s.len != 1u)
        cs_runtime_error_as("TypeError", line,
                            "ord() expected a character, but bytes of length "
                            "%zu found",
                            c.as.s.len);
      return cs_int((unsigned char)c.as.s.p[0]);
    }
    {
      size_t adv;
      uint32_t cp = cs_utf8_step(c.as.s.p, c.as.s.len, 0, &adv);
      if (adv != c.as.s.len)
        cs_runtime_error_as("TypeError", line,
                            "ord() expected a character, but string of length "
                            "%zu found",
                            c.as.s.len);
      return cs_int((int64_t)cp);
    }
  }
  cs_runtime_error_as("TypeError", line,
                      "ord() expected string of length 1, but %s found",
                      cs_type_name(c));
  return cs_none(); /* not reached */
}

cs_value cs_builtin_chr(cs_value i, int line) {
  char buf[4];
  size_t n;
  cs_no_missing(i, line);
  if (i.tag != CS_INT)
    cs_runtime_error_as("TypeError", line, "chr() expected int, got '%s'",
                        cs_type_name(i));
  if (i.as.i < 0 || i.as.i > 0x10FFFF)
    cs_runtime_error_as("ValueError", line,
                        "chr() arg not in range(0x110000)");
  if (i.as.i >= 0xD800 && i.as.i <= 0xDFFF)
    cs_runtime_error_as("ValueError", line,
                        "chr() surrogates are not allowed (cstar strings "
                        "are UTF-8)");
  n = cs_utf8_put(buf, (uint32_t)i.as.i);
  return cs_text_copy(buf, n, line);
}

cs_value cs_builtin_bin(cs_value x, int line) {
  cs_no_missing(x, line);
  return cs_base_convert(x, 2, "0b", line);
}

cs_value cs_builtin_oct(cs_value x, int line) {
  cs_no_missing(x, line);
  return cs_base_convert(x, 8, "0o", line);
}

cs_value cs_builtin_hex(cs_value x, int line) {
  cs_no_missing(x, line);
  return cs_base_convert(x, 16, "0x", line);
}

cs_value cs_builtin_ascii(cs_value x, int line) {
  return cs_repr_value(x, true, line); /* non-ASCII → \u/\U escapes */
}

cs_value cs_builtin_repr(cs_value v, int line) {
  return cs_repr_value(v, false, line);
}

cs_value cs_builtin_bytes(cs_value source, cs_value encoding, cs_value errors,
                          int line) {
  if (source.tag == CS_MISSING) return cs_bytes_of(NULL, 0u); /* bytes() */
  cs_no_missing(source, line);
  if (source.tag == CS_STRING) {
    if (encoding.tag == CS_MISSING || encoding.tag == CS_NONE)
      cs_runtime_error_as("TypeError", line,
                          "string argument without an encoding"); /* Python */
    if (encoding.tag != CS_STRING)
      cs_runtime_error_as("TypeError", line,
                          "bytes() argument 'encoding' must be str, not %s",
                          cs_type_name(encoding));
    if (!(encoding.as.s.len == 5u &&
          memcmp(encoding.as.s.p, "utf-8", 5u) == 0) &&
        !(encoding.as.s.len == 4u && memcmp(encoding.as.s.p, "utf8", 4u) == 0))
      cs_runtime_error_as("ValueError", line,
                          "cstar supports only the UTF-8 encoding");
    if (errors.tag != CS_MISSING && errors.tag != CS_NONE) {
      if (errors.tag != CS_STRING)
        cs_runtime_error_as("TypeError", line,
                            "bytes() argument 'errors' must be str, not %s",
                            cs_type_name(errors));
      /* The handler name itself is never consulted (Python looks it up
       * lazily too — only when an encode actually fails, and cstar
       * strings are validated UTF-8, so that cannot happen here). */
    }
    return cs_builtin_encode(source, line); /* UTF-8 is the identity encode */
  }
  if (encoding.tag != CS_MISSING && encoding.tag != CS_NONE)
    cs_runtime_error_as("TypeError", line,
                        "encoding without a string argument");
  if (errors.tag != CS_MISSING && errors.tag != CS_NONE)
    cs_runtime_error_as("TypeError", line, "errors without a string argument");
  if (source.tag == CS_BYTES) { /* immutable → copy to match Python's new object */
    char* p = NULL;
    if (source.as.s.len > 0u) {
      p = cs_gc_buf(CS_GC_BYTES, source.as.s.len, line);
      memcpy(p, source.as.s.p, source.as.s.len);
    }
    return cs_bytes_of(p, source.as.s.len);
  }
  if (source.tag == CS_INT || source.tag == CS_BOOL) {
    /* Python: bool is an int here — bytes(True) is b'\x00'. */
    int64_t n = (source.tag == CS_BOOL) ? (source.as.b ? 1 : 0) : source.as.i;
    char* p = NULL;
    if (n < 0)
      cs_runtime_error_as("ValueError", line, "negative count"); /* Python */
    if (n > 0) {
      size_t z = (size_t)n;
      p = cs_gc_buf(CS_GC_BYTES, z, line);
      memset(p, 0, z);
    }
    return cs_bytes_of(p, (n > 0) ? (size_t)n : 0u);
  }
  switch (source.tag) { /* anything else must be iterable (Python) */
    case CS_LIST:
    case CS_TUPLE:
    case CS_SET:
    case CS_MAP:
    case CS_RANGE:
    case CS_ITERATOR:
    case CS_FILE:
      break;
    default:
      cs_runtime_error_as("TypeError", line,
                          "cannot convert '%s' object to bytes",
                          cs_type_name(source));
  }
  { /* iterable of ints 0..255 */
    cs_iter it = cs_iter_open(source, line);
    cs_value item;
    cs_buf b;
    char* p = NULL;
    b.p = NULL;
    b.len = 0;
    b.cap = 0;
    while (cs_iter_next(&it, &item)) {
      if (item.tag == CS_BOOL)
        item = cs_int(item.as.b ? 1 : 0); /* Python: bool elements are ints */
      if (item.tag != CS_INT)
        cs_runtime_error_as("TypeError", line,
                            "'%s' object cannot be interpreted as an integer",
                            cs_type_name(item));
      if (item.as.i < 0 || item.as.i > 255)
        cs_runtime_error_as("ValueError", line,
                            "bytes must be in range(0, 256)");
      cs_buf_addc(&b, (char)(unsigned char)item.as.i, line);
    }
    if (b.len > 0u) {
      p = cs_gc_buf(CS_GC_BYTES, b.len, line);
      memcpy(p, b.p, b.len);
    }
    free(b.p);
    return cs_bytes_of(p, b.len);
  }
}

/* ---- slices (SPEC §5.11.5) -------------------------------------------- */

cs_value cs_builtin_slice(cs_value start, cs_value stop, cs_value step,
                          int line) {
  cs_value t[3];
  (void)line; /* Python 3.14: slice() stores whatever it is given — step
                * validation happens when the slice is APPLIED, which
                * cstar never does (xs[slice_obj] is not wired). */
  t[0] = (start.tag == CS_MISSING) ? cs_none() : start;
  t[1] = (stop.tag == CS_MISSING) ? cs_none() : stop;
  t[2] = (step.tag == CS_MISSING) ? cs_none() : step;
  return cs_tuple_of(t, 3u);
}

/* ---- unsupported stubs (SPEC §5.11.6) --------------------------------- */

cs_value cs_builtin_unsupported(const char* name, int line) {
  const char* why = "not supported";
  if (strcmp(name, "super") == 0)
    why = "classes have no inheritance (SPEC §5.11.6)";
  else if (strcmp(name, "property") == 0)
    why = "there are no descriptors or decorators";
  else if (strcmp(name, "staticmethod") == 0 || strcmp(name, "classmethod") == 0)
    why = "there are no decorators";
  else if (strcmp(name, "issubclass") == 0)
    why = "classes have no inheritance";
  else if (strcmp(name, "breakpoint") == 0)
    why = "there is no interactive debugger";
  else if (strcmp(name, "help") == 0)
    why = "there is no interactive help (SPEC.md is the reference)";
  else if (strcmp(name, "memoryview") == 0)
    why = "bytes are immutable and have no buffer views";
  else if (strcmp(name, "object") == 0)
    why = "define your own class — every class is already a root";
  cs_runtime_error_as("RuntimeError", line, "%s() is not supported: %s", name,
                      why);
  return cs_none(); /* not reached */
}

/* ---- HTTP builtins (libcurl): cs_builtin_http / cs_builtin_http_get /
 * cs_builtin_url_encode. The first two return "STATUS\nBODY" as Text. On a
 * connection/TLS failure before any bytes are received, status is 0 and
 * BODY is the curl error string. */

typedef struct {
  cs_buf buf;
} cs_http_ctx;

static size_t cs_http_write(char* ptr, size_t size, size_t nmemb, void* ud) {
  cs_http_ctx* ctx = (cs_http_ctx*)ud;
  size_t n = size * nmemb;
  if (n > 0) cs_buf_addn(&ctx->buf, ptr, n, 0);
  return n;
}

static char* cs_http_cstr(cs_value v) {
  char* s = (char*)malloc(v.as.s.len + 1);
  if (!s) return NULL;
  memcpy(s, v.as.s.p, v.as.s.len);
  s[v.as.s.len] = '\0';
  return s;
}

/* Persistent libcurl handle: keeps connections alive across calls, so every
 * call after the first skips the TCP + TLS handshake (the biggest part of the
 * network overhead). HTTP/2 is negotiated via ALPN when the server supports
 * it. */
static CURL* g_curl = NULL;

static void cs_http_init_handle(void) {
  if (g_curl) return;
  g_curl = curl_easy_init();
  if (!g_curl) return; /* caller reports */
  /* Prefer HTTP/3 (QUIC) when available; libcurl falls back to HTTP/2
   * if QUIC cannot be established. Keep-alive + handle reuse then keep the
   * connection (and TLS session) warm across calls. */
#ifdef CURL_HTTP_VERSION_3
  curl_easy_setopt(g_curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_3);
#else
  curl_easy_setopt(g_curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
#endif
  curl_easy_setopt(g_curl, CURLOPT_TCP_FASTOPEN, 1L);
  curl_easy_setopt(g_curl, CURLOPT_NOSIGNAL, 1L);
  /* Keep the reused connections alive (short idle) so they are not torn
   * down between calls; allow the pool to hold several host connections. */
  curl_easy_setopt(g_curl, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(g_curl, CURLOPT_TCP_KEEPIDLE, 20L);
  curl_easy_setopt(g_curl, CURLOPT_TCP_KEEPINTVL, 10L);
  curl_easy_setopt(g_curl, CURLOPT_MAXCONNECTS, 8L);
}

/* Build the curl_slist for a request. `headers` is either CS_NONE (default
 * JSON headers for a POST with body) or CS_STRING holding one header per
 * line ("Name: value"); blank lines are skipped. A caller-supplied
 * Content-Type suppresses the JSON default. Always appends Accept and the
 * User-Agent. */
static struct curl_slist* cs_http_build_headers(cs_value headers,
                                                 bool post_with_body) {
  struct curl_slist* hdrs = NULL;
  bool have_ct = false;
  if (headers.tag == CS_STRING) {
    const char* p = headers.as.s.p;
    size_t len = headers.as.s.len;
    size_t i = 0;
    while (i < len) {
      size_t j = i;
      while (j < len && p[j] != '\n') j++;
      size_t e = j;
      if (e > i && p[e - 1] == '\r') e--;
      if (e > i) {
        char* hline = (char*)malloc(e - i + 1);
        if (!hline) { curl_slist_free_all(hdrs); return NULL; }
        memcpy(hline, p + i, e - i);
        hline[e - i] = '\0';
        if (strncasecmp(hline, "Content-Type:", 13) == 0) have_ct = true;
        hdrs = curl_slist_append(hdrs, hline);
        free(hline);
        if (!hdrs) return NULL;
      }
      i = j + 1;
    }
  }
  if (post_with_body && !have_ct)
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  hdrs = curl_slist_append(hdrs, "Accept: application/json");
  hdrs = curl_slist_append(hdrs, "User-Agent: jev-pipeline/1.0 (cstar)");
  return hdrs;
}

/* Shared perform: method_post selects POST (with `body`, None = empty) vs
 * GET. Returns the "STATUS\nBODY" text. */
static cs_value cs_http_perform(cs_value url, bool method_post,
                                cs_value body, cs_value headers, int line) {
  cs_http_ctx ctx;
  struct curl_slist* hdrs = NULL;
  char errbuf[CURL_ERROR_SIZE];
  long status = 0;
  CURLcode rc;
  cs_buf rb;
  char stbuf[24];

  cs_http_init_handle();
  if (!g_curl)
    cs_runtime_error_as("RuntimeError", line, "http: curl init failed");

  memset(&ctx, 0, sizeof ctx);
  memset(errbuf, 0, sizeof errbuf);
  rb.p = NULL; rb.len = 0; rb.cap = 0;

  hdrs = cs_http_build_headers(headers, method_post && body.tag != CS_NONE);

  {
    char* uc = cs_http_cstr(url);
    char* bc = (method_post && body.tag != CS_NONE) ? cs_http_cstr(body) : NULL;
    if (!uc || (method_post && body.tag != CS_NONE && !bc)) {
      free(uc); free(bc);
      curl_slist_free_all(hdrs);
      return cs_none();
    }
    curl_easy_setopt(g_curl, CURLOPT_URL, uc);
    if (method_post) {
      curl_easy_setopt(g_curl, CURLOPT_POST, 1L);
      if (bc) curl_easy_setopt(g_curl, CURLOPT_COPYPOSTFIELDS, bc);
      else    curl_easy_setopt(g_curl, CURLOPT_POSTFIELDS, NULL);
    } else {
      curl_easy_setopt(g_curl, CURLOPT_HTTPGET, 1L);
    }
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(g_curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, cs_http_write);
    curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, hdrs);
    rc = curl_easy_perform(g_curl);
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &status);
    free(uc); free(bc);
  }
  curl_slist_free_all(hdrs);

  {
    int k = snprintf(stbuf, sizeof stbuf, "%ld", status);
    cs_buf_addn(&rb, stbuf, (size_t)k, line);
  }
  cs_buf_addc(&rb, '\n', line);
  {
    const char* msg = NULL;
    if (rc != CURLE_OK && ctx.buf.len == 0) {
      msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
      cs_buf_addn(&rb, msg, strlen(msg), line);
    } else if (ctx.buf.len > 0) {
      cs_buf_addn(&rb, ctx.buf.p, ctx.buf.len, line);
    }
  }
  free(ctx.buf.p);
  cs_buf_end(&rb, line);
  return cs_text_from_buf(&rb, line);
}

/* POST url with `body` (None = empty body); `headers` is None or a text of
 * newline-separated header lines (Burp-style raw header block). */
cs_value cs_builtin_http(cs_value url, cs_value body, cs_value headers,
                         int line) {
  if (url.tag != CS_STRING)
    cs_runtime_error_as("TypeError", line, "http_post() expects text url, got %s",
                        cs_type_name(url));
  if (body.tag != CS_STRING && body.tag != CS_NONE)
    cs_runtime_error_as("TypeError", line,
                        "http_post() expects text or None body, got %s",
                        cs_type_name(body));
  if (headers.tag != CS_STRING && headers.tag != CS_NONE)
    cs_runtime_error_as("TypeError", line,
                        "http_post() expects text or None headers, got %s",
                        cs_type_name(headers));
  return cs_http_perform(url, true, body, headers, line);
}

/* GET url; `headers` likewise None or a text of header lines. */
cs_value cs_builtin_http_get(cs_value url, cs_value headers, int line) {
  if (url.tag != CS_STRING)
    cs_runtime_error_as("TypeError", line, "http_get() expects text url, got %s",
                        cs_type_name(url));
  if (headers.tag != CS_STRING && headers.tag != CS_NONE)
    cs_runtime_error_as("TypeError", line,
                        "http_get() expects text or None headers, got %s",
                        cs_type_name(headers));
  return cs_http_perform(url, false, cs_none(), headers, line);
}

/* ---- url_encode(text): percent-encoding for query strings / form bodies --
 * Unreserved (kept literal): A-Z a-z 0-9 and '-' '_' '.' '~'. Every other
 * byte of the UTF-8 text becomes %XX (uppercase hex). */
cs_value cs_builtin_url_encode(cs_value text, int line) {
  cs_buf b;
  size_t i;
  static const char hex[] = "0123456789ABCDEF";
  if (text.tag != CS_STRING)
    cs_runtime_error_as("TypeError", line, "url_encode() expects text, got %s",
                        cs_type_name(text));
  b.p = NULL; b.len = 0; b.cap = 0;
  for (i = 0; i < text.as.s.len; i++) {
    unsigned char c = (unsigned char)text.as.s.p[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      cs_buf_addc(&b, (char)c, line);
    } else {
      cs_buf_addc(&b, '%', line);
      cs_buf_addc(&b, hex[c >> 4], line);
      cs_buf_addc(&b, hex[c & 0x0F], line);
    }
  }
  cs_buf_end(&b, line);
  return cs_text_from_buf(&b, line);
}
