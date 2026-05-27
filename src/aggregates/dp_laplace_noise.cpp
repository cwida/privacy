#include "aggregates/dp_laplace_noise.hpp"
#include "aggregates/pac_aggregate.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/function/scalar_function.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace duckdb {

static void DpLaplaceNoiseFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();

	Value seed_val;
	uint64_t seed = (context.TryGetCurrentSetting("privacy_seed", seed_val) && !seed_val.IsNull())
	                    ? uint64_t(seed_val.GetValue<int64_t>())
	                    : uint64_t(std::random_device {}());
	// If a seed was explicitly set, keep the run deterministic; otherwise vary per active query.
	if (!seed_val.IsNull()) {
		// keep deterministic
	} else {
		seed ^= PAC_MAGIC_HASH * static_cast<uint64_t>(context.ActiveTransaction().GetActiveQuery());
	}

	auto count = args.size();
	BinaryExecutor::Execute<double, double, double>(
	    args.data[0], args.data[1], result, count, [&](double value, double scale) -> double {
		    if (scale <= 0.0 || !std::isfinite(scale)) {
			    return value;
		    }
		    // Per-row seed mixing — stable across vectorized batches as long as inputs repeat.
		    std::mt19937_64 gen(seed ^ (PAC_MAGIC_HASH * static_cast<uint64_t>(std::hash<double> {}(value) ^
		                                                                       std::hash<double> {}(scale))));
		    std::uniform_real_distribution<double> uni(-0.5, 0.5);
		    double u = uni(gen);
		    // Inverse CDF of Laplace(0, scale): -scale * sgn(u) * ln(1 - 2|u|)
		    double sign = (u < 0.0) ? -1.0 : 1.0;
		    double noise = -scale * sign * std::log(std::max(1e-300, 1.0 - 2.0 * std::abs(u)));
		    return value + noise;
	    });
}

