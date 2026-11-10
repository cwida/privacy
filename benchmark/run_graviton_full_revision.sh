#!/usr/bin/env bash
# Run analytical experiments first, then DP experiments, then microbenchmarks.
set -euo pipefail
root="${1:?Usage: run_graviton_full_revision.sh ROOT NEW_RESULTS_DIR}"
results="${2:?A fresh result directory is required}"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test ! -e "$results" || { echo "Refusing existing results: $results" >&2; exit 1; }
mkdir -p "$results/repro" "$results/csv"
exec > >(tee -a "$results/repro/run.log") 2>&1
printf 'running\n' > "$results/repro/exit_code"
trap 'code=$?; printf "%s\n" "$code" > "$results/repro/exit_code"; date -u > "$results/repro/finished_utc.txt"' EXIT
date -u > "$results/repro/started_utc.txt"
cd "$repo"
git rev-parse HEAD > "$results/repro/source_commit.txt"
git status --short > "$results/repro/source_status.txt"
git submodule status > "$results/repro/submodules.txt"
{ uname -a; lscpu; free -h; lsblk -o NAME,SIZE,MODEL,MOUNTPOINTS; df -h "$root"; clang-22 --version; } > "$results/repro/environment.txt"
for unit in asa-main-prepare-20261110 asa-duckdb2-build-20261110 asa-data-prepare-20261110 asa-data-prepare-retry-20261110 asa-duckdb2-validated-20261110; do
  while systemctl is-active --quiet "$unit"; do sleep 10; done
done
test "$(cat "$root/repro/main_prepare/exit_code")" = 0
test "$(cat "$root/repro/duckdb2_build/exit_code")" = 0
test "$(cat "$root/repro/data_prepare/exit_code")" = 0
cp build/release/CMakeCache.txt "$results/repro/CMakeCache.txt"
python3 benchmark/prepare_revision_configs.py "$root" "$results/configs"
cp --reflink=auto "$root/data/tpch_sf30_base.db" "$root/data/tpch_sf30_main.db"
cp --reflink=auto "$root/data/tpch_sf30_base.db" "$root/data/tpch_sf30_duckdb2.db"
failures=0
run_stage() {
  local stage="$1"
  shift
  printf '%s\n' "$stage" > "$results/repro/stage"
  date -u
  if "$@" > "$results/repro/$stage.log" 2>&1; then
    printf '%s,0\n' "$stage" >> "$results/repro/stage_status.csv"
  else
    local code=$?
    printf '%s,%s\n' "$stage" "$code" >> "$results/repro/stage_status.csv"
    failures=$((failures+1))
    tail -25 "$results/repro/$stage.log"
  fi
}
run_stage tpch_main "$root/privacy/build/release/extension/privacy/as_tpch_benchmark" --config "$results/configs/as_tpch_sf30_main.json"
run_stage tpch_2 "$repo/build/release/extension/privacy/as_tpch_benchmark" --config "$results/configs/as_tpch_sf30.json"
run_stage tpch_m_sweep "$repo/build/release/extension/privacy/as_tpch_benchmark" --config "$results/configs/as_tpch_sf30_m_sweep.json"
run_stage clickbench "$repo/build/release/extension/privacy/as_clickbench_benchmark" --config "$results/configs/as_clickbench_graviton.json"
run_stage ssb env SSB_MEMORY_LIMIT=30GB bash benchmark/ssb/run.sh "$root/data/ssb_sf30" "$results/ssb_sf30" 16 5
run_stage meps bash benchmark/meps_brr/run.sh "$root/data/meps" "$results/meps" 16 5
run_stage tpcds_validation python3 benchmark/tpcds/run.py --config "$results/configs/as_tpcds_validate_sf1.json" --validate
run_stage tpcds python3 benchmark/tpcds/run.py --config "$results/configs/as_tpcds_sf30.json"
for config in "$results"/configs/dp_tpch*.json; do
  name="$(basename "$config" .json)"
  if [[ "$name" == *stability* ]]; then
    run_stage "$name" python3 benchmark/dp/run_tpch_sass_stability.py --config "$config" \
      --duckdb "$repo/build/release/duckdb" --out "$results/csv/$name.csv" \
      --summary-out "$results/csv/${name}_summary.csv" --strict
  else
    run_stage "$name" "$repo/build/release/extension/privacy/dp_benchmark_runner" --config "$config"
  fi
done
run_stage microbench bash benchmark/microbench/run_graviton.sh "$results/microbench"
printf 'complete\n' > "$results/repro/stage"
test "$failures" = 0
