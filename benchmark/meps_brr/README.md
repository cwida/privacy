# MEPS replicate-weight workloads

Two survey analyses on the Medical Expenditure Panel Survey (MEPS), whose variance is estimated with balanced
repeated replication (BRR). Each analysis is written as SQL in four variants: `queries/` (ordinary full-sample
SQL), `naive_queries/`, `conditional_queries/` and `as_queries/`. The runner is `meps_brr_poc`; no R code is timed.

## Workloads

- **`damico2006`**: Table 2 of [Damico (2009), The R Journal](https://journal.r-project.org/articles/RJ-2009-018/).
  Mean expenditure of adults (`AGE06X > 17`) and of nonelderly adults (`AGE06X > 17 AND AGE06X < 65`) on HC-105,
  with the historical HC-036BRR 1996--2006 file and its 64 replicates. The runner checks the published means
  (4009.1, 3155.4) and both published conventions for the BRR standard error (83.6/77.5 centred on the full
  estimate, 83.0/77.0 centred on the replicate mean), up to the rounding of the one-decimal published values.
- **`ahrq2016`**: the estimands of
  [AHRQ's event-analysis example](https://github.com/HHS-AHRQ/MEPS/blob/master/R/summary_tables_examples/use_events_2016.R)
  on HC-192 persons and HC-188F/G events: office and outpatient counts and five payment-source means, each for all
  events and for physician events, plus two office-visit means per person (people with zero visits included).
  AHRQ's example estimates variance by Taylor linearization; here the same estimands use the 128 official BRR
  replicates, so its standard errors are not reproduced.

## Replicates as worlds

There is no random sampling in this benchmark: the worlds are the survey's official BRR replicates, 64 for 2006 and
128 for 2016. Each person has one binary flag per replicate, and the
[official rule](https://meps.ahrq.gov/mepsweb/data_stats/download_data/pufs/h036brr/h36brr24doc.shtml) defines the
replicate weight as `2 × flag × person weight`. Event records inherit the flag and weight of their person, and a
mean uses the denominator of its own replicate. No noise, Fay adjustment or privacy mechanism is involved.

## Compared methods

| Paper name | `mode` in the CSV | What runs | What is timed |
|---|---|---|---|
| DuckDB | `duckdb` | The full-sample weighted analysis: one answer per metric. | The whole query. |
| Naive-SA | `naive_as` | The analysis once per replicate. The SQL reads that replicate's flag column and weights rows by `2 * weight * flag`. | The first replicate only; the reported time is that latency × 64 or × 128. |
| Conditional SQL | `conditional_sql` | One ordinary query with one conditional `SUM` per replicate over the original flag columns. Scans and joins are shared. | The whole query. |
| SIMD-ASA | `simd_as` | One query using one (64 replicates) or two (128 replicates) `as_sum` aggregates. A non-materialized view packs each person's flags into 64-bit masks inside the query; ratios are computed per lane in SQL. | The whole query. |

DuckDB is a cost reference for a single estimate; it does not do the work of 64 or 128 estimates.

The naive runtime is extrapolated (`timing_scope=replicate1_x_worlds`; the other modes have `complete_query`). The
remaining replicates are still executed, untimed, because they supply the reference values and the BRR standard
errors. Conditional SQL is the strongest ordinary-SQL baseline: it also evaluates every replicate in one pass, but
with one aggregate per replicate instead of one vectorized aggregate.

All four variants keep the event join and the per-person visit-count CTE inside the query. The naive and SIMD-ASA
variants additionally join the BRR file; the full-sample query does not need it. Nothing is precomputed: there is
no materialized contribution table and no stored mask.

A timing covers planning, scans, joins, mask construction where it applies, aggregation, ratio evaluation and
result materialization. Query generation, `EXPLAIN`, validation, the cross-replicate standard-error computation,
download and import are outside it. Each database gets one warm-up run (run 0) followed by five measured runs in
rotating method order; the figure shows per-query medians.

## Running

Python with pandas is needed only to download and convert the public input files. The paper's configuration is 16
threads and five repetitions on the Graviton4 machine:

```sh
python3 benchmark/meps_brr/prepare.py /path/to/data/meps/damico2006 --workload damico2006
python3 benchmark/meps_brr/prepare.py /path/to/data/meps/ahrq2016 --workload ahrq2016
bash benchmark/meps_brr/run.sh /path/to/data/meps /path/to/results/meps 16 5
Rscript --vanilla benchmark/meps_brr/plot_meps_results.R /path/to/results/meps 16
```

`run.sh` takes the data root, a new output directory, a comma-separated list of thread counts and the number of
measured repetitions. Like the SSB wrapper it builds and measures the approximate SUM kernel (`PAC_NOAPPROX=OFF`)
and the exact one (`PAC_NOAPPROX=ON`), keeps both CMake caches and restores the default build afterwards; do not run
it concurrently with anything else that uses the same build directory. Existing input and result directories are
refused. The converter records input URLs, hashes and row counts in a manifest, and corrects one artefact of pandas'
XPORT reader (zeros decoded as `2**-260`).

## Validation and outputs

All replicate values of the naive, conditional and SIMD-ASA variants are exported together with the full-sample
point estimates and both BRR standard errors (`_estimates.csv`; the full-sample rows carry zero as a placeholder
standard error). Every variant must agree with the ordinary SQL reference: normalized error at most `1e-8` for
ordinary SQL and `1e-3` for SIMD-ASA, with the observed maximum recorded. Every output must be finite and have the
expected number of metrics and replicates. For `damico2006` the published values are an additional external check;
for `ahrq2016` there is none, because the published standard errors use a different variance estimator.

For each query the runner also saves the SQL of every variant (the naive one instantiated for the first replicate)
and the `EXPLAIN` plans. The plot script shows the ordinary-SQL bars from the approximate build only, so the
controls are not counted twice, and one SIMD-ASA bar per SUM kernel.
