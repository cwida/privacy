#include "aggregates/filterless_aggregate.hpp"

#include "aggregates/as_clip_aggr.hpp"
#include "aggregates/dp_laplace_noise.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "utils/privacy_helpers.hpp"

#include <cmath>
#include <cstring>

namespace duckdb {

static bool FilterlessPuIsSampled(uint64_t pu_hash, int sample_bits) {
	D_ASSERT(sample_bits >= 0 && sample_bits <= FILTERLESS_MAX_SAMPLE_BITS);
	if (sample_bits == 0) {
		return true;
	}
	return (pu_hash >> (64 - sample_bits)) == 0;
}

static double FilterlessSampleWeight(int sample_bits) {
	D_ASSERT(sample_bits >= 0 && sample_bits <= FILTERLESS_MAX_SAMPLE_BITS);
	return std::ldexp(1.0, sample_bits);
}

void ValidateFilterlessSampleBits(int64_t sample_bits) {
	if (sample_bits < 0 || sample_bits > FILTERLESS_MAX_SAMPLE_BITS) {
		throw InvalidInputException("dp_filterless_sample_bits must be between 0 and %d", FILTERLESS_MAX_SAMPLE_BITS);
	}
}

void ValidateFilterlessClipSupport(double clip_support) {
	if (!std::isfinite(clip_support) || clip_support <= 0.0) {
		throw InvalidInputException("dp_filterless_clip_support must be a positive finite number");
	}
}

void ValidateFilterlessBoundsEpsilonFraction(double fraction) {
	if (!std::isfinite(fraction) || fraction <= 0.0 || fraction >= 1.0) {
		throw InvalidInputException("dp_filterless_bounds_epsilon_fraction must be between 0 and 1");
	}
}

FilterlessSettings GetFilterlessSettings(ClientContext &context) {
	Value value;
	int64_t sample_bits = 0;
	if (context.TryGetCurrentSetting("dp_filterless_sample_bits", value) && !value.IsNull()) {
		sample_bits = value.GetValue<int64_t>();
	}
	ValidateFilterlessSampleBits(sample_bits);

	double clip_support = std::numeric_limits<double>::quiet_NaN();
	if (!context.TryGetCurrentSetting("dp_filterless_clip_support", value) || value.IsNull()) {
		throw InvalidInputException("dp_filterless_clip_support must be set to a positive finite number");
	}
	clip_support = value.GetValue<double>();
	ValidateFilterlessClipSupport(clip_support);

	double bounds_fraction = 0.25;
	if (context.TryGetCurrentSetting("dp_filterless_bounds_epsilon_fraction", value) && !value.IsNull()) {
		bounds_fraction = value.GetValue<double>();
	}
	ValidateFilterlessBoundsEpsilonFraction(bounds_fraction);

	return {static_cast<int>(sample_bits), FilterlessSampleWeight(static_cast<int>(sample_bits)), clip_support,
	        bounds_fraction};
}

struct FilterlessBin {
	double support;
	hugeint_t answer_sum;
	uint64_t answer_count;
};

struct FilterlessComponentState {
	FilterlessBin *positive;
	FilterlessBin *negative;
	uint64_t active_contributions;
	uint64_t sampled_contributions;
};

struct FilterlessState {
	FilterlessComponentState component;
	uint64_t nonce;
	bool nonce_set;
};

struct FilterlessAvgState {
	FilterlessComponentState sum_component;
	FilterlessComponentState count_component;
	uint64_t nonce;
	bool nonce_set;
};

// With the shared 2^-27 anchor and factor-4 levels, 80 bins reach 2^133 and therefore cover
// the complete signed HUGEINT / DECIMAL(38) domain without saturating the top bin.
constexpr int FILTERLESS_EXACT_BIN_COUNT = 80;
// COUNT partials are BIGINT. Converting INT64_MAX to DOUBLE rounds it to 2^63 and routes it to
// bin 45, so higher bins are unreachable for this public input type and must not participate.
constexpr int FILTERLESS_COUNT_BIN_COUNT = 46;
// HUGEINT and DECIMAL(38) values reach at most bin 77 after DOUBLE-based bin indexing.
constexpr int FILTERLESS_HUGEINT_BIN_COUNT = 78;

struct FilterlessExactBin {
	double support;
	hugeint_t answer_sum;
	uint64_t answer_count;
};

struct FilterlessExactComponentState {
	FilterlessExactBin *positive;
	FilterlessExactBin *negative;
	uint64_t active_contributions;
	uint64_t sampled_contributions;
};

struct FilterlessExactState {
	FilterlessExactComponentState component;
	uint64_t nonce;
	bool nonce_set;
};

struct FilterlessBindData : public FunctionData {
	int sample_bits;
	double sample_weight;
	double clip_support;
	bool noise_enabled;
	double epsilon;
	double bounds_fraction;
	double max_groups;
	bool has_explicit_config;
	double input_scale;
	bool approximate_values;
	idx_t exact_bin_count;
	hugeint_t exact_output_min;
	hugeint_t exact_output_max;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<FilterlessBindData>(*this);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto other = dynamic_cast<const FilterlessBindData *>(&other_p);
		return other && sample_bits == other->sample_bits && clip_support == other->clip_support &&
		       noise_enabled == other->noise_enabled && epsilon == other->epsilon &&
		       bounds_fraction == other->bounds_fraction && max_groups == other->max_groups &&
		       has_explicit_config == other->has_explicit_config && input_scale == other->input_scale &&
		       approximate_values == other->approximate_values && exact_bin_count == other->exact_bin_count &&
		       exact_output_min == other->exact_output_min && exact_output_max == other->exact_output_max;
	}
};

struct FilterlessResult {
	double lower_bound;
	double upper_bound;
	double clipped_value;
	double noise_scale;
	int32_t negative_bin;
	int32_t positive_bin;
	double negative_support;
	double positive_support;
	uint64_t active_contributions;
	uint64_t sampled_contributions;
};

static double SaturatingNoiseScale(long double sensitivity, long double epsilon) {
	if (sensitivity <= 0.0) {
		return 0.0;
	}
	auto scale = sensitivity / epsilon;
	return scale >= static_cast<long double>(std::numeric_limits<double>::max()) ? std::numeric_limits<double>::max()
	                                                                             : static_cast<double>(scale);
}

static double EvaluateConstantDouble(ClientContext &context, const Expression &expression, const string &name) {
	if (!expression.IsFoldable()) {
		throw InvalidInputException("filterless: %s must be a constant", name);
	}
	return ExpressionExecutor::EvaluateScalar(context, expression).GetValue<double>();
}

