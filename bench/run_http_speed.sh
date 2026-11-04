#!/usr/bin/env bash
# bench/run_http_speed.sh — run bench/http_speed.pen N>=30 times against the
# local lab, CPU-pinned, and report min/mean per key. Methodology per
# docs/performance/optimization.md §9 (interleaved min-of-N A/B, taskset -c 0,
# byte-compat key set across runs).
#
# Usage:  bash bench/run_http_speed.sh [N]     (default N=30)
set -u
cd "$(dirname "$0")/.."
N="${1:-30}"

LAB=examples/sqli_lab.py
BIN_DIR=bench/bin
BIN="$BIN_DIR/http_speed.bin"
mkdir -p "$BIN_DIR"

# build once
build/pentest build bench/http_speed.pen -o "$BIN" || exit 1

# start the lab
python3 "$LAB" >/tmp/sqli_lab_bench.log 2>&1 &
LAB_PID=$!
trap 'kill $LAB_PID 2>/dev/null' EXIT
sleep 1

OUT=/tmp/http_speed_runs.txt
: > "$OUT"
for i in $(seq 1 "$N"); do
  taskset -c 0 "$BIN" >> "$OUT" 2>/dev/null || echo "RUN $i FAILED" >> "$OUT"
done

echo "runs=$N pinned=taskset -c 0"
echo "--- per-key min / mean / max (ms unless noted) ---"
# aggregate: each run prints key=value lines; collect values per key
awk -F= '
  { v[$1] = (v[$1] "," $2); cnt[$1]++; sum[$1]+=$2; if(!(($1) in mn)||$2<mn[$1])mn[$1]=$2; if($2>mx[$1])mx[$1]=$2 }
  END {
    for (k in cnt) {
      n=split(v[k], a, ","); s=0; for(i=2;i<=n;i++) s+=a[i];
      printf "%-32s min=%-14s mean=%-14s max=%-14s (n=%d)\n", k, mn[k], s/(n-1), mx[k], cnt[k];
    }
  }' "$OUT" | sort

echo "--- raw first run ---"
awk 'NR==1,/^$/' "$OUT" | head -40
