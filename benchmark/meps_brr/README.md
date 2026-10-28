# MEPS application workloads (WIP)

For the Graviton4 server configuration, persistent launch commands, and artifact
provenance, see [EC2 reproducibility](../EC2_REPRODUCIBILITY.md).

Two workloads use explicit SQL files in `queries/`, `naive_queries/` and
`as_queries/`, plus `conditional_queries/` for the one-query ordinary SQL baseline,
following the TPC-H/SSB benchmark organization.
The existing C++ target name `meps_brr_poc` is retained. No R runtime is timed.

## Workloads and original methodology

- **damico2006**: Table 2 of [Damico (2009), The R Journal](https://journal.r-project.org/articles/RJ-2009-018/).
  Uses HC-105, AGE06X > 17 (adults), and AGE06X > 17 AND AGE06X < 65
  (nonelderly adults). Uses the **historical HC-036BRR 1996–2006 file** and its
  64 flags, not the first 64 flags from a newer design. The runner checks the
  published means (4009.1, 3155.4) and both published BRR standard-error conventions
  (83.6/77.5 full-estimate centered; 83.0/77.0 replicate-mean centered), allowing
  only the rounding of the one-decimal published values. This reproduces the two
  mean/SE examples, not every analysis in the paper.
- **ahrq2016**: [AHRQ's event-analysis example](https://github.com/HHS-AHRQ/MEPS/blob/master/R/summary_tables_examples/use_events_2016.R).
  Uses HC-192 persons and HC-188F/G events: office/outpatient counts and five
  payment-source means, each for all events and physician-only events, plus two
  office-visit means per person. The latter retains people with zero visits.
  **Adaptation:** AHRQ's example uses Taylor variance estimation. Here the same
  estimands use the official 128 binary BRR flags and standard BRR weights.
  This is not a reproduction of the original Taylor standard errors.

The official BRR rule is [replicate weight = 2 × BRR flag × person weight](https://meps.ahrq.gov/mepsweb/data_stats/download_data/pufs/h036brr/h36brr24doc.shtml).
Means use each replicate's own denominator. All event records inherit their person's
flag and survey weight. No random sampling, noise, Fay adjustment or DP claim is involved.

## Four SQL modes

- **duckdb**: ordinary full-sample weighted SQL. One answer per metric.
  This is a single-query cost reference, **not equivalent work** to 64/128 estimates.
- **naive_as**: the same analysis repeated for each official BRR replicate.
  Its SQL reads `BRR@REPLICATE@` directly and applies `2 * weight * BRR`.
  No packed-mask construction or AS function is imposed on this baseline.
  Reported latency is the first replicate's query time multiplied by 64 or 128,
  matching the TPC-H extrapolation. Remaining replicates are evaluated for
  correctness and BRR standard errors, outside reported timing.
- **simd_as**: explicitly rewritten SQL using one or two 64-lane `as_sum` blocks.
  The non-materialized `persons_as` view joins people to BRR and packs the indicators
  inside the timed query. Ratios are calculated lane-wise in SQL.
- **conditional_sql**: one ordinary SQL query computing all official replicate
  estimates with conditional SUMs on the original BRR columns. The scan and joins
  are shared, without packed masks or AS functions. SQL lists only package the
  aggregate outputs; they do not expand input rows by replicate.

All SQL retains natural, wide aggregate expressions. There is no lateral
metric-row expansion, contribution materialization, or precomputed aggregate table.
The event join remains in all four SQL variants. The per-person visit-count CTE
also runs inside each query. Standard SQL does not need a BRR join; naive/AS do.

The runner times planning, scans, linkage, mask construction when applicable,
joins, aggregation, SQL ratio evaluation, and result materialization. Query generation,
EXPLAIN, result validation and final cross-replicate SE computation are outside
`query_ms`. Import/validation are recorded separately. Download/CSV conversion is
also outside query timing. This measures database execution, not raw-download time.

## Prepare and run

From the repository root (Python/pandas is only for downloading/converting input):

```sh
python3 benchmark/meps_brr/prepare.py /path/to/data/damico2006 --workload damico2006
python3 benchmark/meps_brr/prepare.py /path/to/data/ahrq2016 --workload ahrq2016
caffeinate -i bash benchmark/meps_brr/run.sh /path/to/data /path/to/new/results 1,8 5
```

Existing input CSVs/result directories are refused. Each converter records input
URLs/hashes/counts in its manifest. The converter fixes only the exact pandas
XPORT zero-decoding artifact `2**-260`, without thresholding legitimate values.

As in SSB, the wrapper builds/runs `PAC_NOAPPROX=OFF` and `ON`, retaining their
CMake caches, then restores the default approximate build. Do not run concurrently
with another task using the shared build. These labels identify SUM build modes;
they do not promise bitwise equality of floating-point SQL results.

## Validation and artifacts

For each query the runner saves the complete standard/AS SQL, first-replicate naive
SQL and EXPLAIN plans. The checked-in naive template specifies every other replicate.
All replicate values from naive, conditional and AS SQL are exported, alongside full-sample
points and both BRR standard errors. The standard SQL rows have zero SE placeholders:
they do not estimate uncertainty by themselves.

All runs must agree with the first ordinary SQL reference. The inherited numerical
guard is normalized error <= 1e-8 for ordinary SQL and <= 1e-3 for AS outputs, with
the observed maximum recorded. This is not an exact-equivalence claim. Every output
must be finite, have the expected metric/replicate count, and preserve nonempty ratio
domains. Damico's numerical checks supply an additional published external oracle.
AHRQ's Taylor SEs are not a BRR oracle; its event-query translation still requires
source/methodology review rather than merely comparing two implementations.

The wrapper's final argument is the number of measured repetitions. It adds run
zero as warmup, then rotates method order over runs 1--5 for a five-repetition
paper run. These are warm executions, not OS cold-cache measurements. Report
per-query medians separately for each workload and build. `timing_scope` marks
the naive extrapolation separately from complete-query timings.
Older materialized-contribution and generic-metric results are not comparable with
this revised suite and must not supply its speedup claims.

For the paper plot, run the wrapper once with five measured repetitions:

```sh
bash benchmark/meps_brr/run.sh /path/to/data /path/to/new/results 8 5
Rscript benchmark/meps_brr/plot_meps_results.R /path/to/new/results 8
```

The combined figure uses medians of five warm executions. Ordinary SQL bars use only the
approximate-build measurements, so controls are not counted twice. Both AS builds
are shown separately. Raw measurements and median summaries are exported as CSV.
