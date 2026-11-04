// modules.cpp — Stage 2D import linker and selective-loading pass.
// See modules.h for the contract. The pass runs before semantic analysis and
// code generation; it changes only the AST, never the runtime representation
// of a value or a call.
#include "modules.h"

#include <cerrno>
#include <unistd.h>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "frontend.h"

namespace pentest {
namespace {

std::string path_dirname(const std::string& path) {
  std::size_t slash = path.rfind('/');
  if (slash == std::string::npos) return ".";
  if (slash == 0) return "/";
  return path.substr(0, slash);
}

bool path_is_absolute(const std::string& path) {
  return !path.empty() && path[0] == '/';
}

std::string path_join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (a.back() == '/') return a + b;
  return a + "/" + b;
}

std::string canonical_path(const std::string& path) {
  char buf[PATH_MAX];
  if (::realpath(path.c_str(), buf) != nullptr) return std::string(buf);
  return std::string();
}

bool read_source(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in),
             std::istreambuf_iterator<char>());
  return in.good() || in.eof();
}

std::string last_component(const std::string& dotted) {
  std::size_t dot = dotted.rfind('.');
  return dot == std::string::npos ? dotted : dotted.substr(dot + 1);
}

// `a.b` -> `a/b`; the parser guarantees identifier components only.
std::string module_relative_path(const std::string& dotted) {
  std::string rel = dotted;
  for (char& c : rel)
    if (c == '.') c = '/';
  return rel;
}

bool is_private_name(const std::string& name) {
  return !name.empty() && name[0] == '_';
}

// ---------------------------------------------------------------------------
// Module registry
// ---------------------------------------------------------------------------

struct Module {
  std::string path;   // canonical source path
  std::string dir;    // canonical directory
  std::string source;
  Program prog;
  std::unordered_map<std::string, std::string> rename;  // own globals -> __ptmN_name
  int id = 0;
  bool loading = false;
  bool expanded = false;
};

struct ImportBinding {
  Module* module = nullptr;
  bool has_alias = false;
  std::string alias;
};

// Per-program rename maps used while expanding one file.
struct LinkMaps {
  const std::unordered_map<std::string, std::string>* own = nullptr;
  std::unordered_map<std::string, std::string> from;   // source name -> internal
  std::unordered_map<std::string, Module*> aliases;    // module alias -> module
  std::unordered_set<std::string> own_names;
};

// Scope information for the global-reference rewriter. The rewriter needs
// exactly the binding rules semantic.cpp uses: params/locals/nested names
// shadow module globals; `global` names escape to module scope; `nonlocal`
// names stay in the enclosing function.
struct Scope {
  std::unordered_set<std::string> locals;
  std::unordered_set<std::string> global_names;
  const Scope* parent = nullptr;
  bool module_level = false;

  bool has_local(const std::string& n) const {
    for (const Scope* s = this; s != nullptr; s = s->parent) {
      if (s->global_names.count(n) != 0) return false;
      if (s->locals.count(n) != 0) return true;
    }
    return false;
  }

  bool is_global_decl(const std::string& n) const {
    for (const Scope* s = this; s != nullptr; s = s->parent) {
      if (s->global_names.count(n) != 0) return true;
      if (s->locals.count(n) != 0) return false;
    }
    return false;
  }
};

void collect_target_names(const AssignTarget& t,
                          std::unordered_set<std::string>& out) {
  switch (t.kind) {
    case AssignTarget::Kind::Name:
    case AssignTarget::Kind::Rest:
      out.insert(t.name);
      break;
    case AssignTarget::Kind::Tuple:
      for (const AssignTarget& e : t.elems) collect_target_names(e, out);
      break;
    default:
      break;
  }
}

void collect_stmt_bindings(const Stmt& s, std::unordered_set<std::string>& out,
                           bool include_nested_funcs) {
  switch (s.kind) {
    case StmtKind::Assign:
      collect_target_names(s.target, out);
      break;
    case StmtKind::For:
      out.insert(s.name);
      for (const Stmt& b : s.body) collect_stmt_bindings(b, out, true);
      break;
    case StmtKind::While:
      for (const Stmt& b : s.body) collect_stmt_bindings(b, out, true);
      break;
    case StmtKind::If:
      for (const std::vector<Stmt>& body : s.bodies)
        for (const Stmt& b : body) collect_stmt_bindings(b, out, true);
      if (s.has_else)
        for (const Stmt& b : s.else_body) collect_stmt_bindings(b, out, true);
      break;
    case StmtKind::Switch:
      for (const CaseArm& arm : s.cases)
        for (const Stmt& b : arm.body) collect_stmt_bindings(b, out, true);
      break;
    case StmtKind::Try:
      for (const Stmt& b : s.body) collect_stmt_bindings(b, out, true);
      for (const ExceptArm& arm : s.arms) {
        if (!arm.name.empty()) out.insert(arm.name);
        for (const Stmt& b : arm.body) collect_stmt_bindings(b, out, true);
      }
      for (const Stmt& b : s.finally_body)
        collect_stmt_bindings(b, out, true);
      break;
    case StmtKind::Func:
      if (include_nested_funcs) out.insert(s.func->name);
      break;
    case StmtKind::Class:
      out.insert(s.name);
      break;
    default:
      break;
  }
}

