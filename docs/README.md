# Privacy Extension Documentation

Detailed documentation for the `privacy` DuckDB extension. The docs are grouped by
layer: the shared SQL syntax; the mode-agnostic SIMD aggregation engine (`simd-asa/`);
the PAC privacy layer (`pac/`); and the differential-privacy layer (`dp/`).

## Contents

### General

| Document                                             | Description |
|------------------------------------------------------|-------------|
| [syntax.md](syntax.md)                               | SQL syntax / DDL (`CREATE PU TABLE`, `PRIVACY_KEY`, `PRIVACY_LINK`, `PROTECTED`) — shared by all modes |
| [build/README.md](build/README.md)                   | Build instructions |
| [test/README.md](test/README.md)                     | Running SQL and C++ tests |

### SIMD-ASA engine (mode-agnostic core)

| Document                                             | Description |
|------------------------------------------------------|-------------|
| [simd-asa/aggregates.md](simd-asa/aggregates.md)     | The 64-counter SIMD stochastic aggregates (COUNT/SUM/AVG/MIN/MAX), SWAR, clip & sample variants |

### PAC privacy layer

| Document                                             | Description |
|------------------------------------------------------|-------------|
| [pac/query_operators.md](pac/query_operators.md)     | Query rewriting / compilation pipeline |
| [pac/internal_operators.md](pac/internal_operators.md) | Internal operator transformations (Top-K pushdown, etc.) |
| [pac/runtime_checks.md](pac/runtime_checks.md)       | Runtime safety checks |
| [pac/ptracking.md](pac/ptracking.md)                 | Persistent secret p-tracking |
| [pac/utility.md](pac/utility.md)                     | Utility diff for accuracy measurement |

### Differential privacy layer

| Document                                             | Description |
|------------------------------------------------------|-------------|
| [dp/dp_mechanisms.md](dp/dp_mechanisms.md)           | Implementation reference for all three DP modes (`dp_standard`, `dp_elastic`, `dp_sass`) |
| [dp/dp_sass_smooth_sensitivity.md](dp/dp_sass_smooth_sensitivity.md) | `dp_sass` smooth-sensitivity deep-dive |

### Benchmarks

| Document                                             | Description |
|------------------------------------------------------|-------------|
| [benchmark/README.md](benchmark/README.md)           | Benchmark overview |
| [benchmark/tpch.md](benchmark/tpch.md)               | TPC-H benchmark instructions |
| [benchmark/imdb.md](benchmark/imdb.md)               | IMDB / JOB benchmark instructions |
| [benchmark/microbenchmarks.md](benchmark/microbenchmarks.md) | Microbenchmark suite |
| [benchmark/clickbench.md](benchmark/clickbench.md)   | ClickBench benchmark |
| [benchmark/sqlstorm.md](benchmark/sqlstorm.md)       | SQLStorm benchmark |
| [benchmark/utility.md](benchmark/utility.md)         | DP utility metrics and diagnostics |
| [repro.md](../repro.md)                              | Paper reproduction instructions |

For the main documentation (syntax, examples, configuration), see the [README.md](../README.md) in the repository root.
