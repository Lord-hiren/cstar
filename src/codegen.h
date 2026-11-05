// codegen.h — interface between the CLI driver and the C backend.
// Shared contract.
#pragma once

#include <string>

#include "ast.h"

namespace cstar {

// Generates C11 code for `program` (which must already have passed
// semantic_check) into the file `out_c_path`. The generated file includes
// "cstar_rt.h" and is compiled together with runtime/cstar_rt.c as:
//   clang -std=c11 -O2 -o <out> gen.c cstar_rt.c -lm
// Returns false and (best-effort) reports a diagnostic message on failure.
bool generate_c(const Program& program, const std::string& out_c_path);

}  // namespace cstar
