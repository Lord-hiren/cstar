// semantic.cpp — Pentest M2 Stage 2B semantic pass
// (docs/contracts/m2b-semantics.md; M1 rules: docs/contracts/m1-semantics.md §6–§7; M2-2A
// rules: docs/contracts/m2a-semantics.md §7–§8).
//
// Extends the M1/2A multi-phase pass with the 2B function overhaul:
// default/named/variadic parameters, function values, nested functions with
// transitive capture analysis (boxing), and global/nonlocal. All errors are
// collected; M1/2A wording is preserved except where 2B §0/§11 approves a
// change (function names are readable as values; nested declarations are
// legal).
//
// v0.3 namespace split: a global VARIABLE and a global FUNCTION may share a
// name — a call is spelled `a(...)` while a read is the bare name `a`, so the
// two namespaces are syntactically disjoint. Reads resolve the variable first
// (globals_ before funcs_), calls resolve the function first (funcs_ before
// globals_), so `a = 10` + `a():` yields `print(a)` → 10 and `print(a())` →
// the function's result.
// Exceptions that keep a name exclusive: classes (constructed with call
// syntax — a same-named function would shadow the constructor) and nested
// declarations (their names are locals of the enclosing function — real
// storage collisions, 2B §6.3.2).
//
// Scope model (2B §6.3–6.5): one FuncScope per function, chained through
// enclosing functions. A nested `function` statement's name is a local of the
// enclosing function (hoisted closure); reads walk own scope → enclosing
// scopes (recording captures, transitively) → globals; binds stay in the own
// scope unless a `global`/`nonlocal` declaration targets them.
//
// Call-site annotations for codegen: bare-name calls to a known non-nested
// global function keep the M1 fast path (callee set, callee_expr null). Calls
// through a nested function's local value (or any other variable) get a Var
// `callee_expr` annotated with the load resolution (Local/Param/Global +
// is_cell) so codegen can evaluate the callee and use the dynamic pt_call
// binder. Static arity/named-argument checks run whenever the callee's
// declaration is known (2B §6.5).
//
// M2-2C §2 extends every statement-walking pass with try/except/finally
// (try body, except-arm bodies, finally body); an `as`-bound exception
// name follows the for-loop target binding rule (statement binding, no
// fresh sub-scope). `raise expr` resolves its expression only — whether
// the value is a raisable exception is a runtime concern (§2.3).
//
// M2-2C §3 adds classes: discovery registers each class name as an ordinary
// module global (a read before the declaration statement runs keeps the
// normal used-before-assignment guard) and each method as a phase-4 function
// body that never enters the bare-name function map — `move()` / `f = move`
// is an undefined function/variable, methods are reachable only through
// attribute access. Class bodies hold only method declarations and `pass`;
// classes are global-scope only (an in-function class is a compile error).
// Attribute reads/assignment resolve the object expression only (lookup is
// dynamic, §3.2); the `is` / `is not` identity operator resolves like any
// other Binary (§3.4).
#include "semantic.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "frontend.h"  // parse_source / semantic_check (contract interface)
#include "builtins.h"  // Python-parity builtin table (SPEC §5.11.5)

namespace pentest {
namespace {

// All reserved builtin names (M2-2B §8). Each mirrors `print`'s M1 reserved
// treatment: call-target-only, cannot be assigned / used as a parameter /
// declared as a function; a bare read as a value is an undefined variable.
// M2-2C §2.2 exception classes join the set for BINDING purposes only —
// reads resolve through is_builtin_exception_class (singleton values).
// Python-parity builtins join through py_builtin_reserved (SPEC §5.11.5):
// the implemented table, the always-fail stubs, and the unavailable
// eval/exec/compile trio (which error the moment they are CALLED).
bool is_reserved(const std::string& name) {
  return name == "print" || name == "range" || name == "len" ||
         name == "str" || name == "int" || name == "float" || name == "bool" ||
         name == "type" || name == "encode" || name == "decode" ||
         name == "set" || name == "collect" ||
         name == "input" || name == "time" || name == "clock" ||
          name == "http_post" || name == "http_get" || name == "url_encode" ||
         /* SIMD vector constructors/reductions (fvec/ivec) */
         name == "fvec" || name == "ivec" || name == "fvec_fill" ||
         name == "ivec_fill" || name == "dot" ||
         py_builtin_reserved(name) ||
         is_builtin_exception_class(name);
}

// Per-function resolution scope (2B §6.3–6.5). One per function (top-level,
// main-declared or nested); nested scopes chain to their enclosing function.
// `locals` includes nested-function names (which are locals holding closure
// values), assignment targets, destructuring leaves and loop variables,
// collected in first-assignment (source) order.
struct FuncScope {
  FuncDecl* decl = nullptr;
  FuncScope* parent = nullptr;  // enclosing function (null at global scope)

  std::unordered_set<std::string> params;   // positional + vararg/kwarg names
  std::unordered_set<std::string> locals;   // non-param locals (incl. nested)
  std::unordered_set<std::string> assigned; // locals from assignment targets /
                                            // loop vars (excludes nested names)
  std::unordered_map<std::string, FuncDecl*> nested;  // nested decl bindings
  std::unordered_set<std::string> global_names;   // `global n` in this func
  std::unordered_set<std::string> nonlocal_names; // `nonlocal n` in this func
  std::unordered_map<std::string, VarRes> nonlocal_res;  // owner res per name
  std::unordered_set<std::string> boxed;  // owner side: captured (cell) names

  std::vector<std::string> captures;  // ordered capture list (deterministic)
  std::unordered_set<std::string> capture_set;
  std::unordered_map<std::string, VarRes> capture_res_map;
};

class Checker {
 public:
  Checker(Program& prog, Diagnostics& diags) : prog_(prog), diags_(diags) {}

  bool run() {
    discover_functions();    // phase 1
    collect_globals();       // phase 2
    resolve_global_stmts();  // phase 3
    // Phase 4: every global function (incl. main()-declared statements and
    // duplicates, matching M1's body resolution) — nested functions are
    // processed recursively from their parent.
    for (FuncDecl* f : global_funcs_) {
      int saved = loop_depth_;
      loop_depth_ = 0;
      prepare_function(f, nullptr);
      loop_depth_ = saved;
    }
    // Phase 5: cross-scope access (docs/reference/cross-scope-access.md §4). Deferred
    // until every function's locals/params/boxing flags are final, so a
    // `variable.` reference may target any sibling/enclosing binding in any
    // declaration order (the language's forward-reference philosophy).
    resolve_qual_sites();
    return ok_;
  }

 private:
  // ---- phase 1: declaration discovery ------------------------------------
  void discover_functions() {
    for (TopLevel& item : prog_.items) {
      if (item.kind == TopKind::Func) {
        register_global_function(item.func.get());
      } else if (item.kind == TopKind::Stmt) {
        discover_stmt_functions(item.stmt);
      } else if (item.kind == TopKind::Main) {
        if (prog_.has_main) {
          error(item.main->loc,
                "only one 'main:' block is allowed per file");
        } else {
          prog_.has_main = true;
        }
        for (Stmt& s : item.main->body) discover_stmt_functions(s);
      }
    }
  }

  // A `function` statement at top level or in main()'s body is an ordinary
  // global function (2B §6.3.2 — M1 behavior). Descends into blocks but never
  // into function bodies (nested decls are discovered with their parent).
  void discover_stmt_functions(Stmt& s) {
    switch (s.kind) {
      case StmtKind::Func:
        register_global_function(s.func.get());
        break;
      case StmtKind::If:
        for (std::vector<Stmt>& body : s.bodies)
          for (Stmt& b : body) discover_stmt_functions(b);
        if (s.has_else)
          for (Stmt& b : s.else_body) discover_stmt_functions(b);
        break;
      case StmtKind::While:
      case StmtKind::For:
        for (Stmt& b : s.body) discover_stmt_functions(b);
        break;
      case StmtKind::Switch:
        for (CaseArm& arm : s.cases)
          for (Stmt& b : arm.body) discover_stmt_functions(b);
        break;
      case StmtKind::Try:  // M2-2C §2.1
        for (Stmt& b : s.body) discover_stmt_functions(b);
        for (ExceptArm& arm : s.arms)
          for (Stmt& b : arm.body) discover_stmt_functions(b);
        for (Stmt& b : s.finally_body) discover_stmt_functions(b);
        break;
      case StmtKind::Class:  // M2-2C §3.1: global-scope class declaration —
                             // registers its name and methods; the body
                             // itself holds no statements to descend into
        register_class(s);
        break;
      default:
        break;
    }
  }

  void register_global_function(FuncDecl* f) {
    f->c_name = next_c_name(f->name);
    if (is_reserved(f->name)) {
      error(f->loc, "cannot declare function with reserved name '" +
                        f->name + "'");
    } else if (funcs_.find(f->name) != funcs_.end()) {
      error(f->loc, "duplicate function '" + f->name + "'");
    } else {
      funcs_[f->name] = f;
      prog_.functions.push_back(f);
    }
    check_function_params(f);
    global_funcs_.push_back(f);  // body is resolved even for duplicates (M1)
  }

  // M2-2C §3.1: `class NAME:` — registered like a global function, except
  // the name becomes an ordinary module global (the note_global_name
  // injection without the reserved-name path: a read before the declaration
  // statement runs gives the normal used-before-assignment runtime guard)
  // and the methods never enter the bare-name map funcs_. Method bodies are
  // still resolved in phase 4 through global_funcs_ (and prog_.functions,
  // first-wins like duplicate functions), but `move()` / `f = move` is an
  // undefined function/variable — methods are reachable only as attributes.
  void register_class(Stmt& s) {
    if (!classes_.insert(s.name).second)
      error(s.loc, "duplicate class '" + s.name + "'");
    else
      note_global(s.name);
    // §3.1: the body may hold only function declarations (moved into
    // class_methods by the parser) and `pass` — anything else, including a
    // nested `class`, is a compile error at the statement's location.
    for (const Stmt& b : s.body)
      if (b.kind != StmtKind::Pass)
        error(b.loc, "only function declarations are allowed in a class body");
    std::unordered_set<std::string> seen;
    for (FuncDecl& m : s.class_methods) {
      m.is_method = true;
      m.c_name = next_c_name(s.name + "_" + m.name);  // pt_fn_<class>_<method>
      if (!seen.insert(m.name).second) {
        error(m.loc, "duplicate method '" + m.name + "' in class '" +
                         s.name + "'");
      } else {
        prog_.functions.push_back(&m);
      }
      check_function_params(&m);
      global_funcs_.push_back(&m);  // body resolved in phase 4 (M1 pattern)
    }
  }

