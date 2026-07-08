//
// Created by ila on 07/07/26.
//

#ifndef AS_CLICKBENCH_BENCHMARK_HPP
#define AS_CLICKBENCH_BENCHMARK_HPP

#include <set>
#include <string>
#include "duckdb.hpp"

namespace duckdb {

int RunASClickBenchBenchmark(const string &db_path = "clickbench.db",
                             const string &queries_dir = "benchmark/clickbench/clickbench_queries",
                             const string &out_csv = "", bool micro = false, const std::set<int> &query_filter = {},
                             int threads = 8, const string &memory_limit = "");

} // namespace duckdb

#endif // AS_CLICKBENCH_BENCHMARK_HPP
