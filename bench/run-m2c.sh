#!/usr/bin/env bash
# bench/run-m2c.sh — Pentest Stage 2C performance baseline runner.
#
# Companion to bench/run-m2b.sh (frozen on the recorded 2B run) and
# bench/run.sh (frozen on the preserved M1 compiler). This runner measures
# the CURRENT compiler (build/pentest, 0.2.0-m2c) on the same Stage-2B
# case set in bench/cases/m2b/ — the headline of the 2C re-record is the
# GARBAGE COLLECTOR: 2B leaked every heap object (RSS grew with the
# workload: 44–344 MB); under 2C the same workloads must sit at the
# ~12 MB floor, with timings showing the collector's cost.
#
#   * while_int / while_float / while_string — the SAME workloads as the
#     M1 baseline (int_loop / float_loop / string_ops: identical step
#     counts, identical arithmetic) expressed with REAL loops;
#   * list_ops / map_ops / set_ops / text_ops / closure_ops /
#     dispatch_ops / coll_mixed — workloads M1 could not express at all
#     (collections, text methods, closures, indirect calls).
#
# For every case (<name>.pen + <name>.cpp twin):
#   1. regenerates the .pen from bench/gen-m2b.py (deterministic),
#   2. builds the C++ twin:  clang++ -std=c++17 -O2  -> bench/bin/m2c/
#   3. builds the .pen with the CURRENT compiler (`build` mode, which
#      NEVER caches — see docs/contracts/m1-semantics.md §9), recording cold
#      compile wall time,
#   4. asserts both binaries print byte-identical output (fails loudly),
#   5. runs each binary BENCH_REPS times, pinned to one core with
#      `taskset -c 0` when available, timing EXECUTION ONLY, reporting
#      min and median wall ms, plus peak RSS (the headline column — see
#      the before/after-vs-2B section),
#   6. probes the `run`-mode content-hash cache on while_int: cold vs warm,
#   7. emits bench/results/m2c-baseline.md including before/after tables
#      against the recorded 2B baseline (timings, RSS, compile times).
#
# Usage:  bash bench/run-m2c.sh
# Env:    BENCH_REPS=7       timing repetitions
#         BENCH_REBUILD=1    reuse existing bench/bin/m2c binaries
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH="$ROOT/bench"
PENTEST="$ROOT/build/pentest"
CASES="$BENCH/cases/m2b"
BIN="$BENCH/bin/m2c"
RESULTS="$BENCH/results"
GEN="$BENCH/gen-m2b.py"
M2B_TIMINGS="$RESULTS/timings-m2b.txt"
M2B_RSS="$RESULTS/rss-m2b.txt"
M2B_COMPILE="$RESULTS/compile-pen-m2b.txt"
REPS="${BENCH_REPS:-7}"
REBUILD="${BENCH_REBUILD:-0}"
CXX="${CXX:-clang++}"
CXXFLAGS="-std=c++17 -O2"
PEN_CLANG_FLAGS="clang -std=c11 -O2 -o <out> gen.c cstar_rt.c -lm"

command -v "$CXX" >/dev/null || { echo "error: $CXX not found" >&2; exit 1; }
[ -x "$PENTEST" ] || { echo "error: $PENTEST missing — build it with make" >&2; exit 1; }

CASE_ORDER="while_int while_float while_string list_ops map_ops set_ops text_ops closure_ops dispatch_ops coll_mixed"
wl() {
  case "$1" in
    while_int)     echo '12,976,128 int LCG steps (real while loop)' ;;
    while_float)   echo '12,976,128 double steps (real while loop)' ;;
    while_string)  echo '750,000 frames: 600,000 appends + 150,000 resets (= M1 string_ops)' ;;
    list_ops)      echo '200,000 appends + sum pass + index reads' ;;
    map_ops)       echo '150,000 inserts + 300,000 lookups + key-sum pass' ;;
    set_ops)       echo '240,000 adds (half dup) + 120,000 finds + full drain' ;;
    text_ops)      echo '200,000 x (upper+split+join+replace+find+starts_with)' ;;
    closure_ops)   echo '300,000 closure creations + calls via function value' ;;
    dispatch_ops)  echo '300,000 indirect calls (positional+named+**kwargs)' ;;
    coll_mixed)    echo '202,752 reps int+list+map+text (period-80 reset)' ;;
    *)             echo '' ;;
  esac
}

