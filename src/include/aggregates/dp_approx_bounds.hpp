#pragma once

#include "duckdb.hpp"

namespace duckdb {

class ExtensionLoader;

void RegisterDpApproxBoundsAggregateFunctions(ExtensionLoader &loader);

} // namespace duckdb