  // Unique C name for every function (nested functions may repeat names in
  // sibling scopes).
  std::string next_c_name(const std::string& name) {
    std::string base = "pt_fn_" + name;
    std::string cand = base;
    int n = 2;
    while (!c_names_.insert(cand).second)
      cand = base + "_" + std::to_string(n++);
    return cand;
  }

  // Parameter validation + min_args/max_args (M1 wording; 2B §6/§11).
  void check_function_params(FuncDecl* f) {
    std::unordered_set<std::string> seen;
    bool seen_default = false;
    for (size_t i = 0; i < f->params.size(); ++i) {
      const std::string& p = f->params[i];
      bool has_def = i < f->defaults.size() && f->defaults[i] != nullptr;
      if (is_reserved(p))
        error(f->loc, "cannot use reserved name '" + p + "' as parameter");
      if (!seen.insert(p).second)
        error(f->loc, "duplicate parameter '" + p + "' in function '" +
                          f->name + "'");
      if (has_def) {
        seen_default = true;
      } else if (seen_default) {
        error(f->loc,
              "parameter with default must come after required parameters");
      }
    }
    if (f->has_vararg) {
      if (is_reserved(f->vararg))
        error(f->loc, "cannot use reserved name '" + f->vararg +
                          "' as parameter");
      if (!seen.insert(f->vararg).second)
        error(f->loc, "duplicate parameter '" + f->vararg + "' in function '" +
                          f->name + "'");
    }
    if (f->has_kwarg) {
      if (is_reserved(f->kwarg))
        error(f->loc, "cannot use reserved name '" + f->kwarg +
                          "' as parameter");
      if (!seen.insert(f->kwarg).second)
        error(f->loc, "duplicate parameter '" + f->kwarg + "' in function '" +
                          f->name + "'");
    }
    int min_args = 0;
    for (size_t i = 0; i < f->params.size(); ++i)
      if (!(i < f->defaults.size() && f->defaults[i])) ++min_args;
    f->min_args = min_args;
    f->max_args = f->has_vararg ? -1 : static_cast<int>(f->params.size());
  }

  // ---- phase 2: globals (top level + main block) -------------------------
  void collect_globals() {
    for (TopLevel& item : prog_.items) {
      if (item.kind == TopKind::Stmt) {
        collect_global_stmt(item.stmt);
      } else if (item.kind == TopKind::Main) {
        for (Stmt& s : item.main->body) collect_global_stmt(s);
      }
    }
  }

  // Recursively notes every assignment target (incl. destructuring leaves)
  // and for-loop variable of a global-scope statement (through if/while/for/
  // switch nesting) as a global in first-assignment (source) order. Loop
  // variables leak (M2-2A §7). `function` statements are not variable
  // bindings (they registered as global functions in phase 1).
  void collect_global_stmt(Stmt& s) {
    switch (s.kind) {
      case StmtKind::Assign:
        collect_global_target(s.target, /*loop_target=*/false);
        break;
      case StmtKind::For:
        // The loop variable is bound before the body runs; the iterable is
        // an expression (cannot assign), so note the target first.
        note_global_name(s.name, s.loc, /*loop_target=*/true);
        for (Stmt& b : s.body) collect_global_stmt(b);
        break;
      case StmtKind::While:
        for (Stmt& b : s.body) collect_global_stmt(b);
        break;
      case StmtKind::If:
        for (std::vector<Stmt>& body : s.bodies)
          for (Stmt& b : body) collect_global_stmt(b);
        if (s.has_else)
          for (Stmt& b : s.else_body) collect_global_stmt(b);
        break;
      case StmtKind::Switch:
        for (CaseArm& arm : s.cases)
          for (Stmt& b : arm.body) collect_global_stmt(b);
        break;
      case StmtKind::Try:  // M2-2C §2.1: an `as` binding registers like a
                           // for-loop target (statement binding)
        for (Stmt& b : s.body) collect_global_stmt(b);
        for (ExceptArm& arm : s.arms) {
          if (!arm.name.empty())
            note_global_name(arm.name, arm.loc, /*loop_target=*/false);
          for (Stmt& b : arm.body) collect_global_stmt(b);
        }
        for (Stmt& b : s.finally_body) collect_global_stmt(b);
        break;
      case StmtKind::Class:  // M2-2C §3.1: the name was injected in phase 1;
                             // class bodies bind nothing (methods are function
                             // scopes). Unlike plain variables, a class keeps
                             // its name exclusive to functions (v0.3 §7):
                             // classes are constructed with call syntax, so a
                             // same-named function would shadow the
                             // constructor — every function is registered by
                             // now (phase 1)
        if (funcs_.find(s.name) != funcs_.end())
          error(s.loc, "name '" + s.name +
                           "' is already declared as a function");
        break;
      default:
        break;
    }
  }

  void collect_global_target(const AssignTarget& t, bool loop_target) {
    switch (t.kind) {
      case AssignTarget::Kind::Name:
      case AssignTarget::Kind::Rest:
        note_global_name(t.name, t.loc, loop_target);
        break;
      case AssignTarget::Kind::Index:
        // `xs[i] = v`: the object/index are uses, not bindings; they resolve
        // as expressions in phase 3.
        break;
      case AssignTarget::Kind::Attr:  // M2-2C §3.2: `obj.attr = v` — the
                                      // object is a use, not a binding
        break;
      case AssignTarget::Kind::VarQual:  // docs/reference/cross-scope-access.md: writes an
                                         // existing binding, never binds
        break;
      case AssignTarget::Kind::Tuple:
        for (const AssignTarget& e : t.elems)
          collect_global_target(e, /*loop_target=*/false);
        break;
    }
  }

  // Top-level / main-scope binding. A `function` statement is not a variable
  // binding (it registered as a global function in phase 1) — only assignments
  // (plain and destructuring, including loop targets) create globals here. A
  // nested function's name is a local of its enclosing function (2B §6.3.2) —
  // never a global.
  //
  // Namespace split (v0.3): a global VARIABLE may share a name with a global
  // FUNCTION — usage decides which namespace a site names: a bare read `a`
  // resolves in the variable namespace (globals_ checked before funcs_), a
  // call `a ->` in the function namespace (funcs_ checked before globals_).
  // Only the reserved-name check still rejects a binding here.
  void note_global_name(const std::string& name, SourceLoc loc,
                        bool loop_target) {
    if (is_reserved(name)) {
      if (loop_target) {
        error(loc, "cannot use reserved name '" + name +
                       "' as loop variable");
      } else {
        error(loc, "cannot assign to reserved name '" + name + "'");
      }
      return;
    }
    note_global(name);
  }

  void note_global(const std::string& name) {
    if (globals_.insert(name).second) prog_.globals.push_back(name);
  }

  // ---- phase 3: top-level and main-block statements ----------------------
  void resolve_global_stmts() {
    for (TopLevel& item : prog_.items) {
      if (item.kind == TopKind::Stmt) {
        resolve_stmt(item.stmt, /*sc=*/nullptr, /*in_main=*/false);
      } else if (item.kind == TopKind::Main) {
        for (Stmt& s : item.main->body)
          resolve_stmt(s, /*sc=*/nullptr, /*in_main=*/true);
      }
    }
  }

  // ---- phase 4: function bodies (top-level + nested, recursive) ----------
  //
  // Pipeline per function: collect locals → pre-scan global/nonlocal decls
  // (validating nonlocal bindings) → placement check → resolve defaults
  // (params become visible in order) → process nested declarations (capture
  // analysis happens inside, before any enclosing statement is resolved so
  // boxing flags are final) → resolve body statements → finalize annotations.
  void prepare_function(FuncDecl* f, FuncScope* parent) {
    FuncScope sc;
    sc.decl = f;
    sc.parent = parent;

    std::unordered_set<std::string> full_params(f->params.begin(),
                                                f->params.end());
    if (f->has_vararg) full_params.insert(f->vararg);
    if (f->has_kwarg) full_params.insert(f->kwarg);

    f->locals.clear();
    collect_local_stmts(f->body, full_params, sc);
    scan_decls(f->body, sc);
    drop_declared_locals(f, sc);
    check_decl_placement(f->body, sc);

    // Defaults resolve in the function's own scope with parameters visible
    // in order: a default may reference earlier params, locals (incl. nested
    // function names), enclosing-scope captures and globals (2B §6/§13).
    for (size_t i = 0; i < f->params.size(); ++i) {
      if (i < f->defaults.size() && f->defaults[i])
        resolve_expr(*f->defaults[i], &sc);
      sc.params.insert(f->params[i]);
    }
    if (f->has_vararg) sc.params.insert(f->vararg);
    if (f->has_kwarg) sc.params.insert(f->kwarg);

    process_nested_decls(f->body, sc);
    for (Stmt& s : f->body) resolve_stmt(s, &sc, /*in_main=*/false);
    finalize_function(*f, sc);
  }

  // Collects local names (assignment targets incl. destructuring leaves,
  // for-loop variables, nested-function names) in first-assignment order
  // through all statement nesting — but never into nested function bodies
  // (they own their own scopes).
  void collect_local_stmts(const std::vector<Stmt>& stmts,
                           const std::unordered_set<std::string>& params,
                           FuncScope& sc) {
    for (const Stmt& s : stmts) {
      switch (s.kind) {
        case StmtKind::Assign:
          collect_local_target(s.target, params, sc);
          break;
        case StmtKind::For:
          note_local_name(s.name, params, sc);
          collect_local_stmts(s.body, params, sc);
          break;
        case StmtKind::While:
          collect_local_stmts(s.body, params, sc);
          break;
        case StmtKind::If:
          for (const std::vector<Stmt>& body : s.bodies)
            collect_local_stmts(body, params, sc);
          if (s.has_else)
            collect_local_stmts(s.else_body, params, sc);
          break;
        case StmtKind::Switch:
          for (const CaseArm& arm : s.cases)
            collect_local_stmts(arm.body, params, sc);
          break;
        case StmtKind::Try:  // M2-2C §2.1: `as` names are statement
                             // bindings (first-assign order, like for-vars)
          collect_local_stmts(s.body, params, sc);
          for (const ExceptArm& arm : s.arms) {
            if (!arm.name.empty()) note_local_name(arm.name, params, sc);
            collect_local_stmts(arm.body, params, sc);
          }
          collect_local_stmts(s.finally_body, params, sc);
          break;
        case StmtKind::Func:
          // A nested declaration's name is a local of the enclosing function
          // (2B §6.3.2). Reserved/param conflicts are reported when the decl
          // is processed.
          note_local_name(s.func->name, params, sc, /*from_binding=*/false);
          break;
        case StmtKind::Class:  // M2-2C §3.1: this pass walks function bodies
                               // only — any class reached here is nested in
                               // a function (top level / main() register in
                               // phase 1 instead)
          error(s.loc, "classes must be declared at the top level");
          break;
        default:
          break;
      }
    }
  }

