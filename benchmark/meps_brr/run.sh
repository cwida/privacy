#!/usr/bin/env bash
# All performance measurements are made by the C++ runner; no R dependency.
set -euo pipefail
if [ "$#" -lt 2 ] || [ "$#" -gt 4 ]; then
  echo "Usage: bash benchmark/meps_brr/run.sh DATA_DIR NEW_OUTPUT_DIR [THREADS_CSV] [REPETITIONS]" >&2
  exit 2
fi
data_dir="$1"
output_dir="$2"
threads_csv="${3:-1,2,4,8}"
repetitions="${4:-5}"
if [ -e "$output_dir" ]; then
  echo "Refusing to overwrite output directory: $output_dir" >&2
  exit 1
fi
mkdir -p "$output_dir"
IFS=',' read -ra thread_counts <<< "$threads_csv"
for threads in "${thread_counts[@]}"; do
  prefix="$output_dir/threads_$threads"
  build/release/extension/privacy/meps_brr_poc "$data_dir" "$prefix.db" \
    benchmark/meps_brr/queries "$prefix" "$threads" "$repetitions" > "$prefix.log" 2>&1
done
