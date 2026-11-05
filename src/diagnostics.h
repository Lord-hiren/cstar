// diagnostics.h — compile-time error reporting (shared contract).
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ast.h"

namespace cstar {

// Prints errors to stderr in the form:
//   <file>:<line>:<col>: error: <message>
//   <source line>
//   <caret>
// When no location is available: "<file>: error: <message>" or
// "error: <message>".
//
// The linker (modules/imports) parses several files; every SourceLoc carries
// its file so a diagnostic always shows the right source line. set_source()
// keeps each file's lines (keyed by the path stored in SourceLoc::file).
class Diagnostics {
 public:
  // Attaches the source text so error output can show a caret line. Also
  // makes `file_path` the current file for unlocated errors.
  void set_source(const std::string& file_path, const std::string& source);

  // Located error. Safe to call before set_source (caret line omitted then).
  void error(SourceLoc loc, const std::string& message);

  // Unlocated error.
  void error(const std::string& message);

  bool has_error() const { return count_ > 0; }
  int error_count() const { return count_; }
  const std::string& file_path() const { return file_; }

 private:
  std::string file_;
  std::vector<std::string> lines_;
  std::unordered_map<std::string, std::vector<std::string>> sources_;
  int count_ = 0;
};

}  // namespace cstar
