// src/codegen.cpp — Pentest M2 backend: AST → C11 code generator.
//
// Implements generate_c() (src/codegen.h). Extends the M1 emitter
// (docs/contracts/m1-semantics.md §10) with everything Stage 2A adds
// (docs/contracts/m2a-semantics.md §11) and, as of the Stage 2B
// collections/text/bytes backend integration, everything
// docs/contracts/m2b-semantics.md §2-§8, §10, §12 define for the collection sector:
// collection literals (list/map/set/tuple), bytes literals, indexing,
// slicing, method calls, comma-join, explicit casts to index/destructuring
// assignment targets (compound forms included), and the len/str/int/float/
// bool/type/encode/decode/set builtins. The 2B function sector (uniform
// ABI: defaults *args/**kwargs, function values/indirect calls, nested
// functions + closures/cells, `nonlocal`) is deliberately NOT lowered here
// yet — those nodes fail with a "pending 2B function-ABI sub-goal" message
// instead of the generic pre-integration failures.
// while preserving the M1 output structure
// byte-for-byte for M1 programs:
//   #include "pentest_rt.h"
//   static pt_value pt_g_<name>;                 (one per Program.globals)
//   <forward declarations of ALL functions>
//   <function definitions, declaration order>
//   static void pt_main_block(void)              (if Program.has_main)
//   int main(int argc, char** argv)              (top-level flow in source order)
//
// Expressions are A-normalized: every effectful subexpression (variable read
// with its used-before-assignment guard, operator, call) is emitted as its
// own statement in source order, so side effects (print inside nested
// expressions, guards) always happen left-to-right like the Python-ish
// surface language implies, regardless of C's unspecified evaluation order.
//
// Conditions (if/elif headers, while headers) additionally get a purely
// inline emission path (Emitter::try_inline): a tree is inlined as a single
// C expression only when every call node ends up with at most one effectful
// child, which keeps evaluation order deterministic and lets headers take
// the literal contract forms `if/else if (pt_truthy(cond, line))` and
// `while (pt_truthy(cond, line))`. Trees that cannot be inlined fall back to
// A-normalized temporaries placed in the lazy position: before the `if` for
// the first condition, inside the `else { ... }` block for a non-inlineable
// `elif` (forcing an inner `if`), and inside `for (;;) { ... }` for a while
// whose header cannot be written as one expression (condition statements
// must be re-evaluated every iteration, so they live inside the loop and a
// `if (!pt_truthy(...)) break;` pre-test replaces the C header).
//
// Subexpressions that must stay lazy inside a single expression (ternary
// arms, the right operand of and/or) use try_inline with a GNU statement
// expression `({ ... })` fallback — the only way to keep a statement
// sequence lazy in expression position. It is only emitted when a lazy
// child cannot be written as one expression; clang (the contract compiler,
// §11) accepts it in -std=c11 mode.
//
// The program must already have passed semantic_check, so FuncDecl.c_name,
// FuncDecl.locals, Var/Assign/For `res`, Call.is_builtin_print and
// Call.is_builtin_range are populated, `break`/`continue` are loop-checked
// and case labels are constants.
//
// Generated runtime calls all receive the Pentest source line of their AST
// node (pt_eq is the M1 total operator and takes no line; pt_guard takes the
// Var/compound-target line).

#include "codegen.h"

#include "semantic.h"  // M2-2C §2.2: is_builtin_exception_class
#include "builtins.h"  // SPEC §5.11.5: the Python-parity builtin table

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pentest {
namespace {

[[noreturn]] void fail(const std::string& msg) { throw std::runtime_error(msg); }

// Escapes raw bytes for a C string literal (or the pt_init path):
//  - '"' and '\\' are backslash-escaped;
//  - the first '?' of any "??" pair becomes \077 so no trigraph can form
//    (clang performs trigraph conversion in -std=c11 mode, which would both
//    corrupt the bytes and produce -Wtrigraphs warnings);
//  - every other byte outside printable ASCII becomes a fixed 3-digit octal
//    escape. Fixed width matters: octal escapes consume at most 3 digits, so
//    "\000" followed by '2' cannot merge, and NUL bytes survive.
std::string c_escape(const std::string& s) {
  std::string r;
  r.reserve(s.size() + 8);
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '?' && i + 1 < s.size() && s[i + 1] == '?') {
      r += "\\077";
    } else if (c == '"') {
      r += "\\\"";
    } else if (c == '\\') {
      r += "\\\\";
    } else if (c >= 0x20 && c <= 0x7e) {
      r += static_cast<char>(c);
    } else {
      char buf[5];
      std::snprintf(buf, sizeof(buf), "\\%03o", c);
      r += buf;
    }
  }
  return r;
}

std::string int_literal(long long v) {
  if (v == INT64_MIN) return "pt_int(INT64_MIN)";
  return "pt_int(" + std::to_string(v) + "LL)";
}

// Emits a double literal that strtod's back to the identical value: shortest
// of %.15g/%.16g/%.17g that round-trips, forced to stay a double literal.
std::string float_literal(double d) {
  if (std::isnan(d)) return "pt_float(__builtin_nan(\"\"))";
  if (std::isinf(d)) return d < 0 ? "pt_float(-__builtin_inf())" : "pt_float(__builtin_inf())";
  char buf[64];
  for (int prec = 15; prec <= 17; ++prec) {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, d);
    if (std::strtod(buf, nullptr) == d) break;
  }
  std::string s(buf);
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
      s.find('E') == std::string::npos) {
    s += ".0";
  }
  return "pt_float(" + s + ")";
}

// RAW C literals for the fused SIMD kernels (the pt_int/pt_float forms
// above box into pt_value; vector lanes need the unwrapped text).
std::string int_literal_raw(long long v) { return std::to_string(v) + "LL"; }
std::string float_literal_raw(double d) {
  if (std::isnan(d)) return "__builtin_nan(\"\")";
  if (std::isinf(d)) return d < 0 ? "-__builtin_inf()" : "__builtin_inf()";
  char buf[64];
  for (int prec = 15; prec <= 17; ++prec) {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, d);
    if (std::strtod(buf, nullptr) == d) break;
  }
  std::string s(buf);
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
      s.find('E') == std::string::npos)
    s += ".0";
  return s;
}

// Runtime function for a binary op (M1 + M2-2A §3). `*total` is set for the
// ops that never fail and therefore take no line argument (M1 == / !=);
// `*contains` is set for "in" (bool-returning pt_contains).
const char* binary_fn(const std::string& op, bool* total, bool* contains) {
  *total = false;
  *contains = false;
  /* The numeric/comparison ops emit the header's static-inline _f fast
   * paths (see pentest_rt.h): identical semantics via the same tag checks
   * with an out-of-line fallback, but no call on the numeric hot path —
   * an opaque call in a loop body blocks LLVM's register promotion. */
  if (op == "+") return "pt_add_f";
  if (op == "-") return "pt_sub_f";
  if (op == "*") return "pt_mul_f";
  if (op == "/") return "pt_div_f";
  if (op == "%") return "pt_mod_f";
  if (op == "<") return "pt_lt_f";
  if (op == "<=") return "pt_le_f";
  if (op == ">") return "pt_gt_f";
  if (op == ">=") return "pt_ge_f";
  if (op == "==") { *total = true; return "pt_eq_f"; }
  if (op == "!=") { *total = true; return "pt_ne_f"; }
  if (op == "**") return "pt_pow";      // M2-2A §3.1
  if (op == "&") return "pt_band";      // M2-2A §3.2
  if (op == "|") return "pt_bor";
  if (op == "^") return "pt_bxor";
  if (op == "<<") return "pt_shl";
  if (op == ">>") return "pt_shr";
  if (op == "in") { *contains = true; return "pt_contains"; }  // §3.4
  fail("unknown binary operator '" + op + "'");
}

// C expression text for `<l> <op> <r>` (operands already emitted).
std::string binary_text(const std::string& op, const std::string& l,
                        const std::string& r, int line) {
  bool total = false;
  bool contains = false;
  std::string ln = std::to_string(line);
  // M2-2C §3.4: `is`/`is not` — object identity (never folded: heap
  // identity and text storage sharing are runtime facts).
  if (op == "is") return "pt_bool(pt_identical(" + l + ", " + r + ", " + ln + "))";
  if (op == "is not") return "pt_bool(!pt_identical(" + l + ", " + r + ", " + ln + "))";
  const char* fn = binary_fn(op, &total, &contains);
  if (contains) return "pt_bool(pt_contains(" + l + ", " + r + ", " + ln + "))";
  std::string s = std::string(fn) + "(" + l + ", " + r;
  if (!total) s += ", " + ln;
  s += ")";
  return s;
}

// C expression text for `pt_range3(...)` from 1..3 evaluated argument
// expressions (defaults: start=0, step=1; M2-2A §6).
std::string range3_text(const std::vector<std::string>& a, int line) {
  std::string r0, r1, r2;
  if (a.size() == 1) {
    r0 = "pt_int(0)";
    r1 = a[0];
    r2 = "pt_int(1)";
  } else if (a.size() == 2) {
    r0 = a[0];
    r1 = a[1];
    r2 = "pt_int(1)";
  } else if (a.size() == 3) {
    r0 = a[0];
    r1 = a[1];
    r2 = a[2];
  } else {
    fail("range() expects 1 to 3 arguments, got " + std::to_string(a.size()));
  }
  return "pt_range3(" + r0 + ", " + r1 + ", " + r2 + ", " +
         std::to_string(line) + ")";
}

// ---- C1: compile-time `range()` bounds ------------------------------------
// An integer literal, optionally sign-folded (`-5` parses as unary minus over
// a literal — parser.cpp's `unary` production). Anything it cannot reproduce
// EXACTLY (a non-literal operand, `-INT64_MIN`) returns false, so the fold
// stays conservative by one slot (optimization.md §0.3).
static bool const_int_lit(const Expr* e, int64_t& v) {
  if (!e) return false;
  if (e->kind == ExprKind::IntLit) {
    v = e->ival;
    return true;
  }
  if (e->kind == ExprKind::Unary && e->op == "-" && e->lhs &&
      e->lhs->kind == ExprKind::IntLit) {
    if (e->lhs->ival == INT64_MIN) return false;  // -(2^63) is not an int64
    v = -e->lhs->ival;
    return true;
  }
  return false;
}

// ---- C2 (task-5): static call-argument -> parameter-slot binding ----------
// Shared by BOTH sizes of the twin pair (int_shape's Call admission rule and
// native_int_text's emitted `f_i(...)` text) and by the never-raise proof,
// so admission, emission and proof can never drift apart (the file calls
// drift an internal failure, never a miscompile).
//
// `out[i]` = the expression that fills parameter i: a source argument, or
// the DECLARATION's default when the slot is omitted (defaults in a twin
// candidate are compile-time int literals — see twin_prefilter — so folding
// them into the call is observationally identical: a literal has no
// effects). Binding follows the §11 rules the semantic pass already
// validated for static calls (positional args fill left to right, named
// args bind by name, no duplicates / unknown names / over-flow); anything
// unbindable returns false and the caller falls back to the boxed path
// (conservative by one call, optimization.md §0.3).
//
// EVALUATION ORDER (byte-parity): C leaves the order of call-argument
// evaluation unspecified, so emitting the bound texts in PARAM order must
// reproduce SOURCE order — hence the strictly-increasing param-index rule
// over the original argument sequence. A call whose named args would
// reorder evaluation of effectful subexpressions declines binding.
static bool bind_call_args(const Expr& e, const FuncDecl& f,
                           std::vector<const Expr*>& out) {
  const size_t np = f.params.size();
  if (e.args.size() > np) return false;  // extras need *args/**kwargs
  out.assign(np, nullptr);
  std::vector<char> filled(np, 0);
  long long last = -1;
  bool seen_named = false;
  for (size_t i = 0; i < e.args.size(); ++i) {
    if (!e.args[i]) return false;
    const std::string an = i < e.arg_names.size() ? e.arg_names[i] : "";
    size_t idx;
    if (an.empty()) {
      if (seen_named) return false;  // positional after named (§11 order)
      idx = i;  // positional args are the leading ones
    } else {
      seen_named = true;
      idx = np;
      for (size_t j = 0; j < np; ++j)
        if (f.params[j] == an) {
          idx = j;
          break;
        }
      if (idx == np) return false;  // unknown name (or a **kwargs callee)
    }
    if (idx >= np) return false;
    if (filled[idx]) return false;                    // duplicate binding
    if (static_cast<long long>(idx) <= last) return false;  // would reorder
    filled[idx] = 1;
    out[idx] = e.args[i].get();
    last = static_cast<long long>(idx);
  }
  // Omitted slots take the declaration default; a slot with no default is a
  // missing required argument (semantic rejects those for static calls).
  for (size_t j = 0; j < np; ++j) {
    if (filled[j]) continue;
    if (j >= f.defaults.size() || !f.defaults[j]) return false;
    out[j] = f.defaults[j].get();
  }
  return true;
}

// ---- C3 (task-5 A): bulk call-site argument binding ----------------------
// The bulk-callee sibling of bind_call_args: the FIXED params bind through
// exactly the same §11 rules (positional fill left to right, named args bind
// by name, strictly-increasing param index so PARAM-order emission ==
// SOURCE-order evaluation, no duplicates / unknown names), omitted slots
// take their declaration default — but ONLY a compile-time int literal
// (same fold rule as twin_prefilter: a literal has no effects, so
// evaluating it at the site vs. at entry is observationally identical;
// anything else would move an effectful evaluation across the argument
// sequence and declines). Every argument LEFT OVER past the fixed params is
// the positional bulk tail that fills *more in source order; a tail start is
// only legal once ALL fixed params are filled positionally (any named arg
// at-or-after tail position would reorder evaluation), and a named binding
// to the vararg name itself is rejected (the unpack-by-name form keeps its
// boxed path). Anything unbindable returns false → boxed entry (§0.3).
static bool bind_bulk_call_args(const Expr& e, const FuncDecl& f,
                               std::vector<const Expr*>& fixed,
                               std::vector<const Expr*>& tail) {
  fixed.clear();
  tail.clear();
  if (!f.has_vararg || f.has_kwarg) return false;
  const size_t np = f.params.size();
  std::vector<const Expr*> filled(np, nullptr);
  std::vector<char> taken(np, 0);
  long long last = -1;
  bool seen_named = false;
  size_t npos = 0;
  for (size_t i = 0; i < e.args.size(); ++i) {
    if (!e.args[i]) return false;
    const std::string an = i < e.arg_names.size() ? e.arg_names[i] : "";
    size_t idx;
    if (an.empty()) {
      if (seen_named) return false;  // positional after named (§11 order)
      idx = npos++;
      if (idx >= np) {               // first overflow arg = bulk tail start
        if (idx != np + tail.size()) return false;  // contiguous (npos)
        tail.push_back(e.args[i].get());
        continue;
      }
    } else {
      seen_named = true;
      idx = np;
      for (size_t j = 0; j < np; ++j)
        if (f.params[j] == an) {
          idx = j;
          break;
        }
      if (idx == np) return false;  // unknown name (**kwargs / vararg name)
    }
    if (filled[idx]) return false;                      // duplicate binding
    if (static_cast<long long>(idx) <= last) return false;  // would reorder
    taken[idx] = 1;
    filled[idx] = e.args[i].get();
    last = static_cast<long long>(idx);
  }
  fixed.resize(np);
  for (size_t j = 0; j < np; ++j) {
    if (taken[j]) {
      fixed[j] = filled[j];
      continue;
    }
    if (j >= f.defaults.size() || !f.defaults[j]) return false;
    int64_t dv = 0;
    if (!const_int_lit(f.defaults[j].get(), dv)) return false;  // fold rule
    fixed[j] = f.defaults[j].get();
  }
  return true;
}

// Proof-shaped facts behind folding a `range(...)` header: true ONLY when
// every effective bound is a compile-time int literal and the step is a
// non-zero literal — then pt_range3's validation cannot fire (§6.9: all
// three must be int, so no TypeError; step != 0, so no ValueError) and the
// call would just build a stack value, so skipping it is behavior-preserving
// at execution time AND evaluation order (nothing left to evaluate).
static bool const_range_bounds(const Expr& e, int64_t& st, int64_t& so,
                               int64_t& sp) {
  if (e.kind != ExprKind::Call || !e.is_builtin_range) return false;
  if (e.args.empty() || e.args.size() > 3) return false;  // arity is inert
  for (const std::string& an : e.arg_names)
    if (!an.empty()) return false;  // named binding stays on the dynamic path
  int64_t a0 = 0, a1 = 0, a2 = 1;
  if (!const_int_lit(e.args[0].get(), a0)) return false;
  if (e.args.size() >= 2 && !const_int_lit(e.args[1].get(), a1)) return false;
  if (e.args.size() >= 3 && !const_int_lit(e.args[2].get(), a2)) return false;
  if (e.args.size() == 1) {
    st = 0; so = a0; sp = 1;
  } else if (e.args.size() == 2) {
    st = a0; so = a1; sp = 1;
  } else {
    st = a0; so = a1; sp = a2;
    if (sp == 0) return false;  // `range() arg 3 must not be zero` must fire
  }
  return true;
}

// Raw int64 C constant for a folded bound (INT64_MIN needs the macro: the
// decimal literal is not representable as one C token).
static std::string cint64(int64_t v) {
  return v == INT64_MIN ? std::string("INT64_MIN")
                        : std::to_string(v) + "LL";
}

// C expression text for an inline print: the void-returning pt_print folded
// into a value via the comma operator. The compound literal is block-scoped
// and consumed by the call within the same full expression.
std::string print_inline_text(const std::vector<std::string>& a, int line) {
  std::string s = "(pt_print(";
  if (a.empty()) {
    s += "NULL, 0";
  } else {
    s += "(pt_value[]){";
    for (size_t i = 0; i < a.size(); ++i) {
      if (i) s += ",";
      s += " " + a[i];
    }
    s += " }, " + std::to_string(a.size());
  }
  s += ", " + std::to_string(line) + "), pt_none())";
  return s;
}

// Runtime function for a compound-assignment base operator (M2-2A §3.3,
// M2-2B §10 for index targets); nullptr for anything else.
const char* compound_fn(const std::string& op) {
  if (op == "+=") return "pt_add_f";
  if (op == "-=") return "pt_sub_f";
  if (op == "*=") return "pt_mul_f";
  if (op == "/=") return "pt_div_f";
  if (op == "%=") return "pt_mod_f";
  if (op == "**=") return "pt_pow";
  if (op == "&=") return "pt_band";
  if (op == "|=") return "pt_bor";
  if (op == "^=") return "pt_bxor";
  if (op == "<<=") return "pt_shl";
  if (op == ">>=") return "pt_shr";
  return nullptr;
}

// `(pt_value[]){ a, b, ... }` from already-emitted element expressions.
// Callers A-normalize every effectful element into its own temp first, so
// the initializer performs plain reads only and no C evaluation-order
// question can arise inside it.
std::string value_array_text(const std::vector<std::string>& a) {
  std::string s = "(pt_value[]){";
  for (size_t i = 0; i < a.size(); ++i) {
    if (i) s += ",";
    s += " " + a[i];
  }
  s += " }";
  return s;
}

// Lowered form of a reserved builtin call other than print/range moved into
// the Emitter class (it needs the program/function context for the
// globals/locals/vars/dir name tables) — see Emitter::builtin_call_text.

// ---- M2-2B §13 uniform function ABI helpers -----------------------------
//
// Every generated function is `pt_value f(pt_value* bound, pt_cell** env)`.
// The bound vector holds one slot per positional param, then the *vararg
// slot (list), then the **kwarg slot (map); omitted slots arrive as
// PT_MISSING and are initialized by the callee's entry code.

// All bound-vector slot names in ABI order: positional params, then the
// *vararg name, then the **kwarg name (M2-2B §6 parameter order). Also the
// param_names order of the function's pt_funmeta.
std::vector<std::string> slot_names(const FuncDecl& f) {
  std::vector<std::string> ps = f.params;
  if (f.has_vararg) ps.push_back(f.vararg);
  if (f.has_kwarg) ps.push_back(f.kwarg);
  return ps;
}

size_t n_slots(const FuncDecl& f) {
  return f.params.size() + (f.has_vararg ? 1u : 0u) + (f.has_kwarg ? 1u : 0u);
}

// Per-function metadata instance name (unique: derived from the unique c_name).
std::string meta_name(const FuncDecl& f) { return "pt_meta_" + f.c_name; }

// Binds a static call's already-emitted argument texts into the callee's
// bound-vector slots (M2-2B §6/§13): positional arguments fill slots
// left-to-right — including over defaulted slots, before overflowing into
// the vararg list (Python order) — named arguments fill their slot by
// name, an unfilled defaulted slot passes PT_MISSING (the callee evaluates
// the default at entry, fresh per call), the vararg slot receives a fresh
// list of the positional arguments beyond the parameter count (or
// PT_MISSING when there are none — the callee initializes it empty), and
// the kwarg slot a fresh map of the named arguments that matched no
// parameter (likewise PT_MISSING when empty). `a` holds the
// ALREADY-EMITTED argument texts in source order (side effects happened
// exactly once, left-to-right); texts are only re-placed into slots,
// never re-evaluated. Returns one C expression per slot.
std::vector<std::string> bound_slots_text(const FuncDecl& f, const Expr& call,
                                          const std::vector<std::string>& a,
                                          int line) {
  size_t nparams = f.params.size();
  std::vector<std::string> positional;
  std::vector<std::pair<std::string, std::string>> named;  // name -> text
  positional.reserve(a.size());
  named.reserve(a.size());
  for (size_t i = 0; i < a.size(); ++i) {
    std::string n = i < call.arg_names.size() ? call.arg_names[i] : "";
    if (n.empty()) positional.push_back(a[i]);
    else named.push_back(std::make_pair(n, a[i]));
  }
  std::vector<std::string> slots(n_slots(f));
  std::vector<bool> filled(nparams, false);
  for (size_t p = 0; p < nparams && p < positional.size(); ++p) {
    slots[p] = positional[p];
    filled[p] = true;
  }
  for (const auto& nv : named) {
    if (f.has_vararg && nv.first == f.vararg)
      fail("internal: vararg name '" + nv.first + "' not namable (line " +
           std::to_string(line) + ")");
    if (f.has_kwarg && nv.first == f.kwarg)
      fail("internal: kwarg name '" + nv.first + "' not namable (line " +
           std::to_string(line) + ")");
    for (size_t p = 0; p < nparams; ++p) {
      if (f.params[p] == nv.first && !filled[p]) {
        slots[p] = nv.second;
        filled[p] = true;
        break;
      }
    }
  }
  for (size_t p = 0; p < nparams; ++p) {
    if (filled[p]) continue;
    bool has_default = p < f.defaults.size() && f.defaults[p];
    if (!has_default)
      fail("static call to '" + f.name + "' left required parameter '" +
           f.params[p] + "' unbound (line " + std::to_string(line) + ")");
    slots[p] = "pt_missing()";  // callee evaluates the default at entry
  }
  if (f.has_vararg) {
    std::vector<std::string>::const_iterator start =
        positional.size() > nparams ? positional.cbegin() + nparams
                                    : positional.cend();
    std::vector<std::string> extra(start, positional.cend());
    if (extra.empty()) {
      slots[nparams] = "pt_missing()";  // callee builds the empty list
    } else {
      slots[nparams] = "pt_list_of(" + value_array_text(extra) + ", " +
                       std::to_string(extra.size()) + "u)";
    }
  }
  if (f.has_kwarg) {
    std::vector<std::string> ks, vs;
    for (const auto& nv : named) {
      bool is_param = false;
      for (const std::string& p : f.params)
        if (p == nv.first) { is_param = true; break; }
      if (is_param) continue;
      ks.push_back("PT_STR(\"" + c_escape(nv.first) + "\")");
      vs.push_back(nv.second);
    }
    if (ks.empty()) {
      slots[n_slots(f) - 1] = "pt_missing()";  // callee builds the empty map
    } else {
      slots[n_slots(f) - 1] =
          "pt_map_of(" + value_array_text(ks) + ", " + value_array_text(vs) +
          ", " + std::to_string(ks.size()) + "u, " + std::to_string(line) + ")";
    }
  }
  return slots;
}

// True when evaluating `e` can perform an effect (a guard that can exit, a
// call, an operator that can raise a runtime error). Used for the inline
// safety rule: a C call node may have at most one effectful child, because
// C leaves argument evaluation order unspecified and Pentest requires
// left-to-right order (M2-2A §4 short-circuiting, M1 §10 line passing).
bool has_effect(const Expr& e) {
  switch (e.kind) {
    case ExprKind::Var:
      return true;  // used-before-assignment guard can exit
    case ExprKind::VarQual:  // docs/reference/cross-scope-access.md: same guard on the
                             // pinned slot
      return true;
    case ExprKind::Call:
      return true;  // print / user call / range() validation
    case ExprKind::Unary:
      if (e.op == "not") return e.lhs && has_effect(*e.lhs);  // pt_truthy is total
      return true;  // "-" and "~" can raise a type error
    case ExprKind::Binary:
      if (e.op == "==" || e.op == "!=")  // total ops: only the children matter
        return (e.lhs && has_effect(*e.lhs)) || (e.rhs && has_effect(*e.rhs));
      return true;  // every other binary op can raise a runtime error
    case ExprKind::Logical:
      return (e.lhs && has_effect(*e.lhs)) || (e.rhs && has_effect(*e.rhs));
    case ExprKind::Ternary:
      return (e.lhs && has_effect(*e.lhs)) || (e.rhs && has_effect(*e.rhs)) ||
             (e.third && has_effect(*e.third));
    // M2-2B: list/tuple/can-join construction never fails — only the child
    // expressions decide. Set/map construction can raise (`unhashable
    // type`), and indexing/slicing/method dispatch raise on their own, so
    // those are effectful regardless of their children. Bytes literals are
    // pure like every other literal.
    case ExprKind::ListLit:
    case ExprKind::TupleLit:
    case ExprKind::Join: {
      for (const auto& el : e.elems)
        if (el && has_effect(*el)) return true;
      return false;
    }
    case ExprKind::SetLit:
    case ExprKind::MapLit:
    case ExprKind::Index:
    case ExprKind::Slice:
    case ExprKind::MethodCall:
    case ExprKind::Attr:  // M2-2C §3.2: an attribute read can raise
      return true;
    default:
      return false;  // literals (incl. BytesLit)
  }
}

// Collects every variable READ (ExprKind::Var) in a statement/expression
// tree, keyed by (resolution, name). Assignment targets are writes and do
// not count — except compound assignments (`x += 1`), which read the target
// first. Used to decide which params/locals need a `((void)x);` to stay
// warning-clean under -Wall -Wextra (-Wunused-parameter /
// -Wunused-but-set-variable).
void collect_var_reads(const Expr& e, std::set<std::pair<VarRes, std::string>>& out);

// Assignment-target trees (M2-2B §3): an Index target reads its object and
// index expressions; nested patterns recurse; Name/Rest leaves are writes.
void collect_target_reads(const AssignTarget& t,
                          std::set<std::pair<VarRes, std::string>>& out) {
  if (t.object) collect_var_reads(*t.object, out);
  if (t.index) collect_var_reads(*t.index, out);
  for (const AssignTarget& sub : t.elems) collect_target_reads(sub, out);
}

void collect_var_reads(const Stmt& s, std::set<std::pair<VarRes, std::string>>& out) {
  if (s.expr) collect_var_reads(*s.expr, out);
  if (s.kind == StmtKind::Assign && !s.op.empty() && s.res != VarRes::Unresolved)
    out.insert(std::make_pair(s.res, s.name));  // compound assign reads target
  if (s.kind == StmtKind::Assign) collect_target_reads(s.target, out);
  for (const Stmt& b : s.body) collect_var_reads(b, out);
  for (const auto& c : s.conds)
    if (c) collect_var_reads(*c, out);
  for (const auto& b : s.bodies)
    for (const Stmt& st : b) collect_var_reads(st, out);
  for (const Stmt& st : s.else_body) collect_var_reads(st, out);
  for (const CaseArm& arm : s.cases)
    for (const Stmt& st : arm.body) collect_var_reads(st, out);
}

void collect_var_reads(const Expr& e, std::set<std::pair<VarRes, std::string>>& out) {
  switch (e.kind) {
    case ExprKind::Var:
      out.insert(std::make_pair(e.res, e.name));
      break;
    case ExprKind::VarQual:
      break;  // reads another function's (or a global's) slot — not ours
    case ExprKind::Unary:
      if (e.lhs) collect_var_reads(*e.lhs, out);
      break;
    case ExprKind::Binary:
      if (e.lhs) collect_var_reads(*e.lhs, out);
      if (e.rhs) collect_var_reads(*e.rhs, out);
      break;
    case ExprKind::Logical:
      if (e.lhs) collect_var_reads(*e.lhs, out);
      if (e.rhs) collect_var_reads(*e.rhs, out);
      break;
    case ExprKind::Ternary:
      if (e.lhs) collect_var_reads(*e.lhs, out);
      if (e.rhs) collect_var_reads(*e.rhs, out);
      if (e.third) collect_var_reads(*e.third, out);
      break;
    // M2-2B nodes: callee load (indirect calls), element/key lists, index
    // and slice operands, method object + args.
    case ExprKind::ListLit:
    case ExprKind::SetLit:
    case ExprKind::TupleLit:
    case ExprKind::Join:
    case ExprKind::MethodCall:
      if (e.lhs) collect_var_reads(*e.lhs, out);
      for (const auto& el : e.elems)
        if (el) collect_var_reads(*el, out);
      break;
    case ExprKind::MapLit:
      for (const auto& k : e.keys)
        if (k) collect_var_reads(*k, out);
      for (const auto& el : e.elems)
        if (el) collect_var_reads(*el, out);
      break;
    case ExprKind::Index:
      if (e.lhs) collect_var_reads(*e.lhs, out);
      if (e.rhs) collect_var_reads(*e.rhs, out);
      break;
    case ExprKind::Slice:
      if (e.lhs) collect_var_reads(*e.lhs, out);
      if (e.rhs) collect_var_reads(*e.rhs, out);
      if (e.third) collect_var_reads(*e.third, out);
      if (e.fourth) collect_var_reads(*e.fourth, out);
      break;
    case ExprKind::Call:
      if (e.callee_expr) collect_var_reads(*e.callee_expr, out);
      for (const auto& a : e.args) {
        if (a) collect_var_reads(*a, out);
      }
      break;
    case ExprKind::Attr:  // M2-2C §3.2: reads the object expression
      if (e.lhs) collect_var_reads(*e.lhs, out);
      break;
    default:
      break;
  }
}

// ---- main-only native globals: promote to automatic locals --------------
//
// A native int/float "global" that is referenced ONLY by the main block can
// live in an automatic local of pt_main_block instead of file-scope static
// storage. The main block runs exactly once and no other region can name
// the slot, so storage duration and visibility are unobservable; the win is
// that the C compiler can keep the value in a register across the loop
// (clang otherwise reloads/stores file-scope statics around the lane's
// value-conversion/error helpers). The boxed pt_g_* slot still exists as a
// GC root and stays unset, exactly as for every native global today.
//
// References are collected per region with the same AST coverage as
// collect_var_reads (plus write targets, defaults, nested functions, class
// methods and except bindings). `globals()/locals()/vars()/dir()` can
// observe a global by name through the name-slot table, so any such call
// anywhere declines the whole promotion (conservative, no reflection).
struct GlobalRefScan {
  std::set<std::string> refs;    // every read/write/declaration mention
  std::set<std::string> writes;  // bindings performed in this region
  bool dynamic_names = false;    // globals/locals/vars/dir anywhere
};

static void scan_target_refs(const AssignTarget& t, GlobalRefScan& out);

static void scan_expr_refs(const Expr& e, GlobalRefScan& out) {
  switch (e.kind) {
    case ExprKind::Var:
      if (e.res == VarRes::Global) out.refs.insert(e.name);
      return;
    case ExprKind::VarQual:
      return;  // cross-scope: pinned slots are never native candidates
    case ExprKind::Unary:
      if (e.lhs) scan_expr_refs(*e.lhs, out);
      return;
    case ExprKind::Binary:
    case ExprKind::Logical:
      if (e.lhs) scan_expr_refs(*e.lhs, out);
      if (e.rhs) scan_expr_refs(*e.rhs, out);
      return;
    case ExprKind::Ternary:
      if (e.lhs) scan_expr_refs(*e.lhs, out);
      if (e.rhs) scan_expr_refs(*e.rhs, out);
      if (e.third) scan_expr_refs(*e.third, out);
      return;
    case ExprKind::Index:
      if (e.lhs) scan_expr_refs(*e.lhs, out);
      if (e.rhs) scan_expr_refs(*e.rhs, out);
      return;
    case ExprKind::Slice:
      if (e.lhs) scan_expr_refs(*e.lhs, out);
      if (e.rhs) scan_expr_refs(*e.rhs, out);
      if (e.third) scan_expr_refs(*e.third, out);
      if (e.fourth) scan_expr_refs(*e.fourth, out);
      return;
    case ExprKind::Call:
      if (e.is_builtin &&
          (e.builtin_name == "globals" || e.builtin_name == "locals" ||
           e.builtin_name == "vars" || e.builtin_name == "dir"))
        out.dynamic_names = true;
      if (e.callee_expr) scan_expr_refs(*e.callee_expr, out);
      for (const auto& a : e.args)
        if (a) scan_expr_refs(*a, out);
      return;
    case ExprKind::Attr:
      if (e.lhs) scan_expr_refs(*e.lhs, out);
      return;
    case ExprKind::Join:
    case ExprKind::MethodCall:
    case ExprKind::ListLit:
    case ExprKind::SetLit:
    case ExprKind::TupleLit:
      if (e.lhs) scan_expr_refs(*e.lhs, out);
      for (const auto& el : e.elems)
        if (el) scan_expr_refs(*el, out);
      return;
    case ExprKind::MapLit:
      for (const auto& k : e.keys)
        if (k) scan_expr_refs(*k, out);
      for (const auto& el : e.elems)
        if (el) scan_expr_refs(*el, out);
      return;
    default:
      return;  // literals
  }
}

static void scan_target_refs(const AssignTarget& t, GlobalRefScan& out) {
  if (t.kind == AssignTarget::Kind::Index ||
      t.kind == AssignTarget::Kind::Attr) {
    if (t.object) scan_expr_refs(*t.object, out);
    if (t.index) scan_expr_refs(*t.index, out);
    return;
  }
  if ((t.kind == AssignTarget::Kind::Name ||
       t.kind == AssignTarget::Kind::Rest) &&
      t.res == VarRes::Global) {
    out.refs.insert(t.name);
    out.writes.insert(t.name);
  }
  for (const AssignTarget& sub : t.elems) scan_target_refs(sub, out);
}

static void scan_stmt_refs(const Stmt& s, GlobalRefScan& out) {
  if (s.expr) scan_expr_refs(*s.expr, out);
  if (s.kind == StmtKind::Assign) scan_target_refs(s.target, out);
  if (s.kind == StmtKind::For && s.res == VarRes::Global) {
    out.refs.insert(s.name);
    out.writes.insert(s.name);
  }
  if (s.kind == StmtKind::Class) {
    /* the class statement binds a module global at its execution point */
    out.refs.insert(s.name);
    out.writes.insert(s.name);
  }
  if (s.kind == StmtKind::GlobalDecl)
    for (const std::string& n : s.names) out.refs.insert(n);
  for (const Stmt& b : s.body) scan_stmt_refs(b, out);
  for (const auto& c : s.conds)
    if (c) scan_expr_refs(*c, out);
  for (const auto& b : s.bodies)
    for (const Stmt& st : b) scan_stmt_refs(st, out);
  for (const Stmt& st : s.else_body) scan_stmt_refs(st, out);
  for (const CaseArm& arm : s.cases)
    for (const Stmt& st : arm.body) scan_stmt_refs(st, out);
  for (const ExceptArm& arm : s.arms) {
    if (arm.cond) scan_expr_refs(*arm.cond, out);
    if (!arm.name.empty() && arm.res == VarRes::Global) {
      out.refs.insert(arm.name);
      out.writes.insert(arm.name);
    }
    for (const Stmt& st : arm.body) scan_stmt_refs(st, out);
  }
  for (const Stmt& st : s.finally_body) scan_stmt_refs(st, out);
  if (s.func) {
    for (const auto& d : s.func->defaults)
      if (d) scan_expr_refs(*d, out);
    for (const Stmt& st : s.func->body) scan_stmt_refs(st, out);
  }
  for (const FuncDecl& m : s.class_methods) {
    for (const auto& d : m.defaults)
      if (d) scan_expr_refs(*d, out);
    for (const Stmt& st : m.body) scan_stmt_refs(st, out);
  }
}

// Post-2C perf: scan results for the native int-lane twin analysis
// (declared before Emitter for the member declaration; defined with the
// analysis below).
struct TwinScan;

class Emitter {
 public:
  explicit Emitter(const Program& p) : prog_(p) {}
  std::string run();

 private:
  const Program& prog_;
  std::string out_;
  long temps_ = 0;      // monotonic for the whole file: names unique per C function
  long ivars_ = 0;      // monotonic for-range induction variable indices
  int indent_ = 0;      // current statement indentation (spaces)
  int loop_depth_ = 0;  // enclosing Pentest loops (break/continue legality)

  // M2-2C §2.5/§2.6: try-statement bookkeeping for exit edges. Each active
  // try owns a C scope with setjmp landing pads; `break`/`continue`/`return`
  // crossing it leave the handlers it still has entered (innermost first)
  // and run its finally body once, before the jump (§2.6).
  struct TryCtx {
    std::vector<std::string> entered;   // handler vars entered, innermost
                                        // first (mutated while emitting arm
                                        // and finally bodies)
    const Stmt* finally_src = nullptr;  // the source finally body; null once
                                        // that body itself is being emitted
    int loop_depth = 0;                 // loop_depth_ when the try was seen
  };
  std::vector<TryCtx> try_stack_;
  size_t try_seq_ = 0;  // unique handler/label suffixes (function-scoped)

  void line(const std::string& s) {
    out_ += s;
    out_ += '\n';
  }
  std::string ind() const { return std::string(static_cast<size_t>(indent_), ' '); }
  std::string fresh_temp() { return "pt_t" + std::to_string(temps_++); }
  std::string fresh_ivar() { return "pt_iv" + std::to_string(ivars_++); }

  // M2-2C §1.3(2): pt_value slots of the frame region currently being
  // emitted (hoisted temps; locals/params/cells are declared by the caller
  // around the region). decl_temp records; emit_framed declares them at the
  // top of the region and puts their addresses into the shadow frame.
  std::vector<std::string> slot_names_;
  bool framed_ = false;  // inside a shadow-framed region: `return` emits
                         // pt_frame_end(&pt_fr) before leaving
  // Temps created during the statement currently being emitted that have NOT
  // yet been cleared by a nested statement's own tail. decl_temp pushes to
  // BOTH slot_names_ (declaration + shadow frame: every temp in the region)
  // and pending_temps_ (per-statement clear bookkeeping). A nested emit_stmt
  // clears + truncates its own slice of pending_temps_ at its tail, so the
  // enclosing statement does NOT re-clear them (M2-2C §1.5 clears each temp
  // exactly once — the statement that created it). This keeps a redundant
  // `x = pt_unset()` out of hot loop bodies on the common (not-taken) path.
  std::vector<std::string> pending_temps_;

  std::string decl_temp() {
    std::string t = fresh_temp();
    slot_names_.push_back(t);
    pending_temps_.push_back(t);
    return t;
  }

  // Emits `body_fn` with every decl_temp'd slot hoisted into a declaration
  // block followed by the GC shadow frame (2C §1.3/§1.4). The captured text
  // lands after `pt_frame_begin`, so anything the body needs protected must
  // be rooted by the frame (or by the caller's own `pt_gc_protect` lines —
  // emit them as the first statements of `body_fn`, before its gc point).
  // `base_slots` are pt_value slots already declared by the caller (locals,
  // params, `&pt_cell_x->v`); they are referenced by the frame array but
  // not re-declared. frame_end adds pt_frame_end after the region (regions
  // whose `return` paths are emitted inside handle it themselves).
  void emit_framed(const std::function<void()>& body_fn, bool frame_end,
                   const std::vector<std::string>& base_slots = {}) {
    std::vector<std::string> saved_slots = slot_names_;
    bool saved_framed = framed_;
    slot_names_.clear();
    // Isolate the per-statement clear bookkeeping too: any temps still
    // pending (e.g. from a trailing `return`) belong to this region only.
    std::vector<std::string> saved_pending = pending_temps_;
    pending_temps_.clear();
    framed_ = true;
    std::string captured;
    std::string keep;
    keep.swap(out_);
    try {
      body_fn();
      captured.swap(out_);
      out_.swap(keep);
    } catch (...) {
      out_.swap(keep);  // restore the prefix; emission is aborting anyway
      slot_names_ = saved_slots;
      pending_temps_ = saved_pending;
      framed_ = saved_framed;
      throw;
    }
    for (const std::string& s : slot_names_)
      line(ind() + "pt_value " + s + " = pt_unset();");
    std::vector<std::string> all = base_slots;
    all.insert(all.end(), slot_names_.begin(), slot_names_.end());
    if (!all.empty()) {
      std::string arr = ind() + "pt_value* pt_slots[] = {";
      for (size_t i = 0; i < all.size(); ++i)
        arr += (i ? ", &" : " &") + all[i];
      arr += " };";
      line(arr);
    }
    line(ind() + "pt_frame pt_fr;");
    line(ind() + "pt_frame_begin(&pt_fr, " +
         (all.empty() ? std::string("NULL, 0u")
                      : "pt_slots, " + std::to_string(all.size()) + "u") +
         ");");
    out_ += captured;
    if (frame_end) line(ind() + "pt_frame_end(&pt_fr);");
    slot_names_ = saved_slots;
    pending_temps_ = saved_pending;
    framed_ = saved_framed;
  }

  std::vector<const FuncDecl*> all_functions() const;
  const FuncDecl* find_function(const std::string& name) const;
  const MainDecl* find_main() const;

  // The function currently being emitted (null at global/main scope).
  // Drives cell/capture storage resolution in storage_for.
  const FuncDecl* cur_func_ = nullptr;
  // Whether the current function's body/defaults/hoisting actually referenced
  // the env parameter (captured-name reads/writes or handed-down closure
  // slots); drives the `((void)env)` suppression.
  bool env_used_ = false;

  // M2-2B §6 helpers over cur_func_: own-binding lookup by NAME (the
  // semantic pass guarantees params/locals/captures are disjoint within a
  // function), the nested-decl lookup within the current function's body,
  // and the capture env-slot index.
  bool own_param_index(const std::string& name, size_t& idx) const;
  bool own_local_index(const std::string& name, size_t& idx) const;
  static bool slot_is_cell(const FuncDecl& f, size_t slot);
  long capture_index(const std::string& name) const;
  const FuncDecl* find_own_nested(const std::string& name) const;

  // ---- post-2C perf: native int64 slots (TODO.txt speed goal) ------------
  // analyze_native_ints() runs once before emission; see the block comment
  // above its definition for the proof rules. Native slots keep their
  // pt_value slot (frame/GC layout unchanged) but read/write a parallel
  // native int64_t + used-before-assignment flag.
  void analyze_native_ints();
  bool native_is_local(const std::string& name) const;
  std::string native_read_text(const std::string& name, VarRes res,
                               int line) const;
  bool native_slot_text(const std::string& name, VarRes res,
                        std::string& slot, std::string& flag) const;
  std::string native_int_text(const Expr& e) const;
  // Operand rendering for a context that only needs a boxed pt_value but
  // can compute it from the int lane: proven-int expressions are evaluated
  // natively and boxed ONCE (pt_int(...)), instead of going through the
  // generic boxed operator chain. Falls back to emit_expr for everything
  // else (exact evaluation order is the caller's existing temp order).
  std::string scalar_int_operand(const Expr& e, bool in_function);
  bool native_cond_text(const Expr& e, std::string& out) const;
  bool alloc_free_expr(const Expr& e) const;
  bool body_alloc_free(const std::vector<Stmt>& body) const;
  std::set<std::string> nativ_g_;  // program globals kept native int64
  std::map<const FuncDecl*, std::set<std::string>> nativ_l_;  // locals / fn
  // Native globals referenced only by the main block: their pt_ng_/pt_nfg_
  // pair is declared as an automatic local inside pt_main_block (file-scope
  // declaration skipped), so the C compiler can register-allocate it.
  void analyze_promotable_globals();
  std::set<std::string> local_g_;

  // ---- main-block use facts (int-list + lazy-slice analyses) --------------
  // Every global mention in the main block is classified once, with the
  // statement it sits in. Both optimizations below are restricted to
  // main-only globals (local_g_), so no function/top-level code can observe
  // the names they transform.
  enum class UseRole {
    Bare,        // read as a value / passed / stored / iterated
    IndexBase,   // xs[i] read
    SliceBase,   // xs[a:b] base
    LenArg,      // len(xs)
    MethodRecv,  // xs.append(...) receiver
    AttrBase,    // xs.attr read
    Write,       // plain name assignment
    IndexWrite,  // xs[i] = ... / xs[i] op= ...
    AttrWrite,   // xs.attr = ...
    WriteUnknown // unpack/for/except binding: value not a proven literal
  };
  struct MainUse {
    UseRole role;
    const Stmt* stmt;
    const Expr* expr;    // Index/Slice/MethodCall/Call/Attr node or RHS
    std::string method;  // MethodRecv
    std::string op;      // Write / IndexWrite
  };
  std::map<std::string, std::vector<MainUse>> main_uses_;
  std::map<const Stmt*, size_t> stmt_id_;       // pre-order (textual order)
  std::map<const Stmt*, size_t> stmt_pos_;      // index inside stmt_list_
  std::map<const Stmt*, const std::vector<Stmt>*> stmt_list_;
  size_t main_stmt_next_id_ = 0;
  const std::vector<Stmt>* scan_list_ = nullptr;
  bool main_has_try_ = false;
  void scan_main_uses();
  void scan_use_stmts(const std::vector<Stmt>& list);
  void scan_use_stmt(const Stmt& s);
  void scan_use_expr(const Expr& e, const Stmt* st);
  void scan_use_target(const AssignTarget& t, const Stmt* st,
                       bool unpack = false);

  // Names proven to hold only PT_INT elements for their whole lifetime.
  std::set<std::string> int_lists_;
  // Subset of int_lists_ whose object is proven flat (seq.stride == 0 for the
  // whole lifetime): literals, list(), .copy(). Slices are views, so they are
  // never flat even when they came from a flat list.
  std::set<std::string> flat_lists_;
  // Index nodes whose list base is definitely assigned at that program point
  // (so the native read can use the bare storage and cannot raise a guard).
  std::set<const Expr*> int_list_da_reads_;
  void analyze_int_list_reads();
  void il_da_stmts(const std::vector<Stmt>& list, std::set<std::string>& da);
  void il_da_stmt(const Stmt& s, std::set<std::string>& da);
  void il_da_expr(const Expr& e, std::set<std::string>& da);
  void il_da_target(const AssignTarget& t, std::set<std::string>& da);
  bool int_list_expr(const Expr& e, const std::set<std::string>& cur) const;
  bool int_list_method(const Expr& call,
                       const std::set<std::string>& cur) const;
  bool flat_init_expr(const Expr& e) const;
  bool analyze_int_lists();

  // ---- lazy (ephemeral) slices -------------------------------------------
  // A slice assigned to a main-only global and read ONLY as `target[idx]` in
  // the same straight-line statement list can stay materialized in bounds
  // form: pt_slice_bounds normalizes once, and each read addresses the
  // source directly. The target slot is never written; if any other read,
  // a second write, or a source mutation after the snapshot exists, the
  // candidate is rejected. Handlers are excluded wholesale because a
  // handler could otherwise observe the missing target.
  struct EphemSlice {
    std::string target;
    std::string src;
    std::string psrc;   // source global's rooted storage (pt_g_<name>)
    std::string items;  // stable view fields (source write-free after)
    std::string stride;
    std::string len;
    // Optional: derive this span from an earlier step-1 snapshot of the same
    // source whose raw bounds compare equal (then the normalized interval is
    // identical, so count = ceil(base.count / step)).
    std::string base_bs, base_be, base_span;
    bool flat = false;      // source proven stride == 0 (no stride load)
    bool own_facts = true;  // false when another candidate emitted the locals
    std::string span;   // pt_slice_span (first/count) local
    std::string bstart, bstop;  // raw int64 bound locals
    std::string step_lit;       // positive literal step ("1" when absent)
  };
  struct EphemFacts {
    std::string items, stride, len;
    bool flat = false;
  };
  std::map<const Stmt*, EphemSlice> ephem_;
  std::map<std::pair<const std::vector<Stmt>*, std::string>, EphemFacts>
      ephem_facts_;
  std::map<const Expr*, EphemFacts> ephem_index_facts_;
  std::map<std::string, const EphemSlice*> ephem_active_;
  size_t ephem_seq_ = 0;
  std::set<const std::vector<Stmt>*> switch_arm_lists_;
  void analyze_ephemeral_slices();
  void emit_ephemeral_slice(const Stmt& s, const EphemSlice& es,
                            bool in_function);
  std::string decl_persist_temp() {
    std::string t = fresh_temp();
    slot_names_.push_back(t);  // frame-rooted; NOT cleared per statement
    return t;
  }

  // ---- native float64 slots (float twin of the int64 lane) ---------------
  // analyze_native_floats() (run after analyze_native_ints, before
  // analyze_definite_assign) proves which slots hold ONLY PT_FLOAT values
  // and keeps them as a native double pair (value + used-flag) beside the
  // untouched pt_value slot — the exact architecture of the int lane above.
  // One-directional dependency: float_shape consults the FINAL int lane for
  // mixed int64->double operands (the runtime converts with a plain C cast,
  // pt_dbl/pt_as_double), but nothing in the int fixpoint ever consults
  // float state — so the int lane's output is provably unchanged.
  // float vs int slots are disjoint by construction (int_shape admits no
  // FloatLit/float slot, float_shape admits no IntLit/int slot), and no
  // expression is both shapes.
  void analyze_native_floats();
  bool native_float_is_local(const std::string& name) const;
  std::string native_float_read_text(const std::string& name, VarRes res,
                                     int line) const;
  bool native_float_slot_text(const std::string& name, VarRes res,
                              std::string& slot, std::string& flag) const;
  std::string native_float_text(const Expr& e) const;
  std::set<std::string> nativfg_;  // program globals kept native float64
  std::map<const FuncDecl*, std::set<std::string>> nativfl_;  // locals / fn

  // ---- post-2C perf: native int-lane function twins -----------------------
  // A viable function gets a SECOND entry point with raw int64 params ->
  // int64 result whose body runs entirely in the native lane. nativ_fns_
  // holds the source names of viable callees (only names whose
  // find_function resolution lands on a viable decl); nativ_fn_decls_ the
  // decls themselves. Both are computed TOGETHER with the slot lanes in
  // analyze_native_ints()'s joint fixpoint (a call result is int-shaped
  // exactly when the callee's twin survives, and a twin survives exactly
  // when its locals stay lanes — each shrinks the other). twin_prefilter()
  // supplies the structurally-viable candidates the fixpoint starts from.
  // twin_fn_ is non-null only while a twin body is being emitted — it
  // makes the native-read paths treat cur_func_'s own params as raw
  // noflag int64 lanes (the call-site gate promised int arguments).
  std::map<const FuncDecl*, TwinScan> twin_prefilter();
  // `bulk` = the walker is checking a BULK-ENTRY candidate (task-5 A): its
  // `for m in more` over the callee's own vararg is a sanctioned iterable
  // in addition to range(). The recursive calls propagate the flag.
  void twin_scan_stmts(const std::vector<Stmt>& body, const FuncDecl* f,
                       TwinScan& sc, bool bulk = false);
  void emit_function_native(const FuncDecl& f);
  std::set<std::string> nativ_fns_;
  std::set<const FuncDecl*> nativ_fn_decls_;
  const FuncDecl* twin_fn_ = nullptr;

  // ---- C3 (task-5 A): vararg bulk entries (Route 1) -----------------------
  // A `function f(a, *more)` whose EVERY free reference to `more` sits in a
  // sanctioned position (whole iterable of `for m in more`, or the single
  // argument of `len(more)` — is_bulk_for/is_bulk_len_call below, shared by
  // admission AND emission so they can never drift) gets a second entry
  // `pt_fn_<x>_b(fixed int64s..., const int64_t* bulk, int64_t n)` whose
  // body is the DYNAMIC body re-emitted over a raw int64 tail: no bound
  // vector, no PT_MISSING, no pt_list_of, no frame of its own dispatch —
  // the call site packs stack int64s directly (see emit_bulk_call). The
  // function stays OUT of nativ_fns_/int_shape entirely: its result is a
  // boxed pt_value, so no existing lane/twin proof changes shape (the
  // registry is admission-only; decline anything unprovable, §0.3).
  void analyze_bulk_varargs();
  std::set<const FuncDecl*> bulk_ok_;   // admissible bulk callees
  const FuncDecl* bulk_fn_ = nullptr;   // non-null while emitting a _b body

  // ---- C3 (task-5, Route-2): bulk-body lane facts --------------------------
  // analyze_bulk_lanes() runs INSIDE the joint fixpoint (after
  // analyze_native_ints, same monotone retract-only discipline) and proves,
  // PER BULK FUNCTION — never program-wide:
  //   * bulk_l_[f]: _b-body LOCAL slots whose every write is int-shaped
  //     under BULK-context facts (params are int64 BY ABI here). The
  //     dynamic body does NOT share these lanes: it keeps the boxed
  //     pt_l_* slot and binds the vararg loop by the iterator protocol,
  //     while the _b body reads pt_nl_* lanes — per-body rendering behind
  //     bulk_fn_, exactly how twin_fn_ already splits param rendering.
  //   * bulk_int_fns_/bulk_int_decls_: bulk callees whose RETURN VALUE is
  //     provably PT_INT in the _b context — an int-shape RETURN fact,
  //     joint (retract-only) with bulk_l_. 4b2 ties it into int_shape/
  //     native_int_text so a caller can prove `acc + <bulk call>` int64
  //     and keep its accumulation raw; anything unprovable declines
  //     per-FUNCTION back to today's boxed call (§0.3).
  void analyze_bulk_lanes();
  // int_shape with the BULK-context facts overlaid: merged lane map (final
  // nativ_l_ + this function's bulk_l_), names = final _i twin names +
  // bulk-int names, tdecls likewise — one shared predicate so emission and
  // proof cannot drift.
  bool bulk_int_shape(const Expr& e, const FuncDecl* fn,
                      const std::map<const FuncDecl*, std::set<std::string>>& merged,
                      const std::set<std::string>& names,
                      const std::map<std::string, const FuncDecl*>& tdecls);
  std::map<const FuncDecl*, std::set<std::string>> bulk_l_;
  std::set<std::string> bulk_int_fns_;            // callee-name keyed (int_shape)
  std::set<const FuncDecl*> bulk_int_decls_;      // decl keyed (emission)

  // ---- SIMD fusion: fvec/ivec elementwise kernel collapsing ---------------
  // analyze_native_fvecs() runs after the native lane/twin fixpoint; see
  // the block comment above its definition. A slot (global or local) PROVEN
  // to only ever hold fvec ('f') / ivec ('i') values gets elementwise assignment
  // expressions compiled to ONE fused kernel loop over clang vector-
  // extension lanes — no intermediate vectors, guaranteed vectorization
  // (the runtime dynamic operators are SIMD too, but every chained op
  // would materialize a temporary; fusion removes the memory traffic).
  // The proof is the entire type contract: slots keep their dynamic
  // pt_value storage, and only expressions whose operands are all proven
  // (vector-kind or int-lane scalar) fuse.
  void analyze_native_fvecs();
  // Shape of an expression in the fusion lattice: -1 = not shaped,
  // 0 = scalar (int-lane or numeric literal — broadcasts), 1 = fvec,
  // 2 = ivec (the fixpoint's bitmask values). Emission re-checks with the
  // FINAL proof maps.
  int fvec_shape(const Expr& e, const FuncDecl* fn) const;
  bool fvec_slot_kind(const std::string& name, VarRes res, const FuncDecl* fn,
                      int& kind) const;
  void emit_fvec_assign(const Stmt& s, int kind, bool in_function);
  std::map<std::string, int> nativ_fvg_;  // globals proven 1 (fvec) / 2 (ivec)
  std::map<const FuncDecl*, std::map<std::string, int>>
      nativ_fvl_;  // locals/fn

  // ---- text-chain fusion: formatted-output builder (perf) ---------------
  // A `+` chain whose parts are ALL provably text — string literals and
  // str() calls (anything else, like a bare variable, could carry a
  // non-text operand whose TypeError ordering we would have to reproduce)
  // — compiles to one pt_sb buffer instead of a tree of heap pt_concat
  // calls: literals copy straight in, str(<int-lane expr>) formats straight
  // in (pt_sb_puti — no heap text at all), other str() args materialize
  // once and append. Two tails: a value chain becomes ONE heap text
  // (pt_sb_finish), and f.write(<chain>) writes the buffer with a single
  // fwrite (pt_file_write_buf) — zero heap traffic per record, the
  // file-side equivalent of the print fast path. text_chain_zeroalloc (the
  // all-literal/all-int-lane subset) also reports alloc_free, so the loop
  // back-edge safe point elides on fused write loops.
  bool text_part_fusable(const Expr& e) const;
  bool text_chain_parts(const Expr& e, std::vector<const Expr*>& out) const;
  bool text_chain_zeroalloc(const Expr& e) const;
  // obj = the already-emitted receiver for the write tail, "" for the
  // value tail. Emits part materialization in source order first (every
  // raise point stays exactly where the unfused chain would raise), then
  // the builder block.
  std::string emit_text_chain(const Expr& e, bool in_function,
                              const std::string& obj, int src_line);

  // ---- text-cat in-place growth: the `s = s + chunk` proof ----------------
  // analyze_text_cats() (run from run(), before emission) admits a
  // candidate statement (`name = name + expr`, plain; `name += expr`) to
  // catok_ exactly when the destination slot's buffer is reachable from
  // that slot alone — then the statement emits
  // pt_text_cat2(..., may_grow=1), the capacity-aware amortized append,
  // instead of pt_add_f's exact-size rebuild. The proof rules and the
  // soundness argument are the block comment above its definition; any
  // doubt means the statement stays on the pre-existing emission path.
  void analyze_text_cats();
  std::set<const Stmt*> catok_;  // proven in-place-appendable statements

  // ---- statically-proven instance-method inlining (speed goal: class_ops) -
  // analyze_method_inlines() runs LAST in run() and plans the MethodCall
  // nodes whose every dynamic-dispatch premise it can prove program-wide; the
  // rules and the soundness argument live in the block comment before the
  // definition. A planned site is emitted by emit_inline_method: the callee's
  // body inlined at the site over per-site slots `pt_il<id>_<name>`, result
  // slot `pt_ilr<id>`, merge label `pt_ile<id>end` (the prefixes are pairwise
  // disjoint from each other and from every other generated identifier, so a
  // method local named `res` cannot collide with the result slot).
  // try_inline returns "" for a planned node (forces the A-normal statement
  // form); emit_expr materializes it after the fused-write check.
  struct InlineSite {
    const FuncDecl* method = nullptr;  // the callee (a user-class method)
    const FuncDecl* in_fn = nullptr;   // enclosing function (null: top/main)
    int id = 0;                        // site id — the slot/label namespace
    bool has_return = false;           // the body contains `return`
  };
  void analyze_method_inlines();
  std::string emit_inline_method(const Expr& e, const InlineSite& site,
                                 bool in_function);
  std::map<const Expr*, InlineSite> il_sites_;  // planned sites, by AST node
  int il_next_ = 0;                            // site-id counter (planning)
  // Methods currently being inlined (innermost last). A plan whose bodies
  // reach one another through proven instances would otherwise recurse
  // forever; a site whose callee is already on this stack falls back to the
  // dynamic emission (always valid — try_inline's A-normal force is
  // harmless for the dynamic path too).
  std::vector<const FuncDecl*> il_stack_;
  // Emission context of an inlined body ("" outside any): while set,
  // storage_for maps the method's own params/locals to `pt_il<id>_<name>`
  // and `return` intercepts into `pt_ilr<id> = ...; goto pt_ile<id>end;`.
  std::string micx_;
  int micx_id_ = -1;  // site id of the current inline context
  std::string il_name(const std::string& n) const {
    return "pt_il" + micx_ + "_" + n;
  }
  // Register a site slot with the CURRENT frame region: declared and
  // shadow-frame-rooted at the region top (emit_framed), assigned at entry.
  void il_register(const std::string& slot) { slot_names_.push_back(slot); }

  // ---- post-2C perf: used-flag elision for definitely-assigned slots -------
  // A native slot's used-before-assignment flag (`pt_ng_ok_X`) costs a load+branch
  // on every read and a store on every write, and — worse — those memory ops
  // pin loop counters to the stack so LLVM can't keep them in registers. For a
  // slot whose EVERY read is provably preceded by an unconditional assignment,
  // both the check and the store are dead; we track those in noflag_g_/noflag_l_
  // (analyze_definite_assign(), run after analyze_native_ints()). Conservative:
  // a slot first-assigned only inside a branch/loop keeps its flag, as does any
  // global read inside a function (function entry DA is empty). Reads of a
  // noflag slot compile to the raw int64; writes skip the flag store.
  void analyze_definite_assign();
  void da_check_expr(const Expr& e, std::set<std::string>& da_g,
                     std::set<std::string>& da_l, const FuncDecl* fn);
  void da_walk_stmt(const Stmt& s, std::set<std::string>& da_g,
                    std::set<std::string>& da_l, const FuncDecl* fn);
  void da_walk_stmts(const std::vector<Stmt>& body, std::set<std::string>& da_g,
                     std::set<std::string>& da_l, const FuncDecl* fn);
  bool native_noflag(const std::string& name, VarRes res) const {
    if (res == VarRes::Param)
      return twin_fn_ != nullptr;  // twin lanes: params always assigned
    if (res == VarRes::Global) return noflag_g_.count(name) != 0;
    if (res == VarRes::Local && cur_func_) {
      auto it = noflag_l_.find(cur_func_);
      return it != noflag_l_.end() && it->second.count(name) != 0;
    }
    return false;
  }
  std::set<std::string> noflag_g_;  // globals needing no used-flag
  std::map<const FuncDecl*, std::set<std::string>> noflag_l_;  // locals / fn

  // ---- C2 (task-5): try-elision x lane joint monotone fixpoint -------------
  // A `try` statement whose TRY BODY is proven unable to raise a Pentest
  // exception needs no handler at all: emit_try skips setjmp/pt_try_enter/
  // pt_try_leave (and the whole except-arm pad — dead by construction) when
  // elided_trys_ holds the statement, while try-stack bookkeeping keeps the
  // finally copy on every exit edge exactly as before (ctx.entered empty).
  // has_own_try() — the FOUR lane/twin disqualification sites
  // (:int fixpoint, :float fixpoint, :fvec fixpoint, twin_prefilter) — and
  // twin_scan_stmts() consult the ELIDED set, so a function whose own trys
  // are all elided regains its lane AND contains no setjmp by construction:
  // the C11 6.2.2.1 "indeterminate after longjmp" rule for non-volatile
  // automatics cannot fire, i.e. the register-vs-escaped-slot question
  // disappears instead of being answered. A single surviving registered try
  // keeps today's whole-function disqualification (conservative by one
  // slot). All sets start at the optimistic top (every try elided, every
  // callee non-raising) and only ever SHRINK inside run()'s outer loops —
  // any round that removes nothing is then exactly the shipped, proven
  // state (docs/performance/optimization.md §0.3).
  std::set<const Stmt*> elided_trys_;      // trys emitted WITHOUT registration
  std::set<const FuncDecl*> no_raise_fns_; // callee bodies proven non-raising
  void init_try_elision();                 // collect trys + optimistic tops
  bool retract_try_failures();             // one proof round; true = shrank
  // never_raise proofs mirror the EMITTER's family choice lane-first, then
  // float-lane, then literal-only dynamic; every call is analysis-time only
  // and must match the state emission will ship (see run()).
  bool stmt_never_raise(const Stmt& s, const FuncDecl* fn) const;
  bool body_never_raise(const std::vector<Stmt>& body, const FuncDecl* fn) const;
  bool expr_never_raise(const Expr& e, const FuncDecl* fn) const;  // family pick
  bool cond_never_raise(const Expr& e, const FuncDecl* fn) const;  // native conds
  bool lane_int_no_raise(const Expr& e, const FuncDecl* fn) const;  // int64 lane
  bool lane_int_allowed(const Expr& e, const FuncDecl* fn) const;  // admission
  bool lane_float_no_raise(const Expr& e, const FuncDecl* fn) const;
  bool native_read_no_raise(const std::string& name, VarRes res,
                             const FuncDecl* fn) const;            // guard rules
  // lane-slot lookups with an explicit function (analysis runs before
  // emission sets cur_func_, so native_slot_text/native_is_local are not
  // usable here)
  bool slot_is_int_lane(const std::string& name, VarRes res,
                        const FuncDecl* fn) const {
    if (res == VarRes::Param) return false;  // dynamic body guards params
    if (res == VarRes::Global) return nativ_g_.count(name) != 0;
    if (res == VarRes::Local && fn) {
      auto it = nativ_l_.find(fn);
      return it != nativ_l_.end() && it->second.count(name) != 0;
    }
    return false;
  }
  bool slot_is_float_lane(const std::string& name, VarRes res,
                          const FuncDecl* fn) const {
    if (res == VarRes::Param) return false;
    if (res == VarRes::Global) return nativfg_.count(name) != 0;
    if (res == VarRes::Local && fn) {
      auto it = nativfl_.find(fn);
      return it != nativfl_.end() && it->second.count(name) != 0;
    }
    return false;
  }

  // ---- Phase 2.1: boxed-slot pt_guard elimination for provably-bound reads --
  // Every guarded read compiles to pt_guard(slot, name, scope, line), whose
  // hit path is a single tag compare — but the compare still costs a load
  // (or reload), a branch and the name/scope/line arguments at EVERY read.
  // analyze_guard_elim() (run from run(), before emission) computes
  // noguard_g_/noguard_l_: slots for which EVERY read site can be proven to
  // see a materialized (non-PT_UNSET) value on that path. The proof rules —
  // soundness argument in the definition:
  //   * params: every bound-vector source (exact VLA paths and the §13
  //     binder) fills slots with real values or PT_MISSING, never PT_UNSET,
  //     and pt_guard only fires on PT_UNSET — so a param read AFTER slot
  //     materialization can never raise NameError. Reads inside a default
  //     expression of param i keep their guard unless the param's own slot
  //     is already materialized (index <= i): later params are still
  //     pt_unset() there and their guards MUST keep firing.
  //   * locals/globals: definite-assignment walk with the same conservative
  //     join discipline as analyze_definite_assign() (branches/loops/try
  //     bodies are walked isolated — their assignments never escape), but
  //     unlike the native-flag pass this walk must model EVERY read that
  //     can emit a pt_guard (including compound-assignment target reads,
  //     VarQual reads, tuple-target subexpressions and for/except bind
  //     names), because the starting assumption is "needs no guard".
  // Reads of a no-guard slot compile to the bare storage lvalue; anything
  // the analysis cannot prove keeps the byte-identical pt_guard call.
  void analyze_guard_elim();
  void ge_check_expr(const Expr& e, std::set<std::string>& da_g,
                     std::set<std::string>& da_l, const FuncDecl* fn);
  void ge_walk_stmt(const Stmt& s, std::set<std::string>& da_g,
                    std::set<std::string>& da_l, const FuncDecl* fn);
  void ge_walk_stmts(const std::vector<Stmt>& body, std::set<std::string>& da_g,
                     std::set<std::string>& da_l, const FuncDecl* fn);
  // Emission-time query: may `name`'s guard be elided at this read site?
  bool ge_elide(const std::string& name, VarRes res) const {
    if (res == VarRes::Global) return noguard_g_.count(name) != 0;
    if (res == VarRes::Local || res == VarRes::Param) {
      if (!cur_func_) return false;
      auto it = noguard_l_.find(cur_func_);
      return it != noguard_l_.end() && it->second.count(name) != 0;
    }
    return false;  // Function-value / Unresolved reads keep their guard
  }
  // The guarded read as it appears today, or the bare storage when proven.
  std::string guard_text(const std::string& storage, const std::string& name,
                         VarRes res, int line) const {
    if (ge_elide(name, res)) return storage;
    return "pt_guard(" + storage + ", \"" + c_escape(name) + "\", " +
           scope_name(res) + ", " + std::to_string(line) + ")";
  }
  std::set<std::string> noguard_g_;  // globals whose every read is DA-proven
  std::map<const FuncDecl*, std::set<std::string>>
      noguard_l_;          // locals + params / fn
  int ge_def_pos_ = -1;    // pre-pass: param index of the default being walked
                           // (-1 = body context, all params materialized)
  // Erase `name` from every candidate bucket (conservative catch-all for
  // read forms the walk refuses to classify: VarQual, function values).
  void ge_erase_all(const std::string& name) {
    noguard_g_.erase(name);
    for (auto& kv : noguard_l_) kv.second.erase(name);
  }

  // A-normalized emission: emits any statements the expression needs at the
  // current position and returns the expression text (a temp name for
  // effectful subtrees, an inline literal for pure ones).
  std::string emit_expr(const Expr& e, bool in_function);
  // Pure emission: returns "" (touching neither the output nor the guard
  // statements) when `e` cannot be written as one C expression safely.
  std::string try_inline(const Expr& e, bool in_function);
  // try_inline with a GNU statement-expression fallback; never fails. For
  // children whose evaluation must stay ordered/lazy in position.
  std::string inline_child(const Expr& e, bool in_function);
  std::string stmt_expr(const Expr& e, bool in_function);
  // Lowered form of a reserved builtin call other than print/range (M2-2B
  // §8 legacy set; SPEC §5.11.5 Python-parity set): places the already-
  // emitted argument texts into the call's fixed slot layout — unfilled
  // slots arrive as pt_missing(), variadics as a compound-literal array,
  // dict's named keys as parallel value/name arrays. One direct
  // pt_builtin_* call per builtin, no dispatch.
  std::string builtin_call_text(const Expr& e,
                                const std::vector<std::string>& a, int line);
  // The pt_name_slot table for globals()/locals()/vars()/dir() (SPEC
  // §5.11.5) as an automatic compound literal: one entry per live name of
  // the requested scope (frame for locals/vars, module for globals/dir),
  // referencing the storage the name reads from. `count` receives the
  // entry count (0 → the text is NULL).
  std::string name_table_text(bool frame_scope, size_t& count);

  void emit_body(const std::vector<Stmt>& body, bool in_function);
  void emit_stmt(const Stmt& s, bool in_function);
  // M2-2C §2: try/except/finally lowering + the exit-edge sequence shared
  // by break/continue/return (bound_depth -1 = every enclosing try).
  void emit_try(const Stmt& s, bool in_function);
  void emit_try_exits(int bound_depth, bool in_function);
  void emit_if_from(size_t idx, const Stmt& s, bool in_function);
  void emit_else_tail(size_t after, const Stmt& s, bool in_function);
  void emit_switch_from(size_t idx, const Stmt& s, const std::string& scrut,
                        bool in_function);
  void emit_switch_tail(size_t after, const Stmt& s, const std::string& scrut,
                        bool in_function);
  // C1 perf: a switch over a PROVEN int64 scrutinee with compile-time int
  // labels lowers to a native C `switch` (jump table) instead of the boxed
  // `pt_truthy(pt_eq_f(...))` ladder. native_switch_ok() is the proof gate;
  // anything it cannot prove falls back to the ladder above, unchanged.
  bool native_switch_ok(const Stmt& s) const;
  void emit_native_switch(const Stmt& s, const std::string& nat,
                          bool in_function);
  void emit_function(const FuncDecl& f);
  void emit_main_block(const MainDecl& m);
  // M2-2B §3/§12: stores an already-evaluated source value (temp name) into
  // a destructuring target: Name/Rest leaves, Index leaves (item assignment),
  // and (nested) Tuple patterns with the flat/nested/rest semantics.
  void emit_destructure(const AssignTarget& pat, const std::string& src,
                        bool in_function);

  // M2-2B §13: static call to a top-level (env-less) function: fills the
  // callee's bound vector at the call site and calls it directly. `a` holds
  // the already-emitted argument texts in source order.
  std::string emit_static_call(const FuncDecl& f, const Expr& e,
                               const std::vector<std::string>& a);
  // C3 (task-5 A): pack + dispatch ONE admissible bulk call — every bound
  // expression must spell raw int64 text here; "" means decline (the caller
  // keeps today's boxed path byte-identically).
  std::string emit_bulk_call(const std::vector<const Expr*>& fixed,
                             const std::vector<const Expr*>& tail,
                             const FuncDecl& cf);
  // M2-2B §13: dynamic call through a function value (callee_expr first):
  // pt_call binds arguments at runtime.
  std::string emit_dynamic_call(const Expr& e, bool in_function);
  // M2-2B §13: the static pt_funmeta instance + param-name array of `f`.
  void emit_funmeta(const FuncDecl& f);
  // Nested declarations hoisted out of a function body (source order,
  // descending into blocks but never into function bodies).
  std::vector<const FuncDecl*> scan_nested_decls(
      const std::vector<Stmt>& body) const;

  // ---- cross-scope access (docs/reference/cross-scope-access.md §4) --------------------
  // A binding referenced through `variable.` is "pinned": its storage is a
  // program-lifetime static instead of a frame slot. Two shapes, decided by
  // whether some closure also captures the binding:
  //   * plain pin  — `static pt_cell pt_p_<c>_<n>;`      (value in .v)
  //   * pinned cell — `static pt_cell* pt_px_<c>_<n>;`   (immortal heap
  //     cell created once at entry; closures share it through env slots,
  //     which must point at registered GC blocks — never at statics)
  // Owner-side accesses and qualified accesses both lower to the SAME
  // static, so there is exactly one storage per binding, ever.
  static bool pinned_binding(const FuncDecl& f, const std::string& name) {
    for (const std::string& p : f.pinned)
      if (p == name) return true;
    return false;
  }

  // C lvalue of the value inside a pinned slot (its .v / ->v).
  static std::string pin_value_text(const FuncDecl& owner,
                                    const std::string& name, bool cell) {
    return pin_cvar(owner, name, cell) + (cell ? "->v" : ".v");
  }

  // C lvalue of a VarQual site's storage (the semantic pass annotated pin):
  // the module global for variable.global (empty pin), the pinned static's
  // value for variable.parent / variable.<fn>.
  static std::string qual_value_text(const Expr& e) {
    if (e.pin.empty()) {
      if (e.res != VarRes::Global)
        fail("internal: unresolved variable-access (line " +
             std::to_string(e.loc.line) + ")");
      return "pt_g_" + e.name;
    }
    return e.pin + (e.pin_cell ? "->v" : ".v");
  }
  static std::string qual_value_text(const AssignTarget& t) {
    if (t.pin.empty()) {
      if (t.res != VarRes::Global)
        fail("internal: unresolved variable-access target (line " +
             std::to_string(t.loc.line) + ")");
      return "pt_g_" + t.name;
    }
    return t.pin + (t.pin_cell ? "->v" : ".v");
  }

  // Storage identifier for a resolved Var/Assign — the full C lvalue,
  // including `->v` through a boxed cell (own binding or captured env slot).
  // Fails on Unresolved, on Local/Param outside a function, and on a
  // resolution the semantic pass should have rejected.
  std::string storage_for(const std::string& name, VarRes res, bool in_function,
                          int line) {
    switch (res) {
      case VarRes::Global:
        return "pt_g_" + name;
      case VarRes::Function: {
        // 2B §6.6: a function name read yields a function value. A
        // top-level (or main-declared) function closes over nothing; a
        // nested declaration's value is the hoisted closure stored in its
        // owner local (or that local's cell when boxed).
        const FuncDecl* fn = find_function(name);
        if (fn && !fn->is_nested)
          return "pt_fun_value(&" + meta_name(*fn) + ", NULL, 0u)";
        const FuncDecl* own = cur_func_ ? find_own_nested(name) : nullptr;
        if (!own)
          fail("function value '" + name + "' not visible here (line " +
               std::to_string(line) + ")");
        size_t idx;
        if (!own_local_index(name, idx))
          fail("internal: nested function '" + name +
               "' has no owning local (line " + std::to_string(line) + ")");
        return cur_func_->local_is_cell[idx]
                   ? "pt_cell_" + name + "->v"
                   : "pt_l_" + name;
      }
      case VarRes::Local:
      case VarRes::Param: {
        if (!in_function || !cur_func_)
          fail("local storage '" + name + "' outside a function (line " +
               std::to_string(line) + ")");
        // Own binding first (params shadow locals; params include the
        // vararg/kwarg slots), then captures handed down through env. A
        // pinned own binding lives in its static slot instead — never in a
        // frame slot or cell alias.
        size_t pi;
        if (own_param_index(name, pi)) {
          if (!micx_.empty()) return il_name(name);
          if (pinned_binding(*cur_func_, name))
            return pin_value_text(
                *cur_func_, name,
                pi < cur_func_->param_is_cell.size() &&
                    cur_func_->param_is_cell[pi]);
          return slot_is_cell(*cur_func_, pi) ? "pt_cell_" + name + "->v"
                                              : "pt_l_" + name;
        }
        size_t li;
        if (own_local_index(name, li)) {
          if (!micx_.empty()) return il_name(name);
          if (pinned_binding(*cur_func_, name))
            return pin_value_text(
                *cur_func_, name,
                li < cur_func_->local_is_cell.size() &&
                    cur_func_->local_is_cell[li]);
          return cur_func_->local_is_cell[li] ? "pt_cell_" + name + "->v"
                                              : "pt_l_" + name;
        }
        long ci = capture_index(name);
        if (ci >= 0) {
          env_used_ = true;
          return "env[" + std::to_string(ci) + "]->v";
        }
        fail("internal: unresolved local '" + name + "' (line " +
             std::to_string(line) + ")");
      }
      case VarRes::Unresolved:
        break;
    }
    fail("unresolved variable '" + name + "' (line " + std::to_string(line) + ")");
  }

  static std::string scope_name(VarRes res) {
    return res == VarRes::Global ? "PT_SCOPE_GLOBAL" : "PT_SCOPE_LOCAL";
  }

  // Constant label of a case arm as a pt_value expression (M2-2A §2: the
  // parser guarantees a constant literal; the semantic pass checked dupes).
  static std::string case_label_text(const CaseArm& arm) {
    switch (arm.label_kind) {
      case ExprKind::IntLit:
        return int_literal(arm.ival);
      case ExprKind::FloatLit:
        return float_literal(arm.fval);
      case ExprKind::StrLit:
        return "PT_STR(\"" + c_escape(arm.sval) + "\")";
      case ExprKind::BoolLit:
        return arm.bval ? "pt_bool(true)" : "pt_bool(false)";
      case ExprKind::NoneLit:
        return "pt_none()";
      default:
        break;
    }
    fail("case label is not a constant literal (line " +
         std::to_string(arm.loc.line) + ")");
  }
};

std::vector<const FuncDecl*> Emitter::all_functions() const {
  std::vector<const FuncDecl*> fs;
  if (!prog_.functions.empty()) {
    for (const FuncDecl* f : prog_.functions) {
      if (f) fs.push_back(f);
    }
    return fs;
  }
  // Defensive fallback: derive from source items (same declaration order).
  for (const TopLevel& t : prog_.items) {
    if (t.kind == TopKind::Func && t.func) fs.push_back(t.func.get());
  }
  return fs;
}

const FuncDecl* Emitter::find_function(const std::string& name) const {
  // Top-level (and main-declared) functions only: nested declarations may
  // share names across scopes and are resolved via find_own_nested against
  // the current function instead. M2-2C §3.1: methods are excluded — they
  // share names across classes and are never bare-name reads (the semantic
  // pass rejects those before codegen).
  for (const FuncDecl* f : all_functions()) {
    if (!f->is_nested && !f->is_method && f->name == name) return f;
  }
  return nullptr;
}

// The nested declaration named `name` inside the CURRENT function (source
// order match). Nested bodies never nest into codegen here — scan stops at
// function bodies.
static void scan_nested_in(const std::vector<Stmt>& body,
                           std::vector<const FuncDecl*>& out) {
  for (const Stmt& s : body) {
    switch (s.kind) {
      case StmtKind::Func:
        if (s.func) out.push_back(s.func.get());
        break;  // never descend into the nested body itself
      case StmtKind::If:
        for (const std::vector<Stmt>& b : s.bodies) scan_nested_in(b, out);
        if (s.has_else) scan_nested_in(s.else_body, out);
        break;
      case StmtKind::While:
      case StmtKind::For:
        scan_nested_in(s.body, out);
        break;
      case StmtKind::Switch:
        for (const CaseArm& arm : s.cases) scan_nested_in(arm.body, out);
        break;
      case StmtKind::Try:  // M2-2C §2.1: body, except arms, finally
        scan_nested_in(s.body, out);
        for (const ExceptArm& arm : s.arms) scan_nested_in(arm.body, out);
        scan_nested_in(s.finally_body, out);
        break;
      default:
        break;
    }
  }
}

std::vector<const FuncDecl*> Emitter::scan_nested_decls(
    const std::vector<Stmt>& body) const {
  std::vector<const FuncDecl*> out;
  scan_nested_in(body, out);
  return out;
}

const FuncDecl* Emitter::find_own_nested(const std::string& name) const {
  if (!cur_func_) return nullptr;
  for (const FuncDecl* f : scan_nested_decls(cur_func_->body))
    if (f->name == name) return f;
  return nullptr;
}

// Lowered form of a reserved builtin call other than print/range
// (M2-2B §8): exactly one argument for len/str/int/float/bool/type/encode/
// decode and 0..1 for `set` (the semantic pass enforces both counts; the
// counts are re-checked here defensively). The SPEC §5.11.5 Python-parity
// set follows the semantic pass's slot binding (e.arg_slots/e.arg_nslots):
// this only PLACES the already-emitted texts, unfilled slots arriving as
// pt_missing() — never re-evaluating anything, exactly like
// bound_slots_text does for user calls.
std::string Emitter::builtin_call_text(const Expr& e,
                                       const std::vector<std::string>& a,
                                       int line) {
  const std::string& n = e.builtin_name;
  std::string ln = std::to_string(line);
  if (n == "len") return "pt_len_f(" + a[0] + ", " + ln + ")";
  if (n == "str") return "pt_builtin_str(" + a[0] + ", " + ln + ")";
  if (n == "int") return "pt_builtin_int(" + a[0] + ", " + ln + ")";
  if (n == "float") return "pt_builtin_float(" + a[0] + ", " + ln + ")";
  if (n == "bool") return "pt_builtin_bool(" + a[0] + ", " + ln + ")";
  if (n == "type") return "pt_builtin_type(" + a[0] + ", " + ln + ")";
  if (n == "encode") return "pt_builtin_encode(" + a[0] + ", " + ln + ")";
  if (n == "decode") return "pt_builtin_decode(" + a[0] + ", " + ln + ")";
  if (n == "http_post") {
    if (a.size() < 2 || a.size() > 3)
      fail("'http_post' expects 2 to 3 argument(s), got " +
           std::to_string(a.size()));
    std::string hdr = (a.size() == 3) ? a[2] : "pt_none()";
    return "pt_builtin_http(" + a[0] + ", " + a[1] + ", " + hdr + ", " + ln + ")";
  }
  if (n == "http_get") {
    if (a.empty() || a.size() > 2)
      fail("'http_get' expects 1 to 2 argument(s), got " +
           std::to_string(a.size()));
    std::string hdr = (a.size() == 2) ? a[1] : "pt_none()";
    return "pt_builtin_http_get(" + a[0] + ", " + hdr + ", " + ln + ")";
  }
  if (n == "url_encode") return "pt_builtin_url_encode(" + a[0] + ", " + ln + ")";
  /* SIMD vector constructors and the inner product (fvec/ivec). */
  if (n == "fvec") return "pt_builtin_fvec(" + a[0] + ", " + ln + ")";
  if (n == "ivec") return "pt_builtin_ivec(" + a[0] + ", " + ln + ")";
  if (n == "fvec_fill") return "pt_builtin_fvec_fill(" + a[0] + ", " + a[1] + ", " + ln + ")";
  if (n == "ivec_fill") return "pt_builtin_ivec_fill(" + a[0] + ", " + a[1] + ", " + ln + ")";
  if (n == "dot") return "pt_builtin_dot(" + a[0] + ", " + a[1] + ", " + ln + ")";
  if (n == "set") {
    if (a.empty()) return "pt_builtin_set(NULL, 0u, " + ln + ")";
    if (a.size() != 1)
      fail("'set' expects 0 to 1 argument(s), got " + std::to_string(a.size()));
    return "pt_builtin_set(" + value_array_text(a) + ", 1u, " + ln + ")";
  }
  if (n == "collect") {
    if (!a.empty()) fail("'collect' expects 0 argument(s), got " + std::to_string(a.size()));
    return "pt_int(pt_builtin_collect())";
  }
  if (n == "input") {
    if (a.empty()) return "pt_builtin_input(pt_none(), 0, " + ln + ")";
    if (a.size() > 1)
      fail("'input' expects 0 to 1 argument(s), got " + std::to_string(a.size()));
    return "pt_builtin_input(" + a[0] + ", 1, " + ln + ")";
  }
  if (n == "time") return "pt_float(pt_builtin_time())";
  if (n == "clock") return "pt_float(pt_builtin_clock())";
  // ---- SPEC §5.11.5: the Python-parity builtins ---------------------------
  if (py_builtin_stub(n))  // exists, always raises its own RuntimeError
    return "pt_builtin_unsupported(\"" + n + "\", " + ln + ")";
  const BuiltinSpec* spec = py_builtin_spec(n);
  if (spec == nullptr)
    fail("unsupported builtin '" + n + "' (line " + ln + ")");
  if (n == "globals" || n == "locals") {  // no value arguments at all
    size_t cnt = 0;
    std::string tbl = name_table_text(n == "locals", cnt);
    return "pt_builtin_" + n + "(" + tbl + ", " + std::to_string(cnt) + "u, " +
           ln + ")";
  }
  if (n == "vars" || n == "dir") {
    // obj slot (pt_missing() in the no-argument form): vars() gets the
    // frame table, dir() the globals table.
    std::string obj = a.empty() ? "pt_missing()" : a[0];
    size_t cnt = 0;
    std::string tbl = name_table_text(n == "vars", cnt);
    return "pt_builtin_" + n + "(" + obj + ", " + tbl + ", " +
           std::to_string(cnt) + "u, " + ln + ")";
  }
  if (spec->variadic) {
    // positionals → compound-literal array; max/min's named key/default
    // pull through as fixed trailing parameters.
    std::vector<std::string> pos;
    std::string key_txt = "pt_missing()";
    std::string dflt_txt = "pt_missing()";
    for (size_t i = 0; i < a.size(); ++i) {
      std::string an = i < e.arg_names.size() ? e.arg_names[i] : "";
      if (an == "key" && spec->variadic_key) {
        key_txt = a[i];
        continue;
      }
      if (an == "default" && spec->variadic_default) {
        dflt_txt = a[i];
        continue;
      }
      pos.push_back(a[i]);
    }
    std::string arr =
        pos.empty() ? std::string("NULL") : value_array_text(pos);
    std::string cnt = std::to_string(pos.size()) + "u";
    if (spec->variadic_key || spec->variadic_default)
      return "pt_builtin_" + n + "(" + arr + ", " + cnt + ", " + key_txt + ", " +
             dflt_txt + ", " + ln + ")";
    return "pt_builtin_" + n + "(" + arr + ", " + cnt + ", " + ln + ")";
  }
  if (spec->dict_form) {
    // ≤1 positional source + arbitrary named keys (dict(a=1)): positionals
    // and named values/names pass as parallel arrays.
    std::vector<std::string> pos;
    std::vector<std::string> vals;
    std::vector<std::string> names;
    for (size_t i = 0; i < a.size(); ++i) {
      std::string an = i < e.arg_names.size() ? e.arg_names[i] : "";
      if (an.empty()) {
        pos.push_back(a[i]);
      } else {
        vals.push_back(a[i]);
        names.push_back("\"" + c_escape(an) + "\"");
      }
    }
    std::string name_arr;
    for (size_t i = 0; i < names.size(); ++i) {
      if (i) name_arr += ", ";
      name_arr += names[i];
    }
    if (!names.empty()) name_arr = "(const char* const[]){" + name_arr + "}";
    else name_arr = "NULL";
    return "pt_builtin_dict(" +
           (pos.empty() ? std::string("NULL") : value_array_text(pos)) + ", " +
           std::to_string(pos.size()) + "u, " +
           (vals.empty() ? std::string("NULL") : value_array_text(vals)) +
           ", " + name_arr + ", " + std::to_string(vals.size()) + "u, " + ln +
           ")";
  }
  // Uniform slots: slot k takes the text the semantic pass bound to it, or
  // pt_missing() when no argument filled it (the runtime defaults it).
  {
    std::vector<std::string> slots(e.arg_nslots, "pt_missing()");
    std::string joined;
    for (size_t i = 0; i < a.size(); ++i) {
      int s = (i < e.arg_slots.size()) ? e.arg_slots[i] : -1;
      if (s >= 0) slots[(size_t)s] = a[i];
    }
    for (size_t k = 0; k < slots.size(); ++k) {
      if (k) joined += ", ";
      joined += slots[k];
    }
    if (!joined.empty()) joined += ", ";
    return "pt_builtin_" + n + "(" + joined + ln + ")";
  }
}

// The pt_name_slot table for globals()/locals()/vars()/dir() (SPEC
// §5.11.5). Boxed names reference their pt_value slot (kind 0 — the runtime
// skips PT_UNSET, i.e. not-yet-assigned); native-int names their int64
// slot plus the guard flag (kind 1 — a definitely-assigned noflag slot
// passes NULL, always visible); top-level functions their meta (kind 2 —
// synthesized into a function value on read). Names resolve with the same
// precedence as the read path (native first, then cell-aliased/pinned
// storage), so the table always agrees with what the program sees.
std::string Emitter::name_table_text(bool frame_scope, size_t& count) {
  std::vector<std::string> entries;
  if (frame_scope && cur_func_) {
    const FuncDecl& f = *cur_func_;
    std::vector<std::string> names = slot_names(f);  // params + vararg + kwarg
    for (size_t j = 0; j < f.locals.size(); ++j) names.push_back(f.locals[j]);
    for (size_t i = 0; i < names.size(); ++i) {
      const std::string& nm = names[i];
      std::string slot, flag;
      if (native_slot_text(nm, VarRes::Local, slot, flag)) {
        entries.push_back("{ \"" + c_escape(nm) + "\", 1, (void*)&(" + slot +
                          "), " +
                          (native_noflag(nm, VarRes::Local)
                               ? "NULL"
                               : "(unsigned char*)&(" + flag + ")") +
                          " }");
        continue;
      }
      if (native_float_slot_text(nm, VarRes::Local, slot, flag)) {
        entries.push_back("{ \"" + c_escape(nm) + "\", 3, (void*)&(" + slot +
                          "), " +
                          (native_noflag(nm, VarRes::Local)
                               ? "NULL"
                               : "(unsigned char*)&(" + flag + ")") +
                          " }");
        continue;
      }
      entries.push_back("{ \"" + c_escape(nm) + "\", 0, (void*)&(" +
                        storage_for(nm, VarRes::Local, true, 0) + "), NULL }");
    }
  } else {
    for (size_t g = 0; g < prog_.globals.size(); ++g) {
      const std::string& nm = prog_.globals[g];
      std::string slot, flag;
      if (native_slot_text(nm, VarRes::Global, slot, flag)) {
        entries.push_back("{ \"" + c_escape(nm) + "\", 1, (void*)&(" + slot +
                          "), " +
                          (native_noflag(nm, VarRes::Global)
                               ? "NULL"
                               : "(unsigned char*)&(" + flag + ")") +
                          " }");
        continue;
      }
      if (native_float_slot_text(nm, VarRes::Global, slot, flag)) {
        entries.push_back("{ \"" + c_escape(nm) + "\", 3, (void*)&(" + slot +
                          "), " +
                          (native_noflag(nm, VarRes::Global)
                               ? "NULL"
                               : "(unsigned char*)&(" + flag + ")") +
                          " }");
        continue;
      }
      entries.push_back(
          "{ \"" + c_escape(nm) + "\", 0, (void*)&(pt_g_" + nm + "), NULL }");
    }
    // Top-level function names only. A nested declaration binds through
    // its owning local (already in the lists above); a class METHOD is a
    // member of its class, not a global — methods share names across
    // classes, so including them would duplicate entries in dir() and
    // fake globals()["__init__"] reads (M2-2C §3.1: lookup goes through
    // c_name only).
    for (size_t fi = 0; fi < prog_.functions.size(); ++fi) {
      const FuncDecl* f = prog_.functions[fi];
      if (f->is_nested || f->is_method) continue;
      entries.push_back("{ \"" + c_escape(f->name) + "\", 2, (void*)&" +
                        meta_name(*f) + ", NULL }");
    }
  }
  count = entries.size();
  if (entries.empty()) return "NULL";
  std::string joined;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i) joined += ", ";
    joined += entries[i];
  }
  return "(const pt_name_slot[]){" + joined + " }";
}

bool Emitter::own_param_index(const std::string& name, size_t& idx) const {
  if (!cur_func_) return false;
  const std::vector<std::string> ps = slot_names(*cur_func_);
  for (size_t i = 0; i < ps.size(); ++i) {
    if (ps[i] == name) { idx = i; return true; }
  }
  return false;
}

bool Emitter::own_local_index(const std::string& name, size_t& idx) const {
  if (!cur_func_) return false;
  for (size_t i = 0; i < cur_func_->locals.size(); ++i) {
    if (cur_func_->locals[i] == name) { idx = i; return true; }
  }
  return false;
}

// Is bound slot `slot` (ABI order: params, vararg, kwarg) boxed because a
// nested function captures its name?
bool Emitter::slot_is_cell(const FuncDecl& f, size_t slot) {
  if (slot < f.params.size()) {
    return slot < f.param_is_cell.size() && f.param_is_cell[slot];
  }
  size_t k = slot - f.params.size();
  if (k == 0) return f.vararg_is_cell;
  return f.kwarg_is_cell;
}

long Emitter::capture_index(const std::string& name) const {
  if (!cur_func_) return -1;
  for (size_t i = 0; i < cur_func_->captures.size(); ++i)
    if (cur_func_->captures[i] == name) return static_cast<long>(i);
  return -1;
}

const MainDecl* Emitter::find_main() const {
  const MainDecl* found = nullptr;
  for (const TopLevel& t : prog_.items) {
    if (t.kind == TopKind::Main) {
      if (found) fail("more than one main block");
      found = t.main.get();
      if (!found) fail("main item without body");
    }
  }
  return found;
}

// ---- post-2C perf: native int64 slot analysis (TODO.txt speed goal) --------
//
// A "slot" — a program global or an unboxed function local — is *native*
// when every write to it is a plain or compound assignment whose operator
// and right-hand side are provably int64-shaped, and no protocol binding
// (for target, except-as) or pattern (destructuring) leaf ever writes it.
// Native slots are kept as a native int64_t pair (value + used-before-
// assignment flag) next to their untouched pt_value slot (shadow frame and
// GC roots stay exactly as before), so hot scalar loops compile to the
// same straight C arithmetic a C twin gets, while every observable byte —
// wraps, div/mod edge cases, guard messages, error classes and lines —
// stays identical to the dynamic path.
//
// Locals additionally require that the function contains no try statement:
// setjmp lives in the generated frame of try-containing functions only,
// and C11 7.13.2.1 makes only that frame's automatic objects indeterminate
// after longjmp (handlers reading native locals would see stale/garbage
// values). Globals are static storage — exempt — and never need this.
// Exception-class globals are excluded (injected `pt_g_X = pt_exc_class`
// at startup) and class-name globals are poisoned (the class statement
// writes pt_g_<name> = pt_class_new directly).
//
// Conservative by construction: params, cells/captures, nonlocal targets,
// call results, floats, bools, **/bitwise/shift operators and anything
// unprovable simply stay on the existing dynamic path.

struct SlotWrite {
  const Expr* rhs;         // RHS of the assignment (never null)
  const FuncDecl* fn;      // function the assignment sits in (Var key)
  // Open-coded range-loop target bind: the StmtKind::For emitter writes the
  // int64 induction variable directly into the native slot, so the write is
  // int-shaped with no RHS expression to prove (its emission is the loop
  // bind itself, not native_int_text).
  bool int_bind = false;
  // Compound operator ("", "+=", "-=", "*=", "/=", "%=") — the fvec
  // fusion fixpoint reads it to prove `x <op>= y` kind-preserving; the
  // int-lane fixpoint predates it and ignores the field.
  std::string op;
};

struct SlotInfo {
  std::vector<SlotWrite> writes;
  bool poisoned = false;   // any non-int-shaped write/bind ever targets it
};

using GlobalSlots = std::map<std::string, SlotInfo>;
using LocalSlots =
    std::map<const FuncDecl*, std::map<std::string, SlotInfo>>;

// Expression predicate used by the fixpoint. Emission mirrors this switch
// exactly (Emitter::native_int_text) — if the two ever drift, codegen
// reports an internal failure instead of miscompiling.
//
// C2 (task-5) tuning knobs (defaults keep the pre-C2 behavior):
//  * `slot_write` = false only when checking a WRITE into a lane slot:
//    the slot is emitted TWICE (dynamic entry body + twin body) and the
//    dynamic body's native_int_text cannot spell a param read (it needs
//    twin_fn_), so a param-reading RHS would land the emitter's
//    "assigned a non-int expression" internal failure. Declining it keeps
//    the slot dynamic (conservative by one slot, §0.3). Returns/args keep
//    the param rule: a twin return spells pt_nl_*, a dynamic return boxes.
//  * `tdecls` maps callee name -> still-viable twin decl so the shared
//    call binder (bind_call_args) can order named/omitted args.
// Float-lane predicate, defined after int_shape (the two are mutually
// recursive only through int(<float-lane>) / float(<int-lane>) conversions;
// the analysis alternates the two to a joint fixpoint — see run()).
static bool float_shape(const Expr& e, const FuncDecl* fn,
                        const std::set<std::string>& fg,
                        const std::map<const FuncDecl*, std::set<std::string>>& fl,
                        const std::set<std::string>& ig,
                        const std::map<const FuncDecl*, std::set<std::string>>& il,
                        const std::set<std::string>* twin,
                        const std::map<std::string, const FuncDecl*>* tdecls,
                        const std::set<std::string>* int_lists,
                        const std::set<const Expr*>* int_list_reads);

static bool int_shape(const Expr& e, const FuncDecl* fn,
                      const std::set<std::string>& ig,
                      const std::map<const FuncDecl*, std::set<std::string>>& il,
                      // Post-2C perf: non-null only inside the joint
                      // lane/twin fixpoint (analyze_native_ints) —
                      // enables the twin rules (param reads are int64 by
                      // the call-site gate; calls to still-viable twin
                      // callees over int-shaped positional/named args are
                      // int64). fvec_shape passes null and keeps the
                      // historical (slot-only) shape set.
                      const std::set<std::string>* twin = nullptr,
                      bool slot_write = true,
                      const std::map<std::string, const FuncDecl*>* tdecls =
                          nullptr,
                      // Float lane of the PREVIOUS joint round (nullptr on
                      // the first pass). int(<float-lane>) can raise the
                      // documented ValueError, but every non-raising result
                      // is a proven int64 — the lane helper reproduces the
                      // raise at the same line. null/empty declines.
                      const std::set<std::string>* fg = nullptr,
                      const std::map<const FuncDecl*, std::set<std::string>>* fl =
                          nullptr,
                      // Int-only list facts + the exact Index read sites
                      // whose base is definitely assigned (see the lazy
                      // slice/int-list analyses). Null disables the rule.
                      const std::set<std::string>* int_lists = nullptr,
                      const std::set<const Expr*>* int_list_reads = nullptr) {
  switch (e.kind) {
    case ExprKind::IntLit:
      return true;
    case ExprKind::Var: {
      if (e.res == VarRes::Global) return ig.count(e.name) != 0;
      if (e.res == VarRes::Local && fn) {
        auto it = il.find(fn);
        return it != il.end() && it->second.count(e.name) != 0;
      }
      // Twin lane: the call-site gate passed an actual int64 for this
      // param, so in a twin body the read is a raw int64 lane.
      if (e.res == VarRes::Param && fn && twin && slot_write) return true;
      return false;  // params, function names, unresolved
    }
    case ExprKind::Index: {
      // Element read of a proven int-only list. The site must be DA for the
      // base's bare-storage read (native_int_text emits no guard), and the
      // index itself must be proven int64 (the native helper takes a raw
      // index; a dynamic index keeps the boxed operator path).
      if (!int_lists || !int_list_reads) return false;
      if (!e.lhs || e.lhs->kind != ExprKind::Var ||
          e.lhs->res != VarRes::Global)
        return false;
      if (!int_lists->count(e.lhs->name)) return false;
      if (!int_list_reads->count(&e)) return false;
      return e.rhs &&
             int_shape(*e.rhs, fn, ig, il, twin, slot_write, tdecls, fg, fl,
                       int_lists, int_list_reads);
    }
    case ExprKind::Unary:
      // "-" and "~" over an int operand are int64 (pt_neg's int path wraps;
      // pt_bnot's is a uint64 complement) — mirrors native_int_text.
      return (e.op == "-" || e.op == "~") && e.lhs &&
             int_shape(*e.lhs, fn, ig, il, twin, slot_write, tdecls, fg, fl, int_lists, int_list_reads);
    case ExprKind::Binary: {
      // Bitwise/shift (M2-2A §3.2) over two proven-int operands yield int64
      // ("ints only — no bool promotion", so no widening cases exist).
      if (e.op != "+" && e.op != "-" && e.op != "*" && e.op != "/" &&
          e.op != "%" && e.op != "&" && e.op != "|" && e.op != "^" &&
          e.op != "<<" && e.op != ">>")
        return false;
      return e.lhs && e.rhs &&
             int_shape(*e.lhs, fn, ig, il, twin, slot_write, tdecls, fg, fl, int_lists, int_list_reads) &&
             int_shape(*e.rhs, fn, ig, il, twin, slot_write, tdecls, fg, fl, int_lists, int_list_reads);
    }
    case ExprKind::Call: {
      // Native twin callee (the joint lane fixpoint): a static call to a
      // still-viable twin over int-shaped BOUND args yields int64. The
      // shared binder (bind_call_args) resolves named args to param
      // positions and fills omitted params with their declaration default,
      // in source (== param-slot) evaluation order.
      // The mutual recursion fib -> fib closes through the fixpoint
      // (viability is only ever removed, so this can only shrink).
      if (!e.is_builtin) {
        if (!twin) return false;
        if (e.callee_expr || e.callee.empty()) return false;
        if (!twin->count(e.callee)) return false;
        if (!tdecls) return false;
        auto it = tdecls->find(e.callee);
        if (it == tdecls->end() || !it->second) return false;
        std::vector<const Expr*> bound;
        if (!bind_call_args(e, *it->second, bound)) return false;
        for (const Expr* b : bound)
          if (!b || !int_shape(*b, fn, ig, il, twin, slot_write, tdecls, fg, fl, int_lists, int_list_reads))
            return false;
        return true;
      }
      // SPEC §5.11.5: the int-shaped Python-parity builtins stay in the
      // native lane over proven-int operands — abs(x), round(x) (the
      // one-argument int form is the identity), pow(a, b) (the 2-argument
      // wrapping form) and min/max over exactly two positional operands
      // (no key/default — a key callback is opaque user code).
      const std::string& n = e.builtin_name;
      if (n == "int" && e.args.size() == 1u) {
        // int(<proven float lane>): the lane emits pt_nf2i, which keeps the
        // ValueError for NaN/inf/out-of-range and otherwise returns the
        // identical truncation. fg/fl are the float lane: with no proven
        // float operand the call could be int("...")/int(bool)/type-error,
        // so it stays dynamic. Named argument syntax is not a conversion.
        if (!e.arg_names.empty() && !e.arg_names[0].empty()) return false;
        return e.args[0] && fg && fl &&
               float_shape(*e.args[0], fn, *fg, *fl, ig, il, twin, tdecls,
                           int_lists, int_list_reads);
      }
      if (n == "abs" || n == "round") {
        return e.args.size() == 1u && e.args[0] &&
               int_shape(*e.args[0], fn, ig, il, twin, slot_write, tdecls, fg, fl, int_lists, int_list_reads);
      }
      if (n == "pow") {
        return e.args.size() == 2u && e.args[0] && e.args[1] &&
               int_shape(*e.args[0], fn, ig, il, twin, slot_write, tdecls, fg, fl, int_lists, int_list_reads) &&
               int_shape(*e.args[1], fn, ig, il, twin, slot_write, tdecls, fg, fl, int_lists, int_list_reads);
      }
      if (n == "min" || n == "max") {
        if (e.args.size() != 2u) return false;
        for (const std::string& an : e.arg_names)
          if (!an.empty()) return false;
        return e.args[0] && e.args[1] &&
               int_shape(*e.args[0], fn, ig, il, twin, slot_write, tdecls, fg, fl, int_lists, int_list_reads) &&
               int_shape(*e.args[1], fn, ig, il, twin, slot_write, tdecls, fg, fl, int_lists, int_list_reads);
      }
      return false;
    }
    default:
      return false;
  }
}

static bool native_compound_op(const std::string& op) {
  return op == "+=" || op == "-=" || op == "*=" || op == "/=" || op == "%=" ||
         op == "&=" || op == "|=" || op == "^=" || op == "<<=" || op == ">>=";
}

// Expression predicate of the native FLOAT lane (Emitter::analyze_native_
// floats). An expression is float-shaped when evaluating it on the dynamic
// path would produce exactly a PT_FLOAT value:
//  * a float literal or a read of a float-lane slot;
//  * unary "-" over a float-shaped operand (pt_neg_f's PT_FLOAT path is a
//    plain IEEE negate — sign bit flips, -0.0 -> 0.0, inf/nan preserved);
//  * "+", "-", "*", "/" where BOTH sides are float-shaped (pt_add_f et al.
//    take the numeric fast path), or where exactly ONE side is float-shaped
//    and the other is int-shaped under the FINAL int lane: the runtime then
//    converts int64 -> double with a plain C cast (pt_dbl/pt_as_double) and
//    returns pt_float — the lane emits the identical (double) conversion.
//    ("/" additionally carries pt_div_f's y == 0.0 ZeroDivisionError on the
//    CONVERTED divisor — pt_nfdiv reproduces it.)
// Everything else is DECLINED: float "%" (pt_mod raises TypeError on any
// non-int operand — the lane stays dynamic and raises identically), mixed
// int/float COMPARISONS (pt_cmp3 compares exactly via long double; a C
// double conversion is not bit-exact for int64 near 2^63), "**", bools,
// calls, and every int-shaped write (so `x = 0; x = x + 0.5` declines —
// the slot is dynamic, exactly like today). Mirrors Emitter::
// native_float_text; drift between the two is an internal failure, never a
// miscompile. `ig`/`il` are the FINAL int-lane maps and `twin` the final
// int-twin set (an int-twin call result is a proven int64 → the mixed rule
// applies with the same plain conversion). float_shape only ever checks
// WRITES into a float slot, so its int_shape calls pass the slot-write
// flag (a param-reading mixed operand cannot be spelled by the dynamic
// body's native_float_text — decline, conservative by one slot) and the
// twin-decl map for the shared call binder.
static bool float_shape(const Expr& e, const FuncDecl* fn,
                        const std::set<std::string>& fg,
                        const std::map<const FuncDecl*, std::set<std::string>>& fl,
                        const std::set<std::string>& ig,
                        const std::map<const FuncDecl*, std::set<std::string>>& il,
                        const std::set<std::string>* twin = nullptr,
                        const std::map<std::string, const FuncDecl*>* tdecls =
                            nullptr,
                        const std::set<std::string>* int_lists = nullptr,
                        const std::set<const Expr*>* int_list_reads = nullptr) {
  switch (e.kind) {
    case ExprKind::FloatLit:
      return true;
    case ExprKind::Var: {
      if (e.res == VarRes::Global) return fg.count(e.name) != 0;
      if (e.res == VarRes::Local && fn) {
        auto it = fl.find(fn);
        return it != fl.end() && it->second.count(e.name) != 0;
      }
      return false;  // params / captures / unresolved are never float lanes
    }
    case ExprKind::Unary:
      return e.op == "-" && e.lhs &&
             float_shape(*e.lhs, fn, fg, fl, ig, il, twin, tdecls, int_lists, int_list_reads);
    case ExprKind::Binary: {
      if (e.op != "+" && e.op != "-" && e.op != "*" && e.op != "/")
        return false;
      if (!e.lhs || !e.rhs) return false;
      bool a = float_shape(*e.lhs, fn, fg, fl, ig, il, twin, tdecls, int_lists, int_list_reads);
      bool b = float_shape(*e.rhs, fn, fg, fl, ig, il, twin, tdecls, int_lists, int_list_reads);
      if (a && b) return true;
      // Mixed: the int side must be PROVEN int64 (a dynamic pt_value could
      // hold anything — bool, text — whose runtime behavior differs).
      if (a) return int_shape(*e.rhs, fn, ig, il, twin, /*slot_write=*/false,
                              tdecls, &fg, &fl, int_lists, int_list_reads);
      if (b) return int_shape(*e.lhs, fn, ig, il, twin, /*slot_write=*/false,
                              tdecls, &fg, &fl, int_lists, int_list_reads);
      return false;
    }
    case ExprKind::Call: {
      // float(<proven int lane>): pt_builtin_float's PT_INT branch is a
      // plain (double) cast — the lane emits the identical cast. Anything
      // else (float of text/bool/float, no-arg float, named arg, a shadowed
      // builtin) stays dynamic.
      if (!e.is_builtin || e.builtin_name != "float" || e.args.size() != 1u)
        return false;
      if (!e.arg_names.empty() && !e.arg_names[0].empty()) return false;
      return e.args[0] &&
             int_shape(*e.args[0], fn, ig, il, twin, /*slot_write=*/false,
                       tdecls, &fg, &fl, int_lists, int_list_reads);
    }
    default:
      return false;
  }
}

static void poison_slot(const std::string& name, VarRes res,
                        const FuncDecl* fn, GlobalSlots& gw, LocalSlots& lw) {
  if (res == VarRes::Global)
    gw[name].poisoned = true;
  else if (res == VarRes::Local && fn)
    lw[fn][name].poisoned = true;
}

static void record_write(const std::string& name, VarRes res,
                         const FuncDecl* fn, const Expr* rhs,
                         const std::string& op, GlobalSlots& gw,
                         LocalSlots& lw) {
  if (res != VarRes::Global && res != VarRes::Local) return;  // params etc.
  if (!rhs) return;
  if (!op.empty() && !native_compound_op(op)) {
    poison_slot(name, res, fn, gw, lw);  // **=, bitwise, shifts stay dynamic
    return;
  }
  if (res == VarRes::Global)
    gw[name].writes.push_back(SlotWrite{rhs, fn, false, op});
  else
    lw[fn][name].writes.push_back(SlotWrite{rhs, fn, false, op});
}

// Pattern (Tuple/Rest) leaves bind through unpack temps, never int exprs.
static void collect_pattern_leaves(
    const AssignTarget& t,
    std::vector<std::pair<std::string, VarRes>>& out) {
  if (t.kind == AssignTarget::Kind::Name ||
      t.kind == AssignTarget::Kind::Rest ||
      t.kind == AssignTarget::Kind::VarQual) {
    out.push_back({t.name, t.res});
    return;
  }
  if (t.kind == AssignTarget::Kind::Tuple)
    for (const AssignTarget& el : t.elems) collect_pattern_leaves(el, out);
}

// C3 (task-5 A): shared admit-gates, defined with the bulk-variant block
// above twin_scan_stmts — declared early because the bulk walk (Route-2)
// classifies the sanctioned vararg-For before reaching them.
static bool is_bulk_for(const Stmt& s, const std::string& v);

// `bulk` (C3 Route-2): NULL for the SHARED walk (twin_int/float/fvec/cat
// analyses — identical to Route-1); non-NULL only for analyze_bulk_lanes'
// private collection, whose sanctioned `for m in more` bind records the raw
// int64 int_bind INSTEAD of the iterator poison — the shared walk must keep
// poisoning it, because the DYNAMIC body binds that slot from pt_iter.
static void walk_stmts(const std::vector<Stmt>& body, const FuncDecl* fn,
                       GlobalSlots& gw, LocalSlots& lw,
                       const std::set<const FuncDecl*>* bulk = nullptr);

static void walk_stmt(const Stmt& s, const FuncDecl* fn, GlobalSlots& gw,
                      LocalSlots& lw,
                      const std::set<const FuncDecl*>* bulk = nullptr) {
  switch (s.kind) {
    case StmtKind::Assign: {
      if (s.target.kind == AssignTarget::Kind::Name) {
        record_write(s.name, s.res, fn, s.expr.get(), s.op, gw, lw);
      } else {
        std::vector<std::pair<std::string, VarRes>> leaves;
        collect_pattern_leaves(s.target, leaves);
        for (const auto& lv : leaves)
          poison_slot(lv.first, lv.second, fn, gw, lw);
      }
      break;
    }
    case StmtKind::For: {
      // The open-coded range loop (see the StmtKind::For emitter) binds its
      // target from a known int64 induction variable — an int_bind that can
      // join the native lane. Every other iterable yields arbitrary values →
      // poison, exactly as before. C3 Route-2 (bulk walk only): a `for m ->
      // more` over an ADMITTED bulk callee's own vararg binds a RAW int64
      // (the _b tail element) — an int_bind for analyze_bulk_lanes. The
      // SHARED walk still poisons it: the dynamic body binds the same slot
      // from pt_iter and must never join a shared lane.
      if (s.expr && s.expr->kind == ExprKind::Call && s.expr->is_builtin_range &&
          (s.res == VarRes::Global || (s.res == VarRes::Local && fn))) {
        SlotWrite w{s.expr.get(), fn, true, std::string()};
        if (s.res == VarRes::Global)
          gw[s.name].writes.push_back(w);
        else
          lw[fn][s.name].writes.push_back(w);
      } else if (s.res == VarRes::Local && fn && bulk && bulk->count(fn) &&
                 is_bulk_for(s, fn->vararg) && s.expr->kind == ExprKind::Var) {
        SlotWrite w{s.expr.get(), fn, true, std::string()};  // raw tail bind
        lw[fn][s.name].writes.push_back(w);
      } else {
        poison_slot(s.name, s.res, fn, gw, lw);  // iteration protocol binding
      }
      walk_stmts(s.body, fn, gw, lw, bulk);  // assignments inside still count
      break;
    }
    case StmtKind::Try: {
      walk_stmts(s.body, fn, gw, lw, bulk);
      for (const ExceptArm& a : s.arms) {
        if (!a.name.empty()) poison_slot(a.name, a.res, fn, gw, lw);
        walk_stmts(a.body, fn, gw, lw, bulk);
      }
      walk_stmts(s.finally_body, fn, gw, lw, bulk);
      break;
    }
    case StmtKind::Class:
      poison_slot(s.name, VarRes::Global, fn, gw, lw);  // direct pt_g_ write
      walk_stmts(s.body, fn, gw, lw, bulk);  // pass-only today; be thorough
      break;
    case StmtKind::Func:
      break;  // nested bodies are covered by all_functions()
    case StmtKind::If:
      for (const auto& b : s.bodies) walk_stmts(b, fn, gw, lw, bulk);
      walk_stmts(s.else_body, fn, gw, lw, bulk);
      break;
    case StmtKind::While:
      walk_stmts(s.body, fn, gw, lw, bulk);
      break;
    case StmtKind::Switch:
      for (const auto& b : s.bodies) walk_stmts(b, fn, gw, lw, bulk);
      break;
    default:
      break;  // ExprStmt/Return/Raise/Pass/Break/Continue/Global/Nonlocal
  }
}

static void walk_stmts(const std::vector<Stmt>& body, const FuncDecl* fn,
                       GlobalSlots& gw, LocalSlots& lw,
                       const std::set<const FuncDecl*>* bulk) {
  for (const Stmt& s : body) walk_stmt(s, fn, gw, lw, bulk);
}

// `body` contains a try statement in the function's OWN statement tree
// (never inside nested function bodies — those get their own frames).
static bool has_own_try(const std::vector<Stmt>& body,
                        const std::set<const Stmt*>& elided) {
  for (const Stmt& s : body) {
    // C2: a try emitted WITHOUT registration (elided) holds no setjmp pad in
    // this activation, so it does not disqualify the lane — but its body and
    // arms are still scanned, because a live NESTED try cancels the lane just
    // the same.
    if (s.kind == StmtKind::Try && !elided.count(&s)) return true;
    if (s.kind == StmtKind::Func) continue;  // nested fn: its own check
    if (has_own_try(s.body, elided)) return true;
    for (const auto& b : s.bodies)
      if (has_own_try(b, elided)) return true;
    if (has_own_try(s.else_body, elided)) return true;
    if (has_own_try(s.finally_body, elided)) return true;
    for (const ExceptArm& a : s.arms)
      if (has_own_try(a.body, elided)) return true;
    // C2 repair: case-arm bodies count too — a `case ...:` armed try builds a
    // setjmp pad in this frame exactly like one under an `if`, and the
    // pre-C2 scan walked only s.body/bodies/else/finally/arms, so the old
    // gate could let stack lanes coexist with a live pad behind a switch.
    // Finding MORE trys only removes lanes (conservative by construction).
    for (const CaseArm& a : s.cases)
      if (has_own_try(a.body, elided)) return true;
  }
  return false;
}

// Collects every try STATEMENT reachable from one region root. Nested
// function/class declarations are skipped — their statements belong to
// their own FuncDecl, which is walked separately (pairs are attributed
// exactly once: (owner-fn, try-stmt)).
// NOTE: marked [[maybe_unused]] because the conservative init_try_elision()
// stub does not yet call it; the attribute is harmless once the real fixpoint
// lands and this collector is referenced again.
[[maybe_unused]] static void collect_try_stmts(const Stmt& s, std::vector<const Stmt*>& out) {
  if (s.kind == StmtKind::Func) return;
  if (s.kind == StmtKind::Try) out.push_back(&s);
  for (const Stmt& c : s.body) collect_try_stmts(c, out);
  for (const auto& b : s.bodies)
    for (const Stmt& c : b) collect_try_stmts(c, out);
  for (const Stmt& c : s.else_body) collect_try_stmts(c, out);
  for (const Stmt& c : s.finally_body) collect_try_stmts(c, out);
  for (const ExceptArm& a : s.arms)
    for (const Stmt& c : a.body) collect_try_stmts(c, out);
  for (const CaseArm& a : s.cases)
    for (const Stmt& c : a.body) collect_try_stmts(c, out);
}

// Post-2C perf: scan result of a twin candidate's body (defined before the
// joint lane fixpoint, which keys candidates by it).
struct TwinScan {
  std::vector<const Expr*> returns;       // every return expression
  std::vector<const Expr*> param_writes;  // every assignment RHS into a param
  bool ok = true;                         // structural rules all held
};

void Emitter::analyze_native_ints() {
  GlobalSlots gw;
  LocalSlots lw;

  // Top-level statements and the main() block bind globals; every function
  // body (incl. nested and class methods — complete FuncDecls in
  // all_functions) is walked with itself as the local-slot key.
  for (const TopLevel& t : prog_.items) {
    if (t.kind == TopKind::Stmt)
      walk_stmt(t.stmt, nullptr, gw, lw);
    else if (t.kind == TopKind::Main && t.main)
      walk_stmts(t.main->body, nullptr, gw, lw);
  }
  for (const FuncDecl* f : all_functions()) walk_stmts(f->body, f, gw, lw);

  // Cross-scope access (docs/reference/cross-scope-access.md §4): a pinned slot must
  // live on the ONE storage the qualified path reads — the dynamic pt_value
  // slot — so poison it (and any variable.global target) out of the native
  // lane. Conservative by one slot, exactly like every other poison.
  for (const std::string& g : prog_.pin_globals) gw[g].poisoned = true;
  for (const PinSlot& p : prog_.pins)
    if (p.owner) poison_slot(p.name, VarRes::Local, p.owner, gw, lw);

  // Candidates: declared, unpoisoned slots with at least one write.
  std::set<std::string> ig;
  std::map<const FuncDecl*, std::set<std::string>> il;
  for (const std::string& g : prog_.globals) {
    auto it = gw.find(g);
    if (it == gw.end() || it->second.poisoned || it->second.writes.empty())
      continue;
    if (is_builtin_exception_class(g)) continue;  // injected singleton write
    ig.insert(g);
  }
  for (const auto& fentry : lw) {
    const FuncDecl* f = fentry.first;
    if (!f || has_own_try(f->body, elided_trys_)) continue;  // setjmp frames stay dynamic
    for (const auto& sentry : fentry.second) {
      const SlotInfo& info = sentry.second;
      if (info.poisoned || info.writes.empty()) continue;
      size_t i = 0;
      for (; i < f->locals.size(); ++i)
        if (f->locals[i] == sentry.first) break;
      if (i == f->locals.size()) continue;  // not this function's local
      if (i < f->local_is_cell.size() && f->local_is_cell[i]) continue;
      il[f].insert(sentry.first);
    }
  }

  // ---- joint fixpoint: slot lanes × int-lane twin callees ------------------
  // int_shape's Call rule needs the set of still-viable twin callees, and
  // twin viability needs every callee local in a lane and every return
  // int-shaped — each side shrinks the other, so they must be proven
  // TOGETHER. Both are removal-only and monotone, and the iteration starts
  // at the joint top (max candidate slots, max structurally-viable
  // twins), so alternating sweeps converge to the greatest joint fixpoint.
  // (The twin pass used to run AFTER this function's slot fixpoint, so a
  // `acc = acc + f(i)` write had already evicted `acc` from the lane by the
  // time `f` could be proven int-returning — the call_direct 30x case.)
  std::map<const FuncDecl*, TwinScan> cand = twin_prefilter();
  std::set<const FuncDecl*> viable;
  for (const auto& c : cand) viable.insert(c.first);
  bool changed = true;
  while (changed) {
    changed = false;
    // Names of the still-viable callees (int_shape's twin Call rule keys
    // on RESOLVED names: a name whose find_function lands on a non-viable
    // or shadowed decl is not a twin). C2: the decl MAP rides alongside the
    // name SET so the shared call binder can order named/omitted args.
    std::set<std::string> names;
    std::map<std::string, const FuncDecl*> tdecls;
    for (const FuncDecl* f : viable)
      if (find_function(f->name) == f) {
        names.insert(f->name);
        tdecls.emplace(f->name, f);
      }
    // Slot sweep: a slot stays native only while EVERY write's RHS is
    // int-shaped under the current lanes + twin set. slot_write=false: a
    // param-reading RHS cannot be spelled by the DYNAMIC body's
    // native_int_text (internal failure) — the lane declines instead.
    for (auto& entry : gw) {
      if (!ig.count(entry.first)) continue;
      for (const SlotWrite& w : entry.second.writes)
        if (!w.int_bind &&
            !int_shape(*w.rhs, w.fn, ig, il, &names, /*slot_write=*/false,
                       &tdecls, &nativfg_, &nativfl_, &int_lists_,
                       &int_list_da_reads_)) {
          ig.erase(entry.first);
          changed = true;
          break;
        }
    }
    for (auto& fentry : lw) {
      if (!il.count(fentry.first)) continue;
      for (auto& sentry : fentry.second) {
        if (!il[fentry.first].count(sentry.first)) continue;
        for (const SlotWrite& w : sentry.second.writes)
          if (!w.int_bind &&
              !int_shape(*w.rhs, fentry.first, ig, il, &names,
                         /*slot_write=*/false, &tdecls, &nativfg_,
                         &nativfl_, &int_lists_, &int_list_da_reads_)) {
            il[fentry.first].erase(sentry.first);
            changed = true;
            break;
          }
      }
    }
    // Twin sweep under the (possibly shrunken) lanes: the twin declares NO
    // pt_value local slots, so every local must still be a lane — re-checked
    // every sweep, not just at prefilter time, because the slot sweep above
    // can evict one mid-fixpoint. Returns and param writes must stay
    // int-shaped under the current lanes + twin names.
    for (auto it = viable.begin(); it != viable.end();) {
      const FuncDecl* f = *it;
      bool ok = true;
      auto lit = il.find(f);
      for (const std::string& l : f->locals)
        if (lit == il.end() || !lit->second.count(l)) {
          ok = false;
          break;
        }
      if (ok)
        for (const Expr* r : cand[f].returns)
          if (!int_shape(*r, f, ig, il, &names, /*slot_write=*/true, &tdecls,
                         &nativfg_, &nativfl_, &int_lists_,
                         &int_list_da_reads_)) {
            ok = false;
            break;
          }
      if (ok)
        for (const Expr* w : cand[f].param_writes)
          if (!int_shape(*w, f, ig, il, &names, /*slot_write=*/true, &tdecls,
                         &nativfg_, &nativfl_, &int_lists_,
                         &int_list_da_reads_)) {
            ok = false;
            break;
          }
      if (ok) {
        ++it;
      } else {
        it = viable.erase(it);
        changed = true;
      }
    }
  }

  nativ_g_ = std::move(ig);
  nativ_l_ = std::move(il);
  // C2: run() iterates this analysis under the shrinking try-elision split,
  // so the viability registries must be rebuilt from scratch every entry —
  // accumulating would leave stale twins callable after their evicted
  // candidates (a superset = an out-of-lane call, not a conservative miss).
  nativ_fn_decls_.clear();
  nativ_fns_.clear();
  for (const FuncDecl* f : viable) {
    nativ_fn_decls_.insert(f);
    if (find_function(f->name) == f) nativ_fns_.insert(f->name);
  }
}

// ---- native float64 slot analysis (float twin of the int lane) ------------
//
// A slot (global, or unboxed local of a try-free function) is native-float
// when EVERY write to it — plain or compound, on every path, program-wide —
// is float-shaped under float_shape: a float literal, another lane-float
// expression, or a mixed op whose int operand the FINAL int lane proves
// int64. Any int-typed write (`x = 0`), any "%"/"**"/bitwise op, any
// protocol/pattern/except/class binding, any pin or cell declines the slot —
// it simply stays on the dynamic path, exactly like today. The rules
// mirror analyze_native_ints (same walk, same poison sources, same try/
// cell/pin/exception-class exclusions); the one difference: a range-loop
// bind (w.int_bind) writes a raw int64 and therefore DECLINES a float
// candidate instead of satisfying it.
//
// Runs AFTER analyze_native_ints: float_shape may use the final int lane
// (and final int-twin set) for mixed operands and float(<int64>). The int
// lane consults THIS lane only for int(<float>), so run() alternates the
// two analyses to their joint fixpoint (starting from an empty float lane).
// The candidate walk is the same walk_stmt collection; float and int slots
// are disjoint (no expression is both int-shaped and float-shaped), so no
// cross-lane eviction can occur.
void Emitter::analyze_native_floats() {
  GlobalSlots gw;
  LocalSlots lw;
  for (const TopLevel& t : prog_.items) {
    if (t.kind == TopKind::Stmt)
      walk_stmt(t.stmt, nullptr, gw, lw);
    else if (t.kind == TopKind::Main && t.main)
      walk_stmts(t.main->body, nullptr, gw, lw);
  }
  for (const FuncDecl* f : all_functions()) walk_stmts(f->body, f, gw, lw);

  // Pinned cross-scope slots (docs/reference/cross-scope-access.md §4) and class-name
  // globals stay dynamic — same poison sources as the int lane.
  for (const std::string& g : prog_.pin_globals) gw[g].poisoned = true;
  for (const PinSlot& p : prog_.pins)
    if (p.owner) poison_slot(p.name, VarRes::Local, p.owner, gw, lw);

  std::set<std::string> fg;
  std::map<const FuncDecl*, std::set<std::string>> fl;
  for (const std::string& g : prog_.globals) {
    auto it = gw.find(g);
    if (it == gw.end() || it->second.poisoned || it->second.writes.empty())
      continue;
    if (is_builtin_exception_class(g)) continue;  // injected singleton write
    if (nativ_g_.count(g)) continue;  // int-lane slot (disjoint by proof)
    fg.insert(g);
  }
  for (const auto& fentry : lw) {
    const FuncDecl* f = fentry.first;
    if (!f || has_own_try(f->body, elided_trys_)) continue;  // setjmp frames stay dynamic
    auto intit = nativ_l_.find(f);
    for (const auto& sentry : fentry.second) {
      const SlotInfo& info = sentry.second;
      if (info.poisoned || info.writes.empty()) continue;
      if (intit != nativ_l_.end() && intit->second.count(sentry.first))
        continue;  // int-lane slot
      size_t i = 0;
      for (; i < f->locals.size(); ++i)
        if (f->locals[i] == sentry.first) break;
      if (i == f->locals.size()) continue;  // not this function's local
      if (i < f->local_is_cell.size() && f->local_is_cell[i]) continue;
      fl[f].insert(sentry.first);
    }
  }

  // Removal-only monotone fixpoint: every write's RHS must stay
  // float-shaped under the shrinking candidate sets (the int lane maps and
  // twin set are final constants here). C2: decl map for the shared call
  // binder, built once from the final int-twin registries.
  std::map<std::string, const FuncDecl*> tdecls;
  for (const FuncDecl* f : nativ_fn_decls_) tdecls.emplace(f->name, f);
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto it = fg.begin(); it != fg.end();) {
      bool ok = true;
      for (const SlotWrite& w : gw[*it].writes) {
        if (w.int_bind || (!w.op.empty() && w.op != "+=" && w.op != "-=" &&
                           w.op != "*=" && w.op != "/=") ||
            !float_shape(*w.rhs, w.fn, fg, fl, nativ_g_, nativ_l_,
                         &nativ_fns_, &tdecls, &int_lists_,
                         &int_list_da_reads_)) {
          ok = false;
          break;
        }
      }
      if (ok) ++it;
      else { it = fg.erase(it); changed = true; }
    }
    for (auto& fentry : fl) {
      for (auto it = fentry.second.begin(); it != fentry.second.end();) {
        bool ok = true;
        for (const SlotWrite& w : lw[fentry.first][*it].writes) {
          if (w.int_bind || (!w.op.empty() && w.op != "+=" && w.op != "-=" &&
                             w.op != "*=" && w.op != "/=") ||
              !float_shape(*w.rhs, fentry.first, fg, fl, nativ_g_, nativ_l_,
                           &nativ_fns_, &tdecls, &int_lists_,
                           &int_list_da_reads_)) {
            ok = false;
            break;
          }
        }
        if (ok) ++it;
        else { it = fentry.second.erase(it); changed = true; }
      }
    }
  }

  nativfg_ = std::move(fg);
  nativfl_ = std::move(fl);
}

// Promote native globals whose every reference sits in the main block to
// automatic locals of pt_main_block (see the GlobalRefScan block comment).
// Runs after the lane fixpoints (only native slots are candidates) and
// before emission; analyze_definite_assign/guard elimination still see the
// same names, so noflag/guard facts are unchanged.
void Emitter::analyze_promotable_globals() {
  local_g_.clear();
  const MainDecl* main_block = find_main();
  if (!main_block) return;
  GlobalRefScan top, main, fns;
  bool has_import = false;
  for (const TopLevel& t : prog_.items) {
    if (t.kind == TopKind::Stmt) scan_stmt_refs(t.stmt, top);
    else if (t.kind == TopKind::Func && t.func) {
      /* a top-level `def` binds its name (the function value) at module
       * scope, outside pt_main_block — never promote a read of it */
      top.refs.insert(t.func->name);
      top.writes.insert(t.func->name);
    } else if (t.kind == TopKind::Import) {
      has_import = true;  // import bindings are module-scope writes we
                          // do not enumerate; decline all promotion
    }
  }
  for (const Stmt& s : main_block->body) scan_stmt_refs(s, main);
  for (const FuncDecl* f : all_functions()) {
    for (const auto& d : f->defaults)
      if (d) scan_expr_refs(*d, fns);
    for (const Stmt& s : f->body) scan_stmt_refs(s, fns);
  }
  // globals()/locals()/vars()/dir() can read a slot by name through the
  // name table, which would need the static declaration's address.
  if (has_import || top.dynamic_names || main.dynamic_names ||
      fns.dynamic_names)
    return;
  auto promote = [&](const std::set<std::string>& names) {
    for (const std::string& g : names) {
      if (!main.refs.count(g)) continue;
      if (top.refs.count(g) || fns.refs.count(g)) continue;
      local_g_.insert(g);
    }
  };
  promote(nativ_g_);
  promote(nativfg_);
  /* Boxed globals: same storage-duration proof, but they additionally need
   * a write in the main block (so they cannot be an import/definition
   * binding initialized elsewhere) and stay rooted through the main
   * frame's slot array. Pinned slots keep their static cell ABI. */
  for (const std::string& g : prog_.globals) {
    if (local_g_.count(g) || nativ_g_.count(g) || nativfg_.count(g))
      continue;
    if (is_builtin_exception_class(g)) continue;
    if (std::find(prog_.pin_globals.begin(), prog_.pin_globals.end(), g) !=
        prog_.pin_globals.end())
      continue;
    if (!main.writes.count(g)) continue;
    if (top.refs.count(g) || fns.refs.count(g)) continue;
    local_g_.insert(g);
  }
}



// ---- definite assignment for int-list read sites --------------------------
// The native list-element read uses the bare slot storage (no pt_guard), so
// the slot must be provably assigned at that exact read. This is a plain
// forward DA walk over the main block for global name bindings; branches and
// loops are walked with copies and never propagate assignments outward (the
// same conservative rule analyze_definite_assign uses for native flags).

void Emitter::il_da_expr(const Expr& e, std::set<std::string>& da) {
  switch (e.kind) {
    case ExprKind::Index:
      if (e.lhs && e.lhs->kind == ExprKind::Var &&
          e.lhs->res == VarRes::Global && da.count(e.lhs->name))
        int_list_da_reads_.insert(&e);
      if (e.lhs) il_da_expr(*e.lhs, da);
      if (e.rhs) il_da_expr(*e.rhs, da);
      return;
    case ExprKind::Slice:
      if (e.lhs) il_da_expr(*e.lhs, da);
      if (e.has_start && e.rhs) il_da_expr(*e.rhs, da);
      if (e.has_stop && e.third) il_da_expr(*e.third, da);
      if (e.has_step && e.fourth) il_da_expr(*e.fourth, da);
      return;
    case ExprKind::MethodCall:
      if (e.lhs) il_da_expr(*e.lhs, da);
      for (const auto& el : e.elems)
        if (el) il_da_expr(*el, da);
      return;
    case ExprKind::Call:
      if (e.callee_expr) il_da_expr(*e.callee_expr, da);
      for (const auto& a : e.args)
        if (a) il_da_expr(*a, da);
      return;
    case ExprKind::Attr:
      if (e.lhs) il_da_expr(*e.lhs, da);
      return;
    case ExprKind::Unary:
      if (e.lhs) il_da_expr(*e.lhs, da);
      return;
    case ExprKind::Binary:
    case ExprKind::Logical:
      if (e.lhs) il_da_expr(*e.lhs, da);
      if (e.rhs) il_da_expr(*e.rhs, da);
      return;
    case ExprKind::Ternary:
      if (e.lhs) il_da_expr(*e.lhs, da);
      if (e.rhs) il_da_expr(*e.rhs, da);
      if (e.third) il_da_expr(*e.third, da);
      return;
    case ExprKind::ListLit:
    case ExprKind::SetLit:
    case ExprKind::TupleLit:
    case ExprKind::Join:
      if (e.lhs) il_da_expr(*e.lhs, da);
      for (const auto& el : e.elems)
        if (el) il_da_expr(*el, da);
      return;
    case ExprKind::MapLit:
      for (const auto& k : e.keys)
        if (k) il_da_expr(*k, da);
      for (const auto& el : e.elems)
        if (el) il_da_expr(*el, da);
      return;
    default:
      return;
  }
}

void Emitter::il_da_target(const AssignTarget& t, std::set<std::string>& da) {
  switch (t.kind) {
    case AssignTarget::Kind::Index:
      if (t.object) il_da_expr(*t.object, da);
      if (t.index) il_da_expr(*t.index, da);
      return;
    case AssignTarget::Kind::Attr:
      if (t.object) il_da_expr(*t.object, da);
      return;
    case AssignTarget::Kind::Name:
    case AssignTarget::Kind::Rest:
      // Binding happens after the RHS/target subexpressions were evaluated.
      if (t.res == VarRes::Global) da.insert(t.name);
      return;
    case AssignTarget::Kind::Tuple:
      for (const AssignTarget& sub : t.elems) il_da_target(sub, da);
      return;
    case AssignTarget::Kind::VarQual:
      return;
  }
}

void Emitter::il_da_stmt(const Stmt& s, std::set<std::string>& da) {
  switch (s.kind) {
    case StmtKind::Assign:
      if (s.expr) il_da_expr(*s.expr, da);
      il_da_target(s.target, da);
      return;
    case StmtKind::ExprStmt:
    case StmtKind::Return:
    case StmtKind::Raise:
      if (s.expr) il_da_expr(*s.expr, da);
      return;
    case StmtKind::If: {
      for (const auto& c : s.conds)
        if (c) il_da_expr(*c, da);
      std::set<std::string> outer = da;
      for (const auto& b : s.bodies) {
        std::set<std::string> d = outer;
        il_da_stmts(b, d);
      }
      if (s.has_else) {
        std::set<std::string> d = outer;
        il_da_stmts(s.else_body, d);
      }
      return;  // no assignment escapes a branch
    }
    case StmtKind::While: {
      if (s.expr) il_da_expr(*s.expr, da);
      std::set<std::string> d = da;
      il_da_stmts(s.body, d);
      return;  // the body may not run, and repeats
    }
    case StmtKind::For: {
      if (s.expr) il_da_expr(*s.expr, da);
      std::set<std::string> d = da;
      if (s.res == VarRes::Global) d.insert(s.name);
      il_da_stmts(s.body, d);
      return;
    }
    case StmtKind::Switch: {
      if (s.expr) il_da_expr(*s.expr, da);
      for (const CaseArm& arm : s.cases) {
        std::set<std::string> d = da;
        il_da_stmts(arm.body, d);
      }
      return;
    }
    case StmtKind::Try: {
      std::set<std::string> d = da;
      il_da_stmts(s.body, d);
      for (const ExceptArm& arm : s.arms) {
        std::set<std::string> e = da;  // handler may run before the body
        if (arm.cond) il_da_expr(*arm.cond, e);
        if (arm.res == VarRes::Global && !arm.name.empty()) e.insert(arm.name);
        il_da_stmts(arm.body, e);
      }
      if (s.has_finally) {
        std::set<std::string> f = da;
        il_da_stmts(s.finally_body, f);
      }
      return;
    }
    case StmtKind::Class: {
      std::set<std::string> d = da;
      il_da_stmts(s.body, d);
      return;
    }
    case StmtKind::Func:
    case StmtKind::GlobalDecl:
    case StmtKind::NonlocalDecl:
    case StmtKind::Break:
    case StmtKind::Continue:
    case StmtKind::Pass:
      return;
  }
}

void Emitter::il_da_stmts(const std::vector<Stmt>& list,
                          std::set<std::string>& da) {
  for (const Stmt& s : list) il_da_stmt(s, da);
}

void Emitter::analyze_int_list_reads() {
  int_list_da_reads_.clear();
  const MainDecl* main_block = find_main();
  if (!main_block) return;
  std::set<std::string> da;
  il_da_stmts(main_block->body, da);
}

// ---- main-block use facts -------------------------------------------------
// One classification pass over the main block. The lazy-slice and int-list
// proofs both need to know exactly how a main-only global is touched; doing
// it once keeps the two analyses independent and makes every "other use"
// rejection explicit.

void Emitter::scan_use_expr(const Expr& e, const Stmt* st) {
  switch (e.kind) {
    case ExprKind::Var:
      if (e.res == VarRes::Global)
        main_uses_[e.name].push_back(
            MainUse{UseRole::Bare, st, &e, "", ""});
      return;
    case ExprKind::Index:
      if (e.lhs && e.lhs->kind == ExprKind::Var &&
          e.lhs->res == VarRes::Global)
        main_uses_[e.lhs->name].push_back(
            MainUse{UseRole::IndexBase, st, &e, "", ""});
      else if (e.lhs)
        scan_use_expr(*e.lhs, st);
      if (e.rhs) scan_use_expr(*e.rhs, st);
      return;
    case ExprKind::Slice:
      if (e.lhs && e.lhs->kind == ExprKind::Var &&
          e.lhs->res == VarRes::Global)
        main_uses_[e.lhs->name].push_back(
            MainUse{UseRole::SliceBase, st, &e, "", ""});
      else if (e.lhs)
        scan_use_expr(*e.lhs, st);
      if (e.has_start && e.rhs) scan_use_expr(*e.rhs, st);
      if (e.has_stop && e.third) scan_use_expr(*e.third, st);
      if (e.has_step && e.fourth) scan_use_expr(*e.fourth, st);
      return;
    case ExprKind::MethodCall:
      if (e.lhs && e.lhs->kind == ExprKind::Var &&
          e.lhs->res == VarRes::Global)
        main_uses_[e.lhs->name].push_back(
            MainUse{UseRole::MethodRecv, st, &e, e.method, ""});
      else if (e.lhs)
        scan_use_expr(*e.lhs, st);
      for (const auto& el : e.elems)
        if (el) scan_use_expr(*el, st);
      return;
    case ExprKind::Attr:
      if (e.lhs && e.lhs->kind == ExprKind::Var &&
          e.lhs->res == VarRes::Global)
        main_uses_[e.lhs->name].push_back(
            MainUse{UseRole::AttrBase, st, &e, "", ""});
      else if (e.lhs)
        scan_use_expr(*e.lhs, st);
      return;
    case ExprKind::Call:
      if (e.callee_expr) scan_use_expr(*e.callee_expr, st);
      if (e.is_builtin && !e.callee_expr && e.builtin_name == "len" &&
          e.args.size() == 1u && e.args[0] &&
          e.args[0]->kind == ExprKind::Var &&
          e.args[0]->res == VarRes::Global) {
        main_uses_[e.args[0]->name].push_back(
            MainUse{UseRole::LenArg, st, &e, "", ""});
        return;
      }
      for (const auto& a : e.args)
        if (a) scan_use_expr(*a, st);
      return;
    case ExprKind::Unary:
      if (e.lhs) scan_use_expr(*e.lhs, st);
      return;
    case ExprKind::Binary:
    case ExprKind::Logical:
      if (e.lhs) scan_use_expr(*e.lhs, st);
      if (e.rhs) scan_use_expr(*e.rhs, st);
      return;
    case ExprKind::Ternary:
      if (e.lhs) scan_use_expr(*e.lhs, st);
      if (e.rhs) scan_use_expr(*e.rhs, st);
      if (e.third) scan_use_expr(*e.third, st);
      return;
    case ExprKind::ListLit:
    case ExprKind::SetLit:
    case ExprKind::TupleLit:
    case ExprKind::Join:
      if (e.lhs) scan_use_expr(*e.lhs, st);
      for (const auto& el : e.elems)
        if (el) scan_use_expr(*el, st);
      return;
    case ExprKind::MapLit:
      for (const auto& k : e.keys)
        if (k) scan_use_expr(*k, st);
      for (const auto& el : e.elems)
        if (el) scan_use_expr(*el, st);
      return;
    case ExprKind::VarQual:
      return;
    default:
      return;
  }
}

void Emitter::scan_use_target(const AssignTarget& t, const Stmt* st,
                              bool unpack) {
  switch (t.kind) {
    case AssignTarget::Kind::Name:
    case AssignTarget::Kind::Rest:
      if (t.res == VarRes::Global) {
        if (unpack || t.kind == AssignTarget::Kind::Rest)
          main_uses_[t.name].push_back(
              MainUse{UseRole::WriteUnknown, st, st->expr.get(), "", ""});
        else
          main_uses_[t.name].push_back(
              MainUse{UseRole::Write, st, st->expr.get(), "", st->op});
      }
      return;
    case AssignTarget::Kind::Index:
      if (t.object && t.object->kind == ExprKind::Var &&
          t.object->res == VarRes::Global)
        main_uses_[t.object->name].push_back(
            MainUse{UseRole::IndexWrite, st, st->expr.get(), "", st->op});
      else if (t.object)
        scan_use_expr(*t.object, st);
      if (t.index) scan_use_expr(*t.index, st);
      return;
    case AssignTarget::Kind::Attr:
      if (t.object && t.object->kind == ExprKind::Var &&
          t.object->res == VarRes::Global)
        main_uses_[t.object->name].push_back(
            MainUse{UseRole::AttrWrite, st, st->expr.get(), "", ""});
      else if (t.object)
        scan_use_expr(*t.object, st);
      return;
    case AssignTarget::Kind::Tuple:
      for (const AssignTarget& sub : t.elems)
        scan_use_target(sub, st, /*unpack=*/true);
      return;
    case AssignTarget::Kind::VarQual:
      return;
  }
}

void Emitter::scan_use_stmt(const Stmt& s) {
  switch (s.kind) {
    case StmtKind::Assign:
      if (s.expr) scan_use_expr(*s.expr, &s);
      scan_use_target(s.target, &s, /*unpack=*/false);
      return;
    case StmtKind::ExprStmt:
    case StmtKind::Return:
    case StmtKind::Raise:
      if (s.expr) scan_use_expr(*s.expr, &s);
      return;
    case StmtKind::If:
      for (const auto& c : s.conds)
        if (c) scan_use_expr(*c, &s);
      for (const auto& b : s.bodies) scan_use_stmts(b);
      if (s.has_else) scan_use_stmts(s.else_body);
      return;
    case StmtKind::While:
      if (s.expr) scan_use_expr(*s.expr, &s);
      scan_use_stmts(s.body);
      return;
    case StmtKind::For:
      if (s.expr) scan_use_expr(*s.expr, &s);
      if (s.res == VarRes::Global)
        main_uses_[s.name].push_back(
            MainUse{UseRole::WriteUnknown, &s, nullptr, "", ""});
      scan_use_stmts(s.body);
      return;
    case StmtKind::Switch:
      if (s.expr) scan_use_expr(*s.expr, &s);
      for (const CaseArm& arm : s.cases) {
        switch_arm_lists_.insert(&arm.body);
        scan_use_stmts(arm.body);
      }
      return;
    case StmtKind::Try:
      main_has_try_ = true;
      scan_use_stmts(s.body);
      for (const ExceptArm& arm : s.arms) {
        if (arm.cond) scan_use_expr(*arm.cond, &s);
        if (arm.res == VarRes::Global && !arm.name.empty())
          main_uses_[arm.name].push_back(
              MainUse{UseRole::WriteUnknown, &s, nullptr, "", ""});
        scan_use_stmts(arm.body);
      }
      if (s.has_finally) scan_use_stmts(s.finally_body);
      return;
    case StmtKind::Func:
      return;  // nested function body: captured names keep the slot pinned
    case StmtKind::Class:
      if (s.res == VarRes::Global)
        main_uses_[s.name].push_back(
            MainUse{UseRole::WriteUnknown, &s, nullptr, "", ""});
      scan_use_stmts(s.body);
      return;
    case StmtKind::GlobalDecl:
    case StmtKind::NonlocalDecl:
    case StmtKind::Break:
    case StmtKind::Continue:
    case StmtKind::Pass:
      return;
  }
}

void Emitter::scan_use_stmts(const std::vector<Stmt>& list) {
  const std::vector<Stmt>* saved = scan_list_;
  scan_list_ = &list;
  for (size_t i = 0; i < list.size(); ++i) {
    const Stmt& st = list[i];
    stmt_id_[&st] = main_stmt_next_id_++;
    stmt_pos_[&st] = i;
    stmt_list_[&st] = &list;
    scan_use_stmt(st);
  }
  scan_list_ = saved;
}

void Emitter::scan_main_uses() {
  main_uses_.clear();
  stmt_id_.clear();
  stmt_pos_.clear();
  stmt_list_.clear();
  main_stmt_next_id_ = 0;
  main_has_try_ = false;
  switch_arm_lists_.clear();
  const MainDecl* main_block = find_main();
  if (main_block) scan_use_stmts(main_block->body);
}


// ---- int-only list facts --------------------------------------------------
// A main-only global is int-only when every value ever stored into it is an
// int64: an empty/all-int list literal, list(), a slice/copy of another
// int-only list, or an int-shaped append/insert/extend/index-assignment.
// Its uses may only be element reads, slicing, len, and list methods that
// cannot introduce a non-int element. Any bare read (alias, argument, store,
// iteration, print) disqualifies it: an alias could mutate the list through
// a path this name-based proof cannot see.
//
// The proof is a LEAST fixpoint starting from the empty set, so a circular
// self-support (`xs = []; xs.append(xs[0])`) is accepted only from the first
// round's genuinely-int base, never assumed. Each round is sound for the
// previous set; stopping early only loses optimization.

// True when a value written by `g = expr` is a freshly allocated flat list
// (stride 0). Only called after int_list_expr proved the value is an int list,
// so the element types are already known to be PT_INT.
bool Emitter::flat_init_expr(const Expr& e) const {
  switch (e.kind) {
    case ExprKind::ListLit:
      return true;
    case ExprKind::Call:
      return e.is_builtin && !e.callee_expr && e.builtin_name == "list" &&
             e.args.empty();
    case ExprKind::MethodCall:
      return e.method == "copy" && e.elems.empty() && e.lhs &&
             e.lhs->kind == ExprKind::Var && e.lhs->res == VarRes::Global;
    default:
      return false;  // slices are views; bare vars are not int-list inits
  }
}

bool Emitter::int_list_expr(const Expr& e,
                            const std::set<std::string>& cur) const {
  switch (e.kind) {
    case ExprKind::ListLit:
      for (const auto& el : e.elems)
        if (!el ||
            !int_shape(*el, nullptr, nativ_g_, nativ_l_, nullptr,
                       /*slot_write=*/true, nullptr, &nativfg_, &nativfl_,
                       &cur, &int_list_da_reads_))
          return false;
      return true;
    case ExprKind::Call:
      return e.is_builtin && !e.callee_expr && e.builtin_name == "list" &&
             e.args.empty();
    case ExprKind::Slice:
      return e.lhs && e.lhs->kind == ExprKind::Var &&
             e.lhs->res == VarRes::Global && cur.count(e.lhs->name) != 0;
    case ExprKind::MethodCall:
      return e.method == "copy" && e.elems.empty() && e.lhs &&
             e.lhs->kind == ExprKind::Var &&
             e.lhs->res == VarRes::Global && cur.count(e.lhs->name) != 0;
    default:
      return false;
  }
}

bool Emitter::int_list_method(const Expr& call,
                              const std::set<std::string>& cur) const {
  if (call.kind != ExprKind::MethodCall || !call.lhs) return false;
  auto arg_int = [&](size_t i) {
    return i < call.elems.size() && call.elems[i] &&
           int_shape(*call.elems[i], nullptr, nativ_g_, nativ_l_, nullptr,
                     /*slot_write=*/true, nullptr, &nativfg_, &nativfl_,
                     &cur, &int_list_da_reads_);
  };
  auto arg_list = [&](size_t i) {
    return i < call.elems.size() && call.elems[i] &&
           int_list_expr(*call.elems[i], cur);
  };
  const std::string& m = call.method;
  if (m == "append") return call.elems.size() == 1u && arg_int(0);
  if (m == "insert") return call.elems.size() == 2u && arg_int(1);
  if (m == "extend") return call.elems.size() == 1u && arg_list(0);
  if (m == "pop") return call.elems.size() <= 1u;
  if (m == "clear" || m == "reverse" || m == "sort" || m == "copy")
    return call.elems.empty();
  if (m == "remove") return call.elems.size() == 1u;
  if (m == "count" || m == "index") return !call.elems.empty();
  return false;  // any other method may add/replace elements with anything
}

bool Emitter::analyze_int_lists() {
  std::set<std::string> cur;
  std::set<std::string> flat;
  for (int round = 0; round < 16; ++round) {
    std::set<std::string> next;
    std::set<std::string> next_flat;
    for (const std::string& g : prog_.globals) {
      if (!local_g_.count(g)) continue;
      if (nativ_g_.count(g) || nativfg_.count(g)) continue;
      auto it = main_uses_.find(g);
      if (it == main_uses_.end()) continue;
      bool ok = true, has_list_init = false;
      bool flat_ok = true, has_flat_init = false;
      for (const MainUse& u : it->second) {
        switch (u.role) {
          case UseRole::Write: {
            if (!u.expr) { ok = false; break; }
            if (u.op.empty() || u.op == "+=") {
              if (!int_list_expr(*u.expr, cur)) { ok = false; break; }
              has_list_init = true;
              if (u.op.empty()) {
                if (flat_init_expr(*u.expr))
                  has_flat_init = true;
                else
                  flat_ok = false;
              }
              // "+=" extends the existing object in place; its view fields
              // (in particular stride 0) are preserved.
            } else {
              ok = false;
            }
            break;
          }
          case UseRole::IndexBase:
          case UseRole::SliceBase:
          case UseRole::LenArg:
            break;
          case UseRole::MethodRecv:
            if (!u.expr || !int_list_method(*u.expr, cur)) ok = false;
            break;
          case UseRole::IndexWrite: {
            if (!u.expr) { ok = false; break; }
            bool value_int = int_shape(
                *u.expr, nullptr, nativ_g_, nativ_l_, nullptr,
                /*slot_write=*/true, nullptr, &nativfg_, &nativfl_, &cur,
                &int_list_da_reads_);
            if (u.op.empty() || u.op == "+=" || u.op == "-=" ||
                u.op == "*=" || u.op == "%=" || u.op == "&=" || u.op == "|=" ||
                u.op == "^=" || u.op == "<<=" || u.op == ">>=") {
              if (!value_int) ok = false;
            } else {
              ok = false;
            }
            break;
          }
          default:
            ok = false;  // Bare / AttrBase / AttrWrite / WriteUnknown
            break;
        }
        if (!ok) break;
      }
      if (ok && has_list_init) {
        next.insert(g);
        // Must hold for every value the name can hold, so require at least
        // one flat initializer and no non-flat (slice-view) initializer.
        if (flat_ok && has_flat_init) next_flat.insert(g);
      }
    }
    bool same = (next == cur) && (next_flat == flat);
    cur = std::move(next);
    flat = std::move(next_flat);
    if (same) break;
  }
  bool changed = (cur != int_lists_) || (flat != flat_lists_);
  int_lists_ = std::move(cur);
  flat_lists_ = std::move(flat);
  return changed;
}


// ---- lazy (ephemeral) slice selection -------------------------------------
// Validation is whole-block and whole-lifetime: one slice write, reads only
// via target[idx] later in the SAME statement list, and no source mutation
// after the snapshot statement. A source read is harmless; a source alias,
// store, or mutating method after the snapshot would make the lazy bounds
// observe a newer list than the eager slice would have copied.

void Emitter::analyze_ephemeral_slices() {
  ephem_.clear();
  ephem_active_.clear();
  ephem_seq_ = 0;
  if (main_has_try_) return;  // a handler could observe the skipped store
  if (!find_main()) return;

  for (const auto& kv : main_uses_) {
    const std::string& target = kv.first;
    if (!local_g_.count(target)) continue;
    if (nativ_g_.count(target) || nativfg_.count(target)) continue;

    // Exactly one write, and every other mention is an element read.
    const MainUse* wr = nullptr;
    bool ok = true;
    for (const MainUse& u : kv.second) {
      if (u.role == UseRole::Write) {
        if (wr) { ok = false; break; }
        wr = &u;
      } else if (u.role != UseRole::IndexBase) {
        ok = false;
        break;
      }
    }
    if (!ok || !wr || !wr->stmt || !wr->expr || !wr->op.empty()) continue;
    const Stmt* st = wr->stmt;
    if (st->kind != StmtKind::Assign) continue;
    if (st->target.kind != AssignTarget::Kind::Name) continue;
    if (st->target.name != target ||
        st->target.res != VarRes::Global)
      continue;
    if (!st->expr || st->expr->kind != ExprKind::Slice) continue;
    const Expr& sl = *st->expr;
    if (!sl.lhs || sl.lhs->kind != ExprKind::Var ||
        sl.lhs->res != VarRes::Global)
      continue;
    const std::string& src = sl.lhs->name;
    if (src == target) continue;
    if (!local_g_.count(src)) continue;
    if (nativ_g_.count(src) || nativfg_.count(src)) continue;
    // The fast span path assumes a list source; int_lists_ is exactly that
    // proof and also implies every element is PT_INT for the native reads.
    if (!int_lists_.count(src)) continue;
    if (sl.has_step && sl.fourth &&
        (sl.fourth->kind != ExprKind::IntLit || sl.fourth->ival <= 0))
      continue;  // negative/dynamic steps keep the general pt_slice path
    auto bound_int = [&](const Expr* b) {
      return b && int_shape(*b, cur_func_, nativ_g_, nativ_l_, nullptr,
                            /*slot_write=*/true, nullptr, &nativfg_, &nativfl_,
                            &int_lists_, &int_list_da_reads_);
    };
    if (sl.has_start && sl.rhs && !bound_int(sl.rhs.get())) continue;
    if (sl.has_stop && sl.third && !bound_int(sl.third.get())) continue;

    // Source: reads are free; mutations must all precede the snapshot.
    auto sit = main_uses_.find(src);
    if (sit == main_uses_.end()) continue;
    auto sid = stmt_id_.find(st);
    if (sid == stmt_id_.end()) continue;
    const size_t src_id = sid->second;
    for (const MainUse& u : sit->second) {
      switch (u.role) {
        case UseRole::IndexBase:
        case UseRole::SliceBase:
        case UseRole::LenArg:
          break;
        case UseRole::Write:
        case UseRole::IndexWrite:
        case UseRole::MethodRecv: {
          auto uid = stmt_id_.find(u.stmt);
          if (uid == stmt_id_.end() || uid->second >= src_id) ok = false;
          break;
        }
        case UseRole::Bare:
        case UseRole::AttrBase:
        case UseRole::AttrWrite:
        case UseRole::WriteUnknown:
        default:
          ok = false;
          break;
      }
      if (!ok) break;
    }
    if (!ok) continue;

    // Reads must follow the assignment in the same statement list. Switch
    // arms are refused because a declaration after a case label is not
    // valid C without an arm block.
    auto lit = stmt_list_.find(st);
    auto pit = stmt_pos_.find(st);
    if (lit == stmt_list_.end() || pit == stmt_pos_.end()) continue;
    const std::vector<Stmt>* list = lit->second;
    if (switch_arm_lists_.count(list)) continue;
    const size_t pos = pit->second;
    for (const MainUse& u : kv.second) {
      if (u.role != UseRole::IndexBase) continue;
      auto uit = stmt_list_.find(u.stmt);
      auto upos = stmt_pos_.find(u.stmt);
      if (uit == stmt_list_.end() || uit->second != list ||
          upos == stmt_pos_.end() || upos->second <= pos) {
        ok = false;
        break;
      }
    }
    if (!ok) continue;

    EphemSlice es;
    es.target = target;
    es.src = src;
    const std::string tag = "pt_ep" + std::to_string(ephem_seq_++);
    // No write to the source may follow the snapshot, and main-only globals
    // keep their storage in pt_main_block's rooted frame; reads can address
    // that storage directly instead of copying the value per slice.
    es.psrc = "pt_g_" + src;
    es.flat = flat_lists_.count(src) != 0;
    es.items = tag + "_items";
    es.stride = tag + "_stride";
    es.len = tag + "_len";
    es.span = tag + "_span";
    es.bstart = tag + "_bs";
    es.bstop = tag + "_be";
    es.step_lit = (sl.has_step && sl.fourth)
                      ? std::to_string(sl.fourth->ival)
                      : "1";
    ephem_[st] = std::move(es);
  }

  // A selected target used as another selected slice's source would read a
  // slot that is never written. Reject the whole group (chains are rare).
  std::set<std::string> chosen;
  for (const auto& e : ephem_) chosen.insert(e.second.target);
  for (const auto& e : ephem_)
    if (chosen.count(e.second.src)) {
      ephem_.clear();
      break;
    }
  if (ephem_.empty()) return;

  // Candidates in textual order. The earliest snapshot of a (block, source)
  // pair emits the raw view locals; later snapshots in the same block reuse
  // them — the source has no writes after the earliest snapshot, so the
  // fields stay valid for the whole block.
  std::vector<const Stmt*> order;
  order.reserve(ephem_.size());
  for (const auto& e : ephem_) order.push_back(e.first);
  std::sort(order.begin(), order.end(), [&](const Stmt* a, const Stmt* b) {
    return stmt_id_.at(a) < stmt_id_.at(b);
  });
  std::map<std::pair<const std::vector<Stmt>*, std::string>, size_t> first_pos;
  std::map<std::pair<const std::vector<Stmt>*, std::string>, const EphemSlice*>
      base_step1;
  for (const Stmt* st : order) {
    EphemSlice& es = ephem_[st];
    auto key = std::make_pair(stmt_list_.at(st), es.src);
    auto bit = base_step1.find(key);
    if (bit != base_step1.end() && !bit->second->bstart.empty() &&
        !bit->second->bstop.empty() && !es.bstart.empty() &&
        !es.bstop.empty()) {
      es.base_bs = bit->second->bstart;
      es.base_be = bit->second->bstop;
      es.base_span = bit->second->span;
    }
    EphemFacts& f = ephem_facts_[key];
    if (f.items.empty()) {
      f.items = es.items;
      f.stride = es.stride;
      f.len = es.len;
      f.flat = es.flat;
      first_pos[key] = stmt_pos_.at(st);
    } else {
      es.items = f.items;
      es.stride = f.stride;
      es.len = f.len;
      es.flat = f.flat;
      es.own_facts = false;
    }
    if (es.step_lit == "1" && !base_step1.count(key))
      base_step1[key] = &es;
  }

  // Plain source reads in the same block after the snapshot can also use the
  // raw fields (the source is write-free there). Reads in other blocks keep
  // the ordinary guarded/list helpers.
  for (const auto& kv : main_uses_) {
    for (const MainUse& u : kv.second) {
      if (u.role != UseRole::IndexBase || !u.expr || !u.stmt) continue;
      if (u.expr->kind != ExprKind::Index) continue;
      auto lit = stmt_list_.find(u.stmt);
      auto pit = stmt_pos_.find(u.stmt);
      if (lit == stmt_list_.end() || pit == stmt_pos_.end()) continue;
      auto key = std::make_pair(lit->second, kv.first);
      auto fit = ephem_facts_.find(key);
      if (fit == ephem_facts_.end()) continue;
      auto fp = first_pos.find(key);
      if (fp == first_pos.end() || pit->second < fp->second) continue;
      ephem_index_facts_[u.expr] = fit->second;
    }
  }
}

void Emitter::emit_ephemeral_slice(const Stmt& s, const EphemSlice& es,
                                   bool in_function) {
  (void)in_function;  // candidates are main-only by construction
  const Expr& sl = *s.expr;
  const bool present_start = sl.has_start && sl.rhs;
  const bool present_stop = sl.has_stop && sl.third;
  const std::string ln = std::to_string(sl.loc.line);

  // Source read first (guard included, exactly as emit_expr(Var) would).
  // The guard runs for its NameError side effect; the value is then addressed
  // through its rooted main-frame storage (no per-slice copy or slot).
  line(ind() + "(void)" +
       guard_text(es.psrc, sl.lhs->name, VarRes::Global, sl.loc.line) + ";");
  // The proof forbids source writes after this point, so the view fields are
  // stable for every fused read; snapshot them into raw locals once.
  if (es.own_facts) {
    line(ind() + "pt_value* " + es.items + " = " + es.psrc +
         ".as.o->as.seq.items;");
    if (!es.flat)
      line(ind() + "ptrdiff_t " + es.stride + " = " + es.psrc +
           ".as.o->as.seq.stride;");
    line(ind() + "size_t " + es.len + " = " + es.psrc + ".as.o->as.seq.len;");
  }

  // Evaluate each present bound into a raw int64 local in source order.
  // The analysis proved int_shape, so the native rendering cannot fail.
  if (present_start) {
    std::string b = native_int_text(*sl.rhs);
    if (b.empty()) fail("ephemeral slice start lost its native rendering");
    line(ind() + "int64_t " + es.bstart + " = " + b + ";");
  }
  if (present_stop) {
    std::string b = native_int_text(*sl.third);
    if (b.empty()) fail("ephemeral slice stop lost its native rendering");
    line(ind() + "int64_t " + es.bstop + " = " + b + ";");
  }
  if (!es.base_span.empty()) {
    line(ind() + "pt_slice_span " + es.span + ";");
    line(ind() + "if (" + es.bstart + " == " + es.base_bs + " && " +
         es.bstop + " == " + es.base_be + ") {");
    line(ind() + "  " + es.span + ".first = " + es.base_span + ".first;");
    line(ind() + "  " + es.span + ".count = (" + es.base_span +
         ".count + (uint64_t)" + es.step_lit + " - 1u) / (uint64_t)" +
         es.step_lit + ";");
    line(ind() + "} else {");
    line(ind() + "  " + es.span + " = pt_slice_span_n(" + es.len + ", " +
         es.bstart + ", 1, " + es.bstop + ", 1, " + es.step_lit + ");");
    line(ind() + "}");
  } else {
    line(ind() + "pt_slice_span " + es.span + " = pt_slice_span_n(" +
         es.len + ", " + (present_start ? es.bstart : "0") + ", " +
         (present_start ? "1" : "0") + ", " +
         (present_stop ? es.bstop : "0") + ", " +
         (present_stop ? "1" : "0") + ", " + es.step_lit + ");");
  }
  ephem_active_[es.target] = &es;
}

// ---- SIMD fusion: fvec/ivec elementwise kernel collapsing ------------------
//
// WHY: a chain like `y = a * x + y` over first-class vectors costs one
// heap temporary PER operator under the dynamic path — every intermediate
// is allocated, written, read back, and collected (memory traffic, not
// arithmetic, is the cost). The runtime operators are themselves SIMD,
// but no auto-vectorizer can fuse across separately-called loops. This
// pass proves which slots only ever hold fvec/ivec values and compiles
// their elementwise assignments to a SINGLE fused kernel loop over clang
// vector-extension lanes (PT_VW wide, scalar tail) — Halide/XLA-style
// intermediate elimination, with vectorization guaranteed by explicit
// lanes instead of hoped for.
//
// PROOF RULES (the slot-kind fixpoint; masks: 1 = fvec, 2 = ivec):
//  * A candidate slot (global, or non-cell local of a try-free function,
//    never pinned) starts maximally permissive (mask 3) and INTERSECTS the
//    kind of every write. A plain write must be kind-shaped ('f'/'i');
//    a scalar-shaped or unshaped plain write removes the slot; a compound
//    write (`+=`/`-=`/`*=`) constrains only via its RHS shape (a scalar
//    RHS is a broadcast and preserves the slot kind); `/=`/`%=` and
//    range-loop binds remove the slot (v1: no fused division).
//  * fvec_shape walks expressions over: proven vector slots, int-lane
//    scalars (nativ_l_/nativ_g_ — their reads broadcast), int/float
//    literals, unary `-`, and `+ - *` over shape-compatible sides.
//    fvec mixing with ivec is NOT shaped (the dynamic path raises the
//    standard TypeError).
//  * At EMISSION the same shape is re-checked against the final maps, so
//    a slot that lost its kind in the fixpoint never fuses — the proof is
//    the whole type contract. Slots stay dynamic pt_value storage; only
//    the expression lowering changes.
//
// Semantics: per-element arithmetic is exactly the dynamic operators'
// (same IEEE ops; int64 lanes wrap like pt_wrap_*), and a length
// mismatch between operands raises the same 'fvec/ivec length mismatch'
// ValueError at the same line — evaluated BEFORE any element is written.
int Emitter::fvec_shape(const Expr& e, const FuncDecl* fn) const {
  switch (e.kind) {
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
      return 0;  // numeric literal: broadcasts (bool is NOT numeric here)
    case ExprKind::Var: {
      if (e.res == VarRes::Global) {
        auto it = nativ_fvg_.find(e.name);
        if (it != nativ_fvg_.end()) return it->second;
      } else if (e.res == VarRes::Local && fn) {
        auto it = nativ_fvl_.find(fn);
        if (it != nativ_fvl_.end()) {
          auto jt = it->second.find(e.name);
          if (jt != it->second.end()) return jt->second;
        }
      }
      if (int_shape(e, fn, nativ_g_, nativ_l_, nullptr, true, nullptr,
                    &nativfg_, &nativfl_, &int_lists_,
                    &int_list_da_reads_)) return 0;  // int-lane scalar
      return -1;
    }
    case ExprKind::Unary:
      if (e.op == "-" && e.lhs) {
        int k = fvec_shape(*e.lhs, fn);
        return (k == 1 || k == 2) ? k : (k == 0 ? 0 : -1);
      }
      return -1;
    case ExprKind::Binary: {
      if (e.op != "+" && e.op != "-" && e.op != "*") return -1;
      if (!e.lhs || !e.rhs) return -1;
      int a = fvec_shape(*e.lhs, fn), b = fvec_shape(*e.rhs, fn);
      if (a < 0 || b < 0) return -1;
      if (a == 0) return b;             // scalar <op> vec → vec kind
      if (b == 0) return a;
      return a == b ? a : -1;           // fvec+ivec stays dynamic
    }
    case ExprKind::Call: {
      // The four vector constructors produce their kind; nothing else
      // (dot/sum/len reduce or count, user calls are opaque).
      if (!e.is_builtin) return -1;
      const std::string& n = e.builtin_name;
      if (n == "fvec" || n == "fvec_fill") return 1;
      if (n == "ivec" || n == "ivec_fill") return 2;
      return -1;
    }
    default:
      return -1;
  }
}

bool Emitter::fvec_slot_kind(const std::string& name, VarRes res,
                             const FuncDecl* fn, int& kind) const {
  if (res == VarRes::Global) {
    auto it = nativ_fvg_.find(name);
    if (it == nativ_fvg_.end()) return false;
    kind = it->second;
    return true;
  }
  if (res == VarRes::Local && fn) {
    auto it = nativ_fvl_.find(fn);
    if (it == nativ_fvl_.end()) return false;
    auto jt = it->second.find(name);
    if (jt == it->second.end()) return false;
    kind = jt->second;
    return true;
  }
  return false;  // params, qualified targets: dynamic
}

void Emitter::analyze_native_fvecs() {
  GlobalSlots gw;
  LocalSlots lw;
  for (const TopLevel& t : prog_.items) {
    if (t.kind == TopKind::Stmt)
      walk_stmt(t.stmt, nullptr, gw, lw);
    else if (t.kind == TopKind::Main && t.main)
      walk_stmts(t.main->body, nullptr, gw, lw);
  }
  for (const FuncDecl* f : all_functions()) walk_stmts(f->body, f, gw, lw);

  // Pinned cross-scope slots read through cells/statics — never fused.
  for (const std::string& g : prog_.pin_globals) gw[g].poisoned = true;
  for (const PinSlot& p : prog_.pins)
    if (p.owner) poison_slot(p.name, VarRes::Local, p.owner, gw, lw);

  struct Cand {
    int mask = 3;  // 1 = fvec possible, 2 = ivec possible (start permissive)
  };
  std::map<std::string, Cand> ig;
  std::map<const FuncDecl*, std::map<std::string, Cand>> il;
  for (const std::string& g : prog_.globals) {
    auto it = gw.find(g);
    if (it == gw.end() || it->second.poisoned || it->second.writes.empty())
      continue;
    if (is_builtin_exception_class(g)) continue;
    ig[g];
  }
  for (const auto& fentry : lw) {
    const FuncDecl* f = fentry.first;
    if (!f || has_own_try(f->body, elided_trys_)) continue;
    for (const auto& sentry : fentry.second) {
      const SlotInfo& info = sentry.second;
      if (info.poisoned || info.writes.empty()) continue;
      size_t i = 0;
      for (; i < f->locals.size(); ++i)
        if (f->locals[i] == sentry.first) break;
      if (i == f->locals.size()) continue;
      if (i < f->local_is_cell.size() && f->local_is_cell[i]) continue;
      il[f][sentry.first];
    }
  }

  // Shape of a write under the in-flight candidate maps (mirror of
  // fvec_shape with the fixpoint's permissive masks).
  std::function<int(const Expr&, const FuncDecl*)> wshape =
      [&](const Expr& e, const FuncDecl* fn) -> int {
    switch (e.kind) {
      case ExprKind::IntLit:
      case ExprKind::FloatLit:
        return 0;
      case ExprKind::Var: {
        if (e.res == VarRes::Global) {
          auto it = ig.find(e.name);
          if (it != ig.end()) return it->second.mask;
        } else if (e.res == VarRes::Local && fn) {
          auto it = il.find(fn);
          if (it != il.end()) {
            auto jt = it->second.find(e.name);
            if (jt != it->second.end()) return jt->second.mask;
          }
        }
        if (int_shape(e, fn, nativ_g_, nativ_l_, nullptr, true, nullptr,
                      &nativfg_, &nativfl_, &int_lists_,
                      &int_list_da_reads_)) return 0;
        return -1;
      }
      case ExprKind::Unary:
        if (e.op == "-" && e.lhs) {
          int k = wshape(*e.lhs, fn);
          return (k == 1 || k == 2) ? k : (k == 0 ? 0 : -1);
        }
        return -1;
      case ExprKind::Binary: {
        if (e.op != "+" && e.op != "-" && e.op != "*") return -1;
        if (!e.lhs || !e.rhs) return -1;
        int a = wshape(*e.lhs, fn), b = wshape(*e.rhs, fn);
        if (a < 0 || b < 0) return -1;
        if (a == 0) return b;
        if (b == 0) return a;
        return a == b ? a : -1;
      }
      case ExprKind::Call: {
        if (!e.is_builtin) return -1;
        const std::string& n = e.builtin_name;
        if (n == "fvec" || n == "fvec_fill") return 1;
        if (n == "ivec" || n == "ivec_fill") return 2;
        return -1;
      }
      default:
        return -1;
    }
  };

  auto dead = [](Cand& c) { c.mask = 0; };
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto& entry : gw) {
      auto it = ig.find(entry.first);
      if (it == ig.end() || it->second.mask == 0) continue;
      for (const SlotWrite& w : entry.second.writes) {
        if (w.int_bind) { dead(it->second); break; }
        int m = wshape(*w.rhs, w.fn);
        if (w.op.empty()) {
          if (m != 1 && m != 2) { dead(it->second); break; }
          it->second.mask &= m;
        } else {
          if (w.op == "/=" || w.op == "%=" || m < 0) { dead(it->second); break; }
          if (m == 1 || m == 2) it->second.mask &= m;
        }
        if (it->second.mask == 0) break;
      }
      if (it != ig.end() && it->second.mask == 0) {
        ig.erase(it);
        changed = true;
      }
    }
    for (auto& fentry : il) {
      for (auto& sentry : fentry.second) {
        if (sentry.second.mask == 0) continue;
        const std::vector<SlotWrite>* writes = nullptr;
        {
          auto gwit = lw.find(fentry.first);
          if (gwit != lw.end()) {
            auto sit = gwit->second.find(sentry.first);
            if (sit != gwit->second.end()) writes = &sit->second.writes;
          }
        }
        if (!writes) continue;
        for (const SlotWrite& w : *writes) {
          if (w.int_bind) { dead(sentry.second); break; }
          int m = wshape(*w.rhs, w.fn);
          if (w.op.empty()) {
            if (m != 1 && m != 2) { dead(sentry.second); break; }
            sentry.second.mask &= m;
          } else {
            if (w.op == "/=" || w.op == "%=" || m < 0) { dead(sentry.second); break; }
            if (m == 1 || m == 2) sentry.second.mask &= m;
          }
          if (sentry.second.mask == 0) break;
        }
      }
      // Erase dead locals so the next pass skips them.
      for (auto it = fentry.second.begin(); it != fentry.second.end();) {
        if (it->second.mask == 0) { it = fentry.second.erase(it); changed = true; }
        else ++it;
      }
    }
  }

  // Final: keep exactly the single-kind survivors.
  for (auto it = ig.begin(); it != ig.end();) {
    if (it->second.mask == 1 || it->second.mask == 2)
      { nativ_fvg_[it->first] = it->second.mask; ++it; }
    else
      it = ig.erase(it);
  }
  for (auto& fentry : il) {
    for (auto& sentry : fentry.second) {
      if (sentry.second.mask == 1 || sentry.second.mask == 2)
        nativ_fvl_[fentry.first][sentry.first] = sentry.second.mask;
    }
  }
}

// ---- text-cat in-place growth (perf: text_build's `s = s + chunk`) --------
//
// pt_text_cat2(a, b, line, may_grow=1) may append b into a's OWN buffer
// (while the block carries spare capacity) — memory is mutated through the
// destination slot alone. That is unobservable exactly when no other live
// reference to the block can exist, so a candidate statement (`name =
// name + expr` with the Binary's lhs exactly the target slot, or `name +=
// expr`) is admitted to catok_ only when EVERY access to its slot falls
// under one of these holds:
//
//   slot shape    an ordinary global or the function's own local — no
//                 env cell (captured), no pinned cross-scope static, no
//                 param/vararg/kwarg (an incoming caller value is already
//                 aliased in the caller);
//   writes        every OTHER write to the slot is an immediate Text/
//                 Bytes literal: the value never arrives from user code (a
//                 call result, another variable, a container element, a
//                 for/except/destructure/class binding could hand over a
//                 value a second slot also holds — and cat2's fresh-block
//                 path would abandon that shared buffer while the other
//                 holder still points into it). Immediate NON-text
//                 literals decline as well: they mark the slot as something
//                 other than text, keeping dynamic int counters on the
//                 inline pt_add_f fast path;
//   reads         every read is either the candidate's own concat operand
//                 (the `s` in `s = s + ...`, evaluated before the append
//                 and consumed by it — including `s = s + s` and `s += s`)
//                 or a single positional len() argument (which retains
//                 nothing past the call). Anything else — `t = s`, `f(s)`,
//                 `return s`, a method receiver, a loop iterable, a
//                 container key/index, a condition — could carry the buffer
//                 past the append: under copy-every-time semantics `t is s`
//                 diverges right there, and a retained interior value could
//                 even point at a block the grow path dropped. Reads inside
//                 pattern targets (object/index positions) are reads too;
//                 only pattern LEAVES are writes, handled by the write rule;
//   scope tables  no globals()/locals()/vars()/dir() name table (which
//                 hands out the slot's STORAGE, not a copy) exists in
//                 reach — program-wide for globals, the function's own
//                 tree for locals — and no cross-scope `variable.` access
//                 names the slot (declined by name, program-wide).
//
// With those holds the buffer has exactly one root (the slot): an in-place
// append changes bytes no other live value can name, pt_text_cat2 restores
// the slot's NUL before returning, and every result is byte-identical to
// pt_concat's — only buffer identity and capacity differ. GC cannot
// intervene mid-statement: the slot still roots the old block until the
// store, the append path does not allocate, and the fresh-block path
// allocates through pt_gc_new_full, which never collects. Doubt = not in
// catok_ = the exact pre-existing emission (pt_add_f -> pt_concat).
struct CatInfo {
  std::vector<const Stmt*> cands;  // candidate statements for this slot
  bool text_lit = false;     // an immediate Text/Bytes literal write exists
  bool non_text_lit = false; // an immediate non-text literal write exists
  bool bad_write = false;    // a write that is neither candidate nor literal
  bool bad_read = false;     // a read outside the allowed set
  bool structural = false;   // cell / param / not-an-own-local
};
using CatKey = std::pair<const FuncDecl*, std::string>;

struct CatScan {
  std::map<CatKey, CatInfo> keys;
  std::set<std::string> varqual_names;  // names a `variable.` access uses
  bool name_table = false;              // a scope-name table exists anywhere
  std::set<const FuncDecl*> name_table_fn;  // fn trees containing one
  const FuncDecl* fn = nullptr;             // function currently walked
  std::set<const Expr*> exempt;  // allowed self-read operand nodes
};

static CatInfo& cat_slot(CatScan& c, const std::string& name, VarRes res) {
  return c.keys[CatKey{res == VarRes::Global ? nullptr : c.fn, name}];
}

static void cat_expr(CatScan& c, const Expr& e);
static void cat_stmt(CatScan& c, const Stmt& s);
static void cat_stmts(CatScan& c, const std::vector<Stmt>& body);
static void cat_target(CatScan& c, const AssignTarget& t);

// A write to a slot. `cand` = the statement is a growth candidate (already
// registered). Otherwise the RHS decides: an immediate Text/Bytes literal
// is text evidence, an immediate non-text literal is non-text evidence, and
// anything else — including no RHS at all (for/except/destructure/class
// bindings) — is a write the proof cannot account for. Resolutions outside
// Global/Local/Param decline BOTH key shapes so no candidate sees the
// write missed.
static void cat_write(CatScan& c, const std::string& name, VarRes res,
                      bool is_cell, const Expr* rhs, bool cand) {
  if (res != VarRes::Global && res != VarRes::Local && res != VarRes::Param) {
    c.keys[CatKey{c.fn, name}].bad_write = true;
    c.keys[CatKey{nullptr, name}].bad_write = true;
    return;
  }
  CatInfo& in = cat_slot(c, name, res);
  if (is_cell) in.structural = true;
  if (res == VarRes::Param) in.structural = true;
  if (cand) return;
  if (!rhs) {
    in.bad_write = true;
    return;
  }
  switch (rhs->kind) {
    case ExprKind::StrLit:
    case ExprKind::BytesLit:
      in.text_lit = true;
      return;
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
    case ExprKind::BoolLit:
    case ExprKind::NoneLit:
      in.non_text_lit = true;
      return;
    default:
      in.bad_write = true;
      return;
  }
}

// A read of a slot (an ExprKind::Var in value position). Allowed reads are
// pre-marked node-for-node in cat_scan's exempt set (the candidate's own
// concat operand, a len() argument); everything else is disqualifying.
static void cat_read(CatScan& c, const Expr& e) {
  if (c.exempt.count(&e)) return;
  if (e.res != VarRes::Global && e.res != VarRes::Local && e.res != VarRes::Param)
    return;  // function names / unresolved — not value-slot traffic
  CatInfo& in = cat_slot(c, e.name, e.res);
  if (e.is_cell) in.structural = true;
  if (e.res == VarRes::Param) in.structural = true;
  in.bad_read = true;
}

static void cat_expr(CatScan& c, const Expr& e) {
  switch (e.kind) {
    case ExprKind::Var:
      cat_read(c, e);
      return;
    case ExprKind::VarQual:
      // A cross-scope `variable.` access touches the slot's storage
      // directly — decline every same-named key (validation is by name).
      c.varqual_names.insert(e.name);
      return;
    case ExprKind::Call: {
      if (e.is_builtin) {
        const std::string& n = e.builtin_name;
        if (n == "globals" || n == "locals" || n == "vars" || n == "dir") {
          c.name_table = true;
          if (c.fn) c.name_table_fn.insert(c.fn);
        }
        // Allowed read: len(<slot>) — one positional argument, and the
        // call retains nothing past its integer result.
        if (n == "len" && e.args.size() == 1u &&
            (e.arg_names.empty() || e.arg_names[0].empty()) && e.args[0])
          c.exempt.insert(e.args[0].get());
      }
      if (e.callee_expr) cat_expr(c, *e.callee_expr);
      for (const auto& a : e.args)
        if (a) cat_expr(c, *a);
      return;
    }
    case ExprKind::Unary:
      if (e.lhs) cat_expr(c, *e.lhs);
      return;
    case ExprKind::Binary:
    case ExprKind::Logical:
      if (e.lhs) cat_expr(c, *e.lhs);
      if (e.rhs) cat_expr(c, *e.rhs);
      return;
    case ExprKind::Ternary:
      if (e.lhs) cat_expr(c, *e.lhs);
      if (e.rhs) cat_expr(c, *e.rhs);
      if (e.third) cat_expr(c, *e.third);
      return;
    case ExprKind::ListLit:
    case ExprKind::SetLit:
    case ExprKind::TupleLit:
    case ExprKind::Join:
    case ExprKind::MethodCall:
      if (e.lhs) cat_expr(c, *e.lhs);  // method receiver = a read
      for (const auto& el : e.elems)
        if (el) cat_expr(c, *el);
      return;
    case ExprKind::MapLit:
      for (const auto& k : e.keys)
        if (k) cat_expr(c, *k);
      for (const auto& el : e.elems)
        if (el) cat_expr(c, *el);
      return;
    case ExprKind::Index:
      if (e.lhs) cat_expr(c, *e.lhs);
      if (e.rhs) cat_expr(c, *e.rhs);
      return;
    case ExprKind::Slice:
      if (e.lhs) cat_expr(c, *e.lhs);
      if (e.rhs) cat_expr(c, *e.rhs);
      if (e.third) cat_expr(c, *e.third);
      if (e.fourth) cat_expr(c, *e.fourth);
      return;
    case ExprKind::Attr:
      if (e.lhs) cat_expr(c, *e.lhs);
      return;
    default:
      return;  // literals carry no variable traffic
  }
}

// An assignment target: Name/Rest leaves are WRITES (a destructure leaf
// hands the slot a value the proof cannot account for), VarQual names the
// slot for every same-named key, and object/index expressions are reads.
static void cat_target(CatScan& c, const AssignTarget& t) {
  switch (t.kind) {
    case AssignTarget::Kind::Name:
    case AssignTarget::Kind::Rest:
      cat_write(c, t.name, t.res, t.is_cell, nullptr, /*cand=*/false);
      return;
    case AssignTarget::Kind::VarQual:
      c.varqual_names.insert(t.name);
      return;
    case AssignTarget::Kind::Index:
      if (t.object) cat_expr(c, *t.object);
      if (t.index) cat_expr(c, *t.index);
      break;
    case AssignTarget::Kind::Attr:
      if (t.object) cat_expr(c, *t.object);
      break;
    case AssignTarget::Kind::Tuple:
      break;
  }
  for (const AssignTarget& el : t.elems) cat_target(c, el);
}

// Structural shape of a growth candidate: plain `name = name + expr` (the
// Binary's lhs is exactly the target slot) or compound `name += expr`.
static bool cat_candidate(const Stmt& s) {
  if (s.kind != StmtKind::Assign) return false;
  if (s.target.kind != AssignTarget::Kind::Name) return false;
  if (s.res == VarRes::Unresolved) return false;
  if (s.op.empty()) {
    return s.expr && s.expr->kind == ExprKind::Binary && s.expr->op == "+" &&
           s.expr->lhs && s.expr->lhs->kind == ExprKind::Var &&
           s.expr->lhs->name == s.name && s.expr->lhs->res == s.res;
  }
  return s.op == "+=";
}

static void cat_stmt(CatScan& c, const Stmt& s) {
  switch (s.kind) {
    case StmtKind::Assign: {
      const bool cand = cat_candidate(s);
      if (s.target.kind == AssignTarget::Kind::Name) {
        if (cand && (s.res == VarRes::Global || s.res == VarRes::Local ||
                     s.res == VarRes::Param)) {
          CatInfo& in = cat_slot(c, s.name, s.res);
          if (s.is_cell || s.res == VarRes::Param) in.structural = true;
          if (s.op.empty() && s.expr->lhs->is_cell) in.structural = true;
          in.cands.push_back(&s);
        }
        cat_write(c, s.name, s.res, s.is_cell, s.expr.get(), cand);
      } else {
        cat_target(c, s.target);
      }
      // The candidate's own operand reads are the allowed self-reads: the
      // `s` of `s = s + ...` (evaluated before the append, consumed by it),
      // plus a direct `s` rhs for `s = s + s` / `s += s`.
      if (cand) {
        if (s.op.empty()) {
          c.exempt.insert(s.expr->lhs.get());
          if (s.expr->rhs && s.expr->rhs->kind == ExprKind::Var &&
              s.expr->rhs->name == s.name && s.expr->rhs->res == s.res)
            c.exempt.insert(s.expr->rhs.get());
        } else if (s.expr && s.expr->kind == ExprKind::Var &&
                   s.expr->name == s.name && s.expr->res == s.res) {
          c.exempt.insert(s.expr.get());
        }
      }
      if (s.expr) cat_expr(c, *s.expr);
      c.exempt.clear();  // statements hold no nested statements
      break;
    }
    case StmtKind::For:
      if (s.expr) cat_expr(c, *s.expr);  // the iterable is a read
      cat_write(c, s.name, s.res, s.is_cell, nullptr, /*cand=*/false);
      cat_stmts(c, s.body);
      break;
    case StmtKind::Try: {
      cat_stmts(c, s.body);
      for (const ExceptArm& a : s.arms) {
        if (a.cond) cat_expr(c, *a.cond);
        if (!a.name.empty())
          cat_write(c, a.name, a.res, a.is_cell, nullptr, /*cand=*/false);
        cat_stmts(c, a.body);
      }
      cat_stmts(c, s.finally_body);
      break;
    }
    case StmtKind::If:
      for (const auto& cond : s.conds)
        if (cond) cat_expr(c, *cond);
      for (const auto& b : s.bodies) cat_stmts(c, b);
      cat_stmts(c, s.else_body);
      break;
    case StmtKind::While:
      if (s.expr) cat_expr(c, *s.expr);
      cat_stmts(c, s.body);
      break;
    case StmtKind::Switch:
      if (s.expr) cat_expr(c, *s.expr);
      for (const auto& arm : s.cases) cat_stmts(c, arm.body);
      break;
    case StmtKind::Return:
    case StmtKind::Raise:
    case StmtKind::ExprStmt:
      if (s.expr) cat_expr(c, *s.expr);
      break;
    case StmtKind::Class:
      cat_write(c, s.name, VarRes::Global, false, nullptr, /*cand=*/false);
      cat_stmts(c, s.body);
      break;
    case StmtKind::Func:
      break;  // nested bodies + defaults: walked through all_functions()
    default:
      break;  // Pass/Break/Continue/GlobalDecl/NonlocalDecl
  }
}

static void cat_stmts(CatScan& c, const std::vector<Stmt>& body) {
  for (const Stmt& st : body) cat_stmt(c, st);
}

void Emitter::analyze_text_cats() {
  CatScan c;
  for (const TopLevel& t : prog_.items) {
    if (t.kind == TopKind::Stmt)
      cat_stmt(c, t.stmt);
    else if (t.kind == TopKind::Main && t.main)
      cat_stmts(c, t.main->body);
  }
  for (const FuncDecl* f : all_functions()) {
    c.fn = f;
    for (const ExprPtr& d : f->defaults)
      if (d) cat_expr(c, *d);
    cat_stmts(c, f->body);
  }

  // ---- validation: every hold must be proven for the slot ---------------
  for (auto& entry : c.keys) {
    CatInfo& in = entry.second;
    if (in.cands.empty()) continue;
    if (in.structural || in.bad_read || in.bad_write || in.non_text_lit)
      continue;
    if (!in.text_lit) continue;  // no Text/Bytes evidence for the slot
    const std::string& nm = entry.first.second;
    const FuncDecl* fn = entry.first.first;
    if (c.varqual_names.count(nm)) continue;  // cross-scope access names it
    if (!fn) {
      if (c.name_table) continue;  // a scope table anywhere can expose it
      bool declared = false, pinned = false;
      for (const std::string& g : prog_.globals)
        if (g == nm) { declared = true; break; }
      for (const std::string& g : prog_.pin_globals)
        if (g == nm) { pinned = true; break; }
      if (!declared || pinned) continue;
    } else {
      if (c.name_table_fn.count(fn)) continue;  // a table in its own tree
      bool pinned = false, par = false, cap = false;
      for (const std::string& p : fn->pinned)
        if (p == nm) { pinned = true; break; }
      for (const std::string& p : fn->params)
        if (p == nm) { par = true; break; }
      for (const std::string& p : fn->captures)
        if (p == nm) { cap = true; break; }
      if (pinned || par || cap) continue;
      if (nm == fn->vararg || nm == fn->kwarg) continue;
      size_t i = 0;
      for (; i < fn->locals.size(); ++i)
        if (fn->locals[i] == nm) break;
      if (i == fn->locals.size()) continue;  // not an own (non-param) local
      if (i < fn->local_is_cell.size() && fn->local_is_cell[i]) continue;
    }
    for (const Stmt* st : in.cands) catok_.insert(st);
  }
}

// ---- statically-proven instance-method inlining ---------------------------
// analyze_method_inlines() (run LAST in run(), after every analysis whose
// state it must respect) plans MethodCall sites whose every dynamic-dispatch
// premise it can prove program-wide; emit_inline_method then materializes a
// planned site as the callee's body inlined at the call site over per-site
// slots `pt_il<id>_<name>`, result slot `pt_ilr<id>`, merge label
// `pt_ile<id>end`.
//
// SOUNDNESS ARGUMENT. The dynamic path for `o.m(a…)` evaluates `o`, looks
// `m` up on the receiver's class, binds (receiver, args) positionally to the
// method's parameters, and runs the body in a fresh frame. Because Pentest
// has NO inheritance (`class C(Base):` is a parse error), a receiver that
// always carries class C dispatches to exactly C's method `m`, and a fresh
// frame's params/locals are exactly the per-site slots with the receiver
// bound to the same object value — so the inlined body is the same
// computation over the same storage shapes. Every observable event stays
// identical:
//
//   * RECEIVER PROOF (flow-insensitive points-to over value slots; keys are
//     storage identities: (function, resolution, name) for local/param, the
//     bare name for module globals). Slots start at their initial content —
//     locals: unset; globals: unset (a class-name global — user classes and
//     builtin exception classes alike — starts at the sentinel "$class",
//     the class OBJECT, never an instance); `self`: the
//     containing class (dispatch guarantees it, and a rebinding `self = …`
//     only unions in more classes); every other param: TOP (caller-unknown
//     arguments); a key that is not the function's own binding (a
//     capture/qualified read, whose storage and writes live under a
//     different key): TOP. Each write unions in its value: a constructor call `C(…)`
//     contributes {C} — rejected when C's global is ever rebound anywhere,
//     since the call could then hit a non-class — a plain name copy
//     contributes the source slot's content, and everything else
//     (compound/loop-target/except-binding/tuple-unpack/cell/pinned/
//     qualified writes, any other expression) contributes TOP. A site's
//     receiver must read exactly {C}: one class, no TOP. Reading an unset
//     slot raises NameError under the ordinary guard before the body runs
//     (or is impossible per the DA proofs), so the body never runs on a
//     non-instance.
//   * DISPATCH: C's class body must declare `m`; otherwise the dynamic path
//     raises AttributeError — the site is declined and that raise stays.
//   * BINDING: named arguments, varargs and kwargs are declined, and the
//     site's positional count must equal the parameter count, reproducing
//     the binder's exact positional binding (receiver → params[0], args
//     left-to-right). Defaults can never fire (all params are supplied).
//   * BODY STRUCTURE: methods containing try (the return intercept runs no
//     finallys), nested declarations (closure hoisting lives in
//     emit_function's prologue), nonlocal/cell/pinned bindings (storage_for's
//     inline diversion must be the authoritative mapping), or native-lane
//     locals (their Assign emission writes pt_nl_/pt_nfl_ storage that does
//     not exist in the caller's frame) are declined. Every other statement
//     form emits through the ordinary emitters with cur_func_ set to the
//     method, so guards, line numbers, raise wording, attribute get/set and
//     operator errors are byte-identical to the dynamic body; the caller's
//     try/finally state is deliberately untouched (a callee return/raise
//     never runs the caller's finallys dynamically either).
//   * FRAMES/ROOTS: site slots join the current shadow-frame region
//     (il_register): declared `pt_unset()` at the region top and rooted for
//     the whole region — at least as conservative as the callee's frame.
//     Locals are re-set to pt_unset() at every entry (a dynamic frame starts
//     fresh each call; the region-top initializer runs only once).
//   * RE-ENTRY: il_stack_ refuses to inline a method already being inlined,
//     so cyclic plans degrade to the dynamic path.
//   * SITES INSIDE A `finally` BODY are never planned: the emitter re-emits
//     finally bodies per exit edge, which would duplicate the merge label in
//     one C function (writes inside finallys are still collected — they are
//     real writes).
//   * OBSERVABILITY: the runtime carries no traceback/frame introspection —
//     every error surface is (message, raising line) — so the disappearance
//     of a physical call frame is unobservable. FLAGGED DEVIATION CANDIDATE
//     (contract §1.4): the inlined site carries NO entry `pt_gc_maybe`; the
//     callee-entry safe point disappears with the call. Body-internal safe
//     points (loop back-edges, statement boundaries) are untouched — the
//     body emits through the ordinary path.
void Emitter::analyze_method_inlines() {
  struct SlotKey {
    const FuncDecl* fn = nullptr;
    int res = 0;  // VarRes
    std::string name;
    bool operator<(const SlotKey& o) const {
      if (fn != o.fn) return fn < o.fn;
      if (res != o.res) return res < o.res;
      return name < o.name;
    }
  };
  struct Pt {
    bool top = false;
    std::set<std::string> cls;
    bool same(const Pt& o) const { return top == o.top && cls == o.cls; }
  };
  struct Wr {
    const FuncDecl* scope = nullptr;  // where the written expression lives
    const Expr* val = nullptr;        // null = unclassifiable (TOP)
  };

  // ---- class registry: name -> method table, method -> owning class -----
  std::set<std::string> class_names;
  std::map<std::string, std::map<std::string, const FuncDecl*>> classes;
  std::map<const FuncDecl*, std::string> method_class;
  for (const TopLevel& t : prog_.items) {
    if (t.kind != TopKind::Stmt || t.stmt.kind != StmtKind::Class) continue;
    class_names.insert(t.stmt.name);
    auto& tbl = classes[t.stmt.name];
    for (const FuncDecl& m : t.stmt.class_methods) {
      tbl[m.name] = &m;
      method_class[&m] = t.stmt.name;
    }
  }
  // Builtin exception classes are class-valued module globals written by
  // pt_init (never by an Assign we could track) — they carry the same
  // "$class" sentinel as user classes so copies of them can never prove an
  // instance (their dispatch table does not exist in `classes` either).
  for (const std::string& g : prog_.globals)
    if (is_builtin_exception_class(g)) class_names.insert(g);
  if (class_names.empty()) return;

  // ---- walk #1: every MethodCall site + every value-slot write -----------
  std::vector<std::pair<const Expr*, const FuncDecl*>> sites;  // (site, scope)
  std::map<SlotKey, std::vector<Wr>> wrs;
  std::set<std::string> class_rebound;  // class globals with a user write

  auto key_of = [](const FuncDecl* sc, VarRes r, const std::string& n) {
    SlotKey k;
    k.fn = (r == VarRes::Global) ? nullptr : sc;
    k.res = (int)r;
    k.name = n;
    return k;
  };
  auto note_write = [&](const FuncDecl* sc, VarRes r, const std::string& n,
                        const Expr* v) {
    if (r == VarRes::Unresolved || r == VarRes::Function) return;
    if (r != VarRes::Global && !sc) return;  // Local/Param needs a scope
    if (r == VarRes::Global && class_names.count(n)) class_rebound.insert(n);
    wrs[key_of(sc, r, n)].push_back(Wr{sc, v});
  };

  std::function<void(const Expr&, const FuncDecl*, bool)> walk_e;
  std::function<void(const AssignTarget&, const FuncDecl*, bool)> walk_t;
  std::function<void(const Stmt&, const FuncDecl*, bool)> walk_s;
  std::function<void(const FuncDecl*)> walk_fn;

  walk_e = [&](const Expr& e, const FuncDecl* sc, bool in_finally) {
    if (e.kind == ExprKind::MethodCall && !in_finally)
      sites.push_back({&e, sc});
    switch (e.kind) {
      case ExprKind::Var:
      case ExprKind::VarQual:
      case ExprKind::IntLit:
      case ExprKind::FloatLit:
      case ExprKind::StrLit:
      case ExprKind::BytesLit:
      case ExprKind::BoolLit:
      case ExprKind::NoneLit:
        return;
      case ExprKind::Unary:
        if (e.lhs) walk_e(*e.lhs, sc, in_finally);
        return;
      case ExprKind::Binary:
      case ExprKind::Logical:
        if (e.lhs) walk_e(*e.lhs, sc, in_finally);
        if (e.rhs) walk_e(*e.rhs, sc, in_finally);
        return;
      case ExprKind::Ternary:
        if (e.lhs) walk_e(*e.lhs, sc, in_finally);
        if (e.rhs) walk_e(*e.rhs, sc, in_finally);
        if (e.third) walk_e(*e.third, sc, in_finally);
        return;
      case ExprKind::Call:
        if (e.callee_expr) walk_e(*e.callee_expr, sc, in_finally);
        for (const auto& a : e.args)
          if (a) walk_e(*a, sc, in_finally);
        return;
      case ExprKind::MethodCall:
      case ExprKind::Attr:
        if (e.lhs) walk_e(*e.lhs, sc, in_finally);
        for (const auto& el : e.elems)
          if (el) walk_e(*el, sc, in_finally);
        return;
      case ExprKind::Index:
        if (e.lhs) walk_e(*e.lhs, sc, in_finally);
        if (e.rhs) walk_e(*e.rhs, sc, in_finally);
        return;
      case ExprKind::Slice:
        if (e.lhs) walk_e(*e.lhs, sc, in_finally);
        if (e.has_start && e.rhs) walk_e(*e.rhs, sc, in_finally);
        if (e.has_stop && e.third) walk_e(*e.third, sc, in_finally);
        if (e.has_step && e.fourth) walk_e(*e.fourth, sc, in_finally);
        return;
      case ExprKind::ListLit:
      case ExprKind::SetLit:
      case ExprKind::TupleLit:
      case ExprKind::Join:
        for (const auto& el : e.elems)
          if (el) walk_e(*el, sc, in_finally);
        return;
      case ExprKind::MapLit:
        for (const auto& k : e.keys)
          if (k) walk_e(*k, sc, in_finally);
        for (const auto& el : e.elems)
          if (el) walk_e(*el, sc, in_finally);
        return;
    }
  };

  walk_t = [&](const AssignTarget& t, const FuncDecl* sc, bool in_finally) {
    if (t.object) walk_e(*t.object, sc, in_finally);
    if (t.index) walk_e(*t.index, sc, in_finally);
    for (const AssignTarget& sub : t.elems) walk_t(sub, sc, in_finally);
  };

  walk_fn = [&](const FuncDecl* f) {
    for (const auto& d : f->defaults)
      if (d) walk_e(*d, f, false);
    for (const Stmt& b : f->body) walk_s(b, f, false);
  };

  walk_s = [&](const Stmt& s, const FuncDecl* sc, bool in_finally) {
    switch (s.kind) {
      case StmtKind::Assign: {
        if (s.expr) walk_e(*s.expr, sc, in_finally);
        walk_t(s.target, sc, in_finally);
        if (s.target.kind == AssignTarget::Kind::Name) {
          // Plain, non-cell, non-compound: classify the RHS; anything else
          // contributes TOP (a compound/boxed value is not provenance-stable).
          const Expr* v = nullptr;
          if (s.op.empty() && s.expr && !s.is_cell) v = s.expr.get();
          note_write(sc, s.res, s.name, v);
        } else if (s.target.kind == AssignTarget::Kind::VarQual) {
          // A qualified global write hits the same module storage; a pinned
          // write hits pin storage (whose owner slots start TOP anyway).
          if (s.target.pin.empty())
            note_write(nullptr, VarRes::Global, s.target.name, nullptr);
        } else if (s.target.kind == AssignTarget::Kind::Tuple ||
                   s.target.kind == AssignTarget::Kind::Rest) {
          // Destructuring leaves receive element values of unknown class.
          std::function<void(const AssignTarget&)> leaves =
              [&](const AssignTarget& lt) {
            if (lt.kind == AssignTarget::Kind::Name ||
                lt.kind == AssignTarget::Kind::Rest)
              note_write(sc, lt.res, lt.name, nullptr);
            else if (lt.kind == AssignTarget::Kind::VarQual) {
              if (lt.pin.empty())
                note_write(nullptr, VarRes::Global, lt.name, nullptr);
            }
            for (const AssignTarget& sub : lt.elems) leaves(sub);
          };
          leaves(s.target);
        }
        return;
      }
      case StmtKind::ExprStmt:
      case StmtKind::Return:
      case StmtKind::Raise:
        if (s.expr) walk_e(*s.expr, sc, in_finally);
        return;
      case StmtKind::If:
        for (size_t i = 0; i < s.conds.size(); ++i) {
          if (s.conds[i]) walk_e(*s.conds[i], sc, in_finally);
          if (i < s.bodies.size())
            for (const Stmt& b : s.bodies[i]) walk_s(b, sc, in_finally);
        }
        for (const Stmt& b : s.else_body) walk_s(b, sc, in_finally);
        return;
      case StmtKind::While:
        if (s.expr) walk_e(*s.expr, sc, in_finally);
        for (const Stmt& b : s.body) walk_s(b, sc, in_finally);
        return;
      case StmtKind::For:
        if (s.expr) walk_e(*s.expr, sc, in_finally);
        note_write(sc, s.res, s.name, nullptr);  // element class unknown
        for (const Stmt& b : s.body) walk_s(b, sc, in_finally);
        return;
      case StmtKind::Switch:
        if (s.expr) walk_e(*s.expr, sc, in_finally);
        for (const CaseArm& arm : s.cases)
          for (const Stmt& b : arm.body) walk_s(b, sc, in_finally);
        return;
      case StmtKind::Try:
        for (const Stmt& b : s.body) walk_s(b, sc, in_finally);
        for (const ExceptArm& arm : s.arms) {
          if (arm.cond) walk_e(*arm.cond, sc, in_finally);
          if (!arm.name.empty())
            note_write(sc, arm.res, arm.name, nullptr);
          for (const Stmt& b : arm.body) walk_s(b, sc, in_finally);
        }
        // finally bodies are re-emitted per exit edge: real writes (collected
        // above with the normal flag through the arms), but sites planned
        // here would duplicate a merge label — walk with in_finally = true.
        for (const Stmt& b : s.finally_body) walk_s(b, sc, true);
        return;
      case StmtKind::Func:
        if (s.func) walk_fn(s.func.get());
        return;
      case StmtKind::Class:
        for (const FuncDecl& m : s.class_methods) walk_fn(&m);
        for (const Stmt& b : s.body) walk_s(b, sc, in_finally);
        return;
      case StmtKind::GlobalDecl:
      case StmtKind::NonlocalDecl:
      case StmtKind::Break:
      case StmtKind::Continue:
      case StmtKind::Pass:
        return;
    }
  };

  for (const TopLevel& t : prog_.items) {
    if (t.kind == TopKind::Func) {
      if (t.func) walk_fn(t.func.get());
    } else if (t.kind == TopKind::Main) {
      if (t.main)
        for (const Stmt& b : t.main->body) walk_s(b, nullptr, false);
    } else {
      walk_s(t.stmt, nullptr, false);
    }
  }

  // ---- points-to fixpoint -------------------------------------------------
  auto init_pt = [&](const SlotKey& k) {
    Pt p;
    if (k.res == (int)VarRes::Global) {
      // A class-name global holds the class OBJECT until a user write says
      // otherwise — copies of it must never prove an instance.
      if (class_names.count(k.name)) p.cls.insert("$class");
      return p;
    }
    const FuncDecl* f = k.fn;
    if (!f) {  // Local/Param without a function scope: never happens (main
               // binds globals), but be safe.
      p.top = true;
      return p;
    }
    if (k.res == (int)VarRes::Param) {
      if (k.name == "self" && f->is_method) {
        auto it = method_class.find(f);
        if (it != method_class.end()) {
          p.cls.insert(it->second);  // dispatch guarantees the class
          return p;
        }
      }
      p.top = true;  // any other param: caller-unknown arguments
      return p;
    }
    if (pinned_binding(*f, k.name)) {
      p.top = true;  // pin storage is reachable from other scopes
      return p;
    }
    for (size_t i = 0; i < f->locals.size(); ++i) {
      if (f->locals[i] == k.name) {
        if (i < f->local_is_cell.size() && f->local_is_cell[i]) p.top = true;
        return p;  // an OWN local: no tracked writes = unset (guards raise)
      }
    }
    // Not an own binding of this function: a capture/qualified read whose
    // storage (and writes) live under a DIFFERENT key. Its content here is
    // unknown — TOP, so neither a receiver check nor a copy chain can ever
    // inherit a stale {} from it.
    p.top = true;
    return p;
  };
  std::map<SlotKey, Pt> pt;
  auto pt_at = [&](const SlotKey& k) -> Pt& {
    auto it = pt.find(k);
    if (it == pt.end()) it = pt.emplace(k, init_pt(k)).first;
    return it->second;
  };
  auto ctor_class = [&](const Expr& x, std::string& out) {
    if (x.kind != ExprKind::Call || x.callee.empty() || !x.callee_expr ||
        x.callee_expr->kind != ExprKind::Var ||
        x.callee_expr->name != x.callee ||
        x.callee_expr->res != VarRes::Global || !class_names.count(x.callee))
      return false;
    out = x.callee;
    return true;
  };
  auto classify = [&](const Wr& w) {
    Pt p;
    if (!w.val) {
      p.top = true;
      return p;
    }
    const Expr& x = *w.val;
    if (x.kind == ExprKind::Call) {
      std::string c;
      if (ctor_class(x, c) && !class_rebound.count(c))
        p.cls.insert(c);
      else
        p.top = true;
      return p;
    }
    if (x.kind == ExprKind::Var &&
        (x.res == VarRes::Global || x.res == VarRes::Local ||
         x.res == VarRes::Param))
      return pt_at(key_of(w.scope, x.res, x.name));  // copy chain
    p.top = true;
    return p;
  };
  for (const auto& kv : wrs) pt_at(kv.first);  // materialize every write key
  for (bool change = true; change;) {
    change = false;
    for (const auto& kv : wrs) {
      Pt nv = init_pt(kv.first);
      for (const Wr& w : kv.second) {
        Pt c = classify(w);  // monotone in the source states
        if (c.top) nv.top = true;
        nv.cls.insert(c.cls.begin(), c.cls.end());
      }
      Pt& cur = pt_at(kv.first);
      if (!cur.same(nv)) {
        cur = nv;
        change = true;
      }
    }
  }

  // ---- callee eligibility (per method, cached) ----------------------------
  std::map<const FuncDecl*, std::pair<bool, bool>> elig;  // {ok, has_return}
  std::function<std::pair<bool, bool>(const FuncDecl*)> check =
      [&](const FuncDecl* m) -> std::pair<bool, bool> {
    auto it = elig.find(m);
    if (it != elig.end()) return it->second;
    std::pair<bool, bool> r{true, false};
    if (m->has_vararg || m->has_kwarg || m->params.empty() ||
        !m->captures.empty())
      r.first = false;
    for (bool c : m->param_is_cell)
      if (c) r.first = false;
    for (bool c : m->local_is_cell)
      if (c) r.first = false;
    for (const std::string& p : m->params)
      if (pinned_binding(*m, p)) r.first = false;
    for (const std::string& l : m->locals)
      if (pinned_binding(*m, l)) r.first = false;
    auto nl = nativ_l_.find(m);
    if (nl != nativ_l_.end() && !nl->second.empty()) r.first = false;
    auto nf = nativfl_.find(m);
    if (nf != nativfl_.end() && !nf->second.empty()) r.first = false;
    std::function<void(const std::vector<Stmt>&)> bs =
        [&](const std::vector<Stmt>& body) {
      for (const Stmt& s : body) {
        if (s.kind == StmtKind::Try || s.kind == StmtKind::Func ||
            s.kind == StmtKind::NonlocalDecl) {
          r.first = false;  // finally/closure-hoisting/box interactions
          continue;         // (a declined method needs no further scanning)
        }
        if (s.kind == StmtKind::Return) r.second = true;
        bs(s.body);
        for (const std::vector<Stmt>& b : s.bodies) bs(b);
        bs(s.else_body);
        for (const CaseArm& a : s.cases) bs(a.body);
        for (const ExceptArm& a : s.arms) bs(a.body);
        bs(s.finally_body);
      }
    };
    bs(m->body);
    elig[m] = r;
    return r;
  };

  // ---- plan the sites -----------------------------------------------------
  for (auto& sp : sites) {
    const Expr& e = *sp.first;
    const FuncDecl* sc = sp.second;
    if (!e.lhs) continue;
    bool named = false;
    for (const std::string& n : e.elem_names)
      if (!n.empty()) named = true;
    if (named) continue;  // binder-shaped calls stay dynamic
    bool missing = false;
    for (const auto& el : e.elems)
      if (!el) missing = true;
    if (missing) continue;
    // Fused-write shapes keep the baseline fused path (emit_expr refuses
    // them too) — never plan them.
    bool fuse = e.method == "write" && e.elems.size() == 1u && e.elems[0] &&
                (e.elem_names.empty() || e.elem_names[0].empty());
    if (fuse) {
      std::vector<const Expr*> chk;
      fuse = text_chain_parts(*e.elems[0], chk);
    }
    if (fuse) continue;
    // Receiver: a proven slot or a direct constructor call.
    std::string cls;
    const Expr& rec = *e.lhs;
    if (rec.kind == ExprKind::Var) {
      if (rec.res != VarRes::Global && rec.res != VarRes::Local &&
          rec.res != VarRes::Param)
        continue;
      if (rec.res != VarRes::Global && !sc) continue;
      const Pt& p = pt_at(key_of(sc, rec.res, rec.name));
      if (p.top || p.cls.size() != 1u) continue;
      cls = *p.cls.begin();
    } else if (rec.kind == ExprKind::Call) {
      if (!ctor_class(rec, cls)) continue;
      if (class_rebound.count(cls)) continue;  // call may hit a non-class
    } else {
      continue;
    }
    // Dispatch target: C must declare `m` (else AttributeError stays
    // dynamic) and binding must be exact-positional.
    auto cit = classes.find(cls);
    if (cit == classes.end()) continue;
    auto mit = cit->second.find(e.method);
    if (mit == cit->second.end()) continue;
    const FuncDecl* m = mit->second;
    if (!check(m).first) continue;
    if (e.elems.size() + 1u != m->params.size()) continue;
    // Sites inside native/bulk twin bodies keep their dynamic calls (those
    // bodies must never run the ordinary boxed emission path we would use).
    if (sc && (nativ_fns_.count(sc->name) || bulk_ok_.count(sc))) continue;
    InlineSite site;
    site.method = m;
    site.in_fn = sc;
    site.id = il_next_++;
    site.has_return = check(m).second;
    il_sites_[&e] = site;
  }
}

// Emission of a planned site (see the soundness comment above the planning
// pass). Returns the expression text of the call's value: the result slot
// when the body can return, else `pt_none()` (a body with no `return` falls
// off its end into None).
std::string Emitter::emit_inline_method(const Expr& e, const InlineSite& site,
                                         bool in_function) {
  const FuncDecl& m = *site.method;
  if (!e.lhs)
    fail("method call without object (line " + std::to_string(e.loc.line) + ")");
  // Receiver and arguments evaluate in the CALLER's context, in source
  // order, before the method scope is entered — a receiver/argument
  // expression belongs to the caller's scope, exactly as on the dynamic
  // path (receiver first, then args left-to-right).
  std::string obj = emit_expr(*e.lhs, in_function);
  std::vector<std::string> args;
  args.reserve(e.elems.size());
  for (const auto& el : e.elems) {
    if (!el)
      fail("method argument missing (line " + std::to_string(e.loc.line) + ")");
    args.push_back(emit_expr(*el, in_function));  // source order
  }
  if (args.size() + 1u != m.params.size())
    fail("internal: inline site arity drift (line " +
         std::to_string(e.loc.line) + ")");

  // Enter the method scope: storage_for resolves the method's own
  // params/locals to the per-site slots, `return` intercepts to the result
  // slot + merge label, and cur_func_ drives the DA/guard/native lookups of
  // the body exactly as emit_function would set it.
  std::string saved_micx = micx_;
  int saved_id = micx_id_;
  const FuncDecl* saved_fn = cur_func_;
  bool saved_env = env_used_;
  il_stack_.push_back(&m);
  micx_ = "_" + std::to_string(site.id);
  micx_id_ = site.id;
  cur_func_ = &m;

  // Slots join the CURRENT frame region: declared `pt_unset()` at the
  // region top (emit_framed) and rooted in the shadow frame for the whole
  // region — at least as conservative as the callee's own frame would be.
  for (const std::string& p : m.params) il_register(il_name(p));
  for (const std::string& l : m.locals) il_register(il_name(l));
  if (site.has_return) il_register("pt_ilr" + std::to_string(micx_id_));

  // Entry binding: receiver → params[0], args left-to-right — the binder's
  // exact positional order (planning guaranteed the counts).
  line(ind() + il_name(m.params[0]) + " = " + obj + ";");
  for (size_t i = 1; i < m.params.size(); ++i)
    line(ind() + il_name(m.params[i]) + " = " + args[i - 1] + ";");
  // A dynamic frame starts every local unset on every call; the region-top
  // initializer runs only once per region, so re-set them per entry.
  for (const std::string& l : m.locals)
    line(ind() + il_name(l) + " = pt_unset();");
  if (site.has_return)
    line(ind() + "pt_ilr" + std::to_string(site.id) + " = pt_none();");

  emit_body(m.body, /*in_function=*/true);
  if (site.has_return)
    line(ind() + "pt_ile" + std::to_string(site.id) + "end: ;");

  il_stack_.pop_back();
  cur_func_ = saved_fn;
  micx_ = saved_micx;
  micx_id_ = saved_id;
  env_used_ = saved_env;
  if (site.has_return) return "pt_ilr" + std::to_string(site.id);
  return "pt_none()";
}

// Fused elementwise kernel for one assignment statement (see the block
// comment above analyze_native_fvecs). Emits:
//   { pt_value t = <guarded leaf reads>;        // once each — no re-eval
//     const double* d = pt_fvec_data_of(t); ... // hoisted data pointers
//     int64_t s = <int-lane scalar>; ...        // hoisted broadcasts
//     size_t n = pt_vec_len_of(<first>);        // + expect_len on the rest
//     pt_obj* r = pt_fvec_new(n, line);
//     double* pd = pt_fvec_data_of(pt_fvec_val(r));
//     size_t pt_vix = 0;
//     for (; pt_vix + PT_VW <= n; pt_vix += PT_VW) {   // guaranteed lanes
//       pt_vr = <expr over lane loads>; memcpy(pd + pt_vix, &pt_vr, ...); }
//     for (; pt_vix < n; pt_vix++) pd[pt_vix] = <expr over scalar loads>;
//     target = pt_fvec_val(r); }
// Per-element arithmetic equals the dynamic operators' exactly; the
// length check raises before anything is written.
void Emitter::emit_fvec_assign(const Stmt& s, int kind, bool in_function) {
  const bool fv = (kind == 1);
  const char* vdn = fv ? "pt_vd" : "pt_vi";
  const char* accf = fv ? "pt_fvec_data_of" : "pt_ivec_data_of";
  const char* ctor = fv ? "pt_fvec_val" : "pt_ivec_val";
  const char* nmk = fv ? "fvec" : "ivec";
  const char* ty = fv ? "double" : "int64_t";
  const std::string ln_s = std::to_string(s.loc.line);
  const FuncDecl* fn = cur_func_;

  struct FLeaf { const Expr* e; std::string res; std::string stem; };
  struct SLeaf { const Expr* e; std::string text; std::string stem; };
  std::vector<FLeaf> fvars;
  std::vector<SLeaf> svars;

  std::function<void(const Expr&)> collect = [&](const Expr& x) {
    // Decompose shaped elementwise nodes; leaves (vector vars, vector
    // constructor calls, int-lane scalar subtrees, literals) stop here.
    if (x.kind == ExprKind::Binary && x.lhs && x.rhs &&
        (x.op == "+" || x.op == "-" || x.op == "*")) {
      collect(*x.lhs);
      collect(*x.rhs);
      return;
    }
    if (x.kind == ExprKind::Unary && x.op == "-" && x.lhs) {
      collect(*x.lhs);
      return;
    }
    if (fvec_shape(x, fn) == kind) {
      for (FLeaf& l : fvars)
        if (l.e->kind == ExprKind::Var && x.kind == ExprKind::Var &&
            l.e->name == x.name && l.res == std::to_string((int)x.res))
          return;  // one guarded read per slot, however many uses
      FLeaf l;
      l.e = &x;
      l.res = std::to_string((int)x.res);
      l.stem = decl_temp();
      fvars.push_back(l);
      return;
    }
    if (x.kind == ExprKind::IntLit || x.kind == ExprKind::FloatLit) return;
    if (int_shape(x, fn, nativ_g_, nativ_l_, nullptr, true, nullptr,
                  &nativfg_, &nativfl_, &int_lists_,
                  &int_list_da_reads_)) {
      std::string t = native_int_text(x);
      if (t.empty())
        fail("internal: int-shaped scalar without text in fused kernel (line " +
             ln_s + ")");
      SLeaf l;
      l.e = &x;
      l.text = t;
      l.stem = decl_temp();
      svars.push_back(l);
      return;
    }
    fail("internal: unshaped operand in fused " + std::string(nmk) +
         " kernel (line " + ln_s + ")");
  };
  collect(*s.expr);

  // Lane/scalar text of one leaf or literal node. Vector VAR leaves match
  // by name+res (collect dedups a reused slot to one temp, but every AST
  // occurrence is a distinct node).
  auto leaf_text = [&](const Expr& x, bool vec) -> std::string {
    for (const FLeaf& l : fvars)
      if (l.e == &x ||
          (x.kind == ExprKind::Var && l.e->kind == ExprKind::Var &&
           l.e->name == x.name && l.res == std::to_string((int)x.res))) {
        if (vec)
          return std::string("(*(const ") + vdn + "*)(" + l.stem + "_d + pt_vix))";
        return l.stem + "_d[pt_vix]";
      }
    for (const SLeaf& l : svars)
      if (l.e == &x) {
        if (vec)
          return fv ? std::string("PT_VD_SPLAT((double)(") + l.stem + "))"
                    : std::string("PT_VI_SPLAT(") + l.stem + ")";
        return fv ? std::string("((double)(") + l.stem + "))"
                  : std::string("(") + l.stem + ")";
      }
    // Literals: RAW C texts (emit_expr would box them into pt_value).
    if (x.kind == ExprKind::FloatLit) {
      std::string lit = float_literal_raw(x.fval);
      return vec ? std::string("PT_VD_SPLAT(") + lit + ")" : "(" + lit + ")";
    }
    std::string lit = int_literal_raw(x.ival);  // IntLit
    if (vec)
      return fv ? std::string("PT_VD_SPLAT((double)(") + lit + "))"
                : std::string("PT_VI_SPLAT(") + lit + ")";
    return fv ? std::string("((double)(") + lit + "))" : "(" + lit + ")";
  };
  std::function<std::string(const Expr&, bool)> tr =
      [&](const Expr& x, bool vec) -> std::string {
    if (x.kind == ExprKind::Binary && x.lhs && x.rhs)
      return "(" + tr(*x.lhs, vec) + " " + x.op + " " + tr(*x.rhs, vec) + ")";
    if (x.kind == ExprKind::Unary && x.op == "-" && x.lhs)
      return "(-" + tr(*x.lhs, vec) + ")";
    return leaf_text(x, vec);
  };
  auto root_text = [&](bool vec) -> std::string {
    if (s.op.empty()) return tr(*s.expr, vec);
    // Compound: the target read is the left operand.
    std::string base = s.op.substr(0, s.op.size() - 1);  // "+=" → "+"
    std::string t = vec ? std::string("(*(const ") + vdn + "*)(TGT_d + pt_vix))"
                        : std::string("TGT_d[pt_vix]");
    return "(" + t + " " + base + " " + tr(*s.expr, vec) + ")";
  };

  line(ind() + "{");
  indent_ += 2;
  // Leaf extraction: every operand read exactly once.
  std::string first_op;
  if (!s.op.empty()) {
    std::string storage = storage_for(s.name, s.res, in_function, s.loc.line);
    std::string vt = decl_temp();
    line(ind() + "pt_value " + vt + " = " +
         guard_text(storage, s.name, s.res, s.loc.line) + ";");
    line(ind() + "const " + std::string(ty) + "* TGT_d = " + accf + "(" + vt +
         ");");
    first_op = vt;
  }
  for (const FLeaf& l : fvars) {
    line(ind() + "pt_value " + l.stem + " = " + emit_expr(*l.e, in_function) +
         ";");
    line(ind() + "const " + std::string(ty) + "* " + l.stem + "_d = " + accf +
         "(" + l.stem + ");");
    if (first_op.empty()) first_op = l.stem;
  }
  for (const SLeaf& l : svars)
    line(ind() + "int64_t " + l.stem + " = " + l.text + ";");
  if (first_op.empty())
    fail("internal: fused " + std::string(nmk) + " kernel with no vector "
         "operand (line " + ln_s + ")");

  std::string nt = decl_temp();
  line(ind() + "size_t " + nt + " = pt_vec_len_of(" + first_op + ");");
  for (const FLeaf& l : fvars)
    if (l.stem != first_op)
      line(ind() + std::string("pt_") + nmk + "_expect_len(" + l.stem + ", " +
           nt + ", " + ln_s + ");");
  std::string ro = decl_temp();
  line(ind() + "pt_obj* " + ro + " = pt_" + nmk + "_new(" + nt + ", " + ln_s +
       ");");
  std::string pd = decl_temp();
  line(ind() + std::string(ty) + "* " + pd + " = " + accf + "(" + ctor + "(" +
       ro + "));");
  line(ind() + "size_t pt_vix = 0;");
  line(ind() + "for (; pt_vix + PT_VW <= " + nt + "; pt_vix += PT_VW) {");
  indent_ += 2;
  line(ind() + std::string(vdn) + " pt_vr = " + root_text(true) + ";");
  line(ind() + "memcpy(" + pd + " + pt_vix, &pt_vr, sizeof pt_vr);");
  indent_ -= 2;
  line(ind() + "}");
  line(ind() + "for (; pt_vix < " + nt + "; pt_vix++) {");
  indent_ += 2;
  line(ind() + pd + "[pt_vix] = " + root_text(false) + ";");
  indent_ -= 2;
  line(ind() + "}");
  line(ind() + storage_for(s.name, s.res, in_function, s.loc.line) + " = " +
       ctor + "(" + ro + ");");
  indent_ -= 2;
  line(ind() + "}");
}

// ---- post-2C perf: native int-lane function twins -------------------------
//
// WHY: a function call used to cost the whole dynamic machinery per
// invocation — bound-vector materialization, local pt_value slots, tag
// dispatch on every operator, boxing on every boundary — even when the
// program is pure int64 arithmetic (fib, collatz, primality loops). The
// slot-lane analysis above cannot bridge the param/return boundary: a
// param is a runtime-typed pt_value and a user call returns one, so no
// expression inside such a function ever became int-shaped, and
// call-heavy int code stayed fully dynamic.
//
// A TWIN is a second C entry point emitted beside the (unchanged)
// dynamic one:
//     static int64_t pt_fn_fib_i(int64_t n);   // int64 in, int64 out
// Its body is the same statement tree emitted with the native lane
// active for every binding — params and locals are int64_t, operators
// are raw C through the existing native_int_text lane, and returns stay
// int64 (the caller boxes only when it needs a pt_value). Call sites
// whose arguments are ALL int-shaped call the twin; every other call
// site keeps the dynamic entry point, so float/string/None behavior is
// bit-identical — the call-site gate is the entire type contract.
//
// Viability rules (all conservative; each keeps the twin's body
// emittable with exactly the storage it declares):
//  * top-level static function: not nested (nested calls are dynamic),
//    no captures (env-free), no vararg/kwarg/defaults (call sites pass
//    exactly params.size() positional args), no cells or pins (no
//    pt_cell / static-slot storage), no nested declarations (no closure
//    hoisting), no try (setjmp frames stay dynamic);
//  * every local is in nativ_l_[f] — the twin declares NO pt_value
//    local slots at all (dynamic statements inside still root their
//    pt_value temps through the emit_framed temp frame);
//  * every assignment target is a plain Name (Index/Attr/Tuple targets
//    would materialize pt_l_* storage the twin does not have), every
//    `for` is a range() loop (its int64 induction variable binds
//    straight into a lane; the iterator protocol binds arbitrary
//    values), and locals()/vars()/dir() never appear (their name tables
//    expose pt_l_* slot addresses; globals() reads file-scope storage
//    only and stays allowed);
//  * every return carries an expression (bare `return` is None, not an
//    int), every such expression — and every write to a param — is
//    int-shaped under the twin rules below, and the body's LAST
//    statement is a Return, so control cannot fall off the end (the
//    twin emits an unreachable `return 0` guard for C).
//
// int_shape's twin rules: a param read is int-shaped (the call-site gate
// promised an int) and a call to a still-viable twin callee over
// int-shaped positional args is int-shaped. Mutual recursion (fib calls
// fib) closes through the usual monotone fixpoint: viability is only
// ever removed, so the check can only shrink to a fixpoint.

// Introspection builtins whose emitted name tables expose the addresses
// of the dynamic pt_value slots (pt_l_*) — a twin declares none of them.
static bool twin_banned_expr(const Expr& e) {
  switch (e.kind) {
    case ExprKind::Call: {
      if (e.is_builtin &&
          (e.builtin_name == "locals" || e.builtin_name == "vars" ||
           e.builtin_name == "dir"))
        return true;
      if (e.callee_expr && twin_banned_expr(*e.callee_expr)) return true;
      for (const auto& a : e.args)
        if (a && twin_banned_expr(*a)) return true;
      break;
    }
    case ExprKind::MethodCall:
    case ExprKind::ListLit:
    case ExprKind::SetLit:
    case ExprKind::TupleLit:
    case ExprKind::Join:
      if (e.lhs && twin_banned_expr(*e.lhs)) return true;
      for (const auto& el : e.elems)
        if (el && twin_banned_expr(*el)) return true;
      break;
    case ExprKind::MapLit:
      for (const auto& k : e.keys)
        if (k && twin_banned_expr(*k)) return true;
      for (const auto& el : e.elems)
        if (el && twin_banned_expr(*el)) return true;
      break;
    case ExprKind::Ternary:
      if (e.lhs && twin_banned_expr(*e.lhs)) return true;
      if (e.rhs && twin_banned_expr(*e.rhs)) return true;
      if (e.third && twin_banned_expr(*e.third)) return true;
      break;
    case ExprKind::Slice:
      if (e.lhs && twin_banned_expr(*e.lhs)) return true;
      if (e.rhs && twin_banned_expr(*e.rhs)) return true;
      if (e.third && twin_banned_expr(*e.third)) return true;
      if (e.fourth && twin_banned_expr(*e.fourth)) return true;
      break;
    case ExprKind::Unary:
    case ExprKind::Binary:
    case ExprKind::Logical:
    case ExprKind::Index:
    case ExprKind::Attr:
      if (e.lhs && twin_banned_expr(*e.lhs)) return true;
      if (e.rhs && twin_banned_expr(*e.rhs)) return true;
      break;
    default:
      break;
  }
  return false;
}

// ---- C3 (task-5 A): vararg bulk-entry admissibility (ONE shared walker) ---
// A `function f(a, *more)` may twin into a BULK entry point only when every
// free reference to `more` inside its body sits in exactly one of two
// sanctioned positions:
//   (1) the whole iterable expression of a `for <lhs> in more`, or
//   (2) the single positional argument of the `len(...)` builtin.
// Anything else — indexing, attribute/method call, store/pass/return/
// concat/aliasing into another binding, any other call argument, calling
// `more` itself — declines the bulk path and keeps today's boxed entry
// byte-identically (§0.3). The SAME predicates feed three places: this
// admission scan (analyze_bulk_varargs), twin_scan_stmts' structural For
// rule for bulk candidates, and the _b body's emission gates (For binder,
// len text) — one computation, so admission and emission cannot drift; if
// they ever did, the drift is a LOUD C compile failure, never a silent
// miscompile, because the _b entry declares NO pt_l_<vararg> slot and any
// un-sanctioned read spells that missing storage.
static bool is_vararg_var(const Expr& e, const std::string& v) {
  return e.kind == ExprKind::Var && e.res == VarRes::Param && e.name == v;
}
static bool is_bulk_len_call(const Expr& e, const std::string& v) {
  return e.kind == ExprKind::Call && e.is_builtin && e.builtin_name == "len" &&
         e.args.size() == 1u && e.args[0] &&
         (e.arg_names.empty() || e.arg_names[0].empty()) &&
         is_vararg_var(*e.args[0], v);
}
static bool is_bulk_for(const Stmt& s, const std::string& v) {
  return s.kind == StmtKind::For && s.expr && is_vararg_var(*s.expr, v);
}

// Recursive reference walk over every Expr child edge (same coverage as
// twin_banned_expr). `ok_pos` marks the walk edge as sanctioned — only the
// two gates above ever pass true, and only for the whole expression they
// anchor. Sets `bad` on the first unsanctioned reference and stops early.
static bool bulk_expr_refs(const Expr& e, const std::string& v, bool ok_pos,
                           bool& bad) {
  if (bad) return false;
  switch (e.kind) {
    case ExprKind::Var:
      if (is_vararg_var(e, v) && !ok_pos) bad = true;
      break;
    case ExprKind::VarQual:
      if (e.name == v) bad = true;  // qualified atom: never the plain param
      break;
    case ExprKind::Call:
      if (e.callee == v) { bad = true; break; }  // calling `more` itself
      if (is_bulk_len_call(e, v)) return true;   // SANCTIONED position (2):
                                                  // nothing left to walk
      if (e.callee_expr && !bulk_expr_refs(*e.callee_expr, v, false, bad))
        return false;
      break;
    default:
      break;
  }
  if (bad) return false;
  if (e.lhs && !bulk_expr_refs(*e.lhs, v, false, bad)) return false;
  if (e.rhs && !bulk_expr_refs(*e.rhs, v, false, bad)) return false;
  if (e.third && !bulk_expr_refs(*e.third, v, false, bad)) return false;
  if (e.fourth && !bulk_expr_refs(*e.fourth, v, false, bad)) return false;
  for (const auto& a : e.args)
    if (a && !bulk_expr_refs(*a, v, false, bad)) return false;
  for (const auto& k : e.keys)
    if (k && !bulk_expr_refs(*k, v, false, bad)) return false;
  for (const auto& el : e.elems)
    if (el && !bulk_expr_refs(*el, v, false, bad)) return false;
  return !bad;
}

static bool bulk_stmt_refs(const Stmt& s, const std::string& v, bool& bad);
static bool bulk_target_refs(const AssignTarget& t, const std::string& v,
                             bool& bad) {
  if (bad) return false;
  switch (t.kind) {
    case AssignTarget::Kind::Name:
    case AssignTarget::Kind::Rest:
    case AssignTarget::Kind::VarQual:
      if (t.name == v) bad = true;  // any STORE into `more` rejects outright
      break;
    case AssignTarget::Kind::Index:
    case AssignTarget::Kind::Attr:
      if (t.object && !bulk_expr_refs(*t.object, v, false, bad)) return false;
      if (t.index && !bulk_expr_refs(*t.index, v, false, bad)) return false;
      break;
    case AssignTarget::Kind::Tuple:
      for (const AssignTarget& el : t.elems)
        if (!bulk_target_refs(el, v, bad)) return false;
      break;
  }
  return !bad;
}

// Statement-level walk: mirrors the statement tree the _b entry will emit
// (nested Func/Class declarations are rejected by scan_nested_decls BEFORE
// this ever runs; defensively rejecting them here as well).
static bool bulk_stmt_refs(const Stmt& s, const std::string& v, bool& bad) {
  if (bad) return false;
  switch (s.kind) {
    case StmtKind::Assign:
      if (!bulk_target_refs(s.target, v, bad)) return false;
      if (s.expr && !bulk_expr_refs(*s.expr, v, false, bad)) return false;
      break;
    case StmtKind::ExprStmt:
    case StmtKind::Return:
    case StmtKind::Raise:
      if (s.expr && !bulk_expr_refs(*s.expr, v, false, bad)) return false;
      break;
    case StmtKind::If:
      for (const ExprPtr& c : s.conds)
        if (c && !bulk_expr_refs(*c, v, false, bad)) return false;
      for (const auto& b : s.bodies)
        for (const Stmt& c : b)
          if (!bulk_stmt_refs(c, v, bad)) return false;
      for (const Stmt& c : s.else_body)
        if (!bulk_stmt_refs(c, v, bad)) return false;
      break;
    case StmtKind::While:
      if (s.expr && !bulk_expr_refs(*s.expr, v, false, bad)) return false;
      for (const Stmt& c : s.body)
        if (!bulk_stmt_refs(c, v, bad)) return false;
      break;
    case StmtKind::For:
      // The loop TARGET may not be the vararg itself (a store into `more`
      // through the binder), and the iterable is either the sanctioned
      // whole-param position — then only its BODY still needs walking —
      // or an ordinary expression walked with ok_pos=false.
      if (s.res == VarRes::Param && s.name == v) { bad = true; break; }
      if (!(is_bulk_for(s, v) &&
            s.expr->kind == ExprKind::Var))
        if (s.expr && !bulk_expr_refs(*s.expr, v, false, bad)) return false;
      for (const Stmt& c : s.body)
        if (!bulk_stmt_refs(c, v, bad)) return false;
      break;
    case StmtKind::Switch:
      if (s.expr && !bulk_expr_refs(*s.expr, v, false, bad)) return false;
      for (const CaseArm& a : s.cases)
        for (const Stmt& c : a.body)
          if (!bulk_stmt_refs(c, v, bad)) return false;
      break;
    case StmtKind::Try:
      for (const Stmt& c : s.body)
        if (!bulk_stmt_refs(c, v, bad)) return false;
      for (const ExceptArm& a : s.arms) {
        if (a.name == v) { bad = true; return false; }  // binding collision
        for (const Stmt& c : a.body)
          if (!bulk_stmt_refs(c, v, bad)) return false;
      }
      for (const Stmt& c : s.finally_body)
        if (!bulk_stmt_refs(c, v, bad)) return false;
      break;
    case StmtKind::GlobalDecl:
    case StmtKind::NonlocalDecl:
      for (const std::string& n : s.names)
        if (n == v) { bad = true; return false; }
      break;
    case StmtKind::Func:
    case StmtKind::Class:
      bad = true;  // unreachable: scan_nested_decls rejects first (loud > silent)
      break;
    case StmtKind::Break:
    case StmtKind::Continue:
    case StmtKind::Pass:
      break;
  }
  return !bad;
}

void Emitter::twin_scan_stmts(const std::vector<Stmt>& body, const FuncDecl* f,
                              TwinScan& sc, bool bulk) {
  for (const Stmt& s : body) {
    switch (s.kind) {
      case StmtKind::Assign:
        if (s.target.kind != AssignTarget::Kind::Name) {
          sc.ok = false;  // Index/Attr/Tuple targets materialize pt_l_*
          break;          // storage the twin does not declare
        }
        if (s.expr) {
          if (twin_banned_expr(*s.expr)) sc.ok = false;
          if (s.res == VarRes::Param) sc.param_writes.push_back(s.expr.get());
        }
        break;
      case StmtKind::ExprStmt:
        if (s.expr && twin_banned_expr(*s.expr)) sc.ok = false;
        break;
      case StmtKind::Return:
        if (!s.expr) {
          sc.ok = false;  // bare return: None is not an int
        } else {
          if (twin_banned_expr(*s.expr)) sc.ok = false;
          sc.returns.push_back(s.expr.get());
        }
        break;
      case StmtKind::If:
        for (const ExprPtr& c : s.conds)
          if (!c || twin_banned_expr(*c)) sc.ok = false;
        for (const std::vector<Stmt>& b : s.bodies) twin_scan_stmts(b, f, sc, bulk);
        twin_scan_stmts(s.else_body, f, sc, bulk);
        break;
      case StmtKind::While:
        if (!s.expr || twin_banned_expr(*s.expr)) sc.ok = false;
        twin_scan_stmts(s.body, f, sc, bulk);
        break;
      case StmtKind::For: {
        // Only the open-coded range() loop binds an int64 induction
        // variable straight into a lane; every other iterable runs the
        // iterator protocol over arbitrary values. C3 bulk (task-5 A): a
        // `for m in more` whose iterable is EXACTLY this function's own
        // vararg is the second sanctioned shape — the _b entry binds the
        // same slot from a raw int64 tail element instead of pt_iter —
        // and admission shares is_bulk_for() with the emission gate.
        bool range_for = s.expr && s.expr->kind == ExprKind::Call &&
                         s.expr->is_builtin_range;
        bool vararg_for =
            bulk && f && is_bulk_for(s, f->vararg) &&
            s.expr->kind == ExprKind::Var;
        if (!range_for && !vararg_for) {
          sc.ok = false;
          break;
        }
        if (range_for && twin_banned_expr(*s.expr)) sc.ok = false;  // range(...)
        twin_scan_stmts(s.body, f, sc, bulk);
        break;
      }
      case StmtKind::Switch:
        if (!s.expr || twin_banned_expr(*s.expr)) sc.ok = false;
        for (const CaseArm& arm : s.cases) twin_scan_stmts(arm.body, f, sc, bulk);
        break;
      case StmtKind::Raise:
        if (s.expr && twin_banned_expr(*s.expr)) sc.ok = false;
        break;
      case StmtKind::Try:
        // C2: an ELIDED try emits exactly its body (emit_try skips the
        // setjmp/enter/leave pad and drops the dead except arms), so the
        // twin scans only what will exist. A registered try still
        // disqualifies: its setjmp frame must stay dynamic.
        if (elided_trys_.count(&s))
          twin_scan_stmts(s.body, f, sc, bulk);
        else
          sc.ok = false;
        break;
      case StmtKind::Func:
      case StmtKind::Class:
        sc.ok = false;  // closure hoisting / class write
        break;
      default:  // Pass/Break/Continue/GlobalDecl/NonlocalDecl: no runtime
        break;   // expression to place
    }
  }
}

// Structural twin prefilter: the candidate set the joint lane/twin
// fixpoint (analyze_native_ints) starts from. Every check here is static
// (no lane-set dependency); the "every local in a lane" rule is NOT
// checked here — the lane sets are still shrinking inside the fixpoint, so
// that rule is re-evaluated on every twin sweep there.
std::map<const FuncDecl*, TwinScan> Emitter::twin_prefilter() {
  std::map<const FuncDecl*, TwinScan> cand;
  for (const FuncDecl* f : all_functions()) {
    if (f->is_nested || f->c_name.empty()) continue;  // dynamic-only entry
    if (!f->captures.empty()) continue;               // env-dependent
    if (f->has_vararg || f->has_kwarg) continue;      // extra bound slots
    bool bad = false;
    for (const ExprPtr& d : f->defaults)
      if (d) {
        // C2 (task-5): a compile-time int-literal default is FOLDED INTO the
        // call at each twin call site by the shared binder (caller-stuffing —
        // no per-arity entry): a literal has no effects, so evaluating it at
        // the call vs. at materialization is observationally identical. Any
        // other default keeps today's rejection (its expression may evaluate
        // or raise at materialization time — conservative by one shape, §0.3).
        int64_t dv = 0;
        if (!const_int_lit(d.get(), dv)) {
          bad = true;  // dynamic PT_MISSING slot materialization at entry
          break;
        }
      }
    if (bad) continue;
    for (bool c : f->param_is_cell)
      if (c) {
        bad = true;
        break;
      }
    for (bool c : f->local_is_cell)
      if (c) {
        bad = true;
        break;
      }
    if (bad) continue;
    for (const std::string& p : f->params)
      if (pinned_binding(*f, p)) {
        bad = true;
        break;
      }
    for (const std::string& l : f->locals)
      if (pinned_binding(*f, l)) {
        bad = true;
        break;
      }
    if (bad) continue;
    if (has_own_try(f->body, elided_trys_)) continue;  // setjmp frames
    if (!scan_nested_decls(f->body).empty()) continue;  // closure hoisting
    // Definite return: control cannot fall off the end into the twin's
    // unreachable `return 0` guard.
    if (f->body.empty() || f->body.back().kind != StmtKind::Return ||
        !f->body.back().expr)
      continue;
    TwinScan sc;
    twin_scan_stmts(f->body, f, sc);
    if (!sc.ok) continue;
    cand.emplace(f, std::move(sc));
  }
  return cand;
}

// Post-2C perf: emit the int-lane twin of an int-lane-viable function.
// Same statement tree as the dynamic body, but every binding is an int64
// lane: params are the C parameters (named pt_nl_<p>, the lane names the
// native-read paths already produce; noflag — a param is always
// assigned), locals declare the same int64+flag pair as the dynamic
// body, and every return leaves int64 for the caller's lane. There is no
// bound vector and no local pt_value slot; dynamic statements inside the
// body (print, dynamic calls) root their pt_value temps through the
// emit_framed temp frame exactly like the dynamic body does. The twin
// skips the entry safe point: at entry it holds no GC roots (every lane
// is an int).
void Emitter::emit_function_native(const FuncDecl& f) {
  const FuncDecl* saved_fn = cur_func_;
  const FuncDecl* saved_twin = twin_fn_;
  cur_func_ = &f;
  twin_fn_ = &f;
  indent_ = 2;
  loop_depth_ = 0;
  try_stack_.clear();

  std::string sig;
  for (size_t i = 0; i < f.params.size(); ++i)
    sig += std::string(i ? ", int64_t " : "int64_t ") + "pt_nl_" + f.params[i];
  line("static int64_t " + f.c_name + "_i(" + sig + ") {");

  // Local lanes — same pair as the dynamic body (the flag is the
  // range-for bind target; dead stores vanish, a noflag local never
  // reads it).
  auto it = nativ_l_.find(&f);
  if (it != nativ_l_.end())
    for (const std::string& nl : it->second)
      line(ind() + "int64_t pt_nl_" + nl + " = 0; char pt_nl_ok_" + nl +
           " = 0;");

  emit_framed([&] { emit_body(f.body, /*in_function=*/true); },
              /*frame_end=*/false);
  // Definite-return invariant (twin_prefilter): unreachable in
  // well-formed twins; present so every C path returns a value.
  line(ind() + "return 0; /* unreachable: definite-return twin */");
  line("}");
  line("");

  cur_func_ = saved_fn;
  twin_fn_ = saved_twin;
}

// ---- post-2C perf: definite-assignment for used-flag elision (TODO.txt) -----
//
// A native slot carries a used-before-assignment flag so a read before any
// write raises NameError. That flag costs a load+branch per read and a store
// per write, and the memory ops pin loop counters to the stack so LLVM can't
// keep them in registers — a big tax on hot scalar loops. This pass proves,
// for some slots, that EVERY read is preceded by an assignment on every path,
// making the flag dead. SOUND by construction (conservative):
//   * Straight-line code threads a definitely-assigned (DA) set forward; an
//     assignment adds the slot, a read not in DA disqualifies the slot.
//   * Branch/loop/try bodies are each walked with a COPY of the incoming DA
//     and their assignments do NOT propagate out (so an `if`-only assignment,
//     a loop read before its write, or try/except edges are never DA).
//   * Every region starts DA-empty (params/locals/globals all unset at entry),
//     so a global read inside a function disqualifies that global unless it is
//     (re)assigned earlier in the same function on all paths.
//   * The noflag decision is global per slot: one non-DA read ANYWHERE removes
//     the slot from noflag, keeping its flag. A slot survives only if no such
//     read exists → eliding its flag check + store is safe. Captures the
//     "counter assigned before the loop" pattern (e.g. lab.pen) exactly.
void Emitter::da_check_expr(const Expr& e, std::set<std::string>& da_g,
                            std::set<std::string>& da_l, const FuncDecl* fn) {
  switch (e.kind) {
    case ExprKind::Var: {
      if (e.res == VarRes::Global) {
        if ((nativ_g_.count(e.name) || nativfg_.count(e.name)) &&
            !da_g.count(e.name))
          noflag_g_.erase(e.name);  // unset read → keep the flag
      } else if (e.res == VarRes::Local && fn) {
        auto it = nativ_l_.find(fn);
        auto ft = nativfl_.find(fn);
        if (((it != nativ_l_.end() && it->second.count(e.name)) ||
             (ft != nativfl_.end() && ft->second.count(e.name))) &&
            !da_l.count(e.name)) {
          auto nf = noflag_l_.find(fn);
          if (nf != noflag_l_.end()) nf->second.erase(e.name);
        }
      }
      break;
    }
    case ExprKind::Unary:
      if (e.lhs) da_check_expr(*e.lhs, da_g, da_l, fn);
      break;
    case ExprKind::Binary:
    case ExprKind::Logical:
      if (e.lhs) da_check_expr(*e.lhs, da_g, da_l, fn);
      if (e.rhs) da_check_expr(*e.rhs, da_g, da_l, fn);
      break;
    case ExprKind::Ternary:
      if (e.lhs) da_check_expr(*e.lhs, da_g, da_l, fn);
      if (e.rhs) da_check_expr(*e.rhs, da_g, da_l, fn);
      if (e.third) da_check_expr(*e.third, da_g, da_l, fn);
      break;
    case ExprKind::Call:
      if (e.callee_expr) da_check_expr(*e.callee_expr, da_g, da_l, fn);
      for (const auto& a : e.args)
        if (a) da_check_expr(*a, da_g, da_l, fn);
      break;
    case ExprKind::MethodCall:
      if (e.lhs) da_check_expr(*e.lhs, da_g, da_l, fn);
      for (const auto& a : e.elems)
        if (a) da_check_expr(*a, da_g, da_l, fn);
      break;
    case ExprKind::Attr:
      if (e.lhs) da_check_expr(*e.lhs, da_g, da_l, fn);
      break;
    case ExprKind::Index:
    case ExprKind::Slice:
      if (e.lhs) da_check_expr(*e.lhs, da_g, da_l, fn);
      if (e.rhs) da_check_expr(*e.rhs, da_g, da_l, fn);
      if (e.third) da_check_expr(*e.third, da_g, da_l, fn);
      if (e.fourth) da_check_expr(*e.fourth, da_g, da_l, fn);
      break;
    case ExprKind::ListLit:
    case ExprKind::SetLit:
    case ExprKind::TupleLit:
    case ExprKind::Join:
      for (const auto& el : e.elems)
        if (el) da_check_expr(*el, da_g, da_l, fn);
      break;
    case ExprKind::MapLit:
      for (const auto& k : e.keys)
        if (k) da_check_expr(*k, da_g, da_l, fn);
      for (const auto& v : e.elems)
        if (v) da_check_expr(*v, da_g, da_l, fn);
      break;
    default:
      break;  // literals carry no reads
  }
}

void Emitter::da_walk_stmt(const Stmt& s, std::set<std::string>& da_g,
                           std::set<std::string>& da_l, const FuncDecl* fn) {
  auto mark_assigned = [&](const std::string& name, VarRes res) {
    if (res == VarRes::Global)
      da_g.insert(name);
    else if (res == VarRes::Local)
      da_l.insert(name);
  };
  // Walk a nested block with a COPY of the current DA; assignments inside do
  // not escape (sound: the block may be skipped or looped).
  auto walk_isolated = [&](const std::vector<Stmt>& body) {
    std::set<std::string> g = da_g, l = da_l;
    da_walk_stmts(body, g, l, fn);
  };

  switch (s.kind) {
    case StmtKind::Assign: {
      // RHS reads are checked against the DA *before* this statement.
      if (s.expr) da_check_expr(*s.expr, da_g, da_l, fn);
      if (s.target.kind == AssignTarget::Kind::Name) {
        // A compound assign (`x += …`) READS the target before writing it, so
        // that read must be DA or the slot keeps its flag. A plain assign only
        // writes. Model the target read (if any) before marking it assigned.
        if (!s.op.empty()) {
          if ((nativ_g_.count(s.name) || nativfg_.count(s.name)) &&
              s.res == VarRes::Global && !da_g.count(s.name))
            noflag_g_.erase(s.name);
          else if (s.res == VarRes::Local && fn) {
            auto it = nativ_l_.find(fn);
            auto ft = nativfl_.find(fn);
            if (((it != nativ_l_.end() && it->second.count(s.name)) ||
                 (ft != nativfl_.end() && ft->second.count(s.name))) &&
                !da_l.count(s.name)) {
              auto nf = noflag_l_.find(fn);
              if (nf != noflag_l_.end()) nf->second.erase(s.name);
            }
          }
        }
        mark_assigned(s.name, s.res);
      } else if (s.target.kind == AssignTarget::Kind::Index ||
                 s.target.kind == AssignTarget::Kind::Attr) {
        if (s.target.object) da_check_expr(*s.target.object, da_g, da_l, fn);
        if (s.target.index) da_check_expr(*s.target.index, da_g, da_l, fn);
      }
      // Tuple/Rest targets bind through unpack temps (never native) — no DA.
      break;
    }
    case StmtKind::ExprStmt:
      if (s.expr) da_check_expr(*s.expr, da_g, da_l, fn);
      break;
    case StmtKind::Return:
    case StmtKind::Raise:
      if (s.expr) da_check_expr(*s.expr, da_g, da_l, fn);
      break;
    case StmtKind::If: {
      for (const auto& c : s.conds)
        if (c) da_check_expr(*c, da_g, da_l, fn);
      for (const auto& b : s.bodies) walk_isolated(b);
      if (s.has_else) walk_isolated(s.else_body);
      break;  // DA unchanged after (branches may be skipped)
    }
    case StmtKind::While: {
      if (s.expr) da_check_expr(*s.expr, da_g, da_l, fn);
      walk_isolated(s.body);  // body sees pre-loop DA only (first-iteration safe)
      break;
    }
    case StmtKind::For: {
      if (s.expr) da_check_expr(*s.expr, da_g, da_l, fn);
      // A native range-loop target's assigned-ness is dynamic — the range
      // may be empty — so its used-flag must survive even with zero reads:
      // the globals()/locals()/vars()/dir() name tables consult it.
      if (nativ_g_.count(s.name)) {
        noflag_g_.erase(s.name);
      } else if (fn) {
        auto it = nativ_l_.find(fn);
        if (it != nativ_l_.end() && it->second.count(s.name))
          noflag_l_[fn].erase(s.name);
      }
      walk_isolated(s.body);  // body sees pre-loop DA only (first-iteration safe)
      break;
    }
    case StmtKind::Switch: {
      if (s.expr) da_check_expr(*s.expr, da_g, da_l, fn);
      for (const auto& arm : s.cases) walk_isolated(arm.body);
      break;
    }
    case StmtKind::Try: {
      walk_isolated(s.body);
      for (const auto& a : s.arms) {
        if (a.cond) da_check_expr(*a.cond, da_g, da_l, fn);
        walk_isolated(a.body);
      }
      if (s.has_finally) walk_isolated(s.finally_body);
      break;  // exception edges: no propagation (sound)
    }
    case StmtKind::Func:
    case StmtKind::Class:
    case StmtKind::GlobalDecl:
    case StmtKind::NonlocalDecl:
    case StmtKind::Pass:
    case StmtKind::Break:
    case StmtKind::Continue:
      break;  // no native reads/writes to track here
    default:
      break;
  }
}

void Emitter::da_walk_stmts(const std::vector<Stmt>& body,
                            std::set<std::string>& da_g,
                            std::set<std::string>& da_l, const FuncDecl* fn) {
  for (const Stmt& s : body) da_walk_stmt(s, da_g, da_l, fn);
}

void Emitter::analyze_definite_assign() {
  // Optimistic start: every native slot (int64 and float64 lanes) needs no
  // flag; disqualify on any non-DA read found anywhere.
  noflag_g_ = nativ_g_;
  noflag_l_ = nativ_l_;
  noflag_g_.insert(nativfg_.begin(), nativfg_.end());
  for (const auto& fentry : nativfl_)
    noflag_l_[fentry.first].insert(fentry.second.begin(), fentry.second.end());

  // Global region: top-level statements and the main() block, in emission
  // order (they run sequentially in the generated main()). Globals are unset
  // at program start, so DA begins empty.
  {
    std::set<std::string> da_g, da_l;
    for (const TopLevel& t : prog_.items) {
      if (t.kind == TopKind::Stmt)
        da_walk_stmt(t.stmt, da_g, da_l, /*fn=*/nullptr);
      else if (t.kind == TopKind::Main && t.main)
        da_walk_stmts(t.main->body, da_g, da_l, /*fn=*/nullptr);
    }
  }

  // Each function is its own region; params/locals/globals all start unset.
  for (const FuncDecl* f : all_functions()) {
    std::set<std::string> da_g, da_l;
    // Param defaults evaluate at function-definition (hoist) time and can read
    // globals; with an empty DA those reads conservatively keep the flag.
    for (const auto& d : f->defaults)
      if (d) da_check_expr(*d, da_g, da_l, f);
    da_walk_stmts(f->body, da_g, da_l, f);
  }
}

// ---- Phase 2.1: boxed-slot pt_guard elimination for provably-bound reads ----
//
// pt_guard(slot, name, scope, line) is static inline and its hit path is a
// single tag compare, but every guarded read still pays for the compare plus
// the name/scope/line arguments and the call-shaped setup at THAT site. This
// pass proves, for some boxed slots, that EVERY pt_guard on that slot can
// never see PT_UNSET, so the guard is dead and the bare storage lvalue is
// observably identical (byte-for-byte: same value, no NameError reachable).
//
// SOUND by construction (conservative — one non-provable read anywhere keeps
// the slot's guard):
//   * params/vararg/kwarg: every bound-vector source (exact-VLA paths in
//     pt_call/pt_call_method/pt_class_call and the §13 binder) fills slots
//     with real values or pt_missing(), NEVER pt_unset(), and the callee's
//     slot-materialization (emit_function step 4) copies them into the frame
//     slot/cell and evaluates PT_MISSING defaults — all before the body runs.
//     So a param read in the BODY can never observe PT_UNSET. In a DEFAULT
//     expression of param i (emitted at materialization index i, fresh per
//     call), slots for indices < i are already materialized (bound or their
//     own default applied) and slot i itself holds pt_missing() (pt_guard only
//     fires on PT_UNSET) — but slots for indices > i (and the vararg/kwarg
//     slots, which materialize after all params) are still pt_unset() there,
//     so a read of those MUST keep its guard. A param read that only ever
//     appears in the body (or in a later param's default) stays no-guard.
//   * locals/globals: definite-assignment walk with the same conservative
//     join discipline as analyze_definite_assign() (branches/loops/try bodies
//     walked isolated — their assignments never escape). Unlike the native
//     flag pass, this walk models EVERY read form that can emit a pt_guard
//     (bare Var reads, compound-assignment target reads, tuple-target
//     object/index subexpressions), because the starting assumption is
//     "needs no guard". A capture read (a name that is not an own
//     param/local of the function) is never seeded into that function's
//     bucket, so it is never elided — the enclosing frame may not have
//     assigned the cell yet.
//   * cross-scope (VarQual) reads/writes, function-value reads and
//     Unresolved reads keep their guard (ge_erase_all): their storage is
//     program-lifetime pin/static state this walk does not model.
// The pass is independent of the native lanes (a native slot never reaches a
// pt_guard site), so it may run after analyze_definite_assign().
void Emitter::ge_check_expr(const Expr& e, std::set<std::string>& da_g,
                            std::set<std::string>& da_l, const FuncDecl* fn) {
  switch (e.kind) {
    case ExprKind::Var: {
      if (e.res == VarRes::Global) {
        if (!da_g.count(e.name)) noguard_g_.erase(e.name);  // unset read → guard
      } else if (e.res == VarRes::Local) {
        // Own local → DA rule. A capture (not own) is simply never seeded in
        // this function's bucket, so the erase below is a no-op for it and
        // ge_elide() will report false (guard kept) at its read site.
        if (fn && !da_l.count(e.name)) noguard_l_[fn].erase(e.name);
      } else if (e.res == VarRes::Param) {
        // Defaults context: keep the guard for slots still pt_unset() here.
        // Body context: params/vararg/kwarg are all materialized → do nothing
        // (they stay no-guard); captures aren't seeded → guard kept anyway.
        if (ge_def_pos_ >= 0 && fn) {
          size_t idx = (size_t)-1;
          for (size_t k = 0; k < fn->params.size(); ++k)
            if (fn->params[k] == e.name) { idx = k; break; }
          if (idx == (size_t)-1) {
            noguard_l_[fn].erase(e.name);  // vararg/kwarg or capture: unset here
          } else if ((int)idx > ge_def_pos_) {
            noguard_l_[fn].erase(e.name);  // later param: still pt_unset()
          }
          // idx <= ge_def_pos_: materialized (or pt_missing) → not PT_UNSET.
        }
      } else {
        ge_erase_all(e.name);  // Function-value / Unresolved reads keep guard
      }
      break;
    }
    case ExprKind::VarQual:
      ge_erase_all(e.name);  // cross-scope pin access: keep the guard
      break;
    case ExprKind::Unary:
      if (e.lhs) ge_check_expr(*e.lhs, da_g, da_l, fn);
      break;
    case ExprKind::Binary:
    case ExprKind::Logical:
      if (e.lhs) ge_check_expr(*e.lhs, da_g, da_l, fn);
      if (e.rhs) ge_check_expr(*e.rhs, da_g, da_l, fn);
      break;
    case ExprKind::Ternary:
      if (e.lhs) ge_check_expr(*e.lhs, da_g, da_l, fn);
      if (e.rhs) ge_check_expr(*e.rhs, da_g, da_l, fn);
      if (e.third) ge_check_expr(*e.third, da_g, da_l, fn);
      break;
    case ExprKind::Call:
      if (e.callee_expr) ge_check_expr(*e.callee_expr, da_g, da_l, fn);
      for (const auto& a : e.args)
        if (a) ge_check_expr(*a, da_g, da_l, fn);
      break;
    case ExprKind::MethodCall:
      if (e.lhs) ge_check_expr(*e.lhs, da_g, da_l, fn);
      for (const auto& a : e.elems)
        if (a) ge_check_expr(*a, da_g, da_l, fn);
      break;
    case ExprKind::Attr:
      if (e.lhs) ge_check_expr(*e.lhs, da_g, da_l, fn);
      break;
    case ExprKind::Index:
    case ExprKind::Slice:
      if (e.lhs) ge_check_expr(*e.lhs, da_g, da_l, fn);
      if (e.rhs) ge_check_expr(*e.rhs, da_g, da_l, fn);
      if (e.third) ge_check_expr(*e.third, da_g, da_l, fn);
      if (e.fourth) ge_check_expr(*e.fourth, da_g, da_l, fn);
      break;
    case ExprKind::ListLit:
    case ExprKind::SetLit:
    case ExprKind::TupleLit:
    case ExprKind::Join:
      for (const auto& el : e.elems)
        if (el) ge_check_expr(*el, da_g, da_l, fn);
      break;
    case ExprKind::MapLit:
      for (const auto& k : e.keys)
        if (k) ge_check_expr(*k, da_g, da_l, fn);
      for (const auto& v : e.elems)
        if (v) ge_check_expr(*v, da_g, da_l, fn);
      break;
    default:
      break;  // literals carry no reads
  }
}

void Emitter::ge_walk_stmt(const Stmt& s, std::set<std::string>& da_g,
                           std::set<std::string>& da_l, const FuncDecl* fn) {
  auto mark_assigned = [&](const std::string& name, VarRes res) {
    if (res == VarRes::Global)
      da_g.insert(name);
    else if (res == VarRes::Local)
      da_l.insert(name);
    // Params are handled by the materialization rule, not the DA set.
  };
  // Walk a nested block with a COPY of the current DA; assignments inside do
  // not escape (sound: the block may be skipped or looped).
  auto walk_isolated = [&](const std::vector<Stmt>& body) {
    std::set<std::string> g = da_g, l = da_l;
    ge_walk_stmts(body, g, l, fn);
  };
  // Model the object/index read subexpressions of an assignment target leaf.
  std::function<void(const AssignTarget&)> walk_tgt_reads =
      [&](const AssignTarget& t) {
        if (t.kind == AssignTarget::Kind::Index ||
            t.kind == AssignTarget::Kind::Attr) {
          if (t.object) ge_check_expr(*t.object, da_g, da_l, fn);
          if (t.index) ge_check_expr(*t.index, da_g, da_l, fn);
        }
        for (const AssignTarget& sub : t.elems) walk_tgt_reads(sub);
      };

  switch (s.kind) {
    case StmtKind::Assign: {
      // RHS reads are checked against the DA *before* this statement.
      if (s.expr) ge_check_expr(*s.expr, da_g, da_l, fn);
      if (s.target.kind == AssignTarget::Kind::Name) {
        // A compound assign (`x += …`) READS the target before writing it, so
        // that read must be DA or the slot keeps its guard. A plain assign
        // only writes. Model the target read (if any) before marking it.
        if (!s.op.empty()) {
          if (s.res == VarRes::Global) {
            if (!da_g.count(s.name)) noguard_g_.erase(s.name);
          } else if (s.res == VarRes::Local && fn && !da_l.count(s.name)) {
            noguard_l_[fn].erase(s.name);
          }
          // Param target in body context is materialized → no guard needed.
        }
        mark_assigned(s.name, s.res);
      } else if (s.target.kind == AssignTarget::Kind::Index ||
                 s.target.kind == AssignTarget::Kind::Attr) {
        walk_tgt_reads(s.target);
      } else if (s.target.kind == AssignTarget::Kind::VarQual) {
        ge_erase_all(s.target.name);  // cross-scope write: keep the guard
        walk_tgt_reads(s.target);
      }
      // Tuple/Rest targets bind through unpack temps: a partial unpack can
      // raise, so do NOT mark the bound names assigned (any later read keeps
      // its guard); still model their object/index read subexpressions.
      else if (s.target.kind == AssignTarget::Kind::Tuple ||
               s.target.kind == AssignTarget::Kind::Rest) {
        walk_tgt_reads(s.target);
      }
      break;
    }
    case StmtKind::ExprStmt:
      if (s.expr) ge_check_expr(*s.expr, da_g, da_l, fn);
      break;
    case StmtKind::Return:
    case StmtKind::Raise:
      if (s.expr) ge_check_expr(*s.expr, da_g, da_l, fn);
      break;
    case StmtKind::If: {
      for (const auto& c : s.conds)
        if (c) ge_check_expr(*c, da_g, da_l, fn);
      for (const auto& b : s.bodies) walk_isolated(b);
      if (s.has_else) walk_isolated(s.else_body);
      break;  // DA unchanged after (branches may be skipped)
    }
    case StmtKind::While: {
      if (s.expr) ge_check_expr(*s.expr, da_g, da_l, fn);
      walk_isolated(s.body);  // body sees pre-loop DA only (first-iteration safe)
      break;
    }
    case StmtKind::For: {
      if (s.expr) ge_check_expr(*s.expr, da_g, da_l, fn);
      // Bind-before-body (§5): pre-mark the loop target inside the isolated
      // body so reads of it there are provably bound. The range may be empty,
      // so the assignment does NOT escape the loop (not marked in da after).
      {
        std::set<std::string> g = da_g, l = da_l;
        if (s.res == VarRes::Global)
          g.insert(s.name);
        else if (s.res == VarRes::Local)
          l.insert(s.name);
        ge_walk_stmts(s.body, g, l, fn);
      }
      break;
    }
    case StmtKind::Switch: {
      if (s.expr) ge_check_expr(*s.expr, da_g, da_l, fn);
      for (const auto& arm : s.cases) walk_isolated(arm.body);
      break;
    }
    case StmtKind::Try: {
      walk_isolated(s.body);
      for (const auto& a : s.arms) {
        if (a.cond) ge_check_expr(*a.cond, da_g, da_l, fn);
        // `as name` is bound when the arm runs → provably bound in the body.
        std::set<std::string> g = da_g, l = da_l;
        if (!a.name.empty()) {
          if (a.res == VarRes::Global)
            g.insert(a.name);
          else if (a.res == VarRes::Local)
            l.insert(a.name);
        }
        ge_walk_stmts(a.body, g, l, fn);
      }
      if (s.has_finally) walk_isolated(s.finally_body);
      break;  // exception edges: no propagation (sound)
    }
    case StmtKind::Func:
    case StmtKind::Class:
    case StmtKind::GlobalDecl:
    case StmtKind::NonlocalDecl:
    case StmtKind::Pass:
    case StmtKind::Break:
    case StmtKind::Continue:
      break;  // no guarded reads to model here
    default:
      break;
  }
}

void Emitter::ge_walk_stmts(const std::vector<Stmt>& body,
                            std::set<std::string>& da_g,
                            std::set<std::string>& da_l, const FuncDecl* fn) {
  for (const Stmt& s : body) ge_walk_stmt(s, da_g, da_l, fn);
}

// ---------------------------------------------------------------------------
// C2 (task-5): the try-elision x lane JOINT monotone proof -------------------
//
// run() drives: init (optimistic top) then { lane/float/DA analyses;
// retract } until a round removes nothing — the shipped state, then.
//
// State:
//   elided_trys_    tries emitted WITHOUT registration (emit_try's elided
//                   path: body + normal-path finally copy, no setjmp, no
//                   except arms — dead by construction);
//   no_raise_fns_   function bodies proven unable to raise a Pentest
//                   exception on any path (rule (c) input: a call to a
//                   callee outside this set may raise).
// Rule (a): a try whose BODY can raise re-registers (elided_trys_ erase).
// Rule (b): a function holding ANY still-registered try leaves no_raise_fns_;
//           so does one whose body can raise (rule (c) depends on it).
// Rule (c): calls proven through no_raise_fns_ tighten callers' try bodies.
// Only ERASURES happen -> termination; a round that erases nothing was
// re-proven against exactly the state analyses just computed -> ships
// (conservative by one slot, docs/performance/optimization.md §0.3).
//
// WHY ELISION IS SOUND / WHY LANES COME BACK: see the declaration block near
// elided_trys_ and emit_try's elided path — a function whose own tries are
// ALL elided holds no setjmp in its activation at all, so C11 6.2.2.1
// (indeterminate non-volatile automatics after longjmp) can never observe
// its lane pairs; a single live pad disqualifies everything as before.
//
// HOW THE PROOF MIRRORS THE EMITTER (the byte-parity contract):
// every judgement below re-uses the EMITTER's own predicates over the same
// final states, so admission == emission began, and the raise overlay
// re-walks exactly the operations emission performs. Two invariants make it
// sound to judge ONE tree that is emitted twice (dynamic entry + twin):
//   * a lane-admitted formula's RAISE SURFACE is boxing-independent
//     (pt_ndiv/nmod/shl/shr/pow and the used-flag reads are the same
//     conditions in both spellings; boxing itself never raises);
//   * Param reads are admitted ONLY as bare reads (pt_guard can never fire —
//     §13 fills slots with values or PT_MISSING, never PT_UNSET — and a
//     twin param is the raw int64), never UNDER an operator: in the dynamic
//     entry a param holds an arbitrary pt_value, so pt_lt_f(int, <text>)
//     could raise there even though the twin's raw compare cannot. That is
//     the "type-stability" clause: comparisons and arithmetic operands must
//     be proven-number (lane slots, literals, lane-twin call results), not
//     merely lane-spellable.
// Anything unprovable -> false -> the try re-registers / callee loses
// no-raise: only performance is lost, an error path never is.

// Every region root (top-level statements, main() body, each function body)
// with the FuncDecl whose frame the statements live in (nullptr at module
// level). Nested function/class declarations own their own FuncDecl roots.
template <typename CB>
static void each_region_root(const Program& prog,
                             const std::vector<const FuncDecl*>& fns, CB&& cb) {
  for (const TopLevel& t : prog.items) {
    if (t.kind == TopKind::Stmt)
      cb(t.stmt, static_cast<const FuncDecl*>(nullptr));
    else if (t.kind == TopKind::Main && t.main)
      for (const Stmt& s : t.main->body)
        cb(s, static_cast<const FuncDecl*>(nullptr));
  }
  for (const FuncDecl* f : fns)
    for (const Stmt& s : f->body) cb(s, f);
}

// Callee-name -> viable twin decl map for int_shape's/float_shape's twin
// Call rules (the shared binder needs the decl). Built from the final
// registries, which analyze_native_ints rebuilds every round.
static std::map<std::string, const FuncDecl*> decl_map_of(
    const std::set<const FuncDecl*>& decls) {
  std::map<std::string, const FuncDecl*> m;
  for (const FuncDecl* f : decls) m.emplace(f->name, f);
  return m;
}

// ---- never-raise judgements (see declaration block near elided_trys_) -----

bool Emitter::native_read_no_raise(const std::string& name, VarRes res,
                                   const FuncDecl* fn) const {
  // Param: a §13 bound slot never holds PT_UNSET (values or PT_MISSING fill
  // it before the body runs) so pt_guard can never fire on a body read; a
  // twin param is the raw int64 (no check at all). Reads inside a param
  // DEFAULT expression live outside every try body (defaults belong to the
  // FuncDecl root), so they are never judged here.
  if (res == VarRes::Param) return fn != nullptr;
  if (res == VarRes::Global) {
    if (!slot_is_int_lane(name, res, fn) && !slot_is_float_lane(name, res, fn))
      return false;  // boxed read -> pt_guard can fire NameError
    return noflag_g_.count(name) != 0;
  }
  if (res == VarRes::Local && fn) {
    if (!slot_is_int_lane(name, res, fn) && !slot_is_float_lane(name, res, fn))
      return false;
    auto it = noflag_l_.find(fn);
    if (it == noflag_l_.end()) return false;
    return it->second.count(name) != 0;
  }
  return false;  // VarQual / function value / unresolved keep their guard
}

bool Emitter::lane_int_allowed(const Expr& e, const FuncDecl* fn) const {
  // Exactly the int-lane FAMILY admission, reusing int_shape over the final
  // maps with slot_write=false: a param-reading formula is not lane-spellable
  // by the dynamic body (see the type-stability clause above), and the Call
  // rule goes through the same shared binder emission uses.
  std::map<std::string, const FuncDecl*> tdecls = decl_map_of(nativ_fn_decls_);
  return int_shape(e, fn, nativ_g_, nativ_l_, &nativ_fns_,
                   /*slot_write=*/false, &tdecls, &nativfg_, &nativfl_,
                   &int_lists_, &int_list_da_reads_);
}

bool Emitter::lane_int_no_raise(const Expr& e, const FuncDecl* fn) const {
  if (!lane_int_allowed(e, fn)) return false;
  switch (e.kind) {
    case ExprKind::IntLit:
      return true;
    case ExprKind::Var:
      return native_read_no_raise(e.name, e.res, fn);
    case ExprKind::Unary:
      // "-" / "~" are the wrapping int paths (pt_nwrap_neg / pt_nbnot):
      // branch-free once the operand read is safe.
      return e.lhs && lane_int_no_raise(*e.lhs, fn);
    case ExprKind::Binary: {
      if (!e.lhs || !e.rhs) return false;
      if (e.op == "/" || e.op == "%") {
        // pt_ndiv/pt_nmod raise ONLY on a zero divisor (INT64_MIN/-1 wraps
        // are handled in the helpers themselves) — provable exactly when
        // the divisor is a non-zero literal.
        int64_t v = 0;
        return const_int_lit(e.rhs.get(), v) && v != 0 &&
               lane_int_no_raise(*e.lhs, fn) && lane_int_no_raise(*e.rhs, fn);
      }
      if (e.op == "<<" || e.op == ">>") {
        // pt_nwrap_shl/shr keep the 0..63 ValueError.
        int64_t v = 0;
        return const_int_lit(e.rhs.get(), v) && v >= 0 && v <= 63 &&
               lane_int_no_raise(*e.lhs, fn) && lane_int_no_raise(*e.rhs, fn);
      }
      // + - * & | ^: wrapping / branch-free int paths.
      return lane_int_no_raise(*e.lhs, fn) && lane_int_no_raise(*e.rhs, fn);
    }
    case ExprKind::Call: {
      if (!e.is_builtin) {
        if (e.callee_expr || e.callee.empty()) return false;
        const FuncDecl* cf = find_function(e.callee);
        if (!cf || !nativ_fn_decls_.count(cf)) return false;
        if (!no_raise_fns_.count(cf)) return false;  // rule (c)
        std::vector<const Expr*> bound;
        if (!bind_call_args(e, *cf, bound)) return false;
        for (const Expr* b : bound)
          if (!b || !lane_int_no_raise(*b, fn)) return false;
        return true;
      }
      const std::string& n = e.builtin_name;
      if (n == "abs" || n == "round")  // pt_nabs / identity
        return e.args.size() == 1u && e.args[0] &&
               lane_int_no_raise(*e.args[0], fn);
      if (n == "pow") {  // pt_npow raises ValueError on a negative exponent
        int64_t v = 0;
        return e.args.size() == 2u && e.args[0] && e.args[1] &&
               lane_int_no_raise(*e.args[0], fn) &&
               lane_int_no_raise(*e.args[1], fn) &&
               const_int_lit(e.args[1].get(), v) && v >= 0;
      }
      if (n == "min" || n == "max")  // pt_nmin/pt_nmax: branch-free
        return e.args.size() == 2u && e.args[0] && e.args[1] &&
               lane_int_no_raise(*e.args[0], fn) &&
               lane_int_no_raise(*e.args[1], fn);
      return false;
    }
    default:
      return false;
  }
}

bool Emitter::lane_float_no_raise(const Expr& e, const FuncDecl* fn) const {
  auto tdecls = decl_map_of(nativ_fn_decls_);
  if (!float_shape(e, fn, nativfg_, nativfl_, nativ_g_, nativ_l_, &nativ_fns_,
                   &tdecls))
    return false;
  // Child checker: recurse in the branch float_shape itself chose (a float
  // child through the float rules, a mixed INT child through the int rules).
  auto child = [&](const Expr& c) {
    return float_shape(c, fn, nativfg_, nativfl_, nativ_g_, nativ_l_,
                       &nativ_fns_, &tdecls)
               ? lane_float_no_raise(c, fn)
               : lane_int_no_raise(c, fn);
  };
  switch (e.kind) {
    case ExprKind::FloatLit:
      return true;
    case ExprKind::Var:
      return native_read_no_raise(e.name, e.res, fn);
    case ExprKind::Unary:
      return e.lhs && child(*e.lhs);
    case ExprKind::Binary: {
      if (!e.lhs || !e.rhs) return false;
      if (e.op == "/") {
        // pt_nfdiv keeps pt_div_f's y == 0.0 ZeroDivisionError on the
        // CONVERTED divisor: provable only when the divisor is a non-zero
        // literal (an int literal converts exactly).
        int64_t iv = 0;
        if (e.rhs->kind == ExprKind::IntLit) {
          if (!const_int_lit(e.rhs.get(), iv) || iv == 0) return false;
        } else if (e.rhs->kind == ExprKind::FloatLit) {
          if (e.rhs->fval == 0.0) return false;
        } else {
          return false;  // value-dependent zero: keep the raise
        }
      }
      // + - * / : IEEE / converting fast paths, no raise beyond == 0 above.
      return child(*e.lhs) && child(*e.rhs);
    }
    default:
      return false;
  }
}

bool Emitter::cond_never_raise(const Expr& e, const FuncDecl* fn) const {
  switch (e.kind) {
    case ExprKind::BoolLit:
      return true;
    case ExprKind::Var:
      return native_read_no_raise(e.name, e.res, fn);
    case ExprKind::Unary:
      // `not <cond>`: pt_truthy never raises (only PT_UNSET would, and no
      // proven operand can carry it).
      return e.op == "not" && e.lhs && cond_never_raise(*e.lhs, fn);
    case ExprKind::Binary: {
      if (!e.lhs || !e.rhs) return false;
      if (e.op != "<" && e.op != "<=" && e.op != ">" && e.op != ">=" &&
          e.op != "==" && e.op != "!=")
        return false;
      // Mirror native_cond_text's branch choice: int compares first, then
      // float-vs-float. Both operands must be TYPE-proven numbers (a Param
      // or boxed operand would leave pt_lt_f/pt_cmp3 at the mercy of the
      // value's runtime type in the dynamic body — type-stability clause).
      if (lane_int_allowed(*e.lhs, fn) && lane_int_allowed(*e.rhs, fn))
        return lane_int_no_raise(*e.lhs, fn) &&
               lane_int_no_raise(*e.rhs, fn);
      auto tdecls = decl_map_of(nativ_fn_decls_);
      if (float_shape(*e.lhs, fn, nativfg_, nativfl_, nativ_g_, nativ_l_,
                      &nativ_fns_, &tdecls) &&
          float_shape(*e.rhs, fn, nativfg_, nativfl_, nativ_g_, nativ_l_,
                      &nativ_fns_, &tdecls))
        return lane_float_no_raise(*e.lhs, fn) &&
               lane_float_no_raise(*e.rhs, fn);
      return false;
    }
    case ExprKind::Logical:
      return e.lhs && e.rhs && cond_never_raise(*e.lhs, fn) &&
             cond_never_raise(*e.rhs, fn);
    default:
      return false;
  }
}

bool Emitter::expr_never_raise(const Expr& e, const FuncDecl* fn) const {
  // Family pick, lane first — see the invariants in the block comment: a
  // proven value-typed formula has the same raise surface in both spellings,
  // so this judges the DYNAMIC and the TWIN emission with one verdict.
  if (lane_int_allowed(e, fn)) return lane_int_no_raise(e, fn);
  auto tdecls = decl_map_of(nativ_fn_decls_);
  if (float_shape(e, fn, nativfg_, nativfl_, nativ_g_, nativ_l_, &nativ_fns_,
                  &tdecls))
    return lane_float_no_raise(e, fn);
  switch (e.kind) {
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
    case ExprKind::StrLit:
    case ExprKind::BytesLit:
    case ExprKind::BoolLit:
    case ExprKind::NoneLit:
      return true;  // literals materialize with no checks (PT_STR static)
    case ExprKind::Var:
      // A bare read: param (never PT_UNSET — see native_read_no_raise) or
      // ... anything else would have been admitted by the lane branches
      // above, and a guarded/boxed read keeps its NameError.
      return e.res == VarRes::Param && fn != nullptr;
    case ExprKind::Unary:
      return e.op == "not" && e.lhs && expr_never_raise(*e.lhs, fn);
    case ExprKind::Logical:
      // pt_truthy itself never raises; each side still evaluates first.
      return e.lhs && e.rhs && expr_never_raise(*e.lhs, fn) &&
             expr_never_raise(*e.rhs, fn);
    case ExprKind::Ternary:
      return e.lhs && e.rhs && e.third && expr_never_raise(*e.lhs, fn) &&
             expr_never_raise(*e.rhs, fn) && expr_never_raise(*e.third, fn);
    case ExprKind::ListLit:
    case ExprKind::TupleLit:
      // pt_list_of/pt_tuple_of only copy already-evaluated values (no
      // hashing, no type checks) — contra SetLit/MapLit, whose constructors
      // hash their items and can raise TypeError (unhashable).
      for (const auto& el : e.elems)
        if (!el || !expr_never_raise(*el, fn)) return false;
      return true;
    case ExprKind::Call: {
      // The non-lane static path: a bare-name user call in this frame goes
      // through bound_slots_text + the direct entry — no raise of its own;
      // raise-ness = args/defaults (all slots the binder fills) + callee.
      if (e.is_builtin || e.is_builtin_print || e.is_builtin_range)
        return false;  // builtins validate at runtime (len/str/print/range/...)
      if (e.callee_expr || e.callee.empty()) return false;
      const FuncDecl* cf = find_function(e.callee);
      if (!cf || !no_raise_fns_.count(cf)) return false;
      std::vector<const Expr*> bound;
      if (!bind_call_args(e, *cf, bound)) return false;
      for (const Expr* b : bound)
        if (!b || !expr_never_raise(*b, fn)) return false;
      return true;
    }
    default:
      // Binary outside the lane families (text ops, mixed-type compares,
      // boxed arithmetic), Index/Slice/MethodCall/Attr/Join/VarQual/Map/Set:
      // every one of them can raise dynamically — conservative false.
      return false;
  }
}

bool Emitter::stmt_never_raise(const Stmt& s, const FuncDecl* fn) const {
  switch (s.kind) {
    case StmtKind::Assign: {
      if (!s.expr) return false;
      if (s.target.kind != AssignTarget::Kind::Name)
        return false;  // Index/Attr/pattern stores: protocol raises
      const bool plain = s.op.empty();
      // Family pick mirrors emit_stmt exactly: int lane -> float lane ->
      // fvec kernel -> dynamic, with the emission's own admission predicates
      // (a mismatch would be a codegen internal failure, so the branches
      // below can never disagree with the shipped code).
      if (slot_is_int_lane(s.name, s.res, fn)) {
        auto tdecls = decl_map_of(nativ_fn_decls_);
        if (!int_shape(*s.expr, fn, nativ_g_, nativ_l_, &nativ_fns_,
                       /*slot_write=*/false, &tdecls, &nativfg_, &nativfl_))
          return false;
        if (!native_read_no_raise(s.name, s.res, fn)) return false;  // compound cur
        if (!lane_int_no_raise(*s.expr, fn)) return false;
        if (plain) return true;
        // Native compound operators: div/mod keep the zero-divisor raise,
        // shifts keep the 0..63 ValueError; every other admitted op wraps.
        if (s.op == "/=" || s.op == "%=") {
          int64_t v = 0;
          return const_int_lit(s.expr.get(), v) && v != 0;
        }
        if (s.op == "<<=" || s.op == ">>=") {
          int64_t v = 0;
          return const_int_lit(s.expr.get(), v) && v >= 0 && v <= 63;
        }
        return native_compound_op(s.op);
      }
      if (slot_is_float_lane(s.name, s.res, fn)) {
        auto tdecls = decl_map_of(nativ_fn_decls_);
        if (!float_shape(*s.expr, fn, nativfg_, nativfl_, nativ_g_, nativ_l_,
                         &nativ_fns_, &tdecls))
          return false;
        if (!native_read_no_raise(s.name, s.res, fn)) return false;
        if (!lane_float_no_raise(*s.expr, fn)) return false;
        if (plain) return true;
        if (s.op != "+=" && s.op != "-=" && s.op != "*=" && s.op != "/=")
          return false;  // the float fixpoint never admits other ops anyway
        if (s.op == "/=") {
          const Expr& r = *s.expr;
          if (r.kind == ExprKind::IntLit) {
            int64_t v = 0;
            if (!const_int_lit(&r, v) || v == 0) return false;
          } else if (r.kind == ExprKind::FloatLit) {
            if (r.fval == 0.0) return false;
          } else {
            return false;
          }
        }
        return true;
      }
      int fk = 0;
      if (fvec_slot_kind(s.name, s.res, fn, fk))
        return false;  // fused kernels have no raise proof here (conservative)
      if (!plain) return false;  // dynamic compound: guarded read + typed op
      return expr_never_raise(*s.expr, fn);  // a plain store cannot raise
    }
    case StmtKind::ExprStmt:
      return s.expr && expr_never_raise(*s.expr, fn);
    case StmtKind::Return:
      return !s.expr || expr_never_raise(*s.expr, fn);
    case StmtKind::If: {
      for (const ExprPtr& c : s.conds)
        if (!c || !cond_never_raise(*c, fn)) return false;
      for (const auto& b : s.bodies)
        if (!body_never_raise(b, fn)) return false;
      if (s.has_else && !body_never_raise(s.else_body, fn)) return false;
      return true;
    }
    case StmtKind::While:
      return s.expr && cond_never_raise(*s.expr, fn) &&
             body_never_raise(s.body, fn);
    case StmtKind::For: {
      if (!s.expr) return false;
      int64_t a = 0, b = 0, c = 0;
      // Only the C1 literal-bounds header (no pt_range3 validation, no
      // iterator protocol) is provably free of raises; both emit the same
      // counted latch, same bound checks folded away.
      if (!const_range_bounds(*s.expr, a, b, c)) return false;
      return body_never_raise(s.body, fn);
    }
    case StmtKind::Switch: {
      if (!s.expr || s.cases.empty()) return false;
      // C1 native switch: proven-int scrutinee + int labels + no arm-level
      // break shaping (native_switch_ok) equals a C switch over plain int64
      // equality — a ladder/pt_eq_f chain is NOT proven (its TypeError edge
      // on an unknown scrutinee type stays live).
      if (!native_switch_ok(s)) return false;
      if (!lane_int_allowed(*s.expr, fn) ||
          !lane_int_no_raise(*s.expr, fn))
        return false;
      for (const CaseArm& a : s.cases)
        if (!body_never_raise(a.body, fn)) return false;
      return true;
    }
    case StmtKind::Try:
      // A registered try's body was proven RAISING by rule (a) — its handler
      // exists precisely because this frame's exceptions are live here.
      // An elided try emits body + finally (arms are dropped), never its
      // arms' code; the finally copy IS emitted on every path — including
      // break/continue/return exit edges — so it gates the ENCLOSING
      // region's raise judgement, but NOT this try's own elision (rule (a)
      // checks the body only): a raise inside finally is never caught by the
      // same try's arms (M2-2C §2.6), registered or elided.
      if (!elided_trys_.count(&s)) return false;
      return body_never_raise(s.body, fn) &&
             (!s.has_finally || body_never_raise(s.finally_body, fn));
    case StmtKind::Raise:
      return false;
    case StmtKind::Break:
    case StmtKind::Continue:
    case StmtKind::Pass:
      // exit statements run only the finally copies of enclosing tries,
      // each accounted at its own Try node as the walker descends.
      return true;
    case StmtKind::Func:
      return true;  // hoisted declaration: no runtime effect at this point
    case StmtKind::GlobalDecl:
    case StmtKind::NonlocalDecl:
      return true;  // emit comments / nothing
    case StmtKind::Class:
      return false;  // top-level only; class machinery unproven (conservative)
    default:
      return false;
  }
}

bool Emitter::body_never_raise(const std::vector<Stmt>& body,
                               const FuncDecl* fn) const {
  for (const Stmt& s : body)
    if (!stmt_never_raise(s, fn)) return false;
  return true;
}

// Optimistic top: EVERY try elided, EVERY function no-raise. run()'s loop
// only ever shrinks these (see the block comment above) — the analyses below
// compute their lanes under exactly this elision split first, so the first
// retract round already judges the shipped-shape state.
void Emitter::init_try_elision() {
  elided_trys_.clear();
  no_raise_fns_.clear();
  std::vector<const FuncDecl*> fns = all_functions();
  each_region_root(prog_, fns, [&](const Stmt& root, const FuncDecl*) {
    std::vector<const Stmt*> found;
    collect_try_stmts(root, found);
    for (const Stmt* t : found) elided_trys_.insert(t);
  });
  for (const FuncDecl* f : fns) no_raise_fns_.insert(f);
}

// One monotone proof round (retract-only; true = shrank). Judged against the
// lane/float/DA/twin states the current run() iteration JUST computed, and
// in dependency order: try elisions first (rule (a)), then functions (rules
// (b)/(c)) — any staleness between the two (a nested try retracted after its
// enclosing region was judged) is re-caught in the next round, which only
// ever erases, so the loop converges.
bool Emitter::retract_try_failures() {
  bool changed = false;
  static int c2dbg = -1;
  if (c2dbg < 0) c2dbg = getenv("PT_C2_DEBUG") ? 1 : 0;
  std::vector<const FuncDecl*> fns = all_functions();
  // Rule (a): an elided try stays elided only while its BODY proves
  // non-raising under the current state.
  each_region_root(prog_, fns, [&](const Stmt& root, const FuncDecl* fn) {
    std::vector<const Stmt*> found;
    collect_try_stmts(root, found);
    for (const Stmt* t : found) {
      if (!elided_trys_.count(t)) continue;
      if (!body_never_raise(t->body, fn)) {
        if (c2dbg)
          fprintf(stderr, "[c2] retract try line %d (fn %s)\n", t->loc.line,
                  fn ? fn->name.c_str() : "<module>");
        elided_trys_.erase(t);
        changed = true;
      } else if (c2dbg) {
        fprintf(stderr, "[c2] keep    try line %d (fn %s)\n", t->loc.line,
                fn ? fn->name.c_str() : "<module>");
      }
    }
  });
  // Rules (b)/(c): a live registered try OR an unprovable body removes the
  // function from the no-raise set.
  for (const FuncDecl* f : fns) {
    if (!no_raise_fns_.count(f)) continue;
    if (has_own_try(f->body, elided_trys_) ||
        !body_never_raise(f->body, f)) {
      if (c2dbg)
        fprintf(stderr, "[c2] drop no_raise %s (live-try=%d)\n",
                f->name.c_str(),
                (int)has_own_try(f->body, elided_trys_));
      no_raise_fns_.erase(f);
      changed = true;
    }
  }
  return changed;
}

// ---- C3 (task-5 A): vararg bulk-entry admissibility registry --------------
// Runs AFTER the try-elision fixpoint settles (it consults elided_trys_ like
// twin_prefilter does) and independently of the lane/twin fixpoint: a bulk
// callee's result stays a boxed pt_value, so admitting it can never change
// an existing lane/twin fact. Admission = twin_prefilter's structural rules
// (minus the has_vararg rejection, plus the sanctioned vararg-For position
// in twin_scan_stmts) + the exhaustiveness of the shared reference walker.
// ANY
// unprovable shape falls back to today's boxed entry (byte-parity by
// construction, §0.3). Computed exactly once per run(); emission and the
// call-site binder only ever READ bulk_ok_.
void Emitter::analyze_bulk_varargs() {
  bulk_ok_.clear();
  for (const FuncDecl* f : all_functions()) {
    if (!f->has_vararg || f->has_kwarg) continue;   // exactly one tail slot
    if (f->is_nested || f->c_name.empty()) continue;  // dynamic-only entry
    if (!f->captures.empty()) continue;             // env-dependent
    bool bad = false;
    for (bool c : f->param_is_cell)
      if (c) { bad = true; break; }                 // mirrored boxed storage
    if (bad) continue;
    for (bool c : f->local_is_cell)
      if (c) { bad = true; break; }
    if (bad) continue;
    if (f->vararg_is_cell) continue;                // captured tail slot
    for (const std::string& p : f->params)
      if (pinned_binding(*f, p)) { bad = true; break; }
    if (bad) continue;
    for (const std::string& l : f->locals)
      if (pinned_binding(*f, l)) { bad = true; break; }
    if (bad) continue;
    if (has_own_try(f->body, elided_trys_)) continue;  // shared lane rules
    if (!scan_nested_decls(f->body).empty()) continue; // closure hoisting
    if (f->body.empty() || f->body.back().kind != StmtKind::Return ||
        !f->body.back().expr)
      continue;  // definite return (same guard twins use)
    // Structural statement rules — the SAME walker twins use, with the one
    // sanctioned addition (vararg For) enabled by the bulk flag.
    TwinScan sc;
    twin_scan_stmts(f->body, f, sc, /*bulk=*/true);
    if (!sc.ok) continue;
    // The reference admissibility scan itself: every free `more` must be
    // exactly the whole iterable of `for m in more` or the single argument
    // of len(more) — the walker flags anything else (see block comment at
    // is_vararg_var / is_bulk_for / is_bulk_len_call, shared with emission).
    bool refbad = false;
    for (const Stmt& s : f->body) {
      if (!bulk_stmt_refs(s, f->vararg, refbad)) break;
      if (refbad) break;
    }
    if (refbad) continue;
    bulk_ok_.insert(f);
  }
}

// ---- C3 (task-5, Route-2 step 4b1): bulk-body lanes + int-return fact ----
//
// PER-FUNCTION facts about admitted bulk callees, live inside the joint
// fixpoint loop (see run()): everything here is REMOVAL-ONLY from an
// optimistic top — analyze_bulk_varargs() re-seeds the admission registry
// from the CURRENT try-elision split each round, so the combined state
// only ever shrinks and the shipped round is the proven one (§0.3).
//
//  * bulk_l_[f] locals: _b-body storage ONLY. The dynamic body of the SAME
//    function declares the boxed pt_l_* slot and binds the vararg loop by
//    pt_iter; native_read_text/native_slot_text choose between the two
//    renderings behind bulk_fn_ (exactly how twin_fn_ splits param
//    rendering today), so these lanes are invisible outside a _b body.
//    Slot admission mirrors analyze_native_ints' slot sweep but under
//    BULK facts (bulk_int_shape): params are int64 BY ABI, the sanctioned
//    vararg-For bind is a raw int64 int_bind.
//  * bulk_int_fns_: _b callees whose every `return` expression is
//    int-shaped under those facts — then the value boxed into the returned
//    pt_value is provably PT_INT (4b2 spills the fact into int_shape +
//    native_int_text at the CALL SITE, so `acc = acc + <bulk call>` keeps
//    acc's accumulation raw).
//
// JOINT because each shrinks the other: a callee stops being int-returning
// when a local lane evicts (writes become unspellable), and a local evicts
// when a callee it calls stops returning int64. Float pollution is covered
// by the same rule the int lane uses — a `acc = <float-shaped>` WRITE has
// !int_shape RHS, so the slot-twin sweep erases the candidate (never
// assumed int from its earlier writes); emit must re-derive under exactly
// the proven state, or decline (a "" there is an internal failure, never a
// silent widen).
void Emitter::analyze_bulk_lanes() {
  bulk_l_.clear();
  bulk_int_fns_.clear();
  bulk_int_decls_.clear();
  if (bulk_ok_.empty()) return;

  // Private collection: the bulk walk records the sanitized vararg-For bind
  // as an int_bind (the SHARED walk everywhere else poisons it — the
  // dynamic body binds from pt_iter and must stay declined from shared
  // lanes).
  LocalSlots lw;
  for (const FuncDecl* f : bulk_ok_) {
    GlobalSlots gw;
    walk_stmts(f->body, f, gw, lw, &bulk_ok_);
  }
  for (const FuncDecl* f : bulk_ok_) {
    std::set<std::string>& bl = bulk_l_[f];
    auto it = lw.find(f);
    if (it == lw.end()) continue;
    for (auto& sentry : it->second) {
      if (sentry.second.poisoned || sentry.second.writes.empty()) continue;
      size_t i = 0;
      for (; i < f->locals.size(); ++i)
        if (f->locals[i] == sentry.first) break;
      if (i == f->locals.size()) continue;  // not this function's local
      if (i < f->local_is_cell.size() && f->local_is_cell[i]) continue;
      bl.insert(sentry.first);
    }
  }

  // Return expressions per admitted callee (twin_scan in bulk mode: the
  // sanctioned vararg-For keeps sc.ok true — analyze_bulk_varargs already
  // enforced it; sc.returns = every `return <expr>` in the body).
  std::map<const FuncDecl*, TwinScan> scans;
  for (const FuncDecl* f : bulk_ok_) {
    TwinScan sc;
    twin_scan_stmts(f->body, f, sc, /*bulk=*/true);
    scans[f] = sc;
  }

  std::set<const FuncDecl*> intok = bulk_ok_;  // optimistic top
  bool changed = true;
  while (changed) {
    changed = false;
    // Callee-name set for int_shape's twin rule: the final _i twins (their
    // results are int64 guarantees) PLUS the currently-viable bulk-int
    // callees (shadowing rule: only decls find_function() still resolves).
    std::set<std::string> names = nativ_fns_;
    std::map<std::string, const FuncDecl*> tdecls;
    for (const FuncDecl* f : nativ_fn_decls_) tdecls.emplace(f->name, f);
    for (const FuncDecl* f : intok)
      if (find_function(f->name) == f) {
        names.insert(f->name);
        tdecls.emplace(f->name, f);
      }
    // Merged lane map: shared lanes (slots the DYNAMIC body already
    // spells natively — so a _b body may read them too) plus THIS round's
    // bulk-only lanes (visible only behind bulk_fn_).
    std::map<const FuncDecl*, std::set<std::string>> merged = nativ_l_;
    for (const FuncDecl* f : bulk_ok_)
      for (const std::string& l : bulk_l_[f]) merged[f].insert(l);
    // Slot sweep: every non-bind write of a bulk lane stays int-shaped
    // under the joint state (removal-only ⇒ monotone ⇒ terminates).
    for (const FuncDecl* f : bulk_ok_) {
      auto it = lw.find(f);
      if (it == lw.end()) continue;
      for (auto lit = bulk_l_[f].begin(); lit != bulk_l_[f].end();) {
        auto sit = it->second.find(*lit);
        if (sit == it->second.end()) { lit = bulk_l_[f].erase(lit); changed = true; continue; }
        bool ok = true;
        for (const SlotWrite& w : sit->second.writes)
          if (!w.int_bind &&
              !bulk_int_shape(*w.rhs, f, merged, names, tdecls)) {
            ok = false;
            break;
          }
        if (ok) ++lit;
        else { lit = bulk_l_[f].erase(lit); changed = true; }
      }
    }
    // Return sweep: an unprovable return retires the callee's int-result
    // fact (its CALL SITES fall back to the boxed path — per-function).
    for (auto it = intok.begin(); it != intok.end();) {
      const FuncDecl* f = *it;
      bool ok = true;
      auto sit = scans.find(f);
      if (sit != scans.end())
        for (const Expr* r : sit->second.returns)
          if (!bulk_int_shape(*r, f, merged, names, tdecls)) { ok = false; break; }
      if (ok) ++it;
      else { it = intok.erase(it); changed = true; }
    }
  }

  for (const FuncDecl* f : intok)
    if (find_function(f->name) == f) {
      bulk_int_fns_.insert(f->name);
      bulk_int_decls_.insert(f);
    }
}

// See the note above analyze_bulk_lanes: ONE predicate (int_shape) run with
// the merged lane set and slot_write=true — the twin-context spelling rule,
// correctly relaxed here because inside a _b body a param read is a raw
// int64 lane (ABI) while the DYNAMIC body of the same function renders the
// write through its ordinary boxed path (bulk_fn_ split). Drift between
// this and emission is the internal-failure habit the file keeps by design.
bool Emitter::bulk_int_shape(const Expr& e,
                             const FuncDecl* fn,
                             const std::map<const FuncDecl*, std::set<std::string>>& merged,
                             const std::set<std::string>& names,
                             const std::map<std::string, const FuncDecl*>& tdecls) {
  return int_shape(e, fn, nativ_g_, merged, &names, /*slot_write=*/true,
                   &tdecls);
}

void Emitter::analyze_guard_elim() {
  // Optimistic seed: every boxed slot needs no guard; disqualify on any read
  // we cannot prove bound. Native lanes never reach a pt_guard site, so they
  // are irrelevant here (harmless to include a native global in noguard_g_).
  noguard_g_.clear();
  for (const std::string& g : prog_.globals) noguard_g_.insert(g);
  noguard_l_.clear();
  for (const FuncDecl* f : all_functions()) {
    std::set<std::string>& cand = noguard_l_[f];
    for (const std::string& p : f->params) cand.insert(p);
    if (f->has_vararg) cand.insert(f->vararg);
    if (f->has_kwarg) cand.insert(f->kwarg);
    for (const std::string& l : f->locals) cand.insert(l);
    // Captures are intentionally NOT seeded: a capture read (env[ci]->v) can
    // observe a cell the enclosing frame has not assigned yet, so it keeps
    // its guard by never being a candidate.
  }

  // Global region: top-level statements and the main() block, in emission
  // order (they run sequentially in the generated main()). Globals are unset
  // at program start, so DA begins empty.
  {
    std::set<std::string> da_g, da_l;
    ge_def_pos_ = -1;
    for (const TopLevel& t : prog_.items) {
      if (t.kind == TopKind::Stmt)
        ge_walk_stmt(t.stmt, da_g, da_l, /*fn=*/nullptr);
      else if (t.kind == TopKind::Main && t.main)
        ge_walk_stmts(t.main->body, da_g, da_l, /*fn=*/nullptr);
    }
  }

  // Each function is its own region; params/locals/globals all start unset.
  for (const FuncDecl* f : all_functions()) {
    std::set<std::string> da_g, da_l;
    // Defaults evaluate per-call at materialization index i (fresh). Walk each
    // with EMPTY DA (an expression cannot assign, and the call-site global
    // state is unknown) and ge_def_pos_ = i so a read of a param whose slot is
    // still pt_unset() (index > i, or the vararg/kwarg slots) keeps its guard.
    for (size_t i = 0; i < f->defaults.size(); ++i) {
      if (!f->defaults[i]) continue;
      ge_def_pos_ = (int)i;
      ge_check_expr(*f->defaults[i], da_g, da_l, f);
    }
    ge_def_pos_ = -1;  // body context: every param slot is materialized
    ge_walk_stmts(f->body, da_g, da_l, f);
  }
  ge_def_pos_ = -1;
}

bool Emitter::native_is_local(const std::string& name) const {
  if (!cur_func_) return false;
  auto it = nativ_l_.find(cur_func_);
  return it != nativ_l_.end() && it->second.count(name) != 0;
}

std::string Emitter::native_read_text(const std::string& name, VarRes res,
                                      int line) const {
  std::string ln = std::to_string(line);
  // Post-2C twin lane: the twin's own params are raw noflag int64 lanes
  // (the call-site gate passed real ints; there is no pt_value slot).
  if (res == VarRes::Param && twin_fn_) return "pt_nl_" + name;
  if (res == VarRes::Global) {
    if (!nativ_g_.count(name)) return "";
    // Post-2C perf: a definitely-assigned slot never raises NameError on read,
    // so skip the used-flag load+branch and read the raw int64.
    if (noflag_g_.count(name)) return "pt_ng_" + name;
    return "pt_nread_g(pt_ng_" + name + ", pt_ng_ok_" + name + ", " + ln +
           ", \"" + c_escape(name) + "\")";
  }
  if (res == VarRes::Local && native_is_local(name)) {
    auto it = noflag_l_.find(cur_func_);
    if (it != noflag_l_.end() && it->second.count(name))
      return "pt_nl_" + name;  // definitely-assigned: raw read, no flag check
    return "pt_nread_l(pt_nl_" + name + ", pt_nl_ok_" + name + ", " + ln +
           ", \"" + c_escape(name) + "\")";
  }
  return "";
}

bool Emitter::native_slot_text(const std::string& name, VarRes res,
                               std::string& slot, std::string& flag) const {
  // Post-2C twin lane: a twin param writes its raw lane (native_noflag
  // says true, so no flag store is ever emitted and the flag name below
  // — which has no storage in the twin — is never referenced).
  if (res == VarRes::Param && twin_fn_) {
    slot = "pt_nl_" + name;
    flag = "pt_nl_ok_" + name;
    return true;
  }
  if (res == VarRes::Global) {
    if (!nativ_g_.count(name)) return false;
    slot = "pt_ng_" + name;
    flag = "pt_ng_ok_" + name;
    return true;
  }
  if (res == VarRes::Local && native_is_local(name)) {
    slot = "pt_nl_" + name;
    flag = "pt_nl_ok_" + name;
    return true;
  }
  return false;
}

bool Emitter::native_float_is_local(const std::string& name) const {
  if (!cur_func_) return false;
  auto it = nativfl_.find(cur_func_);
  return it != nativfl_.end() && it->second.count(name) != 0;
}

// Float-lane twin of native_read_text: the raw double read (guard
// replicated exactly through pt_nfrd_*, same NameError wording), "" for
// anything not in the float lane.
std::string Emitter::native_float_read_text(const std::string& name, VarRes res,
                                            int line) const {
  std::string ln = std::to_string(line);
  if (res == VarRes::Global) {
    if (!nativfg_.count(name)) return "";
    if (noflag_g_.count(name)) return "pt_nfg_" + name;
    return "pt_nfrd_g(pt_nfg_" + name + ", pt_nfg_ok_" + name + ", " + ln +
           ", \"" + c_escape(name) + "\")";
  }
  if (res == VarRes::Local && native_float_is_local(name)) {
    auto it = noflag_l_.find(cur_func_);
    if (it != noflag_l_.end() && it->second.count(name))
      return "pt_nfl_" + name;  // definitely-assigned: raw read, no flag check
    return "pt_nfrd_l(pt_nfl_" + name + ", pt_nfl_ok_" + name + ", " + ln +
           ", \"" + c_escape(name) + "\")";
  }
  return "";
}

bool Emitter::native_float_slot_text(const std::string& name, VarRes res,
                                     std::string& slot, std::string& flag) const {
  if (res == VarRes::Global) {
    if (!nativfg_.count(name)) return false;
    slot = "pt_nfg_" + name;
    flag = "pt_nfg_ok_" + name;
    return true;
  }
  if (res == VarRes::Local && native_float_is_local(name)) {
    slot = "pt_nfl_" + name;
    flag = "pt_nfl_ok_" + name;
    return true;
  }
  return false;
}

// double C expression for a float-shaped expression (mirrors float_shape;
// the int-lane text handles the mixed int64 operand). Literals use the same
// shortest-round-trip spelling as the boxed path (float_literal_raw), so
// the emitted C literal parses to the identical double bits. Operators are
// emitted as plain scalar IEEE C in SOURCE ORDER — no fast-math, no
// reassociation, no FMA contraction difference from the runtime's own
// `pt_dbl(a) op pt_dbl(b)` expressions (clang without -ffast-math keeps
// them strict).
std::string Emitter::native_float_text(const Expr& e) const {
  switch (e.kind) {
    case ExprKind::FloatLit:
      return float_literal_raw(e.fval);
    case ExprKind::Var:
      return native_float_read_text(e.name, e.res, e.loc.line);
    case ExprKind::Unary: {  // "-" (float_shape guarantees the operand)
      if (e.op != "-" || !e.lhs) return "";
      std::string a = native_float_text(*e.lhs);
      if (a.empty()) return "";
      return "(-(" + a + "))";
    }
    case ExprKind::Binary: {
      if (e.op != "+" && e.op != "-" && e.op != "*" && e.op != "/")
        return "";
      if (!e.lhs || !e.rhs) return "";
      std::string a = native_float_text(*e.lhs);
      std::string b = native_float_text(*e.rhs);
      std::string ln = std::to_string(e.loc.line);
      if (!a.empty() && !b.empty()) {
        // "*" goes through pt_nfmul: the dynamic path evaluates the product
        // and a consuming +/- as separate statements (boxed temps), so it
        // never FMA-contracts; a single lane expression would (clang -O3
        // -march=native → vfmadd, ONE rounding). pt_nfmul's empty-asm
        // barrier reproduces the two-rounding behavior bit-for-bit.
        if (e.op == "*") return "pt_nfmul(" + a + ", " + b + ")";
        // "/" must keep pt_div's y == 0.0 ZeroDivisionError: a raw C
        // division would silently produce inf (both/±0.0 raise dynamic).
        if (e.op == "/") return "pt_nfdiv(" + a + ", " + b + ", " + ln + ")";
        return "(" + a + " " + e.op + " " + b + ")";
      }
      // Mixed float/int (the runtime converts int64 -> double with a plain
      // C cast; pt_div_f's divisor check runs on the CONVERTED value —
      // pt_nfdiv reproduces it). native_int_text admits exactly the proven
      // int64 shapes (incl. int-twin call results).
      if (!a.empty()) {
        std::string ib = native_int_text(*e.rhs);
        if (ib.empty()) return "";
        std::string y = "(double)(" + ib + ")";
        if (e.op == "*") return "pt_nfmul(" + a + ", " + y + ")";
        return e.op == "/" ? "pt_nfdiv(" + a + ", " + y + ", " + ln + ")"
                           : "(" + a + " " + e.op + " " + y + ")";
      }
      if (!b.empty()) {
        std::string ia = native_int_text(*e.lhs);
        if (ia.empty()) return "";
        if (e.op == "*")
          return "pt_nfmul((double)(" + ia + "), " + b + ")";
        return e.op == "/" ? "pt_nfdiv((double)(" + ia + "), " + b + ", " + ln +
                                 ")"
                           : "((double)(" + ia + ") " + e.op + " " + b + ")";
      }
      return "";
    }
    case ExprKind::Call: {
      // float(<proven int64>): pt_builtin_float's PT_INT path is exactly
      // this C cast (the same conversion, the same rounding). Mirrors
      // float_shape's Call admission.
      if (!e.is_builtin || e.builtin_name != "float" || e.args.size() != 1u)
        return "";
      if (!e.arg_names.empty() && !e.arg_names[0].empty()) return "";
      std::string a = native_int_text(*e.args[0]);
      if (a.empty()) return "";
      return "((double)(" + a + "))";
    }
    default:
      return "";
  }
}

std::string Emitter::scalar_int_operand(const Expr& e, bool in_function) {
  if (int_shape(e, cur_func_, nativ_g_, nativ_l_, nullptr,
                /*slot_write=*/true, nullptr, &nativfg_, &nativfl_,
                &int_lists_, &int_list_da_reads_)) {
    std::string t = native_int_text(e);
    if (!t.empty()) return "pt_int(" + t + ")";
  }
  return emit_expr(e, in_function);
}

// int64 C expression for an int-shaped expression (mirrors int_shape).
std::string Emitter::native_int_text(const Expr& e) const {
  switch (e.kind) {
    case ExprKind::IntLit:
      if (e.ival == INT64_MIN) return "INT64_MIN";
      return std::to_string(e.ival) + "LL";
    case ExprKind::Var:
      return native_read_text(e.name, e.res, e.loc.line);
    case ExprKind::Index: {
      // Element read of a proven int-only list at a DA read site. The base
      // is a main-only global (local_g_), so its identifier is pt_g_<name>
      // and the DA proof means no used-before-assignment guard is needed.
      if (!e.lhs || e.lhs->kind != ExprKind::Var ||
          e.lhs->res != VarRes::Global)
        return "";
      {
        auto ep = ephem_active_.find(e.lhs->name);
        if (ep != ephem_active_.end()) {
          // Lazy slice target in the native int lane: read the source
          // element directly; the element is proven PT_INT by int_lists_.
          if (!int_lists_.count(e.lhs->name) || !e.rhs) return "";
          std::string idx = native_int_text(*e.rhs);
          if (idx.empty()) return "";
          const EphemSlice* es = ep->second;
          std::string ln = std::to_string(e.loc.line);
          return "pt_slice_iget_raw(" + es->items + ", " +
                 (es->flat ? "0" : es->stride) + ", " + es->span + ".first, " +
                 es->step_lit + ", (size_t)" + es->span + ".count, " + idx +
                 ", " + ln + ")";
        }
      }
      {
        auto ff = ephem_index_facts_.find(&e);
        if (ff != ephem_index_facts_.end() && e.rhs) {
          std::string idx = native_int_text(*e.rhs);
          if (!idx.empty()) {
            std::string ln2 = std::to_string(e.loc.line);
            return "pt_il_iget_raw(" + ff->second.items + ", " +
                   (ff->second.flat ? "0" : ff->second.stride) + ", " +
                   ff->second.len + ", " + idx + ", " + ln2 + ")";
          }
        }
      }
      if (!int_lists_.count(e.lhs->name) || !int_list_da_reads_.count(&e))
        return "";
      if (!e.rhs) return "";
      std::string idx = native_int_text(*e.rhs);
      if (idx.empty()) return "";
      std::string ln = std::to_string(e.loc.line);
      if (flat_lists_.count(e.lhs->name))
        return "pt_il_iget_flat(pt_g_" + e.lhs->name + ", " + idx + ", " + ln +
               ")";
      return "pt_il_iget(pt_g_" + e.lhs->name + ", " + idx + ", " + ln + ")";
    }
    case ExprKind::Unary: {  // "-" / "~" (int_shape guarantees the operand)
      if ((e.op != "-" && e.op != "~") || !e.lhs) return "";
      std::string a = native_int_text(*e.lhs);
      if (a.empty()) return "";
      if (e.op == "~") return "pt_nbnot(" + a + ")";
      return "pt_nwrap_neg(" + a + ")";
    }
    case ExprKind::Binary: {
      if (!e.lhs || !e.rhs) return "";
      std::string a = native_int_text(*e.lhs);
      if (a.empty()) return "";
      std::string b = native_int_text(*e.rhs);
      if (b.empty()) return "";
      std::string ln = std::to_string(e.loc.line);
      if (e.op == "+") return "pt_nwrap_add(" + a + ", " + b + ")";
      if (e.op == "-") return "pt_nwrap_sub(" + a + ", " + b + ")";
      if (e.op == "*") return "pt_nwrap_mul(" + a + ", " + b + ")";
      if (e.op == "/") return "pt_ndiv(" + a + ", " + b + ", " + ln + ")";
      if (e.op == "%") return "pt_nmod(" + a + ", " + b + ", " + ln + ")";
      // Bitwise ops are branch-free; shifts carry the pt_shl/pt_shr range
      // check (same ValueError, same line) — only the operand-type check
      // was proven away, never the count check.
      if (e.op == "&") return "pt_nwrap_band(" + a + ", " + b + ")";
      if (e.op == "|") return "pt_nwrap_bor(" + a + ", " + b + ")";
      if (e.op == "^") return "pt_nwrap_bxor(" + a + ", " + b + ")";
      if (e.op == "<<") return "pt_nwrap_shl(" + a + ", " + b + ", " + ln + ")";
      if (e.op == ">>") return "pt_nwrap_shr(" + a + ", " + b + ", " + ln + ")";
      return "";
    }
    case ExprKind::Call: {
      // Mirrors int_shape's Call case; the lane helpers are value-identical
      // to the pt_builtin_* int paths (wrapping neg/modpow, half-even never
      // applies: round(int) with no ndigits is the identity).
      if (!e.is_builtin) {
        // Native twin callee: the call runs in the callee's int64 lane and
        // returns int64 — no boxing anywhere on the path (the mutual
        // recursion fib -> fib stays pure C int64 end to end). C2: named
        // args and omitted int-literal defaults bind statically through the
        // SHARED binder (mirror of int_shape's Call rule — same binding,
        // same source/param-order guarantee), the omitted values are
        // stuffed into the call as raw int64 literals (caller-stuffing
        // instead of per-arity entry points).
        if (e.callee_expr || e.callee.empty() || !nativ_fns_.count(e.callee))
          return "";
        const FuncDecl* cf = find_function(e.callee);
        if (!cf || !nativ_fn_decls_.count(cf)) return "";
        std::vector<const Expr*> bound;
        if (!bind_call_args(e, *cf, bound)) return "";
        std::string joined;
        for (size_t i = 0; i < bound.size(); ++i) {
          if (!bound[i]) return "";
          std::string t = native_int_text(*bound[i]);
          if (t.empty()) return "";
          if (i) joined += ", ";
          joined += t;
        }
        return cf->c_name + "_i(" + joined + ")";  // c_name carries pt_fn_
      }
      if (e.args.empty()) return "";
      const std::string& n = e.builtin_name;
      std::string ln = std::to_string(e.loc.line);
      if (n == "int" && e.args.size() == 1u) {
        // int(<proven float lane>): pt_nf2i reproduces pt_builtin_int's
        // PT_FLOAT branch exactly (truncation, range/NaN ValueError at this
        // line). Mirrors int_shape's Call admission.
        if (!e.arg_names.empty() && !e.arg_names[0].empty()) return "";
        std::string a = native_float_text(*e.args[0]);
        if (a.empty()) return "";
        return "pt_nf2i(" + a + ", " + ln + ")";
      }
      if (n == "abs" && e.args.size() == 1u) {
        std::string a = native_int_text(*e.args[0]);
        if (a.empty()) return "";
        return "pt_nabs(" + a + ")";
      }
      if (n == "round" && e.args.size() == 1u)
        return native_int_text(*e.args[0]); /* int, no ndigits: identity */
      if (n == "pow" && e.args.size() == 2u) {
        std::string a = native_int_text(*e.args[0]);
        std::string b = native_int_text(*e.args[1]);
        if (a.empty() || b.empty()) return "";
        return "pt_npow(" + a + ", " + b + ", " + ln + ")";
      }
      if ((n == "min" || n == "max") && e.args.size() == 2u) {
        std::string a = native_int_text(*e.args[0]);
        std::string b = native_int_text(*e.args[1]);
        if (a.empty() || b.empty()) return "";
        return (n == "min" ? "pt_nmin(" : "pt_nmax(") + a + ", " + b + ")";
      }
      return "";
    }
    default:
      return "";
  }
}

// C condition for a mixed int64/double comparison, exact for every value
// (see pt_ncmp_i64_d: a plain double compare while |i| <= 2^53, the
// runtime's long-double rule beyond). `float_left` selects the operand
// order; the reversed forms swap the operator's direction.
static std::string mixed_cmp_text(const std::string& is, const std::string& ds,
                                  const std::string& op, bool float_left) {
  if (float_left) {
    // ds OP is  ==  is <reversed> ds
    if (op == "<") return "pt_ngt_i_d(" + is + ", " + ds + ")";
    if (op == "<=") return "pt_nge_i_d(" + is + ", " + ds + ")";
    if (op == ">") return "pt_nlt_i_d(" + is + ", " + ds + ")";
    if (op == ">=") return "pt_nle_i_d(" + is + ", " + ds + ")";
    if (op == "==") return "pt_neq_i_d(" + is + ", " + ds + ")";
    return "pt_nne_i_d(" + is + ", " + ds + ")";
  }
  if (op == "<") return "pt_nlt_i_d(" + is + ", " + ds + ")";
  if (op == "<=") return "pt_nle_i_d(" + is + ", " + ds + ")";
  if (op == ">") return "pt_ngt_i_d(" + is + ", " + ds + ")";
  if (op == ">=") return "pt_nge_i_d(" + is + ", " + ds + ")";
  if (op == "==") return "pt_neq_i_d(" + is + ", " + ds + ")";
  return "pt_nne_i_d(" + is + ", " + ds + ")";
}

// C bool expression for a condition that is provably int/bool-typed:
// int comparisons, truthiness of a native int slot, `not`, and and/or
// over native conditions (short-circuit order matches the dynamic
// lowering: left always, right only when the contract says so). Value-
// position and/or/ternary are NOT rewritten — only truth here.
bool Emitter::native_cond_text(const Expr& e, std::string& out) const {
  switch (e.kind) {
    case ExprKind::BoolLit:
      // Parenthesized like every other arm: callers splice nc directly into
      // `if <nc> {` / `while <nc> {` (the C grammar requires the parens).
      out = e.bval ? "(true)" : "(false)";
      return true;
    case ExprKind::Var: {
      std::string nr = native_read_text(e.name, e.res, e.loc.line);
      if (nr.empty()) {
        // Float lane: pt_truthy's PT_FLOAT rule (v.as.d != 0.0 — -0.0 and
        // only -0.0/0.0 falsy, NaN truthy) as a raw C condition.
        std::string fr = native_float_read_text(e.name, e.res, e.loc.line);
        if (fr.empty()) return false;
        out = "(" + fr + " != 0.0)";
        return true;
      }
      out = "(" + nr + " != 0)";  // pt_truthy's int rule (M1 §4)
      return true;
    }
    case ExprKind::Unary: {
      if (e.op != "not" || !e.lhs) return false;
      std::string inner;
      if (!native_cond_text(*e.lhs, inner)) return false;
      out = "(!" + inner + ")";
      return true;
    }
    case ExprKind::Binary: {
      if (!e.lhs || !e.rhs) return false;
      if (e.op != "<" && e.op != "<=" && e.op != ">" && e.op != ">=" &&
          e.op != "==" && e.op != "!=")
        return false;
      std::string a = native_int_text(*e.lhs);
      if (a.empty()) {
        // Float lane: float-vs-float comparisons compile to the same IEEE
        // compares the _f fast paths inline (pt_lt_f/.../pt_eq_f PT_FLOAT
        // branches — NaN unordered, all false; == / != IEEE equality). The
        // int lane's text is tried FIRST, so int comparisons are emitted
        // exactly as before. A mixed float/int pair (exactly one side is a
        // proven lane int) uses the exact pt_ncmp_i64_d wrappers, matching
        // pt_cmp3's long-double rule including NaN.
        std::string fa = native_float_text(*e.lhs);
        if (fa.empty()) return false;
        std::string ib = native_int_text(*e.rhs);
        if (!ib.empty()) {
          out = "(" + mixed_cmp_text(ib, fa, e.op, /*float_left=*/true) + ")";
          return true;
        }
        std::string fb = native_float_text(*e.rhs);
        if (fb.empty()) return false;
        out = "(" + fa + " " + e.op + " " + fb + ")";
        return true;
      }
      std::string b = native_int_text(*e.rhs);
      if (b.empty()) {
        std::string fb = native_float_text(*e.rhs);
        if (fb.empty()) return false;
        out = "(" + mixed_cmp_text(a, fb, e.op, /*float_left=*/false) + ")";
        return true;
      }
      out = "(" + a + " " + e.op + " " + b + ")";
      return true;
    }
    case ExprKind::Logical: {
      if (e.op != "and" && e.op != "or") return false;
      if (!e.lhs || !e.rhs) return false;
      std::string a, b;
      if (!native_cond_text(*e.lhs, a)) return false;
      if (!native_cond_text(*e.rhs, b)) return false;
      out = "(" + a + (e.op == "and" ? " && " : " || ") + b + ")";
      return true;
    }
    default:
      return false;
  }
}

// True when evaluating `e` cannot grow the GC heap. §7.3: the ONLY
// collection trigger is pt_gc_maybe() itself (the allocators have no
// pressure check), so eliding a safe point is sound exactly when nothing in
// the body can allocate — then the threshold check would be a no-op
// anyway, and no collect() result can change. Conservative by design:
// unknown node kinds, dynamic "+"/"*" (string concat/repeat), user calls
// and non-proven builtins all count as "may allocate".
// ---- text-chain fusion (perf: formatted-output builder) -----------------
// A + chain of provably-text parts (string literals and str() calls) builds
// ONE pt_sb buffer instead of a tree of heap concatenations; the write tail
// goes straight to stdio with one fwrite. See the declaration block above
// nativ_fvl_ for the full contract.

namespace {
// Conservative "no call anywhere" walk for the zero-alloc claim: a native
// int expression may embed a twin function call, and alloc_free_expr has
// always treated every Call as may-allocate — stay consistent.
bool expr_has_call(const Expr& e) {
  if (e.kind == ExprKind::Call) return true;
  if (e.lhs && expr_has_call(*e.lhs)) return true;
  if (e.rhs && expr_has_call(*e.rhs)) return true;
  if (e.third && expr_has_call(*e.third)) return true;
  if (e.fourth && expr_has_call(*e.fourth)) return true;
  for (const auto& a : e.args)
    if (a && expr_has_call(*a)) return true;
  for (const auto& el : e.elems)
    if (el && expr_has_call(*el)) return true;
  return false;
}
}  // namespace

bool Emitter::text_part_fusable(const Expr& e) const {
  if (e.kind == ExprKind::StrLit) return true;
  if (e.kind == ExprKind::Binary && e.op == "+") return true;  // flattened
  // str() always yields text, so a chain of literals + str() calls can
  // never raise the dynamic `+`'s operand-type error — fusing it cannot
  // change which error fires or where. A BARE operand (variable, call
  // result...) could carry a non-text value and stays unfused: the
  // dynamic path then raises exactly as before.
  if (e.kind == ExprKind::Call && e.is_builtin && e.builtin_name == "str" &&
      e.args.size() == 1u && (e.arg_names.empty() || e.arg_names[0].empty()))
    return true;
  return false;
}

bool Emitter::text_chain_parts(const Expr& e,
                               std::vector<const Expr*>& out) const {
  if (e.kind == ExprKind::Binary && e.op == "+") {
    if (!e.lhs || !e.rhs) return false;
    if (!text_chain_parts(*e.lhs, out)) return false;
    return text_chain_parts(*e.rhs, out);
  }
  if (!text_part_fusable(e)) return false;
  out.push_back(&e);
  return true;
}

bool Emitter::text_chain_zeroalloc(const Expr& e) const {
  std::vector<const Expr*> parts;
  if (!text_chain_parts(e, parts) || parts.empty()) return false;
  for (const Expr* p : parts) {
    if (p->kind == ExprKind::StrLit) continue;
    // A str() part is heap-free when its argument formats straight into
    // the builder through the int lane — and only then (no call anywhere
    // inside: see expr_has_call).
    if (p->kind == ExprKind::Call && p->args.size() == 1u &&
        !expr_has_call(*p->args[0]) &&
        !native_int_text(*p->args[0]).empty())
      continue;
    return false;
  }
  return true;
}

std::string Emitter::emit_text_chain(const Expr& e, bool in_function,
                                     const std::string& obj, int src_line) {
  std::vector<const Expr*> parts;
  if (!text_chain_parts(e, parts) || parts.empty())
    fail("internal: text-chain fusion on a non-fusable chain (line " +
         std::to_string(src_line) + ")");
  struct Mat {       // per-part materialization, source order
    char kind;       // 'L' literal text value, 'I' int64, 'V' pt_value
    std::string c;   // the C expression / temp name
  };
  std::vector<Mat> mats;
  mats.reserve(parts.size());
  const std::string ln = std::to_string(src_line);
  for (const Expr* p : parts) {
    if (p->kind == ExprKind::StrLit) {
      // A static PT_STR value: no emission, no evaluation.
      mats.push_back({'L', emit_expr(*p, in_function)});
    } else if (p->kind == ExprKind::Call) {
      // Provably a str() call (text_part_fusable).
      std::string nt = native_int_text(*p->args[0]);
      if (!nt.empty()) {
        // Proven int: format straight into the builder (pt_sb_puti shares
        // the display path's digit code — str(int)'s exact spelling) and
        // never build the intermediate heap text. One temp: the argument
        // evaluates exactly once, in source order, raises exactly where
        // the unfused emission would raise (lane reads raise the same
        // guard errors as the dynamic slot reads).
        std::string iv = fresh_temp();
        line(ind() + "int64_t " + iv + " = " + nt + ";");
        mats.push_back({'I', iv});
      } else {
        std::string a = emit_expr(*p->args[0], in_function);
        std::string t = decl_temp();
        line(ind() + t + " = pt_builtin_str(" + a + ", " + ln + ");");
        mats.push_back({'V', t});
      }
    } else {
      fail("internal: unexpected text-chain part (line " + ln + ")");
    }
  }
  std::string t = decl_temp();
  std::string sb = fresh_temp();
  line(ind() + "{ pt_sb " + sb + ";");
  indent_ += 2;
  line(ind() + "pt_sb_init(&" + sb + ");");
  for (const Mat& m : mats) {
    if (m.kind == 'I')
      line(ind() + "pt_sb_puti(&" + sb + ", " + m.c + ");");
    else  // 'L' (PT_STR literal value) and 'V' (materialized str() result)
      line(ind() + "pt_sb_putv(&" + sb + ", " + m.c + ");");
  }
  if (obj.empty())
    line(ind() + t + " = pt_sb_finish(&" + sb + ", " + ln + ");");
  else
    line(ind() + t + " = pt_file_write_buf(" + obj + ", " + sb + ".p, " + sb +
         ".n, " + ln + ");");
  line(ind() + "pt_sb_free(&" + sb + ");");
  indent_ -= 2;
  line(ind() + "}");
  return t;
}

bool Emitter::alloc_free_expr(const Expr& e) const {
  switch (e.kind) {
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
    case ExprKind::StrLit:    // PT_STR literal: static .rodata, never heap
    case ExprKind::BytesLit:  // PT_BYTES literal: same lifetime rules
    case ExprKind::BoolLit:
    case ExprKind::NoneLit:
    case ExprKind::Var:  // slot / guard / native reads only
      return true;
    case ExprKind::VarQual:  // cross-scope (docs/reference/cross-scope-access.md §5): a
                             // pt_guard over a pinned static / module global
                             // — same class as Var: slot + guard, never heap
      return true;
    case ExprKind::Unary:  // "-", "~", "not" — pt_neg / int ops / pt_truthy
      return e.rhs != nullptr && alloc_free_expr(*e.rhs);
    case ExprKind::Binary: {
      if (!e.lhs || !e.rhs) return false;
      // "+"/"*" may concat/repeat strings and "**" was never audited — the
      // numeric/comparison ops below are heap-free on both paths
      // (pt_wrap_*, pt_sub, pt_div, pt_mod, pt_cmp3), and so are the
      // bitwise/shift ops (M2-2A §3.2): pt_band/pt_bor/pt_bxor/pt_shl/
      // pt_shr box their int result into one immediate struct and never
      // touch the GC heap. Their error paths (TypeError on a non-int
      // operand, ValueError on an out-of-range shift count) raise and
      // unwind out of the enclosing loop — no back-edge is crossed, so no
      // safe point can be missed — exactly like "-" / "/" / "%" already
      // rely on pt_div/pt_mod raising ZeroDivisionError.
      const std::string& op = e.op;
      if (op != "==" && op != "!=" && op != "<" && op != "<=" && op != ">" &&
          op != ">=" && op != "-" && op != "/" && op != "%" && op != "&" &&
          op != "|" && op != "^" && op != "<<" && op != ">>")
        return false;
      return alloc_free_expr(*e.lhs) && alloc_free_expr(*e.rhs);
    }
    case ExprKind::Logical:  // and/or: lazy, so each operand checked alone
      return e.lhs && e.rhs && alloc_free_expr(*e.lhs) &&
             alloc_free_expr(*e.rhs);
    case ExprKind::Ternary:  // only one branch runs; checking all is stricter
      return e.lhs && e.rhs && e.third && alloc_free_expr(*e.lhs) &&
             alloc_free_expr(*e.rhs) && alloc_free_expr(*e.third);
    case ExprKind::Call: {
      // Heap-free reserved builtins only; user functions, dynamic calls and
      // every other builtin (str/input/print/collect/...) may allocate or
      // must run. codegen dispatches `is_builtin` before `callee_expr`
      // (emit_expr Call case), so this is exactly the builtin_call_text set.
      if (!e.is_builtin) return false;
      const std::string& n = e.builtin_name;
      if (n == "clock" || n == "time") return true;
      // SPEC §5.11.5: the scalar/inspection Python-parity builtins never
      // reach a GC allocation — abs/pow/round run on immediates, the
      // predicates compare/hash existing values, hasattr/delattr touch
      // existing storage. Anything that can ITERATE stays out (a file
      // iterator allocates one line object per step), as do getattr (a
      // method read builds a bound method), the collection builders,
      // chr/bin/oct/hex/repr/ascii/bytes/format/slice (fresh values) and
      // globals/locals/vars/dir/iter/open (their results).
      if (n == "abs" || n == "callable" || n == "delattr" || n == "hasattr" ||
          n == "hash" || n == "id" || n == "isinstance" || n == "ord" ||
          n == "pow" || n == "round")
        return true;
      // C1: `len` proven from runtime/pentest_rt.c — pt_builtin_len is
      // `pt_int((int64_t)pt_len(v, line))` and pt_len only READS (tag
      // switch over length fields, or pt_utf8_count over the bytes and
      // pt_no_missing/TypeError on the error path): no GC-heap block, no
      // collection ever reached from it — then the back-edge check would be
      // a no-op for containing loops. NOT admitted: `sum`/`max 1-arg`/
      // `min 1-arg` iterate, and a TEXT iterable allocates one element copy
      // per step (pt_iter_next PT_ITER_STRING -> pt_text_copy), a FILE
      // iterator one line object; `divmod` returns a pair; `int`/`float`
      // parsing was not audited here.
      if (n == "len") return true;
      if (n == "max" || n == "min") {
        if (e.args.size() < 2u) return false;  // the 1-arg form iterates
        // No key/default: the comparison runs no user code and allocates
        // nothing (the runtime's best-key holder stays unallocated).
        for (const std::string& an : e.arg_names)
          if (an == "key" || an == "default") return false;
        return true;
      }
      return false;
    }
    case ExprKind::MethodCall: {
      // The fused write tail (f.write(<text chain>)) with all-literal /
      // int-lane parts allocates nothing — no heap text, one fwrite.
      return e.method == "write" && e.elems.size() == 1u && e.elems[0] &&
             (e.elem_names.empty() || e.elem_names[0].empty()) &&
             text_chain_zeroalloc(*e.elems[0]);
    }
    default:
      return false;
  }
}

// True when every statement in `body` provably cannot allocate — then the
// loop back-edge GC safe point can be skipped (§7.3: collection timing is
// unobservable; see alloc_free_expr above). Plain and native-integer
// assignments qualify (storing never allocates), `if` chains recurse when
// every condition is heap-free, C1 lets heap-free `while`/`range()` loops and
// proven-int `switch` dispatches recurse the same way (each inner loop's own
// back edge is gated by its own emitter), and break/continue qualify outside
// any try (emit_try_exits would run finallys that may allocate — try blocks
// themselves are rejected outright).
bool Emitter::body_alloc_free(const std::vector<Stmt>& body) const {
  for (const Stmt& s : body) {
    switch (s.kind) {
      case StmtKind::Pass:
        continue;
      case StmtKind::ExprStmt:
        if (!s.expr || !alloc_free_expr(*s.expr)) return false;  // may allocate
        continue;
      case StmtKind::Assign:
        if (s.target.kind == AssignTarget::Kind::VarQual) {
          // Cross-scope plain store (docs/reference/cross-scope-access.md §5): a direct
          // write into the pinned static / module global — the RHS is the
          // only evaluation, exactly like the Name case below. Compound is
          // left conservative: a dynamic "x += y"/"*=" can concat/repeat
          // text (pt_add_f falls back to pt_add), and qualified slots are
          // never native-proven, so the Name path's slot proof is void.
          if (!s.op.empty() || !s.expr) return false;
          if (!alloc_free_expr(*s.expr) && native_int_text(*s.expr).empty())
            return false;
          continue;
        }
        if (s.target.kind != AssignTarget::Kind::Name) return false;
        if (s.res == VarRes::Unresolved || !s.expr) return false;
        if (s.op.empty()) {
          // Plain store: the RHS is the only evaluation. native_int_text
          // also covers "+"/"*" over proven-int operands (pt_add/pt_mul of
          // two ints cannot take the concat path), and native_float_text
          // covers float-lane arithmetic — emitted through the lane it is
          // raw C, and emitted dynamically the numeric fast path boxes an
          // immediate pt_float (never the string-concat branch), so both
          // witnesses are heap-free. Error paths (pt_ndiv / pt_nfdiv
          // ZeroDivisionError) raise and unwind out of the enclosing loop —
          // no back-edge is crossed, so no safe point can be missed.
          if (!alloc_free_expr(*s.expr) && native_int_text(*s.expr).empty() &&
              native_float_text(*s.expr).empty())
            return false;
        } else {
          // Compound: current value meets the RHS (a dynamic "+=" can
          // concat) — restrict to proven native slots, whose reads/writes
          // stay in one lane. (A float slot's op set is +=/-=/*=//= only —
          // the fixpoint declines anything else.)
          std::string slot, flag;
          if (native_slot_text(s.name, s.res, slot, flag)) {
            if (native_int_text(*s.expr).empty()) return false;
          } else if (native_float_slot_text(s.name, s.res, slot, flag)) {
            if (native_float_text(*s.expr).empty()) return false;
          } else {
            return false;
          }
        }
        continue;
      case StmtKind::If: {
        if (s.conds.size() != s.bodies.size()) return false;
        for (size_t i = 0; i < s.conds.size(); ++i) {
          if (!s.conds[i] || !alloc_free_expr(*s.conds[i])) return false;
          if (!body_alloc_free(s.bodies[i])) return false;
        }
        if (!body_alloc_free(s.else_body)) return false;
        continue;
      }
      case StmtKind::Break:
      case StmtKind::Continue:
        if (!try_stack_.empty()) return false;  // enclosing try → finallys
        continue;
      case StmtKind::While: {
        // C1: a nested loop contributes only if ITS condition is heap-free
        // and ITS body is too — then neither its back edge nor any deeper
        // one can ever collect (the collector starts only at pt_gc_maybe,
        // §7.3), so skipping the outer check stays sound. The While emitter
        // applies the same proof to the inner body itself.
        if (!s.expr || !alloc_free_expr(*s.expr)) return false;
        if (!body_alloc_free(s.body)) return false;
        continue;
      }
      case StmtKind::For: {
        // C1: the open-coded range() form only — pt_range3 validates and
        // builds a stack value, the bind is a raw store, the latch is plain
        // int64 arithmetic (§7.3 comment at the For emitter), so every part
        // is heap-free; the range ARGUMENTS are ordinary expressions and must
        // each prove heap-free. The generic iterator protocol stays
        // conservative: pt_iter_next's PT_ITER_STRING arm copies each code
        // point (pt_text_copy) and PT_ITER_FILE reads one line object per
        // step — both allocate.
        if (!s.expr || s.expr->kind != ExprKind::Call || !s.expr->is_builtin_range)
          return false;
        for (const ExprPtr& a : s.expr->args)
          if (!a || !alloc_free_expr(*a)) return false;
        if (!body_alloc_free(s.body)) return false;
        continue;
      }
      case StmtKind::Switch: {
        // C1: dispatch is proven inert when the scrutinee is a PROVEN int64
        // with no call inside it (native_int_text alone admits a twin call —
        // expr_has_call keeps the "no call anywhere" rule of the comment
        // above alloc_free_expr) and every label is an INT constant: the
        // ladder's pt_eq_f then compares two immediates and the native
        // lowering compares two int64s — pt_int() boxes are immediates and
        // nothing reaches the GC heap. A boxed scrutinee or a text/float/bool
        // label can reach heap-touching comparison operators: the gate stays
        // conservative there (arm BODIES are checked either way).
        if (!s.expr || expr_has_call(*s.expr)) return false;
        if (native_int_text(*s.expr).empty()) return false;
        for (const CaseArm& a : s.cases)
          if (!a.is_wildcard && a.label_kind != ExprKind::IntLit) return false;
        for (const CaseArm& a : s.cases)
          if (!body_alloc_free(a.body)) return false;
        continue;
      }
      case StmtKind::Try: {
        // C2: an ELIDED try emits exactly its body (plus its finally copy
        // on the normal edge — exit-edge copies keep their own proofs via
        // the Break/Continue/Return rules above), registering nothing:
        // heap-free therefore means body-free AND finally-free, exactly as
        // if the statements sat inline in the enclosing sequence. A
        // registered try keeps the old refusal (its pads and exit copies
        // carry no proof of their own).
        if (!elided_trys_.count(&s)) return false;
        if (!body_alloc_free(s.body)) return false;
        if (s.has_finally && !body_alloc_free(s.finally_body)) return false;
        continue;
      }
      default:
        return false;  // loops / return / raise / decls …
    }
  }
  return true;
}

std::string Emitter::emit_expr(const Expr& e, bool in_function) {
  switch (e.kind) {
    case ExprKind::IntLit:
      return int_literal(e.ival);
    case ExprKind::FloatLit:
      return float_literal(e.fval);
    case ExprKind::StrLit:
      return "PT_STR(\"" + c_escape(e.sval) + "\")";
    case ExprKind::BytesLit:  // M2-2B §1/§5
      return "PT_BYTES(\"" + c_escape(e.sval) + "\")";
    case ExprKind::BoolLit:
      return e.bval ? "pt_bool(true)" : "pt_bool(false)";
    case ExprKind::NoneLit:
      return "pt_none()";

    // ---- M2-2B collection literals (§4) --------------------------------
    // Elements are A-normalized left-to-right (keys before values, pair by
    // pair, matching the written order), then the constructor receives one
    // compound-literal array of plain temp reads.
    case ExprKind::ListLit:
    case ExprKind::SetLit:
    case ExprKind::TupleLit: {
      std::vector<std::string> items;
      items.reserve(e.elems.size());
      for (const auto& el : e.elems) {
        if (!el) fail("collection literal element missing (line " +
                      std::to_string(e.loc.line) + ")");
        items.push_back(emit_expr(*el, in_function));
      }
      std::string t = decl_temp();
      std::string ctor;
      if (items.empty()) {
        ctor = e.kind == ExprKind::ListLit ? "pt_list_new()"
             : e.kind == ExprKind::SetLit  ? "pt_set_new()"
                                           : "pt_tuple_of(NULL, 0u)";
      } else if (e.kind == ExprKind::ListLit) {
        ctor = "pt_list_of(" + value_array_text(items) + ", " +
               std::to_string(items.size()) + "u)";
      } else if (e.kind == ExprKind::SetLit) {
        ctor = "pt_set_of(" + value_array_text(items) + ", " +
               std::to_string(items.size()) + "u, " + std::to_string(e.loc.line) + ")";
      } else {
        ctor = "pt_tuple_of(" + value_array_text(items) + ", " +
               std::to_string(items.size()) + "u)";
      }
      line(ind() + t + " = " + ctor + ";");
      return t;
    }

    case ExprKind::MapLit: {
      if (e.keys.size() != e.elems.size())
        fail("malformed map literal (line " + std::to_string(e.loc.line) + ")");
      std::vector<std::string> ks, vs;
      ks.reserve(e.keys.size());
      vs.reserve(e.elems.size());
      for (size_t i = 0; i < e.keys.size(); ++i) {
        if (!e.keys[i] || !e.elems[i])
          fail("map literal entry missing (line " + std::to_string(e.loc.line) + ")");
        ks.push_back(emit_expr(*e.keys[i], in_function));    // key first …
        vs.push_back(emit_expr(*e.elems[i], in_function));   // … then value
      }
      std::string t = decl_temp();
      std::string ctor;
      if (ks.empty()) {
        ctor = "pt_map_new()";  // `{}` — the empty map (M2-2B §4)
      } else {
        ctor = "pt_map_of(" + value_array_text(ks) + ", " + value_array_text(vs) +
               ", " + std::to_string(ks.size()) + "u, " +
               std::to_string(e.loc.line) + ")";
      }
      line(ind() + t + " = " + ctor + ";");
      return t;
    }

    case ExprKind::Index: {
      if (!e.lhs || !e.rhs)
        fail("index expression without container or index (line " +
             std::to_string(e.loc.line) + ")");
      if (e.lhs->kind == ExprKind::Var && e.lhs->res == VarRes::Global) {
        auto ep = ephem_active_.find(e.lhs->name);
        if (ep != ephem_active_.end()) {
          // Lazy slice target: index the source through the normalized
          // bounds. pt_slice_get keeps pt_slice[idx]'s exact type/bounds
          // errors and returns the same element value.
          const EphemSlice* es = ep->second;
          std::string idx = scalar_int_operand(*e.rhs, in_function);
          std::string t = decl_temp();
          line(ind() + t + " = pt_slice_get(" + es->psrc + ", " + es->span +
               ".first, " + es->step_lit + ", (size_t)" + es->span + ".count, " +
               idx + ", " + std::to_string(e.loc.line) + ");");
          return t;
        }
      }
      std::string obj = emit_expr(*e.lhs, in_function);
      std::string idx = scalar_int_operand(*e.rhs, in_function);
      std::string t = decl_temp();
      line(ind() + t + " = pt_index_get_f(" + obj + ", " + idx + ", " +
           std::to_string(e.loc.line) + ");");
      return t;
    }

    case ExprKind::Slice: {
      if (!e.lhs) fail("slice without container (line " + std::to_string(e.loc.line) + ")");
      std::string obj = emit_expr(*e.lhs, in_function);
      // §4: an open bound arrives as PT_UNSET (pt_slice normalizes).
      std::string start = (e.has_start && e.rhs) ? emit_expr(*e.rhs, in_function)
                                                 : "pt_unset()";
      std::string stop = (e.has_stop && e.third) ? emit_expr(*e.third, in_function)
                                                 : "pt_unset()";
      std::string step = (e.has_step && e.fourth) ? emit_expr(*e.fourth, in_function)
                                                  : "pt_unset()";
      std::string t = decl_temp();
      line(ind() + t + " = pt_slice(" + obj + ", " + start + ", " +
           stop + ", " + step + ", " + std::to_string(e.loc.line) + ");");
      return t;
    }

    case ExprKind::MethodCall: {
      if (!e.lhs)
        fail("method call without object (line " + std::to_string(e.loc.line) + ")");
      // Fused-write shape first (pure check, no emission): a fuse-shape site
      // keeps the baseline fused allocation path — it is never inlined.
      bool fuse = e.method == "write" && e.elems.size() == 1u && e.elems[0] &&
                  (e.elem_names.empty() || e.elem_names[0].empty());
      if (fuse) {
        std::vector<const Expr*> chk;
        fuse = text_chain_parts(*e.elems[0], chk);
      }
      if (!fuse) {
        auto itu = il_sites_.find(&e);
        if (itu != il_sites_.end()) {
          // Cyclic plans (bodies reaching one another through proven
          // instances) must not recurse forever: never inline a method that
          // is already being inlined — fall through to the dynamic path
          // (try_inline's A-normal force stays valid for it).
          bool reenter = false;
          for (const FuncDecl* f : il_stack_)
            if (f == itu->second.method) {
              reenter = true;
              break;
            }
          if (!reenter) return emit_inline_method(e, itu->second, in_function);
        }
      }
      std::string obj = emit_expr(*e.lhs, in_function);
      // Fused write: f.write(<text chain>) builds the record straight into
      // ONE buffer (no heap text) and writes it with a single fwrite — the
      // file-side equivalent of the print fast path. The receiver emits
      // first, exactly like the unfused argument order below.
      if (fuse) {
        return emit_text_chain(*e.elems[0], in_function, obj, e.loc.line);
      }
      std::vector<std::string> args;
      args.reserve(e.elems.size());
      for (const auto& el : e.elems) {
        if (!el) fail("method argument missing (line " + std::to_string(e.loc.line) + ")");
        args.push_back(emit_expr(*el, in_function));  // emitted in source order
      }
      std::string ln = std::to_string(e.loc.line);
      // M2-2C §3.2: named arguments pass through to the callee's binder —
      // bound methods accept them, builtin container methods reject them at
      // runtime with the §13 wording. Split mirrors emit_dynamic_call.
      size_t npos = 0;
      for (size_t i = 0; i < args.size(); ++i) {
        const std::string n = i < e.elem_names.size() ? e.elem_names[i] : "";
        if (n.empty()) {
          if (npos != i)
            fail("internal: positional argument after named argument (line " + ln + ")");
          ++npos;
        }
      }
      size_t nnamed = args.size() - npos;
      if (nnamed > 0) {
        std::string t = decl_temp();
        std::string joined;
        for (size_t i = 0; i < args.size(); ++i) {
          if (i) joined += ", ";
          joined += args[i];
        }
        std::string pv = fresh_temp();
        line(ind() + "{ pt_value " + pv + "[] = { " + joined + " };");
        indent_ += 2;
        std::string an = fresh_temp();
        std::string names;
        for (size_t i = npos; i < args.size(); ++i) {
          if (i > npos) names += ", ";
          names += "\"" + c_escape(e.elem_names[i]) + "\"";
        }
        line(ind() + "const char* const " + an + "[] = { " + names + " };");
        line(ind() + t + " = pt_call_method(" + obj + ", \"" + c_escape(e.method) +
             "\", " + pv + ", " + std::to_string(npos) + "u, " + an + ", " +
             std::to_string(nnamed) + "u, " + ln + ");");
        indent_ -= 2;
        line(ind() + "}");
        return t;
      }
      // Positional-only (the 2B §13 form — byte-identical to before).
      std::string t = decl_temp();
      std::string call = "pt_call_method(" + obj + ", \"" + c_escape(e.method) + "\", ";
      call += args.empty() ? "NULL, 0u"
                           : value_array_text(args) + ", " + std::to_string(args.size()) + "u";
      call += ", NULL, 0u, " + ln + ")";
      line(ind() + t + " = " + call + ";");
      return t;
    }

    case ExprKind::Attr: {  // M2-2C §3.2: bare attribute read
      if (!e.lhs)
        fail("attribute read without object (line " + std::to_string(e.loc.line) + ")");
      std::string obj = emit_expr(*e.lhs, in_function);
      std::string t = decl_temp();
      line(ind() + t + " = pt_getattr_lit(" + obj + ", \"" + c_escape(e.attr) + "\", " +
           std::to_string(e.loc.line) + ");");
      return t;
    }

    case ExprKind::Join: {  // M2-2B §7 comma-join
      std::vector<std::string> items;
      items.reserve(e.elems.size());
      for (const auto& el : e.elems) {
        if (!el) fail("join element missing (line " + std::to_string(e.loc.line) + ")");
        items.push_back(emit_expr(*el, in_function));
      }
      std::string t = decl_temp();
      line(ind() + t + " = pt_join(" + value_array_text(items) + ", " +
           std::to_string(items.size()) + "u, " + std::to_string(e.loc.line) + ");");
      return t;
    }

    case ExprKind::Var: {
      // Post-2C native slot: inline int64 read (guard replicated exactly),
      // materialized to a pt_value for the dynamic world. Corner: when TWO
      // unset native reads share one expression, which name is reported
      // follows C operand order — same NameError, same message shape.
      std::string nr = native_read_text(e.name, e.res, e.loc.line);
      if (!nr.empty()) return "pt_int(" + nr + ")";
      // Native float slot: same discipline — the double boxes to PT_FLOAT
      // here (pt_float is a bit-copy; no rounding, no signaling), and the
      // dynamic pt_value slot is never the source of truth.
      std::string fr = native_float_read_text(e.name, e.res, e.loc.line);
      if (!fr.empty()) return "pt_float(" + fr + ")";
      std::string storage = storage_for(e.name, e.res, in_function, e.loc.line);
      std::string t = decl_temp();
      line(ind() + t + " = " +
           guard_text(storage, e.name, e.res, e.loc.line) + ";");
      return t;
    }

    case ExprKind::VarQual: {
      // docs/reference/cross-scope-access.md §5: read the pinned static (or module
      // global) directly — same guard, same message as an ordinary read of
      // that storage, no frame lookup of any kind.
      std::string storage = qual_value_text(e);
      std::string t = decl_temp();
      line(ind() + t + " = pt_guard(" + storage + ", \"" + c_escape(e.name) +
           "\", " + scope_name(e.res) + ", " + std::to_string(e.loc.line) + ");");
      return t;
    }

    case ExprKind::Unary: {
      if (!e.lhs) fail("unary operator without operand (line " +
                       std::to_string(e.loc.line) + ")");
      std::string a = emit_expr(*e.lhs, in_function);
      std::string ln = std::to_string(e.loc.line);
      std::string t = decl_temp();
      if (e.op == "-") {
        line(ind() + t + " = pt_neg_f(" + a + ", " + ln + ");");
      } else if (e.op == "~") {  // M2-2A §3.2
        line(ind() + t + " = pt_bnot(" + a + ", " + ln + ");");
      } else if (e.op == "not") {  // M2-2A §4
        line(ind() + t + " = pt_bool(!pt_truthy(" + a + ", " + ln + "));");
      } else {
        fail("unknown unary operator '" + e.op + "'");
      }
      return t;
    }

    case ExprKind::Binary: {
      if (!e.lhs || !e.rhs)
        fail("binary operator without operand (line " + std::to_string(e.loc.line) + ")");
      if (e.op == "+") {
        // Text-chain fusion: a + tree of provably text parts builds one
        // buffer / one heap text (emit_text_chain).
        std::vector<const Expr*> chk;
        if (text_chain_parts(e, chk) && chk.size() >= 2u)
          return emit_text_chain(e, in_function, "", e.loc.line);
      }
      std::string l = emit_expr(*e.lhs, in_function);
      std::string r = emit_expr(*e.rhs, in_function);
      std::string t = decl_temp();
      line(ind() + t + " = " + binary_text(e.op, l, r, e.loc.line) + ";");
      return t;
    }

    case ExprKind::Logical: {
      // M2-2A §4: both operands' outer effects happen left side first (the
      // left is always evaluated), the right only when the contract says so.
      if (!e.lhs || !e.rhs)
        fail("logical operator without operand (line " + std::to_string(e.loc.line) + ")");
      std::string l = emit_expr(*e.lhs, in_function);   // always evaluated
      std::string r = inline_child(*e.rhs, in_function); // lazy: stays inside ?:
      std::string ln = std::to_string(e.loc.line);
      std::string t = decl_temp();
      if (e.op == "and") {
        line(ind() + t + " = (pt_truthy(" + l + ", " + ln +
             ") ? pt_bool(pt_truthy(" + r + ", " + ln + ")) : pt_bool(false));");
      } else if (e.op == "or") {
        line(ind() + t + " = (pt_truthy(" + l + ", " + ln +
             ") ? pt_bool(true) : pt_bool(pt_truthy(" + r + ", " + ln + ")));");
      } else {
        fail("unknown logical operator '" + e.op + "'");
      }
      return t;
    }

    case ExprKind::Ternary: {
      // lhs = condition, rhs = then-value, third = else-value (ast.h).
      if (!e.lhs || !e.rhs || !e.third)
        fail("ternary without operands (line " + std::to_string(e.loc.line) + ")");
      std::string c = emit_expr(*e.lhs, in_function);   // condition: always evaluated
      std::string th = inline_child(*e.rhs, in_function);   // lazy arm
      std::string el = inline_child(*e.third, in_function); // lazy arm
      std::string ln = std::to_string(e.loc.line);
      std::string t = decl_temp();
      line(ind() + t + " = (pt_truthy(" + c + ", " + ln + ") ? " + th +
           " : " + el + ");");
      return t;
    }

    case ExprKind::Call: {
      // ---- C3 (task-5 A): bulk-entry gates ------------------------------
      // (1) CALLEE side, any context: a static call to an admissible bulk
      // callee packs raw int64s and dispatches straight to the _b entry
      // (no bound vector, no PT_MISSING, no pt_list_of); ANY decline below
      // falls through to today's exact boxed path (byte-parity by
      // construction).
      if (!e.is_builtin && !e.is_builtin_print && !e.is_builtin_range &&
          !e.callee_expr && !e.callee.empty()) {
        const FuncDecl* bf = find_function(e.callee);
        if (bf && bulk_ok_.count(bf)) {
          std::vector<const Expr*> bx, bt;
          if (bind_bulk_call_args(e, *bf, bx, bt)) {
            std::string btv = emit_bulk_call(bx, bt, *bf);
            if (!btv.empty()) return btv;
          }
        }
      }
      // (2) ARGUMENT side, ONLY inside a _b body: the sanctioned len(more)
      // spells its count structurally — the boxed pt_l_<vararg> slot
      // deliberately does not exist there, so any other vararg reference
      // would fail the C compile loudly (admissibility drift = compile
      // error, never a miscompile; see the is_bulk_* block above
      // twin_scan_stmts).
      if (bulk_fn_ && cur_func_ == bulk_fn_ && e.is_builtin &&
          is_bulk_len_call(e, cur_func_->vararg)) {
        std::string t = decl_temp();
        line(ind() + t + " = pt_int(pt_nln_" + cur_func_->vararg + ");");
        return t;
      }
      // Post-2C perf: native int-lane twin call. A static call to an
      // int-lane-viable function whose arguments are all int-shaped runs
      // in the twin's int64 lane; the int64 result is boxed right here
      // (one immediate struct) for the dynamic world. native_int_text
      // returns "" for anything off-lane — named args, non-int shapes,
      // dynamic callees — and the call then falls through to the exact
      // dynamic path as before.
      if (!e.is_builtin_print && !e.is_builtin_range && !e.is_builtin &&
          !e.callee_expr && !e.callee.empty() && nativ_fns_.count(e.callee)) {
        std::string nt = native_int_text(e);
        // A temp, not an inline text: emit_expr callers may splice the
        // returned text MORE THAN ONCE (a compound index assign reuses
        // the index for get+set, a switch scrutinee per case), and a
        // call is not re-executable. The int64 stays unboxed until the
        // temp store — one box, one call.
        if (!nt.empty()) {
          std::string t = decl_temp();
          line(ind() + t + " = pt_int(" + nt + ");");
          return t;
        }
      }
      std::vector<std::string> args;
      args.reserve(e.args.size());
      for (const auto& a : e.args) {
        if (!a) fail("call argument missing (line " + std::to_string(e.loc.line) + ")");
        args.push_back(emit_expr(*a, in_function));  // emitted in source order
      }
      if (e.is_builtin_print) {
        std::string ln = std::to_string(e.loc.line);
        if (args.empty()) {
          line(ind() + "pt_print(NULL, 0, " + ln + ");");
        } else {
          std::string s = ind() + "{ pt_value pt_args[] = {";
          for (size_t i = 0; i < args.size(); ++i) {
            if (i) s += ",";
            s += " " + args[i];
          }
          s += " }; pt_print(pt_args, " + std::to_string(args.size()) + ", " + ln + "); }";
          line(s);
        }
        return "pt_none()";
      }
      if (e.is_builtin_range) {  // M2-2A §6: range() outside a for header
        std::string t = decl_temp();
        line(ind() + t + " = " + range3_text(args, e.loc.line) + ";");
        return t;
      }
      if (e.is_builtin) {  // M2-2B §8: len/str/int/float/bool/type/encode/decode/set
        std::string t = decl_temp();
        line(ind() + t + " = " +
                  builtin_call_text(e, args, e.loc.line) + ";");
        return t;
      }
      // M2-2B §13: the callee expression is authoritative when set (it may
      // coexist with the bare name) — dynamic pt_call through a function
      // value. Otherwise a bare name resolving to a top-level function is a
      // static direct call through the bound vector.
      if (e.callee_expr) return emit_dynamic_call(e, in_function);
      if (e.callee.empty())
        fail("call without callee (line " + std::to_string(e.loc.line) + ")");
      const FuncDecl* f = find_function(e.callee);
      if (!f) fail("call to undefined function '" + e.callee + "' (line " +
                   std::to_string(e.loc.line) + ")");
      if (f->is_nested)
        fail("internal: nested callee '" + e.callee +
             "' without annotated callee_expr (line " +
             std::to_string(e.loc.line) + ")");
      if (f->c_name.empty()) fail("function '" + f->name + "' has no c_name");
      return emit_static_call(*f, e, args);
    }
  }
  fail("unknown expression kind");
}

std::string Emitter::try_inline(const Expr& e, bool in_function) {
  switch (e.kind) {
    case ExprKind::IntLit:
      return int_literal(e.ival);
    case ExprKind::FloatLit:
      return float_literal(e.fval);
    case ExprKind::StrLit:
      return "PT_STR(\"" + c_escape(e.sval) + "\")";
    case ExprKind::BytesLit:
      return "PT_BYTES(\"" + c_escape(e.sval) + "\")";
    case ExprKind::BoolLit:
      return e.bval ? "pt_bool(true)" : "pt_bool(false)";
    case ExprKind::NoneLit:
      return "pt_none()";

    // ---- M2-2B literals: same discipline as call arguments -------------
    // A compound literal's initializers and each argument position are
    // unsequenced in C, so at most one child may have effects (it gets its
    // own statement expression); otherwise fall back to A-normal emission.
    case ExprKind::ListLit:
    case ExprKind::TupleLit: {
      size_t effectful = 0;
      for (const auto& el : e.elems)
        if (el && has_effect(*el)) ++effectful;
      if (effectful > 1) return "";
      std::vector<std::string> items;
      items.reserve(e.elems.size());
      for (const auto& el : e.elems) items.push_back(inline_child(*el, in_function));
      if (e.kind == ExprKind::ListLit) {
        if (items.empty()) return "pt_list_new()";
        return "pt_list_of(" + value_array_text(items) + ", " +
               std::to_string(items.size()) + "u)";
      }
      if (items.empty()) return "pt_tuple_of(NULL, 0u)";
      return "pt_tuple_of(" + value_array_text(items) + ", " +
             std::to_string(items.size()) + "u)";
    }

    case ExprKind::SetLit: {
      size_t effectful = 0;
      for (const auto& el : e.elems)
        if (el && has_effect(*el)) ++effectful;
      if (effectful > 1) return "";
      std::vector<std::string> items;
      items.reserve(e.elems.size());
      for (const auto& el : e.elems) items.push_back(inline_child(*el, in_function));
      if (items.empty()) return "pt_set_new()";
      return "pt_set_of(" + value_array_text(items) + ", " +
             std::to_string(items.size()) + "u, " + std::to_string(e.loc.line) + ")";
    }

    case ExprKind::MapLit: {
      if (e.keys.size() != e.elems.size())
        fail("malformed map literal (line " + std::to_string(e.loc.line) + ")");
      size_t effectful = 0;
      for (size_t i = 0; i < e.keys.size(); ++i) {
        if (e.keys[i] && has_effect(*e.keys[i])) ++effectful;
        if (e.elems[i] && has_effect(*e.elems[i])) ++effectful;
      }
      if (effectful > 1) return "";
      std::vector<std::string> ks, vs;
      ks.reserve(e.keys.size());
      vs.reserve(e.elems.size());
      for (size_t i = 0; i < e.keys.size(); ++i) {
        ks.push_back(inline_child(*e.keys[i], in_function));
        vs.push_back(inline_child(*e.elems[i], in_function));
      }
      if (ks.empty()) return "pt_map_new()";
      return "pt_map_of(" + value_array_text(ks) + ", " + value_array_text(vs) +
             ", " + std::to_string(ks.size()) + "u, " +
             std::to_string(e.loc.line) + ")";
    }

    case ExprKind::Index: {
      if (!e.lhs || !e.rhs)
        fail("index expression without container or index (line " +
             std::to_string(e.loc.line) + ")");
      if (has_effect(*e.lhs) && has_effect(*e.rhs)) return "";
      std::string obj = inline_child(*e.lhs, in_function);
      std::string idx = inline_child(*e.rhs, in_function);
      return "pt_index_get_f(" + obj + ", " + idx + ", " +
             std::to_string(e.loc.line) + ")";
    }

    case ExprKind::Slice: {
      if (!e.lhs) fail("slice without container (line " + std::to_string(e.loc.line) + ")");
      size_t effectful = 0;
      if (e.has_start && e.rhs && has_effect(*e.rhs)) ++effectful;
      if (e.has_stop && e.third && has_effect(*e.third)) ++effectful;
      if (e.has_step && e.fourth && has_effect(*e.fourth)) ++effectful;
      std::string start, stop, step;
      if (e.has_start && e.rhs) {
        if (effectful > 1 && has_effect(*e.rhs)) return "";
        start = inline_child(*e.rhs, in_function);
      } else {
        start = "pt_unset()";
      }
      if (e.has_stop && e.third) {
        if (effectful > 1 && has_effect(*e.third)) return "";
        stop = inline_child(*e.third, in_function);
      } else {
        stop = "pt_unset()";
      }
      if (e.has_step && e.fourth) {
        if (effectful > 1 && has_effect(*e.fourth)) return "";
        step = inline_child(*e.fourth, in_function);
      } else {
        step = "pt_unset()";
      }
      return "pt_slice(" + inline_child(*e.lhs, in_function) + ", " + start + ", " +
             stop + ", " + step + ", " + std::to_string(e.loc.line) + ")";
    }

    case ExprKind::MethodCall: {
      if (!e.lhs)
        fail("method call without object (line " + std::to_string(e.loc.line) + ")");
      // Planned inline sites force the A-normal statement form so emit_expr
      // can materialize the inlined region — EXCEPT fuse-shape `write` sites,
      // which keep baseline try_inline behavior (emit_expr fuses those and
      // never inlines them).
      {
        auto itu = il_sites_.find(&e);
        if (itu != il_sites_.end()) {
          bool fuse = e.method == "write" && e.elems.size() == 1u && e.elems[0] &&
                      (e.elem_names.empty() || e.elem_names[0].empty());
          if (fuse) {
            std::vector<const Expr*> chk;
            fuse = text_chain_parts(*e.elems[0], chk);
          }
          if (!fuse) return "";
        }
      }
      // M2-2C §3.2: named arguments need the statement-block form — fall
      // back to A-normal emission (emit_expr).
      for (const std::string& n : e.elem_names)
        if (!n.empty()) return "";
      size_t effectful = 0;
      for (const auto& el : e.elems)
        if (el && has_effect(*el)) ++effectful;
      if (has_effect(*e.lhs) && effectful > 0) return "";
      if (effectful > 1) return "";
      std::string obj = inline_child(*e.lhs, in_function);
      std::vector<std::string> args;
      args.reserve(e.elems.size());
      for (const auto& el : e.elems) args.push_back(inline_child(*el, in_function));
      std::string call = "pt_call_method(" + obj + ", \"" + c_escape(e.method) + "\", ";
      call += args.empty() ? "NULL, 0u"
                           : value_array_text(args) + ", " + std::to_string(args.size()) + "u";
      call += ", NULL, 0u, " + std::to_string(e.loc.line) + ")";
      return call;
    }

    case ExprKind::Attr: {  // M2-2C §3.2: single child — order forced
      if (!e.lhs)
        fail("attribute read without object (line " + std::to_string(e.loc.line) + ")");
      std::string obj = inline_child(*e.lhs, in_function);
      return "pt_getattr_lit(" + obj + ", \"" + c_escape(e.attr) + "\", " +
             std::to_string(e.loc.line) + ")";
    }

    case ExprKind::Join: {
      size_t effectful = 0;
      for (const auto& el : e.elems)
        if (el && has_effect(*el)) ++effectful;
      if (effectful > 1) return "";
      std::vector<std::string> items;
      items.reserve(e.elems.size());
      for (const auto& el : e.elems) items.push_back(inline_child(*el, in_function));
      return "pt_join(" + value_array_text(items) + ", " +
             std::to_string(items.size()) + "u, " + std::to_string(e.loc.line) + ")";
    }

    case ExprKind::Var: {
      std::string nr = native_read_text(e.name, e.res, e.loc.line);  // 2C perf
      if (!nr.empty()) return "pt_int(" + nr + ")";
      std::string fr = native_float_read_text(e.name, e.res, e.loc.line);
      if (!fr.empty()) return "pt_float(" + fr + ")";
      std::string storage = storage_for(e.name, e.res, in_function, e.loc.line);
      return guard_text(storage, e.name, e.res, e.loc.line);
    }

    case ExprKind::VarQual: {  // docs/reference/cross-scope-access.md §5
      std::string storage = qual_value_text(e);
      return "pt_guard(" + storage + ", \"" + c_escape(e.name) + "\", " +
             scope_name(e.res) + ", " + std::to_string(e.loc.line) + ")";
    }

    case ExprKind::Unary: {
      if (!e.lhs) fail("unary operator without operand (line " +
                       std::to_string(e.loc.line) + ")");
      std::string a = try_inline(*e.lhs, in_function);  // single child: order forced
      if (a.empty()) return "";
      std::string ln = std::to_string(e.loc.line);
      if (e.op == "-") return "pt_neg_f(" + a + ", " + ln + ")";
      if (e.op == "~") return "pt_bnot(" + a + ", " + ln + ")";
      if (e.op == "not") return "pt_bool(!pt_truthy(" + a + ", " + ln + "))";
      fail("unknown unary operator '" + e.op + "'");
    }

    case ExprKind::Binary: {
      if (!e.lhs || !e.rhs)
        fail("binary operator without operand (line " + std::to_string(e.loc.line) + ")");
      // Two effectful operands would be evaluated in unspecified order by C.
      if (has_effect(*e.lhs) && has_effect(*e.rhs)) return "";
      std::string l = inline_child(*e.lhs, in_function);
      std::string r = inline_child(*e.rhs, in_function);
      return binary_text(e.op, l, r, e.loc.line);
    }

    case ExprKind::Logical: {
      if (!e.lhs || !e.rhs)
        fail("logical operator without operand (line " + std::to_string(e.loc.line) + ")");
      // The left operand must be a single expression here; the right one may
      // fall back to a statement expression because ?: sequences and lazies it.
      std::string l = try_inline(*e.lhs, in_function);
      if (l.empty()) return "";
      std::string r = inline_child(*e.rhs, in_function);
      std::string ln = std::to_string(e.loc.line);
      if (e.op == "and")
        return "(pt_truthy(" + l + ", " + ln + ") ? pt_bool(pt_truthy(" + r + ", " +
               ln + ")) : pt_bool(false))";
      if (e.op == "or")
        return "(pt_truthy(" + l + ", " + ln + ") ? pt_bool(true) : pt_bool(pt_truthy(" +
               r + ", " + ln + ")))";
      fail("unknown logical operator '" + e.op + "'");
    }

    case ExprKind::Ternary: {
      if (!e.lhs || !e.rhs || !e.third)
        fail("ternary without operands (line " + std::to_string(e.loc.line) + ")");
      std::string c = try_inline(*e.lhs, in_function);
      if (c.empty()) return "";
      std::string th = inline_child(*e.rhs, in_function);
      std::string el = inline_child(*e.third, in_function);
      std::string ln = std::to_string(e.loc.line);
      return "(pt_truthy(" + c + ", " + ln + ") ? " + th + " : " + el + ")";
    }

    case ExprKind::Call: {
      // C3 (task-5 A): inside a _b body the sanctioned len(more) inlines
      // as its raw count — never through the (absent) boxed slot.
      if (bulk_fn_ && cur_func_ == bulk_fn_ && e.is_builtin &&
          is_bulk_len_call(e, cur_func_->vararg))
        return "pt_int(pt_nln_" + cur_func_->vararg + ")";
      // Arguments are eager: at most one may have effects (C arg order), and
      // a hard one goes into its own statement expression.
      size_t effectful = 0;
      for (const auto& a : e.args) {
        if (!a) fail("call argument missing (line " + std::to_string(e.loc.line) + ")");
        if (has_effect(*a)) ++effectful;
      }
      if (effectful > 1) return "";
      std::vector<std::string> args;
      args.reserve(e.args.size());
      for (const auto& a : e.args) args.push_back(inline_child(*a, in_function));
      if (e.is_builtin_print) return print_inline_text(args, e.loc.line);
      if (e.is_builtin_range) return range3_text(args, e.loc.line);
      if (e.is_builtin) {
        // Stubs never consume their arguments — the inline path would drop
        // one effectful argument's evaluation. A-normal fallback keeps it.
        if (py_builtin_stub(e.builtin_name)) return "";
        return builtin_call_text(e, args, e.loc.line);
      }
      // M2-2B §13: every user call lowers through the bound vector (static)
      // or pt_call (dynamic) — both emit statements, never a bare C
      // expression. Fall back to A-normalized emission.
      (void)args;
      return "";
    }
  }
  fail("unknown expression kind");
}

std::string Emitter::stmt_expr(const Expr& e, bool in_function) {
  // Evaluates `e` inside a GNU statement expression `({ ...; t; })`. Used
  // only where the surrounding C construct sequences this expression by
  // itself (a ?: branch, or the single effectful child of a call), so the
  // A-normalized statements keep left-to-right order and, in a branch, stay
  // lazy. Never touches out_.
  std::string t = fresh_temp();
  std::string saved;
  saved.swap(out_);
  std::string v = emit_expr(e, in_function);
  std::string stmts = out_;
  out_.swap(saved);
  return "({ " + stmts + ind() + v + "; })";
}

std::string Emitter::inline_child(const Expr& e, bool in_function) {
  std::string s = try_inline(e, in_function);
  if (!s.empty()) return s;
  return stmt_expr(e, in_function);
}

void Emitter::emit_body(const std::vector<Stmt>& body, bool in_function) {
  for (const Stmt& s : body) emit_stmt(s, in_function);
}

// ---- M2-2C §2: try/except/finally lowering --------------------------------
//
// One source try becomes one C scope holding setjmp landing pads (setjmp
// must live in the generated function — §2.5). A try with both except
// clauses and a finally nests two handlers: inner = except logic,
// outer = finally; each edge converges on a single done label so the
// finally body runs exactly once per path (§2.6).

void Emitter::emit_try_exits(int bound_depth, bool in_function) {
  // Walk the enclosing tries innermost-first. bound_depth >= 0 stops at
  // tries that begin outside the current loop (break/continue must not
  // touch them); -1 (return) takes all. Per try: leave its still-entered
  // handlers, then run the finally body.
  for (size_t i = try_stack_.size(); i-- > 0;) {
    const TryCtx& c = try_stack_[i];  // re-fetched each iteration: the
                                      // finally body may nest (push/pop)
    if (bound_depth >= 0 && c.loop_depth != bound_depth) break;
    if (c.entered.empty() && c.finally_src == nullptr) continue;
    for (const std::string& h : c.entered)
      line(ind() + "pt_try_leave(&" + h + ");");
    if (c.finally_src != nullptr) {
      const Stmt* fin = c.finally_src;
      // While this finally runs the try contributes nothing more: the
      // handlers are gone and a nested exit (return inside finally)
      // must not repeat the body.
      std::vector<std::string> saved_entered = try_stack_[i].entered;
      const Stmt* saved_fin = try_stack_[i].finally_src;
      try_stack_[i].entered.clear();
      try_stack_[i].finally_src = nullptr;
      emit_body(fin->finally_body, in_function);
      try_stack_[i].entered = saved_entered;
      try_stack_[i].finally_src = saved_fin;
    }
  }
}

void Emitter::emit_try(const Stmt& s, bool in_function) {
  if (s.arms.empty() && !s.has_finally)
    fail("try without an except or finally clause (line " +
         std::to_string(s.loc.line) + ")");
  if (elided_trys_.count(&s)) {
    // C2: the joint fixpoint (run(): init_try_elision + retract_try_failures)
    // proved this try's body unable to raise a Pentest exception — under the
    // exact shipped state, stmt_never_raise mirroring the emitter's families
    // — so NO exception can ever land here: emit the body and, when present,
    // the finally copy on the normal path; register nothing, and drop the
    // except arms entirely (dead by construction; the dynamic entry and its
    // twin share this flag). try_stack_ keeps finally_src with an EMPTY
    // `entered` list, so break/continue/return still copy this finally once
    // (§2.6) and emit no pt_try_leave for this scope — the unwind edge the
    // registered pad existed for cannot happen. Statistical visibility is
    // unchanged (§7.3): the collector only starts at pt_gc_maybe, which this
    // statement adds or removes none of.
    // CORRIDOR: because ALL of a function's own tries take this path
    // before its lane is admitted (has_own_try at the four fixpoint sites),
    // no setjmp exists in that activation AT ALL — the C11 6.2.2.1
    // indeterminate-local rule for modified non-volatile automatics between
    // setjmp and longjmp can never observe the lane pairs, which is the
    // longjmp-safety argument for keeping them as plain int64_t/double.
    line(ind() + "{  /* try: body proven non-raising, handler not registered */");
    indent_ += 2;
    TryCtx ctx;
    ctx.loop_depth = loop_depth_;
    ctx.finally_src = s.has_finally ? &s : nullptr;
    try_stack_.push_back(ctx);
    emit_body(s.body, in_function);
    if (s.has_finally) {
      // Normal edge runs the finally exactly once, with this try's own exit
      // copy suppressed — the registered path's emit_fin_copy does the same
      // bookkeeping before its finally body.
      try_stack_.back().finally_src = nullptr;
      emit_body(s.finally_body, in_function);
    }
    try_stack_.pop_back();
    indent_ -= 2;
    line(ind() + "}");
    return;
  }
  const size_t seq = try_seq_++;
  const std::string id = std::to_string(seq);
  const std::string lbl_done = "pt_done" + id;
  const std::string lbl_nf = "pt_nf" + id;
  const bool arms = !s.arms.empty();
  const bool fin = s.has_finally;
  const bool nested = arms && fin;  // §2.5: inner catch, outer finally
  const std::string h_fin = "pt_hf" + id;
  const std::string h_catch = "pt_hc" + id;

  TryCtx ctx;
  ctx.loop_depth = loop_depth_;
  ctx.finally_src = fin ? &s : nullptr;
  if (arms) ctx.entered.push_back(h_catch);  // innermost first
  if (fin) ctx.entered.push_back(h_fin);
  try_stack_.push_back(ctx);
  const size_t idx = try_stack_.size() - 1u;

  // Emits one copy of the finally body with this try's handler state
  // cleared (its handlers are already left on every such edge).
  auto emit_fin_copy = [&](const Stmt& src) {
    std::vector<std::string> saved_entered = try_stack_[idx].entered;
    const Stmt* saved_fin = try_stack_[idx].finally_src;
    try_stack_[idx].entered.clear();
    try_stack_[idx].finally_src = nullptr;
    emit_body(src.finally_body, in_function);
    try_stack_[idx].entered = saved_entered;
    try_stack_[idx].finally_src = saved_fin;
  };

  line(ind() + "{");
  indent_ += 2;

  if (fin) {
    line(ind() + "pt_handler " + h_fin + ";");
    line(ind() + "if (setjmp(" + h_fin + ".env) == 0) {");
    indent_ += 2;
    line(ind() + "pt_try_enter(&" + h_fin + ", PT_H_FINALLY);");
  }
  if (arms) {
    line(ind() + "pt_handler " + h_catch + ";");
    line(ind() + "if (setjmp(" + h_catch + ".env) == 0) {");
    indent_ += 2;
    line(ind() + "pt_try_enter(&" + h_catch + ", PT_H_CATCH);");
  }

  // ---- try body (normal edge) ----
  emit_body(s.body, in_function);
  if (arms) {
    line(ind() + "pt_try_leave(&" + h_catch + ");");
    line(ind() + "goto " + (nested ? lbl_nf : lbl_done) + ";");
    indent_ -= 2;
    line(ind() + "} else {");
    indent_ += 2;
    // Landing pad: copy the pending exception into a frame-registered
    // temp FIRST (before any condition evaluation can collect), then
    // match clauses in source order (§2.4). A later clause's condition
    // may need setup statements (a guarded class-name read does); they
    // go inside a fresh `else` block so they run only when every earlier
    // clause failed — never inside the previous clause's body, where
    // they would be skipped on the failing path and executed after a
    // matched body (same pattern as a non-inlined elif, M2-2A §5).
    std::string e = decl_temp();
    line(ind() + e + " = " + h_catch + ".exc;");
    bool bare_done = false;
    int open_else = 0;  // `else` blocks holding setup, awaiting their close
    for (size_t i = 0; i < s.arms.size(); ++i) {
      const ExceptArm& arm = s.arms[i];
      if (arm.cond == nullptr) {
        line(ind() + (i == 0 ? "if (1) {" : "} else {"));
      } else if (i == 0) {
        std::string cond = emit_expr(*arm.cond, in_function);
        line(ind() + "if (pt_exc_matches(" + e + ", " + cond + ", " +
             std::to_string(arm.loc.line) + ")) {");
      } else {
        std::string inl = try_inline(*arm.cond, in_function);
        if (!inl.empty()) {
          line(ind() + "} else if (pt_exc_matches(" + e + ", " + inl + ", " +
               std::to_string(arm.loc.line) + ")) {");
        } else {
          line(ind() + "} else {");
          indent_ += 2;
          open_else++;
          std::string cond = emit_expr(*arm.cond, in_function);
          line(ind() + "if (pt_exc_matches(" + e + ", " + cond + ", " +
               std::to_string(arm.loc.line) + ")) {");
        }
      }
      indent_ += 2;
      if (!arm.name.empty()) {
        std::string st =
            storage_for(arm.name, arm.res, in_function, arm.loc.line);
        line(ind() + st + " = " + e + ";");
      }
      // Arm bodies run after this catch handler was popped by pt_throw:
      // only a still-entered finally handler may appear on their exit
      // edges (§2.6).
      std::vector<std::string> saved_entered = try_stack_[idx].entered;
      try_stack_[idx].entered = fin ? std::vector<std::string>{h_fin}
                                    : std::vector<std::string>{};
      emit_body(arm.body, in_function);
      try_stack_[idx].entered = saved_entered;
      indent_ -= 2;
      if (arm.cond == nullptr) {
        line(ind() + "}");
        bare_done = true;
        break;  // later arms are unreachable (the catch-all matched)
      }
    }
    if (!bare_done) line(ind() + "} else pt_throw(" + e + ");");
    for (; open_else > 0; --open_else) {  // close setup else-blocks, innermost first
      indent_ -= 2;
      line(ind() + "}");
    }
    indent_ -= 2;
    line(ind() + "}");  // closes the landing else
  }
  if (nested) line(ind() + lbl_nf + ": ;");

  if (fin) {
    // Normal edge: leave the finally handler, run the body once, done.
    line(ind() + "pt_try_leave(&" + h_fin + ");");
    emit_fin_copy(s);
    line(ind() + "goto " + lbl_done + ";");
    indent_ -= 2;
    line(ind() + "} else {");
    indent_ += 2;
    // Unwind edge: pt_throw already popped the handler and restored the
    // depths; run the body, then continue unwinding (§2.5/§2.6).
    std::string e2 = decl_temp();
    line(ind() + e2 + " = " + h_fin + ".exc;");
    emit_fin_copy(s);
    line(ind() + "pt_throw(" + e2 + ");");
    indent_ -= 2;
    line(ind() + "}");
  }
  line(ind() + lbl_done + ": ;");
  indent_ -= 2;
  line(ind() + "}");
  try_stack_.pop_back();
}

void Emitter::emit_else_tail(size_t after, const Stmt& s, bool in_function) {
  // Emits the `} else ...` continuation after bodies[after] was closed at
  // the current indent. An `elif` whose condition cannot be inlined becomes
  // `} else { <cond statements> if (...) ... }` so its statements only run
  // when the chain actually reaches it (M2-2A §5).
  if (after + 1 < s.conds.size()) {
    size_t j = after + 1;
    const Expr& cond = *s.conds[j];
    std::string c = try_inline(cond, in_function);
    if (!c.empty()) {
      line(ind() + "} else if (pt_truthy(" + c + ", " + std::to_string(cond.loc.line) +
           ")) {");
      indent_ += 2;
      emit_body(s.bodies[j], in_function);
      indent_ -= 2;
      emit_else_tail(j, s, in_function);
    } else {
      line(ind() + "} else {");
      indent_ += 2;
      emit_if_from(j, s, in_function);  // statements land inside: lazy
      indent_ -= 2;
      line(ind() + "}");
    }
  } else if (s.has_else) {
    line(ind() + "} else {");
    indent_ += 2;
    emit_body(s.else_body, in_function);
    indent_ -= 2;
    line(ind() + "}");
  } else {
    line(ind() + "}");
  }
}

void Emitter::emit_if_from(size_t idx, const Stmt& s, bool in_function) {
  const Expr& cond = *s.conds[idx];
  std::string nc;
  if (native_cond_text(cond, nc)) {  // Post-2C: native int/bool condition
    // nc is already fully parenthesized (or an atomic `true`/`false`), so
    // it is used bare: wrapping it again yields `((a == b))`, which clang
    // rejects with -Wparentheses-equality — and any warning on stderr
    // fails the byte-compat harness.
    line(ind() + "if " + nc + " {");
  } else {
    std::string c = try_inline(cond, in_function);
    if (c.empty()) c = emit_expr(cond, in_function);  // statements before the if
    line(ind() + "if (pt_truthy(" + c + ", " + std::to_string(cond.loc.line) + ")) {");
  }
  indent_ += 2;
  emit_body(s.bodies[idx], in_function);
  indent_ -= 2;
  emit_else_tail(idx, s, in_function);
}

void Emitter::emit_switch_tail(size_t after, const Stmt& s, const std::string& scrut,
                               bool in_function) {
  if (after + 1 < s.cases.size()) {
    const CaseArm& arm = s.cases[after + 1];
    if (!arm.is_wildcard) {
      line(ind() + "} else if (pt_truthy(pt_eq_f(" + scrut + ", " + case_label_text(arm) +
           "), " + std::to_string(arm.loc.line) + ")) {");
      indent_ += 2;
      emit_body(arm.body, in_function);
      indent_ -= 2;
      emit_switch_tail(after + 1, s, scrut, in_function);
    } else {
      // `case *:` is guaranteed last (§2): the wildcard is the final else.
      line(ind() + "} else {");
      indent_ += 2;
      emit_body(arm.body, in_function);
      indent_ -= 2;
      line(ind() + "}");
    }
  } else {
    line(ind() + "}");
  }
}

void Emitter::emit_switch_from(size_t idx, const Stmt& s, const std::string& scrut,
                               bool in_function) {
  const CaseArm& arm = s.cases[idx];
  if (arm.is_wildcard) {
    // A lone `case *:` matches unconditionally: emit just its block.
    line(ind() + "{  /* switch: wildcard arm */");
    indent_ += 2;
    emit_body(arm.body, in_function);
    indent_ -= 2;
    line(ind() + "}");
    return;
  }
  // First match wins; each arm is its own block, so control leaves the chain
  // automatically (§5: no fall-through, no break). pt_eq_f is total (no line);
  // pt_truthy converts its bool value to a C condition (arm's line).
  line(ind() + "if (pt_truthy(pt_eq_f(" + scrut + ", " + case_label_text(arm) + "), " +
       std::to_string(arm.loc.line) + ")) {");
  indent_ += 2;
  emit_body(arm.body, in_function);
  indent_ -= 2;
  emit_switch_tail(idx, s, scrut, in_function);
}

// ---- C1: native `switch` lowering for proven-int scrutinees ---------------
// A `break` in Pentest ALWAYS targets the innermost enclosing LOOP (a switch
// does not count as one, §6.8) — but inside a C `switch` it would break the
// switch instead. Walk an arm body with `depth` = loops opened *inside* the
// arm; a Break at depth 0 targets the loop OUTSIDE this switch, so that
// switch must not be lowered natively (the ladder keeps `break` reaching the
// enclosing C loop, exactly as today). Nested switches are transparent for
// this walk (their arms sit at this arm's own level). Nested function/class
// declarations own their loops and are not entered.
static bool arm_breaks_outer_loop(const std::vector<Stmt>& body, int depth) {
  for (const Stmt& s : body) {
    switch (s.kind) {
      case StmtKind::Break:
        if (depth == 0) return true;
        break;
      case StmtKind::While:
      case StmtKind::For:
        if (arm_breaks_outer_loop(s.body, depth + 1)) return true;
        break;
      case StmtKind::If:
        for (const std::vector<Stmt>& b : s.bodies)
          if (arm_breaks_outer_loop(b, depth)) return true;
        if (arm_breaks_outer_loop(s.else_body, depth)) return true;
        break;
      case StmtKind::Switch:
        for (const CaseArm& a : s.cases)
          if (arm_breaks_outer_loop(a.body, depth)) return true;
        break;
      case StmtKind::Try:
        if (arm_breaks_outer_loop(s.body, depth)) return true;
        for (const ExceptArm& a : s.arms)
          if (arm_breaks_outer_loop(a.body, depth)) return true;
        if (arm_breaks_outer_loop(s.finally_body, depth)) return true;
        break;
      default:
        break;  // Assign/ExprStmt/Return/Raise/Pass/Func/Class/decls: no loops
    }
  }
  return false;
}

// Proof gate for the native switch: every condition must be PROVEN, else the
// ladder emitted by emit_switch_from/emit_switch_tail runs unchanged
// (conservative by one slot — docs/performance/optimization.md §0.3).
bool Emitter::native_switch_ok(const Stmt& s) const {
  if (s.cases.empty()) return false;
  std::set<long long> seen;
  for (size_t i = 0; i < s.cases.size(); ++i) {
    const CaseArm& a = s.cases[i];
    if (a.is_wildcard) {
      // `case *:` must be last (parser-enforced, §9.1) — and only a LAST
      // wildcard maps onto C's `default:`. Re-check defensively: a stray
      // wildcard inside the list would silently reorder first-match-wins.
      if (i + 1 != s.cases.size()) return false;
      continue;
    }
    // INT labels only. Their total `==` with an int64 scrutinee (§6.7) is
    // plain int64 equality — exactly what a C `case` does. Float labels can
    // match an int scrutinee (`case 1:` vs scrutinee 1.0 and vice versa,
    // §5.5 total ==), bool/text/None labels compare a different tag, so any
    // non-int label keeps the whole switch on the boxed ladder.
    if (a.label_kind != ExprKind::IntLit) return false;
    // Duplicate labels are a semantic-pass compile error already ("duplicate
    // case value in switch"); a C `switch` rejects them as a hard error, so
    // re-check and fall back rather than emit one.
    if (!seen.insert(a.ival).second) return false;
    if (arm_breaks_outer_loop(a.body, 0)) return false;
  }
  return true;
}

// Native lowering: `switch (nat)` evaluates the scrutinee exactly once,
// before any arm (§6.7); labels are constant literals (nothing to evaluate,
// §6.7 parser rule), so evaluation order is trivially preserved; each arm is
// its own block terminated by `break` — arms end automatically, no
// fall-through (§6.7), no C `break` from source (gated above).
void Emitter::emit_native_switch(const Stmt& s, const std::string& nat,
                                 bool in_function) {
  line(ind() + "switch (" + nat + ") {");
  indent_ += 2;
  bool has_wildcard = false;
  for (const CaseArm& a : s.cases) {
    if (a.is_wildcard)
      line(ind() + "default: {");  // `case *:` is last (native_switch_ok)
    else
      line(ind() + "case " +
           (a.ival == INT64_MIN ? std::string("INT64_MIN")
                                : std::to_string(a.ival) + "LL") + ": {");
    indent_ += 2;
    emit_body(a.body, in_function);
    // Arms end on their own; a `return`/`continue` tail already leaves the
    // switch, so an extra `break` there would be dead code (skipped: the
    // build must keep clang's stderr byte-clean).
    const Stmt* last = a.body.empty() ? nullptr : &a.body.back();
    if (!last || (last->kind != StmtKind::Return && last->kind != StmtKind::Continue))
      line(ind() + "break;");
    indent_ -= 2;
    line(ind() + "}");
    has_wildcard = has_wildcard || a.is_wildcard;
  }
  if (!has_wildcard)
    // Semantically a no-op (§6.7: no arm matches → nothing runs), but it
    // keeps clang quiet when the scrutinee folds to a compile-time constant:
    // `switch (9LL) {...}` with no `default:` trips the default-on warning
    // "no case matching constant switch condition", and any stderr from the
    // C build breaks the byte-clean compile contract (§6).
    line(ind() + "default: break;");
  indent_ -= 2;
  line(ind() + "}");
}

void Emitter::emit_stmt(const Stmt& s, bool in_function) {
  const size_t temp_mark = pending_temps_.size();
  bool fell_through = true;  // `return` leaves no reachable tail to clear
  switch (s.kind) {
    case StmtKind::Assign: {
      auto ep = ephem_.find(&s);
      if (ep != ephem_.end()) {
        // Lazy slice: normalize bounds now, skip the target store; the
        // later target[idx] reads fuse onto the source.
        emit_ephemeral_slice(s, ep->second, in_function);
        break;
      }
      if (!s.expr) fail("assignment without expression (line " +
                        std::to_string(s.loc.line) + ")");
      if (s.target.kind == AssignTarget::Kind::Name) {
        // M1/2A fast path: the semantic pass mirrors the target into
        // s.name/s.res; the generated code stays byte-identical to 2A.
        if (s.res == VarRes::Unresolved)
          fail("unresolved assignment target '" + s.name + "' (line " +
               std::to_string(s.loc.line) + ")");
        // Post-2C native slot: store into the native pair — no pt_value
        // touch, no guard temp. The analysis guarantees every write of a
        // native slot is int-shaped, so a "" here is an internal drift bug.
        {
          std::string nslot, nflag;
          if (native_slot_text(s.name, s.res, nslot, nflag)) {
            std::string ln = std::to_string(s.loc.line);
            // Post-2C perf: a definitely-assigned slot's flag is never read,
            // so skip the `flag = 1;` store (dead write off the hot path).
            std::string fstore =
                native_noflag(s.name, s.res) ? std::string() : " " + nflag + " = 1;";
            if (s.op.empty()) {
              std::string v = native_int_text(*s.expr);
              if (v.empty())
                fail("internal: native slot '" + s.name +
                     "' assigned a non-int expression (line " + ln + ")");
              line(ind() + nslot + " = " + v + ";" + fstore);
            } else {
              std::string cur = native_read_text(s.name, s.res, s.loc.line);
              std::string v = native_int_text(*s.expr);
              if (cur.empty() || v.empty())
                fail("internal: native compound slot '" + s.name + "' (line " +
                     ln + ")");
              std::string call;
              if (s.op == "+=") call = "pt_nwrap_add(" + cur + ", " + v + ")";
              else if (s.op == "-=") call = "pt_nwrap_sub(" + cur + ", " + v + ")";
              else if (s.op == "*=") call = "pt_nwrap_mul(" + cur + ", " + v + ")";
              else if (s.op == "/=") call = "pt_ndiv(" + cur + ", " + v + ", " + ln + ")";
              else if (s.op == "%=") call = "pt_nmod(" + cur + ", " + v + ", " + ln + ")";
              else if (s.op == "&=") call = "pt_nwrap_band(" + cur + ", " + v + ")";
              else if (s.op == "|=") call = "pt_nwrap_bor(" + cur + ", " + v + ")";
              else if (s.op == "^=") call = "pt_nwrap_bxor(" + cur + ", " + v + ")";
              else if (s.op == "<<=")
                call = "pt_nwrap_shl(" + cur + ", " + v + ", " + ln + ")";
              else if (s.op == ">>=")
                call = "pt_nwrap_shr(" + cur + ", " + v + ", " + ln + ")";
              else
                fail("internal: native slot '" + s.name + "' non-native op '" +
                     s.op + "' (line " + ln + ")");
              line(ind() + nslot + " = " + call + ";" + fstore);
            }
            return;
          }
        }
        // Post-2C native float slot: store into the native double pair —
        // no pt_value touch, no guard temp. The analysis guarantees every
        // write of a float slot is float-shaped, so a "" here is an
        // internal drift bug. Same flag discipline as the int lane.
        {
          std::string nslot, nflag;
          if (native_float_slot_text(s.name, s.res, nslot, nflag)) {
            std::string ln = std::to_string(s.loc.line);
            std::string fstore =
                native_noflag(s.name, s.res) ? std::string() : " " + nflag + " = 1;";
            if (s.op.empty()) {
              std::string v = native_float_text(*s.expr);
              if (v.empty())
                fail("internal: native float slot '" + s.name +
                     "' assigned a non-float expression (line " + ln + ")");
              line(ind() + nslot + " = " + v + ";" + fstore);
            } else {
              std::string cur = native_float_read_text(s.name, s.res, s.loc.line);
              std::string v = native_float_text(*s.expr);
              if (cur.empty() || v.empty())
                fail("internal: native float compound slot '" + s.name +
                     "' (line " + ln + ")");
              std::string call;
              // Plain IEEE ops in source order (the fixpoint only admits
              // +=, -=, *=, /=; "%" declines the slot — pt_mod raises
              // TypeError on floats, identically dynamic).
              if (s.op == "+=") call = "(" + cur + " + " + v + ")";
              else if (s.op == "-=") call = "(" + cur + " - " + v + ")";
              else if (s.op == "*=")
                call = "pt_nfmul(" + cur + ", " + v + ")";
              else if (s.op == "/=")
                call = "pt_nfdiv(" + cur + ", " + v + ", " + ln + ")";
              else
                fail("internal: native float slot '" + s.name +
                     "' non-float op '" + s.op + "' (line " + ln + ")");
              line(ind() + nslot + " = " + call + ";" + fstore);
            }
            return;
          }
        }
        // SIMD fusion: a fvec/ivec-proven slot assigned a kind-shaped
        // elementwise expression collapses to ONE fused kernel loop —
        // no intermediate vectors, guaranteed lanes (analyze_native_fvecs
        // is the entire type contract; the read side of a compound op is
        // the slot itself, proven by the same pass).
        {
          int fk;
          if (fvec_slot_kind(s.name, s.res, cur_func_, fk)) {
            if (s.op.empty()) {
              // Only elementwise EXPRESSIONS fuse — a bare constructor
              // call stores its result directly (a copy kernel would
              // just burn a pass).
              bool elementwise = s.expr->kind == ExprKind::Binary ||
                                 s.expr->kind == ExprKind::Unary;
              if (elementwise && fvec_shape(*s.expr, cur_func_) == fk) {
                emit_fvec_assign(s, fk, in_function);
                return;
              }
            } else if (s.op == "+=" || s.op == "-=" || s.op == "*=") {
              int rk = fvec_shape(*s.expr, cur_func_);
              if (rk == fk || rk == 0) {
                emit_fvec_assign(s, fk, in_function);
                return;
              }
            }
          }
        }
        std::string target = storage_for(s.name, s.res, in_function, s.loc.line);
        // Text-cat in-place growth (analyze_text_cats): a proven
        // `s = s + expr` / `s += expr` appends into the buffer the slot
        // uniquely owns — pt_text_cat2(..., may_grow=1), O(total) copy
        // volume — instead of rebuilding an exact-size block each time.
        const bool catgrow = catok_.count(&s) != 0;
        if (s.op.empty()) {
          if (catgrow) {
            // Binary "+" with the slot itself as lhs (the proven shape);
            // text-chain fusion cannot fire on a Var operand, so this is
            // exactly the old emission with the capacity-aware operator.
            const Expr& be = *s.expr;
            std::string l = emit_expr(*be.lhs, in_function);
            std::string r = emit_expr(*be.rhs, in_function);
            std::string t = decl_temp();
            line(ind() + t + " = pt_text_cat2(" + l + ", " + r + ", " +
                 std::to_string(be.loc.line) + ", 1);");
            line(ind() + target + " = " + t + ";");
          } else {
            // M1 plain assignment.
            std::string v = emit_expr(*s.expr, in_function);
            line(ind() + target + " = " + v + ";");
          }
        } else {
          // Compound assignment (M2-2A §3.3): read target (guarded), apply the
          // operator at the statement's line, write back.
          const char* fn = compound_fn(s.op);
          if (!fn)
            fail("unknown compound assignment operator '" + s.op + "' (line " +
                 std::to_string(s.loc.line) + ")");
          std::string ln = std::to_string(s.loc.line);
          std::string cur = decl_temp();
          line(ind() + cur + " = " +
               guard_text(target, s.name, s.res, s.loc.line) + ";");
          std::string v = emit_expr(*s.expr, in_function);
          std::string t = decl_temp();
          if (catgrow)  // only `+=` candidates are ever registered in catok_
            line(ind() + t + " = pt_text_cat2(" + cur + ", " + v + ", " + ln +
                 ", 1);");
          else
            line(ind() + t + " = " + fn + "(" + cur + ", " + v + ", " + ln + ");");
          line(ind() + target + " = " + t + ";");
        }
      } else if (s.target.kind == AssignTarget::Kind::Index) {
        // M2-2B §10: index assignment. The container and index expressions
        // are evaluated exactly once, in source order, and reused for both
        // the read and the write of a compound form.
        std::string obj = emit_expr(*s.target.object, in_function);
        std::string idx = scalar_int_operand(*s.target.index, in_function);
        std::string ln = std::to_string(s.loc.line);
        if (s.op.empty()) {
          std::string v = emit_expr(*s.expr, in_function);
          line(ind() + "pt_index_set(" + obj + ", " + idx + ", " + v + ", " + ln + ");");
        } else {
          const char* fn = compound_fn(s.op);
          if (!fn)
            fail("unknown compound assignment operator '" + s.op + "' (line " +
                 std::to_string(s.loc.line) + ")");
          // `xs[i] <op>= v` ≡ `xs[i] = xs[i] <op> v` (§10): the current
          // element is read (line of the statement) before the RHS, matching
          // the expanded form's left operand.
          std::string cur = decl_temp();
          line(ind() + cur + " = pt_index_get_f(" + obj + ", " + idx +
               ", " + ln + ");");
          std::string v = emit_expr(*s.expr, in_function);
          std::string t = decl_temp();
          line(ind() + t + " = " + fn + "(" + cur + ", " + v + ", " + ln + ");");
          line(ind() + "pt_index_set(" + obj + ", " + idx + ", " + t + ", " + ln + ");");
        }
      } else if (s.target.kind == AssignTarget::Kind::Attr) {
        // M2-2C §3.2: attribute assignment. The object is evaluated once,
        // in source order, and reused for the read and the write of a
        // compound form (mirroring the Index branch above).
        if (!s.target.object)
          fail("attribute assignment without object (line " +
               std::to_string(s.loc.line) + ")");
        std::string obj = emit_expr(*s.target.object, in_function);
        std::string ln = std::to_string(s.loc.line);
        std::string key = "\"" + c_escape(s.target.attr) + "\"";
        if (s.op.empty()) {
          std::string v = emit_expr(*s.expr, in_function);
          line(ind() + "pt_setattr_lit(" + obj + ", " + key + ", " + v + ", " + ln + ");");
        } else {
          const char* fn = compound_fn(s.op);
          if (!fn)
            fail("unknown compound assignment operator '" + s.op + "' (line " + ln + ")");
          std::string cur = decl_temp();
          line(ind() + cur + " = pt_getattr_lit(" + obj + ", " + key + ", " + ln + ");");
          std::string v = emit_expr(*s.expr, in_function);
          std::string t = decl_temp();
          line(ind() + t + " = " + fn + "(" + cur + ", " + v + ", " + ln + ");");
          line(ind() + "pt_setattr_lit(" + obj + ", " + key + ", " + t + ", " + ln + ");");
        }
      } else if (s.target.kind == AssignTarget::Kind::VarQual) {
        // docs/reference/cross-scope-access.md §5: store into the pinned static (or the
        // module global) directly. Compound forms read the slot with the
        // same guard an ordinary compound assignment uses.
        std::string target = qual_value_text(s.target);
        if (s.op.empty()) {
          std::string v = emit_expr(*s.expr, in_function);
          line(ind() + target + " = " + v + ";");
        } else {
          const char* fn = compound_fn(s.op);
          if (!fn)
            fail("unknown compound assignment operator '" + s.op + "' (line " +
                 std::to_string(s.loc.line) + ")");
          std::string ln = std::to_string(s.loc.line);
          std::string cur = decl_temp();
          line(ind() + cur + " = pt_guard(" + target + ", \"" +
               c_escape(s.target.name) + "\", " + scope_name(s.target.res) +
               ", " + ln + ");");
          std::string v = emit_expr(*s.expr, in_function);
          std::string t = decl_temp();
          line(ind() + t + " = " + fn + "(" + cur + ", " + v + ", " + ln + ");");
          line(ind() + target + " = " + t + ";");
        }
      } else {
        // M2-2B §12 destructuring (Tuple patterns, incl. single Rest).
        if (!s.op.empty())
          fail("compound assignment to a destructuring target is not supported "
               "(line " + std::to_string(s.loc.line) + ")");
        std::string src = emit_expr(*s.expr, in_function);  // RHS exactly once
        emit_destructure(s.target, src, in_function);
      }
      break;
    }
    case StmtKind::ExprStmt: {
      if (!s.expr) fail("expression statement without expression (line " +
                        std::to_string(s.loc.line) + ")");
      std::string v = emit_expr(*s.expr, in_function);
      line(ind() + "((void)(" + v + "));");
      break;
    }
    case StmtKind::Return: {
      if (!in_function)
        fail("'return' outside of a function (line " + std::to_string(s.loc.line) + ")");
      // Inlined method body: a `return` leaves the inlined region only —
      // store to the region result and jump to its end label. No enclosing
      // try/finally of the CALLER runs here (the method's own statements
      // already emitted their exits inline), and no frame teardown: the
      // caller's frame keeps running. Nested regions chain: an inner
      // region's text is assigned into the outer region's result, which then
      // goto's the outer end label.
      if (!micx_.empty()) {
        std::string v = s.expr ? emit_expr(*s.expr, in_function) : "pt_none()";
        line(ind() + "pt_ilr" + std::to_string(micx_id_) + " = " + v + ";");
        line(ind() + "goto pt_ile" + std::to_string(micx_id_) + "end;");
        fell_through = false;
        break;
      }
      if (s.expr) {
        // Post-2C twin lane: every twin return expression is int-shaped
        // (the joint lane fixpoint proved it) — emit the raw int64. The caller
        // boxes the result only when it needs a pt_value.
        if (twin_fn_) {
          std::string v = native_int_text(*s.expr);
          if (v.empty())
            fail("internal: twin return of a non-int expression (line " +
                 std::to_string(s.loc.line) + ")");
          emit_try_exits(-1, in_function);  // no-op: twins carry no try
          if (framed_) line(ind() + "pt_frame_end(&pt_fr);");
          line(ind() + "return " + v + ";");
          fell_through = false;
          break;
        }
        std::string v = emit_expr(*s.expr, in_function);
        // M2-2C §2.6: leave enclosing handlers + run finallys (innermost
        // first) before leaving the frame.
        emit_try_exits(-1, in_function);
        // Unlink the shadow frame last: the returned expression may call
        // functions (entry safe points) that need this frame rooted (2C §1.5).
        if (framed_) line(ind() + "pt_frame_end(&pt_fr);");
        line(ind() + "return " + v + ";");
      } else {
        emit_try_exits(-1, in_function);
        if (framed_) line(ind() + "pt_frame_end(&pt_fr);");
        line(ind() + "return pt_none();");  // bare `return` = None
      }
      fell_through = false;
      break;
    }

    case StmtKind::If: {
      if (s.conds.empty() || s.bodies.size() != s.conds.size())
        fail("malformed if statement (line " + std::to_string(s.loc.line) + ")");
      emit_if_from(0, s, in_function);
      break;
    }

    case StmtKind::While: {
      if (!s.expr) fail("while without condition (line " + std::to_string(s.loc.line) + ")");
      int ln = s.expr->loc.line;
      // Post-2C: a provably int/bool condition compiles to a native C
      // while header. The back-edge safe point is kept unless the body
      // cannot allocate (§7.3: collection timing is unobservable, and with
      // no allocation the threshold check would be a no-op anyway — see
      // alloc_free_expr / body_alloc_free).
      std::string nc;
      if (native_cond_text(*s.expr, nc)) {
        line(ind() + "while " + nc + " {");  // nc already parenthesized
        indent_ += 2;
        loop_depth_++;
        emit_body(s.body, in_function);
        if (!body_alloc_free(s.body))
          line(ind() + "pt_gc_maybe();");  // loop back-edge safe point (2C §1.4)
        loop_depth_--;
        indent_ -= 2;
        line(ind() + "}");
        break;
      }
      std::string c = try_inline(*s.expr, in_function);
      if (!c.empty()) {
        line(ind() + "while (pt_truthy(" + c + ", " + std::to_string(ln) + ")) {");
        indent_ += 2;
        loop_depth_++;
        emit_body(s.body, in_function);
        if (!(alloc_free_expr(*s.expr) && body_alloc_free(s.body)))
          line(ind() + "pt_gc_maybe();");  // loop back-edge safe point (2C §1.4)
        loop_depth_--;
        indent_ -= 2;
        line(ind() + "}");
      } else {
        // The condition needs statements; they must re-run every iteration,
        // so they live inside the loop and a pre-test break replaces the C
        // header. break/continue still map 1:1.
        line(ind() + "for (;;) {");
        indent_ += 2;
        std::string t = emit_expr(*s.expr, in_function);
        line(ind() + "if (!pt_truthy(" + t + ", " + std::to_string(ln) + ")) break;");
        loop_depth_++;
        emit_body(s.body, in_function);
        if (!(alloc_free_expr(*s.expr) && body_alloc_free(s.body)))
          line(ind() + "pt_gc_maybe();");  // loop back-edge safe point (2C §1.4)
        loop_depth_--;
        indent_ -= 2;
        line(ind() + "}");
      }
      break;
    }

    case StmtKind::For: {
      if (!s.expr) fail("for without iterable (line " + std::to_string(s.loc.line) + ")");
      if (s.res == VarRes::Unresolved)
        fail("unresolved loop variable '" + s.name + "' (line " +
             std::to_string(s.loc.line) + ")");
      std::string target = storage_for(s.name, s.res, in_function, s.loc.line);
      const Expr& it = *s.expr;
      if (it.kind == ExprKind::Call && it.is_builtin_range) {
        // C1: literal bounds fold to a CANONICAL header. const_range_bounds
        // proved every effective bound a compile-time int literal with a
        // non-zero step, so nothing pt_range3 validated can fire (§6.9: all
        // int -> no TypeError; step != 0 -> no ValueError) and the call only
        // built a stack value worth no evaluation — skipping it is
        // behavior-preserving and hands clang constants, so the sign test
        // folds out of the header and the overflow guard drops exactly where
        // the bounds prove no overflow is reachable, leaving a plain counted
        // loop the vectorizer recognizes (like the C twin).
        std::string iv = fresh_ivar();
        int64_t kst = 0, kso = 0, ksp = 1;
        if (const_range_bounds(it, kst, kso, ksp)) {
          bool asc = ksp > 0;
          // Overflow reachable? Values entering the latch are at most
          // stop-1 (ascending) / at least stop+1 (descending) — entry is
          // gated by the condition and the latch is the only writer of iv
          // (body writes go to the slot, not the induction variable). If the
          // mathematical next value fits, the plain add is exact; otherwise
          // keep the SAME sentinel latch as the validated path (§6.9:
          // iteration stops at the int64 edge, no signed-overflow UB).
          bool guard;
          if (asc)
            guard = (kso != INT64_MIN) && ((kso - 1) > (INT64_MAX - ksp));
          else
            guard = (kso != INT64_MAX) && ((kso + 1) < (INT64_MIN - ksp));
          std::string step = cint64(ksp);
          std::string cond = asc ? "(" + iv + " < " + cint64(kso) + ")"
                                 : "(" + iv + " > " + cint64(kso) + ")";
          std::string latch;
          if (!guard) {
            latch = "(" + iv + " + " + step + ")";
          } else if (asc) {
            latch = "((" + iv + " > INT64_MAX - " + step + ") ? INT64_MAX : (" +
                    iv + " + " + step + "))";
          } else {
            latch = "((" + iv + " < INT64_MIN - " + step + ") ? INT64_MIN : (" +
                    iv + " + " + step + "))";
          }
          line(ind() + "int64_t " + iv + " = " + cint64(kst) + ";");
          line(ind() + "for (; " + cond + "; " + iv + " = " + latch + ") {");
        } else {
        // Dynamic bounds: validate via pt_range3, then iterate an int64
        // induction variable with an overflow-safe post step.
        std::vector<std::string> args;
        args.reserve(it.args.size());
        for (const auto& a : it.args) {
          if (!a) fail("range() argument missing (line " + std::to_string(it.loc.line) + ")");
          args.push_back(emit_expr(*a, in_function));  // source order
        }
        int ln = it.loc.line;
        std::string rng = decl_temp();
        line(ind() + rng + " = " + range3_text(args, ln) + ";");
        std::string rs = fresh_temp();
        std::string ro = fresh_temp();
        std::string rp = fresh_temp();
        line(ind() + "int64_t " + rs + " = " + rng + ".as.r.start;");
        line(ind() + "int64_t " + ro + " = " + rng + ".as.r.stop;");
        line(ind() + "int64_t " + rp + " = " + rng + ".as.r.step;");
        line(ind() + "int64_t " + iv + " = " + rs + ";");
        // Loop condition per §5/§6: step>0 → i < stop, else i > stop. The
        // post expression advances with an explicit overflow guard and
        // substitutes a sentinel (INT64_MAX for ascending, INT64_MIN for
        // descending) that cannot satisfy the condition, so iteration stops
        // when the next index would leave int64 range — no signed-overflow
        // UB and every input terminates. `continue` reaches the post
        // expression exactly like a plain C for loop.
        std::string hdr =
            "for (; (" + rp + " > 0) ? (" + iv + " < " + ro + ") : (" + iv + " > " + ro +
            "); " + iv + " = (" + rp + " > 0) ? ((" + iv + " > INT64_MAX - " + rp +
            ") ? INT64_MAX : " + iv + " + " + rp + ") : ((" + iv + " < INT64_MIN - " + rp +
            ") ? INT64_MIN : " + iv + " + " + rp + "))";
        line(ind() + hdr + " {");
        }  // end non-constant bounds (validated pt_range3 path)
        indent_ += 2;
        loop_depth_++;
        // Native range-loop target: bind the int64 induction variable
        // straight into the native slot (+ used-flag — the range may be
        // empty, and DA keeps the flag for every native target). All reads
        // of the name resolve through the native path, so the boxed slot
        // stays UNSET and is never observed.
        std::string nslot, nflag;
        if (native_slot_text(s.name, s.res, nslot, nflag)) {
          line(ind() + nslot + " = " + iv + ";");
          // DA keeps the flag for every local/global for-target (an empty
          // range skips the body, so a read must still error); a twin
          // param lane is noflag — no flag storage exists, and no read of
          // the lane can precede this bind.
          if (!native_noflag(s.name, s.res))
            line(ind() + nflag + " = 1;");
        } else {
          line(ind() + target + " = pt_int(" + iv + ");");  // bind before body (§5)
        }
        emit_body(s.body, in_function);
        // range3 + pt_int bind never allocate (§7.3): body decides alone.
        if (!body_alloc_free(s.body))
          line(ind() + "pt_gc_maybe();");  // loop back-edge safe point (2C §1.4)
        loop_depth_--;
        indent_ -= 2;
        line(ind() + "}");
      } else if (bulk_fn_ && cur_func_ && is_bulk_for(s, cur_func_->vararg) &&
                 it.kind == ExprKind::Var) {
        // C3 (task-5 A): the SANCTIONED bulk iteration. The _b entry holds
        // the raw int64 tail in pt_nlb_<vararg>/pt_nln_<vararg>; each
        // element boxes into the SAME slot the dynamic binder fills (bind
        // before body, §5, source order) instead of the pt_iter protocol
        // over a heap list. Admission (analyze_bulk_varargs) and this gate
        // share is_bulk_for(), so an inadmissible shape never reaches a _b
        // body; a lane-spelling target would be drift — fail loudly.
        std::string nslot, nflag;
        if (native_slot_text(s.name, s.res, nslot, nflag))
          fail("internal: bulk vararg loop target '" + s.name +
               "' resolved to a native lane slot (line " +
               std::to_string(s.loc.line) + ")");
        std::string k = fresh_ivar();
        std::string cnt = "pt_nln_" + cur_func_->vararg;
        std::string elems = "pt_nlb_" + cur_func_->vararg;
        line(ind() + "for (int64_t " + k + " = 0; " + k + " < " + cnt + "; " +
             k + "++) {");
        indent_ += 2;
        loop_depth_++;
        line(ind() + target + " = pt_int(" + elems + "[" + k + "]);");  // bind
        emit_body(s.body, in_function);
        line(ind() + "pt_gc_maybe();");  // loop back-edge safe point (2C §1.4)
        loop_depth_--;
        indent_ -= 2;
        line(ind() + "}");
      } else {
        // Iterator protocol (string now; list/map/set via 2B).
        std::string itex = emit_expr(it, in_function);
        int ln = it.loc.line;
        std::string iter = fresh_temp();
        std::string cur = decl_temp();
        line(ind() + "pt_iter " + iter + " = pt_iter_open(" + itex + ", " +
             std::to_string(ln) + ");");
        line(ind() + cur + " = pt_unset();");
        line(ind() + "while (pt_iter_next(&" + iter + ", &" + cur + ")) {");
        indent_ += 2;
        loop_depth_++;
        line(ind() + target + " = " + cur + ";");  // bind before body (§5)
        emit_body(s.body, in_function);
        line(ind() + "pt_gc_maybe();");  // loop back-edge safe point (2C §1.4)
        loop_depth_--;
        indent_ -= 2;
        line(ind() + "}");
      }
      break;
    }

    case StmtKind::Break: {
      if (loop_depth_ == 0)
        fail("'break' outside of a loop (line " + std::to_string(s.loc.line) + ")");
      // A switch shaped as a ladder (if/else chain) lets this `break;` reach the
      // enclosing C loop — matching §5 (break in a switch inside a loop breaks
      // the loop). C1's native `switch` lowering only fires when NO arm holds
      // a loop-level break (native_switch_ok), so inside an arm a `break;` is
      // either this statement (ladder) or nested in an inner loop — and the
      // innermost C construct then is the loop, in both lowerings.
      emit_try_exits(loop_depth_, in_function);  // M2-2C §2.6
      line(ind() + "break;");
      break;
    }
    case StmtKind::Continue: {
      if (loop_depth_ == 0)
        fail("'continue' outside of a loop (line " + std::to_string(s.loc.line) + ")");
      emit_try_exits(loop_depth_, in_function);  // M2-2C §2.6
      line(ind() + "continue;");
      break;
    }
    case StmtKind::Pass:
      line(ind() + "// pass");  // no-op (§5)
      break;

    case StmtKind::Switch: {
      if (!s.expr) fail("switch without expression (line " + std::to_string(s.loc.line) + ")");
      if (s.cases.empty())
        fail("switch without case arms (line " + std::to_string(s.loc.line) + ")");
      // The scrutinee is evaluated exactly once, before any arm runs.
      // C1: a PROVEN int64 scrutinee plus compile-time int labels lowers to
      // a native C `switch` (one jump table) instead of the boxed
      // pt_truthy(pt_eq_f(...)) ladder — one evaluation either way (native
      // int text never needs pre-statements; every shape it admits folds to a
      // pure expression), same first-match-wins, same total `==`.
      std::string nat = native_int_text(*s.expr);
      if (!nat.empty() && native_switch_ok(s)) {
        emit_native_switch(s, nat, in_function);
        break;
      }
      std::string scrut = emit_expr(*s.expr, in_function);
      emit_switch_from(0, s, scrut, in_function);
      break;
    }

    case StmtKind::GlobalDecl: {
      // No runtime effect: the semantic pass resolved every write to a name
      // declared `global` to global storage (pt_g_…), so the declaration
      // itself needs no code. Kept as a comment for readability.
      std::string cmt = "/* global";
      for (const std::string& n : s.names) cmt += " " + n;
      line(ind() + cmt + " */");
      break;
    }

    case StmtKind::NonlocalDecl:
      // M2-2B §6: `nonlocal n` targets the nearest enclosing binding; the
      // semantic pass resolved every subsequent assignment through the
      // captured cell already, so the declaration itself emits nothing.
      break;

    case StmtKind::Func:
      // M2-2B §6.3.3: nested declarations are hoisted closures, created in
      // the enclosing function's prologue (emit_function); in main() and at
      // top level they are ordinary global functions. The statement itself
      // has no runtime effect.
      break;

    case StmtKind::Try:  // M2-2C §2.1/§2.5
      emit_try(s, in_function);
      break;

    case StmtKind::Raise: {  // M2-2C §2.3
      if (!s.expr)
        fail("raise without expression (line " + std::to_string(s.loc.line) + ")");
      std::string v = emit_expr(*s.expr, in_function);
      line(ind() + "pt_raise(" + v + ", " + std::to_string(s.loc.line) + ");");
      fell_through = false;  // pt_raise never returns (throw or exit)
      break;
    }

    case StmtKind::Class: {  // M2-2C §3.1: runs at its declaration point
      if (in_function)
        fail("class declaration inside a function (line " +
             std::to_string(s.loc.line) + ")");
      const std::string ln = std::to_string(s.loc.line);
      const std::string slot = "pt_g_" + s.name;  // a plain module global
      // Imported classes carry an internal name (__ptmN_Class); user-visible
      // type names and method displays use the original spelling.
      line(ind() + slot + " = pt_class_new(\"" + c_escape(display_ident(s.name)) +
           "\", " + ln + ");");
      for (const FuncDecl& m : s.class_methods) {
        // Methods are env-less global functions (classes are top-level
        // only) with a qualified c_name; the meta keeps the source name,
        // which is what bound-method displays show (§3.3).
        line(ind() + slot + " = pt_class_add_method(" + slot + ", \"" +
             c_escape(m.name) + "\", pt_fun_value(&" + meta_name(m) +
             ", NULL, 0u), " + ln + ");");
      }
      break;
    }

    default:
      fail("unknown statement kind");
  }
  // M2-2C §1.5: a statement's temps are dead at its end — clearing them
  // keeps a stale slot from pinning garbage past the statement (this is
  // what makes dropped cycles collectible at the next safe point). We clear
  // from pending_temps_ (temps created during THIS statement that a nested
  // statement did not already clear) and then truncate, so each temp is
  // cleared exactly once — by the statement that created it — instead of
  // being re-cleared by every enclosing statement (a redundant `pt_unset()`
  // store on the hot path of loop bodies). A nested statement that does NOT
  // fall through (ends in `return`) leaves its temps pending, so we clear
  // them here (still a single clear); `return` at this level unregisters
  // its frame instead and leaves nothing reachable.
  //
  // Perf (temp-clear DCE): INSIDE a loop body the clear is a dead store on
  // the hot back-edge path. Emission is A-normalized — every temp is
  // assigned before its next read — so the stale slot is never observed,
  // and a shadow-frame slot holding a stale pointer is only ever MARKED at
  // a collection (never swept-then-read): skipping the store pins at most
  // one extra iteration of garbage, and §7.3 collection TIMING is
  // unchanged. Six pt_unset calls per loop iteration (a cross-object call
  // each) drop out of every hot loop in the language.
  if (fell_through) {
    if (loop_depth_ == 0) {
      for (size_t i = temp_mark; i < pending_temps_.size(); ++i)
        line(ind() + pending_temps_[i] + " = pt_unset();");
    }
    pending_temps_.resize(temp_mark);
  }
}

void Emitter::emit_function(const FuncDecl& f) {
  if (f.c_name.empty()) fail("function '" + f.name + "' has no c_name");
  ephem_active_.clear();

  const FuncDecl* saved_fn = cur_func_;
  cur_func_ = &f;
  indent_ = 2;
  loop_depth_ = 0;
  try_stack_.clear();  // handlers never cross a function boundary (2C §2.5)

  std::vector<const FuncDecl*> nested = scan_nested_decls(f.body);
  const size_t nslots = n_slots(f);
  env_used_ = false;
  // C3 (task-5 A): bulk entry emission — same body, different ABI: raw
  // int64 params unboxed into their ordinary pt_l_* slots at entry, the
  // raw tail in pt_nlb_*/pt_nln_*, NO bound vector / PT_MISSING defaults /
  // pt_list_of vararg / gc_protect(bound) block, no env (top-level only —
  // analyze_bulk_varargs admitted no captures/cells/pins/kwargs).
  const bool bulk = (bulk_fn_ == &f);

  // M2-2B §13 uniform ABI: `bound` is the parameter slot vector (values or
  // PT_MISSING), `env` the captured-cell vector handed down from the
  // enclosing frame (NULL for top-level, env-less functions). C3: the bulk
  // entry swaps both for raw int64s.
  if (bulk) {
    std::string sig;
    for (size_t i = 0; i < f.params.size(); ++i)
      sig += std::string(i ? ", int64_t " : "int64_t ") + "pt_nl_" + f.params[i];
    if (f.params.size()) sig += ", ";
    sig += "const int64_t* pt_nlb_" + f.vararg + ", int64_t pt_nln_" +
           f.vararg;
    line("static pt_value " + f.c_name + "_b(" + sig + ") {");
  } else {
    line("static pt_value " + f.c_name + "(pt_value* bound, pt_cell** env) {");
  }

  if (nslots == 0 && !bulk) line(ind() + "(void)bound;");

  // (1) Non-boxed locals AND non-boxed params, declared unset at entry
  // (§10; 2C §1.3(2): every pt_value slot must exist before the shadow
  // frame is built — params materialize into these at (4)). Pinned
  // bindings (docs/reference/cross-scope-access.md §4) declare NO frame slot: their
  // storage is the program-lifetime static, declared at file scope.
  for (size_t j = 0; j < f.locals.size(); ++j) {
    if (j < f.local_is_cell.size() && f.local_is_cell[j]) continue;
    if (pinned_binding(f, f.locals[j])) continue;
    line(ind() + "pt_value pt_l_" + f.locals[j] + " = pt_unset();");
  }
  for (size_t i = 0; i < f.params.size(); ++i) {
    if (i < f.param_is_cell.size() && f.param_is_cell[i]) continue;
    if (pinned_binding(f, f.params[i])) continue;
    line(ind() + "pt_value pt_l_" + f.params[i] + " = pt_unset();");
  }
  if (f.has_vararg && !bulk && !f.vararg_is_cell && !pinned_binding(f, f.vararg))
    line(ind() + "pt_value pt_l_" + f.vararg + " = pt_unset();");
  if (f.has_kwarg && !f.kwarg_is_cell && !pinned_binding(f, f.kwarg))
    line(ind() + "pt_value pt_l_" + f.kwarg + " = pt_unset();");
  // (2) Heap cells for every boxed binding: captured params (incl. the
  // vararg/kwarg slots) and captured locals. All cells exist before any
  // closure below captures them; unset init keeps the M1 guard (§6.3.4).
  // A PINNED boxed binding does not allocate: it aliases the immortal
  // pinned cell created once at program entry (docs/reference/cross-scope-access.md §4),
  // so every invocation — and every closure from every invocation — shares
  // the one static slot.
  for (size_t i = 0; i < f.params.size(); ++i) {
    if (i < f.param_is_cell.size() && f.param_is_cell[i]) {
      if (pinned_binding(f, f.params[i]))
        line(ind() + "pt_cell* pt_cell_" + f.params[i] + " = " +
             pin_cvar(f, f.params[i], true) + ";");
      else
        line(ind() + "pt_cell* pt_cell_" + f.params[i] + " = pt_cell_alloc();");
    }
  }
  if (f.has_vararg && f.vararg_is_cell) {
    if (pinned_binding(f, f.vararg))
      line(ind() + "pt_cell* pt_cell_" + f.vararg + " = " +
           pin_cvar(f, f.vararg, true) + ";");
    else
      line(ind() + "pt_cell* pt_cell_" + f.vararg + " = pt_cell_alloc();");
  }
  if (f.has_kwarg && f.kwarg_is_cell) {
    if (pinned_binding(f, f.kwarg))
      line(ind() + "pt_cell* pt_cell_" + f.kwarg + " = " +
           pin_cvar(f, f.kwarg, true) + ";");
    else
      line(ind() + "pt_cell* pt_cell_" + f.kwarg + " = pt_cell_alloc();");
  }
  for (size_t j = 0; j < f.locals.size(); ++j) {
    if (j < f.local_is_cell.size() && f.local_is_cell[j]) {
      if (pinned_binding(f, f.locals[j]))
        line(ind() + "pt_cell* pt_cell_" + f.locals[j] + " = " +
             pin_cvar(f, f.locals[j], true) + ";");
      else
        line(ind() + "pt_cell* pt_cell_" + f.locals[j] + " = pt_cell_alloc();");
    }
  }
  // Post-2C native int64 slots: parallel native storage for the locals the
  // analysis proved int-only (the pt_value slots above stay untouched —
  // they carry the shadow frame / GC roots).
  {
    auto it = nativ_l_.find(&f);
    if (it != nativ_l_.end()) {
      for (const std::string& nl : it->second)
        line(ind() + "int64_t pt_nl_" + nl + " = 0; char pt_nl_ok_" + nl +
             " = 0;");
    }
  }
  // Native float64 slots (float twin of the int lane): same pair, double.
  {
    auto it = nativfl_.find(&f);
    if (it != nativfl_.end()) {
      for (const std::string& nl : it->second)
        line(ind() + "double pt_nfl_" + nl + " = 0.0; char pt_nfl_ok_" + nl +
             " = 0;");
    }
  }
  // Frame membership (2C §1.3(2)): every non-boxed local/param slot plus
  // every cell's value (a boxed binding lives only in its cell — once the
  // closure value that shares it is rebound, nothing else would root it).
  // Pinned bindings are excluded: their storage is program-lifetime static
  // (rooted at file scope, docs/reference/cross-scope-access.md §4), so a per-invocation
  // frame entry would only double-register them. Temps join through
  // decl_temp inside the region below.
  std::vector<std::string> base_slots;
  for (size_t j = 0; j < f.locals.size(); ++j) {
    if (pinned_binding(f, f.locals[j])) continue;
    base_slots.push_back((j < f.local_is_cell.size() && f.local_is_cell[j])
                             ? "pt_cell_" + f.locals[j] + "->v"
                             : "pt_l_" + f.locals[j]);
  }
  for (size_t i = 0; i < f.params.size(); ++i) {
    if (pinned_binding(f, f.params[i])) continue;
    base_slots.push_back((i < f.param_is_cell.size() && f.param_is_cell[i])
                             ? "pt_cell_" + f.params[i] + "->v"
                             : "pt_l_" + f.params[i]);
  }
  if (f.has_vararg && !bulk && !pinned_binding(f, f.vararg))
    base_slots.push_back(f.vararg_is_cell ? "pt_cell_" + f.vararg + "->v"
                                          : "pt_l_" + f.vararg);
  if (f.has_kwarg && !pinned_binding(f, f.kwarg))
    base_slots.push_back(f.kwarg_is_cell ? "pt_cell_" + f.kwarg + "->v"
                                         : "pt_l_" + f.kwarg);
  emit_framed(
      [&] {
        // Entry safe point (2C §1.3(4)/§1.4): `bound` holds the binder's
        // fresh vararg/kwarg values (dynamic) and any static-barr fresh
        // value only until slot materialization below copies them into
        // registered slots — protect across this region's collections.
        if (nslots > 0u && !bulk)
          line(ind() + "pt_gc_protect(bound, " + std::to_string(nslots) + "u);");
        line(ind() + "pt_gc_maybe();");
  // (3) Closure hoisting (§6.3.3): every nested declaration becomes a
  // closure at entry — the function object first (pt_fun_new allocates
  // its env storage, inlined in the object block for ≤ 4 captures — the
  // runtime's closure constructor), then the capture cells are written
  // straight into pt_fun_env(fn)[j] (referencing the cells above or our
  // own env slots, transitively, §6.4), then the value is stored into
  // the named local. Forward references therefore work; a declaration's
  // body never runs at its source position. Nothing between the object's
  // allocation and the fill stores can allocate (cell slots only), so no
  // collection can observe a partially filled env — and the slots read as
  // NULL (trace-safe) even if one did.
  for (const FuncDecl* nd : nested) {
    size_t li;
    if (!own_local_index(nd->name, li))
      fail("internal: nested function '" + nd->name +
           "' has no owning local (line " + std::to_string(nd->loc.line) + ")");
    bool boxed = (li < f.local_is_cell.size() && f.local_is_cell[li]);
    std::string target =
        boxed ? ("pt_cell_" + nd->name + "->v") : ("pt_l_" + nd->name);
    if (nd->captures.empty()) {
      line(ind() + target + " = pt_fun_value(&" + meta_name(*nd) +
           ", NULL, 0u);");
    } else {
      line(ind() + target + " = pt_fun_new(&" + meta_name(*nd) + ", " +
           std::to_string(nd->captures.size()) + "u);");
      for (size_t j = 0; j < nd->captures.size(); ++j) {
        const std::string& c = nd->captures[j];
        std::string cell;
        size_t pi, lci;
        long ci;
        if (own_param_index(c, pi)) {
          if (!slot_is_cell(f, pi))
            fail("internal: captured param '" + c + "' of '" + f.name +
                 "' is not boxed (line " + std::to_string(nd->loc.line) + ")");
          cell = "pt_cell_" + c;
        } else if (own_local_index(c, lci)) {
          if (!(lci < f.local_is_cell.size() && f.local_is_cell[lci]))
            fail("internal: captured local '" + c + "' of '" + f.name +
                 "' is not boxed (line " + std::to_string(nd->loc.line) + ")");
          cell = "pt_cell_" + c;
        } else if ((ci = capture_index(c)) >= 0) {
          cell = "env[" + std::to_string(ci) + "]";  // handed down (§6.4)
          env_used_ = true;
        } else {
          fail("internal: capture '" + c + "' of nested function '" +
               nd->name + "' unresolvable in '" + f.name + "' (line " +
               std::to_string(nd->loc.line) + ")");
        }
        line(ind() + "pt_fun_env(" + target + ")[" + std::to_string(j) +
             "] = " + cell + ";");
      }
    }
  }
  // (4) Slot materialization: each bound slot is copied into the
  // parameter's local storage (or its cell). PT_MISSING defaults are
  // evaluated HERE, in parameter order, fresh per call — a default may
  // reference globals, earlier params and locals, including hoisted
  // closures (§6/§13). The vararg/kwarg slots default to a fresh empty
  // list/map when the binder left them missing.
  for (size_t i = 0; i < f.params.size(); ++i) {
    const std::string& p = f.params[i];
    bool boxed = i < f.param_is_cell.size() && f.param_is_cell[i];
    // A pinned unboxed param materializes into its static slot; everything
    // else keeps the frame slot / cell (docs/reference/cross-scope-access.md §4).
    std::string val =
        boxed ? "pt_cell_" + p + "->v"
              : (pinned_binding(f, p) ? pin_value_text(f, p, false)
                                      : "pt_l_" + p);
    if (bulk) {
      // C3 bulk ABI: a raw int64 from the caller's pack — box ONCE into
      // the registered slot. Omitted params were folded at the call site
      // as int literals (bind_bulk_call_args), so no PT_MISSING path here.
      line(ind() + val + " = pt_int(pt_nl_" + p + ");");
      continue;
    }
    line(ind() + val + " = bound[" + std::to_string(i) + "];");
    if (i < f.defaults.size() && f.defaults[i]) {
      line(ind() + "if (" + val + ".tag == PT_MISSING) {");
      indent_ += 2;
      std::string dv = emit_expr(*f.defaults[i], /*in_function=*/true);
      line(ind() + val + " = " + dv + ";");
      indent_ -= 2;
      line(ind() + "}");
    }
  }
  if (f.has_vararg && !bulk) {
    bool boxed = f.vararg_is_cell;
    std::string val =
        boxed ? "pt_cell_" + f.vararg + "->v"
              : (pinned_binding(f, f.vararg)
                     ? pin_value_text(f, f.vararg, false)
                     : "pt_l_" + f.vararg);
    line(ind() + val + " = bound[" + std::to_string(f.params.size()) + "];");
    line(ind() + "if (" + val + ".tag == PT_MISSING) " + val + " = pt_list_new();");
  }
  if (f.has_kwarg) {
    bool boxed = f.kwarg_is_cell;
    std::string val =
        boxed ? "pt_cell_" + f.kwarg + "->v"
              : (pinned_binding(f, f.kwarg) ? pin_value_text(f, f.kwarg, false)
                                            : "pt_l_" + f.kwarg);
    line(ind() + val + " = bound[" +
         std::to_string(f.params.size() + (f.has_vararg ? 1u : 0u)) + "];");
    line(ind() + "if (" + val + ".tag == PT_MISSING) " + val + " = pt_map_new();");
  }
        // Everything in `bound` is now reachable from frame slots.
        if (nslots > 0u && !bulk)
          line(ind() + "pt_gc_unprotect(" + std::to_string(nslots) + "u);");

  // `((void)x);` for params/locals that are never read: keeps the generated
  // file warning-clean under -Wall -Wextra without changing §10 lines.
  // Reads inside default expressions count too (they read earlier params).
  // Boxed bindings always flow through their cell — no suppression needed.
  std::set<std::pair<VarRes, std::string>> reads;
  for (const Stmt& s : f.body) collect_var_reads(s, reads);
  for (size_t i = 0; i < f.defaults.size(); ++i)
    if (f.defaults[i]) collect_var_reads(*f.defaults[i], reads);
  for (size_t i = 0; i < f.params.size(); ++i) {
    if (i < f.param_is_cell.size() && f.param_is_cell[i]) continue;
    if (pinned_binding(f, f.params[i])) continue;  // no frame slot to suppress
    if (!reads.count(std::make_pair(VarRes::Param, f.params[i])))
      line(ind() + "((void)pt_l_" + f.params[i] + ");");
  }
  if (f.has_vararg && !bulk && !f.vararg_is_cell && !pinned_binding(f, f.vararg) &&
      !reads.count(std::make_pair(VarRes::Param, f.vararg)))
    line(ind() + "((void)pt_l_" + f.vararg + ");");
  if (f.has_kwarg && !f.kwarg_is_cell && !pinned_binding(f, f.kwarg) &&
      !reads.count(std::make_pair(VarRes::Param, f.kwarg)))
    line(ind() + "((void)pt_l_" + f.kwarg + ");");
  for (size_t j = 0; j < f.locals.size(); ++j) {
    if (j < f.local_is_cell.size() && f.local_is_cell[j]) continue;
    if (pinned_binding(f, f.locals[j])) continue;  // no frame slot to suppress
    if (!reads.count(std::make_pair(VarRes::Local, f.locals[j])))
      line(ind() + "((void)pt_l_" + f.locals[j] + ");");
  }

        emit_body(f.body, /*in_function=*/true);
      },
      /*frame_end=*/false, base_slots);
  if (!env_used_ && !bulk) line(ind() + "(void)env;");
  line(ind() + "pt_frame_end(&pt_fr);");  // implicit `return None` path (2C §2.5)
  line(ind() + "return pt_none();");  // implicit `return None` at end of body
  indent_ = 0;
  line("}");
  line("");
  cur_func_ = saved_fn;
}

// M2-2B §13 static call site: the callee's bound vector is filled here
// (positional/named/default/vararg/kwarg binding per §6 — semantic has
// verified the names and arity) and the call is a direct C call with
// env = NULL (top-level, env-less callee).
std::string Emitter::emit_static_call(const FuncDecl& f, const Expr& e,
                                      const std::vector<std::string>& a) {
  std::vector<std::string> slots = bound_slots_text(f, e, a, e.loc.line);
  std::string t = decl_temp();
  if (slots.empty()) {
    line(ind() + t + " = " + f.c_name + "(NULL, NULL);");
    return t;
  }
  std::string joined;
  for (size_t i = 0; i < slots.size(); ++i) {
    if (i) joined += ", ";
    joined += slots[i];
  }
  std::string barr = fresh_temp();
  line(ind() + "{ pt_value " + barr + "[] = { " + joined + " };");
  indent_ += 2;
  line(ind() + t + " = " + f.c_name + "(" + barr + ", NULL);");
  indent_ -= 2;
  line(ind() + "}");
  return t;
}

// ---- C3 (task-5 A): bulk call-site packing -------------------------------
// Materializes one admissible bulk call: every bound expression must spell
// raw int64 C text IN THIS CONTEXT (native_int_text mirrors int_shape
// exactly — same lanes, same twin/binder rules, same "" decline), each
// spelling is stored to a fresh int64 temp in strictly SOURCE order (the
// binder's increasing-param-index rule makes fixed-param order == source
// order, and the tail sits strictly after every fixed bind), the tail
// becomes one stack int64 array, and the call goes straight to the _b
// entry — no pt_value bound vector, no PT_MISSING slots, no pt_list_of,
// no call-side pt_values at all. Returns "" on ANY decline, so the caller
// falls through to today's boxed path byte-identically.
std::string Emitter::emit_bulk_call(const std::vector<const Expr*>& fixed,
                                    const std::vector<const Expr*>& tail,
                                    const FuncDecl& cf) {
  if (!cf.has_vararg || cf.has_kwarg) return "";
  std::vector<std::string> texts;
  texts.reserve(fixed.size() + tail.size());
  for (const Expr* x : fixed) {
    if (!x) return "";
    std::string t = native_int_text(*x);
    if (t.empty()) return "";
    texts.push_back(t);
  }
  for (const Expr* x : tail) {
    if (!x) return "";
    std::string t = native_int_text(*x);
    if (t.empty()) return "";
    texts.push_back(t);
  }
  const size_t nfixed = fixed.size();
  const size_t ntail = tail.size();
  std::vector<std::string> vars;
  vars.reserve(texts.size());
  for (const std::string& t : texts) {  // source order, each evaluated once
    std::string v = fresh_temp();
    line(ind() + "int64_t " + v + " = " + t + ";");
    vars.push_back(v);
  }
  std::string arr = "NULL";
  if (ntail > 0) {
    arr = fresh_temp();
    std::string elems;
    for (size_t i = nfixed; i < vars.size(); ++i) {
      if (i > nfixed) elems += ", ";
      elems += vars[i];
    }
    line(ind() + "int64_t " + arr + "[] = { " + elems + " };");
  }
  std::string joined;
  for (size_t i = 0; i < nfixed; ++i) {
    if (i) joined += ", ";
    joined += vars[i];
  }
  if (nfixed) joined += ", ";
  std::string t = decl_temp();
  line(ind() + t + " = " + cf.c_name + "_b(" + joined + arr + ", " +
       std::to_string(ntail) + "LL);");
  return t;
}

// M2-2B §13 dynamic call: the callee expression (annotated by semantic)
// loads the function value first, then every argument value, then pt_call
// binds them at runtime — the binder raises the §11 residue (unexpected
// name, duplicate binding, missing required, too many positionals) with
// runtime wording. Named argument i has name argnames[i] and value
// posvals[npos + i].
std::string Emitter::emit_dynamic_call(const Expr& e, bool in_function) {
  if (!e.callee_expr)
    fail("dynamic call without callee expression (line " +
         std::to_string(e.loc.line) + ")");
  std::string fv = emit_expr(*e.callee_expr, in_function);
  size_t npos = 0;
  std::vector<std::string> args;
  args.reserve(e.args.size());
  for (size_t i = 0; i < e.args.size(); ++i) {
    const ExprPtr& a = e.args[i];
    if (!a) fail("call argument missing (line " + std::to_string(e.loc.line) + ")");
    args.push_back(emit_expr(*a, in_function));
    std::string n = i < e.arg_names.size() ? e.arg_names[i] : "";
    if (n.empty()) {
      if (npos != i)
        fail("internal: positional argument after named argument (line " +
             std::to_string(e.loc.line) + ")");
      ++npos;
    }
  }
  size_t nnamed = args.size() - npos;
  std::string ln = std::to_string(e.loc.line);
  std::string t = decl_temp();
  if (args.empty()) {
    line(ind() + t + " = pt_call(" + fv +
         ", NULL, 0u, NULL, 0u, " + ln + ");");
    return t;
  }
  std::string joined;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) joined += ", ";
    joined += args[i];
  }
  std::string pv = fresh_temp();
  line(ind() + "{ pt_value " + pv + "[] = { " + joined + " };");
  indent_ += 2;
  if (nnamed == 0) {
    line(ind() + t + " = pt_call(" + fv + ", " + pv + ", " +
         std::to_string(npos) + "u, NULL, 0u, " + ln + ");");
  } else {
    std::string an = fresh_temp();
    std::string names;
    for (size_t i = npos; i < args.size(); ++i) {
      if (i > npos) names += ", ";
      names += "\"" + c_escape(e.arg_names[i]) + "\"";
    }
    line(ind() + "const char* const " + an + "[] = { " + names + " };");
    line(ind() + t + " = pt_call(" + fv + ", " + pv + ", " +
         std::to_string(npos) + "u, " + an + ", " + std::to_string(nnamed) +
         "u, " + ln + ");");
  }
  indent_ -= 2;
  line(ind() + "}");
  return t;
}

void Emitter::emit_main_block(const MainDecl& m) {
  ephem_active_.clear();
  indent_ = 2;
  loop_depth_ = 0;
  try_stack_.clear();
  line("static void pt_main_block(void) {");
  // Main-only native globals are automatic locals here: same identifiers as
  // the file-scope pair would have used, so every read/write/name-table
  // spelling is unchanged; the static declaration is skipped above. Globals
  // are zero-initialized, so the local starts at the same value (the
  // used-before-assignment flag still reports unassigned reads identically).
  for (const std::string& g : nativ_g_) {
    if (!local_g_.count(g)) continue;
    line(ind() + "int64_t pt_ng_" + g + " = 0;");
    if (!noflag_g_.count(g)) line(ind() + "char pt_ng_ok_" + g + " = 0;");
  }
  for (const std::string& g : nativfg_) {
    if (!local_g_.count(g)) continue;
    line(ind() + "double pt_nfg_" + g + " = 0.0;");
    if (!noflag_g_.count(g)) line(ind() + "char pt_nfg_ok_" + g + " = 0;");
  }
  std::vector<std::string> main_base;
  for (const std::string& g : prog_.globals) {
    if (!local_g_.count(g) || nativ_g_.count(g) || nativfg_.count(g))
      continue;
    if (is_builtin_exception_class(g)) continue;
    line(ind() + "pt_value pt_g_" + g + " = pt_unset();");
    main_base.push_back("pt_g_" + g);
  }
  emit_framed(
      [&] {
        line(ind() + "pt_gc_maybe();");
        emit_body(m.body, /*in_function=*/false);
      },
      /*frame_end=*/true, main_base);
  indent_ = 0;
  line("}");
  line("");
}

// M2-2B §3/§12 destructuring. `src` is the name of a temp holding the
// already-evaluated RHS value (evaluated exactly once by the caller).
//
// Count rule (§12 wording): a pattern without a rest leaf needs exactly as
// many elements as leaves (pt_unpack_check raises
// `not enough values to unpack (expected N, got M)` for both directions).
// A rest leaf `*name` at position p of an L-leaf pattern needs at least
// L-1 elements (the rest may be empty) and never errors on extra elements;
// the too-few check reports "expected L" (the pattern's leaf count), which
// keeps N = number of targets uniformly, while pt_unpack_rest takes the
// elements between the fixed leaves around it.
void Emitter::emit_destructure(const AssignTarget& pat, const std::string& src,
                               bool in_function) {
  switch (pat.kind) {
    case AssignTarget::Kind::Name: {
      if (pat.res == VarRes::Unresolved)
        fail("unresolved assignment target '" + pat.name + "' (line " +
             std::to_string(pat.loc.line) + ")");
      std::string storage = storage_for(pat.name, pat.res, in_function, pat.loc.line);
      line(ind() + storage + " = " + src + ";");
      break;
    }

    case AssignTarget::Kind::Rest: {
      // Reached only as a whole-target `*name = expr`: src is the RHS value
      // itself, so the rest list spans everything from 0. A rest leaf inside
      // a Tuple pattern is bound directly by the Tuple branch — the parent
      // already extracted its exact span.
      if (pat.res == VarRes::Unresolved)
        fail("unresolved assignment target '" + pat.name + "' (line " +
             std::to_string(pat.loc.line) + ")");
      std::string storage = storage_for(pat.name, pat.res, in_function, pat.loc.line);
      std::string ln = std::to_string(pat.loc.line);
      std::string n = fresh_temp();
      line(ind() + "size_t " + n + " = pt_unpack_len(" + src + ", " + ln + ");");
      line(ind() + storage + " = pt_unpack_rest(" + src + ", 0u, " + n + ", " +
           ln + ");");
      break;
    }

    case AssignTarget::Kind::VarQual: {  // docs/reference/cross-scope-access.md §5: a
                                         // qualified leaf stores into its
                                         // pinned slot (or module global)
      line(ind() + qual_value_text(pat) + " = " + src + ";");
      break;
    }

    case AssignTarget::Kind::Index: {
      // Index leaf: the element goes into an existing container. The
      // container/index expressions are evaluated here (each exactly once).
      std::string obj = emit_expr(*pat.object, in_function);
      std::string idx = emit_expr(*pat.index, in_function);
      line(ind() + "pt_index_set(" + obj + ", " + idx + ", " + src + ", " +
           std::to_string(pat.loc.line) + ");");
      break;
    }

    case AssignTarget::Kind::Attr: {  // M2-2C §3.2: attribute leaf in a pattern
      if (!pat.object)
        fail("attribute target without object (line " +
             std::to_string(pat.loc.line) + ")");
      std::string obj = emit_expr(*pat.object, in_function);
      line(ind() + "pt_setattr_lit(" + obj + ", \"" + c_escape(pat.attr) + "\", " +
           src + ", " + std::to_string(pat.loc.line) + ");");
      break;
    }

    case AssignTarget::Kind::Tuple: {
      std::string ln = std::to_string(pat.loc.line);
      size_t L = pat.elems.size();
      if (L == 0) fail("empty destructuring pattern (line " + ln + ")");
      long rest_pos = -1;
      for (size_t j = 0; j < L; ++j) {
        if (pat.elems[j].kind == AssignTarget::Kind::Rest) {
          if (rest_pos != -1)
            fail("more than one rest target in one destructuring pattern (line " +
                 ln + ")");
          rest_pos = static_cast<long>(j);
        }
      }
      std::string n = fresh_temp();
      line(ind() + "size_t " + n + " = pt_unpack_len(" + src + ", " + ln + ");");
      if (rest_pos < 0) {
        line(ind() + "pt_unpack_check(" + n + ", " + std::to_string(L) + "u, " +
             ln + ");");
      } else if (L > 1) {
        // minimum fill: leaves before + leaves after the rest (rest ≥ empty)
        line(ind() + "if (" + n + " < " + std::to_string(L - 1) +
             "u) pt_unpack_check(" + n + ", " + std::to_string(L) + "u, " + ln +
             ");");
      }
      // Leaf j reads element j before the rest and elements n-(L-j) after it
      // (the last L-j elements), so the rest spans exactly the middle.
      for (size_t j = 0; j < L; ++j) {
        const AssignTarget& leaf = pat.elems[j];
        if (rest_pos >= 0 && static_cast<long>(j) == rest_pos) {
          // The rest spans [j, stop) where stop leaves room for the fixed
          // leaves after it: stop = n - (L-1-j) (== n for a trailing rest,
          // == n for the lone `*name` pattern). pt_unpack_rest clamps.
          std::string stop_expr;
          size_t after = L - 1 - static_cast<size_t>(rest_pos);
          stop_expr = (after == 0) ? n : n + " - " + std::to_string(after) + "u";
          std::string elem = decl_temp();
          line(ind() + elem + " = pt_unpack_rest(" + src + ", " +
               std::to_string(j) + "u, " + stop_expr + ", " + ln + ");");
          // The leaf is exactly this extracted rest list: bind it directly
          // (emit_destructure's Rest branch would re-slice).
          if (leaf.res == VarRes::Unresolved)
            fail("unresolved assignment target '" + leaf.name + "' (line " +
                 ln + ")");
          std::string storage = storage_for(leaf.name, leaf.res, in_function,
                                            leaf.loc.line);
          line(ind() + storage + " = " + elem + ";");
          continue;
        }
        std::string idx_expr;
        if (rest_pos < 0 || static_cast<long>(j) < rest_pos) {
          idx_expr = std::to_string(j) + "u";
        } else {
          idx_expr = n + " - " + std::to_string(L - j) + "u";
        }
        std::string elem = decl_temp();
        line(ind() + elem + " = pt_unpack_get(" + src + ", " +
             idx_expr + ", " + ln + ");");
        emit_destructure(leaf, elem, in_function);
      }
      break;
    }
  }
}

// M2-2B §13: the static pt_funmeta instance of `f` plus its slot-name
// array (ABI order: positional params, then *vararg, then **kwarg). The
// has_default bitmask covers the positional slots only; vararg/kwarg slots
// never carry defaults.
void Emitter::emit_funmeta(const FuncDecl& f) {
  if (f.c_name.empty()) fail("function '" + f.name + "' has no c_name");
  std::vector<std::string> slots = slot_names(f);
  const size_t nslots = slots.size();
  if (nslots > 64 && !f.defaults.empty())
    fail("function '" + f.name +
         "' has too many parameter slots for the has-default mask");
  size_t nreq = 0;
  while (nreq < f.params.size() &&
         !(nreq < f.defaults.size() && f.defaults[nreq]))
    ++nreq;
  unsigned long long mask = 0;
  for (size_t i = 0; i < f.params.size(); ++i)
    if (i < f.defaults.size() && f.defaults[i]) mask |= 1ull << i;
  std::string pn = "NULL";
  if (!slots.empty()) {
    pn = "pt_mpn_" + f.c_name;
    std::string items;
    for (size_t i = 0; i < slots.size(); ++i) {
      if (i) items += ", ";
      items += "\"" + c_escape(slots[i]) + "\"";
    }
    line("static const char* const " + pn + "[] = { " + items + " };");
  }
  line("static const pt_funmeta " + meta_name(f) + " PT_UNUSED = {\"" +
       c_escape(f.name) + "\", " + pn + ", " + std::to_string(nslots) + "u, " +
       std::to_string(nreq) + "u, " + std::to_string(mask) + "ull, " +
       (f.has_vararg ? "true" : "false") + ", " +
       (f.has_kwarg ? "true" : "false") + ", " + f.c_name + "};");
}

std::string Emitter::run() {
  const MainDecl* main_block = find_main();
  if (prog_.has_main != (main_block != nullptr))
    fail("Program.has_main does not match the main item");
  std::vector<const FuncDecl*> funcs = all_functions();
  // C2 (task-5): JOINT monotone fixpoint — the lane/twin/float/DA analyses
  // are computed under the CURRENT try-elision split, then every elided try
  // and every no-raise callee is re-proven against exactly that shipped
  // state (never_raise mirrors emission family-by-family). Both state sets
  // start at their optimistic top and only ever SHRINK, so the loop
  // terminates; the round that removes nothing is provably consistent —
  // i.e. the state emission will use, conservative by one slot (§0.3).
  scan_main_uses();
  analyze_int_list_reads();
  init_try_elision();
  for (bool more = true; more;) {
    // C3 (task-5 A): vararg bulk-entry registry re-seeded from THIS round's
    // elided-try state (monotone: retraction can only shrink it), ahead of
    // every analysis that reads it — inside the joint fixpoint so Route-2's
    // bulk-lane facts settle with the SAME shipped state, not a stale one.
    analyze_bulk_varargs();
    // int <-> float conversions make the two lane fixpoints mutually
    // dependent: int(<float lane>) can join the int lane, while
    // float(<int lane>) and the mixed int64->double arithmetic rules feed
    // the float lane from the int lane. Start with an empty float lane and
    // alternate to a joint fixpoint. Each analysis is re-run from its full
    // candidate top and only gains slots when the other lane grew, so the
    // pair of sets is monotone and the loop terminates. Programs without
    // conversions converge in the first round (the second pass repeats the
    // identical result).
    nativfg_.clear();
    nativfl_.clear();
    // Seed local_g_ (main-only boxed candidates) before the int-list proof;
    // the post-lane call below re-derives it with the final native set.
    analyze_promotable_globals();
    analyze_native_ints();    // native int64 slots + int-lane twins (joint
                              // fixpoint; has_own_try sees the elided split)
    analyze_native_floats();  // float64 lanes (final int lane for mixed
                              // int64->double ops + float(int64))
    for (;;) {
      std::set<std::string> ig = nativ_g_;
      std::map<const FuncDecl*, std::set<std::string>> il = nativ_l_;
      std::set<std::string> fg = nativfg_;
      std::map<const FuncDecl*, std::set<std::string>> fl = nativfl_;
      analyze_native_ints();   // may now admit int(<proven float lane>)
      analyze_native_floats(); // re-check under the grown int lane
      bool il_changed = analyze_int_lists();
      if (ig == nativ_g_ && il == nativ_l_ && fg == nativfg_ &&
          fl == nativfl_ && !il_changed)
        break;
    }
    analyze_promotable_globals();  // main-only natives -> pt_main_block locals
    analyze_definite_assign();  // elide used-flag for DA slots
    // C3 Route-2 (task-5 B/4b1): bulk-body lanes + _b int-return facts,
    // under the round's lane/twin/DA state (removal-only, same discipline).
    analyze_bulk_lanes();
    more = retract_try_failures();
  }
  analyze_guard_elim();  // Phase 2.1: elide pt_guard for provably-bound slots
  analyze_native_fvecs(); // SIMD fusion: fvec/ivec-proven slots (uses both)
  analyze_text_cats();  // amortized `s = s + chunk`: in-place append proof
  analyze_method_inlines();  // LAST: plan provable method inlines (see its
                             // block comment; conservative placeholder now)
  // Final int-list facts under the shipped lane/DA/local_g_ state.
  analyze_promotable_globals();
  analyze_int_lists();
  analyze_ephemeral_slices();

  line("#include \"pentest_rt.h\"");
  line("");

  // Globals: zero-initialized static storage = tag unset (§10).
  for (const std::string& g : prog_.globals) line("static pt_value pt_g_" + g + ";");
  // Post-2C native globals: parallel native pair (set iteration is
  // deterministic). The pt_value slot above stays — it is a GC root.
  for (const std::string& g : nativ_g_) {
    if (local_g_.count(g)) continue;  // automatic local in pt_main_block
    line("static int64_t pt_ng_" + g + " = 0; static char pt_ng_ok_" + g +
         " = 0;");
  }
  // Native float64 globals (float twin of the int lane): same pair, double.
  for (const std::string& g : nativfg_) {
    if (local_g_.count(g)) continue;  // automatic local in pt_main_block
    line("static double pt_nfg_" + g + " = 0.0; static char pt_nfg_ok_" + g +
         " = 0;");
  }
  if (!prog_.globals.empty()) line("");
  // Cross-scope access (docs/reference/cross-scope-access.md §4): pinned slots. A plain
  // pin is a static cell struct whose .v holds the value (rooted through
  // the vector below); a pinned CELL is a static pointer to the immortal
  // heap cell created once at program entry (rooted by the collector's
  // pinned-cell table — closure env slots must reference registered GC
  // blocks, never static memory).
  for (const PinSlot& p : prog_.pins) {
    if (p.cell)
      line("static pt_cell* " + p.cvar + ";");
    else
      line("static pt_cell " + p.cvar + ";");
  }
  if (!prog_.pins.empty()) line("");

  // Forward declarations of ALL functions (forward/mutual recursion). The
  // uniform §13 ABI: pt_value f(pt_value* bound, pt_cell** env).
  for (const FuncDecl* f : funcs) {
    if (f->c_name.empty()) fail("function '" + f->name + "' has no c_name");
    std::string d = "static pt_value " + f->c_name +
                    "(pt_value* bound, pt_cell** env) PT_UNUSED;";
    line(d);  // suppresses -Wunused-function for never-called fns
  }
  // Post-2C perf: int-lane twins (int64 params -> int64 result) beside
  // the dynamic entry points — same PT_UNUSED treatment.
  for (const FuncDecl* f : funcs) {
    if (!nativ_fn_decls_.count(f)) continue;
    std::string sig;
    for (size_t i = 0; i < f->params.size(); ++i)
      sig += (i ? ", int64_t" : "int64_t");
    line("static int64_t " + f->c_name + "_i(" + sig + ") PT_UNUSED;");
  }
  // C3 (task-5 A): bulk vararg entries (raw fixed int64s + int64 tail)
  // beside the dynamic entry points — same PT_UNUSED treatment.
  for (const FuncDecl* f : funcs) {
    if (!bulk_ok_.count(f)) continue;
    std::string sig;
    for (size_t i = 0; i < f->params.size(); ++i)
      sig += (i ? ", int64_t" : "int64_t");
    if (f->params.size()) sig += ", const int64_t*, int64_t";
    else sig += "const int64_t*, int64_t";
    line("static pt_value " + f->c_name + "_b(" + sig + ") PT_UNUSED;");
  }
  if (!funcs.empty()) line("");

  // M2-2B §13: one static pt_funmeta per function, carrying the display
  // name, slot names in ABI order, required count, has-default mask and
  // the entry point — the data behind function values and pt_call.
  for (const FuncDecl* f : funcs) emit_funmeta(*f);
  if (!funcs.empty()) line("");

  // Definitions, declaration order.
  for (const FuncDecl* f : funcs) emit_function(*f);
  // Post-2C perf: int-lane twins after their dynamic counterparts (the
  // dynamic bodies are emitted with the original lane state — a twin's
  // param lanes exist only inside emit_function_native).
  for (const FuncDecl* f : funcs)
    if (nativ_fn_decls_.count(f)) emit_function_native(*f);
  // C3 (task-5 A): bulk vararg entries after their dynamic counterparts
  // (same reason twins wait: the dynamic bodies keep the original state).
  for (const FuncDecl* f : funcs) {
    if (!bulk_ok_.count(f)) continue;
    bulk_fn_ = f;
    emit_function(*f);
    bulk_fn_ = nullptr;
  }

  // The main(): block, if any.
  if (main_block) emit_main_block(*main_block);

  // M2-2C §1.3(1): GC root vector — one address per program global, plus
  // one per plain pinned slot (docs/reference/cross-scope-access.md §4: a pinned
  // binding is program-lifetime storage, so its value is rooted exactly
  // like a global's) — emitted at file scope so it outlives every
  // collection. Pinned CELL slots are rooted by the collector's
  // pinned-cell table instead (pt_cell_pinned).
  size_t n_roots = prog_.globals.size();
  for (const PinSlot& p : prog_.pins)
    if (!p.cell) ++n_roots;
  if (n_roots > 0) {
    std::string arr = "static pt_value* const pt_g_roots[] = {";
    bool first = true;
    for (const std::string& g : prog_.globals) {
      if (!first) arr += ",";
      first = false;
      arr += " &pt_g_" + g;
    }
    for (const PinSlot& p : prog_.pins)
      if (!p.cell) {
        if (!first) arr += ",";
        first = false;
        arr += " &" + p.cvar + ".v";
      }
    arr += " };";
    line(arr);
    line("");
  }

  // Entry point + top-level flow in source order.
  line("int main(int argc, char** argv) {");
  indent_ = 2;
  loop_depth_ = 0;
  try_stack_.clear();
  if (n_roots == 0) {
    line(ind() + "pt_init(argc, argv, \"" + c_escape(prog_.file_path) +
         "\", NULL, 0u);");
  } else {
    line(ind() + "pt_init(argc, argv, \"" + c_escape(prog_.file_path) +
         "\", pt_g_roots, " + std::to_string(n_roots) + "u);");
  }
  // M2-2C §2.2: bind the builtin exception-class singletons into their
  // injected global slots (semantic injects only the names actually read;
  // pt_init above created the singletons, and the runtime's internal
  // table roots them from here on).
  for (const std::string& g : prog_.globals)
    if (is_builtin_exception_class(g))
      line(ind() + "pt_g_" + g + " = pt_exc_class(\"" + g + "\");");
  emit_framed(
      [&] {
        // Cross-scope access (docs/reference/cross-scope-access.md §4): create the
        // immortal cells of pinned+captured bindings once, before any
        // function that aliases them can run.
        for (const PinSlot& p : prog_.pins)
          if (p.cell)
            line(ind() + p.cvar + " = pt_cell_pinned();");
        for (const TopLevel& t : prog_.items) {
          switch (t.kind) {
            case TopKind::Stmt:
              line(ind() + "pt_gc_maybe();");  // statement boundary (2C §1.4)
              emit_stmt(t.stmt, /*in_function=*/false);
              break;
            case TopKind::Func:
              break;  // definition already emitted above
            case TopKind::Main:
              line(ind() + "pt_gc_maybe();");
              line(ind() + "pt_main_block();");
              break;
            default:
              fail("unknown top-level item kind");
          }
        }
      },
      /*frame_end=*/true);
  line(ind() + "return 0;");
  indent_ = 0;
  line("}");
  return out_;
}

}  // namespace

bool generate_c(const Program& program, const std::string& out_c_path) {
  try {
    Emitter em(program);
    std::string source = em.run();
    std::ofstream out(out_c_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      fprintf(stderr, "%s: error: cannot open '%s' for writing\n",
              program.file_path.empty() ? "<input>" : program.file_path.c_str(),
              out_c_path.c_str());
      return false;
    }
    out << source;
    out.close();
    if (!out) {
      fprintf(stderr, "%s: error: failed writing '%s'\n",
              program.file_path.empty() ? "<input>" : program.file_path.c_str(),
              out_c_path.c_str());
      return false;
    }
    return true;
  } catch (const std::exception& ex) {
    fprintf(stderr, "%s: error: codegen: %s\n",
            program.file_path.empty() ? "<input>" : program.file_path.c_str(),
            ex.what());
    return false;
  }
}

}  // namespace pentest