static unique_ptr<FunctionData> BindFilterless(ClientContext &context, vector<unique_ptr<Expression>> &arguments,
                                               idx_t config_offset, bool approximate_values, idx_t exact_bin_count) {
	auto settings = GetFilterlessSettings(context);
	bool noise_enabled = IsPacNoiseEnabled(context, true);
	double epsilon = GetValidatedDpEpsilon(context, "dp_filterless");
	double max_groups = 1.0;
	TryGetDpMaxGroupsContributed(context, max_groups);
	if (!std::isfinite(max_groups) || max_groups <= 0.0) {
		max_groups = 1.0;
	}
	bool has_explicit_config = arguments.size() > config_offset;

	if (has_explicit_config) {
		epsilon = EvaluateConstantDouble(context, *arguments[config_offset], "epsilon");
		max_groups = EvaluateConstantDouble(context, *arguments[config_offset + 1], "max groups");
	}
	if (!std::isfinite(epsilon) || epsilon <= 0.0) {
		throw InvalidInputException("filterless: epsilon must be a positive finite number");
	}
	if (!std::isfinite(max_groups) || max_groups <= 0.0) {
		throw InvalidInputException("filterless: max groups must be a positive finite number");
	}
	auto result = make_uniq<FilterlessBindData>();
	result->sample_bits = settings.sample_bits;
	result->sample_weight = settings.sample_weight;
	result->clip_support = settings.clip_support;
	result->noise_enabled = noise_enabled;
	result->epsilon = epsilon;
	result->bounds_fraction = settings.bounds_epsilon_fraction;
	result->max_groups = max_groups;
	result->has_explicit_config = has_explicit_config;
	result->input_scale = 1.0;
	result->approximate_values = approximate_values;
	result->exact_bin_count = exact_bin_count;
	result->exact_output_min = NumericLimits<hugeint_t>::Minimum();
	result->exact_output_max = NumericLimits<hugeint_t>::Maximum();
	return std::move(result);
}

static unique_ptr<FunctionData> BindFilterlessSum(ClientContext &context, AggregateFunction &,
                                                  vector<unique_ptr<Expression>> &arguments) {
	idx_t bin_count = arguments[2]->return_type.InternalType() == PhysicalType::INT64 ? FILTERLESS_COUNT_BIN_COUNT
	                                                                                  : FILTERLESS_HUGEINT_BIN_COUNT;
	return BindFilterless(context, arguments, 4, true, bin_count);
}

static unique_ptr<FunctionData> BindFilterlessCount(ClientContext &context, AggregateFunction &,
                                                    vector<unique_ptr<Expression>> &arguments) {
	return BindFilterless(context, arguments, 4, false, FILTERLESS_COUNT_BIN_COUNT);
}

static unique_ptr<FunctionData> BindFilterlessAvg(ClientContext &context, AggregateFunction &,
                                                  vector<unique_ptr<Expression>> &arguments) {
	return BindFilterless(context, arguments, 6, true, FILTERLESS_HUGEINT_BIN_COUNT);
}

constexpr uint64_t FILTERLESS_MAX_SCALED_MAGNITUDE = uint64_t(1) << 60;

static uint64_t AsScaledMagnitude(double value) {
	auto scaled = ScaleFloatToInt64<double, CLIP_DOUBLE_SHIFT>(value);
	// Use symmetric saturation so equal out-of-range positive and negative
	// values cancel in the two-sided accumulator.
	return scaled == INT64_MIN ? static_cast<uint64_t>(INT64_MAX) : static_cast<uint64_t>(std::abs(scaled));
}

static uint64_t ScaledMagnitude(double value) {
	return std::min(AsScaledMagnitude(value), FILTERLESS_MAX_SCALED_MAGNITUDE);
}

static idx_t BinIndex(uint64_t scaled_magnitude) {
	if (scaled_magnitude == 0) {
		return 0;
	}
	int bit_width = 64 - __builtin_clzll(scaled_magnitude);
	int index = (bit_width - 1) / CLIP_LEVEL_SHIFT;
	return static_cast<idx_t>(std::min(index, CLIP_NUM_LEVELS_64 - 1));
}

static double BinUpperBound(int index) {
	if (index < 0) {
		return 0.0;
	}
	int exponent = (index + 1) * CLIP_LEVEL_SHIFT;
	return std::ldexp(1.0, exponent) / CLIP_DOUBLE_SCALE;
}

static FilterlessBin *EnsureBins(FilterlessBin *&bins, ArenaAllocator &allocator) {
	if (!bins) {
		bins = reinterpret_cast<FilterlessBin *>(allocator.Allocate(sizeof(FilterlessBin) * CLIP_NUM_LEVELS_64));
		memset(bins, 0, sizeof(FilterlessBin) * CLIP_NUM_LEVELS_64);
	}
	return bins;
}

static FilterlessBin &GetBin(FilterlessComponentState &state, double value, uint64_t scaled_magnitude,
                             ArenaAllocator &allocator) {
	bool negative = std::signbit(value) && value != 0.0;
	auto index = BinIndex(scaled_magnitude);
	return negative ? EnsureBins(state.negative, allocator)[index] : EnsureBins(state.positive, allocator)[index];
}

static void UpdateComponent(FilterlessComponentState &state, uint64_t pu_hash, bool active, bool answer_valid,
                            double answer_value, bool histogram_valid, double histogram_value,
                            const FilterlessBindData &bind, ArenaAllocator &allocator, bool approximate_values) {
	if (active) {
		state.active_contributions++;
		if (answer_valid) {
			if (!std::isfinite(answer_value)) {
				throw InvalidInputException("filterless: filtered aggregate contribution must be finite");
			}
			auto magnitude = ScaledMagnitude(answer_value);
			auto &answer_bin = GetBin(state, answer_value, magnitude, allocator);
			if (approximate_values) {
				magnitude = ClipApproximateMagnitude64(magnitude);
			}
			answer_bin.answer_sum = Hugeint::Add(answer_bin.answer_sum, Hugeint::Convert(magnitude));
			answer_bin.answer_count++;
		}
	}
	if (FilterlessPuIsSampled(pu_hash, bind.sample_bits) && histogram_valid) {
		if (!std::isfinite(histogram_value)) {
			throw InvalidInputException("filterless: histogram aggregate contribution must be finite");
		}
		auto magnitude = ScaledMagnitude(histogram_value);
		GetBin(state, histogram_value, magnitude, allocator).support += bind.sample_weight;
		state.sampled_contributions++;
	}
}

static void CombineBins(const FilterlessBin *source, FilterlessBin *&target, ArenaAllocator &allocator) {
	if (!source) {
		return;
	}
	auto target_bins = EnsureBins(target, allocator);
	for (idx_t i = 0; i < CLIP_NUM_LEVELS_64; i++) {
		target_bins[i].support += source[i].support;
		target_bins[i].answer_sum = Hugeint::Add(target_bins[i].answer_sum, source[i].answer_sum);
		target_bins[i].answer_count += source[i].answer_count;
	}
}

static void CombineComponent(const FilterlessComponentState &source, FilterlessComponentState &target,
                             ArenaAllocator &allocator) {
	CombineBins(source.positive, target.positive, allocator);
	CombineBins(source.negative, target.negative, allocator);
	target.active_contributions += source.active_contributions;
	target.sampled_contributions += source.sampled_contributions;
}

