#!/usr/bin/env python3
"""Deterministic generator for the Pentest Stage 2B benchmark .pen cases.

Stage 2B adds real `while`/`for` loops, collections (list/map/set), Text and
Bytes, and the full function sector (closures, defaults, named args). Unlike
M1 (docs/performance/baseline-m1.md), iteration needs no call-chain emulation: a `while`
loop is O(1) source statements, so the compile-time superlinear cliff that
dominated M1 (111 s for a 13 M-step chain) disappears.

Two case families:

  * `while_*`  — the SAME workloads as the M1 baseline (int LCG, float LCG,
                 small-string appends) expressed idiomatically with real
                 loops, giving a direct before/after ratio vs M1.
  * `coll_*` / `map_*` / `text_*` / `closure_*` — Stage-2B-only workloads
                 M1 could not express at all (collections, text methods,
                 closures / function values).

Every case has an identical C++ twin in bench/cases/m2b/<name>.cpp: same
arithmetic sequence, same data types (int64 two's-complement wrap ==
uint64_t; % and < signed on both sides), same output bytes. Float formatting
in the twins is a line-for-line copy of runtime/pentest_rt.c pt_fmt_float
(unchanged from M1). Fully deterministic: no randomness anywhere.

Run:  python3 bench/gen-m2b.py        (writes bench/cases/m2b/<name>.pen)
"""

import os

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = os.path.join(HERE, "cases", "m2b")

LCG_M = 1664525
LCG_C = 1013904223
LCG_SEED = 22695477

STR_LIT = "0123456789ABCDEF"  # 16 bytes, appended per iteration
STR_SEED = "seed0000"         # 8 bytes; reset value (bounds peak string)

# Iteration counts, chosen so each case runs ~0.3-1.5 s on the reference
# machine (min/median of 7 reps stay stable) while keeping the no-GC heap
# growth (every intermediate object leaks by design, 2C) to tens of MB.
STEPS_INT = 12976128          # matches M1 int_loop / float_loop exactly
STEPS_FLOAT = 12976128
STR_ITERS = 600000            # matches M1 string_ops concat count
LIST_N = 200000
MAP_N = 150000
SET_N = 120000
TEXT_N = 200000
CLOSURE_N = 300000
DISPATCH_N = 300000


def write(name, lines):
    with open(os.path.join(CASES, name + ".pen"), "w") as f:
        f.write("\n".join(lines) + "\n")


def gen_while_int():
    # Direct successor of M1 int_loop: identical LCG, identical step count,
    # now a real while loop (O(1) statements) instead of a K x N call chain.
    write("while_int", [
        "# while_int: %d int LCG steps `a = a*%d + %d` (wrapping int64) in a" % (STEPS_INT, LCG_M, LCG_C),
        "# real while loop. Before/after partner: M1 int_loop (same steps).",
        "main:",
        "    a = %d" % LCG_SEED,
        "    i = 0",
        "    while i < %d:" % STEPS_INT,
        "        a = a * %d + %d" % (LCG_M, LCG_C),
        "        i += 1",
        "    print(a)",
    ])


def gen_while_float():
    # Direct successor of M1 float_loop.
    write("while_float", [
        "# while_float: %d double steps `x = x*1.0000001 + 0.5` in a real" % STEPS_FLOAT,
        "# while loop. Before/after partner: M1 float_loop (same steps).",
        "main:",
        "    x = 1.0",
        "    i = 0",
        "    while i < %d:" % STEPS_FLOAT,
        "        x = x * 1.0000001 + 0.5",
        "        i += 1",
        "    print(x)",
    ])


