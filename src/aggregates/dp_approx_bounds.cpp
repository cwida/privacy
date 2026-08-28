#include "aggregates/dp_approx_bounds.hpp"

#include "aggregates/as_clip_aggr.hpp"
#include "aggregates/as_clip_sum.hpp"
#include "aggregates/dp_laplace_noise.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"
#include "utils/privacy_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace duckdb {

// The largest histogram is the floating-point profile. Smaller numeric types retain the same
// geometric construction but allocate and noise only the bins needed by their public domain.
constexpr idx_t DP_APPROX_BOUNDS_MAX_BINS = 2047;
constexpr double DP_APPROX_BOUNDS_INITIAL_FAILURE_PROBABILITY = 1e-9;
constexpr double DP_APPROX_BOUNDS_MIN_SUCCESS_PROBABILITY = 1.0 - 1e-6;
constexpr int DP_APPROX_BOUNDS_MAX_ATTEMPTS = 30;
constexpr double DP_APPROX_BOUNDS_EPSILON_FRACTION = 0.5;

struct DpApproxBoundsBin {
	long double magnitude_sum;
	uint64_t count;
};

struct DpApproxBoundsState {
	DpApproxBoundsBin *positive;
	DpApproxBoundsBin *negative;
};

struct DpApproxBoundsBindData : public FunctionData {
	double epsilon;
	double max_groups;
	double max_contributions;
	double scale;
	idx_t num_bins;
	bool noise_enabled;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<DpApproxBoundsBindData>(*this);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto other = dynamic_cast<const DpApproxBoundsBindData *>(&other_p);
		return other && epsilon == other->epsilon && max_groups == other->max_groups &&
		       max_contributions == other->max_contributions && scale == other->scale && num_bins == other->num_bins &&
		       noise_enabled == other->noise_enabled;
	}
};

struct DpApproxBoundsResult {
	bool success;
	double lower_bound;
	double upper_bound;
	double clipped_value;
	double noise_scale;
	int32_t selected_bin;
	int32_t attempts;
	uint64_t contributions;
};

static idx_t BinaryBinsForMagnitude(double magnitude, double scale) {
	double bins = std::ceil((std::log(magnitude) - std::log(scale)) / std::log(2.0)) + 1.0;
	return static_cast<idx_t>(std::max(1.0, std::min(bins, static_cast<double>(DP_APPROX_BOUNDS_MAX_BINS))));
}

DpApproxBoundsParameters GetDpApproxBoundsParameters(const LogicalType &type) {
	if (type.id() == LogicalTypeId::DECIMAL) {
		auto width = DecimalType::GetWidth(type);
		auto decimal_scale = DecimalType::GetScale(type);
		double scale = std::pow(10.0, -static_cast<double>(decimal_scale));
		double maximum = std::pow(10.0, static_cast<double>(width - decimal_scale));
		return {scale, BinaryBinsForMagnitude(maximum, scale)};
	}
	// Integral ApproxBounds starts at one because there are no representable values between zero and
	// one. The bin count is based on the input type, not DuckDB's widened SUM return type.
	switch (type.InternalType()) {
	case PhysicalType::INT8:
		return {1.0, BinaryBinsForMagnitude(NumericLimits<int8_t>::Maximum(), 1.0)};
	case PhysicalType::INT16:
		return {1.0, BinaryBinsForMagnitude(NumericLimits<int16_t>::Maximum(), 1.0)};
	case PhysicalType::INT32:
		return {1.0, BinaryBinsForMagnitude(NumericLimits<int32_t>::Maximum(), 1.0)};
	case PhysicalType::INT64:
		return {1.0, BinaryBinsForMagnitude(static_cast<double>(NumericLimits<int64_t>::Maximum()), 1.0)};
	case PhysicalType::UINT8:
		return {1.0, BinaryBinsForMagnitude(NumericLimits<uint8_t>::Maximum(), 1.0)};
	case PhysicalType::UINT16:
		return {1.0, BinaryBinsForMagnitude(NumericLimits<uint16_t>::Maximum(), 1.0)};
	case PhysicalType::UINT32:
		return {1.0, BinaryBinsForMagnitude(NumericLimits<uint32_t>::Maximum(), 1.0)};
	case PhysicalType::UINT64:
		return {1.0, BinaryBinsForMagnitude(static_cast<double>(NumericLimits<uint64_t>::Maximum()), 1.0)};
	case PhysicalType::INT128:
		return {1.0, 128};
	case PhysicalType::UINT128:
		return {1.0, 129};
	default:
		break;
	}
	return {std::numeric_limits<double>::min(), DP_APPROX_BOUNDS_MAX_BINS};
}

static double EvaluateConfig(ClientContext &context, const Expression &expression, const string &function_name,
                             const string &name) {
	if (!expression.IsFoldable()) {
		throw InvalidInputException(function_name + ": " + name + " must be a constant");
	}
	return ExpressionExecutor::EvaluateScalar(context, expression).GetValue<double>();
}

