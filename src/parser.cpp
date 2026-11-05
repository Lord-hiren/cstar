// parser.cpp — cstar M2 Stage 2B recursive-descent parser
// (docs/contracts/m2b-semantics.md §3/§6/§7; M2-2A §2 and M1 §3 elsewhere;
// M2-2C §2.1 adds try/except/finally and raise, §3.1/§3.4 add class
// declarations, attribute reads/assignment and `is`/`is not`).
#include "parser.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "frontend.h"  // parse_source (contract interface)
#include "lexer.h"

namespace cstar {

namespace {

// True for tokens that may begin an expression (M2-2B §3 primary: literals
// incl. Bytes, collection literals, parens; unary 'not'/'-'/~).
bool starts_expr(TokKind k) {
  switch (k) {
    case TokKind::Int:
    case TokKind::Float:
    case TokKind::String:
    case TokKind::Bytes:
    case TokKind::KwTrue:
    case TokKind::KwFalse:
    case TokKind::KwNone:
    case TokKind::KwNot:
    case TokKind::Minus:
    case TokKind::Tilde:
    case TokKind::LParen:
    case TokKind::LBracket:
    case TokKind::LBrace:
      return true;
    default:
      return false;
  }
}

// True for the compound statement keywords (M2-2A §2; M2-2C §2.1 adds
// `try`, whose try body is block-form only; §3.1 adds `class`, likewise
// block-form). A single-line suite holds
// exactly one simple statement, so these are rejected right after a ':' on
// the same line.
bool starts_compound(TokKind k) {
  switch (k) {
    case TokKind::KwIf:
    case TokKind::KwWhile:
    case TokKind::KwFor:
    case TokKind::KwSwitch:
    case TokKind::KwTry:
    case TokKind::KwClass:
      return true;
    default:
      return false;
  }
}

// Compound statements do not leave a NEWLINE token for their caller: the
// block form eats the last newline inside the block, and a single-line
// suite consumes its own. Simple statements still require the trailing
// NEWLINE (M1 behavior — two statements on one line stay an error).
// M2-2B: a nested `function` declaration (StmtKind::Func) is "compound" in
// this sense — its own block body consumes everything through the DEDENT,
// so no trailing NEWLINE is left for the caller.
bool is_compound(const Stmt& s) {
  switch (s.kind) {
    case StmtKind::If:
    case StmtKind::While:
    case StmtKind::For:
    case StmtKind::Switch:
    case StmtKind::Func:
    case StmtKind::Try:  // M2-2C §2.1: block-form try body
    case StmtKind::Class:  // M2-2C §3.1: block-form class body
      return true;
    default:
      return false;
  }
}

ExprPtr make_binary(std::string op, ExprPtr lhs, ExprPtr rhs, SourceLoc loc) {
  auto e = std::make_unique<Expr>();
  e->kind = ExprKind::Binary;
  e->loc = loc;
  e->op = std::move(op);
  e->lhs = std::move(lhs);
  e->rhs = std::move(rhs);
  return e;
}

// "and" / "or" — short-circuit logical op returning bool (M2-2A §4).
ExprPtr make_logical(std::string op, ExprPtr lhs, ExprPtr rhs, SourceLoc loc) {
  auto e = std::make_unique<Expr>();
  e->kind = ExprKind::Logical;
  e->loc = loc;
  e->op = std::move(op);
  e->lhs = std::move(lhs);
  e->rhs = std::move(rhs);
  return e;
}

class Parser {
 public:
  Parser(const std::vector<Token>& toks, Program& prog, Diagnostics& diags)
      : toks_(toks), prog_(prog), diags_(diags) {}

  bool parse_program() {
    prog_.items.clear();
    while (true) {
      if (at(TokKind::Newline)) {
        advance();
        continue;
      }
      if (at(TokKind::Eof)) break;
      if (at(TokKind::KwImport) || at(TokKind::KwFrom)) {
        TopLevel tl;
        tl.kind = TopKind::Import;
        if (!parse_import_decl(tl.import)) return false;
        prog_.items.push_back(std::move(tl));
        if (!expect(TokKind::Newline)) return false;
      } else if (at(TokKind::KwMain)) {
        TopLevel tl;
        tl.kind = TopKind::Main;
        if (!parse_main_decl(tl.main)) return false;
        prog_.items.push_back(std::move(tl));
      } else if (at(TokKind::Ident) && at_decl_start()) {
        // v0.3: a top-level function declaration is `name(params):` (always
        // parenthesised — `name():` for zero parameters; the `function`
        // keyword is gone). Only an Ident that begins a declaration lands
        // here; every other Ident statement (assignment, call, join) falls
        // through to parse_stmt.
        TopLevel tl;
        tl.kind = TopKind::Func;
        if (!parse_func_decl(tl.func)) return false;
        prog_.items.push_back(std::move(tl));
      } else {
        TopLevel tl;
        tl.kind = TopKind::Stmt;
        if (!parse_stmt(tl.stmt)) return false;
        bool simple = !is_compound(tl.stmt);
        prog_.items.push_back(std::move(tl));
        // Every simple statement ends at a newline (synthesized at EOF if
        // missing); a compound statement already consumed its own.
        if (simple && !expect(TokKind::Newline)) return false;
      }
    }
    return true;
  }

 private:
  // ---- modules/imports (Stage 2D) ---------------------------------------
  // import_decl := "import" module_name [ "as" IDENT ]
  //              | "from" module_name "import" import_name { "," import_name }
  // import_name := IDENT [ "as" IDENT ]
  // module_name := IDENT { "." IDENT }
  // Top level only; the linker resolves the file and rewrites references
  // (src/modules.cpp). A trailing NEWLINE is consumed by parse_program.
  bool parse_import_decl(std::unique_ptr<ImportDecl>& out) {
    auto imp = std::make_unique<ImportDecl>();
    imp->loc = cur().loc;
    if (at(TokKind::KwFrom)) {
      imp->kind = ImportDecl::Kind::From;
      advance();  // "from"
      if (!parse_module_name(imp->module)) return false;
      if (!at(TokKind::KwImport))
        return fail(cur(), "expected 'import' after module name");
      advance();  // "import"
      while (true) {
        if (!at(TokKind::Ident))
          return fail(cur(), "expected a name to import");
        ImportName n;
        n.loc = cur().loc;
        n.name = cur().text;
        advance();
        if (at(TokKind::KwAs)) {
          advance();
          if (!at(TokKind::Ident))
            return fail(cur(), "expected alias after 'as'");
          n.alias = cur().text;
          advance();
        }
        imp->names.push_back(std::move(n));
        if (at(TokKind::Comma)) {
          advance();
          continue;
        }
        break;
      }
      if (imp->names.empty())
        return fail(cur(), "expected a name to import");
    } else {
      imp->kind = ImportDecl::Kind::Module;
      advance();  // "import"
      if (!parse_module_name(imp->module)) return false;
      if (at(TokKind::KwAs)) {
        advance();
        if (!at(TokKind::Ident))
          return fail(cur(), "expected alias after 'as'");
        imp->alias = cur().text;
        advance();
      }
    }
    out = std::move(imp);
    return true;
  }

  bool parse_module_name(std::string& out) {
    if (!at(TokKind::Ident))
      return fail(cur(), "expected module name");
    out = cur().text;
    advance();
    while (at(TokKind::Dot)) {
      advance();
      if (!at(TokKind::Ident))
        return fail(cur(), "expected module name after '.'");
      out += ".";
      out += cur().text;
      advance();
    }
    return true;
  }