template <class BIN_TYPE>
static int FindSupportedBin(const BIN_TYPE *bins, idx_t bin_count, const FilterlessBindData &bind, uint64_t,
                            double histogram_epsilon, double &selected_support) {
	D_ASSERT(bin_count <= FILTERLESS_EXACT_BIN_COUNT);
	double scale =
	    bind.noise_enabled
	        ? SaturatingNoiseScale(static_cast<long double>(bind.sample_weight) * bind.max_groups, histogram_epsilon)
	        : 0.0;
	int selected = -1;
	selected_support = 0.0;
	if (!bins) {
		return selected;
	}
	double noised_support[FILTERLESS_EXACT_BIN_COUNT];
	if (scale > 0.0) {
		double support[FILTERLESS_EXACT_BIN_COUNT];
		for (idx_t i = 0; i < bin_count; i++) {
			support[i] = bins[i].support;
		}
		AddDpLaplaceNoiseBatch(support, noised_support, bin_count, scale);
	}
	for (idx_t i = 0; i < bin_count; i++) {
		double support = scale > 0.0 ? noised_support[i] : bins[i].support;
		if (support >= bind.clip_support) {
			selected = static_cast<int>(i);
			selected_support = support;
		}
	}
	return selected;
}

static hugeint_t ScaledBinUpperBound(int index) {
	if (index < 0) {
		return hugeint_t(0);
	}
	auto exponent = static_cast<uint64_t>((index + 1) * CLIP_LEVEL_SHIFT);
	return Hugeint::Convert(uint64_t(1) << exponent);
}

static hugeint_t AddRepeatedBound(hugeint_t result, hugeint_t bound, uint64_t count, bool negative) {
	if (count == 0) {
		return result;
	}
	auto total = Hugeint::Multiply(bound, Hugeint::Convert(count));
	return negative ? Hugeint::Subtract(result, total) : Hugeint::Add(result, total);
}

static double ClipComponent(const FilterlessComponentState &state, int negative_bin, int positive_bin) {
	auto positive_bound = ScaledBinUpperBound(positive_bin);
	auto negative_bound = ScaledBinUpperBound(negative_bin);
	hugeint_t result(0);
	for (int i = 0; i < CLIP_NUM_LEVELS_64; i++) {
		if (state.positive) {
			if (i <= positive_bin) {
				result = Hugeint::Add(result, state.positive[i].answer_sum);
			} else {
				result = AddRepeatedBound(result, positive_bound, state.positive[i].answer_count, false);
			}
		}
		if (state.negative) {
			if (i <= negative_bin) {
				result = Hugeint::Subtract(result, state.negative[i].answer_sum);
			} else {
				result = AddRepeatedBound(result, negative_bound, state.negative[i].answer_count, true);
			}
		}
	}
	return Hugeint::Cast<double>(result) / CLIP_DOUBLE_SCALE;
}

static FilterlessResult FinalizeComponent(const FilterlessComponentState &state, const FilterlessBindData &bind,
                                          uint64_t nonce_base, double epsilon, bool nonnegative) {
	double histogram_epsilon = epsilon * bind.bounds_fraction;
	double value_epsilon = epsilon * (1.0 - bind.bounds_fraction);
	double negative_support = 0.0;
	double positive_support = 0.0;
	int positive_bin =
	    FindSupportedBin(state.positive, CLIP_NUM_LEVELS_64, bind, nonce_base, histogram_epsilon, positive_support);
	int negative_bin = nonnegative
	                       ? -1
	                       : FindSupportedBin(state.negative, CLIP_NUM_LEVELS_64, bind, nonce_base + CLIP_NUM_LEVELS_64,
	                                          histogram_epsilon, negative_support);
	double positive_bound = BinUpperBound(positive_bin);
	double negative_bound = BinUpperBound(negative_bin);
	double clipped = ClipComponent(state, negative_bin, positive_bin);
	double scale = SaturatingNoiseScale(
	    static_cast<long double>(std::max(negative_bound, positive_bound)) * bind.max_groups, value_epsilon);
	return {-negative_bound,
	        positive_bound,
	        clipped,
	        scale,
	        negative_bin,
	        positive_bin,
	        negative_support,
	        positive_support,
	        state.active_contributions,
	        state.sampled_contributions};
}

static idx_t ExactBinIndex(hugeint_t value, const FilterlessBindData &bind) {
	double magnitude = std::abs(Hugeint::Cast<double>(value)) / bind.input_scale;
	if (!std::isfinite(magnitude)) {
		throw InvalidInputException("filterless: exact aggregate contribution is outside the supported numeric range");
	}
	if (magnitude == 0.0) {
		return 0;
	}
	int exponent;
	std::frexp(magnitude, &exponent);
	int scaled_exponent = exponent - 1 + CLIP_DOUBLE_SHIFT;
	idx_t index = scaled_exponent <= 0 ? 0 : static_cast<idx_t>(scaled_exponent / CLIP_LEVEL_SHIFT);
	if (index >= FILTERLESS_EXACT_BIN_COUNT) {
		throw InvalidInputException("filterless: exact aggregate contribution is outside the supported bin range");
	}
	return index;
}

static double ExactBinUpperBound(int index) {
	if (index < 0) {
		return 0.0;
	}
	return std::ldexp(1.0, (index + 1) * CLIP_LEVEL_SHIFT - CLIP_DOUBLE_SHIFT);
}

static FilterlessExactBin *EnsureExactBins(FilterlessExactBin *&bins, ArenaAllocator &allocator) {
	if (!bins) {
		bins = reinterpret_cast<FilterlessExactBin *>(
		    allocator.Allocate(sizeof(FilterlessExactBin) * FILTERLESS_EXACT_BIN_COUNT));
		memset(bins, 0, sizeof(FilterlessExactBin) * FILTERLESS_EXACT_BIN_COUNT);
	}
	return bins;
}

static FilterlessExactBin &GetExactBin(FilterlessExactComponentState &state, hugeint_t value,
                                       const FilterlessBindData &bind, ArenaAllocator &allocator) {
	bool negative = value < 0;
	auto index = ExactBinIndex(value, bind);
	return negative ? EnsureExactBins(state.negative, allocator)[index]
	                : EnsureExactBins(state.positive, allocator)[index];
}

static hugeint_t ToHugeint(hugeint_t value) {
	return value;
}

template <class INPUT_TYPE>
static hugeint_t ToHugeint(INPUT_TYPE value) {
	return Hugeint::Convert(value);
}

template <class INPUT_TYPE>
static void UpdateExactComponent(FilterlessExactComponentState &state, uint64_t pu_hash, bool active, bool answer_valid,
                                 INPUT_TYPE answer_value, bool histogram_valid, INPUT_TYPE histogram_value,
                                 const FilterlessBindData &bind, ArenaAllocator &allocator) {
	if (active) {
		state.active_contributions++;
		if (answer_valid) {
			auto exact_answer = ToHugeint(answer_value);
			auto &answer_bin = GetExactBin(state, exact_answer, bind, allocator);
			answer_bin.answer_sum = Hugeint::Add(answer_bin.answer_sum, exact_answer);
			answer_bin.answer_count++;
		}
	}
	if (FilterlessPuIsSampled(pu_hash, bind.sample_bits) && histogram_valid) {
		auto exact_histogram = ToHugeint(histogram_value);
		GetExactBin(state, exact_histogram, bind, allocator).support += bind.sample_weight;
		state.sampled_contributions++;
	}
}

