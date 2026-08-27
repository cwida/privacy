#include "aggregates/dp_approx_bounds.hpp"

#include "aggregates/as_clip_aggr.hpp"
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

// These constants match google/differential-privacy ApproxBounds<double> at
// b5b0286e313b64a692abfc8c528d54ef47d960a0.
constexpr idx_t DP_APPROX_BOUNDS_NUM_BINS = 2047;
constexpr double DP_APPROX_BOUNDS_INITIAL_FAILURE_PROBABILITY = 1e-9;
constexpr double DP_APPROX_BOUNDS_MIN_SUCCESS_PROBABILITY = 1.0 - 1e-6;
constexpr int DP_APPROX_BOUNDS_MAX_ATTEMPTS = 30;

void ValidateDpApproxBoundsEpsilonFraction(double fraction) {
	if (!std::isfinite(fraction) || fraction <= 0.0 || fraction >= 1.0) {
		throw InvalidInputException("dp_approx_bounds_epsilon_fraction must be between 0 and 1");
	}
}

double GetDpApproxBoundsEpsilonFraction(ClientContext &context) {
	Value value;
	double result = 0.5;
	if (context.TryGetCurrentSetting("dp_approx_bounds_epsilon_fraction", value) && !value.IsNull()) {
		result = value.GetValue<double>();
	}
	ValidateDpApproxBoundsEpsilonFraction(result);
	return result;
}

struct DpApproxBoundsBin {
	double support;
	long double magnitude_sum;
	uint64_t count;
};

struct DpApproxBoundsState {
	DpApproxBoundsBin *positive;
	DpApproxBoundsBin *negative;
	uint64_t nonce;
	bool nonce_set;
};

struct DpApproxBoundsBindData : public FunctionData {
	double epsilon;
	double max_groups;
	double bounds_fraction;
	uint64_t seed;
	bool noise_enabled;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<DpApproxBoundsBindData>(*this);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto other = dynamic_cast<const DpApproxBoundsBindData *>(&other_p);
		return other && epsilon == other->epsilon && max_groups == other->max_groups &&
		       bounds_fraction == other->bounds_fraction && seed == other->seed &&
		       noise_enabled == other->noise_enabled;
	}
};

struct DpApproxBoundsResult {
	bool success;
	double lower_bound;
	double upper_bound;
	double clipped_value;
	double noise_scale;
	double threshold;
	int32_t selected_bin;
	int32_t attempts;
	uint64_t contributions;
};

static double EvaluateConfig(ClientContext &context, const Expression &expression, const string &name) {
	if (!expression.IsFoldable()) {
		throw InvalidInputException("dp_approx_bounds_sum: %s must be a constant", name);
	}
	return ExpressionExecutor::EvaluateScalar(context, expression).GetValue<double>();
}

static unique_ptr<FunctionData> BindDpApproxBounds(ClientContext &context, AggregateFunction &,
                                                   vector<unique_ptr<Expression>> &arguments) {
	if (arguments.size() != 4) {
		throw InternalException("dp_approx_bounds_sum: expected value, epsilon, max_groups, and nonce");
	}
	double epsilon = EvaluateConfig(context, *arguments[1], "epsilon");
	double max_groups = EvaluateConfig(context, *arguments[2], "max groups");
	if (!std::isfinite(epsilon) || epsilon <= 0.0) {
		throw InvalidInputException("dp_approx_bounds_sum: epsilon must be a positive finite number");
	}
	if (!std::isfinite(max_groups) || max_groups <= 0.0) {
		throw InvalidInputException("dp_approx_bounds_sum: max groups must be a positive finite number");
	}
	auto result = make_uniq<DpApproxBoundsBindData>();
	result->epsilon = epsilon;
	result->max_groups = max_groups;
	result->bounds_fraction = GetDpApproxBoundsEpsilonFraction(context);
	result->seed = GetDpNoiseSeed(context);
	result->noise_enabled = IsPacNoiseEnabled(context, true);
	return std::move(result);
}

