#include "aggregates/dp_laplace_noise.hpp"
#include "aggregates/pac_aggregate.hpp"
#include "utils/privacy_helpers.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/function/scalar_function.hpp"

#include <cmath>
#include <random>
#include <algorithm>

namespace duckdb {

static constexpr double DP_SAMPLE_MEDIAN_COUNTERS = 64.0;
static constexpr double PAC_COUNTER_LIST_SCALE = 2.0;

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

static double ComputeMedianSmoothSensitivity(vector<double> values, double beta, int affected, double lower,
                                             double upper) {
	std::sort(values.begin(), values.end());
	const idx_t n = values.size();
	if (n == 0) {
		return 0.0;
	}
	const idx_t median_idx = (n - 1) / 2;
	double diameter = std::max(0.0, upper - lower);
	double smooth = 0.0;
	for (idx_t k = 0; k <= n; k++) {
		idx_t moved = static_cast<idx_t>(affected) * (k + 1);
		double local = diameter;
		if (moved < n && median_idx >= moved && median_idx + moved < n) {
			local = values[median_idx + moved] - values[median_idx - moved];
		}
		double term = local * std::exp(-beta * static_cast<double>(k));
		if (term > smooth) {
			smooth = term;
		}
		if (std::exp(-beta * static_cast<double>(k)) * diameter < 1e-15) {
			break;
		}
	}
	return smooth;
}

static void DpSmoothMedianNoiseFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	idx_t count = args.size();

	UnifiedVectorFormat list_data;
	args.data[0].ToUnifiedFormat(count, list_data);
	auto list_entries = UnifiedVectorFormat::GetData<list_entry_t>(list_data);

	auto &child_vec = ListVector::GetEntry(args.data[0]);
	UnifiedVectorFormat child_data;
	child_vec.ToUnifiedFormat(ListVector::GetListSize(args.data[0]), child_data);
	auto child_values = UnifiedVectorFormat::GetData<PAC_FLOAT>(child_data);

	UnifiedVectorFormat epsilon_data;
	UnifiedVectorFormat delta_data;
	UnifiedVectorFormat affected_data;
	UnifiedVectorFormat lower_data;
	UnifiedVectorFormat upper_data;
	args.data[1].ToUnifiedFormat(count, epsilon_data);
	args.data[2].ToUnifiedFormat(count, delta_data);
	args.data[3].ToUnifiedFormat(count, affected_data);
	args.data[4].ToUnifiedFormat(count, lower_data);
	args.data[5].ToUnifiedFormat(count, upper_data);

	auto epsilons = UnifiedVectorFormat::GetData<double>(epsilon_data);
	auto deltas = UnifiedVectorFormat::GetData<double>(delta_data);
	auto affecteds = UnifiedVectorFormat::GetData<int32_t>(affected_data);
	auto lowers = UnifiedVectorFormat::GetData<double>(lower_data);
	auto uppers = UnifiedVectorFormat::GetData<double>(upper_data);

	auto result_data = FlatVector::GetData<double>(result);
	auto &result_validity = FlatVector::Validity(result);

	Value seed_val;
	uint64_t seed = (context.TryGetCurrentSetting("privacy_seed", seed_val) && !seed_val.IsNull())
	                    ? uint64_t(seed_val.GetValue<int64_t>())
	                    : uint64_t(std::random_device {}());
	if (seed_val.IsNull()) {
		seed ^= PAC_MAGIC_HASH * static_cast<uint64_t>(context.ActiveTransaction().GetActiveQuery());
	}
	bool noise_enabled = IsPacNoiseEnabled(context, true);

