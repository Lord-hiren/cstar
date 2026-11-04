#!/usr/bin/env bash
# bench/run.sh — Pentest M1 performance baseline runner.
#
# For every case in bench/cases/ (<name>.pen + <name>.cpp twin):
#   1. regenerates the .pen from bench/gen.py (deterministic),
#   2. builds the C++ twin:  clang++ -std=c++17 -O2  -> bench/bin/<name>.cpp.bin
#   3. builds the .pen with the PRESERVED M1 compiler (bench/tools/pentest-m1,
#      `build` mode, which NEVER caches — see docs/contracts/m1-semantics.md §9),
#      recording cold compile wall time -> bench/bin/<name>.pen.bin
#   4. asserts both binaries print byte-identical output (fails loudly),
#   5. runs each binary BENCH_REPS times (default 7), pinned to one core with
#      `taskset -c 0` when available, timing EXECUTION ONLY (the compile was
#      done in step 2/3), reporting min and median wall ms,
#   6. probes the `run`-mode content-hash cache (XDG_CACHE_HOME pointed into
#      bench/results/cache-probe, removed afterwards) on int_loop: cold vs warm.
#
# Usage:  bash bench/run.sh
# Env:    BENCH_REPS=7       timing repetitions
#         BENCH_REBUILD=1    reuse existing bench/bin binaries (compile times
#                            are then NOT re-measured for reused binaries)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH="$ROOT/bench"
PENTEST="$BENCH/tools/pentest-m1"
CASES="$BENCH/cases"
BIN="$BENCH/bin"
RESULTS="$BENCH/results"
GEN="$BENCH/gen.py"
REPS="${BENCH_REPS:-7}"
REBUILD="${BENCH_REBUILD:-0}"
CXX="${CXX:-clang++}"
CXXFLAGS="-std=c++17 -O2"   # exact flags for every C++ twin
PEN_CLANG_FLAGS="clang -std=c11 -O2 -o <out> gen.c pentest_rt.c -lm"

command -v "$CXX" >/dev/null || { echo "error: $CXX not found" >&2; exit 1; }
[ -x "$PENTEST" ] || { echo "error: $PENTEST missing — preserved M1 compiler required" >&2; exit 1; }

# Case order for the report. Workload strings must stay in sync with
# bench/gen.py PARAMS (K chain depth, N top-level chain invocations).
CASE_ORDER="startup int_loop float_loop call_chain call_overhead string_ops mixed"
wl() {
  case "$1" in
    startup)      echo 'process start + init + `print(0)` + teardown' ;;
    int_loop)     echo '12,976,128 int LCG steps (K=2048 x N=6336)' ;;
    float_loop)   echo '12,976,128 double steps (K=2048 x N=6336)' ;;
    call_chain)   echo '2,400,256 frames x (6 int ops + 3 cmp ops + 1 call) (K=1024 x N=2344)' ;;
    call_overhead) echo '12,976,128 pass-through calls (K=2048 x N=6336)' ;;
    string_ops)   echo '600,000 x 16-byte string appends (K=125 x N=6000, period-5 reset)' ;;
    mixed)        echo '500,000 int steps + 400,000 appends (K=125 x N=4000)' ;;
    *)            echo '' ;;
  esac
}

# Pin to one core when taskset is usable (some containers forbid it).
PIN=""
if command -v taskset >/dev/null && taskset -c 0 true 2>/dev/null; then
  PIN="taskset -c 0"
else
  echo "note: taskset -c 0 unavailable — timings are unpinned" >&2
fi

mkdir -p "$BIN" "$RESULTS"
rm -rf "$RESULTS/cache-probe"

echo "== Pentest M1 baseline: $($PENTEST --version) =="

# 1. regenerate .pen cases --------------------------------------------------
python3 "$GEN"