void collect_decl_names(const Stmt& s, std::unordered_set<std::string>& globals,
                        std::unordered_set<std::string>& nonlocals) {
  switch (s.kind) {
    case StmtKind::GlobalDecl:
      for (const std::string& n : s.names) globals.insert(n);
      break;
    case StmtKind::NonlocalDecl:
      for (const std::string& n : s.names) nonlocals.insert(n);
      break;
    case StmtKind::If:
      for (const std::vector<Stmt>& body : s.bodies)
        for (const Stmt& b : body) collect_decl_names(b, globals, nonlocals);
      if (s.has_else)
        for (const Stmt& b : s.else_body)
          collect_decl_names(b, globals, nonlocals);
      break;
    case StmtKind::While:
    case StmtKind::For:
      for (const Stmt& b : s.body) collect_decl_names(b, globals, nonlocals);
      break;
    case StmtKind::Switch:
      for (const CaseArm& arm : s.cases)
        for (const Stmt& b : arm.body)
          collect_decl_names(b, globals, nonlocals);
      break;
    case StmtKind::Try:
      for (const Stmt& b : s.body) collect_decl_names(b, globals, nonlocals);
      for (const ExceptArm& arm : s.arms)
        for (const Stmt& b : arm.body)
          collect_decl_names(b, globals, nonlocals);
      for (const Stmt& b : s.finally_body)
        collect_decl_names(b, globals, nonlocals);
      break;
    default:
      break;
  }
}

// Top-level bindings of a module (mirrors semantic.cpp collect_globals, plus
// function and class declarations). These are the names the linker renames
// and exposes as imports.
void collect_module_bindings(const Program& prog,
                             std::unordered_set<std::string>& out) {
  for (const TopLevel& item : prog.items) {
    if (item.kind == TopKind::Func) {
      out.insert(item.func->name);
    } else if (item.kind == TopKind::Main) {
      for (const Stmt& s : item.main->body) collect_stmt_bindings(s, out, true);
    } else if (item.kind == TopKind::Stmt) {
      collect_stmt_bindings(item.stmt, out, true);
    }
  }
}

bool expr_is_pure(const Expr& e) {
  switch (e.kind) {
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
    case ExprKind::StrLit:
    case ExprKind::BytesLit:
    case ExprKind::BoolLit:
    case ExprKind::NoneLit:
    case ExprKind::Var:
      return true;
    case ExprKind::Unary:
      return e.lhs != nullptr && expr_is_pure(*e.lhs);
    case ExprKind::Binary:
    case ExprKind::Logical:
      return e.lhs != nullptr && e.rhs != nullptr && expr_is_pure(*e.lhs) &&
             expr_is_pure(*e.rhs);
    case ExprKind::Ternary:
      return e.lhs != nullptr && e.rhs != nullptr && e.third != nullptr &&
             expr_is_pure(*e.lhs) && expr_is_pure(*e.rhs) &&
             expr_is_pure(*e.third);
    case ExprKind::ListLit:
    case ExprKind::SetLit:
    case ExprKind::TupleLit:
    case ExprKind::Join:
      for (const ExprPtr& x : e.elems)
        if (x == nullptr || !expr_is_pure(*x)) return false;
      return true;
    case ExprKind::MapLit:
      for (const ExprPtr& x : e.keys)
        if (x == nullptr || !expr_is_pure(*x)) return false;
      for (const ExprPtr& x : e.elems)
        if (x == nullptr || !expr_is_pure(*x)) return false;
      return true;
    // Attribute/index/slice reads can raise; calls and method calls can have
    // arbitrary effects. A global with such an initializer is treated as
    // module-initialization code and is never dropped.
    default:
      return false;
  }
}

bool item_is_prunable_decl(const TopLevel& item,
                           std::vector<std::string>& declares) {
  if (item.kind == TopKind::Func) {
    declares.push_back(item.func->name);
    return true;
  }
  if (item.kind == TopKind::Stmt && item.stmt.kind == StmtKind::Class) {
    declares.push_back(item.stmt.name);
    return true;
  }
  if (item.kind == TopKind::Stmt && item.stmt.kind == StmtKind::Assign &&
      item.stmt.op.empty() &&
      item.stmt.target.kind == AssignTarget::Kind::Name &&
      item.stmt.expr != nullptr && expr_is_pure(*item.stmt.expr)) {
    declares.push_back(item.stmt.name);
    return true;
  }
  return false;
}

void collect_expr_refs(const Expr& e, std::unordered_set<std::string>& out);

