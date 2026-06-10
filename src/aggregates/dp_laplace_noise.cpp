#include "aggregates/dp_laplace_noise.hpp"
#include "aggregates/pac_aggregate.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace duckdb {

static uint64_t GetDpNoiseSeed(ClientContext &context) {
	Value seed_val;
	uint64_t seed = 42;
	if (context.TryGetCurrentSetting("privacy_seed", seed_val) && !seed_val.IsNull()) {
		seed = uint64_t(seed_val.GetValue<int64_t>());
	} else {
		seed ^= PAC_MAGIC_HASH * static_cast<uint64_t>(context.ActiveTransaction().GetActiveQuery());
	}
	return (seed * PAC_MAGIC_HASH) ^ PAC_MAGIC_HASH;
}

static double AddLaplaceNoise(double value, double scale, uint64_t seed, uint64_t nonce) {
	if (scale <= 0.0 || !std::isfinite(scale)) {
		return value;
	}
	std::mt19937_64 gen(seed ^ (PAC_MAGIC_HASH * nonce));
	std::uniform_real_distribution<double> uni(-0.5, 0.5);
	double u = uni(gen);
	double sign = (u < 0.0) ? -1.0 : 1.0;
	double noise = -scale * sign * std::log(std::max(1e-300, 1.0 - 2.0 * std::abs(u)));
	return value + noise;
}

static void DpLaplaceNoiseFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	uint64_t seed = GetDpNoiseSeed(context);
	auto count = args.size();

	if (args.ColumnCount() == 3) {
		TernaryExecutor::Execute<double, double, uint64_t, double>(
		    args.data[0], args.data[1], args.data[2], result, count,
		    [&](double value, double scale, uint64_t nonce) -> double {
			    return AddLaplaceNoise(value, scale, seed, nonce);
		    });
		return;
	}

	uint64_t row_nonce = 0;
	BinaryExecutor::Execute<double, double, double>(
	    args.data[0], args.data[1], result, count,
	    [&](double value, double scale) -> double { return AddLaplaceNoise(value, scale, seed, row_nonce++); });
}