  void collect_local_target(const AssignTarget& t,
                            const std::unordered_set<std::string>& params,
                            FuncScope& sc) {
    switch (t.kind) {
      case AssignTarget::Kind::Name:
      case AssignTarget::Kind::Rest:
        note_local_name(t.name, params, sc);
        break;
      case AssignTarget::Kind::Index:
        break;  // uses, not bindings
      case AssignTarget::Kind::Attr:
        break;  // M2-2C §3.2: uses, not bindings
      case AssignTarget::Kind::VarQual:
        break;  // docs/reference/cross-scope-access.md: writes an existing binding
      case AssignTarget::Kind::Tuple:
        for (const AssignTarget& e : t.elems)
          collect_local_target(e, params, sc);
        break;
    }
  }

  void note_local_name(const std::string& name,
                       const std::unordered_set<std::string>& params,
                       FuncScope& sc, bool from_binding = true) {
    // Reserved names are reported during resolution; params shadow locals.
    if (is_reserved(name) || params.find(name) != params.end()) return;
    if (from_binding) sc.assigned.insert(name);
    if (sc.locals.insert(name).second) sc.decl->locals.push_back(name);
  }

  // Pre-scans `global`/`nonlocal` declarations (source order, not descending
  // into nested function bodies). Validates that every nonlocal name has an
  // enclosing binding (2B §11) and records the owner resolution.
  void scan_decls(const std::vector<Stmt>& stmts, FuncScope& sc) {
    for (const Stmt& s : stmts) {
      if (s.kind == StmtKind::GlobalDecl) {
        for (const std::string& n : s.names) sc.global_names.insert(n);
      } else if (s.kind == StmtKind::NonlocalDecl) {
        for (const std::string& n : s.names) {
          sc.nonlocal_names.insert(n);
          FuncScope* owner = find_owner(sc.parent, n);
          if (!owner) {
            error(s.loc, "no binding for nonlocal '" + n + "' found");
          } else {
            sc.nonlocal_res[n] =
                owner->params.count(n) ? VarRes::Param : VarRes::Local;
          }
        }
      } else if (s.kind != StmtKind::Func) {
        scan_decls_block(s, sc);
      }
    }
  }

  void scan_decls_block(const Stmt& s, FuncScope& sc) {
    switch (s.kind) {
      case StmtKind::If:
        for (const std::vector<Stmt>& body : s.bodies)
          scan_decls(body, sc);
        if (s.has_else) scan_decls(s.else_body, sc);
        break;
      case StmtKind::While:
      case StmtKind::For:
        scan_decls(s.body, sc);
        break;
      case StmtKind::Switch:
        for (const CaseArm& arm : s.cases) scan_decls(arm.body, sc);
        break;
      case StmtKind::Try:  // M2-2C §2.1
        scan_decls(s.body, sc);
        for (const ExceptArm& arm : s.arms) scan_decls(arm.body, sc);
        scan_decls(s.finally_body, sc);
        break;
      case StmtKind::Class:  // M2-2C §3.1: an in-function class (already
                             // rejected at local collection); method bodies
                             // and the class body scan as their own scopes
        break;
      default:
        break;
    }
  }

  // A `global n` declaration makes every reference to n in the function a
  // global reference; drop stale local entries (2B §6).
  // A `global`/`nonlocal` declaration redirects the name's bindings to
  // global storage / the enclosing cell (2B §6): the name must not remain
  // a local of this function, or reads would resolve to a fresh local and
  // shadow the declared target. Runs before nested processing, so boxing
  // is still clean.
  void drop_declared_locals(FuncDecl* f, FuncScope& sc) {
    if (sc.global_names.empty() && sc.nonlocal_names.empty()) return;
    std::vector<std::string> kept;
    for (const std::string& n : f->locals) {
      if (sc.global_names.count(n) || sc.nonlocal_names.count(n))
        sc.locals.erase(n);
      else
        kept.push_back(n);
    }
    f->locals = std::move(kept);
    sc.boxed.clear();  // no local can be boxed before nested processing
  }

  // 2B §6 placement rule: a global/nonlocal declaration must appear before
  // any binding or use of that name in the function body (walking statements
  // in source order, descending into blocks but not into nested function
  // bodies).
  void check_decl_placement(const std::vector<Stmt>& stmts, FuncScope& sc) {
    if (sc.global_names.empty() && sc.nonlocal_names.empty()) return;
    std::unordered_set<std::string> declared;
    std::unordered_set<std::string> reported;
    placement_walk(stmts, sc, declared, reported);
  }

  void placement_walk(const std::vector<Stmt>& stmts, FuncScope& sc,
                      std::unordered_set<std::string>& declared,
                      std::unordered_set<std::string>& reported) {
    for (const Stmt& s : stmts) placement_stmt(s, sc, declared, reported);
  }

  void placement_stmt(const Stmt& s, FuncScope& sc,
                      std::unordered_set<std::string>& declared,
                      std::unordered_set<std::string>& reported) {
    switch (s.kind) {
      case StmtKind::GlobalDecl:
      case StmtKind::NonlocalDecl:
        for (const std::string& n : s.names) declared.insert(n);
        break;
      case StmtKind::Func:
        break;  // the nested body/defaults belong to the nested function
      case StmtKind::Assign:
        placement_target(s.target, s.loc, sc, declared, reported);
        if (s.expr) placement_expr(*s.expr, sc, declared, reported);
        break;
      case StmtKind::ExprStmt:
      case StmtKind::Return:
        if (s.expr) placement_expr(*s.expr, sc, declared, reported);
        break;
      case StmtKind::If:
        for (size_t i = 0; i < s.conds.size(); ++i) {
          if (s.conds[i]) placement_expr(*s.conds[i], sc, declared, reported);
          if (i < s.bodies.size())
            placement_walk(s.bodies[i], sc, declared, reported);
        }
        if (s.has_else) placement_walk(s.else_body, sc, declared, reported);
        break;
      case StmtKind::While:
        if (s.expr) placement_expr(*s.expr, sc, declared, reported);
        placement_walk(s.body, sc, declared, reported);
        break;
      case StmtKind::For:
        if (s.expr) placement_expr(*s.expr, sc, declared, reported);
        placement_name(s.name, s.loc, sc, declared, reported);
        placement_walk(s.body, sc, declared, reported);
        break;
      case StmtKind::Switch:
        if (s.expr) placement_expr(*s.expr, sc, declared, reported);
        for (const CaseArm& arm : s.cases)
          placement_walk(arm.body, sc, declared, reported);
        break;
      case StmtKind::Raise:  // M2-2C §2.3
        if (s.expr) placement_expr(*s.expr, sc, declared, reported);
        break;
      case StmtKind::Try:  // M2-2C §2.1 — source order: body, arms, finally
        placement_walk(s.body, sc, declared, reported);
        for (const ExceptArm& arm : s.arms) {
          if (arm.cond) placement_expr(*arm.cond, sc, declared, reported);
          if (!arm.name.empty())
            placement_name(arm.name, arm.loc, sc, declared, reported);
          placement_walk(arm.body, sc, declared, reported);
        }
        placement_walk(s.finally_body, sc, declared, reported);
        break;
      case StmtKind::Class:  // M2-2C §3.1: methods are their own function
                             // scopes (their placement runs in phase 4); an
                             // in-function class was already rejected
        break;
      default:
        break;
    }
  }

  void placement_expr(const Expr& e, FuncScope& sc,
                      std::unordered_set<std::string>& declared,
                      std::unordered_set<std::string>& reported) {
    switch (e.kind) {
      case ExprKind::Var:
        placement_name(e.name, e.loc, sc, declared, reported);
        break;
      case ExprKind::Unary:
        if (e.lhs) placement_expr(*e.lhs, sc, declared, reported);
        break;
      case ExprKind::Binary:
      case ExprKind::Logical:
        if (e.lhs) placement_expr(*e.lhs, sc, declared, reported);
        if (e.rhs) placement_expr(*e.rhs, sc, declared, reported);
        break;
      case ExprKind::Ternary:
        if (e.lhs) placement_expr(*e.lhs, sc, declared, reported);
        if (e.rhs) placement_expr(*e.rhs, sc, declared, reported);
        if (e.third) placement_expr(*e.third, sc, declared, reported);
        break;
      case ExprKind::Call:
        if (!e.callee.empty())
          placement_name(e.callee, e.loc, sc, declared, reported);
        if (e.callee_expr)
          placement_expr(*e.callee_expr, sc, declared, reported);
        for (const auto& a : e.args)
          if (a) placement_expr(*a, sc, declared, reported);
        break;
      case ExprKind::ListLit:
      case ExprKind::SetLit:
      case ExprKind::TupleLit:
      case ExprKind::Join:
        for (const auto& el : e.elems)
          if (el) placement_expr(*el, sc, declared, reported);
        break;
      case ExprKind::MapLit:
        for (const auto& k : e.keys)
          if (k) placement_expr(*k, sc, declared, reported);
        for (const auto& el : e.elems)
          if (el) placement_expr(*el, sc, declared, reported);
        break;
      case ExprKind::Index:
        if (e.lhs) placement_expr(*e.lhs, sc, declared, reported);
        if (e.rhs) placement_expr(*e.rhs, sc, declared, reported);
        break;
      case ExprKind::Slice:
        if (e.lhs) placement_expr(*e.lhs, sc, declared, reported);
        if (e.rhs) placement_expr(*e.rhs, sc, declared, reported);
        if (e.third) placement_expr(*e.third, sc, declared, reported);
        if (e.fourth) placement_expr(*e.fourth, sc, declared, reported);
        break;
      case ExprKind::MethodCall:
        if (e.lhs) placement_expr(*e.lhs, sc, declared, reported);
        for (const auto& el : e.elems)
          if (el) placement_expr(*el, sc, declared, reported);
        break;
      case ExprKind::Attr:  // M2-2C §3.2: bare attribute read — only the
                            // object expression uses names
        if (e.lhs) placement_expr(*e.lhs, sc, declared, reported);
        break;
      default:
        break;  // literals
    }
  }

  void placement_target(const AssignTarget& t, SourceLoc loc, FuncScope& sc,
                        std::unordered_set<std::string>& declared,
                        std::unordered_set<std::string>& reported) {
    switch (t.kind) {
      case AssignTarget::Kind::Name:
      case AssignTarget::Kind::Rest:
        placement_name(t.name, t.loc, sc, declared, reported);
        break;
      case AssignTarget::Kind::Index:
        if (t.object) placement_expr(*t.object, sc, declared, reported);
        if (t.index) placement_expr(*t.index, sc, declared, reported);
        break;
      case AssignTarget::Kind::Attr:  // M2-2C §3.2: only the object uses names
        if (t.object) placement_expr(*t.object, sc, declared, reported);
        break;
      case AssignTarget::Kind::VarQual:  // qualified target: no plain names
        break;
      case AssignTarget::Kind::Tuple:
        for (const AssignTarget& e : t.elems)
          placement_target(e, e.loc, sc, declared, reported);
        break;
    }
    (void)loc;
  }