void collect_stmt_refs(const Stmt& s, std::unordered_set<std::string>& out) {
  switch (s.kind) {
    case StmtKind::Assign:
      if (s.target.kind == AssignTarget::Kind::Name ||
          s.target.kind == AssignTarget::Kind::Rest) {
        out.insert(s.target.name);
      } else if (s.target.kind == AssignTarget::Kind::VarQual) {
        out.insert(s.target.name);
        if (s.target.attr != "global" && s.target.attr != "parent")
          out.insert(s.target.attr);
      } else if (s.target.kind == AssignTarget::Kind::Tuple) {
        std::unordered_set<std::string> names;
        collect_target_names(s.target, names);
        out.insert(names.begin(), names.end());
      }
      if (s.target.object) collect_expr_refs(*s.target.object, out);
      if (s.target.index) collect_expr_refs(*s.target.index, out);
      if (s.expr) collect_expr_refs(*s.expr, out);
      break;
    case StmtKind::ExprStmt:
    case StmtKind::Return:
    case StmtKind::Raise:
      if (s.expr) collect_expr_refs(*s.expr, out);
      break;
    case StmtKind::If:
      for (const ExprPtr& c : s.conds)
        if (c) collect_expr_refs(*c, out);
      for (const std::vector<Stmt>& body : s.bodies)
        for (const Stmt& b : body) collect_stmt_refs(b, out);
      if (s.has_else)
        for (const Stmt& b : s.else_body) collect_stmt_refs(b, out);
      break;
    case StmtKind::While:
      if (s.expr) collect_expr_refs(*s.expr, out);
      for (const Stmt& b : s.body) collect_stmt_refs(b, out);
      break;
    case StmtKind::For:
      out.insert(s.name);
      if (s.expr) collect_expr_refs(*s.expr, out);
      for (const Stmt& b : s.body) collect_stmt_refs(b, out);
      break;
    case StmtKind::Switch:
      if (s.expr) collect_expr_refs(*s.expr, out);
      for (const CaseArm& arm : s.cases)
        for (const Stmt& b : arm.body) collect_stmt_refs(b, out);
      break;
    case StmtKind::GlobalDecl:
      out.insert(s.names.begin(), s.names.end());
      break;
    case StmtKind::Func:
      for (const ExprPtr& d : s.func->defaults)
        if (d) collect_expr_refs(*d, out);
      for (const Stmt& b : s.func->body) collect_stmt_refs(b, out);
      break;
    case StmtKind::Try:
      for (const Stmt& b : s.body) collect_stmt_refs(b, out);
      for (const ExceptArm& arm : s.arms) {
        if (arm.cond) collect_expr_refs(*arm.cond, out);
        if (!arm.name.empty()) out.insert(arm.name);
        for (const Stmt& b : arm.body) collect_stmt_refs(b, out);
      }
      for (const Stmt& b : s.finally_body) collect_stmt_refs(b, out);
      break;
    case StmtKind::Class:
      for (const FuncDecl& m : s.class_methods) {
        for (const ExprPtr& d : m.defaults)
          if (d) collect_expr_refs(*d, out);
        for (const Stmt& b : m.body) collect_stmt_refs(b, out);
      }
      break;
    default:
      break;
  }
}

void collect_expr_refs(const Expr& e, std::unordered_set<std::string>& out) {
  switch (e.kind) {
    case ExprKind::Var:
      out.insert(e.name);
      break;
    case ExprKind::VarQual:
      out.insert(e.name);
      if (e.attr != "global" && e.attr != "parent") out.insert(e.attr);
      break;
    case ExprKind::Call:
      if (!e.callee.empty()) out.insert(e.callee);
      if (e.callee_expr) collect_expr_refs(*e.callee_expr, out);
      for (const ExprPtr& a : e.args)
        if (a) collect_expr_refs(*a, out);
      break;
    default:
      break;
  }
  if (e.lhs) collect_expr_refs(*e.lhs, out);
  if (e.rhs) collect_expr_refs(*e.rhs, out);
  if (e.third) collect_expr_refs(*e.third, out);
  if (e.fourth) collect_expr_refs(*e.fourth, out);
  for (const ExprPtr& x : e.elems)
    if (x) collect_expr_refs(*x, out);
  for (const ExprPtr& x : e.keys)
    if (x) collect_expr_refs(*x, out);
  // MethodCall/Attribute names are not declarations; only their objects can
  // reference a module-level name.
}

// ---------------------------------------------------------------------------
// Linker
// ---------------------------------------------------------------------------

class Linker {
 public:
  Linker(const std::string& entry_path, bool strict, LinkResult& result,
         Diagnostics& diags)
      : entry_path_(entry_path), strict_(strict), result_(result),
        diags_(diags) {}

  bool run(Program& entry) {
    if (!expand_entry(entry)) return false;
    flatten(entry);
    if (!strict_) prune(entry);
    return true;
  }

