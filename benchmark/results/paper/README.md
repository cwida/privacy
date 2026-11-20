# Paper Figure Data

Inputs and rendered figures for the evaluation of the revised paper (DuckDB v2.0.0). `figures/` holds the PNGs
exactly as they appear in the paper; every one of them regenerates byte-identically from the inputs listed below with
the plot scripts in this repository. Fresh benchmark runs write to `benchmark/results/utility/` and do not touch this
directory.

| Figure | File in `figures/` | Inputs | Plot script |
|---|---|---|---|
| 1 | `tpch_benchmark_plot_sf30_paper.png` | `tpch/as_tpch_sf30_m64.csv` | `benchmark/tpch/plot_tpch_results.R` |
| 4 | `count_optimizations_granite-rapids_paper.png` | `benchmark/microbench/results/granite-rapids/` | `benchmark/microbench/plot_microbenchmark.R` |
| 5 | `sum_optimizations_macbook_paper.png` | `benchmark/microbench/results/macbook/sum_figure_20261119.csv` | same |
| 6 | `minmax_optimizations_graviton_paper.png` | `benchmark/microbench/results/graviton/` | same |
| 7 | `simd_improvements_paper.png` | `benchmark/microbench/results/*/` | same |
| 8 | `clickbench_as_slowdown_plot_paper.png` | `clickbench/as_clickbench.csv` | `benchmark/clickbench/plot_clickbench_as_slowdown.R` |
| 9 | `tpch_as_m_sweep_paper.png` | `tpch/as_tpch_sf30_m_sweep.csv` | `benchmark/tpch/plot_tpch_as_m_sweep.R` |
| 10 | `ssb_sf30_graviton_paper.png` | `ssb/{approximate,exact}/threads_16_timings.csv` | `benchmark/ssb/plot_ssb_results.R` |
| 11 | `meps_brr_graviton_paper.png` | `meps/{approximate,exact}/{damico2006,ahrq2016}/threads_16_timings.csv` | `benchmark/meps_brr/plot_meps_results.R` |
| 13 | `unskewed_sass_two_error_views_delta1e-06_m64_rescaletrue_paper.png` | `dp/dp_tpch_sf30_utility.csv` | `benchmark/dp/plot_unskewed_sass_utility_views.R` |
| 14 | `tpch_sass_sampled_m_1x_paper.png` | `dp/dp_tpch_sf30_utility.csv` | `benchmark/dp/plot_sass_sampled_m_1x.R` |
| 15 | `tpch_jcch_sf30_all5_runtime_compact_paper.png` | `dp/dp_tpch_sf30_utility.csv` | `benchmark/dp/plot_dp_full_utility_runtime.R` |
| 16 | `q01_avg_compact_grid_paper.png` | `dp/dp_tpch_sf30_q01_avg_{columns,groups}.csv` | `benchmark/dp/plot_q01_avg_compact_grid.R` |
| 17 | `q01_stability_m64_m512_rescaled_sumcount_paper.png` | `dp/dp_tpch_sf30_q01_stability_{avg,sum_count}_summary.csv` | `benchmark/dp/plot_q01_stability.R` |
| Table 1 | — | `accuracy/accuracy_{default,signedsum,exactsum}.csv` | — |

The microbenchmark inputs (Figures 4–7) live with the microbenchmark suite; see
[`benchmark/microbench/results/README.md`](../../microbench/results/README.md). The TPC-DS figure (Figure 12) and its
inputs are added together with the TPC-DS benchmark.

The analytical runs (TPC-H, ClickBench, SSB, MEPS, DP) were measured on the Graviton machine with 16 threads and
`priv_state_export=true`; see [`repro.md`](../../../repro.md) for the configurations, machines and commands.

## Regenerating the figures

```bash
P=benchmark/results/paper
O=benchmark/results/figures
mkdir -p $O/ssb

Rscript --vanilla benchmark/tpch/plot_tpch_results.R $P/tpch/as_tpch_sf30_m64.csv $O
Rscript --vanilla benchmark/tpch/plot_tpch_as_m_sweep.R $P/tpch/as_tpch_sf30_m_sweep.csv $O
Rscript --vanilla benchmark/clickbench/plot_clickbench_as_slowdown.R $P/clickbench/as_clickbench.csv $O 64

# Writes ssb_sf30_t16_combined_paper.png (Figure 10) into $O/ssb.
Rscript --vanilla benchmark/ssb/plot_ssb_results.R $P/ssb 30 16 $O/ssb

# The MEPS script writes next to its inputs, so plot from a copy: meps_t16_five_runs_median.png (Figure 11).
cp -R $P/meps $O/meps && Rscript --vanilla benchmark/meps_brr/plot_meps_results.R $O/meps 16

Rscript --vanilla benchmark/dp/plot_unskewed_sass_utility_views.R $P/dp/dp_tpch_sf30_utility.csv $O 1e-6 64 true
Rscript --vanilla benchmark/dp/plot_sass_sampled_m_1x.R $P/dp/dp_tpch_sf30_utility.csv \
  $O/tpch_sass_sampled_m_1x_paper.png tpch 1e-6 median
# Writes tpch_sf30_all5_runtime_compact_paper.png (Figure 15).
Rscript --vanilla benchmark/dp/plot_dp_full_utility_runtime.R $P/dp/dp_tpch_sf30_utility.csv $O
Rscript --vanilla benchmark/dp/plot_q01_avg_compact_grid.R \
  $P/dp/dp_tpch_sf30_q01_avg_columns.csv $P/dp/dp_tpch_sf30_q01_avg_groups.csv $O/q01_avg_compact_grid_paper.png
Rscript --vanilla benchmark/dp/plot_q01_stability.R \
  $P/dp/dp_tpch_sf30_q01_stability_avg_summary.csv $P/dp/dp_tpch_sf30_q01_stability_sum_count_summary.csv \
  $O/q01_stability_m64_m512_rescaled_sumcount_paper.png

# Figures 4-7, written to benchmark/microbench/plots/.
Rscript --vanilla benchmark/microbench/plot_microbenchmark.R
```

Text is drawn in Linux Libertine when that font is installed and in the R `serif` family otherwise. The files in
`figures/` for Figures 4–7 were rendered with Linux Libertine and the others with the `serif` fallback, so a
byte-identical result needs the same font; the plotted values do not depend on it.