  void placement_name(const std::string& n, SourceLoc loc, FuncScope& sc,
                      std::unordered_set<std::string>& declared,
                      std::unordered_set<std::string>& reported) {
    if (declared.count(n)) return;
    bool is_global = sc.global_names.count(n) != 0;
    if (!is_global && !sc.nonlocal_names.count(n)) return;
    if (!reported.insert(n).second) return;
    error(loc, "name '" + n + "' is used before '" +
                   std::string(is_global ? "global" : "nonlocal") +
                   "' declaration");
  }

  // ---- nested function declarations (2B §6.3–6.5) ------------------------
  void process_nested_decls(std::vector<Stmt>& stmts, FuncScope& sc) {
    for (Stmt& s : stmts) {
      switch (s.kind) {
        case StmtKind::Func:
          process_nested_function(*s.func, &sc);
          break;
        case StmtKind::If:
          for (std::vector<Stmt>& body : s.bodies)
            process_nested_decls(body, sc);
          if (s.has_else) process_nested_decls(s.else_body, sc);
          break;
        case StmtKind::While:
        case StmtKind::For:
          process_nested_decls(s.body, sc);
          break;
        case StmtKind::Switch:
          for (CaseArm& arm : s.cases) process_nested_decls(arm.body, sc);
          break;
        case StmtKind::Try:  // M2-2C §2.1
          process_nested_decls(s.body, sc);
          for (ExceptArm& arm : s.arms) process_nested_decls(arm.body, sc);
          process_nested_decls(s.finally_body, sc);
          break;
        case StmtKind::Class:  // M2-2C §3.1: methods were registered in
                               // discovery — they are global functions, not
                               // nested declarations of the enclosing scope
          break;
        default:
          break;
      }
    }
  }

  void process_nested_function(FuncDecl& nd, FuncScope* parent) {
    nd.is_nested = true;
    nd.c_name = next_c_name(nd.name);
    // Name conflicts (2B §6.3.2): with a parameter or an existing local →
    // M1 function-conflict wording; a second decl of the same name → M1
    // duplicate-function wording.
    if (is_reserved(nd.name)) {
      error(nd.loc, "cannot declare function with reserved name '" +
                        nd.name + "'");
    } else if (parent->params.count(nd.name)) {
      error(nd.loc, "name '" + nd.name +
                        "' is already declared as a function");
    } else if (parent->nested.count(nd.name)) {
      error(nd.loc, "duplicate function '" + nd.name + "'");
    } else if (parent->assigned.count(nd.name)) {
      error(nd.loc, "name '" + nd.name +
                        "' is already declared as a function");
    }
    if (!parent->params.count(nd.name) && !parent->locals.count(nd.name)) {
      parent->locals.insert(nd.name);
      parent->decl->locals.push_back(nd.name);
    }
    parent->nested.emplace(nd.name, &nd);  // first decl wins on duplicates
    prog_.functions.push_back(&nd);
    check_function_params(&nd);
    int saved = loop_depth_;
    loop_depth_ = 0;
    prepare_function(&nd, parent);
    loop_depth_ = saved;
  }

  void finalize_function(FuncDecl& f, FuncScope& sc) {
    f.param_is_cell.assign(f.params.size(), false);
    for (size_t i = 0; i < f.params.size(); ++i)
      f.param_is_cell[i] = sc.boxed.count(f.params[i]) != 0;
    f.vararg_is_cell = f.has_vararg && sc.boxed.count(f.vararg) != 0;
    f.kwarg_is_cell = f.has_kwarg && sc.boxed.count(f.kwarg) != 0;
    f.local_is_cell.assign(f.locals.size(), false);
    for (size_t i = 0; i < f.locals.size(); ++i)
      f.local_is_cell[i] = sc.boxed.count(f.locals[i]) != 0;
    f.captures = sc.captures;
    f.capture_res.clear();
    f.capture_is_cell.clear();
    for (const std::string& n : sc.captures) {
      auto it = sc.capture_res_map.find(n);
      f.capture_res.push_back(
          it != sc.capture_res_map.end() ? it->second : VarRes::Local);
      f.capture_is_cell.push_back(true);  // captures are heap cells
    }
  }

  // ---- scope-chain helpers -----------------------------------------------
  FuncScope* find_owner(FuncScope* from, const std::string& name) {
    for (FuncScope* s = from; s; s = s->parent)
      if (s->params.count(name) || s->locals.count(name)) return s;
    return nullptr;
  }

  // Records a capture of `name` (owned by an enclosing function) in `sc`,
  // marks the owner's binding boxed, and propagates the capture to the
  // intermediate frames (up to but excluding the owner) so the cell is
  // handed down through the environment chain (2B §6.4 transitive capture).
  void add_capture(FuncScope* sc, const std::string& name, VarRes res) {
    if (!sc->capture_set.insert(name).second) return;
    sc->captures.push_back(name);
    sc->capture_res_map[name] = res;
    FuncScope* owner = find_owner(sc->parent, name);
    if (!owner) return;
    owner->boxed.insert(name);
    if (owner != sc->parent) add_capture(sc->parent, name, res);
  }

  // ---- statement / expression resolution ---------------------------------
  void resolve_stmt(Stmt& s, FuncScope* sc, bool in_main) {
    switch (s.kind) {
      case StmtKind::Assign:
        resolve_assign_target(s.target, sc);
        if (s.target.kind == AssignTarget::Kind::Name) {
          s.name = s.target.name;  // mirror (M1/2A fast paths)
          s.res = s.target.res;
          s.is_cell = s.target.is_cell;
        }
        if (s.expr) resolve_expr(*s.expr, sc);
        break;
      case StmtKind::Func:
        // Registered in discovery; nested bodies were processed before the
        // enclosing statements (hoisting, 2B §6.3.3).
        break;
      case StmtKind::GlobalDecl:
        s.ress.assign(s.names.size(), VarRes::Global);
        s.cells.assign(s.names.size(), false);
        break;
      case StmtKind::NonlocalDecl:
        s.ress.assign(s.names.size(), VarRes::Local);
        s.cells.assign(s.names.size(), false);
        if (!sc) {
          // global / main scope: no enclosing binding can exist (2B §11).
          for (size_t i = 0; i < s.names.size(); ++i)
            error(s.loc, "no binding for nonlocal '" + s.names[i] + "' found");
        } else {
          for (size_t i = 0; i < s.names.size(); ++i) {
            auto it = sc->nonlocal_res.find(s.names[i]);
            if (it != sc->nonlocal_res.end()) {
              s.ress[i] = it->second;
              s.cells[i] = true;
            }
          }
        }
        break;
      case StmtKind::ExprStmt:
        if (s.expr) resolve_expr(*s.expr, sc);
        break;
      case StmtKind::Return:
        if (!sc) {
          if (in_main)
            error(s.loc, "'return' is not allowed inside 'main:'");
          else
            error(s.loc, "'return' outside of a function");
        }
        if (s.expr) resolve_expr(*s.expr, sc);
        break;
      case StmtKind::If:
        for (size_t i = 0; i < s.conds.size(); ++i) {
          if (s.conds[i]) resolve_expr(*s.conds[i], sc);
          if (i < s.bodies.size())
            for (Stmt& b : s.bodies[i]) resolve_stmt(b, sc, in_main);
        }
        if (s.has_else) {
          for (Stmt& b : s.else_body) resolve_stmt(b, sc, in_main);
        }
        break;
      case StmtKind::While:
        if (s.expr) resolve_expr(*s.expr, sc);
        resolve_loop_body(s.body, sc, in_main);
        break;
      case StmtKind::For:
        if (s.expr) resolve_expr(*s.expr, sc);
        resolve_for_target(s, sc);
        resolve_loop_body(s.body, sc, in_main);
        break;
      case StmtKind::Break:
        if (loop_depth_ == 0)
          error(s.loc, "'break' outside of a loop");
        break;
      case StmtKind::Continue:
        if (loop_depth_ == 0)
          error(s.loc, "'continue' outside of a loop");
        break;
      case StmtKind::Pass:
        break;
      case StmtKind::Switch:
        if (s.expr) resolve_expr(*s.expr, sc);
        check_duplicate_cases(s);
        for (CaseArm& arm : s.cases)
          for (Stmt& b : arm.body) resolve_stmt(b, sc, in_main);
        break;
      case StmtKind::Try:
        // M2-2C §2.1/§2.4: try body, then arms in source order (cond as an
        // ordinary expression in the current scope, `as` binding like a
        // for-loop target), then the finally body. No fresh sub-scope — the
        // binding is a statement binding like any other. loop_depth_ is not
        // disturbed, so break/continue inside a try body keep the If/While/
        // For behavior (legal when lexically inside a loop).
        for (Stmt& b : s.body) resolve_stmt(b, sc, in_main);
        for (ExceptArm& arm : s.arms) {
          if (arm.cond) resolve_expr(*arm.cond, sc);
          if (!arm.name.empty()) resolve_except_binding(arm, sc);
          for (Stmt& b : arm.body) resolve_stmt(b, sc, in_main);
        }
        for (Stmt& b : s.finally_body) resolve_stmt(b, sc, in_main);
        break;
      case StmtKind::Raise:
        // M2-2C §2.3: only the expression is resolved; class-ness of the
        // raised value is a runtime concern.
        if (s.expr) resolve_expr(*s.expr, sc);
        break;
      case StmtKind::Class:
        // M2-2C §3.1: method bodies are wired by discovery and resolved
        // through the ordinary per-function machinery (phase 4 via
        // global_funcs_); the class name is an ordinary global by now, and
        // the class body holds only `pass` (content validated in phase 1).
        // An in-function class already errored during local collection.
        break;
    }
  }

  // Resolves a loop body with the loop nesting depth raised: `break` and
  // `continue` are legal inside (a switch does not count as a loop, so a
  // break inside a switch inside a loop still targets the loop).
  void resolve_loop_body(std::vector<Stmt>& body, FuncScope* sc,
                         bool in_main) {
    ++loop_depth_;
    for (Stmt& b : body) resolve_stmt(b, sc, in_main);
    --loop_depth_;
  }