 private:
  // ---- resolution --------------------------------------------------------
  std::string resolve_module(const std::string& spec,
                             const std::string& importer_dir) {
    std::string rel = module_relative_path(spec);
    std::vector<std::string> roots;
    roots.push_back(importer_dir);
    char cwd[PATH_MAX];
    if (::getcwd(cwd, sizeof cwd) != nullptr) roots.push_back(cwd);
    const char* env = std::getenv("PENTEST_PATH");
    if (env != nullptr) {
      std::string paths(env);
      std::size_t start = 0;
      while (start <= paths.size()) {
        std::size_t colon = paths.find(':', start);
        std::string p = (colon == std::string::npos)
                            ? paths.substr(start)
                            : paths.substr(start, colon - start);
        if (!p.empty()) roots.push_back(p);
        if (colon == std::string::npos) break;
        start = colon + 1;
      }
    }
    for (const std::string& root : roots) {
      if (root.empty()) continue;
      const std::string base =
          path_is_absolute(root) ? root : path_join(std::string("."), root);
      std::vector<std::string> candidates = {
          path_join(base, rel + ".pen"), path_join(base, rel + ".pent"),
          path_join(path_join(base, rel), "__init__.pen"),
          path_join(path_join(base, rel), "__init__.pent")};
      for (const std::string& c : candidates) {
        std::string canon = canonical_path(c);
        if (!canon.empty()) return canon;
      }
    }
    return std::string();
  }

  Module* load_module(const std::string& spec, const std::string& importer_dir,
                      const SourceLoc& loc) {
    const std::string path = resolve_module(spec, importer_dir);
    if (path.empty()) {
      diags_.error(loc, "cannot find module '" + spec + "'");
      return nullptr;
    }
    auto it = modules_.find(path);
    if (it != modules_.end()) {
      Module* m = it->second.get();
      if (m->loading) {
        diags_.error(loc, "circular import: '" + spec +
                              "' is already being loaded");
        return nullptr;
      }
      if (!m->expanded) {
        diags_.error(loc, "circular import involving module '" + spec + "'");
        return nullptr;
      }
      return m;
    }

    auto mod = std::make_unique<Module>();
    mod->path = path;
    mod->dir = path_dirname(path);
    mod->id = next_id_++;
    if (!read_source(path, mod->source)) {
      diags_.error(loc, "cannot read module '" + spec + "' (" + path + "): " +
                            std::strerror(errno));
      return nullptr;
    }
    diags_.set_source(path, mod->source);
    mod->prog.file_path = path;
    if (!parse_source(path, mod->source, mod->prog, diags_)) return nullptr;
    for (TopLevel& item : mod->prog.items) {
      item.origin = path;
      item.imported = true;
    }

    // Internal names for every top-level binding of this module.
    std::unordered_set<std::string> names;
    collect_module_bindings(mod->prog, names);
    for (const std::string& n : names) {
      mod->rename[n] = "__ptm" + std::to_string(mod->id) + "_" + n;
    }

    Module* raw = mod.get();
    modules_[path] = std::move(mod);

    raw->loading = true;
    const bool ok = expand_module(*raw);
    raw->loading = false;
    if (!ok) return nullptr;
    raw->expanded = true;

    result_.dependency_paths.push_back(path);
    result_.dependency_blob.append(path);
    result_.dependency_blob.push_back('\0');
    result_.dependency_blob.append(raw->source);
    result_.dependency_blob.push_back('\0');
    return raw;
  }

  // ---- expansion ---------------------------------------------------------
  bool expand_entry(Program& entry) {
    std::unordered_set<std::string> own_names;
    collect_module_bindings(entry, own_names);
    LinkMaps maps;
    maps.own = nullptr;
    maps.own_names = std::move(own_names);
    if (!process_imports(entry, path_dirname(entry_path_), maps)) return false;
    rename_own(entry, maps, /*is_module=*/false);
    return true;
  }

  bool expand_module(Module& mod) {
    LinkMaps maps;
    maps.own = &mod.rename;
    collect_module_bindings(mod.prog, maps.own_names);
    if (!process_imports(mod.prog, mod.dir, maps)) return false;
    rename_own(mod.prog, maps, /*is_module=*/true);
    // An imported module is not the entry point: its main: block is ignored
    // (Stage 2D contract). Remove it before flattening.
    std::vector<TopLevel> kept;
    kept.reserve(mod.prog.items.size());
    for (TopLevel& item : mod.prog.items) {
      if (item.kind == TopKind::Main) continue;
      kept.push_back(std::move(item));
    }
    mod.prog.items = std::move(kept);
    return true;
  }

