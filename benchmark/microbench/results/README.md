# Microbenchmark Results

Results of `bench_count.sh`, `bench_sum_avg.sh` and `bench_min_max.sh`, one directory per machine.
`plot_microbenchmark.R` reads the newest file of each suite and writes the figures to `../plots/`.

| Directory | Machine | Sizes |
|---|---|---|
| `graviton/` | AWS Graviton4 | 10M, 100M, 1000M rows |
| `granite-rapids/` | Intel Granite Rapids | COUNT: all; SUM and MIN/MAX: 100M rows |
| `epyc/` | AMD EPYC | COUNT: all; SUM and MIN/MAX: 100M rows |
| `macbook/` | MacBook Pro (M2 Max) | COUNT and SUM: all; MIN/MAX: 100M rows |

Runs restricted to 100M rows supply the cross-machine SIMD comparison (Figure 7), which only uses that size.
`macbook/sum_figure_*.csv` is the input of the SUM optimization figure (Figure 5); the SIMD comparison reads
`macbook/sum_avg_*.csv`.

A `wall_sec` of `-1` marks a configuration that failed: ablation builds at the highest group counts, which run out
of memory or temporary disk space (see
[`docs/benchmark/microbenchmarks.md`](../../../docs/benchmark/microbenchmarks.md)).