  // The loop variable follows the assigned-anywhere rule: Local in a
  // function (Param if it shadows a parameter, unless a global/nonlocal
  // declaration targets it), Global at top level / in main(); reserved-name
  // errors were reported in phase 2 for the global scopes and are reported
  // by bind_name for functions. A loop target may share a function's name
  // (v0.3 namespace split — reads name the variable, calls the function).
  void resolve_for_target(Stmt& s, FuncScope* sc) {
    if (sc) {
      bool cell = false;
      s.res = bind_name(s.name, sc, s.loc, /*is_loop=*/true, cell);
      s.is_cell = cell;
    } else {
      s.res = VarRes::Global;
      s.is_cell = false;
    }
  }

  // The `as`-bound exception name (M2-2C §2.1 "scoped like other statement
  // bindings") follows the for-loop target rule exactly: Local in a
  // function (Param if it shadows a parameter, unless a global/nonlocal
  // declaration targets it), Global at top level / in main(); reserved-name
  // errors were reported in phase 2 for the global scopes and are reported
  // by bind_name for functions.
  void resolve_except_binding(ExceptArm& arm, FuncScope* sc) {
    if (sc) {
      bool cell = false;
      arm.res = bind_name(arm.name, sc, arm.loc, /*is_loop=*/false, cell);
      arm.is_cell = cell;
    } else {
      arm.res = VarRes::Global;
      arm.is_cell = false;
    }
  }

  // Resolves an assignment-target tree (2B §3/destructuring). Name and Rest
  // leaves bind (local in a function per the assigned-anywhere rule, global
  // at top level); Index targets walk their object/index as expressions and
  // bind nothing; Tuple patterns recurse.
  void resolve_assign_target(AssignTarget& t, FuncScope* sc) {
    switch (t.kind) {
      case AssignTarget::Kind::Name:
      case AssignTarget::Kind::Rest:
        if (sc) {
          bool cell = false;
          t.res = bind_name(t.name, sc, t.loc, /*is_loop=*/false, cell);
          t.is_cell = cell;
        } else {
          // Top level / main block: phase 2 already reported reserved-name
          // errors; every binding target is a global (a name shared with a
          // function is fine — v0.3 namespace split).
          t.res = VarRes::Global;
          t.is_cell = false;
        }
        break;
      case AssignTarget::Kind::Index:
        if (t.object) resolve_expr(*t.object, sc);
        if (t.index) resolve_expr(*t.index, sc);
        break;
      case AssignTarget::Kind::Attr:  // M2-2C §3.2: `obj.attr = v` — resolve
                                      // the object; the attribute binds
                                      // nothing (fields are dynamic)
        if (t.object) resolve_expr(*t.object, sc);
        break;
      case AssignTarget::Kind::VarQual:  // docs/reference/cross-scope-access.md §4: the
                                         // qualified variable itself is the
                                         // target; it never binds a new name
        record_qual_site(t, sc);
        break;
      case AssignTarget::Kind::Tuple:
        for (AssignTarget& e : t.elems) resolve_assign_target(e, sc);
        break;
    }
  }

  // Binds an assigned name in the current function (2B §6): a `global`
  // declaration targets global storage, `nonlocal` the enclosing binding
  // (through the cell), params/locals update in place, a name owned by an
  // enclosing function requires nonlocal, anything else becomes a new local
  // (assigned-anywhere rule; may shadow a top-level function — M1 behavior).
  VarRes bind_name(const std::string& name, FuncScope* sc, SourceLoc loc,
                   bool is_loop, bool& is_cell) {
    is_cell = false;
    if (is_reserved(name)) {
      error(loc, is_loop ? "cannot use reserved name '" + name +
                               "' as loop variable"
                         : "cannot assign to reserved name '" + name + "'");
      return VarRes::Local;
    }
    if (sc->global_names.count(name)) {
      // v0.3 namespace split: `global a` targets the variable namespace even
      // when a top-level function `a` also exists (calls `a ->` still reach
      // the function — resolve_call prefers funcs_).
      note_global(name);
      return VarRes::Global;
    }
    if (sc->nonlocal_names.count(name)) {
      auto it = sc->nonlocal_res.find(name);
      VarRes res = it != sc->nonlocal_res.end() ? it->second : VarRes::Local;
      is_cell = it != sc->nonlocal_res.end();
      add_capture(sc, name, res);  // writes go through the owner's cell
      return res;
    }
    if (sc->params.count(name)) {
      is_cell = sc->boxed.count(name) != 0;
      return VarRes::Param;
    }
    // A name owned by an enclosing function cannot be silently shadowed by
    // an assignment: rebinding requires `nonlocal` (2B §6). (Reads, in
    // contrast, resolve own locals first — a read-only shadow is legal.)
    if (find_owner(sc->parent, name)) {
      error(loc, "name '" + name +
                     "' is a local of an enclosing function; declare " +
                     "'nonlocal " + name + "' to rebind it");
      return VarRes::Local;
    }
    if (sc->locals.count(name)) {
      if (sc->nested.count(name))
        error(loc, "name '" + name +
                       "' is already declared as a function");  // 2B §6.3.2
      is_cell = sc->boxed.count(name) != 0;
      return VarRes::Local;
    }
    // New function-local (assigned-anywhere; usually pre-collected).
    sc->locals.insert(name);
    sc->assigned.insert(name);
    sc->decl->locals.push_back(name);
    return VarRes::Local;
  }

  // Reports `duplicate case value in switch` at every arm duplicating an
  // earlier arm (labels compared under M1's total `==`, M2-2A §8).
  void check_duplicate_cases(const Stmt& s) {
    for (size_t i = 0; i < s.cases.size(); ++i) {
      for (size_t j = 0; j < i; ++j) {
        if (case_labels_equal(s.cases[i], s.cases[j])) {
          error(s.cases[i].loc, "duplicate case value in switch");
          break;
        }
      }
    }
  }

  static bool case_labels_equal(const CaseArm& a, const CaseArm& b) {
    if (a.is_wildcard || b.is_wildcard)
      return a.is_wildcard && b.is_wildcard;
    if (a.label_kind != b.label_kind) {
      // Under the total `==` only int/float pairs compare across kinds.
      bool a_int = a.label_kind == ExprKind::IntLit;
      bool b_int = b.label_kind == ExprKind::IntLit;
      bool a_flt = a.label_kind == ExprKind::FloatLit;
      bool b_flt = b.label_kind == ExprKind::FloatLit;
      if (a_int && b_flt) return int_eq_float(a.ival, b.fval);
      if (a_flt && b_int) return int_eq_float(b.ival, a.fval);
      return false;  // bool/str/None never equal a different type
    }
    switch (a.label_kind) {
      case ExprKind::IntLit:
        return a.ival == b.ival;
      case ExprKind::FloatLit:
        return a.fval == b.fval;  // no NaN literal exists
      case ExprKind::StrLit:
        return a.sval == b.sval;  // decoded bytes
      case ExprKind::BoolLit:
        return a.bval == b.bval;
      case ExprKind::NoneLit:
        return true;
      default:
        return false;
    }
  }

  // Exact int64/double equality (no epsilon): d must be integral, within
  // int64 range, and round-trip exactly.
  static bool int_eq_float(long long i, double d) {
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
      return false;  // NaN or outside int64 range
    long long di = static_cast<long long>(d);
    return di == i && static_cast<double>(di) == d;
  }

  // ---- expressions --------------------------------------------------------
  void resolve_expr(Expr& e, FuncScope* sc) {
    switch (e.kind) {
      case ExprKind::Unary:
        if (e.lhs) resolve_expr(*e.lhs, sc);
        break;
      case ExprKind::Binary:
      case ExprKind::Logical:
        if (e.lhs) resolve_expr(*e.lhs, sc);
        if (e.rhs) resolve_expr(*e.rhs, sc);
        break;
      case ExprKind::Ternary:
        if (e.lhs) resolve_expr(*e.lhs, sc);       // condition
        if (e.rhs) resolve_expr(*e.rhs, sc);       // then-value
        if (e.third) resolve_expr(*e.third, sc);   // else-value
        break;
      case ExprKind::Call:
        resolve_call(e, sc);
        break;
      case ExprKind::Var:
        resolve_var_read(e, sc);
        break;
      case ExprKind::VarQual:
        // Cross-scope access (docs/reference/cross-scope-access.md §4): recorded now,
        // resolved in phase 5 (the binding may live in a function whose
        // locals are not collected yet, and forward references must work).
        record_qual_site(e, sc);
        break;
      case ExprKind::ListLit:
      case ExprKind::SetLit:
      case ExprKind::TupleLit:
      case ExprKind::Join:
        for (auto& el : e.elems)
          if (el) resolve_expr(*el, sc);
        break;
      case ExprKind::MapLit:
        for (auto& k : e.keys)
          if (k) resolve_expr(*k, sc);
        for (auto& el : e.elems)
          if (el) resolve_expr(*el, sc);
        break;
      case ExprKind::Index:
        if (e.lhs) resolve_expr(*e.lhs, sc);
        if (e.rhs) resolve_expr(*e.rhs, sc);
        break;
      case ExprKind::Slice:
        if (e.lhs) resolve_expr(*e.lhs, sc);
        if (e.rhs) resolve_expr(*e.rhs, sc);      // start
        if (e.third) resolve_expr(*e.third, sc);  // stop
        if (e.fourth) resolve_expr(*e.fourth, sc);  // step
        break;
      case ExprKind::MethodCall:
        if (e.lhs) resolve_expr(*e.lhs, sc);
        for (auto& el : e.elems)
          if (el) resolve_expr(*el, sc);
        // M2-2C §3.2: argument form is dynamic — named arguments reach the
        // bound method's binder (builtins reject them at runtime with the
        // §13 wording), so nothing is checked statically here (supersedes
        // the 2B §12 positional-only rule).
        break;
      case ExprKind::Attr:
        // M2-2C §3.2: bare attribute read — resolve the object only; which
        // attribute exists is dynamic (AttributeError at runtime).
        if (e.lhs) resolve_expr(*e.lhs, sc);
        break;
      default:
        break;  // literals
    }
  }

  // Builtin exception classes (2C §2.2) resolve as readable globals —
  // inserted into the global set once, bound by codegen from the runtime
  // singleton. Not assignable (the reserved set rejects every binding).
  void inject_exc_class(const std::string& name) {
    if (globals_.insert(name).second) prog_.globals.push_back(name);
  }