  bool process_imports(Program& prog, const std::string& dir, LinkMaps& maps) {
    for (TopLevel& item : prog.items) {
      if (item.kind != TopKind::Import) continue;
      ImportDecl& imp = *item.import;
      Module* m = load_module(imp.module, dir, imp.loc);
      if (m == nullptr) return false;
      ImportBinding bind;
      bind.module = m;

      if (imp.kind == ImportDecl::Kind::Module) {
        std::string alias = imp.alias.empty() ? last_component(imp.module)
                                              : imp.alias;
        if (maps.own_names.count(alias) != 0) {
          diags_.error(imp.loc, "import alias '" + alias +
                                    "' conflicts with a declaration in this "
                                    "module");
          return false;
        }
        if (maps.from.count(alias) != 0) {
          diags_.error(imp.loc, "import alias '" + alias +
                                    "' conflicts with an imported name");
          return false;
        }
        auto it = maps.aliases.find(alias);
        if (it != maps.aliases.end() && it->second != m) {
          diags_.error(imp.loc, "two modules are imported with the alias '" +
                                    alias + "'");
          return false;
        }
        maps.aliases[alias] = m;
        bind.has_alias = true;
        bind.alias = alias;
      } else {
        for (const ImportName& n : imp.names) {
          if (is_private_name(n.name)) {
            diags_.error(n.loc, "cannot import private name '" + n.name +
                                    "' from module '" + imp.module + "'");
            return false;
          }
          auto it = m->rename.find(n.name);
          if (it == m->rename.end()) {
            diags_.error(n.loc, "module '" + imp.module +
                                    "' has no declaration '" + n.name + "'");
            return false;
          }
          const std::string bound = n.alias.empty() ? n.name : n.alias;
          if (maps.own_names.count(bound) != 0) {
            diags_.error(n.loc, "imported name '" + bound +
                                    "' conflicts with a declaration in this "
                                    "module");
            return false;
          }
          if (maps.aliases.count(bound) != 0) {
            diags_.error(n.loc, "imported name '" + bound +
                                    "' conflicts with a module alias");
            return false;
          }
          auto prev = maps.from.find(bound);
          if (prev != maps.from.end() && prev->second != it->second) {
            diags_.error(n.loc, "imported name '" + bound +
                                    "' is already imported from another "
                                    "module");
            return false;
          }
          maps.from[bound] = it->second;
        }
      }
      bindings_[&imp] = std::move(bind);
    }
    return true;
  }

  void rename_own(Program& prog, LinkMaps& maps, bool is_module) {
    for (TopLevel& item : prog.items) {
      if (item.kind == TopKind::Import) continue;
      if (item.kind == TopKind::Func) {
        if (is_module) {
          auto it = maps.own->find(item.func->name);
          if (it != maps.own->end()) item.func->name = it->second;
        }
        Scope module;
        module.module_level = true;
        rename_function(*item.func, module, maps);
      } else if (item.kind == TopKind::Main) {
        Scope module;
        module.module_level = true;
        for (Stmt& s : item.main->body) rename_stmt(s, module, maps);
      } else if (item.kind == TopKind::Stmt) {
        Scope module;
        module.module_level = true;
        rename_stmt(item.stmt, module, maps);
      }
    }
  }

  // ---- individual rename helpers ----------------------------------------
  std::string rename_read(const std::string& name, Scope& scope,
                          LinkMaps& maps) {
    if (scope.has_local(name)) return name;
    if (scope.is_global_decl(name)) {
      // fall through to the module map
    }
    auto f = maps.from.find(name);
    if (f != maps.from.end()) return f->second;
    if (maps.own != nullptr) {
      auto o = maps.own->find(name);
      if (o != maps.own->end()) return o->second;
    }
    return name;
  }

  std::string rename_target(const std::string& name, Scope& scope,
                            LinkMaps& maps, const SourceLoc& loc,
                            bool allow_imported = false) {
    if (!scope.module_level && scope.has_local(name)) return name;
    if (maps.aliases.count(name) != 0) {
      diags_.error(loc, "cannot assign to module alias '" + name + "'");
      return name;
    }
    auto f = maps.from.find(name);
    if (f != maps.from.end()) {
      if (!allow_imported) {
        diags_.error(loc, "cannot assign to imported name '" + name + "'");
      }
      return f->second;
    }
    if (maps.own != nullptr) {
      auto o = maps.own->find(name);
      if (o != maps.own->end()) return o->second;
    }
    return name;
  }

  std::string internal_export(Module* m, const std::string& name,
                              const SourceLoc& loc) {
    if (is_private_name(name)) {
      diags_.error(loc, "'" + name + "' is private in module '" +
                            display_ident(m->path) + "'");
      return name;
    }
    auto it = m->rename.find(name);
    if (it == m->rename.end()) {
      diags_.error(loc, "module '" + m->path + "' has no declaration '" +
                            name + "'");
      return name;
    }
    return it->second;
  }