  // ---- token access ------------------------------------------------------
  const Token& cur() const { return toks_[i_]; }
  const Token& peek(size_t off) const {
    size_t j = i_ + off;
    return j < toks_.size() ? toks_[j] : toks_.back();
  }
  bool at(TokKind k) const { return cur().kind == k; }
  // Token test for the qualifier of a `variable.<q> <name>` atom
  // (docs/reference/cross-scope-access.md §3): any identifier, plus the reserved
  // `global` keyword — which can never be a variable/function name, so
  // `variable.global` is unambiguous.
  static bool qual_tok(const Token& t) {
    return t.kind == TokKind::Ident || t.kind == TokKind::KwGlobal;
  }
  void advance() {
    if (i_ + 1 < toks_.size()) ++i_;  // never moves past Eof
  }

  bool fail(const Token& tok, const std::string& msg) {
    // Speculative target_list parsing (M2-2B statement dispatch) must stay
    // silent: only the confirmed statement path may report (first error
    // wins, caret display).
    if (spec_depth_ == 0) diags_.error(tok.loc, msg);
    return false;
  }

  // Consume `k`, or report `msg` (generic "unexpected token 'x'" when empty).
  bool expect(TokKind k, const std::string& msg = std::string()) {
    if (at(k)) {
      advance();
      return true;
    }
    if (!msg.empty()) return fail(cur(), msg);
    return fail(cur(), "unexpected token '" + token_display(cur()) + "'");
  }

  // ---- declarations ------------------------------------------------------
  // v0.3: `NAME "(" [ params ] ")" ":" block` — always parenthesised, so a
  // zero-parameter function is `name():` (there is no bare `name:` form and
  // no separate call operator; calls and declarations both spell their
  // argument/parameter lists with parens).
  // `main` is KwMain and never reaches here (it parses via parse_main_decl);
  // the `function` keyword is gone and reports a friendly error at the
  // statement-dispatch level. Used for top-level declarations (TopKind::Func)
  // and nested declarations (StmtKind::Func, M2-2B §6.1).
  bool parse_func_decl(std::unique_ptr<FuncDecl>& out) {
    if (!at(TokKind::Ident)) return fail(cur(), "expected function name");
    auto fn = std::make_unique<FuncDecl>();
    fn->name = cur().text;
    fn->loc = cur().loc;
    advance();  // NAME
    if (!expect(TokKind::LParen, "expected '(' after the function name"))
      return false;
    if (!parse_params(*fn)) return false;
    if (!expect(TokKind::RParen, "expected ')' after the parameter list"))
      return false;
    if (!expect(TokKind::Colon, "expected ':'")) return false;
    if (!parse_block_header_and_body(fn->body)) return false;
    out = std::move(fn);
    return true;
  }

  // params := [ param { "," param } [ "," vararg ] [ "," kwarg ]
  //          | vararg [ "," kwarg ] | kwarg ]            (possibly empty)
  // param  := NAME [ "=" expr ]   (a default parses at TERNARY level so the
  // comma keeps separating parameters — §3's `expr` here cannot swallow the
  // param separator). defaults[] stays parallel to params (null = required).
  // The REQUIRED-AFTER-DEFAULT ordering rule is a SEMANTIC check (§6/§11)
  // and is deliberately not enforced here; only the syntactic shape
  // (duplicate/ misplaced '*' and '**') is.
  // v0.3: the parameter list is parenthesised (`name(a, b):`), so an empty
  // list is the zero-parameter form `name():`.
  bool parse_params(FuncDecl& fn) {
    if (at(TokKind::RParen)) return true;  // `name():` — zero parameters
    enum class PState { Positional, Vararg, Kwarg };
    PState state = PState::Positional;
    while (true) {
      if (at(TokKind::Star)) {
        if (state == PState::Kwarg)
          return fail(cur(), "'*name' cannot follow '**name'");
        if (state == PState::Vararg)
          return fail(cur(), "only one '*' parameter is allowed");
        advance();
        if (!at(TokKind::Ident)) return fail(cur(), "expected identifier");
        fn.has_vararg = true;
        fn.vararg = cur().text;
        advance();
        state = PState::Vararg;
      } else if (at(TokKind::StarStar)) {
        if (state == PState::Kwarg)
          return fail(cur(), "only one '**' parameter is allowed");
        advance();
        if (!at(TokKind::Ident)) return fail(cur(), "expected identifier");
        fn.has_kwarg = true;
        fn.kwarg = cur().text;
        advance();
        state = PState::Kwarg;
      } else if (at(TokKind::Ident)) {
        if (state == PState::Vararg)
          return fail(cur(), "positional parameter cannot follow '*name'");
        if (state == PState::Kwarg)
          return fail(cur(), "positional parameter cannot follow '**name'");
        fn.params.push_back(cur().text);
        advance();
        if (at(TokKind::Assign)) {
          advance();
          ExprPtr def;
          if (!parse_ternary(def)) return false;
          fn.defaults.push_back(std::move(def));
        } else {
          fn.defaults.push_back(nullptr);
        }
      } else {
        return fail(cur(), "expected identifier");
      }
      if (at(TokKind::Comma)) {
        advance();
        continue;
      }
      break;
    }
    return true;
  }

  bool parse_main_decl(std::unique_ptr<MainDecl>& out) {
    const Token& kw = cur();
    advance();  // "main"
    auto md = std::make_unique<MainDecl>();
    md->loc = kw.loc;
    if (!expect(TokKind::Colon, "expected ':' after 'main'")) return false;
    if (!parse_block_header_and_body(md->body)) return false;
    out = std::move(md);
    return true;
  }

  // ---- blocks and suites -------------------------------------------------
  // After the ':' of a block header: NEWLINE INDENT. A block must contain at
  // least one statement (M1: "expected an indented block after ':'").
  bool parse_block_header() {
    if (!at(TokKind::Newline))
      return fail(cur(), "expected an indented block after ':'");
    advance();
    if (!at(TokKind::Indent))
      return fail(cur(), "expected an indented block after ':'");
    advance();
    while (at(TokKind::Newline)) advance();
    if (at(TokKind::Dedent) || at(TokKind::Eof))
      return fail(cur(), "expected an indented block after ':'");
    return true;
  }

  // NEWLINE INDENT { NL } stmt { NL | stmt } DEDENT (M1 §3).
  bool parse_block_header_and_body(std::vector<Stmt>& body) {
    if (!parse_block_header()) return false;
    while (true) {
      Stmt s;
      if (!parse_stmt(s)) return false;
      bool simple = !is_compound(s);
      body.push_back(std::move(s));
      if (simple && !expect(TokKind::Newline)) return false;
      while (at(TokKind::Newline)) advance();
      if (at(TokKind::Dedent)) {
        advance();
        break;
      }
      if (at(TokKind::Eof)) break;  // defensive; synthesized DEDENT precedes EOF
    }
    return true;
  }

  // suite := NL INDENT block DEDENT | simple_stmt (M2-2A §2). Used by
  // if/elif/else/while, which allow the single-line form; for, switch and
  // case clauses require the block form and go through
  // parse_block_header[_and_body] directly (whose header check reports
  // "expected an indented block after ':'" for a suite on the same line).
  bool parse_suite(std::vector<Stmt>& body) {
    if (at(TokKind::Newline)) return parse_block_header_and_body(body);
    if (starts_compound(cur().kind))
      return fail(cur(),
                  "expected a simple statement on the same line after ':'");
    Stmt s;
    if (!parse_stmt(s)) return false;
    body.push_back(std::move(s));
    // A single-line body is exactly one simple statement; parse_stmt leaves
    // its trailing newline to the caller, so the suite consumes it here.
    return expect(TokKind::Newline);
  }