  void resolve_var_read(Expr& e, FuncScope* sc) {
    if (sc) {
      // A `global` declaration makes every reference in the function global.
      // Register the slot even when nothing ever assigns it (the read then
      // trips the usual used-before-assignment guard at runtime instead of
      // codegen hitting an undeclared pt_g_<name>).
      if (sc->global_names.count(e.name)) {
        note_global(e.name);
        e.res = VarRes::Global;
        return;
      }
      if (sc->params.count(e.name)) {
        e.res = VarRes::Param;
        return;
      }
      // Reading one of the function's own nested-declaration names yields a
      // function value (2B §6.6); the binding is immutable by rule, so a
      // direct reference is safe.
      if (sc->nested.count(e.name)) {
        e.res = VarRes::Function;
        return;
      }
      if (sc->locals.count(e.name)) {
        e.res = VarRes::Local;
        e.is_cell = sc->boxed.count(e.name) != 0;
        return;
      }
      FuncScope* owner = find_owner(sc->parent, e.name);
      if (owner) {
        VarRes res = owner->params.count(e.name) ? VarRes::Param
                                                 : VarRes::Local;
        add_capture(sc, e.name, res);  // read capture (by reference)
        e.res = res;
        e.is_cell = true;
        return;
      }
    }
    if (globals_.count(e.name)) {
      e.res = VarRes::Global;
      return;
    }
    if (funcs_.count(e.name)) {
      // Reading a top-level function name yields a function value
      // (2B §6.6 — approved M1 change: was "cannot be used as a variable").
      e.res = VarRes::Function;
      return;
    }
    if (is_builtin_exception_class(e.name)) {
      // M2-2C §2.2: readable singleton class value (2C §2.4 except conds,
      // 2C §2.3 bare `raise ValueError`, first-class class references).
      inject_exc_class(e.name);
      e.res = VarRes::Global;
      return;
    }
    // Includes builtin names: `print`/`range`/etc. as bare values get the
    // same M1 treatment `print` receives — undefined variable (2B §11).
    error(e.loc, "undefined variable '" + e.name + "'");
    e.res = VarRes::Global;
  }

  // ---- calls ---------------------------------------------------------------
  //
  // Static checks (2B §6.5/§11) run whenever the callee's declaration is
  // known — top-level or nested. Indirect callees (callee_expr with no bare
  // name) and calls through variables are dynamic (runtime binder, 2B §13).
  void resolve_call(Expr& e, FuncScope* sc) {
    if (e.callee.empty()) {
      // Indirect call: dynamic binder — skip static arity/name checks.
      if (e.callee_expr) resolve_expr(*e.callee_expr, sc);
      for (auto& arg : e.args)
        if (arg) resolve_expr(*arg, sc);
      return;
    }
    const std::string& n = e.callee;
    if (is_builtin_exception_class(n)) {
      // M2-2C §2.2: calling a class constructs an exception instance — a
      // dynamic pt_call through the injected global (0 or 1 positional
      // arg; pt_exc_construct validates at runtime).
      inject_exc_class(n);
      auto v = std::make_unique<Expr>();
      v->kind = ExprKind::Var;
      v->loc = e.loc;
      v->name = n;
      v->res = VarRes::Global;
      v->is_cell = false;
      e.callee_expr = std::move(v);
      for (auto& arg : e.args)
        if (arg) resolve_expr(*arg, sc);
      return;
    }
    if (is_reserved(n)) {
      resolve_builtin_call(e, sc);
      return;
    }
    FuncDecl* target = nullptr;
    bool dynamic = false;  // callee is a variable holding a function value
    if (sc) {
      auto it = sc->nested.find(n);
      if (it != sc->nested.end()) {
        target = it->second;
      } else if (sc->params.count(n) || sc->locals.count(n)) {
        dynamic = true;  // own local/param variable
      } else if (sc->global_names.count(n)) {
        // A `global` declaration redirects the VARIABLE namespace only. For
        // the CALL namespace a top-level function of the same name still
        // wins (v0.3 split — identical to top-level resolution); without one
        // the call goes dynamically through the global variable's value.
        if (!funcs_.count(n)) dynamic = true;
      } else {
        FuncScope* owner = find_owner(sc->parent, n);
        if (owner) {
          auto own = owner->nested.find(n);
          if (own != owner->nested.end())
            target = own->second;
          else
            dynamic = true;  // enclosing local/param holding a function
        }
      }
    }
    if (!target && !dynamic) {
      auto itf = funcs_.find(n);
      if (itf != funcs_.end()) {
        target = itf->second;
      } else if (globals_.count(n)) {
        dynamic = true;  // global variable (may hold a function value)
      } else {
        error(e.loc, "undefined function '" + n + "'");
        for (auto& arg : e.args)
          if (arg) resolve_expr(*arg, sc);
        return;
      }
    }
    if (target) check_static_call(e, *target);
    if (!target || target->is_nested) {
      // Nested callees go through the local's function value (dynamic
      // pt_call; 2B §6.5). Annotate a Var callee_expr so codegen can load
      // the closure; a parser-attached bare Var is annotated in place.
      if (e.callee_expr && e.callee_expr->kind == ExprKind::Var) {
        annotate_callee_load(*e.callee_expr, sc);
      } else if (!e.callee_expr) {
        auto v = std::make_unique<Expr>();
        v->kind = ExprKind::Var;
        v->loc = e.loc;
        v->name = n;
        annotate_callee_load(*v, sc);
        e.callee_expr = std::move(v);
      }
    } else {
      // Static direct call to a top-level, env-less function: drop the
      // parser-attached bare Var so `callee_expr != null` uniformly marks
      // the dynamic pt_call path for codegen.
      e.callee_expr.reset();
    }
    for (auto& arg : e.args)
      if (arg) resolve_expr(*arg, sc);
  }

  // Annotates a bare-name callee Var as a storage load (never the direct
  // Function reference: calls through nested/capturing functions always go
  // through the local's value, 2B §6.5).
  void annotate_callee_load(Expr& v, FuncScope* sc) {
    v.is_cell = false;
    if (sc) {
      if (sc->global_names.count(v.name)) {
        note_global(v.name);  // slot registration, as in resolve_var_read
        v.res = VarRes::Global;
        return;
      }
      if (sc->params.count(v.name)) {
        v.res = VarRes::Param;
        v.is_cell = sc->boxed.count(v.name) != 0;
        return;
      }
      if (sc->nested.count(v.name)) {
        v.res = VarRes::Local;  // the local holding the hoisted closure
        v.is_cell = sc->boxed.count(v.name) != 0;
        return;
      }
      if (sc->locals.count(v.name)) {
        v.res = VarRes::Local;
        v.is_cell = sc->boxed.count(v.name) != 0;
        return;
      }
      FuncScope* owner = find_owner(sc->parent, v.name);
      if (owner) {
        VarRes res = owner->params.count(v.name) ? VarRes::Param
                                                 : VarRes::Local;
        add_capture(sc, v.name, res);
        v.res = res;
        v.is_cell = true;
        return;
      }
    }
    v.res = VarRes::Global;  // globals_ was checked by the caller
  }

  // Builtin call (2B §8/§11): per-builtin arity; print any, range 1–3,
  // len/str/int/float/bool/type/encode/decode exactly 1, set 0–1. The
  // Python-parity builtins (SPEC §5.11.5) bind through src/builtins.h's
  // slot table; the Python dynamic-code trio never compiles (§5.11.6) and
  // the Python-concept stubs always fail at runtime (§5.11.6).
  void resolve_builtin_call(Expr& e, FuncScope* sc) {
    const std::string& n = e.callee;
    e.is_builtin = true;
    e.builtin_name = n;
    if (n == "print") e.is_builtin_print = true;  // any arity, including 0
    if (n == "range") e.is_builtin_range = true;  // reserved builtin (2A §7)
    size_t argc = e.args.size();
    bool named = false;
    for (const std::string& an : e.arg_names)
      if (!an.empty()) { named = true; break; }
    if (py_builtin_unavailable(n)) {
      // SPEC §5.11.6: Pentest compiles ahead of time and the runtime never
      // compiles source — no embedded parser, no interpreter, no megabyte
      // of compiler in every binary. A dedicated error beats an unknown
      // name so the intent is documented where it happens. This is the
      // terminal diagnosis for the call — no arity/shape check follows
      // (their shapes only make sense in a language that has them).
      error(e.loc, "'" + n + "' is not available in cstar: programs are "
                           "compiled ahead of time and the runtime never "
                           "compiles source (see SPEC.md §5.11.6)");
      for (auto& arg : e.args)
        if (arg) resolve_expr(*arg, sc);
      return;
    }
    if (py_builtin_stub(n)) {
      // SPEC §5.11.6: recognized and reserved, arguments evaluate normally,
      // then the call always raises RuntimeError with its own reason
      // (pt_builtin_unsupported). Arity is not policed — it can only fail.
      for (auto& arg : e.args)
        if (arg) resolve_expr(*arg, sc);
      return;
    }
    if (const BuiltinSpec* spec = py_builtin_spec(n)) {
      resolve_py_builtin_call(e, *spec);
      if (n == "isinstance") rewrite_isinstance_classinfo(e, sc);
      for (auto& arg : e.args)
        if (arg) resolve_expr(*arg, sc);
      return;
    }
    if (named)
      error(e.loc, "builtin '" + n + "' does not take named arguments");
    if (n == "print") {
      // any arity (M1)
    } else if (n == "range") {
      if (argc < 1 || argc > 3)
        error(e.loc, "function 'range' expects 1 to 3 argument(s), got " +
                         std::to_string(argc));
    } else if (n == "set") {
      if (argc > 1)
        error(e.loc, "'set' expects 0 to 1 argument(s), got " +
                         std::to_string(argc));
    } else if (n == "collect") {
      if (argc != 0)
        error(e.loc, "'collect' expects 0 argument(s), got " +
                         std::to_string(argc));
    } else if (n == "input") {
      if (argc > 1)
        error(e.loc, "'input' expects 0 to 1 argument(s), got " +
                         std::to_string(argc));
    } else if (n == "time" || n == "clock") {
      if (argc != 0)
        error(e.loc, "'" + n + "' expects 0 argument(s), got " +
                         std::to_string(argc));
    } else if (n == "http_post") {
      if (argc < 2 || argc > 3)
        error(e.loc, "'http_post' expects 2 to 3 argument(s), got " +
                         std::to_string(argc));
    } else if (n == "http_get") {
      if (argc < 1 || argc > 2)
        error(e.loc, "'http_get' expects 1 to 2 argument(s), got " +
                         std::to_string(argc));
    } else if (n == "url_encode") {
      if (argc != 1)
        error(e.loc, "'url_encode' expects 1 argument(s), got " +
                         std::to_string(argc));
    } else if (n == "fvec" || n == "ivec") {
      if (argc != 1)
        error(e.loc, "'" + n + "' expects 1 argument(s), got " +
                         std::to_string(argc));
    } else if (n == "fvec_fill" || n == "ivec_fill" || n == "dot") {
      if (argc != 2)
        error(e.loc, "'" + n + "' expects 2 argument(s), got " +
                         std::to_string(argc));
    } else {
      if (argc != 1)
        error(e.loc, "'" + n + "' expects 1 argument(s), got " +
                         std::to_string(argc));
    }
    for (auto& arg : e.args)
      if (arg) resolve_expr(*arg, sc);
  }

