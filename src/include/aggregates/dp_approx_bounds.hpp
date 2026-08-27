#pragma once

#include "duckdb.hpp"

namespace duckdb {

class ExtensionLoader;

void ValidateDpApproxBoundsEpsilonFraction(double fraction);
double GetDpApproxBoundsEpsilonFraction(ClientContext &context);
void RegisterDpApproxBoundsAggregateFunctions(ExtensionLoader &loader);

} // namespace duckdb