def gen_while_string():
    # Successor of M1 string_ops. The M1 chain's period-5 structure does
    # 100 appends + 25 seed-resets per 125 frames (reset frames do NOT
    # append), producing the intermediate-length sequence 8 -> 24 -> 40 ->
    # 56 -> 72 (peak 72) and exactly 600,000 appends + 150,000 resets over
    # N=6000 chain runs. This loop replicates that structure exactly
    # (reset-without-append, 750,000 frames = 600K appends + 150K resets),
    # so the before/after ratio isolates loop-vs-chain cost instead of
    # mixing in different string lengths or concat counts.
    write("while_string", [
        "# while_string: 750,000 frames = 600,000 x 16-byte appends +",
        "# 150,000 period-5 seed resets (reset does not append) — the exact",
        "# intermediate-length sequence and counts of the M1 string_ops",
        "# chain (peak 72 bytes; the no-GC runtime leaks every intermediate).",
        "# Before/after partner: M1 string_ops.",
        "main:",
        "    s = \"%s\"" % STR_SEED,
        "    i = 0",
        "    while i < %d:" % (STR_ITERS * 5 // 4),
        "        if i % 5 == 0:",
        "            s = \"%s\"" % STR_SEED,
        "        else:",
        "            s = s + \"%s\"" % STR_LIT,
        "        i += 1",
        "    print(len(s), s[0], s[len(s) - 1])",
    ])


def gen_list_ops():
    # Stage-2B-only: list append + index read + sum via for-over-list.
    write("list_ops", [
        "# list_ops: %d appends, %d indexed reads, one full sum pass." % (LIST_N, LIST_N),
        "# M1 has no lists; this is new expressible work.",
        "main:",
        "    xs = []",
        "    i = 0",
        "    while i < %d:" % LIST_N,
        "        xs.append(i * 3 + 1)",
        "        i += 1",
        "    total = 0",
        "    for v in xs:",
        "        total = total + v",
        "    print(len(xs), xs[0], xs[%d], xs[len(xs) - 1], total)" % (LIST_N - 1),
    ])


def gen_map_ops():
    # Stage-2B-only: map insert, key lookup, get-with-default, key iteration.
    write("map_ops", [
        "# map_ops: %d inserts, %d lookups, %d get-with-default misses," % (MAP_N, MAP_N, MAP_N),
        "# one key-sum pass. Insertion order is preserved (keys()).",
        "main:",
        "    m = {}",
        "    i = 0",
        "    while i < %d:" % MAP_N,
        "        m[i] = i * 2 + 7",
        "        i += 1",
        "    total = 0",
        "    i = 0",
        "    while i < %d:" % MAP_N,
        "        total = total + m[i] + m.get(i + %d, 0)" % MAP_N,
        "        i += 1",
        "    ksum = 0",
        "    for k in m:",
        "        ksum = ksum + k",
        "    print(len(m), m[0], m[%d], total, ksum)" % (MAP_N - 1),
    ])


def gen_set_ops():
    # Stage-2B-only: set add (dedup), membership, pop-all drain.
    # Insert i and i/2 so half the adds are duplicates -> len == ceil(N/2).
    write("set_ops", [
        "# set_ops: %d adds (half duplicates via i>>1), %d membership tests," % (SET_N, SET_N),
        "# drain via pop() (insertion order).",
        "main:",
        "    s = set()",
        "    i = 0",
        "    while i < %d:" % SET_N,
        "        s.add(i)",
        "        s.add(i >> 1)",
        "        i += 1",
        "    hits = 0",
        "    i = 0",
        "    while i < %d:" % SET_N,
        "        if i in s:",
        "            hits += 1",
        "        i += 1",
        "    popped = 0",
        "    drain = 0",
        "    while len(s) > 0:",
        "        popped = popped + s.pop()",
        "        drain += 1",
        "    print(hits, drain, popped)",
    ])


def gen_text_ops():
    # Stage-2B-only: text method surface (upper/split/join/replace/find).
    # Workload repeats a fixed pipeline TEXT_N times over a short text; the
    # compiler must not fold it (runtime method dispatch is opaque).
    write("text_ops", [
        "# text_ops: %d passes of upper+split+join+replace+find+starts_with" % TEXT_N,
        "# over a 64-code-point text (methods dispatch at runtime).",
        "main:",
        "    base = \"Alpha,Beta,Gamma Deltax Epsilon,Filth\"",
        "    acc = 0",
        "    i = 0",
        "    while i < %d:" % TEXT_N,
        "        u = base.upper()",
        "        parts = u.split(\" \")",
        "        joined = \"|\".join(parts)",
        "        rep = base.replace(\"a\", \"@\")",
        "        f = rep.find(\"@\")",
        "        if base.starts_with(\"Alpha\"):",
        "            acc = acc + len(joined) + f + len(rep)",
        "        i += 1",
        "    print(acc, len(base), base.find(\"Gamma\"))",
    ])


def gen_closure_ops():
    # Stage-2B-only: closure creation + calls through a function value.
    # make_adder builds a closure capturing n; calling it goes through a
    # dynamic function value (pt_call / funmeta), the function-sector hot path.
    write("closure_ops", [
        "# closure_ops: %d closure creations + calls through function values" % CLOSURE_N,
        "# (capture cell read + dynamic dispatch per call).",
        "make_adder(n):",
        "    add(x, by = 1):",
        "        return x + n + by",
        "    return add",
        "main:",
        "    total = 0",
        "    i = 0",
        "    while i < %d:" % CLOSURE_N,
        "        f = make_adder(i)",
        "        total = total + f(i) + f(i, by = 2)",
        "        i += 1",
        "    print(total)",
    ])


def gen_dispatch_ops():
    # Stage-2B-only: call a function value stored in a list/map (indirect
    # pt_call), plus named-arg + vararg/kwarg binding cost.
    write("dispatch_ops", [
        "# dispatch_ops: %d indirect calls through a function value held in a" % DISPATCH_N,
        "# map, mixing positional / named / *args / **kwargs binding.",
        "f(a, b = 5, *rest, **kw):",
        "    s = a + b",
        "    for r in rest:",
        "        s = s + r",
        "    for k in kw:",
        "        s = s + kw[k]",
        "    return s",
        "main:",
        "    m = {\"f\": f}",
        "    total = 0",
        "    i = 0",
        "    while i < %d:" % DISPATCH_N,
        "        total = total + m[\"f\"](i, b = 2, extra = 3)",
        "        i += 1",
        "    print(total)",
    ])


def gen_coll_mixed():
    # Stage-2B-only: interleaved int + list + map + text work (the 'mixed'
    # analogue) — a realistic mixed workload with all new value types live.
    write("coll_mixed", [
        "# coll_mixed: %d reps of (LCG step + list append + map write/lookup +" % (STEPS_INT // 64),
        "# text append with a period-80 reset). Mixed int/list/map/text",
        "# through the dynamic runtime; the reset bounds the no-GC string",
        "# leak to linear (an unreset accumulator would leak ~3.3 GB).",
        "main:",
        "    a = %d" % LCG_SEED,
        "    xs = []",
        "    m = {}",
        "    s = \"%s\"" % STR_SEED,
        "    i = 0",
        "    while i < %d:" % (STEPS_INT // 64),
        "        a = a * %d + %d" % (LCG_M, LCG_C),
        "        xs.append(a)",
        "        m[i] = a",
        "        if i % 80 == 0:",
        "            s = \"%s\"" % STR_SEED,
        "        s = s + \"%s\"" % STR_LIT,
        "        i += 1",
        "    total = 0",
        "    for v in xs:",
        "        total = total + v",
        "    msum = 0",
        "    for k in m:",
        "        msum = msum + m[k]",
        "    print(a, len(xs), len(m), total, msum, len(s))",
    ])


GENERATORS = [
    gen_while_int,
    gen_while_float,
    gen_while_string,
    gen_list_ops,
    gen_map_ops,
    gen_set_ops,
    gen_text_ops,
    gen_closure_ops,
    gen_dispatch_ops,
    gen_coll_mixed,
]


def main():
    os.makedirs(CASES, exist_ok=True)
    for gen in GENERATORS:
        gen()
    for gen in GENERATORS:
        name = gen.__name__[len("gen_"):]
        path = os.path.join(CASES, name + ".pen")
        with open(path) as f:
            nlines = sum(1 for _ in f)
        print("%-14s %7d bytes %5d lines" % (name, os.path.getsize(path), nlines))


if __name__ == "__main__":
    main()
