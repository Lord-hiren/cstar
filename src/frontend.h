// frontend.h — interface between the CLI driver and the frontend
// (lexer + parser + semantic pass). Shared contract.
#pragma once

#include <string>

#include "ast.h"
#include "diagnostics.h"

namespace cstar {

// Lexes and parses `source`. On success fills `out` (file_path + items) and
// returns true. On the first lexical or syntax error, prints it through
// `diags` (source attached for caret display) and returns false.
// The semantic pass is NOT run here.
bool parse_source(const std::string& file_path, const std::string& source,
                  Program& out, Diagnostics& diags);

// Declaration discovery + name resolution + validation (docs/contracts/m1-semantics.md
// §6–§7, docs/contracts/m2a-semantics.md §7–§8). Annotates `out` (Program.functions,
// Program.globals, Program.has_main, FuncDecl.c_name/locals, Var/Assign/For
// resolutions, is_builtin_print, is_builtin_range).
// Reports ALL semantic errors through `diags`; returns false if any.
bool semantic_check(Program& out, Diagnostics& diags);

}  // namespace cstar
