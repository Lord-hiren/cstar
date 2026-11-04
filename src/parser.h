// parser.h — Pentest M2 Stage 2B recursive-descent parser
// (docs/contracts/m2b-semantics.md §3/§6/§7; M2-2A §2, M1 §3 grammar elsewhere).
#pragma once

#include <string>
#include <vector>

#include "ast.h"
#include "diagnostics.h"
#include "lexer.h"

namespace pentest {

// Parses a token stream produced by lex_tokens() into `out` (items only; the
// semantic annotations are left untouched). Reports only the FIRST syntax
// error through `diags` (with caret display) and returns false. `toks` must
// be non-empty and end with an Eof token.
bool parse_tokens(const std::vector<Token>& toks, const std::string& file_path,
                  Program& out, Diagnostics& diags);

}  // namespace pentest
