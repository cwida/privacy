#!/usr/bin/env bash
# Build variants without deleting the analytical build or its measured binaries.
set -euo pipefail
results="${1:?Usage: run_graviton.sh NEW_RESULTS_DIR}"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
test ! -e "$results" || { echo "Refusing existing results: $results" >&2; exit 1; }
mkdir -p "$results/builds" "$repo/benchmark/microbench/binaries"
variants=(default noboundopt nobuffering signedsum exactsum nocascading nosimd)
flags=('' '-DPAC_NOBOUNDOPT' '-DPAC_NOBUFFERING' '-DPAC_SIGNEDSUM' '-DPAC_EXACTSUM' '-DPAC_NOCASCADING' '-DPAC_NOCASCADING -DPAC_NOSIMD -fno-vectorize -fno-slp-vectorize')
for i in "${!variants[@]}"; do
  name="${variants[$i]}"
  cmake -S "$repo/duckdb" -B "$repo/build/graviton_micro" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-22 -DCMAKE_CXX_COMPILER=clang++-22 \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DDUCKDB_EXTENSION_CONFIGS="$repo/extension_config.cmake" -DBUILD_UNITTESTS=OFF \
    -DPAC_NOAPPROX=OFF -DCMAKE_CXX_FLAGS="${flags[$i]}" \
    > "$results/builds/$name.configure.log" 2>&1
  cmake --build "$repo/build/graviton_micro" --target duckdb -j 4 > "$results/builds/$name.build.log" 2>&1
  cp "$repo/build/graviton_micro/duckdb" "$repo/benchmark/microbench/binaries/duckdb_$name"
  cp "$repo/build/graviton_micro/CMakeCache.txt" "$results/builds/$name.CMakeCache.txt"
done
bash "$repo/benchmark/microbench/create_test_db.sh" > "$results/create_data.log" 2>&1
export RESULTS_DIR="$results/csv"
export QUERY_SETUP="SET pac_mi=0.0; SET threads=4; SET max_memory='30G'; SET max_temp_directory_size='90GB'; SET temp_directory='$repo/benchmark/microbench/tmp';"
export WARMUP_RUNS=1 BENCH_RUNS=3
for benchmark in count sum_avg min_max; do
  bash "$repo/benchmark/microbench/bench_$benchmark.sh" > "$results/$benchmark.log" 2>&1
done
for variant in default signedsum exactsum; do
  python3 "$repo/benchmark/microbench/run_accuracy.py" \
    "$repo/benchmark/microbench/binaries/duckdb_$variant" "$results/csv/accuracy_$variant.csv" \
    > "$results/accuracy_$variant.log" 2>&1
done
