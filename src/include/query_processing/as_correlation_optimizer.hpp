#pragma once

namespace duckdb {
class DatabaseInstance;

void RegisterASCorrelationOptimizer(DatabaseInstance &db);

} // namespace duckdb
