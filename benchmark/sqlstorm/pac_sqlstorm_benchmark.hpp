//
// Created by ila on 02/18/26.
//

#ifndef PAC_SQLSTORM_BENCHMARK_HPP
#define PAC_SQLSTORM_BENCHMARK_HPP

#include <string>
#include <vector>
#include "duckdb.hpp"

namespace duckdb {

// Run the SQLStorm benchmark with three passes:
// 1. Clear PAC metadata, run all queries (baseline mode, no PAC)
// 2. Load PAC schema, run all queries again (PAC mode)
// 3. Run dp_elastic with refreshed DP stats
// 4. Print statistics for all modes
// PAC metadata is NOT cleared at the end.
//
// Parameters:
// - queries_dir: directory containing SQLStorm .sql query files
// - out_csv: output CSV path (if empty, auto-named)
// - timeout_s: per-query timeout in seconds
// - tpch_sf: TPC-H scale factor (e.g. 1.00, 0.01)
// - benchmark: which benchmark to run ("tpch", "stackoverflow", or "both")
// - dp_epsilon: StackOverflow dp_elastic epsilon
// - dp_sum_bound: StackOverflow dp_elastic SUM/AVG input bound
//
// Returns 0 on success, non-zero on error.
int RunSQLStormBenchmark(const string &queries_dir = "",
                         const string &out_csv = "",
                         double timeout_s = 10.0,
                         double tpch_sf = 1.0,
                         const string &benchmark = "both",
                         double dp_epsilon = 1.0,
                         double dp_sum_bound = 1000000.0);

} // namespace duckdb

#endif // PAC_SQLSTORM_BENCHMARK_HPP
