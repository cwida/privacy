#pragma once

#include "duckdb.hpp"

namespace duckdb {

class ExtensionLoader;

// Registers `priv_laplace_noise(value DOUBLE, scale DOUBLE) -> DOUBLE`.
// Returns value + Lap(scale) (location 0, scale = scale). Deterministic when `privacy_seed` is set.
void RegisterDpLaplaceNoiseFunction(ExtensionLoader &loader);

// Registers `priv_smooth_median_noise(counters, epsilon, delta, sample_lanes) -> DOUBLE`.
// The counters are already scaled to full-population estimates.
void RegisterDpSmoothMedianNoiseFunction(ExtensionLoader &loader);

} // namespace duckdb