static void CombineExactBins(const FilterlessExactBin *source, FilterlessExactBin *&target, ArenaAllocator &allocator) {
	if (!source) {
		return;
	}
	auto target_bins = EnsureExactBins(target, allocator);
	for (idx_t i = 0; i < FILTERLESS_EXACT_BIN_COUNT; i++) {
		target_bins[i].support += source[i].support;
		target_bins[i].answer_sum = Hugeint::Add(target_bins[i].answer_sum, source[i].answer_sum);
		target_bins[i].answer_count += source[i].answer_count;
	}
}

static void CombineExactComponent(const FilterlessExactComponentState &source, FilterlessExactComponentState &target,
                                  ArenaAllocator &allocator) {
	CombineExactBins(source.positive, target.positive, allocator);
	CombineExactBins(source.negative, target.negative, allocator);
	target.active_contributions += source.active_contributions;
	target.sampled_contributions += source.sampled_contributions;
}

static hugeint_t ExactClippingBound(int bin, const FilterlessBindData &bind) {
	if (bin < 0) {
		return hugeint_t(0);
	}
	double scaled_bound = std::ceil(ExactBinUpperBound(bin) * bind.input_scale);
	hugeint_t result;
	if (!std::isfinite(scaled_bound) || !Hugeint::TryConvert(scaled_bound, result)) {
		return NumericLimits<hugeint_t>::Maximum();
	}
	return result;
}

static hugeint_t ClipExactComponent(const FilterlessExactComponentState &state, int negative_bin, int positive_bin,
                                    const FilterlessBindData &bind) {
	auto positive_bound = ExactClippingBound(positive_bin, bind);
	auto negative_bound = ExactClippingBound(negative_bin, bind);
	hugeint_t result(0);
	for (int i = 0; i < FILTERLESS_EXACT_BIN_COUNT; i++) {
		if (state.positive) {
			result = i <= positive_bin
			             ? Hugeint::Add(result, state.positive[i].answer_sum)
			             : AddRepeatedBound(result, positive_bound, state.positive[i].answer_count, false);
		}
		if (state.negative) {
			result = i <= negative_bin ? Hugeint::Add(result, state.negative[i].answer_sum)
			                           : AddRepeatedBound(result, negative_bound, state.negative[i].answer_count, true);
		}
	}
	return result;
}

struct FilterlessExactResult {
	hugeint_t clipped_value;
	double noise_scale;
};

static FilterlessExactResult FinalizeExactComponent(const FilterlessExactComponentState &state,
                                                    const FilterlessBindData &bind, uint64_t nonce_base, double epsilon,
                                                    bool nonnegative) {
	double histogram_epsilon = epsilon * bind.bounds_fraction;
	double value_epsilon = epsilon * (1.0 - bind.bounds_fraction);
	double ignored_support;
	idx_t bin_count = bind.exact_bin_count;
	int positive_bin =
	    FindSupportedBin(state.positive, bin_count, bind, nonce_base, histogram_epsilon, ignored_support);
	int negative_bin = nonnegative ? -1
	                               : FindSupportedBin(state.negative, bin_count, bind, nonce_base + bin_count,
	                                                  histogram_epsilon, ignored_support);
	double bound = std::max(ExactBinUpperBound(negative_bin), ExactBinUpperBound(positive_bin));
	return {ClipExactComponent(state, negative_bin, positive_bin, bind),
	        SaturatingNoiseScale(static_cast<long double>(bound) * bind.max_groups, value_epsilon)};
}

static idx_t FilterlessStateSize(const AggregateFunction &) {
	return sizeof(FilterlessState);
}

static idx_t FilterlessAvgStateSize(const AggregateFunction &) {
	return sizeof(FilterlessAvgState);
}

static idx_t FilterlessExactStateSize(const AggregateFunction &) {
	return sizeof(FilterlessExactState);
}

static void FilterlessInitialize(const AggregateFunction &, data_ptr_t state_p) {
	memset(state_p, 0, sizeof(FilterlessState));
}

static void FilterlessAvgInitialize(const AggregateFunction &, data_ptr_t state_p) {
	memset(state_p, 0, sizeof(FilterlessAvgState));
}

static void FilterlessExactInitialize(const AggregateFunction &, data_ptr_t state_p) {
	memset(state_p, 0, sizeof(FilterlessExactState));
}

static void SetFilterlessNonce(uint64_t value, uint64_t &nonce, bool &nonce_set) {
	if (nonce_set && nonce != value) {
		throw InvalidInputException("filterless: noise nonce must be constant within each aggregate group");
	}
	nonce = value;
	nonce_set = true;
}

template <idx_t VALUE_COUNT, class INPUT_TYPE>
struct FilterlessInputVectors {
	UnifiedVectorFormat pu;
	UnifiedVectorFormat active;
	UnifiedVectorFormat values[VALUE_COUNT];
	UnifiedVectorFormat nonce;
	const uint64_t *pu_values;
	const bool *active_values;
	const INPUT_TYPE *numeric_values[VALUE_COUNT];
	const uint64_t *nonce_values;

	FilterlessInputVectors(Vector inputs[], idx_t count, bool has_explicit_config) : nonce_values(nullptr) {
		inputs[0].ToUnifiedFormat(count, pu);
		inputs[1].ToUnifiedFormat(count, active);
		pu_values = UnifiedVectorFormat::GetData<uint64_t>(pu);
		active_values = UnifiedVectorFormat::GetData<bool>(active);
		for (idx_t i = 0; i < VALUE_COUNT; i++) {
			inputs[2 + i].ToUnifiedFormat(count, values[i]);
			numeric_values[i] = UnifiedVectorFormat::GetData<INPUT_TYPE>(values[i]);
		}
		if (has_explicit_config) {
			inputs[VALUE_COUNT + 4].ToUnifiedFormat(count, nonce);
			nonce_values = UnifiedVectorFormat::GetData<uint64_t>(nonce);
		}
	}

	bool RequiredValuesAreValid(idx_t row, bool has_explicit_config) const {
		auto pu_index = pu.sel->get_index(row);
		auto active_index = active.sel->get_index(row);
		if (!pu.validity.RowIsValid(pu_index) || !active.validity.RowIsValid(active_index)) {
			return false;
		}
		return !has_explicit_config || nonce.validity.RowIsValid(nonce.sel->get_index(row));
	}

	bool ValueIsValid(idx_t value_index, idx_t row) const {
		auto index = values[value_index].sel->get_index(row);
		return values[value_index].validity.RowIsValid(index);
	}

	INPUT_TYPE ValueOrZero(idx_t value_index, idx_t row) const {
		if (!ValueIsValid(value_index, row)) {
			return INPUT_TYPE(0);
		}
		return numeric_values[value_index][values[value_index].sel->get_index(row)];
	}
};

