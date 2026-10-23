# Paper Result Data

This directory contains the CSV inputs retained for the submitted paper figures. The files are immutable reference
artifacts; new runs use `benchmark/results/utility/` and `benchmark/results/sanity/`.

| File | Contents |
|---|---|
| `as_clickbench_m64_m512.csv` | ClickBench DuckDB and SIMD-AS runtimes |
| `as_tpch_m64.csv` | TPC-H DuckDB, SIMD-AS, naive-AS, and simple-hash runtimes |
| `as_tpch_m_sweep.csv` | Corrected TPC-H DuckDB and variable-`m` SIMD-AS run |
| `as_tpch_m_figure.csv` | TPC-H variable-`m` figure summary |
| `dp_tpch_utility_raw.csv` | Main TPC-H DP utility sweep |
| `dp_tpch_m_figure.csv` | SAA target-error variable-`m` figure summary |
| `dp_tpch_runtime_raw.csv` | TPC-H DP runtime measurements |
| `dp_tpch_runtime_figure.csv` | TPC-H DP runtime figure summary |
| `q01_avg_columns_raw.csv` | Three-run Q01 AVG-column measurements |
| `q01_avg_groups_raw.csv` | Three-run Q01 AVG grouping measurements |
| `q01_avg_figure.csv` | Q01 AVG figure values |
| `q01_stability_avg_summary_raw.csv` | AVG lane-stability measurements |
| `q01_stability_sum_count_summary_raw.csv` | Rescaled SUM/COUNT lane-stability measurements |
| `q01_stability_figure.csv` | Q01 lane-stability figure values |

The JSON configurations in `benchmark/configs/utility/` define the corresponding experiments. See
[`repro.md`](../../../repro.md) for commands, bounds, metrics, and machine details.

## Preliminary revision experiments (2026-10-21)

These additional files are new revision measurements, not inputs to the submitted
figures. Existing reference files above are unchanged.

- `ssb_sf30_t8_{approximate,exact}.csv`: all 13 SSB queries, 64 customer-subset
  worlds, eight threads, a 6 GB DuckDB memory-manager limit, one excluded warmup
  and one measured repetition per mode. Hashing and joins execute inside query
  timing on the original five tables. Exact results agree at FLOAT output
  precision; approximate error is measured, not hidden by a tolerance.
- `ssb_sf30_t8_{approximate,exact}_coverage.csv`: automatic rewrite/execution
  coverage, separate from explicit-rewrite timing and per-world validation.
- `meps_brr_inline_t{1,2,4,8}.csv`: all four MEPS analyses and four methods, five
  executions per configuration. Run 0 is first execution; use runs 1--4 for warm
  medians. Joins, official BRR-mask construction, and weighting are inside query
  timing; import/preparation are recorded separately.
- `meps_brr_inline_t8_estimates.csv`: full-sample estimates and BRR standard errors
  for all methods at eight threads. This is a BRR reproduction of the estimands
  in AHRQ's 2016 examples, not identical Taylor-linearization standard errors.

See [SSB](../../ssb/README.md) and [MEPS](../../meps_brr/README.md) for methodology.
These are laptop measurements on an eight-core M1 with 16 GB RAM. They do not
establish noise-free timing differences or hardware-independent speedups.