static idx_t EvaluateBinCount(ClientContext &context, const Expression &expression, const string &function_name) {
	double value = EvaluateConfig(context, expression, function_name, "number of bins");
	if (!std::isfinite(value) || value < 1.0 || value > static_cast<double>(DP_APPROX_BOUNDS_MAX_BINS) ||
	    std::round(value) != value) {
		throw InvalidInputException(function_name + ": number of bins must be an integer between 1 and " +
		                            std::to_string(DP_APPROX_BOUNDS_MAX_BINS));
	}
	return static_cast<idx_t>(value);
}

static unique_ptr<FunctionData> BindDpApproxBounds(ClientContext &context, AggregateFunction &function,
                                                   vector<unique_ptr<Expression>> &arguments) {
	if (arguments.size() != 5 && arguments.size() != 6) {
		throw InternalException(function.name + ": invalid internal argument count");
	}
	double epsilon = EvaluateConfig(context, *arguments[1], function.name, "epsilon");
	double max_groups = EvaluateConfig(context, *arguments[2], function.name, "max groups");
	double max_contributions =
	    arguments.size() == 6 ? EvaluateConfig(context, *arguments[3], function.name, "max contributions") : 1.0;
	idx_t profile_offset = arguments.size() == 6 ? 4 : 3;
	double scale = EvaluateConfig(context, *arguments[profile_offset], function.name, "scale");
	idx_t num_bins = EvaluateBinCount(context, *arguments[profile_offset + 1], function.name);
	if (!std::isfinite(epsilon) || epsilon <= 0.0) {
		throw InvalidInputException(function.name + ": epsilon must be a positive finite number");
	}
	if (!std::isfinite(max_groups) || max_groups <= 0.0) {
		throw InvalidInputException(function.name + ": max groups must be a positive finite number");
	}
	if (!std::isfinite(max_contributions) || max_contributions <= 0.0) {
		throw InvalidInputException(function.name + ": max contributions must be a positive finite number");
	}
	if (arguments.size() == 6 && std::round(max_contributions) != max_contributions) {
		throw InvalidInputException(function.name + ": max contributions must be integer-valued");
	}
	if (!std::isfinite(scale) || scale <= 0.0) {
		throw InvalidInputException(function.name + ": scale must be a positive finite number");
	}
	auto result = make_uniq<DpApproxBoundsBindData>();
	result->epsilon = epsilon;
	result->max_groups = max_groups;
	result->max_contributions = max_contributions;
	result->scale = scale;
	result->num_bins = num_bins;
	result->noise_enabled = IsPacNoiseEnabled(context, true);
	return std::move(result);
}

static idx_t DpApproxBoundsStateSize(const AggregateFunction &) {
	return sizeof(DpApproxBoundsState);
}

static void DpApproxBoundsInitialize(const AggregateFunction &, data_ptr_t state_p) {
	memset(state_p, 0, sizeof(DpApproxBoundsState));
}

static DpApproxBoundsBin *EnsureBins(DpApproxBoundsBin *&bins, idx_t num_bins, ArenaAllocator &allocator) {
	if (!bins) {
		bins = reinterpret_cast<DpApproxBoundsBin *>(allocator.Allocate(sizeof(DpApproxBoundsBin) * num_bins));
		memset(bins, 0, sizeof(DpApproxBoundsBin) * num_bins);
	}
	return bins;
}

static double BinUpperBound(const DpApproxBoundsBindData &bind, idx_t index) {
	double bound = std::ldexp(bind.scale, static_cast<int>(index));
	if (!std::isfinite(bound)) {
		return std::numeric_limits<double>::max();
	}
	return bound;
}

static idx_t BinIndex(const DpApproxBoundsBindData &bind, double value) {
	double magnitude = std::abs(value);
	if (magnitude <= bind.scale) {
		return 0;
	}
	double raw = std::ceil(std::log2(magnitude) - std::log2(bind.scale));
	if (!std::isfinite(raw) || raw >= static_cast<double>(bind.num_bins - 1)) {
		return bind.num_bins - 1;
	}
	auto index = static_cast<idx_t>(std::max(0.0, raw));
	if (index > 0 && magnitude <= BinUpperBound(bind, index - 1)) {
		index--;
	}
	return index;
}

static void UpdateState(DpApproxBoundsState &state, const DpApproxBoundsBindData &bind, double value,
                        ArenaAllocator &allocator) {
	// NaNs are absent values. Infinite values are clamped to the largest finite DOUBLE before binning,
	// matching the numerical preprocessing used by bounded aggregates.
	if (std::isnan(value)) {
		return;
	}
	if (std::isinf(value)) {
		value = std::copysign(NumericLimits<double>::Maximum(), value);
	}
	auto index = BinIndex(bind, value);
	auto &bin = value < 0.0 ? EnsureBins(state.negative, bind.num_bins, allocator)[index]
	                        : EnsureBins(state.positive, bind.num_bins, allocator)[index];
	bin.magnitude_sum += static_cast<long double>(std::abs(value));
	bin.count++;
}

struct DpApproxBoundsInputs {
	UnifiedVectorFormat value;
	const double *values;

	DpApproxBoundsInputs(Vector inputs[], idx_t count) {
		inputs[0].ToUnifiedFormat(count, value);
		values = UnifiedVectorFormat::GetData<double>(value);
	}