  // ---- statements --------------------------------------------------------
  bool parse_stmt(Stmt& s) {
    const Token& tk = cur();
    s.loc = tk.loc;
    switch (tk.kind) {
      case TokKind::KwFunction:
        return fail(tk, "'function' keyword was removed; declare with "
                        "'name():' or 'name(params):'");
      case TokKind::KwMain:
        // A nested `main:` block is only legal at top level; any other use
        // of the keyword (call or read) is a plain syntax error.
        if (peek(1).kind == TokKind::Colon)
          return fail(tk, "nested 'main' block is not allowed");
        return fail(tk, "unexpected token 'main'");
      case TokKind::KwImport:
      case TokKind::KwFrom:
        return fail(tk, "'import' is only allowed at the top level");
      case TokKind::KwGlobal:
        return parse_decl_names(s, StmtKind::GlobalDecl, "global");
      case TokKind::KwNonlocal:
        return parse_decl_names(s, StmtKind::NonlocalDecl, "nonlocal");
      case TokKind::KwIf:
        s.kind = StmtKind::If;
        return parse_if(s);
      case TokKind::KwWhile:
        s.kind = StmtKind::While;
        return parse_while(s);
      case TokKind::KwFor:
        s.kind = StmtKind::For;
        return parse_for(s);
      case TokKind::KwSwitch:
        s.kind = StmtKind::Switch;
        return parse_switch(s);
      case TokKind::KwTry:
        s.kind = StmtKind::Try;
        return parse_try(s);
      case TokKind::KwClass:
        // M2-2C §3.1: `class NAME:` — block-form only. The body parses as
        // ordinary statements; semantic §3.1 accepts only `function`
        // declarations and `pass`.
        s.kind = StmtKind::Class;
        return parse_class(s);
      case TokKind::KwRaise: {
        // M2-2C §2.3: `raise expr` — a simple statement; bare `raise` is
        // not supported. Value parses at join level (like `return`); the
        // trailing NEWLINE is consumed by the caller.
        s.kind = StmtKind::Raise;
        advance();
        if (!starts_expr(cur().kind) && !at(TokKind::Ident))
          return fail(cur(), "raise requires an exception expression");
        return parse_expr(s.expr);
      }
      // Orphan chain markers (M2-2A §2): an `if` consumes its whole
      // elif/else chain, a switch consumes its case clauses, so these can
      // only show up where a statement is expected.
      case TokKind::KwElif:
        return fail(tk, "'elif' without a matching 'if'");
      case TokKind::KwElse:
        return fail(tk, "'else' without a matching 'if'");
      case TokKind::KwCase:
        return fail(tk, "'case' outside of 'switch'");
      case TokKind::KwReturn: {
        // `return [expr]` — value parses at join level (M2-2B §3/§7); the
        // trailing NEWLINE is consumed by the caller.
        s.kind = StmtKind::Return;
        advance();
        if (!at(TokKind::Newline)) {
          if (!parse_expr(s.expr)) return false;
        }
        return true;
      }
      case TokKind::KwBreak:
        s.kind = StmtKind::Break;
        advance();
        return true;
      case TokKind::KwContinue:
        s.kind = StmtKind::Continue;
        advance();
        return true;
      case TokKind::KwPass:
        s.kind = StmtKind::Pass;
        advance();
        return true;
      case TokKind::Ident:
        // v0.3 nested function declaration: `name(params):` (detected by the
        // '(' … ')' after the name plus the trailing depth-0 ':'; `name(x)`
        // without the colon is a call expression statement).
        if (at_decl_start()) {
          s.kind = StmtKind::Func;
          return parse_func_decl(s.func);
        }
        // The pre-v0.3 `name:` (no parentheses) declaration is gone — the
        // zero-parameter form is `name():`.
        if (peek(1).kind == TokKind::Colon)
          return fail(tk, "'name:' is not a declaration; declare with "
                          "'name():' (no parameters) or 'name(params):'");
        return parse_assign_or_expr_stmt(s);
      case TokKind::LParen:
      case TokKind::Star:
        // M2-2B §3 statement dispatch: speculatively parse a target_list
        // (Name / Name[...] / nested (...) patterns / *rest — a leading
        // `*rest` is a legal first target); '=' or one of the 11 compound-
        // assign tokens after it makes an assignment, otherwise rewind and
        // parse a (join-capable) expression statement. Only Ident/LParen/
        // Star need speculation: no expression can start with `*`.
        return parse_assign_or_expr_stmt(s);
      default:
        break;
    }
    if (starts_expr(tk.kind)) {
      s.kind = StmtKind::ExprStmt;
      return parse_expr(s.expr);
    }
    return fail(tk, "unexpected token '" + token_display(tk) + "'");
  }

  // "global" NAME { "," NAME } | "nonlocal" NAME { "," NAME } (M2-2B §3/§6).
  // Placement and reserved-name checks are semantic; this only parses.
  bool parse_decl_names(Stmt& s, StmtKind kind, const char* kw) {
    advance();  // keyword
    s.kind = kind;
    while (true) {
      if (!at(TokKind::Ident))
        return fail(cur(),
                    std::string("expected identifier after '") + kw + "'");
      s.names.push_back(cur().text);
      advance();
      if (at(TokKind::Comma)) {
        advance();
        continue;
      }
      break;
    }
    return true;
  }

  // True for '=' and the 11 compound-assign tokens (M2-2A §1; 2B §10).
  bool at_assign_op() const {
    switch (cur().kind) {
      case TokKind::Assign:
      case TokKind::PlusAssign:
      case TokKind::MinusAssign:
      case TokKind::StarAssign:
      case TokKind::SlashAssign:
      case TokKind::PercentAssign:
      case TokKind::StarStarAssign:
      case TokKind::AmpAssign:
      case TokKind::PipeAssign:
      case TokKind::CaretAssign:
      case TokKind::LtLtAssign:
      case TokKind::GtGtAssign:
        return true;
      default:
        return false;
    }
  }

  bool parse_assign_or_expr_stmt(Stmt& s) {
    size_t save = i_;
    AssignTarget tgt;
    if (try_parse_target_list(tgt) && at_assign_op()) {
      if (spec_slice_) {  // `xs[a:b] = v` — subscript must be an index (§3)
        diags_.error(spec_slice_loc_, "cannot assign to slice");
        return false;
      }
      s.kind = StmtKind::Assign;
      s.op = at(TokKind::Assign) ? std::string() : cur().text;
      s.target = std::move(tgt);
      // Plain `x = ...` mirrors the target so M1/2A-shaped consumers keep
      // working; `res` stays Unresolved for the semantic pass to fill.
      if (s.target.kind == AssignTarget::Kind::Name)
        s.name = s.target.name;
      advance();  // the assign token
      return parse_expr(s.expr);  // RHS: join level (§3/§7)
    }
    i_ = save;  // not an assignment: rewind and parse an expression
    s.kind = StmtKind::ExprStmt;
    return parse_expr(s.expr);
  }

  // ---- speculative target_list (never reports diagnostics) ---------------
  // target_list := target { "," target } — a single target stays as-is; a
  // parenthesized pattern wraps its inner targets in Kind::Tuple, with "(a)"
  // unwrapping to the lone target and "(a,)" keeping a 1-element Tuple.
  bool try_parse_target_list(AssignTarget& out) {
    ++spec_depth_;
    spec_slice_ = false;
    spec_slice_loc_ = SourceLoc{};
    size_t save = i_;
    std::vector<AssignTarget> ts;
    if (!try_parse_target_list_raw(ts)) {
      i_ = save;
      --spec_depth_;
      return false;
    }
    --spec_depth_;
    if (ts.size() == 1) {
      out = std::move(ts[0]);
    } else {
      out.kind = AssignTarget::Kind::Tuple;
      out.loc = ts[0].loc;
      out.elems = std::move(ts);
    }
    return true;
  }

  bool try_parse_target_list_raw(std::vector<AssignTarget>& ts) {
    size_t save = i_;
    if (!try_parse_target(ts)) {
      i_ = save;
      return false;
    }
    while (at(TokKind::Comma)) {
      advance();
      if (!try_parse_target(ts)) {
        i_ = save;
        return false;
      }
    }
    return true;
  }