PIN=""
if command -v taskset >/dev/null && taskset -c 0 true 2>/dev/null; then
  PIN="taskset -c 0"
else
  echo "note: taskset -c 0 unavailable — timings are unpinned" >&2
fi

mkdir -p "$BIN" "$RESULTS"
rm -rf "$RESULTS/cache-probe-m2c"

echo "== Pentest 2C baseline: $($PENTEST --version) =="

# 1. regenerate .pen cases ---------------------------------------------------
python3 "$GEN"

# 2. build C++ twins ---------------------------------------------------------
: > "$RESULTS/compile-cxx-m2c.txt"
for cpp in "$CASES"/*.cpp; do
  name="$(basename "$cpp" .cpp)"
  out="$BIN/$name.cpp.bin"
  if [ "$REBUILD" = 1 ] || [ ! -x "$out" ] || [ "$cpp" -nt "$out" ]; then
    echo "cxx  $name"
    t0=$(date +%s%N)
    $CXX $CXXFLAGS -o "$out" "$cpp"
    t1=$(date +%s%N)
    echo "$name $(( (t1 - t0) / 1000000 ))" >> "$RESULTS/compile-cxx-m2c.txt"
  fi
done

# 3. build .pen programs (cold compile time; `build` never caches) -----------
: > "$RESULTS/compile-pen-m2c.txt"
for name in $CASE_ORDER; do
  pen="$CASES/$name.pen"
  out="$BIN/$name.pen.bin"
  if [ "$REBUILD" = 1 ] || [ ! -x "$out" ] || [ "$pen" -nt "$out" ]; then
    echo "pen  $name (cold compile)"
    t0=$(date +%s%N)
    "$PENTEST" build "$pen" -o "$out"
    t1=$(date +%s%N)
    echo "$name $(( (t1 - t0) / 1000000 ))" >> "$RESULTS/compile-pen-m2c.txt"
  else
    echo "pen  $name (reused existing binary — compile time not re-measured)"
    echo "$name -" >> "$RESULTS/compile-pen-m2c.txt"
  fi
done

# 4. output equality assertion ----------------------------------------------
echo "== verifying outputs =="
for name in $CASE_ORDER; do
  "$BIN/$name.cpp.bin" > "$BIN/$name.cpp.out"
  "$BIN/$name.pen.bin" > "$BIN/$name.pen.out"
  if ! cmp -s "$BIN/$name.cpp.out" "$BIN/$name.pen.out"; then
    echo "FATAL: output mismatch for $name" >&2
    diff <(od -c "$BIN/$name.cpp.out") <(od -c "$BIN/$name.pen.out") >&2 || true
    exit 1
  fi
  echo "  $name: outputs match ($(wc -c < "$BIN/$name.cpp.out") bytes)"
done

# 5. runtime measurement -----------------------------------------------------
time_ms() {
  local t0 t1
  t0=$(date +%s%N)
  "$@" > /dev/null
  t1=$(date +%s%N)
  awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.3f\n", (b - a) / 1e6 }'
}
stats() {
  sort -n | awk -v n="$REPS" '
    { v[NR] = $1 }
    END {
      half = int(n / 2) + 1
      if (n % 2) m = v[half]; else m = (v[half] + v[half + 1]) / 2
      printf "%.3f %.3f", v[1], m
    }'
}
maxrss_kb() {
  python3 -c 'import resource, subprocess, sys
subprocess.run(sys.argv[1:], stdout=subprocess.DEVNULL, check=True)
print(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)' "$@"
}

: > "$RESULTS/timings-m2c.txt"
: > "$RESULTS/rss-m2c.txt"
for name in $CASE_ORDER; do
  echo "run  $name x$REPS (pinned: ${PIN:-no})"
  for side in pen cpp; do
    samples=$(for _ in $(seq 1 "$REPS"); do
      if [ -n "$PIN" ]; then time_ms taskset -c 0 "$BIN/$name.$side.bin"; else time_ms "$BIN/$name.$side.bin"; fi
    done)
    read -r pmin pmed <<<"$(echo "$samples" | stats)"
    echo "$name $side $pmin $pmed" >> "$RESULTS/timings-m2c.txt"
  done
  if [ -n "$PIN" ]; then
    rpen=$(maxrss_kb taskset -c 0 "$BIN/$name.pen.bin")
    rcpp=$(maxrss_kb taskset -c 0 "$BIN/$name.cpp.bin")
  else
    rpen=$(maxrss_kb "$BIN/$name.pen.bin")
    rcpp=$(maxrss_kb "$BIN/$name.cpp.bin")
  fi
  echo "$name $rpen $rcpp" >> "$RESULTS/rss-m2c.txt"
done

# 6. run-mode content-hash cache probe (while_int) ---------------------------
echo "== run-mode cache probe (while_int) =="
export XDG_CACHE_HOME="$RESULTS/cache-probe-m2c"
rm -rf "$XDG_CACHE_HOME"; mkdir -p "$XDG_CACHE_HOME"
t0=$(date +%s%N); "$PENTEST" run "$CASES/while_int.pen" > /dev/null; t1=$(date +%s%N)
cache_cold=$(( (t1 - t0) / 1000000 ))
t0=$(date +%s%N); "$PENTEST" run "$CASES/while_int.pen" > /dev/null; t1=$(date +%s%N)
cache_warm=$(( (t1 - t0) / 1000000 ))
rm -rf "$XDG_CACHE_HOME"
unset XDG_CACHE_HOME

# 7. report ------------------------------------------------------------------
ts=$(date -u '+%Y-%m-%d %H:%M:%S UTC')
machine=$(uname -a)
cpu=$(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //')
clangv=$($CXX --version | head -1)
clangc=$(clang --version | head -1)
ram=$(free -b | awk '/^Mem:/ { printf "%.1f GiB", $2 / 1073741824 }')
cores=$(nproc)
pentest_v=$("$PENTEST" --version)
if [ -n "$PIN" ]; then pin_note="taskset -c 0 (pinned)"; else pin_note="none (unpinned)"; fi

# before/after lookups from the recorded 2B baseline (if present). Those
# raw files use "name side min med" (timings), "name rpen rcpp" (rss) and
# "name ms" (compile) whitespace rows.
m2b_med() {  # m2b_med <case> -> Pentest median ms, or "-"
  [ -f "$M2B_TIMINGS" ] || { echo "-"; return; }
  awk -v n="$1" '$1 == n && $2 == "pen" { print $4; exit }' "$M2B_TIMINGS"
}
m2b_rss() {  # m2b_rss <case> -> Pentest peak RSS KB, or "-"
  [ -f "$M2B_RSS" ] || { echo "-"; return; }
  awk -v n="$1" '$1 == n { print $2; exit }' "$M2B_RSS"
}
m2b_build() {  # m2b_build <case> -> Pentest build ms, or "-"
  [ -f "$M2B_COMPILE" ] || { echo "-"; return; }
  awk -v n="$1" '$1 == n { print $2; exit }' "$M2B_COMPILE"
}

emit() {
cat <<EOF
# Pentest Stage 2C performance baseline

- Date: $ts
- Machine: \`$machine\`
- CPU: $cpu ($cores logical cores), RAM: $ram
- Compiler (C++ twins): $clangv
- Compiler (Pentest backend, invoked internally): $clangc
- Pentest compiler: $pentest_v
- C++ twin flags: \`$CXX $CXXFLAGS\`
- Pentest compile flags (fixed by the CLI): \`$PEN_CLANG_FLAGS\`
- Timing pinning: $pin_note
- Repetitions: $REPS per binary; **min** and **median** wall-clock ms of
  program execution only (compile excluded: .pen programs are built ahead
  of time with \`build\`, which never caches).
- Methodology identical to the 2B baseline (bench/run-m2b.sh +
  docs/performance/baseline-m2b.md, same machine/pinning/reps) so the before/after
  tables below are apples-to-apples; the 2B rows are the frozen
  \`0.2.0-m2b\` leak-everything run.

## Runtime (program execution only)

| Case | Workload | Pentest min (ms) | Pentest median (ms) | C++ min (ms) | C++ median (ms) | Median ratio | Peak RSS Pentest (KB) | Peak RSS C++ (KB) |
|---|---|---|---|---|---|---|---|---|
EOF
for name in $CASE_ORDER; do
  penline=$(grep "^$name pen " "$RESULTS/timings-m2c.txt")
  cppline=$(grep "^$name cpp " "$RESULTS/timings-m2c.txt")
  rssline=$(grep "^$name " "$RESULTS/rss-m2c.txt")
  pmin=$(echo "$penline" | awk '{print $3}'); pmed=$(echo "$penline" | awk '{print $4}')
  cmin=$(echo "$cppline" | awk '{print $3}'); cmed=$(echo "$cppline" | awk '{print $4}')
  rpen=$(echo "$rssline" | awk '{print $2}'); rcpp=$(echo "$rssline" | awk '{print $3}')
  ratio=$(awk -v a="$pmed" -v b="$cmed" 'BEGIN { if (b > 0) printf "%.1fx", a / b; else printf "n/a" }')
  printf '| %s | %s | %s | %s | %s | %s | %s | %s | %s |\n' \
    "$name" "$(wl "$name")" "$pmin" "$pmed" "$cmin" "$cmed" "$ratio" "$rpen" "$rcpp"
done

cat <<'EOF'

## Before/after vs the recorded 2B baseline (same machine, pinning and reps)

### Timings — the collector's cost (2B leak-everything vs 2C mark & sweep)

| Case | 2B median (ms) | 2C median (ms) | 2C/2B | 2B build (ms) | 2C build (ms) |
|---|---|---|---|---|---|
EOF
for name in $CASE_ORDER; do
  cmed=$(grep "^$name pen " "$RESULTS/timings-m2c.txt" | awk '{print $4}')
  bmed=$(m2b_med "$name")
  cbuild=$(awk -v n="$name" '$1 == n { print $2 }' "$RESULTS/compile-pen-m2c.txt")
  bbuild=$(m2b_build "$name")
  tax=$(awk -v a="$bmed" -v b="$cmed" 'BEGIN { if (a != "-" && a > 0) printf "%.2fx", b / a; else printf "n/a" }')
  printf '| %s | %s | %s | %s | %s | %s |\n' "$name" "$bmed" "$cmed" "$tax" "$bbuild" "$cbuild"
done

cat <<'EOF'

### Peak RSS — the 2C headline (2B grew without bound; 2C must sit at the floor)

| Case | 2B Pentest RSS (KB) | 2C Pentest RSS (KB) | Collapse | 2C C++ RSS (KB) |
|---|---|---|---|---|
EOF
for name in $CASE_ORDER; do
  rssline=$(grep "^$name " "$RESULTS/rss-m2c.txt")
  r2c=$(echo "$rssline" | awk '{print $2}'); rcpp=$(echo "$rssline" | awk '{print $3}')
  r2b=$(m2b_rss "$name")
  factor=$(awk -v a="$r2b" -v b="$r2c" 'BEGIN { if (a != "-" && b > 0) printf "%.1fx smaller", a / b; else printf "n/a" }')
  printf '| %s | %s | %s | %s | %s |\n' "$name" "$r2b" "$r2c" "$factor" "$rcpp"
done

cat <<'EOF'

(2B rows are read from bench/results/timings-m2b.txt, rss-m2b.txt and
compile-pen-m2b.txt — the frozen `0.2.0-m2b` run on this machine. The 2B
RSS column is the leak: 44–344 MB of heap for workloads that need ~12 MB;
the 2C column is the same workloads after the non-moving mark & sweep
collector landed (SPEC §7.3). The timing ratio is the collector's cost.)

## Compile time (.pen -> native, cold; `build` never caches)

| Case | .pen source bytes | Pentest build (ms) | C++ twin build (ms) |
|---|---|---|---|
EOF
for name in $CASE_ORDER; do
  pms=$(awk -v n="$name" '$1 == n { print $2 }' "$RESULTS/compile-pen-m2c.txt")
  cms=$(awk -v n="$name" '$1 == n { print $2 }' "$RESULTS/compile-cxx-m2c.txt" 2>/dev/null || echo "-")
  [ -n "$cms" ] || cms="-"
  bytes=$(wc -c < "$CASES/$name.pen")
  printf '| %s | %s | %s | %s |\n' "$name" "$bytes" "$pms" "$cms"
done
cat <<EOF

## \`run\`-mode content-hash cache probe (while_int, fresh XDG_CACHE_HOME)

| Measurement | Wall time (ms) |
|---|---|
| cold \`run\` (compile + exec) | $cache_cold |
| warm \`run\` (cache hit, exec only) | $cache_warm |

The cache lives in \`\$XDG_CACHE_HOME/pentest/\` (fallback \`~/.cache/pentest/\`),
keyed by sha256(source bytes + compiler version). The probe directory was
removed after measuring. All other timings above use \`build\`-mode binaries,
which are unaffected by this cache.
EOF
}

emit | tee "$RESULTS/m2c-baseline.md"
echo ""
echo "wrote $RESULTS/m2c-baseline.md"
