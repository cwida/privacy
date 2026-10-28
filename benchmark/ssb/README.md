# WIP: Star Schema Benchmark with sampled worlds

For the Graviton4 server configuration, persistent launch commands, and artifact
provenance, see [EC2 reproducibility](../EC2_REPRODUCIBILITY.md).

The benchmark executes all 13 joined SSB queries on the five original tables.
SQL follows the [SSB specification](https://www.cs.umb.edu/~poneil/StarSchemaB.PDF)
listings, including Q1.3 quantity 26--35 and Q2.3 brand `MFGR#2221`.

## Data and execution

Use [electrum/ssb-dbgen](https://github.com/electrum/ssb-dbgen) commit
`219403ad7d1dd32ae1f97b5553abf92129fccd7f`. On macOS, apply
`benchmark/ssb/dbgen-macos.patch` in that external checkout and rebuild:

```sh
make dbgen CFLAGS='-O2 -std=gnu89 -include unistd.h -DDBNAME=\"dss\" -DMAC -DDB2 -DSSBM'
```

The patch enlarges the order-priority buffer to fit `4-NOT SPECIFIED` and fixes
out-of-bounds RNG bookkeeping: stream IDs 97/98 must use the same fallback as
`UnifInt`. Generate every scale with the same patched source.

```sh
bash benchmark/ssb/prepare.sh /path/to/ssb-dbgen /new/data/directory 30
SSB_MEMORY_LIMIT=6GB caffeinate -is bash benchmark/ssb/run.sh \
  /new/data/directory /new/output/directory 8 5
```

Arguments are the data directory, a new output directory, comma-separated thread
counts (default `1,8`), and measured repetitions (default `5`). Use integer scale
factors of at least 1. Generation records the commit, source patch, scale, row
counts, and checksums; the runner requires the completion marker, verifies row
counts and dimension keys, and refuses existing output paths.

The wrapper builds and runs both SUM configurations sequentially:
`PAC_NOAPPROX=OFF` (default approximate) and `PAC_NOAPPROX=ON` (exact cascading).
It stores results and build configurations under `approximate/` and `exact/`,
then restores the default approximate build. Ordinary SQL baselines are measured
only in the approximate run. The exact run uses `--as-only`: it still computes
the independent 64-world SQL reference for validation, but measures only AS.
Plots reuse the first run's baseline timings rather than recording duplicates.
Build, generation, and import times
are outside query timing. `SSB_MEMORY_LIMIT` limits DuckDB's memory manager, not
the entire process RSS. On AC power, macOS `caffeinate -is` prevents idle sleep;
it does not override forced sleep or closing the lid.

## SQL and measurements

As in the TPC-H benchmark, original queries are in `queries/` and explicit
AS rewrites are in `as_queries/`. The original joins, filters, and grouping keys
are retained. CUSTOMER is the sampling entity: `priv_hash(hash(lo_custkey))`
computes membership during each query. LINEORDER already contains its customer's
foreign key, so no additional CUSTOMER join is needed just to obtain that key.
Queries that use CUSTOMER attributes retain their CUSTOMER join.

Settings are deterministic (`privacy_seed=42`, `pac_mi=0`,
`pac_hash_repair=true`): each customer belongs to exactly 32 of 64 worlds,
and all its facts inherit the same membership. Worlds overlap; their
memberships are not independent Bernoulli trials. PART, SUPPLIER, and DATE remain
shared. Customer-level sampling is our experimental extension, not part of SSB.

- `duckdb_full`: ordinary SQL on the full original tables; a runtime reference,
  not an answer-equivalent baseline for sampled worlds.
- `repeated_sql`: 64 ordinary queries, each filtering LINEORDER by one bit of
  the query-time membership expression before joining and aggregating. Reported
  time is world 0's query latency multiplied by 64, matching the TPC-H
  extrapolation. The other 63 worlds are evaluated for correctness only.
- `asa`: one explicitly rewritten query, using
  `as_sum(priv_hash(hash(lo_custkey)), value)` and `as_count` for group presence.

All three modes use the same original tables. Timings include planning, hashing
and world filtering where applicable, joins, aggregation, and result
materialization. There is one excluded warmup per mode; measured order rotates.
C++ conversion and correctness checks are excluded consistently. The original
`ORDER BY` clauses execute in all timed modes. For AS, aggregate ordering keys
are lists and DuckDB compares them lexicographically; this orders the returned
batch, not each world's result independently. Per-world aggregate values are
checked by world and group; SSB has no LIMIT.

The aggregate API returns twice the selected sum. The checker divides by two
to compare actual sampled-world sums, not full-data estimators.
`as_count` distinguishes absent groups from present groups with zero sums;
empty ungrouped SUM is NULL, and empty grouped worlds emit no rows.

## Validation and outputs

Every query checks all 64 worlds against independent ordinary SQL. Missing or
extra groups, NULL mismatches, and non-finite values fail either configuration.
Exact cascading must agree after rounding the SQL reference to the aggregate's
FLOAT output type; this is not exact integer output beyond FLOAT's range.
Approximate SUM reports numerical errors without an arbitrary pass threshold:
maximum/mean relative error, maximum absolute error, and mismatches beyond output
rounding. Relative error is `abs(actual-expected)/max(1,abs(expected))`; NULL
answers are excluded. Structural coverage does not imply numerical equivalence.

Separately, the runner checks automatic-rewriter acceptance and execution of all
13 original queries, including their ORDER BY. CUSTOMER privacy-unit metadata,
a protected customer key, and LINEORDER ownership metadata permit customer
city/nation grouping while retaining key protection. Coverage is recorded, not
silently assumed. This check is separate from per-world equivalence of the
explicit SQL rewrites.

Each configuration writes `_timings.csv`, `_coverage.csv`, `_plans.txt`,
`_queries.sql`, and a DuckDB database containing the five original SSB tables.
Timing rows identify `membership_mode=query_time_priv_hash`; the SQL and plans
show exactly what was timed. `timing_scope=world0_x64` identifies extrapolated
naive timing; `complete_query` identifies the other modes. Paper runs use five
measured executions after one warmup and report medians.

## Paper-style plot

```sh
Rscript benchmark/ssb/plot_ssb_results.R /new/output/directory 30 8
```

The adapter uses `benchmark/tpch/plot_tpch_results.R` for the paper's style.
The combined figure has four bars per query: gray full-data DuckDB, blue repeated
SQL, bright-green approximate AS, and darker striped exact AS. Both ordinary
SQL series come from the first configuration rather than being duplicated.
Labels above blue and green bars show runtime divided by full-data DuckDB
runtime. Query labels use Q1.1--Q4.3; there is no title or caption.
The plotter requires query-time membership timing files.