void RegisterDpLaplaceNoiseFunction(ExtensionLoader &loader) {
	ScalarFunctionSet set("dp_noise");
	set.AddFunction(ScalarFunction("dp_noise", {LogicalType::DOUBLE, LogicalType::DOUBLE}, LogicalType::DOUBLE,
	                               DpLaplaceNoiseFunction));
	set.AddFunction(ScalarFunction("dp_noise", {LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::UBIGINT},
	                               LogicalType::DOUBLE, DpLaplaceNoiseFunction));
	CreateScalarFunctionInfo info(set);
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

static double MedianLocalSensitivity(const std::array<double, 64> &values, int changed_lanes) {
	const int median_idx = 31;
	int lo_idx = median_idx - changed_lanes;
	int hi_idx = median_idx + changed_lanes;
	D_ASSERT(lo_idx >= 0);
	D_ASSERT(hi_idx < static_cast<int>(values.size()));
	return std::max(0.0, values[hi_idx] - values[lo_idx]);
}

static double BoundedMedianValueAt(const std::array<double, 64> &values, int idx, double lower_bound,
                                   double upper_bound) {
	if (idx < 0) {
		return lower_bound;
	}
	if (idx >= static_cast<int>(values.size())) {
		return upper_bound;
	}
	return values[idx];
}

static double SmoothMedianSensitivity(const std::array<double, 64> &values, double beta, int sample_lanes) {
	const int step_size = sample_lanes;
	double smooth = 0.0;
	for (int distance = 0; distance < 64; distance++) {
		int changed_lanes = step_size * (distance + 1);
		if (changed_lanes >= 32) {
			break;
		}
		double local = MedianLocalSensitivity(values, changed_lanes);
		smooth = std::max(smooth, local * std::exp(-beta * static_cast<double>(distance)));
	}
	return smooth;
}

static double BoundedSmoothMedianSensitivity(const std::array<double, 64> &values, double beta, int sample_lanes,
                                             double lower_bound, double upper_bound) {
	const int median_idx = 31;
	const int step_size = sample_lanes;
	double smooth = 0.0;
	for (int distance = 0; distance < 64; distance++) {
		int changed_lanes = step_size * (distance + 1);
		int lo_idx = median_idx - changed_lanes;
		int hi_idx = median_idx + changed_lanes;
		double lo = BoundedMedianValueAt(values, lo_idx, lower_bound, upper_bound);
		double hi = BoundedMedianValueAt(values, hi_idx, lower_bound, upper_bound);
		double local = std::max(0.0, hi - lo);
		smooth = std::max(smooth, local * std::exp(-beta * static_cast<double>(distance)));
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
	bool bounded = args.ColumnCount() == 6;
	auto &list_vec = args.data[0];
	UnifiedVectorFormat list_data, epsilon_data, delta_data, sample_lanes_data, lower_data, upper_data;
	list_vec.ToUnifiedFormat(count, list_data);
	args.data[1].ToUnifiedFormat(count, epsilon_data);
	args.data[2].ToUnifiedFormat(count, delta_data);
	args.data[3].ToUnifiedFormat(count, sample_lanes_data);
	if (bounded) {
		args.data[4].ToUnifiedFormat(count, lower_data);
		args.data[5].ToUnifiedFormat(count, upper_data);
	}

	auto &child_vec = ListVector::GetEntry(list_vec);
	UnifiedVectorFormat child_data;
	child_vec.ToUnifiedFormat(ListVector::GetListSize(list_vec), child_data);
	auto child_values = UnifiedVectorFormat::GetData<PAC_FLOAT>(child_data);
	auto list_entries = UnifiedVectorFormat::GetData<list_entry_t>(list_data);
	auto epsilons = UnifiedVectorFormat::GetData<double>(epsilon_data);
	auto deltas = UnifiedVectorFormat::GetData<double>(delta_data);
	auto sample_lanes_values = UnifiedVectorFormat::GetData<int32_t>(sample_lanes_data);
	auto lower_values = bounded ? UnifiedVectorFormat::GetData<double>(lower_data) : nullptr;
	auto upper_values = bounded ? UnifiedVectorFormat::GetData<double>(upper_data) : nullptr;
	auto result_data = FlatVector::GetData<double>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto list_idx = list_data.sel->get_index(i);
		auto eps_idx = epsilon_data.sel->get_index(i);
		auto delta_idx = delta_data.sel->get_index(i);
		auto sample_lanes_idx = sample_lanes_data.sel->get_index(i);
		auto lower_idx = bounded ? lower_data.sel->get_index(i) : 0;
		auto upper_idx = bounded ? upper_data.sel->get_index(i) : 0;
		if (!list_data.validity.RowIsValid(list_idx) || !epsilon_data.validity.RowIsValid(eps_idx) ||
		    !delta_data.validity.RowIsValid(delta_idx) || !sample_lanes_data.validity.RowIsValid(sample_lanes_idx) ||
		    (bounded && (!lower_data.validity.RowIsValid(lower_idx) || !upper_data.validity.RowIsValid(upper_idx)))) {
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
		int sample_lanes = ValidateDpSampleLanes(sample_lanes_values[sample_lanes_idx]);
		double lower_bound = bounded ? lower_values[lower_idx] : 0.0;
		double upper_bound = bounded ? upper_values[upper_idx] : 0.0;
		if (epsilon <= 0.0 || delta <= 0.0 || delta >= 0.5 || !std::isfinite(epsilon) || !std::isfinite(delta)) {
			result_validity.SetInvalid(i);
			continue;
		}
		if (bounded && (!std::isfinite(lower_bound) || !std::isfinite(upper_bound) || lower_bound >= upper_bound)) {
			result_validity.SetInvalid(i);
			continue;
		}
		std::array<double, 64> values;
		idx_t valid_count = 0;
		for (idx_t j = 0; j < 64; j++) {
			auto child_idx = child_data.sel->get_index(entry.offset + j);
			if (child_data.validity.RowIsValid(child_idx)) {
				double value = static_cast<double>(child_values[child_idx]);
				if (bounded) {
					value = std::max(lower_bound, std::min(upper_bound, value));
				}
				values[valid_count++] = value;
			}
		}
		if (valid_count < 32) {
			result_validity.SetInvalid(i);
			continue;
		}
		std::sort(values.begin(), values.begin() + valid_count);
		for (idx_t j = valid_count; j < values.size(); j++) {
			values[j] = values[valid_count - 1];
		}
		double median = values[31];
		if (!noise_enabled) {
			result_data[i] = median;
			continue;
		}
		double beta = epsilon / (2.0 * std::log(2.0 / delta));
		double smooth = bounded ? BoundedSmoothMedianSensitivity(values, beta, sample_lanes, lower_bound, upper_bound)
		                        : SmoothMedianSensitivity(values, beta, sample_lanes);
		result_data[i] = median + LaplaceNoise((2.0 * smooth) / epsilon, seed ^ (entry.offset * PAC_MAGIC_HASH));
	}
}

void RegisterDpSmoothMedianNoiseFunction(ExtensionLoader &loader) {
	auto list_type = LogicalType::LIST(PacFloatLogicalType());
	ScalarFunctionSet set("dp_smooth_median_noise");
	set.AddFunction(ScalarFunction("dp_smooth_median_noise",
	                               {list_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER},
	                               LogicalType::DOUBLE, DpSmoothMedianNoiseFunction));
	set.AddFunction(ScalarFunction("dp_smooth_median_noise",
	                               {list_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER,
	                                LogicalType::DOUBLE, LogicalType::DOUBLE},
	                               LogicalType::DOUBLE, DpSmoothMedianNoiseFunction));
	CreateScalarFunctionInfo info(set);
	FunctionDescription desc;
	desc.description = "Applies smooth-sensitivity median release to 64 sample counters.";
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace duckdb
