# MEPS BRR application benchmark (WIP)

This replaces the original one-query POC; the existing `meps_brr_poc` C++ target is retained.
No R runtime or R performance claims. All four measured implementations run in C++.

## Workload and provenance

The four SQL files reproduce the estimands and filters of AHRQ's 2016
[expenditure exercise](https://github.com/HHS-AHRQ/MEPS/blob/master/R/workshop_exercises/exercise_1a.R)
and [event analysis](https://github.com/HHS-AHRQ/MEPS/blob/master/R/summary_tables_examples/use_events_2016.R):
national expenditure totals/means/prevalence; age-domain means among spenders;
OB/OP event counts and mean expenditures by payment source; office visits per person.
The latter retains people with no visits. Person weights and replicate membership
propagate to all dependent event records. Non-physician events remain excluded only
from physician-specific domains, following AHRQ's `SEEDOC == 1` definition.

Those original examples use Taylor linearization. We intentionally replace **variance
estimation**, not the estimands, with AHRQ's documented
[standard BRR procedure](https://meps.ahrq.gov/mepsweb/data_stats/download_data/pufs/h036brr/h36brr24doc.shtml).
This is an application reproduction, not an existing published MEPS speed benchmark.
We do not claim identical SEs to the Taylor-based outputs.

For every replicate, weights are `2 * BRR * person_weight`. Means/proportions divide
the replicate numerator by its own replicate denominator. The point estimate uses
full-sample weights, and `SE = sqrt(sum((replicate - point)^2)/128)` (full-estimate
centering, equivalent to the `mse=TRUE` BRR convention). No random-world selection,
new sampling, PAC noise, Fay adjustment, or privacy guarantee is involved.

## Four methods

- `cpp_replicates`: native double-precision, replicate-major weighted reductions,
  parallel over replicates. Inspired by the replicate loop in `survey`, not a port
  or timing of that package. `extract_ms` separately measures sorting/extraction
  into C++ arrays; `compute_ms` measures the remaining native computation.
  `analysis_ms` includes both, so extraction is not hidden in end-to-end comparisons.
- `repeated_sql`: 128 complete ordinary aggregate queries over prepared data.
- `conditional_sql`: one query with 128 pairs of ordinary conditional sums.
- `asa`: two 64-world blocks for each numerator/denominator, using `as_sum`.

All methods reuse the same prepared data. In these additive total/ratio analyses,
event contributions can first be summed per person and metric without changing any
replicate result. We apply this optimization to **every** method; joins are not
repeated 128 times in the reference baseline. Query definitions describe these
contributions; they are not a general automatic rewrite of arbitrary survey SQL.

Both SQL baselines use ordinary bit tests on the supplied binary membership masks;
neither calls AS functions. Common loading, validation, mask construction, and
person/event preparation are measured and charged to every method. This benchmark
therefore isolates aggregation benefits after equally shared relational work.

## Run

From the repository root, with Python/pandas installed only for source conversion:

```sh
python3 benchmark/meps_brr/prepare.py /path/to/new/data
cmake --build build/release --target meps_brr_poc -j 2
bash benchmark/meps_brr/run.sh /path/to/new/data /path/to/new/results 1,2,4,8 5
```

`prepare.py` downloads HC-192, HC-188F/G and HC-036BRR, converts selected fields to
CSV, and records source URLs, SHA-256 hashes, row counts, and download/conversion time.
An existing official `h36brr24.dta` can be supplied with `--brr-dta PATH`.
The converter replaces only the exact `2**-260` artifact from pandas XPORT zero
decoding (pandas issue #30051) with zero. It does not threshold small numbers.
The runner checks the published 2016 counts (6,933 zero-expenditure persons and
33,259 positive-weight persons), and checks point estimates against the rounded
[AHRQ output](https://github.com/HHS-AHRQ/MEPS/blob/master/SAS/workshop_exercises/exercise_1a/Exercise1a_OUTPUT.TXT)
in `queries/expected_points.csv`. These tolerances follow the published precision.
Published Taylor standard errors are deliberately **not** used as BRR oracles.
Earlier validation runs using the uncorrected CSVs are invalid for prevalence and
spender-domain analyses; regenerate input files in a new directory and rerun.
That conversion is outside C++ timing; report it separately rather than hiding it
inside a claimed raw-download end-to-end time. The database is loaded from those
CSV files at each thread setting. Existing database/result paths are refused.

The runner outputs individual run timings, point estimates, BRR SEs, and all 128
reference replicate estimates. Run zero is first execution, **not** an OS cold-cache
measurement. Subsequent modes rotate order. Use later runs for warm medians. The
per-query `first_workflow_ms` includes common import/preparation and that query;
do not sum those rows, which would double-count setup. The final log reports suite
workflow totals with common import/preparation charged once per method.

Every group, point, replicate and SE is checked against the native double reference.
Double SQL paths require relative error <=1e-8; current FLOAT ASA outputs use <=1e-3,
with the observed maximum always recorded. This is an explicit numerical tolerance,
not an exact-equality claim. Undefined ratio domains or non-finite values fail the run.
Fresh input validation rejects missing/duplicate person linkage and non-binary BRR flags.

No speedup should be reported from runs competing with another benchmark. Setting
DuckDB threads does not guarantee every small operator runs in parallel. Native
workers use the same configured limit, capped at 128 independent replicates.
