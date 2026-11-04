// semantic.h — Pentest M1 semantic pass (docs/contracts/m1-semantics.md §6–§7).
// Declaration also lives in frontend.h (shared contract).
#pragma once

#include "ast.h"
#include "diagnostics.h"

namespace pentest {

// Declaration discovery + name resolution + validation. Annotates `out`
// (Program.functions, Program.globals, Program.has_main, FuncDecl.c_name/
// locals, Var/Assign resolutions, is_builtin_print). Reports ALL semantic
// errors through `diags`; returns false if any were reported.
bool semantic_check(Program& out, Diagnostics& diags);

// M2-2C §2.2: the builtin exception-class names (Exception, RuntimeError,
// TypeError, ValueError, NameError, ZeroDivisionError, IndexError,
// KeyError, AttributeError). They are READABLE singleton values — resolve
// injects them into Program.globals and codegen binds each slot from
// pt_exc_class() — while the reserved set keeps them non-assignable.
// MUST stay in sync with pt_exc_cls_names in runtime/pentest_rt.c.
bool is_builtin_exception_class(const std::string& name);

}  // namespace pentest
