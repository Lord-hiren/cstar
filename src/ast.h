// ast.h — Pentest M2 (Stage 2B) abstract syntax tree.
// Shared contract: frontend and backend both code against it.
// Stage 2B extends 2A with: collections (list/map/set/tuple literals), Text/
// Bytes literals, indexing + slicing, method calls, comma-join, destructuring
// targets, function values/indirect calls, default/named/variadic params,
// nested functions + closures (capture analysis), and global/nonlocal.
// Stage 2C extends 2B with try/except/finally and raise (exceptions, §2)
// and with classes, instance attributes and object identity (§3).
// Normative: docs/contracts/m2b-semantics.md (M1 + M2-2A remain normative elsewhere;
// M2-2C: docs/contracts/m2c-semantics.md §2–§3).
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace cstar {

struct SourceLoc {
  int line = 1;
  int col = 1;
  // Canonical source path for diagnostics. Empty means "the current file"
  // (single-file programs and synthesized locations keep the old behavior).
  std::string file;
};

enum class ExprKind {
  IntLit, FloatLit, StrLit, BytesLit, BoolLit, NoneLit,
  Var,
  Unary,      // "-", "~", "not"
  Call,       // callee is a bare name (fast path) or an indirect callee_expr
  Binary,     // M1 ops + "**" "&" "|" "^" "<<" ">>" "in"
  Logical,    // "and" / "or"
  Ternary,    // lhs if <cond> else third
  // M2-2B:
  ListLit,    // elems
  MapLit,     // keys + elems (parallel vectors)
  SetLit,     // elems
  TupleLit,   // elems
  Index,      // object [ index ]        (object = lhs, index = rhs)
  Slice,      // object [ start : stop : step ] (lhs = object; see slice_* )
  MethodCall, // object . name ( args )  (lhs = object)
  Attr,       // M2-2C §3.2: bare attribute read object.name (attr = name;
              // MethodCall stays the `object.name(args)` form)
  Join,       // comma-join: elems joined with a space (assignment/return/expr-stmt)
  // Cross-scope variable access (docs/reference/cross-scope-access.md):
  // `variable.<qualifier> <name>` — a contextual 4-token atom (no new
  // keyword). attr = the qualifier ("global", "parent", or a sibling
  // function name), name = the variable. Semantic fills res/is_cell and
  // pin/pin_cell (the static slot holding the binding, empty for globals).
  VarQual,
};

// How a Var (or a Name Assign target) resolves. Set by the semantic pass.
enum class VarRes {
  Unresolved,
  Param,
  Local,    // function-local (storage: cell if is_cell else cs_l_<name>)
  Global,   // module global (storage: cs_g_<name>)
  Function, // M2-2B: reading a function name yields a function value (2B §6)
};

struct Expr {
  ExprKind kind;
  SourceLoc loc;

  // Literals
  long long ival = 0;   // IntLit
  double fval = 0.0;    // FloatLit
  std::string sval;     // StrLit/BytesLit — decoded bytes (may contain NUL)
  bool bval = false;    // BoolLit

  // Var
  std::string name;     // Var
  VarRes res = VarRes::Unresolved;  // Var — semantic
  bool is_cell = false; // M2-2B: Var resolving to a captured (boxed) local

  // VarQual (docs/reference/cross-scope-access.md): cross-scope access — attr = the
  // qualifier, name = the variable; pin = the C name of the static slot
  // backing the accessed binding ("" for variable.global, which targets
  // module storage directly), pin_cell = the slot is a pinned cell pointer.
  std::string pin;
  bool pin_cell = false;

  // Unary: op "-" | "~" | "not".
  // Binary: op "+" "-" "*" "/" "%" "==" "!=" "<" "<=" ">" ">=" (M1) or
  //         "**" "&" "|" "^" "<<" ">>" (2A) or "in" (v0.3 membership) or
  //         "is" "is not" (2C §3.4 — identity; `is not` negates; same
  //         precedence level and chaining rules as "==").
  // Logical: op "and" | "or".
  std::string op;
  std::unique_ptr<Expr> lhs;    // Unary: operand. Binary/Logical: left.
                                // Ternary: condition. Index/MethodCall/Attr:
                                // object. Slice: object.
  std::unique_ptr<Expr> rhs;    // Binary/Logical: right. Ternary: then.
                                // Index: index.
  std::unique_ptr<Expr> third;  // Ternary: else-value. Slice: stop (nullable).
  std::unique_ptr<Expr> fourth; // Slice: step (nullable).

  // Call — bare-name fast path (callee set) OR dynamic callee. INVARIANT:
  // when the semantic pass marks a call dynamic (nested callee, variable,
  // indirect expression), BOTH fields may be set — `callee` keeps the bare
  // name for diagnostics and `callee_expr` carries the annotated load. Codegen
  // must branch on `callee_expr != null` FIRST. Builtins (print/range/len/...)
  // are always bare-name, callee_expr null, and call-only. arg_names[i] names
  // the i-th arg ("" = positional; named args come last).
  std::string callee;
  std::unique_ptr<Expr> callee_expr;   // M2-2B dynamic callee (authoritative)
  bool is_builtin_print = false;       // semantic
  bool is_builtin_range = false;       // semantic
  bool is_builtin = false;             // semantic: any reserved builtin callee
  std::string builtin_name;            // semantic: which builtin (len/str/...)
  std::vector<std::string> arg_names;  // parallel to args ("" = positional)
  // semantic (SPEC §5.11.5 Python builtins): runtime slot per argument and
  // the total slot count — the fixed cs_builtin_* signature. Positional
  // args fill slots left-to-right, named args their name's slot; unfilled
  // optional slots arrive as cs_missing(). Empty arg_slots = the legacy
  // hand-written layout (the original 15 builtins + print/range).
  std::vector<int> arg_slots;
  size_t arg_nslots = 0;
  std::vector<std::unique_ptr<Expr>> args;

  // Collections / Join / MethodCall
  std::vector<std::unique_ptr<Expr>> elems;  // List/Set/Tuple/Join/MethodCall-args
  std::vector<std::unique_ptr<Expr>> keys;   // MapLit keys (parallel to elems)
  std::string method;                        // MethodCall method name
  std::string attr;                          // Attr attribute name (M2-2C §3.2)
  bool has_start = true, has_stop = true, has_step = false;  // Slice bounds

  // MethodCall arg names ("" = positional), parallel to elems.
  std::vector<std::string> elem_names;
};

using ExprPtr = std::unique_ptr<Expr>;

// ---- M2-2B assignment targets ---------------------------------------------
// A statement LHS. Plain `x = ...` is Kind::Name (s.name/s.res mirror it so
// M1/2A code paths keep working). `xs[i] = ...` is Kind::Index.
// `obj.attr = ...` is Kind::Attr (M2-2C §3.2: object expr + attribute name;
// nothing binds). A destructuring
// `a, b = xs` / `a, *rest = xs` / `(a,b), c = xs` is Kind::Tuple whose elems
// are Name/Index/Attr/Rest leaves (and nested Tuples).
struct AssignTarget {
  enum class Kind { Name, Index, Attr, Tuple, Rest, VarQual };
  Kind kind = Kind::Name;
  SourceLoc loc;

  std::string name;                 // Name, Rest, VarQual (the variable)
  VarRes res = VarRes::Unresolved;  // Name, Rest, VarQual — semantic
  bool is_cell = false;             // Name, Rest — captured local (boxed)

  // VarQual (docs/reference/cross-scope-access.md): attr = the qualifier ("global",
  // "parent" or a sibling function name); pin/pin_cell select the static
  // slot, mirroring the ExprKind::VarQual fields ("" for variable.global).
  std::string pin;
  bool pin_cell = false;

  ExprPtr object;                   // Index: the container expr; Attr: the
                                    // object expr (M2-2C §3.2)
  ExprPtr index;                    // Index: the index expr
  std::string attr;                 // Attr: attribute name (M2-2C §3.2);
                                    // VarQual: the qualifier

  std::vector<AssignTarget> elems;  // Tuple: sub-targets
};

enum class StmtKind {
  Assign,      // target (see below) [op] expr; op "" or the 11 compound forms
  ExprStmt,    // expr (may be a Join)
  Return,
  If, While, For, Break, Continue, Pass, Switch,   // 2A (unchanged)
  GlobalDecl,    // names[] — `global a, b`
  NonlocalDecl, // names[] — `nonlocal a, b`
  Func,          // M2-2B: nested function declaration (holds func below)
  // M2-2C §2 exceptions:
  Try,           // body + arms (+ finally_body when has_finally)
  Raise,         // expr — `raise expr` (§2.3; no bare `raise`)
  // M2-2C §3.1 classes:
  Class,         // `class NAME:` (block-form only) — name = class name,
                 // class_methods = the method declarations (methods only)
};

struct Stmt;
struct FuncDecl;  // defined below; a Kind::Func statement holds one

// One `case` arm of a switch. The parser guarantees non-wildcard labels are
// constant literals (M2-2A §2); the semantic pass checks duplicates under the
// total `==` (M2-2A §8).
struct CaseArm {
  bool is_wildcard = false;  // `case *:`
  ExprKind label_kind = ExprKind::IntLit;  // IntLit/FloatLit/StrLit/BoolLit/NoneLit
  long long ival = 0;
  double fval = 0.0;
  std::string sval;          // StrLit — decoded bytes
  bool bval = false;
  SourceLoc loc;
  std::vector<Stmt> body;    // Stmt is incomplete here — legal C++17 vector
};

// One `except` arm of a try statement (M2-2C §2.1). cond null = bare
// `except:` (catch-all); name non-empty = bound by `as name`, scoped like
// other statement bindings (same mechanism as a for-loop target).
struct ExceptArm {
  SourceLoc loc;          // location of the 'except' keyword
  ExprPtr cond;           // null = bare `except:` (catch-all)
  std::string name;       // bound by `as name`; empty = not bound
  VarRes res = VarRes::Unresolved;  // the `as` binding — semantic
  bool is_cell = false;             // captured local (boxed) — semantic
  std::vector<Stmt> body;  // Stmt is incomplete here — legal C++17 vector
};

struct Stmt {
  StmtKind kind;
  SourceLoc loc;

  // Assign (plain name fast path — mirrors target for Kind::Name):
  std::string name;
  VarRes res = VarRes::Unresolved;
  bool is_cell = false;
  std::string op;                    // "" plain, or one of the 11 compound ops

  AssignTarget target;               // M2-2B: authoritative LHS (Assign)
  ExprPtr expr;

  // 2A control flow (unchanged semantics):
  std::vector<Stmt> body;
  std::vector<ExprPtr> conds;
  std::vector<std::vector<Stmt>> bodies;
  std::vector<Stmt> else_body;
  bool has_else = false;
  std::vector<CaseArm> cases;

  // M2-2C §2.1 try/except/finally: `body` is the try body, `arms` the
  // except clauses in source order, `finally_body` the finally suite
  // (present iff has_finally). Raise uses `expr` (like Return).
  std::vector<ExceptArm> arms;
  std::vector<Stmt> finally_body;
  bool has_finally = false;

  // global / nonlocal declarations (2B §6):
  std::vector<std::string> names;
  std::vector<VarRes> ress;
  std::vector<bool> cells;

  std::unique_ptr<FuncDecl> func;    // Kind::Func: nested function declaration

  // M2-2C §3.1 Kind::Class: class_methods holds the method declarations —
  // the parser moves every `function` statement of the class body here (a
  // complete FuncDecl like Kind::Func's, so the vector of values is legal);
  // `pass` and any statement the semantic pass rejects stay in `body`.
  std::vector<FuncDecl> class_methods;
};

struct FuncDecl {
  std::string name;
  std::vector<std::string> params;    // positional param names (in order)
  std::vector<ExprPtr> defaults;      // parallel to params; null = no default
  bool has_vararg = false;
  std::string vararg;                 // *name collects a list
  bool has_kwarg = false;
  std::string kwarg;                  // **name collects a map
  bool vararg_is_cell = false;        // *name captured by a nested function
  bool kwarg_is_cell = false;         // **name captured by a nested function
  std::vector<Stmt> body;             // flat statements; nested decls are
                                      // StmtKind::Func statements in here
  SourceLoc loc;

  // Semantic annotations:
  std::string c_name;                 // "cs_fn_" + name
  std::vector<std::string> locals;    // non-param local names, first-assign order
  std::vector<bool> local_is_cell;    // parallel to locals: boxed (captured)?
  std::vector<bool> param_is_cell;    // parallel to params: boxed (captured)?
  std::vector<std::string> captures;  // names captured from enclosing funcs (cells)
  std::vector<VarRes> capture_res;    // parallel: resolution of each capture
  std::vector<bool> capture_is_cell;  // parallel
  bool is_nested = false;             // declared inside a function/main
  bool is_method = false;             // M2-2C §3.1: a class method (semantic) —
                                      // method names are shared across classes,
                                      // so lookup goes through c_name only
  int min_args = 0;                   // required positional count (semantic)
  int max_args = -1;                  // -1 = unlimited (has_vararg); else exact

  // Cross-scope access (docs/reference/cross-scope-access.md): names of this function's
  // params/locals referenced through `variable.` elsewhere — those bindings
  // are "pinned" to program-lifetime static storage.
  std::vector<std::string> pinned;
};

// A nested function declaration is a Stmt with kind Func appearing in the
// enclosing function's body (or main()'s body); it has no runtime effect at
// its position — discovery hoists it. codegen emits each nested decl as a C
// function taking an environment (captured cells) parameter.

struct MainDecl {
  std::vector<Stmt> body;
  SourceLoc loc;
};

// ---- modules/imports (Stage 2D) -------------------------------------------
// `import a.b [as alias]` or `from a.b import name [as alias], ...`.
// Module specifiers are dotted identifiers resolved to source files by the
// linker (src/modules.cpp); imported declarations are renamed to internal
// module-unique identifiers, so `name` below is a source-level name.
struct ImportName {
  std::string name;
  std::string alias;  // empty = same name
  SourceLoc loc;
};

struct ImportDecl {
  enum class Kind { Module, From };
  Kind kind = Kind::Module;
  std::string module;                 // dotted specifier as written
  std::string alias;                  // Module: optional alias
  std::vector<ImportName> names;      // From: imported names
  SourceLoc loc;
};

enum class TopKind { Stmt, Func, Main, Import };

struct TopLevel {
  TopKind kind = TopKind::Stmt;
  Stmt stmt;
  std::unique_ptr<FuncDecl> func;
  std::unique_ptr<MainDecl> main;
  std::unique_ptr<ImportDecl> import;

  // Linker annotations (src/modules.cpp). `origin` is the canonical path of
  // the module that owns this declaration; empty = the entry file.
  // `imported` marks declarations that the reachability pass may drop.
  std::string origin;
  bool imported = false;
};

// One pinned binding (docs/reference/cross-scope-access.md §4): the owner function, the
// binding name, and the static C slot backing it. cell = the slot is an
// immortal pinned-cell pointer (the binding is ALSO closure-captured);
// otherwise the slot is a static cs_cell whose .v holds the value directly.
struct PinSlot {
  const FuncDecl* owner = nullptr;
  std::string name;
  std::string cvar;
  bool cell = false;
};

// C name of the static slot backing a pinned binding. Shared by the
// semantic pass (annotation) and codegen (declaration + access) — the two
// must stay in sync, so the spelling lives here, next to the AST.
inline std::string pin_cvar(const FuncDecl& owner, const std::string& name,
                            bool cell) {
  return (cell ? "cs_px_" : "cs_p_") + owner.c_name + "_" + name;
}

// Internal names of imported declarations are `__csm<module-id>_<name>`.
// Diagnostics and runtime-visible class names use the original spelling.
inline std::string display_ident(const std::string& name) {
  if (name.size() > 6 && name.compare(0, 5, "__csm") == 0) {
    std::size_t i = 5;
    while (i < name.size() && name[i] >= '0' && name[i] <= '9') ++i;
    if (i > 5 && i < name.size() && name[i] == '_')
      return name.substr(i + 1);
  }
  return name;
}

struct Program {
  std::string file_path;
  std::vector<TopLevel> items;

  std::vector<FuncDecl*> functions;  // ALL functions, declaration order (incl. nested)
  std::vector<std::string> globals;
  bool has_main = false;

  // Cross-scope access registries (docs/reference/cross-scope-access.md §4), filled by
  // the semantic pass: every pinned function binding and every global that
  // is referenced through `variable.global` (poisoned from the native-int
  // lane so the ordinary and qualified paths always share one storage).
  std::vector<PinSlot> pins;
  std::vector<std::string> pin_globals;
};

}  // namespace cstar