static void UpdateFilterlessStateRow(FilterlessState &state, const FilterlessInputVectors<2, double> &input, idx_t row,
                                     const FilterlessBindData &bind, ArenaAllocator &allocator) {
	auto pu_index = input.pu.sel->get_index(row);
	auto active_index = input.active.sel->get_index(row);
	UpdateComponent(state.component, input.pu_values[pu_index], input.active_values[active_index],
	                input.ValueIsValid(0, row), input.ValueOrZero(0, row), input.ValueIsValid(1, row),
	                input.ValueOrZero(1, row), bind, allocator, bind.approximate_values);
}

static void UpdateFilterlessStateRow(FilterlessAvgState &state, const FilterlessInputVectors<4, double> &input,
                                     idx_t row, const FilterlessBindData &bind, ArenaAllocator &allocator) {
	auto pu_index = input.pu.sel->get_index(row);
	auto active_index = input.active.sel->get_index(row);
	UpdateComponent(state.sum_component, input.pu_values[pu_index], input.active_values[active_index],
	                input.ValueIsValid(0, row), input.ValueOrZero(0, row), input.ValueIsValid(2, row),
	                input.ValueOrZero(2, row), bind, allocator, true);
	UpdateComponent(state.count_component, input.pu_values[pu_index], input.active_values[active_index],
	                input.ValueIsValid(1, row), input.ValueOrZero(1, row), input.ValueIsValid(3, row),
	                input.ValueOrZero(3, row), bind, allocator, false);
}

template <class INPUT_TYPE>
static void UpdateFilterlessStateRow(FilterlessExactState &state, const FilterlessInputVectors<2, INPUT_TYPE> &input,
                                     idx_t row, const FilterlessBindData &bind, ArenaAllocator &allocator) {
	auto pu_index = input.pu.sel->get_index(row);
	auto active_index = input.active.sel->get_index(row);
	UpdateExactComponent(state.component, input.pu_values[pu_index], input.active_values[active_index],
	                     input.ValueIsValid(0, row), input.ValueOrZero(0, row), input.ValueIsValid(1, row),
	                     input.ValueOrZero(1, row), bind, allocator);
}

template <idx_t VALUE_COUNT, class INPUT_TYPE, class STATE_GETTER>
static void FilterlessUpdateRows(Vector inputs[], AggregateInputData &aggr, idx_t count, STATE_GETTER get_state) {
	auto &bind = aggr.bind_data->Cast<FilterlessBindData>();
	FilterlessInputVectors<VALUE_COUNT, INPUT_TYPE> input(inputs, count, bind.has_explicit_config);
	for (idx_t row = 0; row < count; row++) {
		if (!input.RequiredValuesAreValid(row, bind.has_explicit_config)) {
			continue;
		}
		auto state = get_state(row);
		if (bind.has_explicit_config) {
			auto nonce_index = input.nonce.sel->get_index(row);
			SetFilterlessNonce(input.nonce_values[nonce_index], state->nonce, state->nonce_set);
		}
		UpdateFilterlessStateRow(*state, input, row, bind, aggr.allocator);
	}
}

static void FilterlessUpdate(Vector inputs[], AggregateInputData &aggr, idx_t, data_ptr_t state_p, idx_t count) {
	auto state = reinterpret_cast<FilterlessState *>(state_p);
	FilterlessUpdateRows<2, double>(inputs, aggr, count, [state](idx_t) { return state; });
}

static void FilterlessScatterUpdate(Vector inputs[], AggregateInputData &aggr, idx_t, Vector &states, idx_t count) {
	UnifiedVectorFormat state_data;
	states.ToUnifiedFormat(count, state_data);
	auto state_ptrs = UnifiedVectorFormat::GetData<FilterlessState *>(state_data);
	FilterlessUpdateRows<2, double>(inputs, aggr, count,
	                                [&](idx_t row) { return state_ptrs[state_data.sel->get_index(row)]; });
}

static void FilterlessAvgUpdate(Vector inputs[], AggregateInputData &aggr, idx_t, data_ptr_t state_p, idx_t count) {
	auto state = reinterpret_cast<FilterlessAvgState *>(state_p);
	FilterlessUpdateRows<4, double>(inputs, aggr, count, [state](idx_t) { return state; });
}

static void FilterlessAvgScatterUpdate(Vector inputs[], AggregateInputData &aggr, idx_t, Vector &states, idx_t count) {
	UnifiedVectorFormat state_data;
	states.ToUnifiedFormat(count, state_data);
	auto state_ptrs = UnifiedVectorFormat::GetData<FilterlessAvgState *>(state_data);
	FilterlessUpdateRows<4, double>(inputs, aggr, count,
	                                [&](idx_t row) { return state_ptrs[state_data.sel->get_index(row)]; });
}

template <class INPUT_TYPE>
static void FilterlessExactUpdate(Vector inputs[], AggregateInputData &aggr, idx_t, data_ptr_t state_p, idx_t count) {
	auto state = reinterpret_cast<FilterlessExactState *>(state_p);
	FilterlessUpdateRows<2, INPUT_TYPE>(inputs, aggr, count, [state](idx_t) { return state; });
}

template <class INPUT_TYPE>
static void FilterlessExactScatterUpdate(Vector inputs[], AggregateInputData &aggr, idx_t, Vector &states,
                                         idx_t count) {
	UnifiedVectorFormat state_data;
	states.ToUnifiedFormat(count, state_data);
	auto state_ptrs = UnifiedVectorFormat::GetData<FilterlessExactState *>(state_data);
	FilterlessUpdateRows<2, INPUT_TYPE>(inputs, aggr, count,
	                                    [&](idx_t row) { return state_ptrs[state_data.sel->get_index(row)]; });
}

static void FilterlessCombine(Vector &source, Vector &target, AggregateInputData &input, idx_t count) {
	auto sources = FlatVector::GetData<FilterlessState *>(source);
	auto targets = FlatVector::GetData<FilterlessState *>(target);
	for (idx_t i = 0; i < count; i++) {
		CombineComponent(sources[i]->component, targets[i]->component, input.allocator);
		if (sources[i]->nonce_set) {
			SetFilterlessNonce(sources[i]->nonce, targets[i]->nonce, targets[i]->nonce_set);
		}
	}
}

static void FilterlessAvgCombine(Vector &source, Vector &target, AggregateInputData &input, idx_t count) {
	auto sources = FlatVector::GetData<FilterlessAvgState *>(source);
	auto targets = FlatVector::GetData<FilterlessAvgState *>(target);
	for (idx_t i = 0; i < count; i++) {
		CombineComponent(sources[i]->sum_component, targets[i]->sum_component, input.allocator);
		CombineComponent(sources[i]->count_component, targets[i]->count_component, input.allocator);
		if (sources[i]->nonce_set) {
			SetFilterlessNonce(sources[i]->nonce, targets[i]->nonce, targets[i]->nonce_set);
		}
	}
}

static void FilterlessExactCombine(Vector &source, Vector &target, AggregateInputData &input, idx_t count) {
	auto sources = FlatVector::GetData<FilterlessExactState *>(source);
	auto targets = FlatVector::GetData<FilterlessExactState *>(target);
	for (idx_t i = 0; i < count; i++) {
		CombineExactComponent(sources[i]->component, targets[i]->component, input.allocator);
		if (sources[i]->nonce_set) {
			SetFilterlessNonce(sources[i]->nonce, targets[i]->nonce, targets[i]->nonce_set);
		}
	}
}

