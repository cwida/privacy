#pragma once

namespace duckdb {

// Analytical benchmarks run with stochastic aggregate state export on, so that DuckDB 2.0 can spill aggregate states
// under memory pressure (TPC-DS Q78 needs this). The extension default is off, and the aggregation microbenchmarks
// in benchmark/microbench keep that default.
static const char *const ANALYTICAL_STATE_EXPORT_SQL = "SET priv_state_export=true";

} // namespace duckdb
