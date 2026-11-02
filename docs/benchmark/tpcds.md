# TPC-DS coverage and performance

This benchmark submits all 99 original DuckDB TPC-DS queries to the automatic rewriter. Unlike the handwritten AS SQL
timed by the TPC-H and ClickBench runners, it measures the compiler's current coverage as well as execution time.
It does not rewrite query text to remove unsupported constructs. No membership masks or ownership joins are
prematerialized.

```bash
cmake --build build/release --target as_tpcds_benchmark
python3 benchmark/tpcds/run.py --config benchmark/configs/sanity/as_tpcds_smoke.json
python3 benchmark/tpcds/run.py --config benchmark/configs/utility/as_tpcds_sf1.json
```

Run from the repository root. The C++ runner loads DuckDB's version-matched TPC-DS extension, generates data with
`dsdgen`, and exports all 99 original SQL files into the configured output directory. `extension_directory` controls
where the generator extension is installed; choose a writable directory. Use a fresh database/output directory for
each scale factor or build. `--queries 19,52` runs a subset; `--resume` reuses completed per-query process records.

## Sampling model

`CUSTOMER.c_customer_sk` is the sampling entity. The six sales/returns tables inherit membership through the
customer keys declared in `benchmark/tpcds/schema.sql`: bill customer for catalog/web sales, returning customer for
catalog/web returns, and the direct customer key for store sales/returns. Other dimensions remain shared. These
choices define the experiment's ownership semantics; billing and shipping customers are not treated as independent
sampling entities. Fact rows with no matching owner are absent from sampled worlds. Original SQL is not modified.

## Timing and coverage

Each query runs plain DuckDB and the automatic SIMD-ASA rewrite at 64 worlds. Noise and the sample-diversity check
are disabled (`pac_mi=0`, `pac_sample_diversity_check=false`), the seed is 42, and conservative compatibility checking
is enabled. `privacy_noise=true` is retained because setting it false bypasses PAC compilation in this version.
The noised/scalar output path is
used with noise disabled; this is not a measurement of releasing all 64 results to the client. Each mode receives
one untimed warmup and five measured executions. C++ times planning, execution, and result materialization using a
steady clock; process startup, generation, metadata setup, EXPLAIN, and Python orchestration are excluded.

Each original query runs in its own process. A crash or whole-query timeout does not prevent later queries from
running. Raw CSV rows are flushed after every measurement. The wrapper reports median runtime only after all
configured repetitions complete. `summary.csv` records baseline/rewritten status, whether EXPLAIN showed a stochastic
operator, empty results, and the original error message. Physical plans and process logs are retained per query.

An accepted execution is labelled `executed_not_validated`: it is not proof of per-world semantic correctness.
Unsupported queries, compilation/runtime failures, timeouts, and queries without a visible stochastic operator are
reported separately. Small-scale results may be empty and are preliminary coverage diagnostics, not paper-scale
performance evidence. Independent sampled-world validation must precede a correctness/coverage claim in the paper.
