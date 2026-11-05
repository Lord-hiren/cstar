#!/usr/bin/env python3
"""Deterministic generator for the cstar M1 benchmark .pen cases.

M1 has no loops and no conditionals (docs/contracts/m1-semantics.md §1), so iteration is
emulated with FINITE ACYCLIC CALL CHAINS: K distinct functions

    f0(a) = <one step of work>
    fi(a) = f{i-1}(<one step of work>)      for i = 1..K-1

repeated N times as straight-line assignments in top-level (global) scope.
Total step count = K * N, max call-stack depth = K, source size ~ O(K + N)
instead of O(K*N).  The chain is a DAG (f0 does not recurse), so programs
TERMINATE.  Note: genuine recursion can never terminate in M1 — with no
conditional there is no base case — and crashes the M1 runtime with SIGSEGV
(exit 139); see docs/performance/baseline-m1.md.

Every case has an identical C++ twin in bench/cases/<name>.cpp: same
arithmetic sequence, same data types (int64 two's-complement wrap ==
uint64_t; % and < are signed on both sides), same output bytes.  Float
formatting in the twins replicates the runtime's shortest round-trip rule.
Fully deterministic: no randomness anywhere.

Parameters were tuned from measured M1 compile/run characteristics
(docs/performance/baseline-m1.md "Findings"): clang compile time of the generated C is
superlinear in the number of top-level statements (N), so N <= ~12000, while
K (chain depth) is comparatively cheap up to ~2048.

Run:  python3 bench/gen.py        (writes bench/cases/<name>.pen)
"""

import os

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = os.path.join(HERE, "cases")

# name: (K = chain depth, N = top-level chain invocations)
PARAMS = {
    "int_loop":      (2048, 6336),   # 12,967,936 LCG steps
    "float_loop":    (2048, 6336),   # 12,967,936 float steps
    "call_chain":    (1024, 2344),   # 2,400,256 frames (6 int + 3 cmp ops each)
    "call_overhead": (2048, 6336),   # 12,967,936 pass-through calls
    "string_ops":    (125, 6000),    # 600,000 concatenations (period-5 reset)
    "mixed":         (125, 4000),    # 500,000 int steps + 400,000 concats
}

LCG_M = 1664525
LCG_C = 1013904223
LCG_SEED = 22695477

STR_LIT = "0123456789ABCDEF"  # 16 bytes, appended per string frame
STR_SEED = "seed0000"         # 8 bytes; chain reset value
STR_PERIOD = 5                # chain positions i%5==0 restart the accumulator
                              # from the 8-byte seed: keeps M1's leak-every-
                              # concatenation behavior linear (peak string is
                              # 8 + 16*(PERIOD-1) = 72 bytes).
# String frames append ON THE WAY UP (call down, then return r + LIT):
# a reset on the way down would discard every append below it.  A reset
# frame must still CALL the lower chain with the fresh seed and discard the
# result — simply `return "seed0000"` severs the chain, leaving every frame
# below it unreachable (verified: unreferenced static functions get dropped
# by clang even at -O0 — only 5 of 125 chain functions survived in the
# binary, and the program silently did 4x less work than its C++ twin).


def chain(name, k, sig, frame0, framei, init_lines, step, tail_lines):
    """Write a .pen file: f0..f{k-1} chain + N straight-line invocations."""
    lines = [
        "# %s: finite acyclic call chain K=%d, N=%d (M1 has no loops;" % (name, k, PARAMS[name][1]),
        "# iteration = chain repetition in straight-line top-level code).",
    ]
    lines.append("function f0(" + sig + "):")
    for l in frame0.split("\n"):
        lines.append("    " + l)
    for i in range(1, k):
        lines.append("function f" + str(i) + "(" + sig + "):")
        for l in framei.replace("__I__", str(i - 1)).split("\n"):
            lines.append("    " + l)
    lines += init_lines
    n = PARAMS[name][1]
    for _ in range(n):
        lines.append(step.replace("__K__", str(k - 1)))
    lines += tail_lines
    with open(os.path.join(CASES, name + ".pen"), "w") as f:
        f.write("\n".join(lines) + "\n")


def gen_int_loop():
    k, n = PARAMS["int_loop"]
    chain("int_loop", k, "a",
          "return a * %d + %d" % (LCG_M, LCG_C),
          "return f__I__(a * %d + %d)" % (LCG_M, LCG_C),
          ["a = %d" % LCG_SEED], "a = f__K__(a)", ["print(a)"])


def gen_float_loop():
    k, n = PARAMS["float_loop"]
    chain("float_loop", k, "x",
          "return x * 1.0000001 + 0.5",
          "return f__I__(x * 1.0000001 + 0.5)",
          ["x = 1.0"], "x = f__K__(x)", ["print(x)"])


CALL_CHAIN_FRAME = ("\n".join([
    "b = a * 3 + 17",
    "c = b % 1000003",           # constant nonzero divisor
    "d = c < 1000000007",        # comparison ops (M1's only "branch-like" ops)
    "e = d == True",
    "f = b != c",
    "g = e != f",
    "h = a - b",
    "return h * 5 + c",
]))


