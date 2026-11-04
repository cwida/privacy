#pragma once

#include "duckdb/function/aggregate_function.hpp"

namespace duckdb {

void RegisterSumStateExport(AggregateFunction &function, bool double_state);

} // namespace duckdb