# 2. build C++ twins --------------------------------------------------------
: > "$RESULTS/compile-cxx.txt"
for cpp in "$CASES"/*.cpp; do
  name="$(basename "$cpp" .cpp)"
  out="$BIN/$name.cpp.bin"
  if [ "$REBUILD" = 1 ] || [ ! -x "$out" ] || [ "$cpp" -nt "$out" ]; then
    echo "cxx  $name"
    t0=$(date +%s%N)
    $CXX $CXXFLAGS -o "$out" "$cpp"
    t1=$(date +%s%N)
    echo "$name $(( (t1 - t0) / 1000000 ))" >> "$RESULTS/compile-cxx.txt"
  fi
done

# 3. build .pen programs (cold compile time; `build` never caches) ----------
: > "$RESULTS/compile-pen.txt"
for name in $CASE_ORDER; do
  pen="$CASES/$name.pen"
  out="$BIN/$name.pen.bin"
  if [ "$REBUILD" = 1 ] || [ ! -x "$out" ] || [ "$pen" -nt "$out" ]; then
    echo "pen  $name (cold compile)"
    t0=$(date +%s%N)
    "$PENTEST" build "$pen" -o "$out"
    t1=$(date +%s%N)
    echo "$name $(( (t1 - t0) / 1000000 ))" >> "$RESULTS/compile-pen.txt"
  else
    echo "pen  $name (reused existing binary — compile time not re-measured)"
    echo "$name -" >> "$RESULTS/compile-pen.txt"
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

# 5. runtime measurement ----------------------------------------------------
# time_ms <binary...> -> wall ms of one execution (3 decimals, newline-terminated)
time_ms() {
  local t0 t1
  t0=$(date +%s%N)
  "$@" > /dev/null
  t1=$(date +%s%N)
  awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.3f\n", (b - a) / 1e6 }'
}
# stats: reads samples (one per line) on stdin, REPS of them; prints "min median"
stats() {
  sort -n | awk -v n="$REPS" '
    { v[NR] = $1 }
    END {
      half = int(n / 2) + 1
      if (n % 2) m = v[half]; else m = (v[half] + v[half + 1]) / 2
      printf "%.3f %.3f", v[1], m
    }'
}
# maxrss_kb <binary...> -> child peak RSS in KB (python resource.getrusage)
maxrss_kb() {
  python3 -c 'import resource, subprocess, sys
subprocess.run(sys.argv[1:], stdout=subprocess.DEVNULL, check=True)
print(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)' "$@"
}

: > "$RESULTS/timings.txt"
: > "$RESULTS/rss.txt"
for name in $CASE_ORDER; do
  echo "run  $name x$REPS (pinned: ${PIN:-no})"
  for side in pen cpp; do
    samples=$(for _ in $(seq 1 "$REPS"); do
      if [ -n "$PIN" ]; then time_ms taskset -c 0 "$BIN/$name.$side.bin"; else time_ms "$BIN/$name.$side.bin"; fi
    done)
    read -r pmin pmed <<<"$(echo "$samples" | stats)"
    echo "$name $side $pmin $pmed" >> "$RESULTS/timings.txt"
  done
  if [ -n "$PIN" ]; then
    rpen=$(maxrss_kb taskset -c 0 "$BIN/$name.pen.bin")
    rcpp=$(maxrss_kb taskset -c 0 "$BIN/$name.cpp.bin")
  else
    rpen=$(maxrss_kb "$BIN/$name.pen.bin")
    rcpp=$(maxrss_kb "$BIN/$name.cpp.bin")
  fi
  echo "$name $rpen $rcpp" >> "$RESULTS/rss.txt"
done

# 6. run-mode content-hash cache probe (int_loop) ---------------------------
echo "== run-mode cache probe (int_loop) =="
export XDG_CACHE_HOME="$RESULTS/cache-probe"
rm -rf "$XDG_CACHE_HOME"; mkdir -p "$XDG_CACHE_HOME"
t0=$(date +%s%N); "$PENTEST" run "$CASES/int_loop.pen" > /dev/null; t1=$(date +%s%N)
cache_cold=$(( (t1 - t0) / 1000000 ))
t0=$(date +%s%N); "$PENTEST" run "$CASES/int_loop.pen" > /dev/null; t1=$(date +%s%N)
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

emit() {
cat <<EOF
# Pentest M1 performance baseline

- Date: $ts
- Machine: \`$machine\`
- CPU: $cpu ($cores logical cores), RAM: $ram
- Compiler (C++ twins): $clangv
- Compiler (Pentest backend, invoked internally by pentest-m1): $clangc
- Pentest compiler: $pentest_v (preserved copy: \`bench/tools/pentest-m1\`)
- C++ twin flags: \`$CXX $CXXFLAGS\`
- Pentest compile flags (fixed by the M1 CLI): \`$PEN_CLANG_FLAGS\`
- Timing pinning: $pin_note
- Repetitions: $REPS per binary; **min** and **median** wall-clock ms of
  program execution only (compile excluded: .pen programs are built ahead of
  time with \`pentest-m1 build\`, which never caches).

## Runtime (program execution only)

| Case | Workload | Pentest min (ms) | Pentest median (ms) | C++ min (ms) | C++ median (ms) | Median ratio | Peak RSS Pentest (KB) | Peak RSS C++ (KB) |
|---|---|---|---|---|---|---|---|---|
EOF
for name in $CASE_ORDER; do
  penline=$(grep "^$name pen " "$RESULTS/timings.txt")
  cppline=$(grep "^$name cpp " "$RESULTS/timings.txt")
  rssline=$(grep "^$name " "$RESULTS/rss.txt")
  pmin=$(echo "$penline" | awk '{print $3}'); pmed=$(echo "$penline" | awk '{print $4}')
  cmin=$(echo "$cppline" | awk '{print $3}'); cmed=$(echo "$cppline" | awk '{print $4}')
  rpen=$(echo "$rssline" | awk '{print $2}'); rcpp=$(echo "$rssline" | awk '{print $3}')
  ratio=$(awk -v a="$pmed" -v b="$cmed" 'BEGIN { if (b > 0) printf "%.1fx", a / b; else printf "n/a" }')
  printf '| %s | %s | %s | %s | %s | %s | %s | %s | %s |\n' \
    "$name" "$(wl "$name")" "$pmin" "$pmed" "$cmin" "$cmed" "$ratio" "$rpen" "$rcpp"
done
cat <<'EOF'

## Compile time (.pen -> native, cold; `pentest-m1 build` never caches)

| Case | .pen source bytes | Pentest build (ms) | C++ twin build (ms) |
|---|---|---|---|
EOF
for name in $CASE_ORDER; do
  pms=$(awk -v n="$name" '$1 == n { print $2 }' "$RESULTS/compile-pen.txt")
  cms=$(awk -v n="$name" '$1 == n { print $2 }' "$RESULTS/compile-cxx.txt" 2>/dev/null || echo "-")
  [ -n "$cms" ] || cms="-"
  bytes=$(wc -c < "$CASES/$name.pen")
  printf '| %s | %s | %s | %s |\n' "$name" "$bytes" "$pms" "$cms"
done
cat <<EOF

## \`run\`-mode content-hash cache probe (int_loop, fresh XDG_CACHE_HOME)

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

emit | tee "$RESULTS/m1-baseline.md"
echo ""
echo "wrote $RESULTS/m1-baseline.md"
