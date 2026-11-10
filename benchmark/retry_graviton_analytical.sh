#!/usr/bin/env bash
# Retry the two setup failures after DP, before the queued microbenchmarks.
set -euo pipefail
root="${1:?Usage: retry_graviton_analytical.sh ROOT RESULTS_DIR}"
results="${2:?RESULTS_DIR is required}"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec > >(tee -a "$results/repro/analytical_retry.log") 2>&1
printf 'running\n' > "$results/repro/analytical_retry_exit_code"
trap 'printf "%s\n" "$?" > "$results/repro/analytical_retry_exit_code"' EXIT
while [ "$(cat "$results/repro/stage")" != microbench ]; do
  systemctl is-active --quiet asa-full-revision-retry-20261110.service || exit 1
  sleep 10
done
cd "$repo"
git rev-parse HEAD > "$results/repro/analytical_retry_source_commit.txt"
failures=0
for workload in ssb meps; do
  output="$results/meps"
  data="$root/data/meps"
  script=benchmark/meps_brr/run.sh
  if [ "$workload" = ssb ]; then
    output="$results/ssb_sf30"
    data="$root/data/ssb_sf30"
    script=benchmark/ssb/run.sh
  fi
  if [ -e "$output" ]; then
    test ! -e "${output}_failed_setup"
    mv "$output" "${output}_failed_setup"
  fi
  date -u
  if SSB_MEMORY_LIMIT=30GB bash "$script" \
      "$data" "$output" 16 5 > "$results/repro/${workload}_retry.log" 2>&1; then
    printf '%s_retry,0\n' "$workload" >> "$results/repro/stage_status.csv"
  else
    code=$?
    printf '%s_retry,%s\n' "$workload" "$code" >> "$results/repro/stage_status.csv"
    failures=$((failures+1))
    tail -25 "$results/repro/${workload}_retry.log"
  fi
done
test "$failures" = 0
