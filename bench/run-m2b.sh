#!/usr/bin/env bash
# bench/run-m2b.sh — Pentest Stage 2B performance baseline runner.
#
# Companion to bench/run.sh (which stays frozen on the preserved M1
# compiler). This runner measures the CURRENT compiler (build/pentest,
# 0.2.0-m2b) on the Stage-2B case set in bench/cases/m2b/:
#
#   * while_int / while_float / while_string — the SAME workloads as the
#     M1 baseline (int_loop / float_loop / string_ops: identical step
#     counts, identical arithmetic) expressed with REAL loops, giving a
#     direct before/after ratio vs bench/results/m1-baseline.md;
#   * list_ops / map_ops / set_ops / text_ops / closure_ops /
#     dispatch_ops / coll_mixed — workloads M1 could not express at all
#     (collections, text methods, closures, indirect calls).
#
# For every case (<name>.pen + <name>.cpp twin):
#   1. regenerates the .pen from bench/gen-m2b.py (deterministic),
#   2. builds the C++ twin:  clang++ -std=c++17 -O2  -> bench/bin/m2b/
#   3. builds the .pen with the CURRENT compiler (`build` mode, which
#      NEVER caches — see docs/contracts/m1-semantics.md §9), recording cold
#      compile wall time,
#   4. asserts both binaries print byte-identical output (fails loudly),
#   5. runs each binary BENCH_REPS times, pinned to one core with
#      `taskset -c 0` when available, timing EXECUTION ONLY, reporting
#      min and median wall ms, plus peak RSS (the no-GC leak policy makes
#      the RSS column especially interesting for the text/closure cases),
#   6. probes the `run`-mode content-hash cache on while_int: cold vs warm,
#   7. emits bench/results/m2b-baseline.md including a before/after table
#      against the recorded M1 baseline where workloads pair up.
#
# Usage:  bash bench/run-m2b.sh
# Env:    BENCH_REPS=7       timing repetitions
#         BENCH_REBUILD=1    reuse existing bench/bin/m2b binaries
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH="$ROOT/bench"
PENTEST="$ROOT/build/pentest"
CASES="$BENCH/cases/m2b"
BIN="$BENCH/bin/m2b"
RESULTS="$BENCH/results"
GEN="$BENCH/gen-m2b.py"
M1_RESULTS="$RESULTS/m1-baseline.md"
REPS="${BENCH_REPS:-7}"
REBUILD="${BENCH_REBUILD:-0}"
CXX="${CXX:-clang++}"
CXXFLAGS="-std=c++17 -O2"
PEN_CLANG_FLAGS="clang -std=c11 -O2 -o <out> gen.c cstar_rt.c -lm"

command -v "$CXX" >/dev/null || { echo "error: $CXX not found" >&2; exit 1; }
[ -x "$PENTEST" ] || { echo "error: $PENTEST missing — build it with make" >&2; exit 1; }

CASE_ORDER="while_int while_float while_string list_ops map_ops set_ops text_ops closure_ops dispatch_ops coll_mixed"
# Workload pairs to the M1 baseline (2B case -> M1 case) for before/after.
PAIRS="while_int:int_loop while_float:float_loop while_string:string_ops"
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
rm -rf "$RESULTS/cache-probe-m2b"

echo "== Pentest 2B baseline: $($PENTEST --version) =="

# 1. regenerate .pen cases ---------------------------------------------------
python3 "$GEN"

