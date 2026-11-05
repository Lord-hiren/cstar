// lexer.h — cstar M2 Stage 2B lexer (docs/contracts/m2b-semantics.md §1;
// M2-2A lexical rules: docs/contracts/m2a-semantics.md §1; M1: docs/contracts/m1-semantics.md §2).
#pragma once

#include <string>
#include <vector>

#include "ast.h"
#include "diagnostics.h"

namespace cstar {

enum class TokKind {
  Newline,  // "\n" — logical end of statement (suppressed inside brackets)
  Indent,   // start of an indented block
  Dedent,   // return to a shallower indentation level
  Ident,    // [A-Za-z_][A-Za-z0-9_]* (including the reserved name "print";
            // "range" also stays an Ident — its checks are semantic)
  Int,      // decimal integer literal
  Float,    // DIGITS "." DIGITS
  String,   // double-quoted single-line string literal (decoded into sval)
  Bytes,    // M2-2B §1: b"..." bytes literal, 'b' immediately followed by '"'
            // (same escapes as String plus \xHH; decoded into sval)
  // Keywords (reserved; cannot be identifiers):
  KwFunction,
  KwMain,
  KwReturn,
  KwTrue,
  KwFalse,
  KwNone,
  // M2-2A keywords (§1): if elif else for while break continue switch case
  // pass and or not
  KwIf,
  KwElif,
  KwElse,
  KwFor,
  KwWhile,
  KwBreak,
  KwContinue,
  KwSwitch,
  KwCase,
  KwPass,
  KwAnd,
  KwOr,
  KwNot,
  // M2-2B keywords (§1): global nonlocal (reserved)
  KwGlobal,
  KwNonlocal,
  // M2-2C keywords (§2.1): try except finally raise as
  KwTry,
  KwExcept,
  KwFinally,
  KwRaise,
  KwAs,
  // M2-2C keywords (§3): class is
  KwClass,
  KwIs,
  // v0.3: `in` — membership test (x in container) and the for-loop binder
  // (for x in iter:). Replaces the M1/M2 membership `->`, which is gone: the
  // `->` operator was retired in v0.3 (calls and declarations spell their
  // argument/parameter lists with parens, like Python).
  KwIn,
  // Stage 2D: module imports (top-level only).
  KwImport,
  KwFrom,
  // Punctuation:
  LParen,
  RParen,
  LBracket,   // M2-2B §1: [
  RBracket,   // M2-2B §1: ]
  LBrace,     // M2-2B §1: {
  RBrace,     // M2-2B §1: }
  Colon,
  Comma,
  Dot,        // M2-2B §1: . (method calls; bare attribute reads, M2-2C §3.2)
  Assign,   // =
  EqEq,     // ==
  BangEq,   // !=
  Lt,       // <
  Le,       // <=
  Gt,       // >
  Ge,       // >=
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  // M2-2A operators (§1; maximal munch — no "**=", "<<=", ">>="):
  Arrow,         // "->" — NOT an operator in v0.3; lexed only so the parser
                 // can report "call with 'f(args)'" for the retired spelling
  StarStar,      // **
  Amp,           // &
  Pipe,          // |
  Caret,         // ^
  Tilde,         // ~
  LtLt,          // <<
  GtGt,          // >>
  PlusAssign,    // +=
  MinusAssign,   // -=
  StarAssign,    // *=
  SlashAssign,   // /=
  PercentAssign, // %=
  StarStarAssign,   // **= (M2-2A integration: compound exponent/bitwise forms)
  AmpAssign,        // &=
  PipeAssign,       // |=
  CaretAssign,      // ^=
  LtLtAssign,       // <<=
  GtGtAssign,       // >>=
  Eof,
};

struct Token {
  TokKind kind = TokKind::Eof;
  SourceLoc loc;

  std::string text;    // raw lexeme (IDENT/keyword text, literal spelling,
                       // operator spelling; for String: the source text
                       // including the surrounding quotes)
  long long ival = 0;  // Int
  double fval = 0.0;   // Float
  std::string sval;    // String — decoded bytes (escapes processed, may
                       // contain embedded NUL)
};

// Human-readable spelling for error messages ("unexpected token 'x'").
std::string token_display(const Token& tok);

// Lexes `source` into `out` according to §1/§2 (Python-style INDENT/DEDENT,
// bracket-depth suppression of NEWLINE for (, [ and {, all comment forms,
// text + b"..." literals). Attaches the source to `diags` for caret display.
// On the FIRST lexical error, reports it through `diags` and returns false;
// otherwise returns true and `out` ends with an Eof token.
bool lex_tokens(const std::string& file_path, const std::string& source,
                std::vector<Token>& out, Diagnostics& diags);

}  // namespace cstar
