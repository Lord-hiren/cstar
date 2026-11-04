#!/usr/bin/env python3
"""Interleaved scoreboard runner for tools/hpcbench (method in README.md).

Builds every .pen with build/pentest and every .c with clang under the
driver's own flags (-std=c11 -O3 -march=native, no fast-math), asserts
that all variants of a workload print the SAME checksum, then runs
HPCBENCH_ROUNDS (default 5) interleaved rounds — start order rotated each
round to defeat thermal/scheduler drift — and reports:

  * min kernel time (the program's own CLOCK_MONOTONIC section; the same
    clock Pentest's clock() reads) — the primary metric;
  * min whole-process CPU time (getrusage children) — secondary;
  * ratios: Pentest-fused vs C baselines, and vs the dynamic Pentest
    route where one exists.

Usage:  python3 tools/hpcbench/run.py [--write]   (--write: also save
tools/hpcbench/RESULTS.md).  Exit 1 on build failure, variant crash, or
checksum mismatch.
"""
import atexit
import math
import os
import platform
import resource
import shutil
import subprocess
import sys
import tempfile
import time

ROUNDS = int(os.environ.get("HPCBENCH_ROUNDS", "5"))
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
PENTEST = os.path.join(ROOT, "build", "pentest")
CFLAGS = ["-std=c11", "-O3", "-march=native"]

# workload -> [(label, kind, path, extra-argv)]
#   kind "pen" builds with `pentest build`; kind "c" with clang.
WORKLOADS = [
    ("saxpy", [
        ("pen-fused", "pen", "saxpy.pen", []),
        ("c-multi",   "c",   "saxpy.c",   ["multi"]),
        ("c-fused",   "c",   "saxpy.c",   ["fused"]),
    ]),
    ("chain3", [
        ("pen-fused", "pen", "chain3.pen",     []),
        ("pen-dyn",   "pen", "chain3_dyn.pen", []),
        ("c-multi",   "c",   "chain3.c",       ["multi"]),
        ("c-fused",   "c",   "chain3.c",       ["fused"]),
    ]),
    ("dot", [
        ("pen-fused", "pen", "dot.pen", []),
        ("c-fused",   "c",   "dot.c",   []),
    ]),
    ("collatz", [
        ("pen", "pen", "collatz.pen", []),
        ("c",   "c",   "collatz.c",   []),
    ]),
    ("ptrlist", [
        ("pen", "pen", "ptrlist.pen", []),
        ("c",   "c",   "ptrlist.c",   []),
    ]),
    ("io", [
        ("pen",      "pen", "io.pen",      []),
        ("pen-join", "pen", "io_join.pen", []),
        ("c-stdio",  "c",   "io.c",        ["stdio"]),
        ("c-raw",    "c",   "io.c",        ["raw"]),
    ]),
]

# The io workload writes its data file into the cwd (the shared temp
# dir); Pentest has no file-delete builtin, so the runner removes it.
IO_ARTIFACT = os.path.join(tempfile.gettempdir(), "hpcbench_io.dat")


def die(msg):
    print("FAIL:", msg, file=sys.stderr)
    sys.exit(1)


def build_all(tmp):
    """Return {label: [argv]} for every variant; builds once."""
    clang = shutil.which("clang")
    if not clang:
        die("clang not found on PATH")
    if not os.access(PENTEST, os.X_OK):
        die("build/pentest missing — run `make` first")
    cmds = {}
    for wname, variants in WORKLOADS:
        for label, kind, path, extra in variants:
            src = os.path.join(HERE, path)
            out = os.path.join(tmp, "%s_%s" % (wname, label))
            if kind == "pen":
                b = subprocess.run([PENTEST, "build", src, "-o", out],
                                   capture_output=True, text=True)
                if b.returncode != 0:
                    die("pentest build %s:\n%s%s" % (path, b.stdout, b.stderr))
            else:
                b = subprocess.run([clang] + CFLAGS + ["-o", out, src, "-lm"],
                                   capture_output=True, text=True)
                if b.returncode != 0:
                    die("clang %s:\n%s%s" % (path, b.stdout, b.stderr))
            cmds[(wname, label)] = [out] + extra
    return cmds


