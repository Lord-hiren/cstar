// src/builtins.h — the Python-parity builtin table (SPEC §5.11.5).
//
// Single source of truth for the NEW Python builtins: semantic uses it to
// validate arity and named arguments, codegen uses it to bind arguments
// into the runtime call's fixed slot layout. The 15 original Pentest
// builtins (print/range/len/...) keep their hand-written checks in
// semantic.cpp — this table never touches them.
//
// Slot model: every builtin has a FIXED slot list (its runtime signature).
// Positional arguments fill slots left-to-right; a named argument fills the
// slot whose name it matches. Unfilled optional slots arrive in the runtime
// as cs_missing() and are defaulted there. Validation rejects "multiple
// values for argument 'x'" exactly like Python.
//
// Design contract (docs/performance/optimization.md): every one of these compiles to a
// single direct cs_builtin_* call — no dispatch table, no dynamic lookup,
// nothing emitted unless the program uses it.

#ifndef CSTAR_BUILTINS_H_
#define CSTAR_BUILTINS_H_

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace cstar {

struct BuiltinSpec {
  const char* name;
  size_t min_args;                   // required slots (positional OR named)
  std::vector<const char*> slots;    // runtime slot layout (named-binding keys)
  bool variadic;                     // positionals unbounded past slots.size()
  bool variadic_key;                 // variadic form accepts a named `key`
  bool variadic_default;             // variadic form accepts a named `default`
  bool dict_form;                    // dict(): arbitrary named args = map keys
  bool slice_form;                   // slice(): 1 positional arg binds STOP
};

// The Python-parity builtins (SPEC §5.11.5). Order is the table walk order
// — linear scan is fine (call sites resolve once at compile time).
inline const BuiltinSpec* py_builtin_spec(const std::string& n) {
  static const BuiltinSpec kTable[] = {
      // name        min slots                                          flags
      {"abs", 1, {"x"}, false, false, false, false, false},
      {"all", 1, {"iterable"}, false, false, false, false, false},
      {"any", 1, {"iterable"}, false, false, false, false, false},
      {"ascii", 1, {"x"}, false, false, false, false, false},
      {"bin", 1, {"x"}, false, false, false, false, false},
      {"bytes", 0, {"source", "encoding", "errors"}, false, false, false, false, false},
      {"callable", 1, {"obj"}, false, false, false, false, false},
      {"chr", 1, {"i"}, false, false, false, false, false},
      {"delattr", 2, {"obj", "name"}, false, false, false, false, false},
      {"dict", 0, {"iterable"}, false, false, false, true, false},
      {"dir", 0, {"obj"}, false, false, false, false, false},
      {"divmod", 2, {"a", "b"}, false, false, false, false, false},
      {"enumerate", 1, {"iterable", "start"}, false, false, false, false, false},
      {"filter", 2, {"function", "iterable"}, false, false, false, false, false},
      {"format", 1, {"value", "format_spec"}, false, false, false, false, false},
      {"getattr", 2, {"obj", "name", "default"}, false, false, false, false, false},
      {"globals", 0, {}, false, false, false, false, false},
      {"hasattr", 2, {"obj", "name"}, false, false, false, false, false},
      {"hash", 1, {"obj"}, false, false, false, false, false},
      {"hex", 1, {"x"}, false, false, false, false, false},
      {"id", 1, {"obj"}, false, false, false, false, false},
      {"isinstance", 2, {"obj", "classinfo"}, false, false, false, false, false},
      {"iter", 1, {"obj"}, false, false, false, false, false},
      {"list", 0, {"iterable"}, false, false, false, false, false},
      {"locals", 0, {}, false, false, false, false, false},
      {"map", 2, {"function"}, true, false, false, false, false},
      {"max", 1, {}, true, true, true, false, false},
      {"min", 1, {}, true, true, true, false, false},
      {"next", 1, {"obj", "default"}, false, false, false, false, false},
      {"oct", 1, {"x"}, false, false, false, false, false},
      {"open", 1, {"file", "mode", "encoding"}, false, false, false, false, false},
      {"ord", 1, {"c"}, false, false, false, false, false},
      {"pow", 2, {"base", "exp", "mod"}, false, false, false, false, false},
      {"repr", 1, {"object"}, false, false, false, false, false},
      {"reversed", 1, {"sequence"}, false, false, false, false, false},
      {"round", 1, {"number", "ndigits"}, false, false, false, false, false},
      {"setattr", 3, {"obj", "name", "value"}, false, false, false, false, false},
      {"slice", 1, {"start", "stop", "step"}, false, false, false, false, true},
      {"sorted", 1, {"iterable", "key", "reverse"}, false, false, false, false, false},
      {"sum", 1, {"iterable", "start"}, false, false, false, false, false},
      {"tuple", 0, {"iterable"}, false, false, false, false, false},
      {"vars", 0, {"obj"}, false, false, false, false, false},
      {"zip", 0, {"iterables"}, true, false, false, false, false},
  };
  for (const BuiltinSpec& s : kTable)
    if (n == s.name) return &s;
  return nullptr;
}

// Recognized names that exist in the language but always fail at runtime —
// Python concepts with no Pentest equivalent (SPEC §5.11.6). Each raises
// RuntimeError with its own reason; nothing else about them is checked.
inline bool py_builtin_stub(const std::string& n) {
  return n == "breakpoint" || n == "classmethod" || n == "help" ||
         n == "issubclass" || n == "memoryview" || n == "object" ||
         n == "property" || n == "staticmethod" || n == "super";
}

// Python's dynamic-code trio: not in the language AT ALL — a dedicated
// compile-time error explains why (SPEC §5.11.6). Calling one is rejected
// in semantic; no runtime entry exists.
inline bool py_builtin_unavailable(const std::string& n) {
  return n == "eval" || n == "exec" || n == "compile";
}

// Every reserved NEW name: table entries + stubs + the unavailable trio
// (call-only, cannot be assigned — same treatment as `print`, SPEC §8).
inline bool py_builtin_reserved(const std::string& n) {
  return py_builtin_spec(n) != nullptr || py_builtin_stub(n) ||
         py_builtin_unavailable(n);
}

}  // namespace cstar

#endif  // CSTAR_BUILTINS_H_
