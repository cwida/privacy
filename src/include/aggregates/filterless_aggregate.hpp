#pragma once

#include "duckdb.hpp"

namespace duckdb {

class ExtensionLoader;

// Bit 0 is reserved for the in-filter marker, leaving 63 hash bits available.
constexpr int FILTERLESS_MAX_SAMPLE_BITS = 63;

struct FilterlessSettings {
	int sample_bits;
	double sample_weight;
	double clip_support;
	bool noise_bounds;
	double bounds_epsilon_fraction;
};

void ValidateFilterlessSampleBits(int64_t sample_bits);
void ValidateFilterlessClipSupport(double clip_support);
void ValidateFilterlessBoundsEpsilonFraction(double fraction);
FilterlessSettings GetFilterlessSettings(ClientContext &context);

void RegisterFilterlessAggregateFunctions(ExtensionLoader &loader);

} // namespace duckdb