  // SPEC §5.11.5 isinstance: a bare builtin type name in classinfo
  // position (`isinstance(x, str)`) names the TYPE, not a value — those
  // names are call-only, so the semantic pass rewrites the Var into the
  // runtime's type-name string (exactly what pt_type_name yields: str→
  // "text", dict→"map", ...). A shadowing param/local wins, as everywhere
  // else; anything else in classinfo stays for the runtime to judge (a
  // class singleton, a tuple, a type-name text).
  void rewrite_isinstance_classinfo(Expr& e, FuncScope* sc) {
    static const std::pair<const char*, const char*> kTypeNames[] = {
        {"int", "int"},     {"float", "float"},   {"bool", "bool"},
        {"str", "text"},    {"bytes", "bytes"},   {"list", "list"},
        {"tuple", "tuple"}, {"set", "set"},       {"dict", "map"},
        {"range", "range"}, {"type", "class"},
    };
    size_t ci = (size_t)-1;
    for (size_t i = 0; i < e.args.size(); ++i)
      if (i < e.arg_names.size() && e.arg_names[i] == "classinfo") {
        ci = i;
        break;
      }
    if (ci == (size_t)-1 && e.args.size() >= 2u &&
        (e.arg_names.size() < 2u || e.arg_names[1].empty()))
      ci = 1; /* the positional classinfo */
    if (ci >= e.args.size()) return;
    std::unique_ptr<Expr>& a = e.args[ci];
    if (!a) return;
    auto shadowed = [&](const std::string& name) {
      for (FuncScope* s = sc; s != nullptr; s = s->parent)
        if (s->params.count(name) || s->locals.count(name)) return true;
      return false;
    };
    auto rewrite_one = [&](std::unique_ptr<Expr>& slot) {
      if (!slot || slot->kind != ExprKind::Var || shadowed(slot->name)) return;
      for (const auto& kv : kTypeNames) {
        if (slot->name == kv.first) {
          auto lit = std::make_unique<Expr>();
          lit->kind = ExprKind::StrLit;
          lit->loc = slot->loc;
          lit->sval = kv.second;
          slot = std::move(lit);
          return;
        }
      }
    };
    if (a->kind == ExprKind::TupleLit) { /* isinstance(x, (int, float)) */
      for (std::unique_ptr<Expr>& el : a->elems) rewrite_one(el);
      return;
    }
    rewrite_one(a);
  }

  // Binds a Python builtin's arguments into its fixed runtime slot layout
  // (SPEC §5.11.5): positionals fill slots left-to-right, named arguments
  // fill their name's slot, and every slot is also nameable — a superset
  // of Python's positional-only/keywork-only splits, never a subset.
  // Exceptions: dict takes at most one positional source and arbitrary
  // named keys, slice's single positional argument is STOP (slice(5) ==
  // slice(None, 5, None)), and variadics (max/min/map/zip) keep their
  // positionals as an array. Errors reuse Python's message shapes.
  void resolve_py_builtin_call(Expr& e, const BuiltinSpec& spec) {
    const std::string n(spec.name);
    size_t argc = e.args.size();
    size_t nslots = spec.slots.size();
    std::vector<int> slot_of(argc, -1);
    std::vector<bool> filled(nslots, false);
    if (spec.dict_form) {
      // at most ONE positional source; every named argument is a key
      size_t npos = 0;
      for (size_t i = 0; i < argc; ++i) {
        if (i < e.arg_names.size() && !e.arg_names[i].empty()) continue;
        slot_of[i] = 0;
        ++npos;
      }
      if (npos > 1)
        error(e.loc, "'dict' expects at most 1 positional argument, got " +
                         std::to_string(npos));
      if (npos == 1) filled[0] = true;
      e.arg_slots = std::move(slot_of);
      e.arg_nslots = nslots;
      return;
    }
    size_t npos = 0;
    bool key_named = false, default_named = false;
    for (size_t i = 0; i < argc; ++i) {
      const std::string& an = e.arg_names[i];
      if (!an.empty()) {
        if (spec.variadic) {
          // max/min accept a named `key`/`default` (both pull through the
          // runtime's fixed trailing parameters); map/zip take no names.
          if (an == "key" && spec.variadic_key && !key_named) {
            key_named = true;
            continue;
          }
          if (an == "default" && spec.variadic_default && !default_named) {
            default_named = true;
            continue;
          }
          error(e.loc, "'" + n + "' has no named argument '" + an + "'");
          continue;
        }
        int s = -1;
        for (size_t k = 0; k < nslots; ++k)
          if (an == spec.slots[k]) { s = (int)k; break; }
        if (s < 0) {
          error(e.loc, "'" + n + "' has no named argument '" + an + "'");
          continue;
        }
        if (filled[s]) {
          error(e.loc,
                "'" + n + "' got multiple values for argument '" + an + "'");
          continue;
        }
        filled[s] = true;
        slot_of[i] = s;
        continue;
      }
      // Positional.
      if (spec.variadic) {
        slot_of[i] = -1;  // lives in the arg array (codegen)
        ++npos;
        continue;
      }
      size_t s = npos;
      if (spec.slice_form && argc == 1) s = 1;  // slice(x) == slice(None, x)
      if (s >= nslots) {
        error(e.loc, "'" + n + "' expects at most " + std::to_string(nslots) +
                         " argument(s), got " + std::to_string(argc));
        continue;
      }
      if (filled[s]) {
        error(e.loc, "'" + n + "' got multiple values for argument '" +
                         spec.slots[s] + "'");
        continue;
      }
      filled[s] = true;
      slot_of[i] = (int)s;
      ++npos;
    }
    if (spec.variadic) {
      if (npos < spec.min_args)
        error(e.loc, "'" + n + "' expects at least " +
                         std::to_string(spec.min_args) +
                         " argument(s), got " + std::to_string(npos));
    } else {
      size_t nf = 0;
      for (bool f : filled) nf += f ? 1u : 0u;
      if (nf < spec.min_args) {
        // Python's shape: 'abs() missing 1 required positional argument:
        // 'x'' (plural arguments + comma-joined slots above 1).
        size_t missing = spec.min_args - nf;
        std::string slots;
        for (size_t k = 0; k < nslots && missing > 0; ++k) {
          if (filled[k]) continue;
          if (!slots.empty()) slots += ", ";
          slots += std::string("'") + spec.slots[k] + "'";
          --missing;
        }
        error(e.loc, n + "() missing " + std::to_string(spec.min_args - nf) +
                         " required positional argument" +
                         (spec.min_args - nf == 1 ? "" : "s") + ": " + slots);
      }
    }
    e.arg_slots = std::move(slot_of);
    e.arg_nslots = nslots;
  }

  // Static binding checks against a known declaration (2B §11). Fixed
  // functions called positionally keep the exact M1 message ("expects N
  // argument(s), got M"); defaults/variadics/named args use the 2B
  // at-least/at-most forms.
  void check_static_call(const Expr& e, const FuncDecl& f) {
    size_t argc = e.args.size();
    size_t np = 0;  // positional count
    for (size_t i = 0; i < argc; ++i)
      if (i >= e.arg_names.size() || e.arg_names[i].empty()) ++np;

    bool has_default = false;
    for (const auto& d : f.defaults)
      if (d) { has_default = true; break; }

    if (!has_default && !f.has_vararg && !f.has_kwarg) {
      bool any_named = false;
      for (const std::string& an : e.arg_names)
        if (!an.empty()) { any_named = true; break; }
      if (!any_named && np != f.params.size()) {
        error(e.loc, "function '" + display_ident(f.name) + "' expects " +
                         std::to_string(f.params.size()) +
                         " argument(s), got " + std::to_string(np));
      } else if (any_named) {
        check_named_args(e, f, np);
      }
      return;
    }

    // Defaults / *args / **kwargs: named-argument binding first, then the
    // positional bounds (2B §11).
    size_t named_bound = check_named_args(e, f, np);
    (void)named_bound;  // kept for symmetry; coverage is computed below
    if (!f.has_vararg && np > f.params.size()) {
      error(e.loc, "function '" + display_ident(f.name) + "' expects at most " +
                       std::to_string(f.params.size()) + " argument(s), got " +
                       std::to_string(np));
    }
    // Required parameters are a CONTIGUOUS prefix (defaults are trailing,
    // §6), so every slot 0..min_args-1 must receive a positional or a
    // named binding. A named argument that binds a defaulted slot does not
    // satisfy a missing required one (§11 "missing required"): report the
    // number of arguments actually bound to required slots.
    size_t nreq = static_cast<size_t>(f.min_args);
    size_t covered = np < nreq ? np : nreq;
    for (size_t i = 0; i < e.args.size(); ++i) {
      if (i >= e.arg_names.size() || e.arg_names[i].empty()) continue;
      const std::string& an = e.arg_names[i];
      for (size_t j = 0; j < f.params.size(); ++j) {
        if (f.params[j] != an) continue;
        if (j >= np && j < nreq) ++covered;  // named fill of a required slot
        break;
      }
    }
    if (covered < nreq) {
      error(e.loc, "function '" + display_ident(f.name) + "' expects at least " +
                       std::to_string(nreq) + " argument(s), got " +
                       std::to_string(covered));
    }
  }

  // Validates named arguments against a known declaration; returns how many
  // named args bind positional params (for the required-count check).
  size_t check_named_args(const Expr& e, const FuncDecl& f, size_t np) {
    size_t named_bound = 0;
    std::unordered_set<std::string> seen;
    for (size_t i = 0; i < e.args.size(); ++i) {
      if (i >= e.arg_names.size() || e.arg_names[i].empty()) continue;
      const std::string& an = e.arg_names[i];
      if (!seen.insert(an).second) {
        error(e.loc, "argument '" + an + "' got multiple values in call to '" +
                         f.name + "'");
        continue;
      }
      // A named argument matching a positional parameter ALWAYS binds that
      // parameter (2B §6/§9.2), even when the declaration has **kwargs;
      // only names outside the parameter list are captured by **kwargs.
      size_t idx = f.params.size();
      for (size_t j = 0; j < f.params.size(); ++j)
        if (f.params[j] == an) { idx = j; break; }
      if (idx == f.params.size()) {
        if (f.has_kwarg) continue;  // unknown name is captured by **kwargs
        error(e.loc, "unexpected argument name '" + an + "' in call to '" +
                         f.name + "'");
      } else if (idx < np) {
        error(e.loc, "argument '" + an +
                         "' got multiple values in call to '" + f.name + "'");
      } else {
        ++named_bound;
      }
    }
    return named_bound;
  }

