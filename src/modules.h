// modules.h — Stage 2D import linker and selective-loading pass.
//
// Link pipeline (called by the CLI after parse_source and before
// semantic_check):
//   1. Resolve every `import m` / `from m import x` to a source file
//      (relative to the importing file, the working directory, then
//      $PENTEST_PATH), parse it once, and reject cycles.
//   2. Rename each imported module's top-level declarations to
//      `__csm<id>_<name>` and rewrite that module's own global references.
//      `from` imports and `module.name` references in the importer are
//      rewritten to those unique internal names, so imported names never
//      collide with the importing program or another module.
//   3. Flatten the module graph into the entry Program in import order. Each
//      module's top-level statements execute once, at its first import.
//   4. Unless `strict` (check mode), drop imported declarations that are not
//      reachable from the entry program or from imported module
//      initialisation. The pass is a single worklist over the AST; it adds
//      no runtime instructions.
//
// The importer only pays for what it names: an unused imported function,
// class, or pure global is never analysed, emitted, or compiled.
#pragma once

#include <string>
#include <vector>

#include "ast.h"
#include "diagnostics.h"

namespace cstar {

struct LinkResult {
  // Canonical paths of every loaded module (entry excluded), in load order.
  std::vector<std::string> dependency_paths;
  // Cache-key material: path + '\0' + source for each dependency, in load
  // order. Empty when the program has no imports.
  std::string dependency_blob;
};

// Expands imports in `program` (already parsed from `entry_path`).
// strict=false is run/build mode: unreachable imported declarations are
// removed before semantic analysis. strict=true is check mode: every
// imported declaration is validated. Returns false after reporting errors.
bool link_imports(Program& program, const std::string& entry_path,
                  bool strict, LinkResult& result, Diagnostics& diags);

}  // namespace cstar