  void rewrite_alias_attr(Expr& e, Scope& scope, LinkMaps& maps) {
    // e is Attr(Var(alias), name); the caller has checked the shape.
    Expr& obj = *e.lhs;
    auto a = maps.aliases.find(obj.name);
    if (a == maps.aliases.end()) return;
    if (scope.has_local(obj.name)) return;
    const std::string target = internal_export(a->second, e.attr, e.loc);
    const SourceLoc loc = e.loc;
    e.kind = ExprKind::Var;
    e.name = target;
    e.lhs.reset();
    e.rhs.reset();
    e.third.reset();
    e.fourth.reset();
    e.attr.clear();
    e.op.clear();
    e.callee.clear();
    e.callee_expr.reset();
    e.method.clear();
    e.args.clear();
    e.arg_names.clear();
    e.elems.clear();
    e.keys.clear();
    e.elem_names.clear();
    e.loc = loc;
  }

  void rewrite_alias_method(Expr& e, Scope& scope, LinkMaps& maps) {
    // MethodCall(Var(alias), method, args) -> Call(method-internal, args).
    Expr& obj = *e.lhs;
    auto a = maps.aliases.find(obj.name);
    if (a == maps.aliases.end()) return;
    if (scope.has_local(obj.name)) return;
    const std::string target = internal_export(a->second, e.method, e.loc);
    std::vector<ExprPtr> args = std::move(e.elems);
    std::vector<std::string> names = std::move(e.elem_names);
    const SourceLoc loc = e.loc;
    e.kind = ExprKind::Call;
    e.callee = target;
    e.callee_expr = std::make_unique<Expr>();
    e.callee_expr->kind = ExprKind::Var;
    e.callee_expr->loc = loc;
    e.callee_expr->name = target;
    e.args = std::move(args);
    e.arg_names = std::move(names);
    e.lhs.reset();
    e.elems.clear();
    e.elem_names.clear();
    e.method.clear();
  }

  void rename_expr(Expr& e, Scope& scope, LinkMaps& maps) {
    switch (e.kind) {
      case ExprKind::Var: {
        if (maps.aliases.count(e.name) != 0 && !scope.has_local(e.name)) {
          diags_.error(e.loc, "module alias '" + e.name +
                                  "' cannot be used as a value; write " +
                                  e.name + ".name");
          return;
        }
        e.name = rename_read(e.name, scope, maps);
        return;
      }
      case ExprKind::VarQual: {
        if (e.attr == "global") {
          e.name = rename_read(e.name, scope, maps);
        } else if (e.attr != "parent") {
          // qualifier is a sibling/self function name
          e.attr = rename_read(e.attr, scope, maps);
        }
        return;
      }
      case ExprKind::Attr:
        if (e.lhs != nullptr && e.lhs->kind == ExprKind::Var &&
            maps.aliases.count(e.lhs->name) != 0 &&
            !scope.has_local(e.lhs->name)) {
          rewrite_alias_attr(e, scope, maps);
          return;
        }
        if (e.lhs) rename_expr(*e.lhs, scope, maps);
        return;
      case ExprKind::MethodCall:
        if (e.lhs != nullptr && e.lhs->kind == ExprKind::Var &&
            maps.aliases.count(e.lhs->name) != 0 &&
            !scope.has_local(e.lhs->name)) {
          // Rename arguments before turning the method call into a call.
          for (ExprPtr& a : e.elems)
            if (a) rename_expr(*a, scope, maps);
          rewrite_alias_method(e, scope, maps);
          return;
        }
        if (e.lhs) rename_expr(*e.lhs, scope, maps);
        for (ExprPtr& a : e.elems)
          if (a) rename_expr(*a, scope, maps);
        return;
      case ExprKind::Call:
        if (e.callee_expr) rename_expr(*e.callee_expr, scope, maps);
        if (e.callee_expr != nullptr &&
            e.callee_expr->kind == ExprKind::Var) {
          e.callee = e.callee_expr->name;
        } else if (!e.callee.empty()) {
          e.callee = rename_read(e.callee, scope, maps);
        }
        for (ExprPtr& a : e.args)
          if (a) rename_expr(*a, scope, maps);
        return;
      default:
        break;
    }
    if (e.lhs) rename_expr(*e.lhs, scope, maps);
    if (e.rhs) rename_expr(*e.rhs, scope, maps);
    if (e.third) rename_expr(*e.third, scope, maps);
    if (e.fourth) rename_expr(*e.fourth, scope, maps);
    if (e.callee_expr) rename_expr(*e.callee_expr, scope, maps);
    for (ExprPtr& a : e.args)
      if (a) rename_expr(*a, scope, maps);
    for (ExprPtr& x : e.elems)
      if (x) rename_expr(*x, scope, maps);
    for (ExprPtr& x : e.keys)
      if (x) rename_expr(*x, scope, maps);
  }