	bool IsValid(idx_t row) const {
		return value.validity.RowIsValid(value.sel->get_index(row));
	}
};

template <class STATE_GETTER>
static void UpdateRows(Vector inputs[], AggregateInputData &input, idx_t count, STATE_GETTER get_state) {
	DpApproxBoundsInputs vectors(inputs, count);
	auto &bind = input.bind_data->Cast<DpApproxBoundsBindData>();
	for (idx_t row = 0; row < count; row++) {
		if (!vectors.IsValid(row)) {
			continue;
		}
		auto value_index = vectors.value.sel->get_index(row);
		UpdateState(*get_state(row), bind, vectors.values[value_index], input.allocator);
	}
}

static void DpApproxBoundsUpdate(Vector inputs[], AggregateInputData &input, idx_t, data_ptr_t state_p, idx_t count) {
	auto state = reinterpret_cast<DpApproxBoundsState *>(state_p);
	UpdateRows(inputs, input, count, [state](idx_t) { return state; });
}

static void DpApproxBoundsScatterUpdate(Vector inputs[], AggregateInputData &input, idx_t, Vector &states,
                                        idx_t count) {
	UnifiedVectorFormat state_data;
	states.ToUnifiedFormat(count, state_data);
	auto state_ptrs = UnifiedVectorFormat::GetData<DpApproxBoundsState *>(state_data);
	UpdateRows(inputs, input, count, [&](idx_t row) { return state_ptrs[state_data.sel->get_index(row)]; });
}

static void CombineBins(const DpApproxBoundsBin *source, DpApproxBoundsBin *&target, idx_t num_bins,
                        ArenaAllocator &allocator) {
	if (!source) {
		return;
	}
	auto target_bins = EnsureBins(target, num_bins, allocator);
	for (idx_t i = 0; i < num_bins; i++) {
		target_bins[i].magnitude_sum += source[i].magnitude_sum;
		target_bins[i].count += source[i].count;
	}
}

static void DpApproxBoundsCombine(Vector &source, Vector &target, AggregateInputData &input, idx_t count) {
	auto sources = FlatVector::GetData<DpApproxBoundsState *>(source);
	auto targets = FlatVector::GetData<DpApproxBoundsState *>(target);
	auto num_bins = input.bind_data->Cast<DpApproxBoundsBindData>().num_bins;
	for (idx_t i = 0; i < count; i++) {
		CombineBins(sources[i]->positive, targets[i]->positive, num_bins, input.allocator);
		CombineBins(sources[i]->negative, targets[i]->negative, num_bins, input.allocator);
	}
}

static double LaplaceThreshold(double success_probability, double scale, idx_t num_bins) {
	double log_success_per_bin = std::log(success_probability) / (2.0 * static_cast<double>(num_bins));
	double upper_tail_probability = -std::expm1(log_success_per_bin);
	return -scale * std::log(2.0 * upper_tail_probability);
}

static double BinLowerMagnitude(const DpApproxBoundsBindData &bind, idx_t index) {
	return index == 0 ? 0.0 : BinUpperBound(bind, index - 1);
}

static bool FindBounds(const DpApproxBoundsBindData &bind, const double *positive, const double *negative,
                       double threshold, DpApproxBoundsResult &result) {
	int32_t lower_bin = -1;
	int32_t upper_bin = -1;
	for (idx_t reverse = bind.num_bins; reverse > 0; reverse--) {
		idx_t i = reverse - 1;
		if (negative[i] >= threshold) {
			result.lower_bound = -BinUpperBound(bind, i);
			lower_bin = static_cast<int32_t>(i);
			break;
		}
	}
	if (lower_bin < 0) {
		for (idx_t i = 0; i < bind.num_bins; i++) {
			if (positive[i] >= threshold) {
				result.lower_bound = BinLowerMagnitude(bind, i);
				lower_bin = static_cast<int32_t>(i);
				break;
			}
		}
	}

	for (idx_t reverse = bind.num_bins; reverse > 0; reverse--) {
		idx_t i = reverse - 1;
		if (positive[i] >= threshold) {
			result.upper_bound = BinUpperBound(bind, i);
			upper_bin = static_cast<int32_t>(i);
			break;
		}
	}
	if (upper_bin < 0) {
		for (idx_t i = 0; i < bind.num_bins; i++) {
			if (negative[i] >= threshold) {
				result.upper_bound = -BinLowerMagnitude(bind, i);
				upper_bin = static_cast<int32_t>(i);
				break;
			}
		}
	}

	result.selected_bin = std::max(lower_bin, upper_bin);
	return lower_bin >= 0 && upper_bin >= 0;
}

static uint64_t CountContributions(const DpApproxBoundsState &state, idx_t num_bins) {
	uint64_t result = 0;
	for (idx_t i = 0; i < num_bins; i++) {
		if (state.positive) {
			result += state.positive[i].count;
		}
		if (state.negative) {
			result += state.negative[i].count;
		}
	}
	return result;
}

