#!/usr/bin/env bash
# Run the revision workloads sequentially on a prepared Graviton machine.
set -euo pipefail
root="${1:?Usage: bash benchmark/run_graviton_revision.sh ROOT [THREADS] [NEW_RESULTS_DIR]}"
threads="${2:-16}"
results="${3:-$root/results}"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$results/repro" "$root/data"
exec > >(tee -a "$results/repro/run.log") 2>&1
printf 'running\n' > "$results/repro/exit_code"
trap 'code=$?; printf "%s\n" "$code" > "$results/repro/exit_code"; date -u > "$results/repro/finished_utc.txt"' EXIT
date -u > "$results/repro/started_utc.txt"
{
  uname -a
  cat /etc/os-release
  lscpu
  free -h
  lsblk -o NAME,SIZE,TYPE,MOUNTPOINTS,MODEL
  df -h "$root"
  clang-22 --version
  cmake --version
} > "$results/repro/environment.txt"
dpkg-query -W > "$results/repro/packages.tsv"
cd "$repo"
# Pin the embedded DuckDB version even when using a source archive without .git.
cmake -S duckdb -B build/release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-22 \
  -DCMAKE_CXX_COMPILER=clang++-22 \
  -DDUCKDB_EXTENSION_CONFIGS="$repo/extension_config.cmake" \
  -DOVERRIDE_GIT_DESCRIBE=v1.5.4-0-g08e34c447b \
  -DBUILD_UNITTESTS=OFF -DPAC_NOAPPROX=OFF
cmake --build build/release --target as_ssb_benchmark meps_brr_poc duckdb privacy_loadable_extension -j 4
build/release/duckdb -c 'SELECT version();' > "$results/repro/duckdb_version.txt"
if [ ! -d "$root/ssb-dbgen" ]; then
  git clone https://github.com/electrum/ssb-dbgen.git "$root/ssb-dbgen"
  git -C "$root/ssb-dbgen" checkout 219403ad7d1dd32ae1f97b5553abf92129fccd7f
  git -C "$root/ssb-dbgen" apply "$repo/benchmark/ssb/dbgen-macos.patch"
fi
# The safety fixes are platform-independent. GNU feature macros expose
# O_LARGEFILE for the legacy generator on current glibc.
make -C "$root/ssb-dbgen" dbgen CC=clang-22 \
  CFLAGS='-O2 -std=gnu89 -include unistd.h -D_GNU_SOURCE -DDBNAME=\"dss\" -DLINUX -DDB2 -DSSBM'
if [ ! -f "$root/data/ssb_sf30/generation_complete" ]; then
  bash benchmark/ssb/prepare.sh "$root/ssb-dbgen" "$root/data/ssb_sf30" 30
fi
SSB_MEMORY_LIMIT=30GB bash benchmark/ssb/run.sh \
  "$root/data/ssb_sf30" "$results/ssb_sf30" "$threads" 5
python3 -m venv "$root/prepare-env"
"$root/prepare-env/bin/pip" install pandas
"$root/prepare-env/bin/pip" freeze > "$results/repro/python_requirements.txt"
for workload in damico2006 ahrq2016; do
  if [ ! -f "$root/data/meps/$workload/manifest.json" ]; then
    "$root/prepare-env/bin/python" benchmark/meps_brr/prepare.py \
      "$root/data/meps/$workload" --workload "$workload"
  fi
done
# The wrapper adds run 0 as warmup before five measured executions.
bash benchmark/meps_brr/run.sh "$root/data/meps" "$results/meps" "$threads" 5
find "$results" -name '*_timings.csv' -o -name '*_coverage.csv'
