//
// Created by ila on 02/12/26.
//

#ifndef PAC_CLICKHOUSE_BENCHMARK_HPP
#define PAC_CLICKHOUSE_BENCHMARK_HPP

#include <string>
#include <vector>
#include <set>
#include "duckdb.hpp"

namespace duckdb {

// Run the ClickHouse Hits dataset benchmark (ClickBench).
// Uses fork-based process isolation: the parent never opens DuckDB for
// benchmarking; a child process handles all DB access. If the child is
// killed (OOM, crash), the parent survives and spawns a new one.
//
// Parameters:
// - db_path: path to DuckDB database file (default "clickbench.db")
// - queries_dir: directory containing create.sql, load.sql, queries.sql, setup.sql
//                (default "benchmark/clickbench/clickbench_queries")
// - out_csv: output CSV path (if empty, auto-named)
// - micro: if true, use a smaller subset for quick testing
// - run_naive: if true, also run the explicit sample-table-join "naive" variants for the
//              sampling mechanisms (pac -> clickbench_naive_pac_queries, dp_sass ->
//              clickbench_naive_dp_queries) that are present in --modes
// - modes: which privacy mechanisms to measure (subset of {pac,dp_standard,dp_elastic,dp_sass});
//          baseline always runs regardless
// - query_filter: optional 1-based ClickBench query ids to run; empty means all
// - bound_multipliers: optional sensitivity-bound multipliers; empty means {1.0}
// - sass_releases: optional dp_sass release methods to run; empty means {"median"}
//
// Returns 0 on success, non-zero on error.
int RunClickHouseBenchmark(const string &db_path = "clickbench.db",
                           const string &queries_dir = "benchmark/clickbench/clickbench_queries",
                           const string &out_csv = "",
                           bool micro = false,
                           bool run_naive = false,
                           const std::set<string> &modes = {"pac", "dp_standard", "dp_elastic", "dp_sass"},
                           const std::set<int> &query_filter = {},
                           const vector<double> &bound_multipliers = {1.0},
                           const vector<string> &sass_releases = {"median"});

} // namespace duckdb

#endif // PAC_CLICKHOUSE_BENCHMARK_HPP
