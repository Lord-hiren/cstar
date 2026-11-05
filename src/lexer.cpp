// lexer.cpp — cstar M2 Stage 2B lexer (docs/contracts/m2b-semantics.md §1;
// M2-2A lexical rules: docs/contracts/m2a-semantics.md §1; M1: docs/contracts/m1-semantics.md §2).
#include "lexer.h"

#include <cstdlib>
#include <utility>

namespace cstar {
namespace {

bool is_ident_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_ident_cont(char c) {
  return is_ident_start(c) || (c >= '0' && c <= '9');
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// M2-2B §1: hex digit for the bytes-literal \xHH escape (-1 if not hex).
int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

class Lexer {
 public:
  Lexer(const std::string& file, const std::string& src,
        std::vector<Token>& out, Diagnostics& diags)
      : src_(src), out_(out), diags_(diags), file_(file) {}

  bool run() {
    while (true) {
      if (at_line_start_ && depth_ == 0) {
        if (!handle_line_start()) return false;
      }
      if (eof()) return finish();
      if (!scan_unit()) return false;
    }
  }

 private:
  // ---- character helpers -------------------------------------------------
  char peek(size_t off = 0) const {
    size_t p = pos_ + off;
    return p < src_.size() ? src_[p] : '\0';
  }
  bool eof() const { return pos_ >= src_.size(); }
  SourceLoc here() const { return SourceLoc{line_, col_, file_}; }

  void advance_char() {  // never the '\n' character
    ++pos_;
    ++col_;
  }
  void advance_newline() {
    ++pos_;
    ++line_;
    col_ = 1;
  }

  bool fail(SourceLoc loc, const std::string& msg) {
    diags_.error(loc, msg);
    return false;
  }

  void push(TokKind k, SourceLoc loc, std::string text = std::string()) {
    if (k != TokKind::Newline && k != TokKind::Indent &&
        k != TokKind::Dedent && k != TokKind::Eof) {
      line_has_tokens_ = true;
    }
    Token t;
    t.kind = k;
    t.loc = loc;
    t.text = std::move(text);
    out_.push_back(std::move(t));
  }

  // ---- line start: indentation processing --------------------------------
  // Called only at depth 0 with at_line_start_ true. Consumes the leading
  // whitespace; for a real (non-blank, non-comment) line it compares the
  // indentation against the stack and emits INDENT/DEDENT tokens.
  bool handle_line_start() {
    at_line_start_ = false;  // re-armed by the '\n' handler
    int spaces = 0;
    while (!eof()) {
      char c = src_[pos_];
      if (c == ' ') {
        ++spaces;
        advance_char();
      } else if (c == '\t') {
        return fail(here(), "tabs are not allowed for indentation");
      } else if (c == '\r') {
        advance_char();  // tolerate CRLF input
      } else {
        break;
      }
    }
    // Blank line or EOF: no indentation processing, no NEWLINE token.
    if (eof() || src_[pos_] == '\n') return true;
    // Comment-only line: does not affect indentation.
    if (starts_comment()) return true;

    // A real line: reconcile with the indent stack.
    int top = indents_.back();
    if (spaces > top) {
      indents_.push_back(spaces);
      push(TokKind::Indent, SourceLoc{line_, 1, file_});
    } else if (spaces < top) {
      SourceLoc loc{line_, spaces + 1, file_};
      int popped = 0;
      while (indents_.back() > spaces) {
        indents_.pop_back();
        ++popped;
      }
      if (indents_.back() != spaces)
        return fail(loc, "inconsistent indentation");
      for (int i = 0; i < popped; ++i) push(TokKind::Dedent, loc);
    }
    return true;
  }

  bool starts_comment() const {
    if (peek() == '#') return true;
    if (peek() == '/' && (peek(1) == '/' || peek(1) == '*')) return true;
    if (peek() == '"' && peek(1) == '"' && peek(2) == '"') return true;
    return false;
  }

  // ---- one lexing unit (token, comment, whitespace, or newline) ----------
  bool scan_unit() {
    char c = src_[pos_];

    // Whitespace. A tab is only an error in leading whitespace, which is
    // checked in handle_line_start(); mid-line it is skipped.
    if (c == ' ' || c == '\t' || c == '\r') {
      advance_char();
      return true;
    }

    // Newline (logical end of statement at bracket depth 0).
    if (c == '\n') {
      SourceLoc nl_loc = here();
      advance_newline();
      if (depth_ == 0) {
        if (line_has_tokens_) {
          push(TokKind::Newline, nl_loc);
          line_has_tokens_ = false;
        }
        at_line_start_ = true;
      }
      return true;
    }

    // Comments, checked before '/' is treated as division. Never reached
    // inside string literals, which are scanned separately below.
    if (c == '#') {
      skip_line_comment();
      return true;
    }
    if (c == '/' && peek(1) == '/') {
      skip_line_comment();
      return true;
    }
    if (c == '/' && peek(1) == '*') return block_comment();
    if (c == '"' && peek(1) == '"' && peek(2) == '"') return triple_comment();

    if (c == '"') return string_literal();
    // M2-2B §1: 'b' IMMEDIATELY followed by '"' starts a bytes literal
    // (b "x" with a space stays an IDENT followed by a string).
    if (c == 'b' && peek(1) == '"') return bytes_literal();
    if (is_digit(c)) return number();
    if (is_ident_start(c)) return identifier();
    return punctuation();
  }

  void skip_line_comment() {
    while (!eof() && src_[pos_] != '\n') advance_char();
  }

  bool block_comment() {
    SourceLoc start = here();
    advance_char();  // '/'
    advance_char();  // '*'
    while (true) {
      if (eof()) return fail(start, "unterminated block comment");
      if (src_[pos_] == '\n') {
        advance_newline();
        continue;
      }
      if (src_[pos_] == '*' && peek(1) == '/') {
        advance_char();
        advance_char();
        return true;
      }
      advance_char();
    }
  }

  bool triple_comment() {
    SourceLoc start = here();
    advance_char();
    advance_char();
    advance_char();  // '"""'
    while (true) {
      if (eof()) return fail(start, "unterminated triple-quoted comment");
      if (src_[pos_] == '\n') {
        advance_newline();
        continue;
      }
      if (src_[pos_] == '"' && peek(1) == '"' && peek(2) == '"') {
        advance_char();
        advance_char();
        advance_char();
        return true;
      }
      advance_char();
    }
  }

  bool string_literal() {
    SourceLoc start = here();
    size_t raw_start = pos_;
    advance_char();  // opening quote
    std::string val;
    while (true) {
      if (eof() || src_[pos_] == '\n')
        return fail(start, "unterminated string literal");
      char c = src_[pos_];
      if (c == '"') {
        advance_char();
        Token t;
        t.kind = TokKind::String;
        t.loc = start;
        t.text = src_.substr(raw_start, pos_ - raw_start);
        t.sval = std::move(val);
        out_.push_back(std::move(t));
        line_has_tokens_ = true;
        return true;
      }
      if (c == '\\') {
        SourceLoc esc = here();
        advance_char();  // backslash
        if (eof() || src_[pos_] == '\n')
          return fail(start, "unterminated string literal");
        char e = src_[pos_];
        advance_char();
        switch (e) {
          case 'n':
            val.push_back('\n');
            break;
          case 't':
            val.push_back('\t');
            break;
          case 'r':
            val.push_back('\r');
            break;
          case '\\':
            val.push_back('\\');
            break;
          case '"':
            val.push_back('"');
            break;
          case '0':
            val.push_back('\0');
            break;
          default:
            return fail(esc, std::string("unknown escape sequence '\\") + e +
                                 "'");
        }
        continue;
      }
      val.push_back(c);
      advance_char();
    }
  }

  // M2-2B §1: b"..." — like a text literal (M1 escape set) plus \xHH.
  // Unterminated -> "unterminated bytes literal" (first error wins, caret).
  bool bytes_literal() {
    SourceLoc start = here();
    size_t raw_start = pos_;
    advance_char();  // 'b'
    advance_char();  // opening quote
    std::string val;
    while (true) {
      if (eof() || src_[pos_] == '\n')
        return fail(start, "unterminated bytes literal");
      char c = src_[pos_];
      if (c == '"') {
        advance_char();
        Token t;
        t.kind = TokKind::Bytes;
        t.loc = start;
        t.text = src_.substr(raw_start, pos_ - raw_start);
        t.sval = std::move(val);
        out_.push_back(std::move(t));
        line_has_tokens_ = true;
        return true;
      }
      if (c == '\\') {
        SourceLoc esc = here();
        advance_char();  // backslash
        if (eof() || src_[pos_] == '\n')
          return fail(start, "unterminated bytes literal");
        char e = src_[pos_];
        advance_char();
        if (e == 'x') {
          // \xHH — exactly two hex digits (M2-2B §1/§5).
          int hi = hex_digit(peek());
          int lo = hex_digit(peek(1));
          if (hi < 0 || lo < 0) return fail(esc, "invalid \\x escape");
          advance_char();
          advance_char();
          val.push_back(static_cast<char>((hi << 4) | lo));
          continue;
        }
        switch (e) {
          case 'n':
            val.push_back('\n');
            break;
          case 't':
            val.push_back('\t');
            break;
          case 'r':
            val.push_back('\r');
            break;
          case '\\':
            val.push_back('\\');
            break;
          case '"':
            val.push_back('"');
            break;
          case '0':
            val.push_back('\0');
            break;
          default:
            return fail(esc, std::string("unknown escape sequence '\\") + e +
                                 "'");
        }
        continue;
      }
      val.push_back(c);
      advance_char();
    }
  }

  bool number() {
    SourceLoc start = here();
    size_t start_pos = pos_;
    while (!eof() && is_digit(src_[pos_])) advance_char();
    bool is_float = false;
    if (peek() == '.' && is_digit(peek(1))) {
      is_float = true;
      advance_char();  // '.'
      while (!eof() && is_digit(src_[pos_])) advance_char();
    }
    std::string lexeme = src_.substr(start_pos, pos_ - start_pos);
    if (is_float) {
      Token t;
      t.kind = TokKind::Float;
      t.loc = start;
      t.text = lexeme;
      t.fval = std::strtod(lexeme.c_str(), nullptr);
      out_.push_back(std::move(t));
      line_has_tokens_ = true;
      return true;
    }
    // Decimal integer; must fit in signed 64-bit.
    unsigned long long acc = 0;
    for (char d : lexeme) {
      unsigned dig = static_cast<unsigned>(d - '0');
      if (acc > (9223372036854775807ULL - dig) / 10)
        return fail(start, "integer literal out of range");
      acc = acc * 10ULL + dig;
    }
    Token t;
    t.kind = TokKind::Int;
    t.loc = start;
    t.text = lexeme;
    t.ival = static_cast<long long>(acc);
    out_.push_back(std::move(t));
    line_has_tokens_ = true;
    return true;
  }

  bool identifier() {
    SourceLoc start = here();
    size_t start_pos = pos_;
    while (!eof() && is_ident_cont(src_[pos_])) advance_char();
    std::string word = src_.substr(start_pos, pos_ - start_pos);
    TokKind k;
    if (word == "function")
      k = TokKind::KwFunction;
    else if (word == "main")
      k = TokKind::KwMain;
    else if (word == "return")
      k = TokKind::KwReturn;
    else if (word == "True")
      k = TokKind::KwTrue;
    else if (word == "False")
      k = TokKind::KwFalse;
    else if (word == "None")
      k = TokKind::KwNone;
    // M2-2A keywords (§1). "range" deliberately stays an Ident: it is a
    // reserved builtin name whose checks live in the semantic pass.
    else if (word == "if")
      k = TokKind::KwIf;
    else if (word == "elif")
      k = TokKind::KwElif;
    else if (word == "else")
      k = TokKind::KwElse;
    else if (word == "for")
      k = TokKind::KwFor;
    else if (word == "while")
      k = TokKind::KwWhile;
    else if (word == "break")
      k = TokKind::KwBreak;
    else if (word == "continue")
      k = TokKind::KwContinue;
    else if (word == "switch")
      k = TokKind::KwSwitch;
    else if (word == "case")
      k = TokKind::KwCase;
    else if (word == "pass")
      k = TokKind::KwPass;
    else if (word == "and")
      k = TokKind::KwAnd;
    else if (word == "or")
      k = TokKind::KwOr;
    else if (word == "not")
      k = TokKind::KwNot;
    // M2-2B keywords (§1): global nonlocal (reserved). "set" deliberately
    // stays an Ident (a builtin constructor call, like range).
    else if (word == "global")
      k = TokKind::KwGlobal;
    else if (word == "nonlocal")
      k = TokKind::KwNonlocal;
    // M2-2C keywords (§2.1): try except finally raise as.
    else if (word == "try")
      k = TokKind::KwTry;
    else if (word == "except")
      k = TokKind::KwExcept;
    else if (word == "finally")
      k = TokKind::KwFinally;
    else if (word == "raise")
      k = TokKind::KwRaise;
    else if (word == "as")
      k = TokKind::KwAs;
    // M2-2C keywords (§3): class is.
    else if (word == "class")
      k = TokKind::KwClass;
    else if (word == "is")
      k = TokKind::KwIs;
    // v0.3: `in` — membership test and the for-loop binder.
    else if (word == "in")
      k = TokKind::KwIn;
    // Stage 2D: modules/imports.
    else if (word == "import")
      k = TokKind::KwImport;
    else if (word == "from")
      k = TokKind::KwFrom;
    else
      k = TokKind::Ident;
    Token t;
    t.kind = k;
    t.loc = start;
    t.text = word;
    out_.push_back(std::move(t));
    line_has_tokens_ = true;
    return true;
  }

  bool punctuation() {
    SourceLoc loc = here();
    char c = src_[pos_];
    switch (c) {
      case '(':
      case '[':
      case '{':
        // M2-2B §1: bracket depth (and thus NEWLINE suppression inside
        // brackets) covers (, [ and { alike.
        advance_char();
        ++depth_;
        open_brackets_.push_back(std::make_pair(c, loc));
        if (c == '(')
          push(TokKind::LParen, loc, "(");
        else if (c == '[')
          push(TokKind::LBracket, loc, "[");
        else
          push(TokKind::LBrace, loc, "{");
        return true;
      case ')':
      case ']':
      case '}':
        advance_char();
        if (depth_ > 0) {
          --depth_;
          if (!open_brackets_.empty()) open_brackets_.pop_back();
        }
        if (c == ')')
          push(TokKind::RParen, loc, ")");
        else if (c == ']')
          push(TokKind::RBracket, loc, "]");
        else
          push(TokKind::RBrace, loc, "}");
        return true;
      case '.':
        advance_char();
        push(TokKind::Dot, loc, ".");
        return true;
      case ':':
        advance_char();
        push(TokKind::Colon, loc, ":");
        return true;
      case ',':
        advance_char();
        push(TokKind::Comma, loc, ",");
        return true;
      case '+':
        // Maximal munch: '+=' before '+'.
        if (peek(1) == '=') {
          advance_char();
          advance_char();
          push(TokKind::PlusAssign, loc, "+=");
        } else {
          advance_char();
          push(TokKind::Plus, loc, "+");
        }
        return true;
      case '-':
        // M2-2A §1 order: '-' followed by '>' is Arrow, then '-=' is
        // MinusAssign; anything else is Minus. `->` retired as an operator in
        // v0.3 — it is still lexed so the parser can name it in the error
        // "'->' is not an operator; call with 'f(args)'".
        if (peek(1) == '>') {
          advance_char();
          advance_char();
          push(TokKind::Arrow, loc, "->");
        } else if (peek(1) == '=') {
          advance_char();
          advance_char();
          push(TokKind::MinusAssign, loc, "-=");
        } else {
          advance_char();
          push(TokKind::Minus, loc, "-");
        }
        return true;
      case '*':
        // Maximal munch: '**=' (3 chars), then '**', then '*='.
        if (peek(1) == '*') {
          advance_char();
          advance_char();
          if (peek(0) == '=') {
            advance_char();
            push(TokKind::StarStarAssign, loc, "**=");
          } else {
            push(TokKind::StarStar, loc, "**");
          }
        } else if (peek(1) == '=') {
          advance_char();
          advance_char();
          push(TokKind::StarAssign, loc, "*=");
        } else {
          advance_char();
          push(TokKind::Star, loc, "*");
        }
        return true;
      case '%':
        if (peek(1) == '=') {
          advance_char();
          advance_char();
          push(TokKind::PercentAssign, loc, "%=");
        } else {
          advance_char();
          push(TokKind::Percent, loc, "%");
        }
        return true;
      case '&':
        if (peek(1) == '=') {
          advance_char();
          advance_char();
          push(TokKind::AmpAssign, loc, "&=");
        } else {
          advance_char();
          push(TokKind::Amp, loc, "&");
        }
        return true;
      case '|':
        if (peek(1) == '=') {
          advance_char();
          advance_char();
          push(TokKind::PipeAssign, loc, "|=");
        } else {
          advance_char();
          push(TokKind::Pipe, loc, "|");
        }
        return true;
      case '^':
        if (peek(1) == '=') {
          advance_char();
          advance_char();
          push(TokKind::CaretAssign, loc, "^=");
        } else {
          advance_char();
          push(TokKind::Caret, loc, "^");
        }
        return true;
      case '~':
        advance_char();
        push(TokKind::Tilde, loc, "~");
        return true;
      case '=':
        if (peek(1) == '=') {
          advance_char();
          advance_char();
          push(TokKind::EqEq, loc, "==");
        } else {
          advance_char();
          push(TokKind::Assign, loc, "=");
        }
        return true;
      case '!':
        if (peek(1) == '=') {
          advance_char();
          advance_char();
          push(TokKind::BangEq, loc, "!=");
          return true;
        }
        advance_char();
        return fail(loc, "unexpected character '!'");
      case '<':
        // Maximal munch: '<<=' (3 chars), then '<<', then '<='.
        if (peek(1) == '<') {
          advance_char();
          advance_char();
          if (peek(0) == '=') {
            advance_char();
            push(TokKind::LtLtAssign, loc, "<<=");
          } else {
            push(TokKind::LtLt, loc, "<<");
          }
        } else if (peek(1) == '=') {
          advance_char();
          advance_char();
          push(TokKind::Le, loc, "<=");
        } else {
          advance_char();
          push(TokKind::Lt, loc, "<");
        }
        return true;
      case '>':
        // Maximal munch: '>>=' (3 chars), then '>>', then '>='.
        if (peek(1) == '>') {
          advance_char();
          advance_char();
          if (peek(0) == '=') {
            advance_char();
            push(TokKind::GtGtAssign, loc, ">>=");
          } else {
            push(TokKind::GtGt, loc, ">>");
          }
        } else if (peek(1) == '=') {
          advance_char();
          advance_char();
          push(TokKind::Ge, loc, ">=");
        } else {
          advance_char();
          push(TokKind::Gt, loc, ">");
        }
        return true;
      case '/':
        // '//' and '/*' were checked in scan_unit() (comments win over '/').
        if (peek(1) == '=') {
          advance_char();
          advance_char();
          push(TokKind::SlashAssign, loc, "/=");
        } else {
          advance_char();
          push(TokKind::Slash, loc, "/");
        }
        return true;
      default:
        break;
    }
    advance_char();
    return fail(loc, std::string("unexpected character '") + c + "'");
  }

  // ---- EOF ---------------------------------------------------------------
  bool finish() {
    SourceLoc loc = here();
    if (depth_ > 0) {
      // M1 message for parens is preserved byte-for-byte ("unclosed '('");
      // a stray unclosed '[' / '{' names its own bracket.
      char open = '(';
      SourceLoc open_loc = loc;
      if (!open_brackets_.empty()) {
        open = open_brackets_.back().first;
        open_loc = open_brackets_.back().second;
      }
      return fail(open_loc, std::string("unclosed '") + open + "'");
    }
    if (line_has_tokens_) {
      push(TokKind::Newline, loc);  // synthesized final newline
      line_has_tokens_ = false;
    }
    while (indents_.size() > 1) {
      indents_.pop_back();
      push(TokKind::Dedent, loc);
    }
    push(TokKind::Eof, loc, "<eof>");
    return true;
  }

  const std::string& src_;
  std::vector<Token>& out_;
  Diagnostics& diags_;

  size_t pos_ = 0;
  int line_ = 1;
  int col_ = 1;
  int depth_ = 0;                 // unbalanced '(', '[', '{' depth
  std::vector<std::pair<char, SourceLoc>> open_brackets_;
  std::string file_;
  bool at_line_start_ = true;     // indentation not yet processed for this line
  bool line_has_tokens_ = false;  // real tokens seen on the current line
  std::vector<int> indents_{0};
};

}  // namespace

std::string token_display(const Token& tok) {
  switch (tok.kind) {
    case TokKind::Newline:
      return "newline";
    case TokKind::Indent:
      return "indent";
    case TokKind::Dedent:
      return "dedent";
    case TokKind::Eof:
      return "end of file";
    default:
      return tok.text;
  }
}

bool lex_tokens(const std::string& file_path, const std::string& source,
                std::vector<Token>& out, Diagnostics& diags) {
  diags.set_source(file_path, source);
  out.clear();
  Lexer lex(file_path, source, out, diags);
  return lex.run();
}

}  // namespace cstar