  bool try_parse_target(std::vector<AssignTarget>& out) {
    const Token& tk = cur();
    if (tk.kind == TokKind::Star) {
      // rest marker: "*" NAME (M2-2B §3; only inside a destructuring list)
      AssignTarget t;
      t.kind = AssignTarget::Kind::Rest;
      t.loc = tk.loc;
      advance();
      if (!at(TokKind::Ident)) return false;
      t.name = cur().text;
      advance();
      out.push_back(std::move(t));
      return true;
    }
    if (tk.kind == TokKind::Ident) {
      // Cross-scope access (docs/reference/cross-scope-access.md §3): the contextual
      // 4-token atom `variable <qualifier> <name>` may head a target — as
      // a plain store (`variable.global a = 1`, incl. compound forms) or
      // as the object of an index/attribute chain (`variable.a xs[0] = 1`).
      AssignTarget t;
      ExprPtr head;  // non-null only for the VarQual head
      if (tk.text == "variable" && peek(1).kind == TokKind::Dot &&
          qual_tok(peek(2)) && peek(3).kind == TokKind::Ident) {
        t.kind = AssignTarget::Kind::VarQual;
        t.loc = tk.loc;
        t.attr = peek(2).text;
        t.name = peek(3).text;
        head = std::make_unique<Expr>();
        head->kind = ExprKind::VarQual;
        head->loc = tk.loc;
        head->attr = t.attr;
        head->name = t.name;
        i_ += 4;
      } else {
        t.kind = AssignTarget::Kind::Name;
        t.loc = tk.loc;
        t.name = tk.text;
        advance();
      }
      if (at(TokKind::LBracket) || at(TokKind::Dot)) {
        // Postfix chain over the leading NAME (or the VarQual atom): `[ subscript ]` chains
        // (M2-2B §3) and `.attr` names (M2-2C §3.2) in any mix. The chain
        // tail selects the target — an index/slice tail is Kind::Index, an
        // attribute tail Kind::Attr — with the object expression built
        // exactly like the Index machinery's (a Var for the NAME, then the
        // accumulated postfix expression). A slice TAIL of the chain is
        // reported only once the statement is confirmed as an assignment
        // (spec_slice_ at the first '[' location); a slice inside the
        // object (`xs[a:b][0] = v`) stays legal.
        SourceLoc lb = cur().loc;
        bool saw_lb = false;
        ExprPtr cur_e;
        if (head) {
          cur_e = std::move(head);
        } else {
          cur_e = std::make_unique<Expr>();
          cur_e->kind = ExprKind::Var;
          cur_e->loc = t.loc;
          cur_e->name = t.name;
        }
        while (at(TokKind::LBracket) || at(TokKind::Dot)) {
          if (at(TokKind::LBracket)) {
            if (!saw_lb) {
              lb = cur().loc;
              saw_lb = true;
            }
            advance();  // '['
            ExprPtr next;
            if (!parse_subscript(std::move(cur_e), next)) return false;
            cur_e = std::move(next);
          } else {
            advance();  // '.'
            if (!at(TokKind::Ident)) return false;
            auto a = std::make_unique<Expr>();
            a->kind = ExprKind::Attr;
            a->loc = cur().loc;
            a->attr = cur().text;
            advance();  // NAME
            a->lhs = std::move(cur_e);
            cur_e = std::move(a);
          }
        }
        if (saw_lb && cur_e->kind == ExprKind::Slice) {
          spec_slice_ = true;
          spec_slice_loc_ = lb;
        }
        AssignTarget ix;
        ix.loc = t.loc;
        if (cur_e->kind == ExprKind::Attr) {
          // M2-2C §3.2: `obj.attr = ...`
          ix.kind = AssignTarget::Kind::Attr;
          ix.attr = cur_e->attr;
          ix.object = std::move(cur_e->lhs);
        } else {
          ix.kind = AssignTarget::Kind::Index;
          ix.object = std::move(cur_e->lhs);
          ix.index = std::move(cur_e->rhs);
        }
        out.push_back(std::move(ix));
        return true;
      }
      out.push_back(std::move(t));
      return true;
    }
    if (tk.kind == TokKind::LParen) {
      // "(" target_list [ "," ] ")" — nested pattern.
      advance();
      std::vector<AssignTarget> inner;
      if (!try_parse_target(inner)) return false;
      bool trailing_comma = false;
      while (at(TokKind::Comma)) {
        advance();
        if (at(TokKind::RParen)) {
          trailing_comma = true;
          break;
        }
        if (!try_parse_target(inner)) return false;
      }
      if (!at(TokKind::RParen)) return false;
      advance();
      if (inner.size() == 1 && !trailing_comma) {
        out.push_back(std::move(inner[0]));  // "(a)" is just `a`
      } else {
        AssignTarget tup;
        tup.kind = AssignTarget::Kind::Tuple;
        tup.loc = inner[0].loc;
        tup.elems = std::move(inner);
        out.push_back(std::move(tup));
      }
      return true;
    }
    return false;
  }

  // "if" expr ":" suite { "elif" expr ":" suite } [ "else" ":" suite ] —
  // the whole chain belongs to this statement; a later orphan elif/else at
  // statement level gets the normative message (M2-2A §2). At most one
  // `else`, and nothing may follow it.
  bool parse_if(Stmt& s) {
    advance();  // "if"
    ExprPtr cond;
    if (!parse_ternary(cond)) return false;
    if (!expect(TokKind::Colon, "expected ':'")) return false;
    if (!parse_suite(s.bodies.emplace_back())) return false;
    s.conds.push_back(std::move(cond));
    while (true) {
      if (at(TokKind::KwElif)) {
        advance();
        ExprPtr econd;
        if (!parse_ternary(econd)) return false;
        if (!expect(TokKind::Colon, "expected ':'")) return false;
        if (!parse_suite(s.bodies.emplace_back())) return false;
        s.conds.push_back(std::move(econd));
        continue;
      }
      if (at(TokKind::KwElse)) {
        advance();
        if (!expect(TokKind::Colon, "expected ':'")) return false;
        if (!parse_suite(s.else_body)) return false;
        s.has_else = true;
      }
      break;
    }
    return true;
  }

  // "while" expr ":" suite — single-line bodies allowed (M2-2A §0).
  bool parse_while(Stmt& s) {
    advance();  // "while"
    if (!parse_ternary(s.expr)) return false;
    if (!expect(TokKind::Colon, "expected ':'")) return false;
    return parse_suite(s.body);
  }

  // "for" NAME "in" expr ":" NL INDENT block DEDENT — block form only;
  // anything else after the loop variable is "expected 'in' after loop
  // variable" (v0.3: the binder is `in`, matching membership; `->` no longer
  // exists as an operator — calls use parens).
  bool parse_for(Stmt& s) {
    advance();  // "for"
    if (!at(TokKind::Ident)) return fail(cur(), "expected identifier");
    s.name = cur().text;
    advance();
    if (!at(TokKind::KwIn))
      return fail(cur(), "expected 'in' after loop variable");
    advance();
    if (!parse_ternary(s.expr)) return false;
    if (!expect(TokKind::Colon, "expected ':'")) return false;
    return parse_block_header_and_body(s.body);
  }

  // "switch" expr ":" NL INDENT { case_clause } DEDENT — block form only,
  // at least one case, source-order arms (M2-2A §2/§5).
  bool parse_switch(Stmt& s) {
    advance();  // "switch"
    if (!parse_ternary(s.expr)) return false;
    if (!expect(TokKind::Colon, "expected ':'")) return false;
    if (!parse_block_header()) return false;
    while (true) {
      while (at(TokKind::Newline)) advance();
      if (at(TokKind::Dedent)) {
        advance();
        break;
      }
      if (at(TokKind::Eof)) break;  // defensive; synthesized DEDENT precedes EOF
      if (!at(TokKind::KwCase)) {
        if (s.cases.empty())
          return fail(cur(), "expected at least one case in 'switch'");
        return fail(cur(),
                    "unexpected token '" + token_display(cur()) + "'");
      }
      if (!parse_case_arm(s)) return false;
    }
    return true;
  }