	for (idx_t i = 0; i < count; i++) {
		idx_t list_idx = list_data.sel->get_index(i);
		if (!list_data.validity.RowIsValid(list_idx)) {
			result_validity.SetInvalid(i);
			continue;
		}

		idx_t epsilon_idx = epsilon_data.sel->get_index(i);
		idx_t delta_idx = delta_data.sel->get_index(i);
		idx_t affected_idx = affected_data.sel->get_index(i);
		idx_t lower_idx = lower_data.sel->get_index(i);
		idx_t upper_idx = upper_data.sel->get_index(i);
		if (!epsilon_data.validity.RowIsValid(epsilon_idx) || !delta_data.validity.RowIsValid(delta_idx) ||
		    !affected_data.validity.RowIsValid(affected_idx) || !lower_data.validity.RowIsValid(lower_idx) ||
		    !upper_data.validity.RowIsValid(upper_idx)) {
			result_validity.SetInvalid(i);
			continue;
		}

		double epsilon = epsilons[epsilon_idx];
		double delta = deltas[delta_idx];
		int affected = affecteds[affected_idx];
		double lower = lowers[lower_idx];
		double upper = uppers[upper_idx];
		if (epsilon <= 0.0 || delta <= 0.0 || delta >= 0.5 || affected <= 0 || lower > upper) {
			result_validity.SetInvalid(i);
			continue;
		}

		auto &entry = list_entries[list_idx];
		if (entry.length == 0) {
			result_validity.SetInvalid(i);
			continue;
		}

		vector<double> values;
		values.reserve(entry.length);
		// PAC counter lists are already multiplied by PAC_COUNTER_LIST_SCALE to compensate
		// for the normal 50% PAC sampling path, so only apply the remaining correction here.
		double scale = (DP_SAMPLE_MEDIAN_COUNTERS / PAC_COUNTER_LIST_SCALE) / static_cast<double>(affected);
		for (idx_t j = 0; j < entry.length; j++) {
			idx_t child_idx = child_data.sel->get_index(entry.offset + j);
			if (!child_data.validity.RowIsValid(child_idx)) {
				continue;
			}
			double value = static_cast<double>(child_values[child_idx]) * scale;
			values.push_back(std::min(std::max(value, lower), upper));
		}
		if (values.empty()) {
			result_validity.SetInvalid(i);
			continue;
		}

		std::sort(values.begin(), values.end());
		double median = values[(values.size() - 1) / 2];
		if (!noise_enabled) {
			result_data[i] = median;
			continue;
		}

		double beta = epsilon / (2.0 * std::log(2.0 / delta));
		double smooth = ComputeMedianSmoothSensitivity(values, beta, affected, lower, upper);
		double laplace_scale = 2.0 * smooth / epsilon;
		if (laplace_scale <= 0.0 || !std::isfinite(laplace_scale)) {
			result_data[i] = median;
			continue;
		}
		std::mt19937_64 gen(seed ^ (PAC_MAGIC_HASH * static_cast<uint64_t>(std::hash<double> {}(median) ^
		                                                                   std::hash<double> {}(laplace_scale))));
		std::uniform_real_distribution<double> uni(-0.5, 0.5);
		double u = uni(gen);
		double sign = (u < 0.0) ? -1.0 : 1.0;
		double noise = -laplace_scale * sign * std::log(std::max(1e-300, 1.0 - 2.0 * std::abs(u)));
		result_data[i] = median + noise;
	}
}

void RegisterDpLaplaceNoiseFunction(ExtensionLoader &loader) {
	ScalarFunction fn("dp_laplace_noise", {LogicalType::DOUBLE, LogicalType::DOUBLE}, LogicalType::DOUBLE,
	                  DpLaplaceNoiseFunction);
	CreateScalarFunctionInfo info(fn);
	FunctionDescription desc;
	desc.description = "Adds Laplace(0, scale) noise to a value for elastic-sensitivity DP.";
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));

	auto list_type = LogicalType::LIST(PacFloatLogicalType());
	ScalarFunction median_fn("dp_smooth_median_noise",
	                         {list_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER,
	                          LogicalType::DOUBLE, LogicalType::DOUBLE},
	                         LogicalType::DOUBLE, DpSmoothMedianNoiseFunction);
	CreateScalarFunctionInfo median_info(median_fn);
	FunctionDescription median_desc;
	median_desc.description = "[INTERNAL] Releases a 64-counter sample median with smooth-sensitivity DP noise.";
	median_info.descriptions.push_back(std::move(median_desc));
	loader.RegisterFunction(std::move(median_info));
}

} // namespace duckdb