  void rename_target(AssignTarget& t, Scope& scope, LinkMaps& maps) {
    switch (t.kind) {
      case AssignTarget::Kind::Name:
      case AssignTarget::Kind::Rest:
        t.name = rename_target(t.name, scope, maps, t.loc);
        break;
      case AssignTarget::Kind::Tuple:
        for (AssignTarget& e : t.elems) rename_target(e, scope, maps);
        break;
      case AssignTarget::Kind::Index:
        if (t.object) rename_expr(*t.object, scope, maps);
        if (t.index) rename_expr(*t.index, scope, maps);
        break;
      case AssignTarget::Kind::Attr:
        if (t.object != nullptr && t.object->kind == ExprKind::Var &&
            maps.aliases.count(t.object->name) != 0 &&
            !scope.has_local(t.object->name)) {
          const std::string target =
              internal_export(maps.aliases[t.object->name], t.attr, t.loc);
          t.kind = AssignTarget::Kind::Name;
          t.name = target;
          t.object.reset();
          t.index.reset();
          t.attr.clear();
          return;
        }
        if (t.object) rename_expr(*t.object, scope, maps);
        break;
      case AssignTarget::Kind::VarQual:
        if (t.attr == "global") {
          t.name = rename_target(t.name, scope, maps, t.loc);
        } else if (t.attr != "parent") {
          t.attr = rename_read(t.attr, scope, maps);
        }
        break;
    }
  }

  void collect_function_scope(const FuncDecl& f, Scope& scope) {
    std::unordered_set<std::string> bindings;
    std::unordered_set<std::string> globals;
    std::unordered_set<std::string> nonlocals;
    bindings.insert(f.params.begin(), f.params.end());
    if (f.has_vararg) bindings.insert(f.vararg);
    if (f.has_kwarg) bindings.insert(f.kwarg);
    for (const Stmt& s : f.body) {
      collect_decl_names(s, globals, nonlocals);
    }
    for (const Stmt& s : f.body) collect_stmt_bindings(s, bindings, true);
    scope.locals.clear();
    scope.global_names = std::move(globals);
    for (const std::string& n : bindings) {
      if (scope.global_names.count(n) == 0 && nonlocals.count(n) == 0)
        scope.locals.insert(n);
    }
  }

  void rename_function(FuncDecl& f, const Scope& enclosing, LinkMaps& maps) {
    for (ExprPtr& d : f.defaults)
      if (d) rename_expr(*d, const_cast<Scope&>(enclosing), maps);
    Scope inner;
    inner.parent = &enclosing;
    inner.module_level = false;
    collect_function_scope(f, inner);
    for (Stmt& s : f.body) rename_stmt(s, inner, maps);
  }

  void rename_stmt(Stmt& s, Scope& scope, LinkMaps& maps) {
    switch (s.kind) {
      case StmtKind::Assign:
        rename_target(s.target, scope, maps);
        if (s.target.kind == AssignTarget::Kind::Name) s.name = s.target.name;
        if (s.expr) rename_expr(*s.expr, scope, maps);
        break;
      case StmtKind::ExprStmt:
      case StmtKind::Return:
      case StmtKind::Raise:
        if (s.expr) rename_expr(*s.expr, scope, maps);
        break;
      case StmtKind::If:
        for (ExprPtr& c : s.conds)
          if (c) rename_expr(*c, scope, maps);
        for (std::vector<Stmt>& body : s.bodies)
          for (Stmt& b : body) rename_stmt(b, scope, maps);
        if (s.has_else)
          for (Stmt& b : s.else_body) rename_stmt(b, scope, maps);
        break;
      case StmtKind::While:
        if (s.expr) rename_expr(*s.expr, scope, maps);
        for (Stmt& b : s.body) rename_stmt(b, scope, maps);
        break;
      case StmtKind::For:
        s.name = rename_target(s.name, scope, maps, s.loc);
        if (s.expr) rename_expr(*s.expr, scope, maps);
        for (Stmt& b : s.body) rename_stmt(b, scope, maps);
        break;
      case StmtKind::Switch:
        if (s.expr) rename_expr(*s.expr, scope, maps);
        for (CaseArm& arm : s.cases)
          for (Stmt& b : arm.body) rename_stmt(b, scope, maps);
        break;
      case StmtKind::GlobalDecl:
        for (std::string& n : s.names) {
          if (maps.from.count(n) != 0) {
            diags_.error(s.loc, "cannot declare imported name '" + n +
                                    "' global");
            continue;
          }
          n = rename_read(n, scope, maps);
        }
        break;
      case StmtKind::NonlocalDecl:
        break;
      case StmtKind::Func: {
        FuncDecl& f = *s.func;
        if (scope.module_level) {
          if (maps.own != nullptr) {
            auto it = maps.own->find(f.name);
            if (it != maps.own->end()) f.name = it->second;
          }
        }
        rename_function(f, scope, maps);
        break;
      }
      case StmtKind::Try:
        for (Stmt& b : s.body) rename_stmt(b, scope, maps);
        for (ExceptArm& arm : s.arms) {
          if (arm.cond) rename_expr(*arm.cond, scope, maps);
          if (!arm.name.empty())
            arm.name = rename_target(arm.name, scope, maps, arm.loc);
          for (Stmt& b : arm.body) rename_stmt(b, scope, maps);
        }
        for (Stmt& b : s.finally_body) rename_stmt(b, scope, maps);
        break;
      case StmtKind::Class: {
        if (scope.module_level && maps.own != nullptr) {
          auto it = maps.own->find(s.name);
          if (it != maps.own->end()) s.name = it->second;
        }
        Scope module;
        module.module_level = true;
        for (FuncDecl& m : s.class_methods) rename_function(m, module, maps);
        for (Stmt& b : s.body) rename_stmt(b, module, maps);
        break;
      }
      default:
        break;
    }
  }