static void WriteDebugResult(Vector &result, idx_t row, const FilterlessResult &value) {
	auto &children = StructVector::GetEntries(result);
	FlatVector::GetData<double>(*children[0])[row] = value.lower_bound;
	FlatVector::GetData<double>(*children[1])[row] = value.upper_bound;
	FlatVector::GetData<double>(*children[2])[row] = value.clipped_value;
	FlatVector::GetData<double>(*children[3])[row] = value.noise_scale;
	FlatVector::GetData<int32_t>(*children[4])[row] = value.negative_bin;
	FlatVector::GetData<int32_t>(*children[5])[row] = value.positive_bin;
	FlatVector::GetData<double>(*children[6])[row] = value.negative_support;
	FlatVector::GetData<double>(*children[7])[row] = value.positive_support;
	FlatVector::GetData<uint64_t>(*children[8])[row] = value.active_contributions;
	FlatVector::GetData<uint64_t>(*children[9])[row] = value.sampled_contributions;
}

static void WriteAvgDebugResult(Vector &result, idx_t row, const FilterlessResult &sum,
                                const FilterlessResult &denominator, double released_average) {
	auto &children = StructVector::GetEntries(result);
	FlatVector::GetData<double>(*children[0])[row] = sum.lower_bound;
	FlatVector::GetData<double>(*children[1])[row] = sum.upper_bound;
	FlatVector::GetData<double>(*children[2])[row] = sum.clipped_value;
	FlatVector::GetData<double>(*children[3])[row] = sum.noise_scale;
	FlatVector::GetData<double>(*children[4])[row] = denominator.upper_bound;
	FlatVector::GetData<double>(*children[5])[row] = denominator.clipped_value;
	FlatVector::GetData<double>(*children[6])[row] = denominator.noise_scale;
	FlatVector::GetData<double>(*children[7])[row] = released_average;
	FlatVector::GetData<int32_t>(*children[8])[row] = sum.negative_bin;
	FlatVector::GetData<int32_t>(*children[9])[row] = sum.positive_bin;
	FlatVector::GetData<int32_t>(*children[10])[row] = denominator.positive_bin;
	FlatVector::GetData<uint64_t>(*children[11])[row] = sum.active_contributions;
	FlatVector::GetData<uint64_t>(*children[12])[row] = sum.sampled_contributions;
}

template <bool DEBUG, bool COUNT>
static void FilterlessFinalize(Vector &states, AggregateInputData &input, Vector &result, idx_t count, idx_t offset) {
	auto state_ptrs = FlatVector::GetData<FilterlessState *>(states);
	auto &bind = input.bind_data->Cast<FilterlessBindData>();
	auto result_data = DEBUG ? nullptr : FlatVector::GetData<double>(result);
	for (idx_t i = 0; i < count; i++) {
		uint64_t nonce = state_ptrs[i]->nonce_set ? state_ptrs[i]->nonce : 0;
		auto value = FinalizeComponent(state_ptrs[i]->component, bind, nonce * 1024, bind.epsilon, COUNT);
		if (DEBUG) {
			WriteDebugResult(result, offset + i, value);
		} else {
			result_data[offset + i] =
			    bind.noise_enabled ? AddDpLaplaceNoise(value.clipped_value, value.noise_scale) : value.clipped_value;
		}
	}
}

template <bool DEBUG>
static void FilterlessAvgFinalize(Vector &states, AggregateInputData &input, Vector &result, idx_t count,
                                  idx_t offset) {
	auto state_ptrs = FlatVector::GetData<FilterlessAvgState *>(states);
	auto &bind = input.bind_data->Cast<FilterlessBindData>();
	auto result_data = DEBUG ? nullptr : FlatVector::GetData<double>(result);
	for (idx_t i = 0; i < count; i++) {
		uint64_t nonce = state_ptrs[i]->nonce_set ? state_ptrs[i]->nonce : 0;
		double component_epsilon = bind.epsilon / 2.0;
		auto sum = FinalizeComponent(state_ptrs[i]->sum_component, bind, nonce * 2048, component_epsilon, false);
		auto denominator = FinalizeComponent(state_ptrs[i]->count_component, bind,
		                                     nonce * 2048 + 2 * CLIP_NUM_LEVELS_64 + 1, component_epsilon, true);
		double noised_sum =
		    bind.noise_enabled ? AddDpLaplaceNoise(sum.clipped_value, sum.noise_scale) : sum.clipped_value;
		double noised_count = bind.noise_enabled ? AddDpLaplaceNoise(denominator.clipped_value, denominator.noise_scale)
		                                         : denominator.clipped_value;
		double average = noised_count > 0.0 ? noised_sum / noised_count : 0.0;
		if (DEBUG) {
			WriteAvgDebugResult(result, offset + i, sum, denominator, average);
		} else {
			result_data[offset + i] = average;
		}
	}
}

template <class OUTPUT_TYPE>
static OUTPUT_TYPE CastExactResult(hugeint_t value) {
	OUTPUT_TYPE result;
	if (!Hugeint::TryCast(value, result)) {
		return value < 0 ? NumericLimits<OUTPUT_TYPE>::Minimum() : NumericLimits<OUTPUT_TYPE>::Maximum();
	}
	return result;
}

template <>
hugeint_t CastExactResult<hugeint_t>(hugeint_t value) {
	return value;
}

static hugeint_t SaturatingHugeintFromDouble(double value) {
	if (std::isnan(value)) {
		return hugeint_t(0);
	}
	hugeint_t result;
	if (Hugeint::TryConvert(value, result)) {
		return result;
	}
	return std::signbit(value) ? NumericLimits<hugeint_t>::Minimum() : NumericLimits<hugeint_t>::Maximum();
}

static hugeint_t SaturatingHugeintAdd(hugeint_t left, hugeint_t right) {
	auto result = left;
	if (Hugeint::TryAddInPlace(result, right)) {
		return result;
	}
	return right < 0 ? NumericLimits<hugeint_t>::Minimum() : NumericLimits<hugeint_t>::Maximum();
}

static hugeint_t ClampExactResult(hugeint_t value, const FilterlessBindData &bind) {
	return std::max(bind.exact_output_min, std::min(value, bind.exact_output_max));
}

template <class OUTPUT_TYPE, bool COUNT>
static void FilterlessExactFinalize(Vector &states, AggregateInputData &input, Vector &result, idx_t count,
                                    idx_t offset) {
	auto state_ptrs = FlatVector::GetData<FilterlessExactState *>(states);
	auto result_data = FlatVector::GetData<OUTPUT_TYPE>(result);
	auto &bind = input.bind_data->Cast<FilterlessBindData>();
	for (idx_t i = 0; i < count; i++) {
		uint64_t nonce = state_ptrs[i]->nonce_set ? state_ptrs[i]->nonce : 0;
		auto value = FinalizeExactComponent(state_ptrs[i]->component, bind, nonce * 1024, bind.epsilon, COUNT);
		auto released = value.clipped_value;
		if (bind.noise_enabled) {
			double noise = AddDpLaplaceNoise(0.0, value.noise_scale);
			auto scaled_noise = SaturatingHugeintFromDouble(noise * bind.input_scale);
			released = SaturatingHugeintAdd(released, scaled_noise);
		}
		result_data[offset + i] = CastExactResult<OUTPUT_TYPE>(ClampExactResult(released, bind));
	}
}

