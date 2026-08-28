#pragma once

#include "duckdb.hpp"

namespace duckdb {

class ExtensionLoader;

struct DpApproxBoundsParameters {
	double scale;
	idx_t num_bins;
};

// Chooses the geometric histogram used for the SQL input type. Integral and decimal values use
// their natural unit; floating-point values cover the complete finite DOUBLE domain.
DpApproxBoundsParameters GetDpApproxBoundsParameters(const LogicalType &type);

void RegisterDpApproxBoundsAggregateFunctions(ExtensionLoader &loader);

} // namespace duckdb