static long double ComputeClampedSum(const DpApproxBoundsState &state, const DpApproxBoundsBindData &bind, double lower,
                                     double upper) {
	long double result = 0.0;
	idx_t positive_lower_bin = lower > 0.0 ? BinIndex(bind, lower) : 0;
	idx_t positive_upper_bin = upper > 0.0 ? BinIndex(bind, upper) : 0;
	idx_t negative_lower_bin = lower < 0.0 ? BinIndex(bind, -lower) : 0;
	idx_t negative_upper_bin = upper < 0.0 ? BinIndex(bind, -upper) : 0;
	for (idx_t i = 0; i < bind.num_bins; i++) {
		if (state.positive) {
			auto &bin = state.positive[i];
			if (upper <= 0.0) {
				result += static_cast<long double>(bin.count) * upper;
			} else if (lower > 0.0 && i <= positive_lower_bin) {
				result += static_cast<long double>(bin.count) * lower;
			} else if (i > positive_upper_bin) {
				result += static_cast<long double>(bin.count) * upper;
			} else {
				result += bin.magnitude_sum;
			}
		}
		if (state.negative) {
			auto &bin = state.negative[i];
			if (lower >= 0.0) {
				result += static_cast<long double>(bin.count) * lower;
			} else if (i > negative_lower_bin) {
				result += static_cast<long double>(bin.count) * lower;
			} else if (upper < 0.0 && i <= negative_upper_bin) {
				result += static_cast<long double>(bin.count) * upper;
			} else {
				result -= bin.magnitude_sum;
			}
		}
	}
	return result;
}

static DpApproxBoundsResult FinalizeState(const DpApproxBoundsState &state, const DpApproxBoundsBindData &bind,
                                          bool relax_sum_bounds) {
	DpApproxBoundsResult result {};
	result.selected_bin = -1;
	result.contributions = CountContributions(state, bind.num_bins);
	if (result.contributions == 0) {
		return result;
	}

	double positive[DP_APPROX_BOUNDS_MAX_BINS];
	double negative[DP_APPROX_BOUNDS_MAX_BINS];
	for (idx_t i = 0; i < bind.num_bins; i++) {
		positive[i] = state.positive ? static_cast<double>(state.positive[i].count) : 0.0;
		negative[i] = state.negative ? static_cast<double>(state.negative[i].count) : 0.0;
	}

	bool found = false;
	if (!bind.noise_enabled) {
		result.attempts = 1;
		found = FindBounds(bind, positive, negative, 1.0, result);
	} else {
		double bounds_epsilon = bind.epsilon * DP_APPROX_BOUNDS_EPSILON_FRACTION;
		double histogram_scale = bind.max_groups * bind.max_contributions / bounds_epsilon;
		AddDpLaplaceNoiseBatch(positive, positive, bind.num_bins, histogram_scale);
		AddDpLaplaceNoiseBatch(negative, negative, bind.num_bins, histogram_scale);

		double success_probability = 1.0 - DP_APPROX_BOUNDS_INITIAL_FAILURE_PROBABILITY;
		for (int attempt = 1; attempt <= DP_APPROX_BOUNDS_MAX_ATTEMPTS; attempt++) {
			found = FindBounds(bind, positive, negative,
			                   LaplaceThreshold(success_probability, histogram_scale, bind.num_bins), result);
			result.attempts = attempt;
			if (found) {
				break;
			}
			double failure_probability = 1.0 - success_probability;
			success_probability = 1.0 - 10.0 * failure_probability;
			if (!(success_probability > DP_APPROX_BOUNDS_MIN_SUCCESS_PROBABILITY)) {
				break;
			}
		}
	}

	if (!found) {
		return result;
	}
	if (relax_sum_bounds) {
		// Bounded SUM sensitivity is governed by the larger magnitude. Expanding the other side to the
		// same magnitude therefore improves clipping utility without consuming additional privacy budget.
		if (result.lower_bound == 0.0 && result.upper_bound == 0.0) {
			result.lower_bound = -1.0;
			result.upper_bound = 1.0;
		} else {
			double magnitude = std::max(std::abs(result.lower_bound), std::abs(result.upper_bound));
			result.lower_bound = -magnitude;
			result.upper_bound = magnitude;
		}
	}
	result.success = true;
	result.clipped_value = static_cast<double>(ComputeClampedSum(state, bind, result.lower_bound, result.upper_bound));
	double value_epsilon = bind.epsilon * (1.0 - DP_APPROX_BOUNDS_EPSILON_FRACTION);
	double max_magnitude = std::max(std::abs(result.lower_bound), std::abs(result.upper_bound));
	result.noise_scale = max_magnitude * bind.max_groups * bind.max_contributions / value_epsilon;
	return result;
}

struct DpBoundedValueListBindData : public FunctionData {
	idx_t max_values;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<DpBoundedValueListBindData>(*this);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto other = dynamic_cast<const DpBoundedValueListBindData *>(&other_p);
		return other && max_values == other->max_values;
	}
};

struct DpBoundedValue {
	double value;
	uint64_t score;
};

struct DpBoundedValueListState {
	DpBoundedValue *values;
	idx_t size;
};

