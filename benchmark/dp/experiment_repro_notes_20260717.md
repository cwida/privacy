# DP-SAA Experiment Repro Notes, 2026-07-17

This note records the temporary benchmark setup we used while finalizing the
paper figures. Keep it with the benchmark scripts when building reproducibility
automation next week.

## Runtime Workaround

The TPC-H DP runtime benchmark must be run as repeated one-run processes, then
combined afterwards.

The direct `runs=3` setup produced contaminated DP-SAA timings. In one observed
run, Q01 DP-SAA took about 3.4s in run 0 and then about 42s in run 1 in the same
process. Running the same query with `EXPLAIN ANALYZE`, and running the runner
as a single-run process, did not reproduce the 40s timing. The plans for
DP-bounded and DP-SAA were also the same up to the final aggregate.

Working hypothesis: SAA diagnostic/materialization work in the runner pollutes
the same DuckDB process enough to make later timed rows unusable. Until this is
fixed properly, the paper runtime CSV should use three independent one-run
processes.

EC2 command state for the clean workaround:

- Host: `ec2-3-15-173-194.us-east-2.compute.amazonaws.com`
- Repo: `/home/ubuntu/privacy`
- Branch: `dp-sass-variable-m`
- Tmux session: `tpch_runtime_1x_3clean`
- Remote script: `/tmp/run_tpch_runtime_3x1run.sh`
- Per-run configs:
  - `/tmp/tpch_sf30_runtime_perfect_m64_m512_clean_run0.json`
  - `/tmp/tpch_sf30_runtime_perfect_m64_m512_clean_run1.json`
  - `/tmp/tpch_sf30_runtime_perfect_m64_m512_clean_run2.json`
- Per-run CSVs:
  - `/tmp/tpch_sf30_runtime_perfect_m64_m512_clean_run0.csv`
  - `/tmp/tpch_sf30_runtime_perfect_m64_m512_clean_run1.csv`
  - `/tmp/tpch_sf30_runtime_perfect_m64_m512_clean_run2.csv`
- Combined CSV:
  - `/tmp/tpch_sf30_runtime_perfect_m64_m512_3run_clean.csv`
- Combined log:
  - `/tmp/tpch_sf30_runtime_perfect_m64_m512_3run_clean.log`

Local single-run reference files:

- `benchmark/dp/tpch_sf30_runtime_perfect_m64_m512_1run_20260717.csv`
- `benchmark/dp/tpch_sf30_runtime_perfect_m64_m512_1run_20260717.log`

Local clean 3-run files copied back from EC2:

- `benchmark/dp/tpch_sf30_runtime_perfect_m64_m512_3run_clean_20260717.csv`
- `benchmark/dp/tpch_sf30_runtime_perfect_m64_m512_3run_clean_20260717.log`
- `benchmark/dp/tpch_sf30_runtime_perfect_m64_m512_3run_clean_summary_20260717.csv`

Do not use the old 3-run-in-one-process runtime CSV for the paper runtime
claim:

- `benchmark/dp/tpch_sf30_all5_paper_utility_central75_delta1overN_m64_m128_m256_m512_allbounds_3run_results.csv`

That file is still useful for checking utility columns, but its runtime columns
are not reliable for the DP-SAA runtime figure.

## Main Utility Sweep

Config:

- `benchmark/dp/configs/tpch_sf30_all5_paper_utility_central75_delta1overN_m64_m128_m256_m512_allbounds_3run.json`

Result/log:

- `benchmark/dp/tpch_sf30_all5_paper_utility_central75_delta1overN_m64_m128_m256_m512_allbounds_3run_results.csv`
- `benchmark/dp/tpch_sf30_all5_paper_utility_central75_delta1overN_m64_m128_m256_m512_allbounds_3run.log`

Setup:

- Dataset: TPC-H SF30 on Graviton, `tpch_sf30_graviton.db`
- Queries: Q01, Q05, Q06, Q14, Q19
- Runs: 3
- Threads: 16
- Epsilon: `1.0`
- Delta: `2.2222222222222222e-7`, i.e. about `1 / N_PU`
- Bound multipliers: `0.01, 0.1, 1, 10, 100`
- Mechanisms: `dp_standard`, `dp_elastic`, `dp_sass`
- DP-SAA release: `average`
- DP-SAA rescaling: `false`
- DP-SAA m values: `64, 128, 256, 512`
- DP-SAA output-domain bounds: central 75 percent of the non-private lane
  outputs, per aggregate and per `m`
- DP-bounded / DP-elastic contribution bounds: oracle-like per-PU bounds, with
  the same multiplier applied

Important corrected bounds:

- Q01 `c_u = 4`
- Q01 explicit SUM bounds use a list:
  `50000,105000000,105000000,113400000`