void RegisterDpLaplaceNoiseFunction(ExtensionLoader &loader) {
	ScalarFunction fn("dp_laplace_noise", {LogicalType::DOUBLE, LogicalType::DOUBLE}, LogicalType::DOUBLE,
	                  DpLaplaceNoiseFunction);
	CreateScalarFunctionInfo info(fn);
	FunctionDescription desc;
	desc.description = "Adds Laplace(0, scale) noise to a value for elastic-sensitivity DP.";
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

static double LaplaceNoise(double scale, uint64_t seed) {
	if (scale <= 0.0 || !std::isfinite(scale)) {
		return 0.0;
	}
	std::mt19937_64 gen(seed);
	std::uniform_real_distribution<double> uni(-0.5, 0.5);
	double u = uni(gen);
	double sign = (u < 0.0) ? -1.0 : 1.0;
	return -scale * sign * std::log(std::max(1e-300, 1.0 - 2.0 * std::abs(u)));
}

static double SmoothMedianSensitivity(const std::array<double, 64> &values, double lower, double upper, int affected,
                                      double beta) {
	int median_idx = 31;
	double smooth = 0.0;
	for (int k = 0; k <= affected; k++) {
		int lo_idx = median_idx - k;
		int hi_idx = median_idx + 1 + k;
		double lo = lo_idx >= 0 ? values[lo_idx] : lower;
		double hi = hi_idx < static_cast<int>(values.size()) ? values[hi_idx] : upper;
		double local = std::max(0.0, hi - lo);
		smooth = std::max(smooth, local * std::exp(-beta * static_cast<double>(k)));
	}
	return smooth;
}

static void DpSmoothMedianNoiseFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	Value seed_val;
	uint64_t seed = (context.TryGetCurrentSetting("privacy_seed", seed_val) && !seed_val.IsNull())
	                    ? uint64_t(seed_val.GetValue<int64_t>())
	                    : uint64_t(std::random_device {}());
	if (seed_val.IsNull()) {
		seed ^= PAC_MAGIC_HASH * static_cast<uint64_t>(context.ActiveTransaction().GetActiveQuery());
	}
	bool noise_enabled = true;
	Value noise_val;
	if (context.TryGetCurrentSetting("privacy_noise", noise_val) && !noise_val.IsNull()) {
		noise_enabled = noise_val.GetValue<bool>();
	}

	idx_t count = args.size();
	auto &list_vec = args.data[0];
	UnifiedVectorFormat list_data, epsilon_data, delta_data, affected_data, lower_data, upper_data;
	list_vec.ToUnifiedFormat(count, list_data);
	args.data[1].ToUnifiedFormat(count, epsilon_data);
	args.data[2].ToUnifiedFormat(count, delta_data);
	args.data[3].ToUnifiedFormat(count, affected_data);
	args.data[4].ToUnifiedFormat(count, lower_data);
	args.data[5].ToUnifiedFormat(count, upper_data);

	auto &child_vec = ListVector::GetEntry(list_vec);
	UnifiedVectorFormat child_data;
	child_vec.ToUnifiedFormat(ListVector::GetListSize(list_vec), child_data);
	auto child_values = UnifiedVectorFormat::GetData<PAC_FLOAT>(child_data);
	auto list_entries = UnifiedVectorFormat::GetData<list_entry_t>(list_data);
	auto epsilons = UnifiedVectorFormat::GetData<double>(epsilon_data);
	auto deltas = UnifiedVectorFormat::GetData<double>(delta_data);
	auto affected_values = UnifiedVectorFormat::GetData<int32_t>(affected_data);
	auto lowers = UnifiedVectorFormat::GetData<double>(lower_data);
	auto uppers = UnifiedVectorFormat::GetData<double>(upper_data);
	auto result_data = FlatVector::GetData<double>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto list_idx = list_data.sel->get_index(i);
		auto eps_idx = epsilon_data.sel->get_index(i);
		auto delta_idx = delta_data.sel->get_index(i);
		auto affected_idx = affected_data.sel->get_index(i);
		auto lower_idx = lower_data.sel->get_index(i);
		auto upper_idx = upper_data.sel->get_index(i);
		if (!list_data.validity.RowIsValid(list_idx) || !epsilon_data.validity.RowIsValid(eps_idx) ||
		    !delta_data.validity.RowIsValid(delta_idx) || !affected_data.validity.RowIsValid(affected_idx) ||
		    !lower_data.validity.RowIsValid(lower_idx) || !upper_data.validity.RowIsValid(upper_idx)) {
			result_validity.SetInvalid(i);
			continue;
		}
		auto &entry = list_entries[list_idx];
		if (entry.length != 64) {
			result_validity.SetInvalid(i);
			continue;
		}
		double epsilon = epsilons[eps_idx];
		double delta = deltas[delta_idx];
		int affected = affected_values[affected_idx];
		double lower = lowers[lower_idx];
		double upper = uppers[upper_idx];
		if (epsilon <= 0.0 || delta <= 0.0 || delta >= 0.5 || affected <= 0 || !std::isfinite(epsilon) ||
		    !std::isfinite(delta) || !std::isfinite(lower) || !std::isfinite(upper) || lower > upper) {
			result_validity.SetInvalid(i);
			continue;
		}
		std::array<double, 64> values;
		for (idx_t j = 0; j < 64; j++) {
			auto child_idx = child_data.sel->get_index(entry.offset + j);
			double value =
			    child_data.validity.RowIsValid(child_idx) ? static_cast<double>(child_values[child_idx]) : 0.0;
			values[j] = std::min(std::max(value, lower), upper);
		}
		std::sort(values.begin(), values.end());
		double median = values[31];
		if (!noise_enabled) {
			result_data[i] = median;
			continue;
		}
		double beta = epsilon / (2.0 * std::log(2.0 / delta));
		double smooth = SmoothMedianSensitivity(values, lower, upper, std::min(affected, 63), beta);
		result_data[i] = median + LaplaceNoise((2.0 * smooth) / epsilon, seed ^ (entry.offset * PAC_MAGIC_HASH));
	}
}

void RegisterDpSmoothMedianNoiseFunction(ExtensionLoader &loader) {
	auto list_type = LogicalType::LIST(PacFloatLogicalType());
	ScalarFunction fn("dp_smooth_median_noise",
	                  {list_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::DOUBLE,
	                   LogicalType::DOUBLE},
	                  LogicalType::DOUBLE, DpSmoothMedianNoiseFunction);
	CreateScalarFunctionInfo info(fn);
	FunctionDescription desc;
	desc.description = "Applies smooth-sensitivity median release to 64 sample counters.";
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace duckdb