  // case_clause := "case" case_label ":" NL INDENT block DEDENT
  //              | "case" "*"    ":" NL INDENT block DEDENT
  // case_label  := [ "-" ] (INT | FLOAT) | STRING | True | False | None —
  // constant literals only; a negative numeric label folds the unary minus
  // into the stored value (M2-2A §2).
  bool parse_case_arm(Stmt& sw) {
    advance();  // "case"
    CaseArm arm;
    arm.loc = cur().loc;  // label start (the '*' for a wildcard)
    bool wildcard = false;
    if (at(TokKind::Star) && peek(1).kind == TokKind::Colon) {
      wildcard = true;
      arm.is_wildcard = true;
      advance();  // '*'
    } else {
      bool neg = false;
      if (at(TokKind::Minus) &&
          (peek(1).kind == TokKind::Int || peek(1).kind == TokKind::Float)) {
        neg = true;
        advance();  // '-'
      }
      const Token& lt = cur();
      switch (lt.kind) {
        case TokKind::Int:
          arm.label_kind = ExprKind::IntLit;
          arm.ival = neg ? -lt.ival : lt.ival;
          advance();
          break;
        case TokKind::Float:
          arm.label_kind = ExprKind::FloatLit;
          arm.fval = neg ? -lt.fval : lt.fval;
          advance();
          break;
        case TokKind::String:
          arm.label_kind = ExprKind::StrLit;
          arm.sval = lt.sval;
          advance();
          break;
        case TokKind::KwTrue:
        case TokKind::KwFalse:
          arm.label_kind = ExprKind::BoolLit;
          arm.bval = (lt.kind == TokKind::KwTrue);
          advance();
          break;
        case TokKind::KwNone:
          arm.label_kind = ExprKind::NoneLit;
          advance();
          break;
        default:
          return fail(lt, "case label must be a constant literal");
      }
      // Anything but ':' after the literal means the label was part of a
      // larger expression (IDENT/expression labels are rejected here).
      if (!at(TokKind::Colon))
        return fail(cur(), "case label must be a constant literal");
    }
    if (!expect(TokKind::Colon, "expected ':'")) return false;
    if (!parse_block_header_and_body(arm.body)) return false;
    sw.cases.push_back(std::move(arm));
    // "'case *' must be the last case in a switch" (M2-2A §2).
    if (wildcard && at(TokKind::KwCase))
      return fail(cur(), "'case *' must be the last case in a switch");
    return true;
  }

  // "try" ":" NL INDENT block DEDENT { except_clause } [ "finally" ":" suite ]
  // (M2-2C §2.1). The try body is block-form only (like for/switch);
  // except/finally bodies use the suite form (single-line bodies legal, like
  // if/while). At least one except or finally clause is required.
  bool parse_try(Stmt& s) {
    const Token& kw = cur();
    advance();  // "try"
    if (!expect(TokKind::Colon, "expected ':'")) return false;
    if (!parse_block_header_and_body(s.body)) return false;
    while (at(TokKind::KwExcept)) {
      // except_clause := "except" [ expr ] [ "as" IDENT ] ":" suite — the
      // expression stops naturally before `as`/`:` (neither is an
      // expression token); a bare `except:` catches everything.
      ExceptArm arm;
      arm.loc = cur().loc;  // the 'except' keyword
      advance();
      if (!at(TokKind::Colon)) {
        if (!parse_ternary(arm.cond)) return false;
        if (at(TokKind::KwAs)) {
          advance();
          if (!at(TokKind::Ident)) return fail(cur(), "expected identifier");
          arm.name = cur().text;
          advance();
        }
      }
      if (!expect(TokKind::Colon, "expected ':'")) return false;
      if (!parse_suite(arm.body)) return false;
      s.arms.push_back(std::move(arm));
    }
    if (at(TokKind::KwFinally)) {
      advance();
      if (!expect(TokKind::Colon, "expected ':'")) return false;
      if (!parse_suite(s.finally_body)) return false;
      s.has_finally = true;
      if (at(TokKind::KwFinally))
        return fail(cur(), "duplicate 'finally' clause");
    }
    if (s.arms.empty() && !s.has_finally)
      return fail(kw, "try block requires at least one 'except' or "
                      "'finally' clause");
    return true;
  }

  // "class" NAME ":" NL INDENT block DEDENT (M2-2C §3.1) — block form only
  // (`class C: pass` on one line is rejected by parse_block_header; a
  // superclass list `class C(Base):` fails the ':' expect — no inheritance
  // in 2C). The body parses with the ordinary statement parser: each
  // `function` declaration is moved into class_methods (the methods), while
  // `pass` and any other statement stay in `body` — the semantic pass
  // accepts `pass` and rejects everything else ("only function declarations
  // are allowed in a class body"), so the parser never reports content.
  bool parse_class(Stmt& s) {
    advance();  // "class"
    if (!at(TokKind::Ident)) return fail(cur(), "expected identifier");
    s.name = cur().text;
    advance();
    if (!expect(TokKind::Colon, "expected ':'")) return false;
    std::vector<Stmt> body;
    if (!parse_block_header_and_body(body)) return false;
    for (Stmt& b : body) {
      if (b.kind == StmtKind::Func && b.func)
        s.class_methods.push_back(std::move(*b.func));  // M2-2C §3.1 method
      else
        s.body.push_back(std::move(b));
    }
    return true;
  }

  // ---- expressions (M2-2A §2 precedence, low -> high) ---------------------
  // expr (join) -> ternary -> or -> and -> not -> comparisons (incl. 'in')
  // -> '|' -> '^' -> '&' -> shifts -> '+'/'-' -> '*','/','%' -> unary
  // '-'/'~' -> '**' -> primary -> postfix (call/index/slice/method)
  //
  // M2-2B §3/§7: `expr := ternary { "," ternary }` — the comma-join level is
  // produced ONLY at assignment RHS, `return`, and expression-statement
  // positions. Call args, collection elements, defaults and control-flow
  // headers parse at `ternary`. >= 2 operands build an ExprKind::Join node.
  bool parse_expr(ExprPtr& e) {
    if (!parse_ternary(e)) return false;
    if (!at(TokKind::Comma)) return true;
    auto j = std::make_unique<Expr>();
    j->kind = ExprKind::Join;
    j->loc = e->loc;
    j->elems.push_back(std::move(e));
    while (at(TokKind::Comma)) {
      advance();
      ExprPtr t;
      if (!parse_ternary(t)) return false;
      j->elems.push_back(std::move(t));
    }
    e = std::move(j);
    return true;
  }

  // ternary := or_expr [ "if" or_expr "else" ternary ] — the else-branch
  // nests (right-assoc); lhs = condition, rhs = then-value, third = else.
  bool parse_ternary(ExprPtr& e) {
    if (!parse_or(e)) return false;
    if (!at(TokKind::KwIf)) return true;
    SourceLoc loc = e->loc;
    advance();  // "if"
    ExprPtr cond;
    if (!parse_or(cond)) return false;
    if (!expect(TokKind::KwElse, "expected 'else'")) return false;
    ExprPtr els;
    if (!parse_ternary(els)) return false;
    auto t = std::make_unique<Expr>();
    t->kind = ExprKind::Ternary;
    t->loc = loc;
    t->lhs = std::move(cond);
    t->rhs = std::move(e);
    t->third = std::move(els);
    e = std::move(t);
    return true;
  }