  // ---- phase 5: cross-scope access (docs/reference/cross-scope-access.md) --------------
  //
  // `variable.<qualifier> <name>` sites are RECORDED during phases 3/4 with
  // a snapshot of the enclosing-function chain (FuncScopes die with their
  // prepare_function call) and resolved here — after every function's
  // params/locals list and boxing flags are final, which is what lets a
  // qualified reference target any sibling/enclosing binding in any
  // declaration order (the language's forward-reference philosophy).
  //
  // Resolution binds the site to the owner's storage and PINs the binding:
  // a param/local reached across function boundaries gets exactly one
  // program-lifetime static slot (docs/reference/cross-scope-access.md §4). The owner's
  // own reads/writes and every `variable.` access then lower to the same
  // plain C static — zero allocation, zero indirection, no per-call frame
  // registry — which is why the feature needs no dedicated optimization.
  struct QualSite {
    bool is_target = false;  // false: ExprKind::VarQual read; true: target
    Expr* expr = nullptr;
    AssignTarget* target = nullptr;
    std::vector<FuncDecl*> chain;  // current function first, outward
    SourceLoc loc;
  };

  std::vector<FuncDecl*> chain_of(FuncScope* sc) {
    std::vector<FuncDecl*> chain;
    for (FuncScope* s = sc; s; s = s->parent)
      if (s->decl) chain.push_back(s->decl);
    return chain;
  }

  void record_qual_site(Expr& e, FuncScope* sc) {
    QualSite site;
    site.expr = &e;
    site.chain = chain_of(sc);
    site.loc = e.loc;
    qual_sites_.push_back(std::move(site));
  }

  void record_qual_site(AssignTarget& t, FuncScope* sc) {
    QualSite site;
    site.is_target = true;
    site.target = &t;
    site.chain = chain_of(sc);
    site.loc = t.loc;
    qual_sites_.push_back(std::move(site));
  }

  // Does `f` bind `name` as a positional param, *vararg/**kwarg slot or
  // local? (is_param distinguishes the Param/Local resolution.)
  static bool func_binds(const FuncDecl& f, const std::string& name,
                         bool& is_param) {
    for (const std::string& p : f.params)
      if (p == name) { is_param = true; return true; }
    if (f.has_vararg && f.vararg == name) { is_param = true; return true; }
    if (f.has_kwarg && f.kwarg == name) { is_param = true; return true; }
    for (const std::string& l : f.locals)
      if (l == name) { is_param = false; return true; }
    return false;
  }

  // Boxing flag of `f`'s binding (finalize_function output): the binding is
  // a heap cell because some nested closure captures it.
  static bool func_binding_is_cell(const FuncDecl& f, const std::string& name,
                                   bool is_param) {
    if (is_param) {
      if (f.has_vararg && f.vararg == name) return f.vararg_is_cell;
      if (f.has_kwarg && f.kwarg == name) return f.kwarg_is_cell;
      for (size_t i = 0; i < f.params.size(); ++i)
        if (f.params[i] == name)
          return i < f.param_is_cell.size() && f.param_is_cell[i];
      return false;
    }
    for (size_t i = 0; i < f.locals.size(); ++i)
      if (f.locals[i] == name)
        return i < f.local_is_cell.size() && f.local_is_cell[i];
    return false;
  }

  // The nested function declaration named `name` inside `owner` (first
  // declaration wins, mirroring process_nested_decls' map semantics).
  static FuncDecl* find_nested_in(const std::vector<Stmt>& stmts,
                                  const std::string& name) {
    for (const Stmt& s : stmts) {
      FuncDecl* f = find_nested_stmt(s, name);
      if (f) return f;
    }
    return nullptr;
  }

  static FuncDecl* find_nested_stmt(const Stmt& s, const std::string& name) {
    switch (s.kind) {
      case StmtKind::Func:
        if (s.func && s.func->name == name) return s.func.get();
        break;
      case StmtKind::If:
        for (const std::vector<Stmt>& body : s.bodies) {
          FuncDecl* f = find_nested_in(body, name);
          if (f) return f;
        }
        if (s.has_else) {
          FuncDecl* f = find_nested_in(s.else_body, name);
          if (f) return f;
        }
        break;
      case StmtKind::While:
      case StmtKind::For: {
        FuncDecl* f = find_nested_in(s.body, name);
        if (f) return f;
        break;
      }
      case StmtKind::Switch:
        for (const CaseArm& arm : s.cases) {
          FuncDecl* f = find_nested_in(arm.body, name);
          if (f) return f;
        }
        break;
      case StmtKind::Try:
        for (const Stmt& b : s.body) {
          FuncDecl* f = find_nested_stmt(b, name);
          if (f) return f;
        }
        for (const ExceptArm& arm : s.arms) {
          FuncDecl* f = find_nested_in(arm.body, name);
          if (f) return f;
        }
        for (const Stmt& b : s.finally_body) {
          FuncDecl* f = find_nested_stmt(b, name);
          if (f) return f;
        }
        break;
      default:
        break;
    }
    return nullptr;
  }

  // Lookup rules (docs/reference/cross-scope-access.md §4):
  //  * global — the name must be a module global;
  //  * parent — the nearest ENCLOSING function binding wins (nonlocal-like
  //    outward search; the current function itself is skipped);
  //  * <fn>  — <fn> must share the current scope: a top-level function when
  //    the site sits at top level or inside a top-level function, else a
  //    nested function of the same enclosing function (the current function
  //    may name itself); the binding may be any param or local of it.
  bool resolve_qual(const std::string& q, const std::string& n,
                    const std::vector<FuncDecl*>& chain, SourceLoc loc,
                    FuncDecl*& owner, VarRes& res, bool& cell) {
    owner = nullptr;
    cell = false;
    res = VarRes::Global;
    if (q == "global") {
      // v0.3 namespace split: a global variable may share a name with a
      // function — `variable.global` targets the variable (it exists iff
      // globals_ holds the name). A function-only name still errors: it has
      // no module storage to reach.
      if (!globals_.count(n)) {
        error(loc, "no binding for 'variable.global " + n + "' found");
        return false;
      }
      return true;
    }
    if (q == "parent") {
      for (size_t i = 1; i < chain.size(); ++i) {
        bool is_param = false;
        if (func_binds(*chain[i], n, is_param)) {
          owner = chain[i];
          res = is_param ? VarRes::Param : VarRes::Local;
          cell = func_binding_is_cell(*chain[i], n, is_param);
          return true;
        }
      }
      error(loc, "no binding for 'variable.parent " + n + "' found");
      return false;
    }
    FuncDecl* fn = nullptr;
    if (chain.size() <= 1) {
      auto it = funcs_.find(q);  // top-level functions (methods excluded)
      if (it != funcs_.end()) fn = it->second;
    } else {
      fn = find_nested_in(chain[1]->body, q);
    }
    if (!fn) {
      error(loc, "'" + q + "' is not a function sharing this scope");
      return false;
    }
    bool is_param = false;
    if (!func_binds(*fn, n, is_param)) {
      error(loc, "no binding for 'variable." + q + " " + n + "' found");
      return false;
    }
    owner = fn;
    res = is_param ? VarRes::Param : VarRes::Local;
    cell = func_binding_is_cell(*fn, n, is_param);
    return true;
  }

  void resolve_qual_sites() {
    for (QualSite& site : qual_sites_) {
      const std::string& q =
          site.is_target ? site.target->attr : site.expr->attr;
      const std::string& n =
          site.is_target ? site.target->name : site.expr->name;
      FuncDecl* owner = nullptr;
      VarRes res = VarRes::Global;
      bool cell = false;
      if (!resolve_qual(q, n, site.chain, site.loc, owner, res, cell))
        continue;
      std::string pin;
      if (owner) {
        pin = pin_cvar(*owner, n, cell);
        note_pin(owner, n, pin, cell);
      } else {
        note_pin_global(n);
      }
      if (site.is_target) {
        site.target->res = res;
        site.target->pin = pin;
        site.target->pin_cell = cell;
      } else {
        site.expr->res = res;
        site.expr->is_cell = false;  // reads go straight to the slot
        site.expr->pin = pin;
        site.expr->pin_cell = cell;
      }
    }
  }

  // Registers a pinned slot once (program registry + owner annotation), so
  // codegen emits exactly one static per (owner, binding) no matter how
  // many qualified sites reach it.
  void note_pin(FuncDecl* owner, const std::string& n, const std::string& pin,
                bool cell) {
    for (const PinSlot& p : prog_.pins)
      if (p.owner == owner && p.name == n) return;
    prog_.pins.push_back(PinSlot{owner, n, pin, cell});
    for (const std::string& pn : owner->pinned)
      if (pn == n) return;
    owner->pinned.push_back(n);
  }

  void note_pin_global(const std::string& n) {
    for (const std::string& g : prog_.pin_globals)
      if (g == n) return;
    prog_.pin_globals.push_back(n);
  }

  void error(SourceLoc loc, const std::string& msg) {
    ok_ = false;
    diags_.error(loc, msg);
  }

  Program& prog_;
  Diagnostics& diags_;
  bool ok_ = true;
  int loop_depth_ = 0;  // enclosing loop nesting (switch does not count)
  std::unordered_map<std::string, FuncDecl*> funcs_;  // global functions
  std::unordered_set<std::string> globals_;
  std::unordered_set<std::string> classes_;  // M2-2C §3.1: class names (dups)
  std::vector<FuncDecl*> global_funcs_;  // bodies to process in phase 4
  std::unordered_set<std::string> c_names_;
  std::vector<QualSite> qual_sites_;  // phase-5 cross-scope access sites
};

}  // namespace

bool semantic_check(Program& out, Diagnostics& diags) {
  Checker c(out, diags);
  return c.run();
}

// M2-2C §2.2 — MUST match pt_exc_cls_names in runtime/pentest_rt.c (the
// exc_classes test builds and catches with every name, guarding drift).
bool is_builtin_exception_class(const std::string& name) {
  return name == "Exception" || name == "RuntimeError" ||
         name == "TypeError" || name == "ValueError" ||
         name == "NameError" || name == "ZeroDivisionError" ||
         name == "IndexError" || name == "KeyError" ||
         name == "AttributeError" ||
         /* SPEC §5.11.7: Python-parity additions — next() exhaustion,
          * open()/read()/write() failures (ENOENT maps to the specific
          * FileNotFoundError, everything else to OSError). */
         name == "StopIteration" || name == "FileNotFoundError" ||
         name == "OSError";
}

}  // namespace pentest
