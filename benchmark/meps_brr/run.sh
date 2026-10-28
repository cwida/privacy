#!/usr/bin/env bash
# WIP: explicit SQL workloads, compared under the same exact/approx builds as SSB.
set -euo pipefail
if [ "$#" -lt 2 ] || [ "$#" -gt 4 ]; then
  echo 'Usage: bash benchmark/meps_brr/run.sh DATA_ROOT NEW_OUTPUT_DIR [THREADS_CSV] [REPETITIONS]' >&2
  exit 2
fi
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
data_dir="$(cd "$1" && pwd)"
output_dir="$2"
threads_csv="${3:-1,8}"
repetitions="${4:-5}"
if [[ ! "$threads_csv" =~ ^[1-9][0-9]*(,[1-9][0-9]*)*$ ]] || [[ ! "$repetitions" =~ ^[1-9][0-9]*$ ]]; then
  echo 'Thread counts and repetitions must be positive integers.' >&2
  exit 2
fi
for workload in damico2006 ahrq2016; do
  for table in persons brr; do
    test -f "$data_dir/$workload/$table.csv" || { echo "Missing $workload/$table.csv" >&2; exit 1; }
  done
done
test -f "$data_dir/ahrq2016/events.csv" || { echo 'Missing ahrq2016/events.csv' >&2; exit 1; }
if [ -e "$output_dir" ]; then
  echo "Refusing to overwrite output directory: $output_dir" >&2
  exit 1
fi
mkdir -p "$output_dir"
output_dir="$(cd "$output_dir" && pwd)"
build_dir="$repo_dir/build/release"
build_mode() {
  cmake -S "$repo_dir/duckdb" -B "$build_dir" -DPAC_NOAPPROX="$1"
  cmake --build "$build_dir" --target meps_brr_poc -j 4
}
restore_needed=1
cleanup() {
  local result=$?
  trap - EXIT
  if [ "$restore_needed" -eq 1 ]; then
    echo 'Restoring default approximate SUM build.' >&2
    if ! build_mode OFF > "$output_dir/restore_default.log" 2>&1; then
      tail -30 "$output_dir/restore_default.log" >&2
      result=1
    fi
  fi
  exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
IFS=',' read -ra thread_counts <<< "$threads_csv"
for kernel in approximate exact; do
  mode_dir="$output_dir/$kernel"
  mkdir "$mode_dir"
  noapprox=OFF
  if [ "$kernel" = exact ]; then noapprox=ON; fi
  restore_needed=1
  echo "Building $kernel SUM." >&2
  if ! build_mode "$noapprox" > "$mode_dir/build.log" 2>&1; then
    tail -30 "$mode_dir/build.log" >&2
    exit 1
  fi
  if [ "$kernel" = approximate ]; then restore_needed=0; fi
  cp "$build_dir/CMakeCache.txt" "$mode_dir/CMakeCache.txt"
  for workload in damico2006 ahrq2016; do
    mkdir "$mode_dir/$workload"
    for threads in "${thread_counts[@]}"; do
      prefix="$mode_dir/$workload/threads_$threads"
      echo "Running $workload / $kernel / $threads threads." >&2
      if ! "$build_dir/extension/privacy/meps_brr_poc" "$data_dir/$workload" "$prefix.db" \
          "$repo_dir/benchmark/meps_brr" "$prefix" "$threads" "$((repetitions + 1))" "$workload" \
          > "$prefix.log" 2>&1; then
        tail -30 "$prefix.log" >&2
        exit 1
      fi
    done
  done
done