- Q01 AVG public domains are per AVG expression, not shared across all AVG
  columns

Bound verification helper:

- `benchmark/dp/verify_tpch_saa_bounds.py`

Recent validation output:

- `benchmark/dp/tmp/tpch_saa_bound_check_latest/`

Paired SAA output-domain multipliers scale interval width around the fixed
oracle midpoint. For base domain `[L,U]` and multiplier `k`, the runner uses
`[c-kh,c+kh]`, where `c=(L+U)/2` and `h=(U-L)/2`. Never multiply the lower and
upper endpoints independently: for positive SUM/COUNT domains that shifts the
whole interval and produces artificial clipping error. The `1x` results from
the earlier run are valid; non-`1x` SAA rows produced before this correction
must not be used.

## Q01 AVG Utility Experiment

Primary plot:

- `benchmark/dp/q01_avg_compact_grid_paper.png`
- `benchmark/dp/q01_avg_compact_grid_paper_summary.csv`

Related inputs/results:

- `benchmark/dp/tmp/q01_avg_cols_3run_20260716_154716.json`
- `benchmark/dp/tmp/q01_avg_groups_3run_20260716_154716.json`
- `benchmark/dp/tmp/q01_avg_cols_central75_fixed_3run_20260716_154716_results.csv`
- `benchmark/dp/tmp/q01_avg_groups_central75_fixed_3run_20260716_154716_results.csv`
- `benchmark/dp/tmp/q01_avg_lane_outputs_m64_m512.csv`
- `benchmark/dp/tmp/q01_avg_lane_outputs_m64_m512_summary.csv`
- `benchmark/dp/tmp/q01_avg_subsample_outputs_with_bounds.csv`

Setup:

- Dataset: TPC-H SF30
- Query family: Q01 AVG-only variants
- AVG columns: `l_quantity`, `l_extendedprice`, `l_discount`
- Grouping variants: no group by, group by `l_linestatus`, group by
  `l_returnflag`, group by both
- Epsilon: `1.0`
- Delta: `1e-6`
- DP-SAA release: `average`
- DP-SAA rescaling: `false`
- DP-SAA m values: `64, 512`
- Bounds: oracle-like central 75 percent SAA output domains, checked against
  printed lane outputs

The latest utility review focused on making sure the AVG bounds are
per-column and match the non-private lane outputs. Keep the lane-output CSVs
above; they are the audit trail for those bounds.

## Q01 Stability Experiment

Configs:

- `benchmark/dp/configs/tpch_sf30_q01_avg_sum_count_stability_m64_m512_1run.json`
- `benchmark/dp/configs/tpch_sf30_q01_sum_count_stability_rescale_m64_m512_1run.json`

Plot/results:

- `benchmark/dp/q01_stability_m64_m512_rescaled_sumcount_paper.png`
- `benchmark/dp/q01_stability_m64_m512_rescaled_sumcount_paper_summary.csv`
- `benchmark/dp/q01_sum_count_rescale_stability_comparison.csv`

Setup:

- Dataset: TPC-H SF30
- Queries: Q01 AVG variants plus SUM/COUNT variants
- DP-SAA release: `average`
- m values: `64, 512`
- Stability metric: for each output cell, compute
  `stddev(lane_outputs) / abs(mean(lane_outputs))`; report the median over
  cells for the query
- AVG outputs are not rescaled
- SUM/COUNT stability is plotted after rescaling, because that is the full-data
  estimator variant we discussed

## AS TPC-H Variable-m Runtime

Plot/results:

- `benchmark/tpch/tpch_as_m_sweep_slowdown_paper.png`
- `benchmark/tpch/tpch_as_m_sweep_slowdown_paper_summary.csv`
- `benchmark/tpch/as_tpch_sf30_graviton_fullsweep_recheck_run1_20260716_101834.csv`
- `benchmark/tpch/as_tpch_sf30_graviton_fullsweep_recheck_run2_20260716_102254.csv`
- `benchmark/tpch/as_tpch_sf30_graviton_fullsweep_recheck_run3_20260716_102712.csv`

Setup:

- Dataset: TPC-H SF30 on Graviton
- Workload: handwritten AS TPC-H queries
- m values: `64, 128, 256, 512`
- Runs: 3
- The `m=64` path uses the original SIMD mask path
- The `m > 64` path uses the variable-m sample aggregate path
- Sampling key: customer privacy unit key, consistent with the DP benchmark

Main observed summary:

- `m=512` remains much faster than the naive AS baseline on Q01
- Mean slowdown increases with `m`, which is the performance/utility tradeoff
  we describe in the paper

## ClickBench AS Variable-m Runtime

Result/log:

- `benchmark/clickbench/as_clickbench_m64_m512_20260713_1807_results.csv`
- `benchmark/clickbench/as_clickbench_m64_m512_20260713_1807.log`
- `benchmark/clickbench/as_clickbench_m64_m512_25gbtmp_20260713_175217_results.csv`
- `benchmark/clickbench/as_clickbench_m64_m512_25gbtmp_20260713_175217.log`

Plot:

- `benchmark/clickbench/clickbench_as_m_sweep_slowdown_paper.png`

Notes:

- The 25GB temporary-disk run was a storage/OOM check, not a different paper
  setup.
- Non-aggregate ClickBench queries were skipped for AS.

## Target/noise Metric Scripts

### One utility experiment, not two manual runs

The full-answer utility pass and the common-SAA-target pass are logically one
experiment. The current use of `dp_benchmark_runner` followed by
`dp_target_metric_runner` is a temporary implementation detail and must not
become the public reproducibility procedure.

The final reproducibility entry point should accept one config and produce one
merged result file containing full-answer error, common-target error, sampling
error, and noise diagnostics. Both metric paths must use the same dataset,
query, mechanism, bounds, epsilon, delta, `m`, rescaling mode, run index, and
seed. Ideally, the query and its non-private full/SAA reference tables are
materialized once and reused for both comparisons. If the two executables are
retained internally, a single wrapper must invoke and merge them atomically,
joining on all experiment keys and failing on missing or duplicate rows.

For the 2026-07-17 rescaled plot refresh, the two executables were invoked
separately only because this unified entry point does not exist yet. Do not
copy that manual split into the artifact instructions.

Keep these files; they are not disposable outputs.

- `benchmark/dp/dp_target_metric_runner.cpp`
- `benchmark/dp/fill_tpch_target_metrics.py`
- `benchmark/dp/run_tpch_jcch_sass_stability.py`
- `benchmark/dp/plot_q01_avg_compact_grid.R`
- `benchmark/dp/plot_q01_stability.R`
- `benchmark/dp/plot_q01_sum_count_rescale.R`
- `benchmark/tpch/plot_tpch_as_m_sweep.R`

Target/noise CSVs used during that review:

- `benchmark/dp/tpch_target_metrics_delta1e-6.csv`
- `benchmark/dp/tpch_target_metrics_delta1e-6_saaavg_m64.csv`
- `benchmark/dp/tpch_target_metrics_delta1e-6_saaavg_m64_plusdpnoise.csv`
- `benchmark/dp/tpch_jcch_sf30_all5_norescale_central75_m64_delta1e-6_saaavg_target_plot_input.csv`
- `benchmark/dp/tpch_jcch_sf30_all5_norescale_central75_m64_delta1e-6_saaavg_plusdpnoise_plot_input.csv`

## Files To Preserve

Do not delete or overwrite these without first copying them elsewhere:

- Any file under `benchmark/dp/configs/` whose name contains `central75`,
  `q01`, `stability`, `delta`, or `support`
- `benchmark/dp/tmp/q01_avg_*.json`
- `benchmark/dp/tmp/q01_avg_*.csv`
- `benchmark/dp/tmp/q01_avg_*.sql`
- `benchmark/dp/ec2_backup_20260715/`
- `benchmark/dp/full_utility_runtime/`
- `benchmark/dp/verify_tpch_saa_bounds.py`
- `benchmark/dp/fill_tpch_target_metrics.py`
- `benchmark/dp/dp_target_metric_runner.cpp`
- `benchmark/dp/run_tpch_jcch_sass_stability.py`
- `benchmark/tpch/plot_tpch_as_m_sweep.R`
- `benchmark/tpch/as_tpch_sf30_graviton_fullsweep_recheck_run*.csv`
- `benchmark/tpch/tpch_as_m_sweep_*`

Generated logs/plots can be moved later, but keep them until the reproducibility
scripts can regenerate the same filenames.

## Backup Snapshot

EC2 backup directory:

- `/home/ubuntu/privacy_backups/20260717_runtime_workaround/`

It contains:

- `privacy_worktree_backup_20260717_lean.tgz`
- `remote_git_status.txt`
- `remote_tracked_diff.patch`
- `run_tpch_runtime_3x1run.sh`
- all three one-run runtime configs and CSVs
- the combined clean runtime CSV/log
- `tpch_sf30_runtime_perfect_m64_m512_3run_clean_summary_20260717.csv`

The backup archive SHA256 is:

```text
551755e0fcf509382ecb990eccd4cc7dad97f2a855bbef456119d07e4b67914c
```

The archive excludes build output, `.git`, `duckdb/`, local DuckDB database
files, `hits.parquet`, and raw SQLStorm data. It includes the source tree,
benchmark configs, Python/R/C++ benchmark helpers, paper files, CSVs, logs, and
plots used during this experiment pass.