static unique_ptr<FunctionData> BindDpBoundedValueList(ClientContext &context, AggregateFunction &,
                                                       vector<unique_ptr<Expression>> &arguments) {
	if (arguments.size() != 2 || !arguments[1]->IsFoldable()) {
		throw InvalidInputException("dp_bounded_value_list: max values must be a constant");
	}
	auto value = ExpressionExecutor::EvaluateScalar(context, *arguments[1]);
	auto max_values = value.GetValue<int64_t>();
	if (max_values <= 0) {
		throw InvalidInputException("dp_bounded_value_list: max values must be positive");
	}
	if (static_cast<uint64_t>(max_values) > NumericLimits<idx_t>::Maximum() / sizeof(DpBoundedValue)) {
		throw InvalidInputException("dp_bounded_value_list: max values is too large");
	}
	auto result = make_uniq<DpBoundedValueListBindData>();
	result->max_values = static_cast<idx_t>(max_values);
	return std::move(result);
}

static idx_t DpBoundedValueListStateSize(const AggregateFunction &) {
	return sizeof(DpBoundedValueListState);
}

static void DpBoundedValueListInitialize(const AggregateFunction &, data_ptr_t state_p) {
	memset(state_p, 0, sizeof(DpBoundedValueListState));
}

static uint64_t BoundedValueScore(double value) {
	uint64_t bits;
	memcpy(&bits, &value, sizeof(bits));
	bits += 0x9e3779b97f4a7c15ULL;
	bits = (bits ^ (bits >> 30)) * 0xbf58476d1ce4e5b9ULL;
	bits = (bits ^ (bits >> 27)) * 0x94d049bb133111ebULL;
	return bits ^ (bits >> 31);
}

