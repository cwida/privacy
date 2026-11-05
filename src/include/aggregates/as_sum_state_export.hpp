#pragma once

#include "duckdb/function/aggregate_function.hpp"

namespace duckdb {

void RegisterSumStateExport(AggregateFunction &function);

} // namespace duckdb