  // or_expr := and_expr { "or" and_expr } (left-assoc).
  bool parse_or(ExprPtr& e) {
    if (!parse_and(e)) return false;
    while (at(TokKind::KwOr)) {
      SourceLoc loc = e->loc;
      advance();
      ExprPtr rhs;
      if (!parse_and(rhs)) return false;
      e = make_logical("or", std::move(e), std::move(rhs), loc);
    }
    return true;
  }

  // and_expr := not_expr { "and" not_expr } (left-assoc).
  bool parse_and(ExprPtr& e) {
    if (!parse_not(e)) return false;
    while (at(TokKind::KwAnd)) {
      SourceLoc loc = e->loc;
      advance();
      ExprPtr rhs;
      if (!parse_not(rhs)) return false;
      e = make_logical("and", std::move(e), std::move(rhs), loc);
    }
    return true;
  }

  // not_expr := "not" not_expr | cmp_expr.
  bool parse_not(ExprPtr& e) {
    if (at(TokKind::KwNot)) {
      SourceLoc loc = cur().loc;
      advance();
      ExprPtr operand;
      if (!parse_not(operand)) return false;
      auto u = std::make_unique<Expr>();
      u->kind = ExprKind::Unary;
      u->loc = loc;
      u->op = "not";
      u->lhs = std::move(operand);
      e = std::move(u);
      return true;
    }
    return parse_cmp(e);
  }

  // cmp_expr := bitor_expr { cmp_op bitor_expr } — M1 comparisons plus the
  // membership operator '->' at the same (left-associative) level.
  // M2-2C §3.4: `is` / `is not` live at this same level with chaining
  // identical to `==` (the whole loop is left-associative); the `not` after
  // `is` is consumed as part of the operator, never as a unary `not`.
  bool parse_cmp(ExprPtr& e) {
    if (!parse_bitor(e)) return false;
    while (true) {
      TokKind k = cur().kind;
      const char* op = nullptr;
      if (k == TokKind::EqEq)
        op = "==";
      else if (k == TokKind::BangEq)
        op = "!=";
      else if (k == TokKind::Lt)
        op = "<";
      else if (k == TokKind::Le)
        op = "<=";
      else if (k == TokKind::Gt)
        op = ">";
      else if (k == TokKind::Ge)
        op = ">=";
      else if (k == TokKind::KwIn)
        op = "in";  // membership: surface keyword `in`, internal op "in"
                    // (lowered to cs_contains by codegen)
      else if (k == TokKind::KwIs)
        op = "is";  // M2-2C §3.4 — `is`, negated by a following `not`
      else
        break;
      advance();
      if (k == TokKind::KwIs && at(TokKind::KwNot)) {
        advance();  // `is not` is one operator (§3.4), not `is` + unary `not`
        op = "is not";
      }
      SourceLoc bloc = e->loc;
      ExprPtr rhs;
      if (!parse_bitor(rhs)) return false;
      e = make_binary(op, std::move(e), std::move(rhs), bloc);
    }
    return true;
  }

  // bitor_expr := bitxor_expr { "|" bitxor_expr } (left-assoc).
  bool parse_bitor(ExprPtr& e) {
    if (!parse_bitxor(e)) return false;
    while (at(TokKind::Pipe)) {
      SourceLoc bloc = e->loc;
      advance();
      ExprPtr rhs;
      if (!parse_bitxor(rhs)) return false;
      e = make_binary("|", std::move(e), std::move(rhs), bloc);
    }
    return true;
  }

  // bitxor_expr := bitand_expr { "^" bitand_expr } (left-assoc).
  bool parse_bitxor(ExprPtr& e) {
    if (!parse_bitand(e)) return false;
    while (at(TokKind::Caret)) {
      SourceLoc bloc = e->loc;
      advance();
      ExprPtr rhs;
      if (!parse_bitand(rhs)) return false;
      e = make_binary("^", std::move(e), std::move(rhs), bloc);
    }
    return true;
  }

  // bitand_expr := shift_expr { "&" shift_expr } (left-assoc).
  bool parse_bitand(ExprPtr& e) {
    if (!parse_shift(e)) return false;
    while (at(TokKind::Amp)) {
      SourceLoc bloc = e->loc;
      advance();
      ExprPtr rhs;
      if (!parse_shift(rhs)) return false;
      e = make_binary("&", std::move(e), std::move(rhs), bloc);
    }
    return true;
  }

  // shift_expr := add_expr { ("<<" | ">>") add_expr } (left-assoc).
  bool parse_shift(ExprPtr& e) {
    if (!parse_add(e)) return false;
    while (true) {
      TokKind k = cur().kind;
      const char* op = nullptr;
      if (k == TokKind::LtLt)
        op = "<<";
      else if (k == TokKind::GtGt)
        op = ">>";
      else
        break;
      advance();
      SourceLoc bloc = e->loc;
      ExprPtr rhs;
      if (!parse_add(rhs)) return false;
      e = make_binary(op, std::move(e), std::move(rhs), bloc);
    }
    return true;
  }

  // add_expr := mul_expr { ("+" | "-") mul_expr } (left-assoc, M1).
  bool parse_add(ExprPtr& e) {
    if (!parse_mul(e)) return false;
    while (true) {
      TokKind k = cur().kind;
      const char* op = nullptr;
      if (k == TokKind::Plus)
        op = "+";
      else if (k == TokKind::Minus)
        op = "-";
      else
        break;
      advance();
      SourceLoc bloc = e->loc;
      ExprPtr rhs;
      if (!parse_mul(rhs)) return false;
      e = make_binary(op, std::move(e), std::move(rhs), bloc);
    }
    return true;
  }

  // mul_expr := unary { ("*" | "/" | "%") unary } (left-assoc, M1).
  bool parse_mul(ExprPtr& e) {
    if (!parse_unary(e)) return false;
    while (true) {
      TokKind k = cur().kind;
      const char* op = nullptr;
      if (k == TokKind::Star)
        op = "*";
      else if (k == TokKind::Slash)
        op = "/";
      else if (k == TokKind::Percent)
        op = "%";
      else
        break;
      advance();
      SourceLoc bloc = e->loc;
      ExprPtr rhs;
      if (!parse_unary(rhs)) return false;
      e = make_binary(op, std::move(e), std::move(rhs), bloc);
    }
    return true;
  }

  // unary := ("-" | "~") unary | power — so "-2 ** 2" is "-(2 ** 2)" (§2).
  bool parse_unary(ExprPtr& e) {
    TokKind k = cur().kind;
    if (k == TokKind::Minus || k == TokKind::Tilde) {
      SourceLoc loc = cur().loc;
      advance();
      ExprPtr operand;
      if (!parse_unary(operand)) return false;
      auto u = std::make_unique<Expr>();
      u->kind = ExprKind::Unary;
      u->loc = loc;
      u->op = (k == TokKind::Minus) ? "-" : "~";
      u->lhs = std::move(operand);
      e = std::move(u);
      return true;
    }
    return parse_power(e);
  }

  // power := primary [ "**" unary ] — right-associative: the right operand
  // recurses through unary, so "2 ** 3 ** 2" is "2 ** (3 ** 2)" (§2).
  bool parse_power(ExprPtr& e) {
    if (!parse_primary(e)) return false;
    if (at(TokKind::StarStar)) {
      SourceLoc bloc = e->loc;
      advance();
      ExprPtr rhs;
      if (!parse_unary(rhs)) return false;
      e = make_binary("**", std::move(e), std::move(rhs), bloc);
    }
    return true;
  }