static uint64_t BoundedValueBits(double value) {
	uint64_t bits;
	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

static bool BoundedValueLess(const DpBoundedValue &left, const DpBoundedValue &right) {
	return left.score < right.score ||
	       (left.score == right.score && BoundedValueBits(left.value) < BoundedValueBits(right.value));
}

static void EnsureBoundedValueStorage(DpBoundedValueListState &state, idx_t max_values, ArenaAllocator &allocator) {
	if (state.values) {
		return;
	}
	state.values = reinterpret_cast<DpBoundedValue *>(allocator.Allocate(sizeof(DpBoundedValue) * max_values));
}

static void InsertBoundedValue(DpBoundedValueListState &state, double value, idx_t max_values,
                               ArenaAllocator &allocator) {
	// Match Google's numerical bounded-mean preprocessing: NaNs do not contribute,
	// while infinities are represented by the largest finite value of the same sign.
	if (std::isnan(value)) {
		return;
	}
	if (std::isinf(value)) {
		value = std::copysign(NumericLimits<double>::Maximum(), value);
	}
	EnsureBoundedValueStorage(state, max_values, allocator);
	DpBoundedValue candidate {value, BoundedValueScore(value)};
	if (state.size < max_values) {
		state.values[state.size++] = candidate;
		std::push_heap(state.values, state.values + state.size, BoundedValueLess);
		return;
	}
	if (BoundedValueLess(candidate, state.values[0])) {
		std::pop_heap(state.values, state.values + state.size, BoundedValueLess);
		state.values[state.size - 1] = candidate;
		std::push_heap(state.values, state.values + state.size, BoundedValueLess);
	}
}

template <class STATE_GETTER>
static void UpdateBoundedValueRows(Vector inputs[], AggregateInputData &input, idx_t count, STATE_GETTER get_state) {
	UnifiedVectorFormat values;
	inputs[0].ToUnifiedFormat(count, values);
	auto value_data = UnifiedVectorFormat::GetData<double>(values);
	auto max_values = input.bind_data->Cast<DpBoundedValueListBindData>().max_values;
	for (idx_t row = 0; row < count; row++) {
		auto value_index = values.sel->get_index(row);
		if (!values.validity.RowIsValid(value_index)) {
			continue;
		}
		InsertBoundedValue(*get_state(row), value_data[value_index], max_values, input.allocator);
	}
}

static void DpBoundedValueListUpdate(Vector inputs[], AggregateInputData &input, idx_t, data_ptr_t state_p,
                                     idx_t count) {
	auto state = reinterpret_cast<DpBoundedValueListState *>(state_p);
	UpdateBoundedValueRows(inputs, input, count, [state](idx_t) { return state; });
}

static void DpBoundedValueListScatterUpdate(Vector inputs[], AggregateInputData &input, idx_t, Vector &states,
                                            idx_t count) {
	UnifiedVectorFormat state_data;
	states.ToUnifiedFormat(count, state_data);
	auto state_ptrs = UnifiedVectorFormat::GetData<DpBoundedValueListState *>(state_data);
	UpdateBoundedValueRows(inputs, input, count, [&](idx_t row) { return state_ptrs[state_data.sel->get_index(row)]; });
}

static void DpBoundedValueListCombine(Vector &source, Vector &target, AggregateInputData &input, idx_t count) {
	auto sources = FlatVector::GetData<DpBoundedValueListState *>(source);
	auto targets = FlatVector::GetData<DpBoundedValueListState *>(target);
	auto max_values = input.bind_data->Cast<DpBoundedValueListBindData>().max_values;
	for (idx_t i = 0; i < count; i++) {
		for (idx_t j = 0; j < sources[i]->size; j++) {
			InsertBoundedValue(*targets[i], sources[i]->values[j].value, max_values, input.allocator);
		}
	}
}

static void DpBoundedValueListFinalize(Vector &states, AggregateInputData &, Vector &result, idx_t count,
                                       idx_t offset) {
	auto state_ptrs = FlatVector::GetData<DpBoundedValueListState *>(states);
	auto entries = FlatVector::GetData<list_entry_t>(result);
	auto &child = ListVector::GetEntry(result);
	idx_t total = ListVector::GetListSize(result);
	for (idx_t i = 0; i < count; i++) {
		total += state_ptrs[i]->size;
	}
	ListVector::Reserve(result, total);
	ListVector::SetListSize(result, total);
	auto output = FlatVector::GetData<double>(child);
	idx_t cursor = total;
	for (idx_t i = count; i > 0; i--) {
		auto *state = state_ptrs[i - 1];
		cursor -= state->size;
		entries[offset + i - 1].offset = cursor;
		entries[offset + i - 1].length = state->size;
		std::sort(state->values, state->values + state->size, BoundedValueLess);
		for (idx_t j = 0; j < state->size; j++) {
			output[cursor + j] = state->values[j].value;
		}
	}
}

// Order-independent scalar form of the AS magnitude accumulator. The compiler
// uses it for per-PU floating SUMs so ordinary DOUBLE SUM cancellation cannot
// erase small contributions before ApproxBounds sees them.
struct DpApproxSumState {
	bool isset;
	hugeint_t positive;
	hugeint_t negative;
	long double wide_positive;
	long double wide_negative;
};

static bool FitsApproxSumFixedPoint(double value) {
	return std::abs(value) <= static_cast<double>(INT64_MAX) / CLIP_DOUBLE_SCALE;
}

static uint64_t ApproxSumScaledMagnitude(double value) {
	auto scaled = ScaleFloatToInt64<double, CLIP_DOUBLE_SHIFT>(value);
	auto magnitude = scaled == INT64_MIN ? static_cast<uint64_t>(INT64_MAX) : static_cast<uint64_t>(std::abs(scaled));
	auto shift = static_cast<uint64_t>(PacClipSumIntState<>::GetLevel(magnitude) * CLIP_LEVEL_SHIFT);
	return (magnitude >> shift) << shift;
}

struct DpApproxSumOperation {
	template <class STATE>
	static void Initialize(STATE &state) {
		state.isset = false;
		state.positive = hugeint_t(0);
		state.negative = hugeint_t(0);
		state.wide_positive = 0.0;
		state.wide_negative = 0.0;
	}

	template <class STATE>
	static void AddValue(STATE &state, double value, idx_t count) {
		// Match bounded numerical aggregates: NaN contributes nothing and infinity is represented by
		// the largest finite value. This keeps exceptional input data out of the observable transcript.
		if (std::isnan(value)) {
			return;
		}
		if (std::isinf(value)) {
			value = std::copysign(NumericLimits<double>::Maximum(), value);
		}
		state.isset = true;
		if (!FitsApproxSumFixedPoint(value)) {
			long double contribution = static_cast<long double>(std::abs(value)) * static_cast<long double>(count);
			if (std::signbit(value) && value != 0.0) {
				state.wide_negative += contribution;
			} else {
				state.wide_positive += contribution;
			}
			return;
		}
		auto scaled = Hugeint::Multiply(Hugeint::Convert(ApproxSumScaledMagnitude(value)), Hugeint::Convert(count));
		if (std::signbit(value) && value != 0.0) {
			state.negative = Hugeint::Add(state.negative, scaled);
		} else {
			state.positive = Hugeint::Add(state.positive, scaled);
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &) {
		AddValue(state, static_cast<double>(input), 1);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &, idx_t count) {
		AddValue(state, static_cast<double>(input), count);
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		target.isset = target.isset || source.isset;
		target.positive = Hugeint::Add(target.positive, source.positive);
		target.negative = Hugeint::Add(target.negative, source.negative);
		target.wide_positive += source.wide_positive;
		target.wide_negative += source.wide_negative;
	}

	template <class RESULT_TYPE, class STATE>
	static void Finalize(STATE &state, RESULT_TYPE &target, AggregateFinalizeData &finalize_data) {
		if (!state.isset) {
			finalize_data.ReturnNull();
			return;
		}
		auto scaled = Hugeint::Subtract(state.positive, state.negative);
		long double value = state.wide_positive - state.wide_negative;
		value += static_cast<long double>(Hugeint::Cast<double>(scaled)) / CLIP_DOUBLE_SCALE;
		value = std::max(-static_cast<long double>(NumericLimits<double>::Maximum()),
		                 std::min(static_cast<long double>(NumericLimits<double>::Maximum()), value));
		target = static_cast<double>(value);
	}

	static bool IgnoreNull() {
		return true;
	}
};

static LogicalType DpApproxBoundsDebugType() {
	child_list_t<LogicalType> children;
	children.emplace_back("success", LogicalType::BOOLEAN);
	children.emplace_back("lower_bound", LogicalType::DOUBLE);
	children.emplace_back("upper_bound", LogicalType::DOUBLE);
	children.emplace_back("clipped_value", LogicalType::DOUBLE);
	children.emplace_back("noise_scale", LogicalType::DOUBLE);
	children.emplace_back("selected_bin", LogicalType::INTEGER);
	children.emplace_back("attempts", LogicalType::INTEGER);
	children.emplace_back("contributions", LogicalType::UBIGINT);
	return LogicalType::STRUCT(std::move(children));
}

static void WriteDebug(Vector &result, idx_t row, const DpApproxBoundsResult &value) {
	auto &children = StructVector::GetEntries(result);
	FlatVector::GetData<bool>(*children[0])[row] = value.success;
	FlatVector::GetData<double>(*children[1])[row] = value.lower_bound;
	FlatVector::GetData<double>(*children[2])[row] = value.upper_bound;
	FlatVector::GetData<double>(*children[3])[row] = value.clipped_value;
	FlatVector::GetData<double>(*children[4])[row] = value.noise_scale;
	FlatVector::GetData<int32_t>(*children[5])[row] = value.selected_bin;
	FlatVector::GetData<int32_t>(*children[6])[row] = value.attempts;
	FlatVector::GetData<uint64_t>(*children[7])[row] = value.contributions;
}

template <class STATE_GETTER>
static void UpdateMeanRows(Vector inputs[], AggregateInputData &input, idx_t count, STATE_GETTER get_state) {
	UnifiedVectorFormat lists;
	inputs[0].ToUnifiedFormat(count, lists);
	auto entries = UnifiedVectorFormat::GetData<list_entry_t>(lists);
	auto &child = ListVector::GetEntry(inputs[0]);
	UnifiedVectorFormat values;
	child.ToUnifiedFormat(ListVector::GetListSize(inputs[0]), values);
	auto value_data = UnifiedVectorFormat::GetData<double>(values);
	auto max_contributions = input.bind_data->Cast<DpApproxBoundsBindData>().max_contributions;
	for (idx_t row = 0; row < count; row++) {
		auto list_index = lists.sel->get_index(row);
		if (!lists.validity.RowIsValid(list_index)) {
			continue;
		}
		auto &entry = entries[list_index];
		if (entry.length > static_cast<idx_t>(max_contributions)) {
			throw InvalidInputException("dp_approx_bounds_mean: per-PU value list exceeds dp_count_bound");
		}
		auto *state = get_state(row);
		for (idx_t j = 0; j < entry.length; j++) {
			auto value_index = values.sel->get_index(entry.offset + j);
			if (!values.validity.RowIsValid(value_index)) {
				continue;
			}
			UpdateState(*state, input.bind_data->Cast<DpApproxBoundsBindData>(), value_data[value_index],
			            input.allocator);
		}
	}
}

static void DpApproxBoundsMeanUpdate(Vector inputs[], AggregateInputData &input, idx_t, data_ptr_t state_p,
                                     idx_t count) {
	auto state = reinterpret_cast<DpApproxBoundsState *>(state_p);
	UpdateMeanRows(inputs, input, count, [state](idx_t) { return state; });
}

static void DpApproxBoundsMeanScatterUpdate(Vector inputs[], AggregateInputData &input, idx_t, Vector &states,
                                            idx_t count) {
	UnifiedVectorFormat state_data;
	states.ToUnifiedFormat(count, state_data);
	auto state_ptrs = UnifiedVectorFormat::GetData<DpApproxBoundsState *>(state_data);
	UpdateMeanRows(inputs, input, count, [&](idx_t row) { return state_ptrs[state_data.sel->get_index(row)]; });
}

static double ClampMean(double value, double lower, double upper) {
	return std::max(lower, std::min(upper, value));
}

static void DpApproxBoundsMeanFinalize(Vector &states, AggregateInputData &input, Vector &result, idx_t count,
                                       idx_t offset) {
	auto state_ptrs = FlatVector::GetData<DpApproxBoundsState *>(states);
	auto &bind = input.bind_data->Cast<DpApproxBoundsBindData>();
	auto result_data = FlatVector::GetData<double>(result);
	for (idx_t i = 0; i < count; i++) {
		auto bounds = FinalizeState(*state_ptrs[i], bind, false);
		if (!bounds.success) {
			FlatVector::Validity(result).SetInvalid(offset + i);
			continue;
		}
		// A point interval has zero sensitivity but cannot define the normalized mean. Widen it by one
		// adjacent geometric step, preserving the selected sign and the original bounded values.
		if (bounds.lower_bound == bounds.upper_bound) {
			if (std::round(bounds.lower_bound) == -1.0 || std::round(bounds.lower_bound) == 0.0) {
				bounds.upper_bound += 1.0;
			} else if (std::round(bounds.upper_bound) == 1.0) {
				bounds.lower_bound = 0.0;
			} else if (bounds.lower_bound < 0.0) {
				bounds.upper_bound = bounds.lower_bound / 2.0;
			} else {
				bounds.lower_bound = bounds.upper_bound / 2.0;
			}
			bounds.clipped_value =
			    static_cast<double>(ComputeClampedSum(*state_ptrs[i], bind, bounds.lower_bound, bounds.upper_bound));
		}
		double range = bounds.upper_bound - bounds.lower_bound;
		if (!(range > 0.0) || !std::isfinite(range)) {
			throw InvalidInputException("dp_approx_bounds_mean: selected value range is not finite and positive");
		}
		double midpoint = bounds.lower_bound + range / 2.0;
		double normalized_sum = bounds.clipped_value - static_cast<double>(bounds.contributions) * midpoint;
		double component_epsilon = bind.epsilon * (1.0 - DP_APPROX_BOUNDS_EPSILON_FRACTION) / 2.0;
		double contribution_sensitivity = bind.max_groups * bind.max_contributions;
		double count_scale = contribution_sensitivity / component_epsilon;
		double sum_scale = contribution_sensitivity * (range / 2.0) / component_epsilon;
		double noised_count = static_cast<double>(bounds.contributions);
		double noised_sum = normalized_sum;
		if (bind.noise_enabled) {
			noised_count = AddDpLaplaceNoise(noised_count, count_scale);
			noised_sum = AddDpLaplaceNoise(noised_sum, sum_scale);
		}
		noised_count = std::max(1.0, noised_count);
		result_data[offset + i] =
		    ClampMean(midpoint + noised_sum / noised_count, bounds.lower_bound, bounds.upper_bound);
	}
}

template <bool DEBUG>
static void DpApproxBoundsFinalize(Vector &states, AggregateInputData &input, Vector &result, idx_t count,
                                   idx_t offset) {
	auto state_ptrs = FlatVector::GetData<DpApproxBoundsState *>(states);
	auto &bind = input.bind_data->Cast<DpApproxBoundsBindData>();
	auto result_data = DEBUG ? nullptr : FlatVector::GetData<double>(result);
	for (idx_t i = 0; i < count; i++) {
		auto value = FinalizeState(*state_ptrs[i], bind, true);
		if (DEBUG) {
			WriteDebug(result, offset + i, value);
			continue;
		}
		if (!value.success) {
			FlatVector::Validity(result).SetInvalid(offset + i);
			continue;
		}
		result_data[offset + i] =
		    bind.noise_enabled ? AddDpLaplaceNoise(value.clipped_value, value.noise_scale) : value.clipped_value;
	}
}

static AggregateFunction MakeDpApproxBoundsFunction(const string &name, const LogicalType &return_type,
                                                    aggregate_finalize_t finalize) {
	return AggregateFunction(
	    name,
	    {LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::UBIGINT},
	    return_type, DpApproxBoundsStateSize, DpApproxBoundsInitialize, DpApproxBoundsScatterUpdate,
	    DpApproxBoundsCombine, finalize, FunctionNullHandling::SPECIAL_HANDLING, DpApproxBoundsUpdate,
	    BindDpApproxBounds);
}

static AggregateFunction MakeDpBoundedValueListFunction() {
	return AggregateFunction("dp_bounded_value_list", {LogicalType::DOUBLE, LogicalType::BIGINT},
	                         LogicalType::LIST(LogicalType::DOUBLE), DpBoundedValueListStateSize,
	                         DpBoundedValueListInitialize, DpBoundedValueListScatterUpdate, DpBoundedValueListCombine,
	                         DpBoundedValueListFinalize, FunctionNullHandling::SPECIAL_HANDLING,
	                         DpBoundedValueListUpdate, BindDpBoundedValueList);
}

static AggregateFunction MakeDpApproxBoundsMeanFunction() {
	return AggregateFunction("dp_approx_bounds_mean",
	                         {LogicalType::LIST(LogicalType::DOUBLE), LogicalType::DOUBLE, LogicalType::DOUBLE,
	                          LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::UBIGINT},
	                         LogicalType::DOUBLE, DpApproxBoundsStateSize, DpApproxBoundsInitialize,
	                         DpApproxBoundsMeanScatterUpdate, DpApproxBoundsCombine, DpApproxBoundsMeanFinalize,
	                         FunctionNullHandling::SPECIAL_HANDLING, DpApproxBoundsMeanUpdate, BindDpApproxBounds);
}

static void RegisterInternalAggregate(ExtensionLoader &loader, AggregateFunction function) {
	loader.RegisterFunction(CreateAggregateFunctionInfo(std::move(function)));
}

void RegisterDpApproxBoundsAggregateFunctions(ExtensionLoader &loader) {
	RegisterInternalAggregate(loader, MakeDpBoundedValueListFunction());

	auto approx_sum = AggregateFunction::UnaryAggregate<DpApproxSumState, double, double, DpApproxSumOperation>(
	    LogicalType::DOUBLE, LogicalType::DOUBLE);
	approx_sum.name = "priv_approx_sum";
	RegisterInternalAggregate(loader, std::move(approx_sum));

	RegisterInternalAggregate(
	    loader, MakeDpApproxBoundsFunction("dp_approx_bounds_sum", LogicalType::DOUBLE, DpApproxBoundsFinalize<false>));
	RegisterInternalAggregate(loader, MakeDpApproxBoundsMeanFunction());

	RegisterInternalAggregate(loader,
	                          MakeDpApproxBoundsFunction("dp_approx_bounds_sum_debug", DpApproxBoundsDebugType(),
	                                                     DpApproxBoundsFinalize<true>));
}

} // namespace duckdb