def gen_call_chain():
    k, n = PARAMS["call_chain"]
    framei = CALL_CHAIN_FRAME.replace("return h * 5 + c", "return f__I__(h * 5 + c)")
    chain("call_chain", k, "a", CALL_CHAIN_FRAME, framei,
          ["a = %d" % LCG_SEED], "a = f__K__(a)",
          ["z = a < 1000000007", "print(a, z)"])


def gen_call_overhead():
    k, n = PARAMS["call_overhead"]
    chain("call_overhead", k, "a", "return a", "return f__I__(a)",
          ["a = %d" % LCG_SEED], "a = f__K__(a)", ["print(a)"])


def gen_string_ops():
    k, n = PARAMS["string_ops"]
    frames = []
    for i in range(1, k):
        if i % STR_PERIOD == 0:
            # reset: restart from the seed BELOW, discard that result above
            frames.append('dummy = f__I__("%s")\nreturn "%s"' % (STR_SEED, STR_SEED))
        else:
            frames.append('r = f__I__(s)\nreturn r + "%s"' % STR_LIT)
    # f0 is a passthrough; appends accumulate on the way up.
    lines = [
        "# string_ops: %d-byte literal appends with period-%d seed resets;"
        % (len(STR_LIT), STR_PERIOD),
        "# K=%d, N=%d.  Appends unwind (call down, then concatenate) so the" % (k, n),
        "# resets cannot discard them below; resets bound M1's leak-every-",
        "# concatenation behavior to linear growth (peak string %d bytes)."
        % (len(STR_SEED) + len(STR_LIT) * (STR_PERIOD - 1)),
        "function f0(s):",
        "    return s",
    ]
    for i in range(1, k):
        lines.append("function f" + str(i) + "(s):")
        body = frames[i - 1].replace("__I__", str(i - 1))
        for l in body.split("\n"):
            lines.append("    " + l)
    lines.append('s = "%s"' % STR_SEED)
    for _ in range(n):
        lines.append('s = "%s"' % STR_SEED)
        lines.append("s = f" + str(k - 1) + "(s)")
    lines.append("print(s)")
    with open(os.path.join(CASES, "string_ops.pen"), "w") as f:
        f.write("\n".join(lines) + "\n")


def gen_mixed():
    k, n = PARAMS["mixed"]
    lines = [
        "# mixed: N=%d repetitions of (K=%d LCG steps) + (period-%d string"
        % (n, k, STR_PERIOD),
        "# appends with seed resets, appends on the unwind).  Two parallel",
        "# chains: M1 functions return a single value and cannot write",
        "# globals, so an int and a string cannot share one chain.",
        "function f0(a):",
        "    return a * %d + %d" % (LCG_M, LCG_C),
    ]
    for i in range(1, k):
        lines.append("function f" + str(i) + "(a):")
        lines.append("    return f" + str(i - 1) + "(a * %d + %d)" % (LCG_M, LCG_C))
    lines.append("function g0(s):")
    lines.append("    return s")
    for i in range(1, k):
        lines.append("function g" + str(i) + "(s):")
        if i % STR_PERIOD == 0:
            lines.append('    dummy = g' + str(i - 1) + '("%s")' % STR_SEED)
            lines.append('    return "%s"' % STR_SEED)
        else:
            lines.append("    r = g" + str(i - 1) + "(s)")
            lines.append('    return r + "%s"' % STR_LIT)
    lines.append("a = %d" % LCG_SEED)
    lines.append('s = "%s"' % STR_SEED)
    for _ in range(n):
        lines.append("a = f" + str(k - 1) + "(a)")
        lines.append('s = "%s"' % STR_SEED)
        lines.append("s = g" + str(k - 1) + "(s)")
    lines.append("print(a, s)")
    with open(os.path.join(CASES, "mixed.pen"), "w") as f:
        f.write("\n".join(lines) + "\n")


def gen_startup():
    with open(os.path.join(CASES, "startup.pen"), "w") as f:
        f.write("# startup: process start + runtime init + one print + teardown.\n"
                "main():\n"
                "    print(0)\n")


GENERATORS = {
    "int_loop": gen_int_loop,
    "float_loop": gen_float_loop,
    "call_chain": gen_call_chain,
    "call_overhead": gen_call_overhead,
    "string_ops": gen_string_ops,
    "mixed": gen_mixed,
    "startup": gen_startup,
}


def main():
    os.makedirs(CASES, exist_ok=True)
    for name in ["startup", "int_loop", "float_loop", "call_chain",
                 "call_overhead", "string_ops", "mixed"]:
        GENERATORS[name]()
        path = os.path.join(CASES, name + ".pen")
        with open(path) as f:
            nlines = sum(1 for _ in f)
        k, n = PARAMS.get(name, (0, 0))
        print("%-14s %9d bytes %8d lines  (K=%-4d N=%-6d)" %
              (name, os.path.getsize(path), nlines, k, n))


if __name__ == "__main__":
    main()