  // primary := INT | FLOAT | STRING | BSTRING | True | False | None | NAME
  //          | list_lit | set_or_map_lit | "(" [ tuple_or_group ] ")"
  // then the postfix trailer loop (M2-2B §3; postfix binds tightest, so it
  // is consumed before `**` in parse_power()).
  bool parse_primary(ExprPtr& e) {
    const Token& tk = cur();
    switch (tk.kind) {
      case TokKind::Int: {
        auto x = std::make_unique<Expr>();
        x->kind = ExprKind::IntLit;
        x->loc = tk.loc;
        x->ival = tk.ival;
        advance();
        e = std::move(x);
        break;
      }
      case TokKind::Float: {
        auto x = std::make_unique<Expr>();
        x->kind = ExprKind::FloatLit;
        x->loc = tk.loc;
        x->fval = tk.fval;
        advance();
        e = std::move(x);
        break;
      }
      case TokKind::String: {
        auto x = std::make_unique<Expr>();
        x->kind = ExprKind::StrLit;
        x->loc = tk.loc;
        x->sval = tk.sval;
        advance();
        e = std::move(x);
        break;
      }
      case TokKind::Bytes: {  // M2-2B §1/§5
        auto x = std::make_unique<Expr>();
        x->kind = ExprKind::BytesLit;
        x->loc = tk.loc;
        x->sval = tk.sval;
        advance();
        e = std::move(x);
        break;
      }
      case TokKind::KwTrue:
      case TokKind::KwFalse: {
        auto x = std::make_unique<Expr>();
        x->kind = ExprKind::BoolLit;
        x->loc = tk.loc;
        x->bval = (tk.kind == TokKind::KwTrue);
        advance();
        e = std::move(x);
        break;
      }
      case TokKind::KwNone: {
        auto x = std::make_unique<Expr>();
        x->kind = ExprKind::NoneLit;
        x->loc = tk.loc;
        advance();
        e = std::move(x);
        break;
      }
      case TokKind::Ident: {
        // Cross-scope access (docs/reference/cross-scope-access.md §3): the contextual
        // 4-token atom `variable <qualifier> <name>` (Ident "variable",
        // '.', qualifier token, variable Ident). The qualifier is any
        // identifier — plus the `global` keyword, which is reserved and so
        // can never be anything else here. Unambiguous in expression
        // position: the old grammar can never follow an attr read with a
        // bare Ident, so no existing program changes meaning.
        if (tk.text == "variable" && peek(1).kind == TokKind::Dot &&
            qual_tok(peek(2)) && peek(3).kind == TokKind::Ident) {
          auto x = std::make_unique<Expr>();
          x->kind = ExprKind::VarQual;
          x->loc = tk.loc;
          x->attr = peek(2).text;  // qualifier: global | parent | <fn>
          x->name = peek(3).text;  // the variable
          i_ += 4;
          e = std::move(x);
          break;
        }
        auto x = std::make_unique<Expr>();
        x->kind = ExprKind::Var;
        x->loc = tk.loc;
        x->name = tk.text;
        advance();
        e = std::move(x);
        break;
      }
      case TokKind::LParen:
        if (!parse_paren_expr(e)) return false;
        break;
      case TokKind::LBracket:
        if (!parse_list_lit(e)) return false;
        break;
      case TokKind::LBrace:
        if (!parse_brace_lit(e)) return false;
        break;
      default:
        return fail(tk, "expected expression");
    }
    return parse_postfix(e);
  }

  // "(" [ tuple_or_group ] ")" — () empty tuple, (e) group, (e, ...) tuple
  // (incl. the 1-tuple (e,)). A comma inside parens therefore always makes a
  // tuple: joins never appear here (M2-2B §3/§7; a join needs no parens).
  bool parse_paren_expr(ExprPtr& e) {
    SourceLoc lp = cur().loc;
    advance();  // '('
    if (at(TokKind::RParen)) {
      auto t = std::make_unique<Expr>();
      t->kind = ExprKind::TupleLit;
      t->loc = lp;
      advance();
      e = std::move(t);
      return true;
    }
    ExprPtr first;
    if (!parse_ternary(first)) return false;
    if (at(TokKind::RParen)) {  // group — a single expression
      advance();
      e = std::move(first);
      return true;
    }
    if (!at(TokKind::Comma)) return expect(TokKind::RParen, "expected ')'");
    auto t = std::make_unique<Expr>();
    t->kind = ExprKind::TupleLit;
    t->loc = lp;
    t->elems.push_back(std::move(first));
    while (at(TokKind::Comma)) {
      advance();
      if (at(TokKind::RParen)) break;  // trailing comma: (e,) 1-tuple
      ExprPtr x;
      if (!parse_ternary(x)) return false;
      t->elems.push_back(std::move(x));
    }
    if (!expect(TokKind::RParen, "expected ')'")) return false;
    e = std::move(t);
    return true;
  }

  // list_lit := "[" [ ternary { "," ternary } ] "]" (M2-2B §3).
  bool parse_list_lit(ExprPtr& e) {
    SourceLoc lb = cur().loc;
    advance();  // '['
    auto x = std::make_unique<Expr>();
    x->kind = ExprKind::ListLit;
    x->loc = lb;
    if (!at(TokKind::RBracket)) {
      while (true) {
        ExprPtr el;
        if (!parse_ternary(el)) return false;
        x->elems.push_back(std::move(el));
        if (at(TokKind::Comma)) {
          advance();
          continue;
        }
        break;
      }
    }
    if (!expect(TokKind::RBracket, "expected ']'")) return false;
    e = std::move(x);
    return true;
  }

  // set_or_map_lit := "{" "}" (empty map) | "{" k ":" v { "," k ":" v } "}"
  //                 | "{" ternary { "," ternary } "}" (set) — M2-2B §3:
  // the first element followed by ':' makes a map (keys + elems parallel),
  // otherwise a set; "{}" is the empty MAP (use set() for an empty set).
  bool parse_brace_lit(ExprPtr& e) {
    SourceLoc lb = cur().loc;
    advance();  // '{'
    if (at(TokKind::RBrace)) {
      auto m = std::make_unique<Expr>();
      m->kind = ExprKind::MapLit;
      m->loc = lb;
      advance();
      e = std::move(m);
      return true;
    }
    ExprPtr k0;
    if (!parse_ternary(k0)) return false;
    if (!at(TokKind::Colon)) {  // set literal
      auto s = std::make_unique<Expr>();
      s->kind = ExprKind::SetLit;
      s->loc = lb;
      s->elems.push_back(std::move(k0));
      while (at(TokKind::Comma)) {
        advance();
        ExprPtr el;
        if (!parse_ternary(el)) return false;
        s->elems.push_back(std::move(el));
      }
      if (!expect(TokKind::RBrace, "expected '}'")) return false;
      e = std::move(s);
      return true;
    }
    advance();  // ':'
    auto m = std::make_unique<Expr>();
    m->kind = ExprKind::MapLit;
    m->loc = lb;
    m->keys.push_back(std::move(k0));
    ExprPtr v0;
    if (!parse_ternary(v0)) return false;
    m->elems.push_back(std::move(v0));
    while (at(TokKind::Comma)) {
      advance();
      ExprPtr key;
      if (!parse_ternary(key)) return false;
      if (!expect(TokKind::Colon, "expected ':'")) return false;
      ExprPtr val;
      if (!parse_ternary(val)) return false;
      m->keys.push_back(std::move(key));
      m->elems.push_back(std::move(val));
    }
    if (!expect(TokKind::RBrace, "expected '}'")) return false;
    e = std::move(m);
    return true;
  }