  // ---- flatten and prune -------------------------------------------------
  void flatten(Program& entry) {
    std::vector<TopLevel> out;
    out.reserve(entry.items.size());
    std::unordered_set<Module*> emitted;
    flatten_items(entry.items, out, emitted);
    entry.items = std::move(out);
  }

  void flatten_items(std::vector<TopLevel>& items, std::vector<TopLevel>& out,
                     std::unordered_set<Module*>& emitted) {
    for (TopLevel& item : items) {
      if (item.kind == TopKind::Import) {
        auto it = bindings_.find(item.import.get());
        if (it == bindings_.end() || it->second.module == nullptr) continue;
        Module* m = it->second.module;
        if (emitted.insert(m).second)
          flatten_items(m->prog.items, out, emitted);
        continue;
      }
      out.push_back(std::move(item));
    }
  }

  // Reachability over imported declarations. Entry items are roots; imported
  // module top-level code is a root unless it is a pure declaration that
  // nothing references. Scanning an item may retain more declarations; the
  // worklist runs once per retained item, so the pass is O(AST size).
  void prune(Program& prog) {
    const std::size_t n = prog.items.size();
    std::vector<bool> keep(n, false);
    std::vector<std::string> declared;
    declared.reserve(4);
    std::unordered_map<std::string, std::vector<std::size_t>> decl_ix;
    std::vector<std::size_t> work;

    for (std::size_t i = 0; i < n; ++i) {
      TopLevel& item = prog.items[i];
      declared.clear();
      const bool prunable = item_is_prunable_decl(item, declared);
      if (!item.imported || !prunable) {
        keep[i] = true;
        work.push_back(i);
        if (item.imported) {
          // An impure top-level initializer is module initialization code;
          // its bound names are still declarations other code can reference.
          for (const std::string& d : declared) decl_ix[d].push_back(i);
        }
      } else {
        for (const std::string& d : declared) decl_ix[d].push_back(i);
      }
    }

    while (!work.empty()) {
      const std::size_t i = work.back();
      work.pop_back();
      std::unordered_set<std::string> refs;
      TopLevel& item = prog.items[i];
      if (item.kind == TopKind::Func) {
        for (const ExprPtr& d : item.func->defaults)
          if (d) collect_expr_refs(*d, refs);
        for (const Stmt& b : item.func->body) collect_stmt_refs(b, refs);
      } else if (item.kind == TopKind::Main) {
        for (const Stmt& b : item.main->body) collect_stmt_refs(b, refs);
      } else if (item.kind == TopKind::Stmt) {
        collect_stmt_refs(item.stmt, refs);
      }
      for (const std::string& r : refs) {
        auto it = decl_ix.find(r);
        if (it == decl_ix.end()) continue;
        for (std::size_t j : it->second) {
          if (!keep[j]) {
            keep[j] = true;
            work.push_back(j);
          }
        }
      }
    }

    std::vector<TopLevel> kept;
    kept.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      if (keep[i]) kept.push_back(std::move(prog.items[i]));
    }
    prog.items = std::move(kept);
  }

  std::string entry_path_;
  bool strict_;
  LinkResult& result_;
  Diagnostics& diags_;
  std::unordered_map<std::string, std::unique_ptr<Module>> modules_;
  std::unordered_map<const ImportDecl*, ImportBinding> bindings_;
  int next_id_ = 1;
};

}  // namespace

bool link_imports(Program& program, const std::string& entry_path, bool strict,
                  LinkResult& result, Diagnostics& diags) {
  // Fast path: a single-file program pays nothing for the import layer. No
  // traversal, no AST copy, no reachability pass. This keeps the feature's
  // compile-time cost at zero for the common case.
  bool has_imports = false;
  for (const TopLevel& item : program.items) {
    if (item.kind == TopKind::Import) {
      has_imports = true;
      break;
    }
  }
  if (!has_imports) return true;

  Linker linker(entry_path, strict, result, diags);
  if (!linker.run(program)) return false;
  // Keep unlocated diagnostics on the entry file after parsing dependencies.
  return !diags.has_error();
}

}  // namespace pentest
