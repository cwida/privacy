# TPC-DS coverage and performance

The benchmark submits all 99 original DuckDB TPC-DS queries to the automatic rewriter and times the available
handwritten AS queries. It reports four modes: ordinary DuckDB, handwritten SIMD-ASA, automatically compiled
SIMD-ASA, and repeated sampled-world execution. The handwritten queries and the separate per-world references
are in `benchmark/tpcds/as_queries/` and `benchmark/tpcds/reference_queries/`.

```bash
cmake --build build/release --target as_tpcds_benchmark
python3 benchmark/tpcds/run.py --config benchmark/configs/sanity/as_tpcds_smoke.json
python3 benchmark/tpcds/run.py --config benchmark/configs/utility/as_tpcds_sf30.json
```

Run from the repository root. The C++ runner loads DuckDB's version-matched TPC-DS extension, generates data with
`dsdgen`, and exports all 99 original SQL files into the configured output directory. `extension_directory` controls
where the generator extension is installed; choose a writable directory. Use a fresh database/output directory for
each scale factor or build. `--queries 19,52` runs a subset; `--resume` reuses completed process records only when
the configuration, runner, schema, and relevant SQL inputs still match.

## Sampling model

`CUSTOMER.c_customer_sk` is the sampling entity. The six sales/returns tables inherit membership through the
customer keys declared in `benchmark/tpcds/schema.sql`: bill customer for catalog/web sales, returning customer for
catalog/web returns, and the direct customer key for store sales/returns. Other dimensions remain shared. These
choices define the experiment's ownership semantics; billing and shipping customers are not treated as independent
sampling entities. Fact rows with no matching owner are absent from sampled worlds. The original query text is retained.

## Timing and coverage

The experiment uses 64 worlds, `privacy_seed=42`, `pac_mi=0`, and `pac_sample_diversity_check=false`.
`privacy_noise=true` retains the automatic compilation path while `pac_mi=0` suppresses additive noise. Compatibility
checking remains conservative, with `priv_check=false` allowing this AS benchmark to use protected columns.

Each mode receives one untimed warmup and five measured executions. C++ times planning, execution, and result
materialization using a steady clock. Preparation, EXPLAIN, process startup, and Python orchestration are outside
the timed interval. The handwritten and compiled paths return scalar/categorical outputs, as in TPC-H. Naive-AS
prepares ordinary SQL over customer-sampled views, times one world, and reports its runtime multiplied by 64.
The CSV retains this multiplier explicitly.

Each original query runs in its own process so a crash or timeout does not prevent later queries from running.
Raw CSV rows are flushed after every measurement; `summary.csv` reports medians only after all configured runs
complete. It retains execution status, stochastic-operator detection, empty results, and error text. Physical plans
and process logs are saved per query. A completed timing is labelled `executed_not_validated`; correctness is
checked separately.

## Validation

Use the same configuration with one of these mutually exclusive modes:

- `--validate` compares handwritten scalar/categorical SQL with the automatically compiled original query.
- `--validate-worlds` compares the separate per-world AS references with independent sampled-world SQL and with
  conditional-aggregate references.
- `--validate-compiled` compares the compiled output with ordinary world-zero SQL as a semantics diagnostic.
  These outputs need not agree when categorical filters use majority decisions, so this is not a compiler-equivalence test.

Validation summaries record exact and floating-point mismatches and maximum absolute/relative error. Missing
handwritten queries, unsupported constructs, compilation/runtime errors, timeouts, and absent stochastic operators
are reported explicitly. `q17_fixture.sql` supplies a small nonempty correctness fixture for Q17; it is not a
performance dataset. Small-scale or empty results remain preliminary diagnostics rather than paper-scale evidence.
