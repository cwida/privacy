# Microbenchmark Results

Results of `bench_count.sh`, `bench_sum_avg.sh` and `bench_min_max.sh` on DuckDB v2.0.0, one directory per machine.
`plot_microbenchmark.R` reads the newest file of each suite and writes the figures to `../plots/`.

| Directory | Machine | Measured | State export | Sizes |
|---|---|---|---|---|
| `graviton/` | AWS Graviton | 2026-11-19 | off | 10M, 100M, 1000M rows |
| `granite-rapids/` | Intel Granite Rapids | 2026-11-20 | off | COUNT: all; SUM and MIN/MAX: 100M rows |
| `epyc/` | AMD EPYC | 2026-11-20 | off | COUNT: all; SUM and MIN/MAX: 100M rows |
| `macbook/` | MacBook M2 Max | 2026-11-17 | on | COUNT and SUM: all; MIN/MAX: 100M rows |

The three server runs were built from the sources of commit `f4c94f4`, where `priv_state_export` defaults to off.
The MacBook run predates that setting, so its aggregates export their state. Runs restricted to 100M rows supply the
cross-machine SIMD comparison (Figure 7), which only uses that size.

`macbook/sum_figure_20261119.csv` is the input of the MacBook SUM figure (Figure 5). Its plain-DuckDB rows and its
naive rows (`standard`, `nocascading`, `nosimd`) come from `macbook/sum_avg_20261117_143308.csv`; its `default`,
`nobuffering` and `exactsum` rows are the measurements of the original submission (same machine, DuckDB 1.5.4,
2026-01-28). `plot_microbenchmark.R` uses a `sum_figure_*.csv` for the SUM figure when one is present and the plain
`sum_avg_*.csv` everywhere else.

A `wall_sec` of `-1` marks a configuration that failed: ablation builds at the highest group counts, which run out
of memory or temporary disk space (see
[`docs/benchmark/microbenchmarks.md`](../../../docs/benchmark/microbenchmarks.md)).