def run_once(cmd):
    """Run one variant; return (checksum, kernel_seconds, cpu_seconds)."""
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    p = subprocess.run(cmd, capture_output=True, text=True,
                       cwd=tempfile.gettempdir())
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    if p.returncode != 0:
        die("crash (%d): %s\n%s%s" % (p.returncode, " ".join(cmd),
                                      p.stdout, p.stderr))
    lines = [ln for ln in p.stdout.splitlines() if ln.strip() != ""]
    if len(lines) < 2:
        die("bad output from %s: %r" % (" ".join(cmd), p.stdout))
    try:
        ck = float(lines[0])
        kernel = float(lines[1])
    except ValueError:
        die("unparseable output from %s: %r" % (" ".join(cmd), p.stdout))
    cpu = ((after.ru_utime - before.ru_utime) +
           (after.ru_stime - before.ru_stime))
    return ck, kernel, cpu


def cpu_model():
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def cleanup_io_artifact():
    try:
        os.remove(IO_ARTIFACT)
    except OSError:
        pass


def main():
    atexit.register(cleanup_io_artifact)
    write = "--write" in sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="hpcbench-") as tmp:
        cmds = build_all(tmp)

        results = {}   # (wname, label) -> [kernel samples], [cpu samples]
        for wname, variants in WORKLOADS:
            # 1. checksum identity across all variants of the workload
            cks = {}
            for label, _kind, _path, _extra in variants:
                ck, _k, _c = run_once(cmds[(wname, label)])
                cks[label] = ck
            ref = next(iter(cks.values()))
            for label, ck in cks.items():
                if ck != ref:   # exact: values are exact dyadic doubles
                    die("%s checksum mismatch: %s=%r vs ref=%r"
                        % (wname, label, ck, ref))
            print("%s: checksum %r identical across %d variants"
                  % (wname, ref, len(cks)))

            # 2. interleaved rounds, rotated start order
            labels = [v[0] for v in variants]
            for rnd in range(ROUNDS):
                order = labels[rnd % len(labels):] + labels[:rnd % len(labels)]
                for label in order:
                    ck, kernel, cpu = run_once(cmds[(wname, label)])
                    if ck != ref:
                        die("%s/%s checksum drifted: %r vs %r"
                            % (wname, label, ck, ref))
                    results.setdefault((wname, label), ([], []))
                    results[(wname, label)][0].append(kernel)
                    results[(wname, label)][1].append(cpu)

    # 3. report
    lines = []
    lines.append("# hpcbench results — %s" % time.strftime("%Y-%m-%d %H:%M"))
    lines.append("")
    lines.append("* host: %s, %d CPUs, kernel %s" %
                 (cpu_model(), os.cpu_count() or 0, platform.release()))
    try:
        v = subprocess.run(["clang", "--version"], capture_output=True,
                           text=True).stdout.splitlines()[0]
        lines.append("* clang: %s" % v)
    except Exception:
        pass
    lines.append("* flags: `%s` for C, the driver's own line for `.pen` "
                 "(no fast-math either side)" % " ".join(CFLAGS))
    lines.append("* method: %d interleaved rounds, rotated start order; "
                 "min-of-%d; checksums asserted identical across variants "
                 "every round" % (ROUNDS, ROUNDS))
    lines.append("")
    lines.append("| workload | variant | kernel s (min) | CPU s (min) |")
    lines.append("|---|---|---|---|")
    for (wname, label), (ks, cs) in results.items():
        lines.append("| %s | %s | %.3f | %.3f |"
                     % (wname, label, min(ks), min(cs)))
    lines.append("")
    lines.append("Ratios (kernel time; the Pentest baseline is the workload's "
                 "first `pen*` variant; >1 means Pentest is faster):")
    lines.append("")
    lines.append("| workload | vs c-multi | vs c-fused | vs fastest C | "
                 "vs pen-dyn |")
    lines.append("|---|---|---|---|---|")
    for wname, variants in WORKLOADS:
        labels = [v[0] for v in variants]
        base = min(results[(wname,
                    next(v[0] for v in variants if v[1] == "pen"))][0])
        def ratio(other):
            if other not in labels:
                return "—"
            return "%.2f\u00d7" % (min(results[(wname, other)][0]) / base)
        c_times = [min(results[(wname, v[0])][0]) for v in variants
                   if v[1] == "c"]
        fastest = ("—" if not c_times
                   else "%.2f\u00d7" % (min(c_times) / base))
        lines.append("| %s | %s | %s | %s | %s |"
                     % (wname, ratio("c-multi"), ratio("c-fused"), fastest,
                        ratio("pen-dyn")))
    lines.append("")
    lines.append("Raw min kernel times per round are the reported values; "
                 "the full sample vectors stay in the runner output above.")
    out = "\n".join(lines)
    print()
    print(out)
    if write:
        with open(os.path.join(HERE, "RESULTS.md"), "w") as f:
            f.write(out + "\n")
        print("\nwrote tools/hpcbench/RESULTS.md")


if __name__ == "__main__":
    main()
