#!/bin/bash
# WIP rebuttal experiment. Run under nohup caffeinate -ims to survive terminal
# closure and prevent idle sleep. Forced sleep/lid closure still pauses the CPU.
set -u
cd /Users/ila/Code/privacy || exit 1
run_dir="$1"
db_path="${2:-$run_dir/tpch_sf30.db}"
# Refuse accidental repeated execution into the same output directory.
if [ -e "$run_dir/runtime.csv" ]; then
  echo "Output already exists: $run_dir/runtime.csv" >&2
  exit 1
fi
build/release/extension/privacy/as_tpch_benchmark \
  30 "$db_path" benchmark "$run_dir/runtime.csv" \
  --threads=8 --as-ms=64 --skip-naive --run-simple-hash \
  --run-grouped-baselines --verdictdb-utility-trials=100
run_exit=$?
printf 'benchmark_exit=%s\n' "$run_exit" > "$run_dir/status.txt"
if [ "$run_exit" -eq 0 ] && rg -q ',-1$' "$run_dir/runtime.csv"; then
  printf 'failed_timing_rows=true\n' >> "$run_dir/status.txt"
  exit 1
fi
exit "$run_exit"
