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
	Value noise_scale;
	Value numerator_noise_scale;
	Value denominator_noise_scale;
	Value valid_lane_count;

	static DpSassNoiseScaleQueryRecord Additive(int32_t aggregate_index, double noise_scale);
	static DpSassNoiseScaleQueryRecord Ratio(int32_t aggregate_index, double numerator_noise_scale,
	                                         double denominator_noise_scale, int32_t valid_lane_count);
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
