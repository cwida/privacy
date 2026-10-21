#!/usr/bin/env bash
# WIP: compare approximate and exact SUM on identical SSB data and world masks.
set -euo pipefail
if [ "$#" -lt 2 ] || [ "$#" -gt 4 ]; then
  echo 'Usage: bash benchmark/ssb/run.sh DATA_DIR NEW_OUTPUT_DIR [THREADS_CSV] [REPETITIONS]' >&2
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
if [ ! -f "$data_dir/generation_complete" ]; then
  echo "Missing generation_complete: $data_dir" >&2
  exit 1
fi
if [ -e "$output_dir" ]; then
  echo "Refusing to overwrite output directory: $output_dir" >&2
  exit 1
fi
mkdir -p "$output_dir"
output_dir="$(cd "$output_dir" && pwd)"
build_dir="$repo_dir/build/release"
runner="$build_dir/extension/privacy/as_ssb_benchmark"
restore_needed=1
build_mode() {
  cmake -S "$repo_dir/duckdb" -B "$build_dir" -DPAC_NOAPPROX="$1"
  cmake --build "$build_dir" --target as_ssb_benchmark duckdb privacy_loadable_extension -j 4
}
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
  if [ "$kernel" = exact ]; then
    noapprox=ON
  fi
  restore_needed=1
  echo "Building $kernel SUM (PAC_NOAPPROX=$noapprox)." >&2
  if ! build_mode "$noapprox" > "$mode_dir/build.log" 2>&1; then
    tail -30 "$mode_dir/build.log" >&2
    exit 1
  fi
  if [ "$kernel" = approximate ]; then
    restore_needed=0
  fi
  cp "$build_dir/CMakeCache.txt" "$mode_dir/CMakeCache.txt"
  for threads in "${thread_counts[@]}"; do
    prefix="$mode_dir/threads_$threads"
    runner_args=("$data_dir" "$prefix.duckdb" "$repo_dir/benchmark/ssb" "$prefix" "$threads" "$repetitions")
    if [ "$kernel" = exact ]; then
      runner_args+=(--as-only)
    fi
    echo "Running $kernel SUM with $threads threads." >&2
    if ! "$runner" "${runner_args[@]}" > "$prefix.log" 2>&1; then
      tail -30 "$prefix.log" >&2
      exit 1
    fi
  done
done