# 2. build C++ twins ---------------------------------------------------------
: > "$RESULTS/compile-cxx-m2b.txt"
for cpp in "$CASES"/*.cpp; do
  name="$(basename "$cpp" .cpp)"
  out="$BIN/$name.cpp.bin"
  if [ "$REBUILD" = 1 ] || [ ! -x "$out" ] || [ "$cpp" -nt "$out" ]; then
    echo "cxx  $name"
    t0=$(date +%s%N)
    $CXX $CXXFLAGS -o "$out" "$cpp"
    t1=$(date +%s%N)
    echo "$name $(( (t1 - t0) / 1000000 ))" >> "$RESULTS/compile-cxx-m2b.txt"
  fi
done

# 3. build .pen programs (cold compile time; `build` never caches) -----------
: > "$RESULTS/compile-pen-m2b.txt"
for name in $CASE_ORDER; do
  pen="$CASES/$name.pen"
  out="$BIN/$name.pen.bin"
  if [ "$REBUILD" = 1 ] || [ ! -x "$out" ] || [ "$pen" -nt "$out" ]; then
    echo "pen  $name (cold compile)"
    t0=$(date +%s%N)
    "$PENTEST" build "$pen" -o "$out"
    t1=$(date +%s%N)
    echo "$name $(( (t1 - t0) / 1000000 ))" >> "$RESULTS/compile-pen-m2b.txt"
  else
    echo "pen  $name (reused existing binary — compile time not re-measured)"
    echo "$name -" >> "$RESULTS/compile-pen-m2b.txt"
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

: > "$RESULTS/timings-m2b.txt"
: > "$RESULTS/rss-m2b.txt"
for name in $CASE_ORDER; do
  echo "run  $name x$REPS (pinned: ${PIN:-no})"
  for side in pen cpp; do
    samples=$(for _ in $(seq 1 "$REPS"); do
      if [ -n "$PIN" ]; then time_ms taskset -c 0 "$BIN/$name.$side.bin"; else time_ms "$BIN/$name.$side.bin"; fi
    done)
    read -r pmin pmed <<<"$(echo "$samples" | stats)"
    echo "$name $side $pmin $pmed" >> "$RESULTS/timings-m2b.txt"
  done
  if [ -n "$PIN" ]; then
    rpen=$(maxrss_kb taskset -c 0 "$BIN/$name.pen.bin")
    rcpp=$(maxrss_kb taskset -c 0 "$BIN/$name.cpp.bin")
  else
    rpen=$(maxrss_kb "$BIN/$name.pen.bin")
    rcpp=$(maxrss_kb "$BIN/$name.cpp.bin")
  fi
  echo "$name $rpen $rcpp" >> "$RESULTS/rss-m2b.txt"
done

# 6. run-mode content-hash cache probe (while_int) ---------------------------
echo "== run-mode cache probe (while_int) =="
export XDG_CACHE_HOME="$RESULTS/cache-probe-m2b"
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

# before/after lookups from the recorded M1 baseline (if present).
# The M1 results file has two tables: the runtime table (11 pipe-fields:
# case | workload | 4 timings | ratio | 2 RSS) and the compile table (6
# fields: case | bytes | pen ms | cxx ms). Both start with "| <case> |",
# so match on NF to select the right row.
m1_med() {  # m1_med <m1case> -> pen median column, or "-"
  [ -f "$M1_RESULTS" ] || { echo "-"; return; }
  awk -F'|' -v n="$1" 'NF == 11 { gsub(/ /, "", $2); if ($2 == n) { gsub(/ /, "", $5); print $5; exit } }' "$M1_RESULTS"
}
m1_build() {  # m1_build <m1case> -> pen build ms column, or "-"
  [ -f "$M1_RESULTS" ] || { echo "-"; return; }
  awk -F'|' -v n="$1" 'NF == 6 { gsub(/ /, "", $2); if ($2 == n) { gsub(/ /, "", $4); print $4; exit } }' "$M1_RESULTS"
}

emit() {
cat <<EOF
# Pentest Stage 2B performance baseline

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
- Methodology identical to the M1 baseline (bench/run.sh +
  docs/performance/baseline-m1.md) so the before/after table below is apples-to-apples.

## Runtime (program execution only)

| Case | Workload | Pentest min (ms) | Pentest median (ms) | C++ min (ms) | C++ median (ms) | Median ratio | Peak RSS Pentest (KB) | Peak RSS C++ (KB) |
|---|---|---|---|---|---|---|---|---|
EOF
for name in $CASE_ORDER; do
  penline=$(grep "^$name pen " "$RESULTS/timings-m2b.txt")
  cppline=$(grep "^$name cpp " "$RESULTS/timings-m2b.txt")
  rssline=$(grep "^$name " "$RESULTS/rss-m2b.txt")
  pmin=$(echo "$penline" | awk '{print $3}'); pmed=$(echo "$penline" | awk '{print $4}')
  cmin=$(echo "$cppline" | awk '{print $3}'); cmed=$(echo "$cppline" | awk '{print $4}')
  rpen=$(echo "$rssline" | awk '{print $2}'); rcpp=$(echo "$rssline" | awk '{print $3}')
  ratio=$(awk -v a="$pmed" -v b="$cmed" 'BEGIN { if (b > 0) printf "%.1fx", a / b; else printf "n/a" }')
  printf '| %s | %s | %s | %s | %s | %s | %s | %s | %s |\n' \
    "$name" "$(wl "$name")" "$pmin" "$pmed" "$cmin" "$cmed" "$ratio" "$rpen" "$rcpp"
done

cat <<'EOF'

## Before/after vs the M1 baseline (identical workloads, real loops vs call chains)

| Workload | M1 case (median ms) | M1 median | 2B case (median ms) | 2B median | Speedup (M1/2B) | M1 build (ms) | 2B build (ms) | Build speedup |
|---|---|---|---|---|---|---|---|---|
EOF
for pair in $PAIRS; do
  b="${pair%%:*}"; m="${pair##*:}"
  bmed=$(grep "^$b pen " "$RESULTS/timings-m2b.txt" | awk '{print $4}')
  bbuild=$(awk -v n="$b" '$1 == n { print $2 }' "$RESULTS/compile-pen-m2b.txt")
  mmed=$(m1_med "$m"); mbld=$(m1_build "$m")
  speed=$(awk -v a="$mmed" -v c="$bmed" 'BEGIN { if (c > 0 && a != "-") printf "%.1fx", a / c; else printf "n/a" }')
  bspeed=$(awk -v a="$mbld" -v c="$bbuild" 'BEGIN { if (c > 0 && a != "-") printf "%.0fx", a / c; else printf "n/a" }')
  printf '| %s | %s (%s) | %s | %s (%s) | %s | %s | %s | %s | %s |\n' \
    "$(wl "$b")" "$m" "$m" "$mmed" "$b" "$b" "$bmed" "$speed" "$mbld" "$bbuild" "$bspeed"
done

cat <<'EOF'

(M1 rows are read from bench/results/m1-baseline.md — the frozen
`bench/tools/pentest-m1` run — same machine, same pinning, same reps.
Compile-time speedup reflects the elimination of M1's superlinear clang
cliff: O(K+N) unrolled statements -> O(1) loop statements.)

## Compile time (.pen -> native, cold; `build` never caches)

| Case | .pen source bytes | Pentest build (ms) | C++ twin build (ms) |
|---|---|---|---|
EOF
for name in $CASE_ORDER; do
  pms=$(awk -v n="$name" '$1 == n { print $2 }' "$RESULTS/compile-pen-m2b.txt")
  cms=$(awk -v n="$name" '$1 == n { print $2 }' "$RESULTS/compile-cxx-m2b.txt" 2>/dev/null || echo "-")
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

emit | tee "$RESULTS/m2b-baseline.md"
echo ""
echo "wrote $RESULTS/m2b-baseline.md"
