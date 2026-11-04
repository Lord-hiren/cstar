#!/usr/bin/env bash
# bench/run-varq.sh — cross-scope access (`variable.`) performance lane.
#
# Builds the Stage 2D pair bench/cases/varq/cross_scope.{pen,cpp}, asserts
# byte-identical output, then times EXECUTION ONLY (min/median over N reps,
# pinned to one core when available) plus the cold compile of the .pen side.
#
# The acceptance target (docs/reference/cross-scope-access.md §6):
#   * the Pentest binary runs the workload in < 20 ms, or
#   * it is at least as fast as the C twin.
# Both are reported; the script exits nonzero if BOTH miss.
#
# Usage:  bash bench/run-varq.sh
# Env:    BENCH_REPS=9       timing repetitions
#         BENCH_REBUILD=1    reuse an existing bench/bin/varq binary
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH="$ROOT/bench"
PENTEST="$ROOT/build/pentest"
CASES="$BENCH/cases/varq"
BIN="$BENCH/bin/varq"
RESULTS="$BENCH/results"
REPS="${BENCH_REPS:-9}"
REBUILD="${BENCH_REBUILD:-0}"
CXX="${CXX:-clang++}"
CXXFLAGS="-std=c++17 -O2"

command -v "$CXX" >/dev/null || { echo "error: $CXX not found" >&2; exit 1; }
[ -x "$PENTEST" ] || { echo "error: $PENTEST missing — build it with make" >&2; exit 1; }

PIN=""
if command -v taskset >/dev/null && taskset -c 0 true 2>/dev/null; then
  PIN="taskset -c 0"
else
  echo "note: taskset 0 unavailable — timings are unpinned" >&2
fi

mkdir -p "$BIN" "$RESULTS"
NAME=cross_scope

echo "== Pentest cross-scope baseline: $($PENTEST --version) =="

# 1. build both sides (cold compile time for the .pen side) -----------------
echo "cxx  $NAME"
$CXX $CXXFLAGS -o "$BIN/$NAME.cpp.bin" "$CASES/$NAME.cpp"
if [ "$REBUILD" = 1 ] || [ ! -x "$BIN/$NAME.pen.bin" ] || \
   [ "$CASES/$NAME.pen" -nt "$BIN/$NAME.pen.bin" ]; then
  echo "pen  $NAME (cold compile)"
  t0=$(date +%s%N)
  "$PENTEST" build "$CASES/$NAME.pen" -o "$BIN/$NAME.pen.bin"
  t1=$(date +%s%N)
  compile_ms=$(( (t1 - t0) / 1000000 ))
else
  echo "pen  $NAME (reused binary — compile time not re-measured)"
  compile_ms=0
fi

# 2. output equality --------------------------------------------------------
"$BIN/$NAME.cpp.bin" > "$BIN/$NAME.cpp.out"
"$BIN/$NAME.pen.bin" > "$BIN/$NAME.pen.out"
if ! cmp -s "$BIN/$NAME.cpp.out" "$BIN/$NAME.pen.out"; then
  echo "FATAL: output mismatch for $NAME" >&2
  diff <(od -c "$BIN/$NAME.cpp.out") <(od -c "$BIN/$NAME.pen.out") >&2 || true
  exit 1
fi
echo "  $NAME: outputs match ($(wc -c < "$BIN/$NAME.cpp.out") bytes)"

# 3. execution timing (min / median of REPS, exec only) ---------------------
time_ms() {  # $1 = label, rest = command; prints "<min> <median>"
  local label="$1"; shift
  local samples=()
  for _ in $(seq "$REPS"); do
    local t0 t1
    t0=$(date +%s%N)
    "$@" > /dev/null 2>&1
    t1=$(date +%s%N)
    samples+=("$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.3f", (b - a) / 1e6 }')")
  done
  printf '%s\n' "${samples[@]}" | sort -n > "$BIN/$label.samples"
  local min med
  min=$(head -n 1 "$BIN/$label.samples")
  med=$(awk -v n="$REPS" 'NR == int((n + 1) / 2) { print; exit }' \
        "$BIN/$label.samples")
  echo "$min $med"
}

echo "== timing ($REPS reps, exec only${PIN:+, pinned}) =="
read -r pen_min pen_med <<<"$(time_ms pen $PIN "$BIN/$NAME.pen.bin")"
read -r cxx_min cxx_med <<<"$(time_ms cxx $PIN "$BIN/$NAME.cpp.bin")"

# 4. verdict ----------------------------------------------------------------
under20=$(awk -v m="$pen_min" 'BEGIN { print (m < 20) ? "yes" : "no" }')
beats_c=$(awk -v p="$pen_min" -v c="$cxx_min" \
          'BEGIN { print (p <= c) ? "yes" : "no" }')
if [ "$under20" = yes ] || [ "$beats_c" = yes ]; then verdict=PASS; else verdict=FAIL; fi

report="$RESULTS/varq-baseline.md"
{
  echo "# Cross-scope access (\`variable.\`) performance baseline"
  echo
  echo "Pair: \`bench/cases/varq/cross_scope.pen\` vs its C twin \`cross_scope.cpp\`"
  echo "- same seed, same arithmetic, byte-identical output, exec-only timing,"
  echo "  $REPS reps${PIN:+, pinned to one core}."
  echo
  echo "| metric | pentest (\`variable.\` access) | C twin |"
  echo "|---|---|---|"
  echo "| min wall ms | $pen_min | $cxx_min |"
  echo "| median wall ms | $pen_med | $cxx_med |"
  echo "| cold compile ms | $compile_ms | n/a (clang direct) |"
  echo
  echo "Target (\`docs/reference/cross-scope-access.md\` §6): pentest < 20 ms OR >= C.  "
  echo "under 20 ms: **$under20** — not slower than C: **$beats_c** → **$verdict**"
} > "$report"

echo "min  pen=${pen_min}ms  c=${cxx_min}ms"
echo "med  pen=${pen_med}ms  c=${cxx_med}ms"
echo "cold compile: ${compile_ms}ms"
echo "target: under20=$under20 beatsC=$beats_c → $verdict"
cat "$report"
[ "$verdict" = PASS ]