static idx_t DpApproxBoundsStateSize(const AggregateFunction &) {
	return sizeof(DpApproxBoundsState);
}

static void DpApproxBoundsInitialize(const AggregateFunction &, data_ptr_t state_p) {
	memset(state_p, 0, sizeof(DpApproxBoundsState));
}

static DpApproxBoundsBin *EnsureBins(DpApproxBoundsBin *&bins, ArenaAllocator &allocator) {
	if (!bins) {
		bins = reinterpret_cast<DpApproxBoundsBin *>(
		    allocator.Allocate(sizeof(DpApproxBoundsBin) * DP_APPROX_BOUNDS_NUM_BINS));
		memset(bins, 0, sizeof(DpApproxBoundsBin) * DP_APPROX_BOUNDS_NUM_BINS);
	}
	return bins;
}

static double BinUpperBound(idx_t index) {
	if (index >= DP_APPROX_BOUNDS_NUM_BINS - 1) {
		return std::numeric_limits<double>::max();
	}
	return std::ldexp(std::numeric_limits<double>::min(), static_cast<int>(index));
}

static idx_t BinIndex(double value) {
	double magnitude = std::abs(value);
	if (magnitude <= std::numeric_limits<double>::min()) {
		return 0;
	}
	double raw = std::ceil(std::log2(magnitude) - std::log2(std::numeric_limits<double>::min()));
	if (!std::isfinite(raw) || raw >= static_cast<double>(DP_APPROX_BOUNDS_NUM_BINS - 1)) {
		return DP_APPROX_BOUNDS_NUM_BINS - 1;
	}
	auto index = static_cast<idx_t>(std::max(0.0, raw));
	if (index > 0 && magnitude <= BinUpperBound(index - 1)) {
		index--;
	}
	return index;
}

static void SetNonce(DpApproxBoundsState &state, uint64_t nonce) {
	if (state.nonce_set && state.nonce != nonce) {
		throw InvalidInputException("dp_approx_bounds_sum: nonce must be constant within each aggregate group");
	}
	state.nonce = nonce;
	state.nonce_set = true;
}

static void UpdateState(DpApproxBoundsState &state, double value, uint64_t nonce, ArenaAllocator &allocator) {
	if (!std::isfinite(value)) {
		throw InvalidInputException("dp_approx_bounds_sum: contribution must be finite");
	}
	SetNonce(state, nonce);
	auto index = BinIndex(value);
	auto &bin =
	    value < 0.0 ? EnsureBins(state.negative, allocator)[index] : EnsureBins(state.positive, allocator)[index];
	bin.support += 1.0;
	bin.magnitude_sum += static_cast<long double>(std::abs(value));
	bin.count++;
}

struct DpApproxBoundsInputs {
	UnifiedVectorFormat value;
	UnifiedVectorFormat nonce;
	const double *values;
	const uint64_t *nonces;

	DpApproxBoundsInputs(Vector inputs[], idx_t count) {
		inputs[0].ToUnifiedFormat(count, value);
		inputs[3].ToUnifiedFormat(count, nonce);
		values = UnifiedVectorFormat::GetData<double>(value);
		nonces = UnifiedVectorFormat::GetData<uint64_t>(nonce);
	}

	bool IsValid(idx_t row) const {
		return value.validity.RowIsValid(value.sel->get_index(row)) &&
		       nonce.validity.RowIsValid(nonce.sel->get_index(row));
	}
};

