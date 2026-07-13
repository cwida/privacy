#pragma once

#include "duckdb.hpp"

namespace duckdb {

class ExtensionLoader;

struct DpSassStabilityQueryRecord {
	int32_t aggregate_index;
	vector<Value> stats;
};

struct DpSassNoiseScaleQueryRecord {
	int32_t aggregate_index;
	double noise_scale;
};

void ClearDpSassStabilityQueryRecords();
vector<DpSassStabilityQueryRecord> TakeDpSassStabilityQueryRecords();
void ClearDpSassNoiseScaleQueryRecords();
vector<DpSassNoiseScaleQueryRecord> TakeDpSassNoiseScaleQueryRecords();

// Registers `dp_noise(value DOUBLE, scale DOUBLE) -> DOUBLE`.
// Returns value + Lap(scale) (location 0, scale = scale). Deterministic when `privacy_seed` is set.
void RegisterDpLaplaceNoiseFunction(ExtensionLoader &loader);

// Registers `dp_smooth_median_noise(counters, epsilon, delta, sample_lanes) -> DOUBLE`.
// The counters are already scaled to full-population estimates.
void RegisterDpSmoothMedianNoiseFunction(ExtensionLoader &loader);

} // namespace duckdb