// Internal scalar form of the as_clip_sum magnitude accumulator. The compiler
// uses it for the per-PU floating SUM below the filterless aggregate, so an
// unstable ordinary DOUBLE SUM cannot erase small contributions before the
// contribution bound is applied.
struct FilterlessApproxSumState {
	bool isset;
	hugeint_t positive;
	hugeint_t negative;
};

struct FilterlessApproxSumOperation {
	template <class STATE>
	static void Initialize(STATE &state) {
		state.isset = false;
		state.positive = hugeint_t(0);
		state.negative = hugeint_t(0);
	}

	static hugeint_t ApproximateScaledValue(double value) {
		return Hugeint::Convert(ClipApproximateMagnitude64(AsScaledMagnitude(value)));
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &) {
		double value = static_cast<double>(input);
		if (!std::isfinite(value)) {
			throw InvalidInputException("filterless: per-PU SUM contribution must be finite");
		}
		state.isset = true;
		auto scaled = ApproximateScaledValue(value);
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
			throw InvalidInputException("filterless: per-PU SUM contribution must be finite");
		}
		state.isset = true;
		auto total = Hugeint::Multiply(ApproximateScaledValue(value), Hugeint::Convert(count));
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

static LogicalType FilterlessDebugType() {
	child_list_t<LogicalType> children;
	children.emplace_back("lower_bound", LogicalType::DOUBLE);
	children.emplace_back("upper_bound", LogicalType::DOUBLE);
	children.emplace_back("clipped_value", LogicalType::DOUBLE);
	children.emplace_back("noise_scale", LogicalType::DOUBLE);
	children.emplace_back("negative_bin", LogicalType::INTEGER);
	children.emplace_back("positive_bin", LogicalType::INTEGER);
	children.emplace_back("negative_support", LogicalType::DOUBLE);
	children.emplace_back("positive_support", LogicalType::DOUBLE);
	children.emplace_back("active_contributions", LogicalType::UBIGINT);
	children.emplace_back("sampled_contributions", LogicalType::UBIGINT);
	return LogicalType::STRUCT(std::move(children));
}

static LogicalType FilterlessAvgDebugType() {
	child_list_t<LogicalType> children;
	children.emplace_back("sum_lower_bound", LogicalType::DOUBLE);
	children.emplace_back("sum_upper_bound", LogicalType::DOUBLE);
	children.emplace_back("clipped_sum", LogicalType::DOUBLE);
	children.emplace_back("sum_noise_scale", LogicalType::DOUBLE);
	children.emplace_back("count_upper_bound", LogicalType::DOUBLE);
	children.emplace_back("clipped_count", LogicalType::DOUBLE);
	children.emplace_back("count_noise_scale", LogicalType::DOUBLE);
	children.emplace_back("released_average", LogicalType::DOUBLE);
	children.emplace_back("sum_negative_bin", LogicalType::INTEGER);
	children.emplace_back("sum_positive_bin", LogicalType::INTEGER);
	children.emplace_back("count_positive_bin", LogicalType::INTEGER);
	children.emplace_back("active_contributions", LogicalType::UBIGINT);
	children.emplace_back("sampled_contributions", LogicalType::UBIGINT);
	return LogicalType::STRUCT(std::move(children));
}

template <class INPUT_TYPE, class OUTPUT_TYPE, bool COUNT>
static AggregateFunction MakeFilterlessExactFunction(const string &name, const LogicalType &input_type,
                                                     const LogicalType &return_type, bool explicit_config) {
	vector<LogicalType> arguments = {LogicalType::UBIGINT, LogicalType::BOOLEAN, input_type, input_type};
	if (explicit_config) {
		arguments.push_back(LogicalType::DOUBLE);
		arguments.push_back(LogicalType::DOUBLE);
		arguments.push_back(LogicalType::UBIGINT);
	}
	return AggregateFunction(name, std::move(arguments), return_type, FilterlessExactStateSize,
	                         FilterlessExactInitialize, FilterlessExactScatterUpdate<INPUT_TYPE>,
	                         FilterlessExactCombine, FilterlessExactFinalize<OUTPUT_TYPE, COUNT>,
	                         FunctionNullHandling::SPECIAL_HANDLING, FilterlessExactUpdate<INPUT_TYPE>,
	                         COUNT ? BindFilterlessCount : BindFilterlessSum);
}

template <class INPUT_TYPE, class OUTPUT_TYPE, bool COUNT>
static void AddFilterlessExactOverloads(AggregateFunctionSet &set, const string &name, const LogicalType &input_type,
                                        const LogicalType &return_type) {
	set.AddFunction(MakeFilterlessExactFunction<INPUT_TYPE, OUTPUT_TYPE, COUNT>(name, input_type, return_type, false));
	set.AddFunction(MakeFilterlessExactFunction<INPUT_TYPE, OUTPUT_TYPE, COUNT>(name, input_type, return_type, true));
}

static AggregateFunction MakeFilterlessDecimalSumFunction(const LogicalType &input_type, const LogicalType &return_type,
                                                          bool explicit_config) {
	switch (input_type.InternalType()) {
	case PhysicalType::INT16:
		return MakeFilterlessExactFunction<int16_t, hugeint_t, false>("filterless_sum", input_type, return_type,
		                                                              explicit_config);
	case PhysicalType::INT32:
		return MakeFilterlessExactFunction<int32_t, hugeint_t, false>("filterless_sum", input_type, return_type,
		                                                              explicit_config);
	case PhysicalType::INT64:
		return MakeFilterlessExactFunction<int64_t, hugeint_t, false>("filterless_sum", input_type, return_type,
		                                                              explicit_config);
	case PhysicalType::INT128:
		return MakeFilterlessExactFunction<hugeint_t, hugeint_t, false>("filterless_sum", input_type, return_type,
		                                                                explicit_config);
	default:
		throw InternalException("filterless_sum: unsupported DECIMAL physical type");
	}
}

static unique_ptr<FunctionData> BindFilterlessDecimalSum(ClientContext &context, AggregateFunction &function,
                                                         vector<unique_ptr<Expression>> &arguments) {
	if (arguments.size() != 4 && arguments.size() != 7) {
		throw InternalException("filterless_sum: unexpected DECIMAL argument count");
	}
	auto input_type = arguments[2]->return_type;
	if (arguments[3]->return_type != input_type) {
		throw InvalidInputException("filterless_sum: answer and histogram DECIMAL types must match");
	}
	auto return_type = LogicalType::DECIMAL(Decimal::MAX_WIDTH_DECIMAL, DecimalType::GetScale(input_type));
	function = MakeFilterlessDecimalSumFunction(input_type, return_type, arguments.size() == 7);
	auto result = BindFilterless(context, arguments, 4, false, FILTERLESS_HUGEINT_BIN_COUNT);
	auto &bind = result->Cast<FilterlessBindData>();
	bind.input_scale = std::pow(10.0, DecimalType::GetScale(input_type));
	bind.exact_output_max = Hugeint::Subtract(Hugeint::POWERS_OF_TEN[Decimal::MAX_WIDTH_DECIMAL], hugeint_t(1));
	bind.exact_output_min = Hugeint::Negate(bind.exact_output_max);
	return result;
}

static void AddSumCountOverloads(AggregateFunctionSet &set, const string &name, aggregate_finalize_t finalize,
                                 const LogicalType &return_type, bind_aggregate_function_t bind) {
	set.AddFunction(
	    AggregateFunction(name, {LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                      return_type, FilterlessStateSize, FilterlessInitialize, FilterlessScatterUpdate,
	                      FilterlessCombine, finalize, FunctionNullHandling::SPECIAL_HANDLING, FilterlessUpdate, bind));
	set.AddFunction(
	    AggregateFunction(name,
	                      {LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalType::DOUBLE, LogicalType::DOUBLE,
	                       LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::UBIGINT},
	                      return_type, FilterlessStateSize, FilterlessInitialize, FilterlessScatterUpdate,
	                      FilterlessCombine, finalize, FunctionNullHandling::SPECIAL_HANDLING, FilterlessUpdate, bind));
}

static void AddAvgOverloads(AggregateFunctionSet &set, const string &name, aggregate_finalize_t finalize,
                            const LogicalType &return_type) {
	set.AddFunction(AggregateFunction(name,
	                                  {LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalType::DOUBLE,
	                                   LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                                  return_type, FilterlessAvgStateSize, FilterlessAvgInitialize,
	                                  FilterlessAvgScatterUpdate, FilterlessAvgCombine, finalize,
	                                  FunctionNullHandling::SPECIAL_HANDLING, FilterlessAvgUpdate, BindFilterlessAvg));
	set.AddFunction(AggregateFunction(
	    name,
	    {LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE,
	     LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::UBIGINT},
	    return_type, FilterlessAvgStateSize, FilterlessAvgInitialize, FilterlessAvgScatterUpdate, FilterlessAvgCombine,
	    finalize, FunctionNullHandling::SPECIAL_HANDLING, FilterlessAvgUpdate, BindFilterlessAvg));
}

void RegisterFilterlessAggregateFunctions(ExtensionLoader &loader) {
	auto approx_sum =
	    AggregateFunction::UnaryAggregate<FilterlessApproxSumState, double, double, FilterlessApproxSumOperation>(
	        LogicalType::DOUBLE, LogicalType::DOUBLE);
	approx_sum.name = "filterless_approx_sum";
	CreateAggregateFunctionInfo approx_sum_info(approx_sum);
	FunctionDescription approx_sum_description;
	approx_sum_description.description =
	    "[INTERNAL] Scalar AS magnitude sum used by dp_filterless per-PU pre-aggregation.";
	approx_sum_info.descriptions.push_back(std::move(approx_sum_description));
	loader.RegisterFunction(std::move(approx_sum_info));

	AggregateFunctionSet sum_set("filterless_sum");
	AddSumCountOverloads(sum_set, "filterless_sum", FilterlessFinalize<false, false>, LogicalType::DOUBLE,
	                     BindFilterlessSum);
	AddFilterlessExactOverloads<int64_t, hugeint_t, false>(sum_set, "filterless_sum", LogicalType::BIGINT,
	                                                       LogicalType::HUGEINT);
	AddFilterlessExactOverloads<hugeint_t, hugeint_t, false>(sum_set, "filterless_sum", LogicalType::HUGEINT,
	                                                         LogicalType::HUGEINT);
	sum_set.AddFunction(
	    AggregateFunction({LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalTypeId::DECIMAL, LogicalTypeId::DECIMAL},
	                      LogicalTypeId::DECIMAL, nullptr, nullptr, nullptr, nullptr, nullptr,
	                      FunctionNullHandling::SPECIAL_HANDLING, nullptr, BindFilterlessDecimalSum));
	sum_set.AddFunction(
	    AggregateFunction({LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalTypeId::DECIMAL, LogicalTypeId::DECIMAL,
	                       LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::UBIGINT},
	                      LogicalTypeId::DECIMAL, nullptr, nullptr, nullptr, nullptr, nullptr,
	                      FunctionNullHandling::SPECIAL_HANDLING, nullptr, BindFilterlessDecimalSum));
	CreateAggregateFunctionInfo sum_info(sum_set);
	FunctionDescription sum_description;
	sum_description.description =
	    "Filterless clipped SUM with separate filtered-answer and fixed-sample histogram contributions.";
	sum_info.descriptions.push_back(std::move(sum_description));
	loader.RegisterFunction(std::move(sum_info));

	AggregateFunctionSet count_set("filterless_count");
	AddSumCountOverloads(count_set, "filterless_count", FilterlessFinalize<false, true>, LogicalType::DOUBLE,
	                     BindFilterlessCount);
	AddFilterlessExactOverloads<int64_t, int64_t, true>(count_set, "filterless_count", LogicalType::BIGINT,
	                                                    LogicalType::BIGINT);
	CreateAggregateFunctionInfo count_info(count_set);
	FunctionDescription count_description;
	count_description.description =
	    "Filterless clipped COUNT with separate filtered-answer and fixed-sample histogram contributions.";
	count_info.descriptions.push_back(std::move(count_description));
	loader.RegisterFunction(std::move(count_info));

	AggregateFunctionSet avg_set("filterless_avg");
	AddAvgOverloads(avg_set, "filterless_avg", FilterlessAvgFinalize<false>, LogicalType::DOUBLE);
	CreateAggregateFunctionInfo avg_info(avg_set);
	FunctionDescription avg_description;
	avg_description.description = "Filterless AVG with separate filtered-answer and fixed-sample histogram partials.";
	avg_info.descriptions.push_back(std::move(avg_description));
	loader.RegisterFunction(std::move(avg_info));

	auto debug_type = FilterlessDebugType();
	AggregateFunctionSet sum_debug_set("filterless_sum_debug");
	AddSumCountOverloads(sum_debug_set, "filterless_sum_debug", FilterlessFinalize<true, false>, debug_type,
	                     BindFilterlessSum);
	loader.RegisterFunction(CreateAggregateFunctionInfo(sum_debug_set));

	AggregateFunctionSet count_debug_set("filterless_count_debug");
	AddSumCountOverloads(count_debug_set, "filterless_count_debug", FilterlessFinalize<true, true>, debug_type,
	                     BindFilterlessCount);
	loader.RegisterFunction(CreateAggregateFunctionInfo(count_debug_set));

	AggregateFunctionSet avg_debug_set("filterless_avg_debug");
	AddAvgOverloads(avg_debug_set, "filterless_avg_debug", FilterlessAvgFinalize<true>, FilterlessAvgDebugType());
	loader.RegisterFunction(CreateAggregateFunctionInfo(avg_debug_set));
}

} // namespace duckdb