template <class STATE_GETTER>
static void UpdateRows(Vector inputs[], AggregateInputData &input, idx_t count, STATE_GETTER get_state) {
	DpApproxBoundsInputs vectors(inputs, count);
	for (idx_t row = 0; row < count; row++) {
		if (!vectors.IsValid(row)) {
			continue;
		}
		auto value_index = vectors.value.sel->get_index(row);
		auto nonce_index = vectors.nonce.sel->get_index(row);
		UpdateState(*get_state(row), vectors.values[value_index], vectors.nonces[nonce_index], input.allocator);
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

static void CombineBins(const DpApproxBoundsBin *source, DpApproxBoundsBin *&target, ArenaAllocator &allocator) {
	if (!source) {
		return;
	}
	auto target_bins = EnsureBins(target, allocator);
	for (idx_t i = 0; i < DP_APPROX_BOUNDS_NUM_BINS; i++) {
		target_bins[i].support += source[i].support;
		target_bins[i].magnitude_sum += source[i].magnitude_sum;
		target_bins[i].count += source[i].count;
	}
}

static void DpApproxBoundsCombine(Vector &source, Vector &target, AggregateInputData &input, idx_t count) {
	auto sources = FlatVector::GetData<DpApproxBoundsState *>(source);
	auto targets = FlatVector::GetData<DpApproxBoundsState *>(target);
	for (idx_t i = 0; i < count; i++) {
		CombineBins(sources[i]->positive, targets[i]->positive, input.allocator);
		CombineBins(sources[i]->negative, targets[i]->negative, input.allocator);
		if (sources[i]->nonce_set) {
			SetNonce(*targets[i], sources[i]->nonce);
		}
	}
}

static double LaplaceThreshold(double success_probability, double scale) {
	double log_success_per_bin = std::log(success_probability) / (2.0 * DP_APPROX_BOUNDS_NUM_BINS);
	double upper_tail_probability = -std::expm1(log_success_per_bin);
	return -scale * std::log(2.0 * upper_tail_probability);
}

static int FindSelectedBin(const double *positive, const double *negative, double threshold) {
	int selected = -1;
	for (idx_t i = 0; i < DP_APPROX_BOUNDS_NUM_BINS; i++) {
		if (positive[i] >= threshold || negative[i] >= threshold) {
			selected = static_cast<int>(i);
		}
	}
	return selected;
}

static uint64_t CountContributions(const DpApproxBoundsState &state) {
	uint64_t result = 0;
	for (idx_t i = 0; i < DP_APPROX_BOUNDS_NUM_BINS; i++) {
		if (state.positive) {
			result += state.positive[i].count;
		}
		if (state.negative) {
			result += state.negative[i].count;
		}
	}
	return result;
}

static long double ClipBins(const DpApproxBoundsBin *bins, int selected_bin, double bound) {
	if (!bins) {
		return 0.0;
	}
	long double result = 0.0;
	for (idx_t i = 0; i < DP_APPROX_BOUNDS_NUM_BINS; i++) {
		result += i <= static_cast<idx_t>(selected_bin)
		              ? bins[i].magnitude_sum
		              : static_cast<long double>(bins[i].count) * static_cast<long double>(bound);
	}
	return result;
}

static DpApproxBoundsResult FinalizeState(const DpApproxBoundsState &state, const DpApproxBoundsBindData &bind) {
	DpApproxBoundsResult result {};
	result.selected_bin = -1;
	result.contributions = CountContributions(state);
	if (result.contributions == 0) {
		return result;
	}

	double positive[DP_APPROX_BOUNDS_NUM_BINS];
	double negative[DP_APPROX_BOUNDS_NUM_BINS];
	for (idx_t i = 0; i < DP_APPROX_BOUNDS_NUM_BINS; i++) {
		positive[i] = state.positive ? state.positive[i].support : 0.0;
		negative[i] = state.negative ? state.negative[i].support : 0.0;
	}

	if (!bind.noise_enabled) {
		result.threshold = 1.0;
		result.attempts = 1;
		result.selected_bin = FindSelectedBin(positive, negative, result.threshold);
	} else {
		double bounds_epsilon = bind.epsilon * bind.bounds_fraction;
		double histogram_scale = bind.max_groups / bounds_epsilon;
		uint64_t nonce = state.nonce_set ? state.nonce : 0;
		AddDpLaplaceNoiseBatch(positive, positive, DP_APPROX_BOUNDS_NUM_BINS, histogram_scale, bind.seed, nonce * 8192);
		AddDpLaplaceNoiseBatch(negative, negative, DP_APPROX_BOUNDS_NUM_BINS, histogram_scale, bind.seed,
		                       nonce * 8192 + DP_APPROX_BOUNDS_NUM_BINS);

		double success_probability = 1.0 - DP_APPROX_BOUNDS_INITIAL_FAILURE_PROBABILITY;
		for (int attempt = 1; attempt <= DP_APPROX_BOUNDS_MAX_ATTEMPTS; attempt++) {
			result.threshold = LaplaceThreshold(success_probability, histogram_scale);
			result.selected_bin = FindSelectedBin(positive, negative, result.threshold);
			result.attempts = attempt;
			if (result.selected_bin >= 0) {
				break;
			}
			double failure_probability = 1.0 - success_probability;
			success_probability = 1.0 - 10.0 * failure_probability;
			if (!(success_probability > DP_APPROX_BOUNDS_MIN_SUCCESS_PROBABILITY)) {
				break;
			}
		}
	}

	if (result.selected_bin < 0) {
		return result;
	}
	result.success = true;
	result.upper_bound = BinUpperBound(static_cast<idx_t>(result.selected_bin));
	result.lower_bound = -result.upper_bound;
	auto positive_sum = ClipBins(state.positive, result.selected_bin, result.upper_bound);
	auto negative_sum = ClipBins(state.negative, result.selected_bin, result.upper_bound);
	result.clipped_value = static_cast<double>(positive_sum - negative_sum);
	double value_epsilon = bind.epsilon * (1.0 - bind.bounds_fraction);
	result.noise_scale = result.upper_bound * bind.max_groups / value_epsilon;
	return result;
}

// Order-independent scalar form of the AS magnitude accumulator. The compiler
// uses it for per-PU floating SUMs so ordinary DOUBLE SUM cancellation cannot
// erase small contributions before ApproxBounds sees them.
struct DpApproxSumState {
	bool isset;
	hugeint_t positive;
	hugeint_t negative;
};

static uint64_t ApproxSumScaledMagnitude(double value) {
	auto scaled = ScaleFloatToInt64<double, CLIP_DOUBLE_SHIFT>(value);
	auto magnitude = scaled == INT64_MIN ? static_cast<uint64_t>(INT64_MAX) : static_cast<uint64_t>(std::abs(scaled));
	return ClipApproximateMagnitude64(magnitude);
}

struct DpApproxSumOperation {
	template <class STATE>
	static void Initialize(STATE &state) {
		state.isset = false;
		state.positive = hugeint_t(0);
		state.negative = hugeint_t(0);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &) {
		double value = static_cast<double>(input);
		if (!std::isfinite(value)) {
			throw InvalidInputException("priv_approx_sum: per-PU SUM contribution must be finite");
		}
		state.isset = true;
		auto scaled = Hugeint::Convert(ApproxSumScaledMagnitude(value));
		if (std::signbit(value) && value != 0.0) {
			state.negative = Hugeint::Add(state.negative, scaled);
		} else {
			state.positive = Hugeint::Add(state.positive, scaled);
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &, idx_t count) {
		double value = static_cast<double>(input);
		if (!std::isfinite(value)) {
			throw InvalidInputException("priv_approx_sum: per-PU SUM contribution must be finite");
		}
		state.isset = true;
		auto total = Hugeint::Multiply(Hugeint::Convert(ApproxSumScaledMagnitude(value)), Hugeint::Convert(count));
		if (std::signbit(value) && value != 0.0) {
			state.negative = Hugeint::Add(state.negative, total);
		} else {
			state.positive = Hugeint::Add(state.positive, total);
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		target.isset = target.isset || source.isset;
		target.positive = Hugeint::Add(target.positive, source.positive);
		target.negative = Hugeint::Add(target.negative, source.negative);
	}

	template <class RESULT_TYPE, class STATE>
	static void Finalize(STATE &state, RESULT_TYPE &target, AggregateFinalizeData &finalize_data) {
		if (!state.isset) {
			finalize_data.ReturnNull();
			return;
		}
		auto scaled = Hugeint::Subtract(state.positive, state.negative);
		target = Hugeint::Cast<double>(scaled) / CLIP_DOUBLE_SCALE;
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
	children.emplace_back("threshold", LogicalType::DOUBLE);
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
	FlatVector::GetData<double>(*children[5])[row] = value.threshold;
	FlatVector::GetData<int32_t>(*children[6])[row] = value.selected_bin;
	FlatVector::GetData<int32_t>(*children[7])[row] = value.attempts;
	FlatVector::GetData<uint64_t>(*children[8])[row] = value.contributions;
}

template <bool DEBUG>
static void DpApproxBoundsFinalize(Vector &states, AggregateInputData &input, Vector &result, idx_t count,
                                   idx_t offset) {
	auto state_ptrs = FlatVector::GetData<DpApproxBoundsState *>(states);
	auto &bind = input.bind_data->Cast<DpApproxBoundsBindData>();
	auto result_data = DEBUG ? nullptr : FlatVector::GetData<double>(result);
	for (idx_t i = 0; i < count; i++) {
		auto value = FinalizeState(*state_ptrs[i], bind);
		if (DEBUG) {
			WriteDebug(result, offset + i, value);
			continue;
		}
		if (!value.success) {
			FlatVector::Validity(result).SetInvalid(offset + i);
			continue;
		}
		uint64_t nonce = state_ptrs[i]->nonce_set ? state_ptrs[i]->nonce : 0;
		result_data[offset + i] = bind.noise_enabled
		                              ? AddDpLaplaceNoise(value.clipped_value, value.noise_scale, bind.seed,
		                                                  nonce * 8192 + 2 * DP_APPROX_BOUNDS_NUM_BINS)
		                              : value.clipped_value;
	}
}

static AggregateFunction MakeDpApproxBoundsFunction(const string &name, const LogicalType &return_type,
                                                    aggregate_finalize_t finalize) {
	return AggregateFunction(
	    name, {LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::UBIGINT}, return_type,
	    DpApproxBoundsStateSize, DpApproxBoundsInitialize, DpApproxBoundsScatterUpdate, DpApproxBoundsCombine, finalize,
	    FunctionNullHandling::SPECIAL_HANDLING, DpApproxBoundsUpdate, BindDpApproxBounds);
}

void RegisterDpApproxBoundsAggregateFunctions(ExtensionLoader &loader) {
	auto approx_sum = AggregateFunction::UnaryAggregate<DpApproxSumState, double, double, DpApproxSumOperation>(
	    LogicalType::DOUBLE, LogicalType::DOUBLE);
	approx_sum.name = "priv_approx_sum";
	CreateAggregateFunctionInfo approx_sum_info(approx_sum);
	FunctionDescription approx_sum_description;
	approx_sum_description.description =
	    "[INTERNAL] Order-independent approximate floating SUM used by private per-PU pre-aggregation.";
	approx_sum_info.descriptions.push_back(std::move(approx_sum_description));
	loader.RegisterFunction(std::move(approx_sum_info));

	auto sum = MakeDpApproxBoundsFunction("dp_approx_bounds_sum", LogicalType::DOUBLE, DpApproxBoundsFinalize<false>);
	CreateAggregateFunctionInfo sum_info(sum);
	FunctionDescription description;
	description.description =
	    "[INTERNAL] Google-compatible query-local ApproxBounds followed by a bounded Laplace SUM.";
	sum_info.descriptions.push_back(std::move(description));
	loader.RegisterFunction(std::move(sum_info));

	auto debug = MakeDpApproxBoundsFunction("dp_approx_bounds_sum_debug", DpApproxBoundsDebugType(),
	                                        DpApproxBoundsFinalize<true>);
	loader.RegisterFunction(CreateAggregateFunctionInfo(debug));
}

} // namespace duckdb
