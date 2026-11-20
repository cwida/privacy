# Star Schema Benchmark (SSB)

The benchmark runs all 13 SSB queries on the five original tables and evaluates each of them over 64 sampled worlds.
The SQL follows the listings of the [SSB specification](https://www.cs.umb.edu/~poneil/StarSchemaB.PDF), including
Q1.3 quantity 26--35 and Q2.3 brand `MFGR#2221`. Original queries are in `queries/`; the rewritten queries are in
`as_queries/`.

## Sampling model

- **Sampling entity.** CUSTOMER. A LINEORDER row belongs to the worlds of its customer (`lo_custkey`), so all facts
  of a customer are sampled together. PART, SUPPLIER and DATE are shared by every world. Customer-level sampling is
  the experiment's choice; it is not part of SSB.
- **Membership.** `priv_hash(hash(lo_custkey))` returns a 64-bit mask whose bit *w* says whether the customer is in
  world *w*. The mask is computed inside every timed query; no sample table or mask column is materialized.
  LINEORDER already carries the customer key, so obtaining it needs no extra join.
- **Shape of the worlds.** With `pac_hash_repair=true` every mask has exactly 32 bits set: each customer is in 32 of
  the 64 worlds. The worlds overlap and are not independent Bernoulli samples.
- **Determinism.** `privacy_seed=42` fixes the masks and `pac_mi=0` disables noise, so every method sees the same
  64 worlds and the outputs can be compared value by value.

## Compared methods

| Paper name | `mode` in the CSV | What runs | What is timed |
|---|---|---|---|
| DuckDB | `duckdb_full` | The original query on all rows. | The whole query. |
| Naive-SA | `repeated_sql` | The original query once per world, with LINEORDER replaced by the rows of that world (below). | World 0 only; the reported time is that latency × 64. |
| SIMD-ASA | `asa` | One rewritten query: `as_sum(mask, value)` for the aggregates and `as_count(mask)` for group presence. | The whole query. |

DuckDB is a cost reference: it answers the query on the full data and its result is not comparable with a sampled
world.

The naive baseline restricts LINEORDER to one world and leaves the rest of the query unchanged:

```sql
(SELECT * FROM lineorder WHERE ((priv_hash(hash(lo_custkey)) >> w) & 1) <> 0) AS ssb_input
```

Its reported runtime is extrapolated: world 0 is timed and multiplied by 64 (`timing_scope=world0_x64` in the CSV;
the other modes have `complete_query`). The remaining 63 worlds are executed too, untimed, because every one of them
is needed to validate the SIMD-ASA result. `repro.md` reports a check of this extrapolation on TPC-H.

All three methods read the same original tables. A timing covers planning, mask computation and world filtering
where they apply, joins, aggregation, the original `ORDER BY` and result materialization. Each mode gets one
untimed warm-up, the order of the modes rotates between repetitions, and the figure shows the median of five
repetitions. Build, data generation, import and result checking are outside the timings.

The wrapper measures two SUM kernels in sequence, each with its own build: the default approximate SUM
(`PAC_NOAPPROX=OFF`) and exact cascading SUM (`PAC_NOAPPROX=ON`). Results go to `approximate/` and `exact/`. The
DuckDB and Naive-SA timings are measured in the approximate run only and reused for the exact run, which still
recomputes the 64 reference worlds to validate itself. The wrapper restores the default build when it finishes.

## Running

Generate the data with [electrum/ssb-dbgen](https://github.com/electrum/ssb-dbgen) at commit
`219403ad7d1dd32ae1f97b5553abf92129fccd7f`, with `benchmark/ssb/dbgen-macos.patch` applied in that checkout.
Despite its name the patch is needed on every platform: it enlarges the order-priority buffer so that
`4-NOT SPECIFIED` fits and fixes out-of-bounds bookkeeping of the random-number streams.

```sh
git -C /path/to/ssb-dbgen apply "$PWD/benchmark/ssb/dbgen-macos.patch"
# Linux
make -C /path/to/ssb-dbgen dbgen \
  CFLAGS='-O2 -std=gnu89 -include unistd.h -D_GNU_SOURCE -DDBNAME=\"dss\" -DLINUX -DDB2 -DSSBM'
# macOS
make -C /path/to/ssb-dbgen dbgen CFLAGS='-O2 -std=gnu89 -include unistd.h -DDBNAME=\"dss\" -DMAC -DDB2 -DSSBM'
```

The paper's configuration is scale factor 30, 16 threads, a 30 GB DuckDB memory limit and five repetitions on the
Graviton4 machine:

```sh
bash benchmark/ssb/prepare.sh /path/to/ssb-dbgen /path/to/data/ssb_sf30 30
SSB_MEMORY_LIMIT=30GB bash benchmark/ssb/run.sh /path/to/data/ssb_sf30 /path/to/results/ssb_sf30 16 5
Rscript --vanilla benchmark/ssb/plot_ssb_results.R /path/to/results/ssb_sf30 30 16
```

`run.sh` takes the data directory, a new output directory, a comma-separated list of thread counts (default `1,8`)
and the number of repetitions (default `5`). `prepare.sh` records the generator commit, patch, scale factor, row
counts and checksums; `run.sh` requires its completion marker, verifies row counts and dimension keys, and refuses
an existing output directory. `SSB_MEMORY_LIMIT` limits DuckDB's memory manager, not the resident size of the
process.

## Validation

Every query is checked in all 64 worlds against the naive per-world SQL, by world and group. Missing or extra
groups, NULL mismatches and non-finite values fail the run in either build.

- `as_sum` returns twice the sum of the selected rows; the checker halves it before comparing.
- `as_count` separates an absent group from a present group whose sum is zero. An empty ungrouped SUM is NULL and an
  empty group produces no row.
- Exact SUM must match the reference after the reference is rounded to the aggregate's FLOAT output type.
- Approximate SUM has no pass threshold. The run reports the maximum and mean relative error
  (`abs(actual - expected) / max(1, abs(expected))`), the maximum absolute error and the number of values that
  differ beyond output rounding.
- The `ORDER BY` of a rewritten query sorts lists of per-world values lexicographically. This orders the returned
  batch, not each world's result on its own. SSB has no `LIMIT`, so the set of rows is unaffected.

Separately, the runner submits the 13 original queries to the automatic rewriter and records whether each is
accepted and executes (`_coverage.csv`). This is independent of the per-world validation of the handwritten
rewrites.

## Outputs

For each build and thread count the runner writes `threads_<n>_timings.csv`, `_coverage.csv`, `_plans.txt` and
`_queries.sql`, plus a DuckDB database with the five tables. The plot script combines both builds into one figure
with four bars per query: DuckDB, Naive-SA, SIMD-ASA with approximate SUM and SIMD-ASA with exact SUM. Bar labels
give the runtime relative to DuckDB.