  // postfix := primary { "[" subscript "]"   — index / slice
  //                    | "." NAME            — attribute read / bound-method
  //                                            value (M2-2C §3.2)
  //                    | "(" [ args ] ")" }  — call (bare, indirect, chained)
  // A dot not followed by IDENT is the normative §11 parser error.
  // v0.3: calls are parenthesised, exactly like Python's — `f(x)`, `f()`.
  // The parens survive only as a call in postfix position; a `(` that begins
  // a primary is still grouping `(a + b)` or a tuple `(a, b)`
  // (parse_primary). An Attr callee (`p.step(i)`) becomes a MethodCall —
  // preserving the instance-method inline cache and inlining; anything else
  // (Var/Index/group/call) becomes a Call. For a bare-IDENT callee the Call
  // keeps the 2A `callee` string (fast path for semantic/codegen) AND sets
  // `callee_expr` (§13).
  bool parse_postfix(ExprPtr& e) {
    while (true) {
      if (at(TokKind::LBracket)) {
        advance();  // '['
        ExprPtr sub;
        if (!parse_subscript(std::move(e), sub)) return false;
        e = std::move(sub);
        continue;
      }
      if (at(TokKind::Dot)) {
        advance();  // '.'
        if (!at(TokKind::Ident))
          return fail(cur(), "expected identifier after '.'");
        // v0.3: `.NAME` is always an attribute read (or bound-method value);
        // the `(` after it builds the MethodCall — so `p.step` alone stays the
        // bound-method value and `p.step(i)` calls it.
        auto a = std::make_unique<Expr>();
        a->kind = ExprKind::Attr;
        a->loc = cur().loc;
        a->attr = cur().text;
        advance();  // NAME
        a->lhs = std::move(e);
        e = std::move(a);
        continue;
      }
      if (at(TokKind::Arrow)) {
        // `->` is not an operator in v0.3 (it was the pre-v0.3 call spelling,
        // and before that the loop binder). Calls use parens.
        return fail(cur(), "'->' is not an operator; call with 'f(args)'");
      }
      if (at(TokKind::LParen)) {
        SourceLoc loc = e->loc;
        advance();  // '('
        auto call = std::make_unique<Expr>();
        if (e->kind == ExprKind::Attr) {
          // `p.step(i)` — MethodCall keeps the instance-method IC (M2-2B §3).
          call->kind = ExprKind::MethodCall;
          call->loc = loc;
          call->method = e->attr;
          call->lhs = std::move(e->lhs);
          if (!parse_call_args(call->elems, call->elem_names)) return false;
        } else {
          call->kind = ExprKind::Call;
          call->loc = loc;
          if (e->kind == ExprKind::Var) call->callee = e->name;
          call->callee_expr = std::move(e);
          if (!parse_call_args(call->args, call->arg_names)) return false;
        }
        if (!expect(TokKind::RParen, "expected ')' after the argument list"))
          return false;
        e = std::move(call);
        continue;
      }
      return true;
    }
  }

  // Call args between '(' and ')': [ NAME "=" ] ternary { "," [ NAME "=" ]
  // ternary }. The closing ')' belongs to the caller (parse_postfix), so an
  // empty list is the zero-argument call `f()`. Each arg parses at TERNARY
  // level (so `f(a + b)` is one argument and `f(a, b)` is two); names
  // parallel args ("" = positional).
  bool parse_call_args(std::vector<ExprPtr>& args,
                       std::vector<std::string>& names) {
    if (at(TokKind::RParen)) return true;  // zero args: `f()`
    while (true) {
      std::string name;
      if (at(TokKind::Ident) && peek(1).kind == TokKind::Assign) {
        name = cur().text;
        advance();
        advance();
      }
      ExprPtr arg;
      if (!parse_ternary(arg)) return false;
      names.push_back(std::move(name));
      args.push_back(std::move(arg));
      if (at(TokKind::Comma)) {
        advance();
        continue;
      }
      break;
    }
    return true;
  }

  // True where an expression may begin (call-arg lists, raise values, …).
  bool at_expr_start() const {
    return starts_expr(cur().kind) || at(TokKind::Ident);
  }

  // v0.3 statement-level declaration probe: at an Ident that starts a
  // statement, true when it begins a function declaration — the '(' … ')'
  // after the name closes a parameter list and a depth-0 ':' follows it.
  // A map-literal, slice or group ':' is always inside brackets, so the first
  // depth-0 token after the matching ')' can only be the declaration colon
  // (this cleanly separates the declaration `a(x):` from the call statement
  // `a(x)`; without the colon it is a call).
  bool at_decl_start() const {
    if (i_ + 1 >= toks_.size() || toks_[i_ + 1].kind != TokKind::LParen)
      return false;  // `name(` is the only declaration opening
    int depth = 0;
    for (size_t j = i_ + 1; j < toks_.size(); ++j) {
      TokKind k = toks_[j].kind;
      if (k == TokKind::LParen)
        ++depth;
      else if (k == TokKind::RParen) {
        if (--depth == 0)  // the ')' that closes the name's '('
          return j + 1 < toks_.size() && toks_[j + 1].kind == TokKind::Colon;
      } else if (k == TokKind::Newline || k == TokKind::Dedent ||
                 k == TokKind::Eof)
        return false;  // the '(' never closed on this line
    }
    return false;
  }

  // subscript := [ expr ] ":" [ expr ] [ ":" expr ] (slice) | expr (index)
  // — with the '[' already consumed; consumes the ']'. Builds Index (rhs =
  // index) or Slice (lhs = object, rhs = start [nullable], third = stop,
  // fourth = step; has_start/has_stop/has_step per M2-2B §3/ast.h).
  bool parse_subscript(ExprPtr obj, ExprPtr& out) {
    SourceLoc oloc = obj->loc;
    ExprPtr start, stop, step;
    bool has_start = false, has_stop = false, has_step = false;
    if (at(TokKind::Colon)) {
      // open start: [: ...]
    } else {
      if (!parse_ternary(start)) return false;
      has_start = true;
      if (!at(TokKind::Colon)) {
        if (!expect(TokKind::RBracket, "expected ']'")) return false;
        auto ix = std::make_unique<Expr>();
        ix->kind = ExprKind::Index;
        ix->loc = oloc;
        ix->lhs = std::move(obj);
        ix->rhs = std::move(start);
        out = std::move(ix);
        return true;
      }
    }
    advance();  // ':'
    if (!at(TokKind::RBracket) && !at(TokKind::Colon)) {
      if (!parse_ternary(stop)) return false;
      has_stop = true;
    }
    if (at(TokKind::Colon)) {
      advance();
      if (!at(TokKind::RBracket)) {
        if (!parse_ternary(step)) return false;
        has_step = true;
      }
    }
    if (!expect(TokKind::RBracket, "expected ']'")) return false;
    auto sl = std::make_unique<Expr>();
    sl->kind = ExprKind::Slice;
    sl->loc = oloc;
    sl->has_start = has_start;
    sl->has_stop = has_stop;
    sl->has_step = has_step;
    sl->lhs = std::move(obj);
    if (has_start) sl->rhs = std::move(start);
    if (has_stop) sl->third = std::move(stop);
    if (has_step) sl->fourth = std::move(step);
    out = std::move(sl);
    return true;
  }

  const std::vector<Token>& toks_;
  Program& prog_;
  Diagnostics& diags_;
  size_t i_ = 0;

  // M2-2B statement dispatch: >0 while speculatively parsing a target_list
  // (suppresses diagnostics); spec_slice_ records a slice subscript found in
  // a target so a confirmed assignment can report it.
  int spec_depth_ = 0;
  bool spec_slice_ = false;
  SourceLoc spec_slice_loc_;
};

}  // namespace

bool parse_tokens(const std::vector<Token>& toks, const std::string& file_path,
                  Program& out, Diagnostics& diags) {
  if (toks.empty()) {
    diags.error("unexpected token 'end of file'");
    return false;
  }
  out.file_path = file_path;
  Parser p(toks, out, diags);
  return p.parse_program();
}

bool parse_source(const std::string& file_path, const std::string& source,
                  Program& out, Diagnostics& diags) {
  diags.set_source(file_path, source);
  out = Program{};
  out.file_path = file_path;
  std::vector<Token> toks;
  if (!lex_tokens(file_path, source, toks, diags)) return false;
  return parse_tokens(toks, file_path, out, diags);
}

}  // namespace cstar
