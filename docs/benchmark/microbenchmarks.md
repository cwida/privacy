# SIMD-AS Microbenchmark Suite

Performance benchmarks for the stochastic aggregates (`as_count`, `as_sum`, `as_avg`, `as_min`, `as_max`) under
several optimization configurations. Each configuration is a separate DuckDB binary.

## Quick start

Build with Clang; the paper uses Clang 22.1.2 (`clang-22` on Ubuntu). The build uses the compiler named by `CC` and
`CXX`.

```bash
sudo apt install make ninja-build cmake clang-22
export CC=clang-22 CXX=clang++-22

cd benchmark/microbench
./create_test_db.sh
./run_all.sh            # build all variants, then run all benchmarks
./run_all.sh -q         # quick test: smaller data
./run_all.sh -b         # build only
./run_all.sh -r count   # one suite: count, sum_avg or min_max
```

`build_variants.sh` (called by `run_all.sh`) rebuilds `build/release` once per variant, copies each `duckdb` binary
to `benchmark/microbench/binaries/` and leaves the last variant, exact SUM, in `build/release`. Rebuild the default
configuration (`rm -rf build/release && GEN=ninja make`) before running anything else from `build/release`.

## Build variants

The flags are C++ macros passed through `CXXFLAGS`, not CMake options.

| Variant | Flags | Description |
|---------|-------|-------------|
| `default` | (none) | All optimizations enabled |
| `nobuffering` | `-DPAC_NOBUFFERING` | No input buffering before a group's state is allocated |
| `nocascading` | `-DPAC_NOCASCADING` | No cascading counters: full-width counters from the start |
| `nosimd` | `-DPAC_NOCASCADING -DPAC_NOSIMD -fno-vectorize -fno-slp-vectorize` | SIMD-unfriendly kernels, auto-vectorization disabled |
| `noboundopt` | `-DPAC_NOBOUNDOPT` | No bound pruning (MIN/MAX only) |
| `signedsum` | `-DPAC_SIGNEDSUM` | No separate handling of negative values in SUM |
| `exactsum` | `-DPAC_EXACTSUM` | Exact cascading SUM instead of approximate SUM |

## Suites

- **`bench_count.sh`**: COUNT, ungrouped and with 10 to 10M groups, on 10M, 100M and 1000M rows.
- **`bench_sum_avg.sh`**: SUM and AVG over value domains of different widths, ungrouped and grouped, with
  sequential and scattered group keys.
- **`bench_min_max.sh`**: MIN and MAX over data types and value distributions (random, monotonically increasing,
  monotonically decreasing), which exercise bound pruning in its worst and best case.

`create_test_db.sh` generates all data with `range()`; random values are `hash(i) % domain`.

## Results and plots

Each suite writes one CSV named `<suite>_<timestamp>.csv` into `RESULTS_DIR` (default `benchmark/microbench/results/`).
The plot script reads one sub-directory per machine and uses the newest file of each suite in it, so write the
results of a machine into its own directory:

```bash
RESULTS_DIR=$PWD/results/graviton ./run_all.sh -r count
Rscript --vanilla plot_microbenchmark.R    # writes plots/*.png
```

The directories `graviton`, `granite-rapids`, `epyc` and `macbook` hold the measurements behind the paper's
figures (see `results/README.md`); a newer file in one of them replaces that machine's data in the plots.

CSV columns: `test` (benchmark case), `aggregate` (not in the COUNT file), `variant`, `rows_m` (rows in millions),
`groups`, `dtype` (data type or value domain; not in the COUNT file), `wall_sec` and `agg_sec` (median wall time and
median time inside the aggregation operator; `-1` if the configuration failed), and `wall_times` / `agg_times`
(all measured runs).

Environment variables: `WARMUP_RUNS` (default 1), `BENCH_RUNS` (default 3), `FORCE_REBUILD=1` to rebuild existing
binaries, `QUERY_SETUP` (below) and `RESULTS_DIR`.

## Query settings and state export

The paper runs pass the same session settings to every query through `QUERY_SETUP`, with one warmup and three
measured runs per cell:

```bash
export QUERY_SETUP="SET pac_mi=0.0; SET threads=4; SET max_memory='30G'; \
  SET max_temp_directory_size='90GB'; SET temp_directory='<local SSD path>';"
export WARMUP_RUNS=1 BENCH_RUNS=3
```

The microbenchmarks keep `priv_state_export=false`, the default in every build variant. With export off DuckDB cannot
spill the aggregate states, and some ablation cells fail once the temporary-storage limit is reached (`-1` in the
CSV): unbuffered SUM and MAX and non-cascading COUNT at 10M scattered groups, and non-cascading SUM at 10M sequential
groups. See the "Aggregate State Export" section of
[`repro.md`](../../repro.md).

## Machines

| Instance | CPU | RAM | Storage |
|----------|-----|-----|---------|
| `c8gd.4xlarge` | 16x Graviton4 | 32 GB | 950 GB instance SSD |
| `c8i.4xlarge` | 16x Granite Rapids | 32 GB | EBS io2 |
| `c8a.4xlarge` | 16x EPYC 9R45 | 32 GB | EBS io2 |

All run Ubuntu. On the Graviton instance, format and mount the instance SSD and point `temp_directory` in
`QUERY_SETUP` at it. A full run of the three suites takes about six hours per machine.

`binomial.sql` computes the distribution of set bits in the membership masks shown in the hash-distribution plot.

## SUM accuracy

`benchmark/microbench/approx_sum_stability.sql` compares ten value distributions against 64 independent sampled
sums. The approximate counters are divided by two before both errors and variances are computed, so all statistics
use the same sample-total units. Run the same setup for each binary variant:

```sh
benchmark/microbench/binaries/duckdb_default -batch <<'SQL'
SET threads=4;
SET memory_limit='30GB';
SET priv_rewrite=false;
SET privacy_seed=42;
SELECT setseed(0.42);
.read benchmark/microbench/approx_sum_stability.sql
SQL
```
