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
