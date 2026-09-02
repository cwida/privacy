#include "aggregates/filterless_aggregate.hpp"

#include "aggregates/as_clip_aggr.hpp"
#include "aggregates/dp_laplace_noise.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/uhugeint.hpp"
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
};

struct FilterlessAvgState {
	FilterlessComponentState sum_component;
	FilterlessComponentState count_component;
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

// A DuckDB relation cannot contain more than 2^64-1 rows, and one exact input has magnitude at most 2^127.
// Three 64-bit limbs therefore hold every possible prefix sum exactly. The helpers below use the same bits as
// either a two's-complement signed value or a non-negative magnitude.
struct FilterlessWideValue {
	uhugeint_t lower;
	uint64_t upper;
};

struct FilterlessExactOverflowNode {
	FilterlessWideValue magnitude;
	FilterlessExactOverflowNode *next;
	uint8_t bin_index;
	bool negative;
};

struct FilterlessExactComponentState {
	FilterlessExactBin *positive;
	FilterlessExactBin *negative;
	FilterlessExactOverflowNode *overflow_bins;
	uint64_t active_contributions;
	uint64_t sampled_contributions;
};

struct FilterlessExactState {
	FilterlessExactComponentState component;
};

struct FilterlessBindData : public FunctionData {
	int sample_bits;
	double sample_weight;
	double clip_support;
	bool noise_enabled;
	double epsilon;
	double bounds_fraction;
	double max_groups;
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
		       input_scale == other->input_scale && approximate_values == other->approximate_values &&
		       exact_bin_count == other->exact_bin_count && exact_output_min == other->exact_output_min &&
		       exact_output_max == other->exact_output_max;
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
	int bit_width = 64 - pac_clzll(scaled_magnitude);
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

static void UpdateComponent(FilterlessComponentState &state, bool active, bool sampled, bool answer_valid,
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
	if (sampled && histogram_valid) {
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
static int FindSupportedBin(const BIN_TYPE *bins, idx_t bin_count, const FilterlessBindData &bind,
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
                                          double epsilon, bool nonnegative) {
	double histogram_epsilon = epsilon * bind.bounds_fraction;
	double value_epsilon = epsilon * (1.0 - bind.bounds_fraction);
	double negative_support = 0.0;
	double positive_support = 0.0;
	int positive_bin = FindSupportedBin(state.positive, CLIP_NUM_LEVELS_64, bind, histogram_epsilon, positive_support);
	int negative_bin =
	    nonnegative ? -1
	                : FindSupportedBin(state.negative, CLIP_NUM_LEVELS_64, bind, histogram_epsilon, negative_support);
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

static idx_t ExactBinIndex(hugeint_t value, double input_scale) {
	double magnitude = std::abs(Hugeint::Cast<double>(value)) / input_scale;
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

static uhugeint_t ExactMagnitude(hugeint_t value) {
	auto bits = static_cast<uhugeint_t>(value);
	return value < 0 ? Uhugeint::Negate(bits) : bits;
}

static void WideAddBits(FilterlessWideValue &target, const FilterlessWideValue &value) {
	auto previous = target.lower;
	target.lower = target.lower + value.lower;
	target.upper += value.upper;
	if (target.lower < previous) {
		target.upper++;
	}
}

static void WideSubtractBits(FilterlessWideValue &target, const FilterlessWideValue &value) {
	auto previous = target.lower;
	target.lower = target.lower - value.lower;
	target.upper -= value.upper;
	if (previous < value.lower) {
		target.upper--;
	}
}

static void WideAddSigned(FilterlessWideValue &target, hugeint_t value) {
	FilterlessWideValue extended {static_cast<uhugeint_t>(value), value < 0 ? NumericLimits<uint64_t>::Maximum() : 0};
	WideAddBits(target, extended);
}

static void WideAddMagnitude(FilterlessWideValue &target, const FilterlessWideValue &magnitude) {
	D_ASSERT((target.upper & (uint64_t(1) << 63)) == 0);
	WideAddBits(target, magnitude);
	D_ASSERT((target.upper & (uint64_t(1) << 63)) == 0);
}

static void WideAddMagnitude(FilterlessWideValue &target, uhugeint_t magnitude) {
	WideAddMagnitude(target, FilterlessWideValue {magnitude, 0});
}

static void WideAccumulateMagnitude(FilterlessWideValue &target, const FilterlessWideValue &magnitude, bool negative) {
	if (negative) {
		WideSubtractBits(target, magnitude);
	} else {
		WideAddBits(target, magnitude);
	}
}

static hugeint_t WideFinalize(const FilterlessWideValue &value, hugeint_t minimum, hugeint_t maximum) {
	bool negative = (value.upper & (uint64_t(1) << 63)) != 0;
	auto magnitude = value;
	if (negative) {
		magnitude = {};
		WideSubtractBits(magnitude, value);
	}
	auto limit = ExactMagnitude(negative ? minimum : maximum);
	if (magnitude.upper != 0 || magnitude.lower > limit) {
		return negative ? minimum : maximum;
	}
	if (negative && magnitude.lower == ExactMagnitude(minimum)) {
		return minimum;
	}
	hugeint_t result;
	if (!Uhugeint::TryCast(magnitude.lower, result)) {
		return negative ? minimum : maximum;
	}
	return negative ? Hugeint::Negate(result) : result;
}

static void WideAddRepeated(FilterlessWideValue &target, hugeint_t value, uint64_t count, bool negative) {
	D_ASSERT(value >= 0);
	hugeint_t product;
	if (Hugeint::TryMultiply(value, Hugeint::Convert(count), product)) {
		WideAddSigned(target, negative ? Hugeint::Negate(product) : product);
		return;
	}

	FilterlessWideValue addend {ExactMagnitude(value), 0};
	while (count != 0) {
		if ((count & 1) != 0) {
			WideAccumulateMagnitude(target, addend, negative);
		}
		count >>= 1;
		if (count != 0) {
			auto doubled = addend;
			WideAddMagnitude(addend, doubled);
		}
	}
}

static FilterlessExactBin *EnsureExactBins(FilterlessExactBin *&bins, ArenaAllocator &allocator) {
	if (!bins) {
		bins = reinterpret_cast<FilterlessExactBin *>(
		    allocator.Allocate(sizeof(FilterlessExactBin) * FILTERLESS_EXACT_BIN_COUNT));
		memset(bins, 0, sizeof(FilterlessExactBin) * FILTERLESS_EXACT_BIN_COUNT);
	}
	return bins;
}

static FilterlessExactBin &GetExactBin(FilterlessExactComponentState &state, bool negative, idx_t index,
                                       ArenaAllocator &allocator) {
	return negative ? EnsureExactBins(state.negative, allocator)[index]
	                : EnsureExactBins(state.positive, allocator)[index];
}

static FilterlessExactOverflowNode *FindExactOverflow(const FilterlessExactComponentState &state, bool negative,
                                                      idx_t bin_index) {
	for (auto node = state.overflow_bins; node; node = node->next) {
		if (node->negative == negative && node->bin_index == bin_index) {
			return node;
		}
	}
	return nullptr;
}

static FilterlessExactOverflowNode &PromoteExactBin(FilterlessExactComponentState &state, FilterlessExactBin &bin,
                                                    bool negative, idx_t bin_index, ArenaAllocator &allocator) {
	auto node =
	    reinterpret_cast<FilterlessExactOverflowNode *>(allocator.Allocate(sizeof(FilterlessExactOverflowNode)));
	memset(node, 0, sizeof(FilterlessExactOverflowNode));
	node->negative = negative;
	D_ASSERT(bin_index < FILTERLESS_EXACT_BIN_COUNT);
	node->bin_index = static_cast<uint8_t>(bin_index);
	node->next = state.overflow_bins;
	state.overflow_bins = node;
	WideAddMagnitude(node->magnitude, ExactMagnitude(bin.answer_sum));
	return *node;
}

static void AddExactBinValue(FilterlessExactComponentState &state, FilterlessExactBin &bin, bool negative,
                             idx_t bin_index, hugeint_t value, ArenaAllocator &allocator) {
	auto overflow = FindExactOverflow(state, negative, bin_index);
	if (overflow) {
		WideAddMagnitude(overflow->magnitude, ExactMagnitude(value));
		return;
	}
	auto result = bin.answer_sum;
	if (Hugeint::TryAddInPlace(result, value)) {
		bin.answer_sum = result;
		return;
	}
	auto &wide = PromoteExactBin(state, bin, negative, bin_index, allocator);
	WideAddMagnitude(wide.magnitude, ExactMagnitude(value));
}

static void AddExactBinValue(FilterlessExactComponentState &state, FilterlessExactBin &bin, bool negative,
                             idx_t bin_index, const FilterlessWideValue &value, ArenaAllocator &allocator) {
	auto overflow = FindExactOverflow(state, negative, bin_index);
	if (!overflow) {
		overflow = &PromoteExactBin(state, bin, negative, bin_index, allocator);
	}
	WideAddMagnitude(overflow->magnitude, value);
}

static hugeint_t ToHugeint(hugeint_t value) {
	return value;
}

static hugeint_t ToHugeint(bool value) {
	return Hugeint::Convert(static_cast<int8_t>(value));
}

template <class INPUT_TYPE>
static hugeint_t ToHugeint(INPUT_TYPE value) {
	return Hugeint::Convert(value);
}

template <class INPUT_TYPE>
static void UpdateExactComponent(FilterlessExactComponentState &state, bool active, bool sampled, bool answer_valid,
                                 INPUT_TYPE answer_value, bool histogram_valid, INPUT_TYPE histogram_value,
                                 const FilterlessBindData &bind, ArenaAllocator &allocator, double input_scale) {
	if (active) {
		state.active_contributions++;
		if (answer_valid) {
			auto exact_answer = ToHugeint(answer_value);
			bool negative = exact_answer < 0;
			auto index = ExactBinIndex(exact_answer, input_scale);
			auto &answer_bin = GetExactBin(state, negative, index, allocator);
			AddExactBinValue(state, answer_bin, negative, index, exact_answer, allocator);
			answer_bin.answer_count++;
		}
	}
	if (sampled && histogram_valid) {
		auto exact_histogram = ToHugeint(histogram_value);
		bool negative = exact_histogram < 0;
		auto index = ExactBinIndex(exact_histogram, input_scale);
		GetExactBin(state, negative, index, allocator).support += bind.sample_weight;
		state.sampled_contributions++;
	}
}

static void CombineExactBins(const FilterlessExactComponentState &source_state,
                             FilterlessExactComponentState &target_state, const FilterlessExactBin *source,
                             FilterlessExactBin *&target, bool negative, ArenaAllocator &allocator) {
	if (!source) {
		return;
	}
	auto target_bins = EnsureExactBins(target, allocator);
	for (idx_t i = 0; i < FILTERLESS_EXACT_BIN_COUNT; i++) {
		target_bins[i].support += source[i].support;
		auto source_overflow = FindExactOverflow(source_state, negative, i);
		if (source_overflow) {
			AddExactBinValue(target_state, target_bins[i], negative, i, source_overflow->magnitude, allocator);
		} else if (source[i].answer_count != 0) {
			AddExactBinValue(target_state, target_bins[i], negative, i, source[i].answer_sum, allocator);
		}
		target_bins[i].answer_count += source[i].answer_count;
	}
}

static void CombineExactComponent(const FilterlessExactComponentState &source, FilterlessExactComponentState &target,
                                  ArenaAllocator &allocator) {
	CombineExactBins(source, target, source.positive, target.positive, false, allocator);
	CombineExactBins(source, target, source.negative, target.negative, true, allocator);
	target.active_contributions += source.active_contributions;
	target.sampled_contributions += source.sampled_contributions;
}

static hugeint_t ExactClippingBound(int bin, double input_scale) {
	if (bin < 0) {
		return hugeint_t(0);
	}
	double scaled_bound = std::ceil(ExactBinUpperBound(bin) * input_scale);
	hugeint_t result;
	if (!std::isfinite(scaled_bound) || !Hugeint::TryConvert(scaled_bound, result)) {
		return NumericLimits<hugeint_t>::Maximum();
	}
	return result;
}

static hugeint_t ClipExactComponent(const FilterlessExactComponentState &state, const FilterlessBindData &bind,
                                    int negative_bin, int positive_bin, double input_scale) {
	auto positive_bound = ExactClippingBound(positive_bin, input_scale);
	auto negative_bound = ExactClippingBound(negative_bin, input_scale);
	FilterlessWideValue result {};
	for (int i = 0; i < FILTERLESS_EXACT_BIN_COUNT; i++) {
		if (state.positive) {
			auto overflow = FindExactOverflow(state, false, i);
			if (i <= positive_bin) {
				if (overflow) {
					WideAccumulateMagnitude(result, overflow->magnitude, false);
				} else {
					WideAddSigned(result, state.positive[i].answer_sum);
				}
			} else {
				WideAddRepeated(result, positive_bound, state.positive[i].answer_count, false);
			}
		}
		if (state.negative) {
			auto overflow = FindExactOverflow(state, true, i);
			if (i <= negative_bin) {
				if (overflow) {
					WideAccumulateMagnitude(result, overflow->magnitude, true);
				} else {
					WideAddSigned(result, state.negative[i].answer_sum);
				}
			} else {
				WideAddRepeated(result, negative_bound, state.negative[i].answer_count, true);
			}
		}
	}
	return WideFinalize(result, bind.exact_output_min, bind.exact_output_max);
}

struct FilterlessExactResult {
	hugeint_t clipped_value;
	double noise_scale;
};

static FilterlessExactResult FinalizeExactComponent(const FilterlessExactComponentState &state,
                                                    const FilterlessBindData &bind, double epsilon, bool nonnegative,
                                                    idx_t bin_count, double input_scale) {
	double histogram_epsilon = epsilon * bind.bounds_fraction;
	double value_epsilon = epsilon * (1.0 - bind.bounds_fraction);
	double ignored_support;
	int positive_bin = FindSupportedBin(state.positive, bin_count, bind, histogram_epsilon, ignored_support);
	int negative_bin =
	    nonnegative ? -1 : FindSupportedBin(state.negative, bin_count, bind, histogram_epsilon, ignored_support);
	double bound = std::max(ExactBinUpperBound(negative_bin), ExactBinUpperBound(positive_bin));
	return {ClipExactComponent(state, bind, negative_bin, positive_bin, input_scale),
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

template <idx_t VALUE_COUNT, class INPUT_TYPE>
struct FilterlessInputVectors {
	UnifiedVectorFormat pu;
	UnifiedVectorFormat active;
	UnifiedVectorFormat values[VALUE_COUNT];
	const uint64_t *pu_values;
	const bool *active_values;
	const INPUT_TYPE *numeric_values[VALUE_COUNT];

	FilterlessInputVectors(Vector inputs[], idx_t count) {
		inputs[0].ToUnifiedFormat(count, pu);
		inputs[1].ToUnifiedFormat(count, active);
		pu_values = UnifiedVectorFormat::GetData<uint64_t>(pu);
		active_values = UnifiedVectorFormat::GetData<bool>(active);
		for (idx_t i = 0; i < VALUE_COUNT; i++) {
			inputs[2 + i].ToUnifiedFormat(count, values[i]);
			numeric_values[i] = UnifiedVectorFormat::GetData<INPUT_TYPE>(values[i]);
		}
	}

	bool RequiredValuesAreValid(idx_t row) const {
		auto pu_index = pu.sel->get_index(row);
		auto active_index = active.sel->get_index(row);
		return pu.validity.RowIsValid(pu_index) && active.validity.RowIsValid(active_index);
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
	bool sampled = FilterlessPuIsSampled(input.pu_values[pu_index], bind.sample_bits);
	UpdateComponent(state.component, input.active_values[active_index], sampled, input.ValueIsValid(0, row),
	                input.ValueOrZero(0, row), input.ValueIsValid(1, row), input.ValueOrZero(1, row), bind, allocator,
	                bind.approximate_values);
}

static void UpdateFilterlessStateRow(FilterlessAvgState &state, const FilterlessInputVectors<4, double> &input,
                                     idx_t row, const FilterlessBindData &bind, ArenaAllocator &allocator) {
	auto pu_index = input.pu.sel->get_index(row);
	auto active_index = input.active.sel->get_index(row);
	bool sampled = FilterlessPuIsSampled(input.pu_values[pu_index], bind.sample_bits);
	UpdateComponent(state.sum_component, input.active_values[active_index], sampled, input.ValueIsValid(0, row),
	                input.ValueOrZero(0, row), input.ValueIsValid(2, row), input.ValueOrZero(2, row), bind, allocator,
	                true);
	UpdateComponent(state.count_component, input.active_values[active_index], sampled, input.ValueIsValid(1, row),
	                input.ValueOrZero(1, row), input.ValueIsValid(3, row), input.ValueOrZero(3, row), bind, allocator,
	                false);
}

template <class INPUT_TYPE>
static void UpdateFilterlessStateRow(FilterlessExactState &state, const FilterlessInputVectors<2, INPUT_TYPE> &input,
                                     idx_t row, const FilterlessBindData &bind, ArenaAllocator &allocator) {
	auto pu_index = input.pu.sel->get_index(row);
	auto active_index = input.active.sel->get_index(row);
	bool sampled = FilterlessPuIsSampled(input.pu_values[pu_index], bind.sample_bits);
	UpdateExactComponent(state.component, input.active_values[active_index], sampled, input.ValueIsValid(0, row),
	                     input.ValueOrZero(0, row), input.ValueIsValid(1, row), input.ValueOrZero(1, row), bind,
	                     allocator, bind.input_scale);
}

template <idx_t VALUE_COUNT, class INPUT_TYPE, class STATE_GETTER>
static void FilterlessUpdateRows(Vector inputs[], AggregateInputData &aggr, idx_t count, STATE_GETTER get_state) {
	auto &bind = aggr.bind_data->Cast<FilterlessBindData>();
	FilterlessInputVectors<VALUE_COUNT, INPUT_TYPE> input(inputs, count);
	for (idx_t row = 0; row < count; row++) {
		if (!input.RequiredValuesAreValid(row)) {
			continue;
		}
		auto state = get_state(row);
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
	}
}

static void FilterlessAvgCombine(Vector &source, Vector &target, AggregateInputData &input, idx_t count) {
	auto sources = FlatVector::GetData<FilterlessAvgState *>(source);
	auto targets = FlatVector::GetData<FilterlessAvgState *>(target);
	for (idx_t i = 0; i < count; i++) {
		CombineComponent(sources[i]->sum_component, targets[i]->sum_component, input.allocator);
		CombineComponent(sources[i]->count_component, targets[i]->count_component, input.allocator);
	}
}

static void FilterlessExactCombine(Vector &source, Vector &target, AggregateInputData &input, idx_t count) {
	auto sources = FlatVector::GetData<FilterlessExactState *>(source);
	auto targets = FlatVector::GetData<FilterlessExactState *>(target);
	for (idx_t i = 0; i < count; i++) {
		CombineExactComponent(sources[i]->component, targets[i]->component, input.allocator);
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
		auto value = FinalizeComponent(state_ptrs[i]->component, bind, bind.epsilon, COUNT);
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
		double component_epsilon = bind.epsilon / 2.0;
		auto sum = FinalizeComponent(state_ptrs[i]->sum_component, bind, component_epsilon, false);
		auto denominator = FinalizeComponent(state_ptrs[i]->count_component, bind, component_epsilon, true);
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

static hugeint_t ReleaseExactComponent(const FilterlessExactComponentState &state, const FilterlessBindData &bind,
                                       double epsilon, bool nonnegative, idx_t bin_count) {
	auto value = FinalizeExactComponent(state, bind, epsilon, nonnegative, bin_count, bind.input_scale);
	auto released = value.clipped_value;
	if (bind.noise_enabled) {
		double noise = AddDpLaplaceNoise(0.0, value.noise_scale);
		auto scaled_noise = SaturatingHugeintFromDouble(noise * bind.input_scale);
		released = SaturatingHugeintAdd(released, scaled_noise);
	}
	return ClampExactResult(released, bind);
}

template <class OUTPUT_TYPE, bool COUNT>
static void FilterlessExactFinalize(Vector &states, AggregateInputData &input, Vector &result, idx_t count,
                                    idx_t offset) {
	auto state_ptrs = FlatVector::GetData<FilterlessExactState *>(states);
	auto result_data = FlatVector::GetData<OUTPUT_TYPE>(result);
	auto &bind = input.bind_data->Cast<FilterlessBindData>();
	for (idx_t i = 0; i < count; i++) {
		auto released =
		    ReleaseExactComponent(state_ptrs[i]->component, bind, bind.epsilon, COUNT, bind.exact_bin_count);
		result_data[offset + i] = CastExactResult<OUTPUT_TYPE>(released);
	}
}

struct FilterlessApproxAvgSumOperation {
	using input_t = double;
	using component_t = FilterlessComponentState;

	static void Update(component_t &state, bool active, bool sampled, bool answer_valid, input_t answer,
	                   bool histogram_valid, input_t histogram, const FilterlessBindData &bind,
	                   ArenaAllocator &allocator) {
		UpdateComponent(state, active, sampled, answer_valid, answer, histogram_valid, histogram, bind, allocator,
		                true);
	}

	static void Combine(const component_t &source, component_t &target, ArenaAllocator &allocator) {
		CombineComponent(source, target, allocator);
	}

	static double Release(const component_t &state, const FilterlessBindData &bind, double epsilon) {
		auto value = FinalizeComponent(state, bind, epsilon, false);
		return bind.noise_enabled ? AddDpLaplaceNoise(value.clipped_value, value.noise_scale) : value.clipped_value;
	}
};

template <class INPUT_TYPE>
struct FilterlessExactAvgSumOperation {
	using input_t = INPUT_TYPE;
	using component_t = FilterlessExactComponentState;

	static void Update(component_t &state, bool active, bool sampled, bool answer_valid, input_t answer,
	                   bool histogram_valid, input_t histogram, const FilterlessBindData &bind,
	                   ArenaAllocator &allocator) {
		UpdateExactComponent(state, active, sampled, answer_valid, answer, histogram_valid, histogram, bind, allocator,
		                     bind.input_scale);
	}

	static void Combine(const component_t &source, component_t &target, ArenaAllocator &allocator) {
		CombineExactComponent(source, target, allocator);
	}

	static double Release(const component_t &state, const FilterlessBindData &bind, double epsilon) {
		auto released = ReleaseExactComponent(state, bind, epsilon, false, bind.exact_bin_count);
		return Hugeint::Cast<double>(released) / bind.input_scale;
	}
};

// The compiler's upper AVG consumes the already paired per-PU SUM and COUNT partials in one pass.
// COUNT stays exact so fusion preserves the previous SUM / BIGINT COUNT release semantics.
template <class SUM_OPERATION>
struct FilterlessFusedAvgState {
	typename SUM_OPERATION::component_t sum_component;
	FilterlessExactComponentState count_component;
};

template <class SUM_OPERATION>
static idx_t FilterlessFusedAvgStateSize(const AggregateFunction &) {
	return sizeof(FilterlessFusedAvgState<SUM_OPERATION>);
}

template <class SUM_OPERATION>
static void FilterlessFusedAvgInitialize(const AggregateFunction &, data_ptr_t state_p) {
	memset(state_p, 0, sizeof(FilterlessFusedAvgState<SUM_OPERATION>));
}

template <class SUM_OPERATION, class STATE_GETTER>
static void FilterlessFusedAvgUpdateRows(Vector inputs[], AggregateInputData &aggr, idx_t count,
                                         STATE_GETTER get_state) {
	UnifiedVectorFormat pu_data, active_data, answer_sum_data, answer_count_data, histogram_sum_data,
	    histogram_count_data;
	inputs[0].ToUnifiedFormat(count, pu_data);
	inputs[1].ToUnifiedFormat(count, active_data);
	inputs[2].ToUnifiedFormat(count, answer_sum_data);
	inputs[3].ToUnifiedFormat(count, answer_count_data);
	inputs[4].ToUnifiedFormat(count, histogram_sum_data);
	inputs[5].ToUnifiedFormat(count, histogram_count_data);
	auto pus = UnifiedVectorFormat::GetData<uint64_t>(pu_data);
	auto active = UnifiedVectorFormat::GetData<bool>(active_data);
	auto answer_sums = UnifiedVectorFormat::GetData<typename SUM_OPERATION::input_t>(answer_sum_data);
	auto answer_counts = UnifiedVectorFormat::GetData<int64_t>(answer_count_data);
	auto histogram_sums = UnifiedVectorFormat::GetData<typename SUM_OPERATION::input_t>(histogram_sum_data);
	auto histogram_counts = UnifiedVectorFormat::GetData<int64_t>(histogram_count_data);
	auto &bind = aggr.bind_data->Cast<FilterlessBindData>();
	for (idx_t row = 0; row < count; row++) {
		auto pu_index = pu_data.sel->get_index(row);
		auto active_index = active_data.sel->get_index(row);
		if (!pu_data.validity.RowIsValid(pu_index) || !active_data.validity.RowIsValid(active_index)) {
			continue;
		}
		auto answer_sum_index = answer_sum_data.sel->get_index(row);
		auto answer_count_index = answer_count_data.sel->get_index(row);
		auto histogram_sum_index = histogram_sum_data.sel->get_index(row);
		auto histogram_count_index = histogram_count_data.sel->get_index(row);
		bool answer_sum_valid = answer_sum_data.validity.RowIsValid(answer_sum_index);
		bool answer_count_valid = answer_count_data.validity.RowIsValid(answer_count_index);
		bool histogram_sum_valid = histogram_sum_data.validity.RowIsValid(histogram_sum_index);
		bool histogram_count_valid = histogram_count_data.validity.RowIsValid(histogram_count_index);
		bool sampled = FilterlessPuIsSampled(pus[pu_index], bind.sample_bits);
		auto state = get_state(row);
		SUM_OPERATION::Update(
		    state->sum_component, active[active_index], sampled, answer_sum_valid,
		    answer_sum_valid ? answer_sums[answer_sum_index] : typename SUM_OPERATION::input_t(0), histogram_sum_valid,
		    histogram_sum_valid ? histogram_sums[histogram_sum_index] : typename SUM_OPERATION::input_t(0), bind,
		    aggr.allocator);
		UpdateExactComponent(state->count_component, active[active_index], sampled, answer_count_valid,
		                     answer_count_valid ? answer_counts[answer_count_index] : 0, histogram_count_valid,
		                     histogram_count_valid ? histogram_counts[histogram_count_index] : 0, bind, aggr.allocator,
		                     1.0);
	}
}

template <class SUM_OPERATION>
static void FilterlessFusedAvgUpdate(Vector inputs[], AggregateInputData &aggr, idx_t, data_ptr_t state_p,
                                     idx_t count) {
	auto state = reinterpret_cast<FilterlessFusedAvgState<SUM_OPERATION> *>(state_p);
	FilterlessFusedAvgUpdateRows<SUM_OPERATION>(inputs, aggr, count, [state](idx_t) { return state; });
}

template <class SUM_OPERATION>
static void FilterlessFusedAvgScatterUpdate(Vector inputs[], AggregateInputData &aggr, idx_t, Vector &states,
                                            idx_t count) {
	UnifiedVectorFormat state_data;
	states.ToUnifiedFormat(count, state_data);
	auto state_ptrs = UnifiedVectorFormat::GetData<FilterlessFusedAvgState<SUM_OPERATION> *>(state_data);
	FilterlessFusedAvgUpdateRows<SUM_OPERATION>(inputs, aggr, count,
	                                            [&](idx_t row) { return state_ptrs[state_data.sel->get_index(row)]; });
}

template <class SUM_OPERATION>
static void FilterlessFusedAvgCombine(Vector &source, Vector &target, AggregateInputData &input, idx_t count) {
	auto sources = FlatVector::GetData<FilterlessFusedAvgState<SUM_OPERATION> *>(source);
	auto targets = FlatVector::GetData<FilterlessFusedAvgState<SUM_OPERATION> *>(target);
	for (idx_t i = 0; i < count; i++) {
		SUM_OPERATION::Combine(sources[i]->sum_component, targets[i]->sum_component, input.allocator);
		CombineExactComponent(sources[i]->count_component, targets[i]->count_component, input.allocator);
	}
}

template <class SUM_OPERATION>
static void FilterlessFusedAvgFinalize(Vector &states, AggregateInputData &input, Vector &result, idx_t count,
                                       idx_t offset) {
	auto state_ptrs = FlatVector::GetData<FilterlessFusedAvgState<SUM_OPERATION> *>(states);
	auto result_data = FlatVector::GetData<double>(result);
	auto &validity = FlatVector::Validity(result);
	auto &bind = input.bind_data->Cast<FilterlessBindData>();
	double component_epsilon = bind.epsilon / 2.0;
	for (idx_t i = 0; i < count; i++) {
		double sum = SUM_OPERATION::Release(state_ptrs[i]->sum_component, bind, component_epsilon);
		auto count_value = FinalizeExactComponent(state_ptrs[i]->count_component, bind, component_epsilon, true,
		                                          FILTERLESS_COUNT_BIN_COUNT, 1.0);
		auto released_count = count_value.clipped_value;
		if (bind.noise_enabled) {
			auto noise = SaturatingHugeintFromDouble(AddDpLaplaceNoise(0.0, count_value.noise_scale));
			released_count = SaturatingHugeintAdd(released_count, noise);
		}
		auto count_result = CastExactResult<int64_t>(released_count);
		if (count_result <= 0) {
			validity.SetInvalid(offset + i);
		} else {
			result_data[offset + i] = sum / static_cast<double>(count_result);
		}
	}
}

struct FilterlessApproxSumState {
	bool isset;
	hugeint_t positive;
	hugeint_t negative;
};

// Use the same scaled-magnitude representation as PAC's approximate SUM so
// cancellation cannot erase small per-PU contributions before clipping.
struct FilterlessApproxSumPairOperation {
	using input_t = double;
	using state_t = FilterlessApproxSumState;
	using result_t = double;

	static hugeint_t ApproximateScaledValue(double value) {
		return Hugeint::Convert(ClipApproximateMagnitude64(AsScaledMagnitude(value)));
	}

	static void Add(state_t &state, input_t value, ArenaAllocator &) {
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

	static void Combine(const state_t &source, state_t &target, ArenaAllocator &) {
		target.isset = target.isset || source.isset;
		target.positive = Hugeint::Add(target.positive, source.positive);
		target.negative = Hugeint::Add(target.negative, source.negative);
	}

	static bool IsSet(const state_t &state) {
		return state.isset;
	}

	static result_t Finalize(const state_t &state) {
		auto scaled = Hugeint::Subtract(state.positive, state.negative);
		return Hugeint::Cast<double>(scaled) / CLIP_DOUBLE_SCALE;
	}
};

struct FilterlessExactSumPartialState {
	hugeint_t value;
	// 0 is unset, 1 is inline, and every other value points to a rare wide overflow state.
	uintptr_t storage;
};

constexpr uintptr_t FILTERLESS_EXACT_SUM_UNSET = 0;
constexpr uintptr_t FILTERLESS_EXACT_SUM_INLINE = 1;

static FilterlessWideValue *GetWideExactSum(const FilterlessExactSumPartialState &state) {
	D_ASSERT(state.storage > FILTERLESS_EXACT_SUM_INLINE);
	return reinterpret_cast<FilterlessWideValue *>(state.storage);
}

static FilterlessWideValue &PromoteExactSum(FilterlessExactSumPartialState &state, ArenaAllocator &allocator) {
	auto wide = reinterpret_cast<FilterlessWideValue *>(allocator.Allocate(sizeof(FilterlessWideValue)));
	memset(wide, 0, sizeof(FilterlessWideValue));
	WideAddSigned(*wide, state.value);
	state.storage = reinterpret_cast<uintptr_t>(wide);
	D_ASSERT(state.storage > FILTERLESS_EXACT_SUM_INLINE);
	return *wide;
}

static void AddExactSum(FilterlessExactSumPartialState &state, hugeint_t value, ArenaAllocator &allocator) {
	if (state.storage == FILTERLESS_EXACT_SUM_UNSET) {
		state.value = value;
		state.storage = FILTERLESS_EXACT_SUM_INLINE;
		return;
	}
	if (state.storage == FILTERLESS_EXACT_SUM_INLINE) {
		auto result = state.value;
		if (Hugeint::TryAddInPlace(result, value)) {
			state.value = result;
			return;
		}
		WideAddSigned(PromoteExactSum(state, allocator), value);
		return;
	}
	WideAddSigned(*GetWideExactSum(state), value);
}

static void CombineExactSum(const FilterlessExactSumPartialState &source, FilterlessExactSumPartialState &target,
                            ArenaAllocator &allocator) {
	if (source.storage == FILTERLESS_EXACT_SUM_UNSET) {
		return;
	}
	if (source.storage == FILTERLESS_EXACT_SUM_INLINE) {
		AddExactSum(target, source.value, allocator);
		return;
	}
	if (target.storage == FILTERLESS_EXACT_SUM_UNSET) {
		target.value = hugeint_t(0);
		target.storage = FILTERLESS_EXACT_SUM_INLINE;
	}
	if (target.storage == FILTERLESS_EXACT_SUM_INLINE) {
		PromoteExactSum(target, allocator);
	}
	WideAddBits(*GetWideExactSum(target), *GetWideExactSum(source));
}

template <class PARTIAL_STATE>
struct FilterlessLowerPairState {
	PARTIAL_STATE answer;
	PARTIAL_STATE histogram;
};

struct FilterlessCountPairOperation {
	using input_t = bool;
	using state_t = uint64_t;
	using result_t = int64_t;

	static void Add(state_t &state, input_t value, ArenaAllocator &) {
		state += static_cast<uint64_t>(value);
	}

	static void Combine(const state_t &source, state_t &target, ArenaAllocator &) {
		target += source;
	}

	static bool IsSet(const state_t &) {
		return true;
	}

	static result_t Finalize(const state_t &state) {
		return static_cast<result_t>(state);
	}
};

template <class INPUT, bool DECIMAL>
struct FilterlessExactSumPairOperation {
	using input_t = INPUT;
	using state_t = FilterlessExactSumPartialState;
	using result_t = hugeint_t;

	static void Add(state_t &state, input_t value, ArenaAllocator &allocator) {
		AddExactSum(state, ToHugeint(value), allocator);
	}

	static void Combine(const state_t &source, state_t &target, ArenaAllocator &allocator) {
		CombineExactSum(source, target, allocator);
	}

	static bool IsSet(const state_t &state) {
		return state.storage != FILTERLESS_EXACT_SUM_UNSET;
	}

	static result_t Finalize(const state_t &state) {
		if (state.storage == FILTERLESS_EXACT_SUM_INLINE) {
			return state.value;
		}
		if (DECIMAL) {
			auto maximum = Hugeint::Subtract(Hugeint::POWERS_OF_TEN[Decimal::MAX_WIDTH_DECIMAL], hugeint_t(1));
			return WideFinalize(*GetWideExactSum(state), Hugeint::Negate(maximum), maximum);
		}
		return WideFinalize(*GetWideExactSum(state), NumericLimits<hugeint_t>::Minimum(),
		                    NumericLimits<hugeint_t>::Maximum());
	}
};

static LogicalType FilterlessLowerPairType(const LogicalType &value_type) {
	return LogicalType::STRUCT({{"answer", value_type}, {"histogram", value_type}});
}

template <class OPERATION>
static idx_t FilterlessLowerPairStateSize(const AggregateFunction &) {
	return sizeof(FilterlessLowerPairState<typename OPERATION::state_t>);
}

template <class OPERATION>
static void FilterlessLowerPairInitialize(const AggregateFunction &, data_ptr_t state_p) {
	memset(state_p, 0, sizeof(FilterlessLowerPairState<typename OPERATION::state_t>));
}

template <class OPERATION, class STATE_GETTER>
static void FilterlessLowerPairUpdateRows(Vector inputs[], idx_t count, ArenaAllocator &allocator,
                                          STATE_GETTER get_state) {
	UnifiedVectorFormat value_data, active_data, sampled_data;
	inputs[0].ToUnifiedFormat(count, value_data);
	inputs[1].ToUnifiedFormat(count, active_data);
	inputs[2].ToUnifiedFormat(count, sampled_data);
	auto values = UnifiedVectorFormat::GetData<typename OPERATION::input_t>(value_data);
	auto active = UnifiedVectorFormat::GetData<bool>(active_data);
	auto sampled = UnifiedVectorFormat::GetData<bool>(sampled_data);
	for (idx_t row = 0; row < count; row++) {
		auto value_index = value_data.sel->get_index(row);
		if (!value_data.validity.RowIsValid(value_index)) {
			continue;
		}
		auto active_index = active_data.sel->get_index(row);
		auto sampled_index = sampled_data.sel->get_index(row);
		bool is_active = active_data.validity.RowIsValid(active_index) && active[active_index];
		bool is_sampled = sampled_data.validity.RowIsValid(sampled_index) && sampled[sampled_index];
		if (is_active || is_sampled) {
			auto state = get_state(row);
			if (is_active) {
				OPERATION::Add(state->answer, values[value_index], allocator);
			}
			if (is_sampled) {
				OPERATION::Add(state->histogram, values[value_index], allocator);
			}
		}
	}
}

template <class OPERATION>
static void FilterlessLowerPairUpdate(Vector inputs[], AggregateInputData &input, idx_t, data_ptr_t state_p,
                                      idx_t count) {
	auto state = reinterpret_cast<FilterlessLowerPairState<typename OPERATION::state_t> *>(state_p);
	FilterlessLowerPairUpdateRows<OPERATION>(inputs, count, input.allocator, [state](idx_t) { return state; });
}

template <class OPERATION>
static void FilterlessLowerPairScatterUpdate(Vector inputs[], AggregateInputData &input, idx_t, Vector &states,
                                             idx_t count) {
	UnifiedVectorFormat state_data;
	states.ToUnifiedFormat(count, state_data);
	auto state_ptrs = UnifiedVectorFormat::GetData<FilterlessLowerPairState<typename OPERATION::state_t> *>(state_data);
	FilterlessLowerPairUpdateRows<OPERATION>(inputs, count, input.allocator,
	                                         [&](idx_t row) { return state_ptrs[state_data.sel->get_index(row)]; });
}

template <class OPERATION>
static void FilterlessLowerPairCombine(Vector &source, Vector &target, AggregateInputData &input, idx_t count) {
	using pair_state_t = FilterlessLowerPairState<typename OPERATION::state_t>;
	auto sources = FlatVector::GetData<pair_state_t *>(source);
	auto targets = FlatVector::GetData<pair_state_t *>(target);
	for (idx_t i = 0; i < count; i++) {
		OPERATION::Combine(sources[i]->answer, targets[i]->answer, input.allocator);
		OPERATION::Combine(sources[i]->histogram, targets[i]->histogram, input.allocator);
	}
}

template <class OPERATION>
static void FilterlessLowerPairFinalize(Vector &states, AggregateInputData &, Vector &result, idx_t count,
                                        idx_t offset) {
	using pair_state_t = FilterlessLowerPairState<typename OPERATION::state_t>;
	auto state_ptrs = FlatVector::GetData<pair_state_t *>(states);
	auto &children = StructVector::GetEntries(result);
	auto answers = FlatVector::GetData<typename OPERATION::result_t>(*children[0]);
	auto histograms = FlatVector::GetData<typename OPERATION::result_t>(*children[1]);
	for (idx_t i = 0; i < count; i++) {
		auto row = offset + i;
		if (OPERATION::IsSet(state_ptrs[i]->answer)) {
			answers[row] = OPERATION::Finalize(state_ptrs[i]->answer);
		} else {
			FlatVector::Validity(*children[0]).SetInvalid(row);
		}
		if (OPERATION::IsSet(state_ptrs[i]->histogram)) {
			histograms[row] = OPERATION::Finalize(state_ptrs[i]->histogram);
		} else {
			FlatVector::Validity(*children[1]).SetInvalid(row);
		}
	}
}

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

static void ConfigureFilterlessDecimalBind(FilterlessBindData &bind, const LogicalType &input_type) {
	bind.input_scale = std::pow(10.0, DecimalType::GetScale(input_type));
	bind.exact_output_max = Hugeint::Subtract(Hugeint::POWERS_OF_TEN[Decimal::MAX_WIDTH_DECIMAL], hugeint_t(1));
	bind.exact_output_min = Hugeint::Negate(bind.exact_output_max);
}

static unique_ptr<FunctionData> BindFilterlessDecimalSum(ClientContext &context, AggregateFunction &function,
                                                         vector<unique_ptr<Expression>> &arguments) {
	if (arguments.size() != 4 && arguments.size() != 6) {
		throw InternalException("filterless_sum: unexpected DECIMAL argument count");
	}
	auto input_type = arguments[2]->return_type;
	if (arguments[3]->return_type != input_type) {
		throw InvalidInputException("filterless_sum: answer and histogram DECIMAL types must match");
	}
	auto return_type = LogicalType::DECIMAL(Decimal::MAX_WIDTH_DECIMAL, DecimalType::GetScale(input_type));
	function = MakeFilterlessDecimalSumFunction(input_type, return_type, arguments.size() == 6);
	auto result = BindFilterless(context, arguments, 4, false, FILTERLESS_HUGEINT_BIN_COUNT);
	ConfigureFilterlessDecimalBind(result->Cast<FilterlessBindData>(), input_type);
	return result;
}

static void AddSumCountOverloads(AggregateFunctionSet &set, const string &name, aggregate_finalize_t finalize,
                                 const LogicalType &return_type, bind_aggregate_function_t bind) {
	set.AddFunction(
	    AggregateFunction(name, {LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                      return_type, FilterlessStateSize, FilterlessInitialize, FilterlessScatterUpdate,
	                      FilterlessCombine, finalize, FunctionNullHandling::SPECIAL_HANDLING, FilterlessUpdate, bind));
	set.AddFunction(AggregateFunction(name,
	                                  {LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalType::DOUBLE,
	                                   LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                                  return_type, FilterlessStateSize, FilterlessInitialize, FilterlessScatterUpdate,
	                                  FilterlessCombine, finalize, FunctionNullHandling::SPECIAL_HANDLING,
	                                  FilterlessUpdate, bind));
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
	     LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	    return_type, FilterlessAvgStateSize, FilterlessAvgInitialize, FilterlessAvgScatterUpdate, FilterlessAvgCombine,
	    finalize, FunctionNullHandling::SPECIAL_HANDLING, FilterlessAvgUpdate, BindFilterlessAvg));
}

template <class SUM_OPERATION>
static AggregateFunction MakeFilterlessFusedAvgFunction(const LogicalType &sum_type, bind_aggregate_function_t bind) {
	auto function =
	    AggregateFunction("priv_filterless_avg",
	                      {LogicalType::UBIGINT, LogicalType::BOOLEAN, sum_type, LogicalType::BIGINT, sum_type,
	                       LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                      LogicalType::DOUBLE, FilterlessFusedAvgStateSize<SUM_OPERATION>,
	                      FilterlessFusedAvgInitialize<SUM_OPERATION>, FilterlessFusedAvgScatterUpdate<SUM_OPERATION>,
	                      FilterlessFusedAvgCombine<SUM_OPERATION>, FilterlessFusedAvgFinalize<SUM_OPERATION>,
	                      FunctionNullHandling::SPECIAL_HANDLING, FilterlessFusedAvgUpdate<SUM_OPERATION>, bind);
	function.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
	return function;
}

static unique_ptr<FunctionData> BindFilterlessFusedApproxAvg(ClientContext &context, AggregateFunction &,
                                                             vector<unique_ptr<Expression>> &arguments) {
	return BindFilterless(context, arguments, 6, true, FILTERLESS_HUGEINT_BIN_COUNT);
}

static unique_ptr<FunctionData> BindFilterlessFusedExactAvg(ClientContext &context, AggregateFunction &,
                                                            vector<unique_ptr<Expression>> &arguments) {
	return BindFilterless(context, arguments, 6, false, FILTERLESS_HUGEINT_BIN_COUNT);
}

static unique_ptr<FunctionData> BindFilterlessFusedDecimalAvg(ClientContext &context, AggregateFunction &function,
                                                              vector<unique_ptr<Expression>> &arguments) {
	auto sum_type = arguments[2]->return_type;
	if (arguments[4]->return_type != sum_type) {
		throw InvalidInputException("priv_filterless_avg: answer and histogram DECIMAL types must match");
	}
	switch (sum_type.InternalType()) {
	case PhysicalType::INT16:
		function = MakeFilterlessFusedAvgFunction<FilterlessExactAvgSumOperation<int16_t>>(
		    sum_type, BindFilterlessFusedDecimalAvg);
		break;
	case PhysicalType::INT32:
		function = MakeFilterlessFusedAvgFunction<FilterlessExactAvgSumOperation<int32_t>>(
		    sum_type, BindFilterlessFusedDecimalAvg);
		break;
	case PhysicalType::INT64:
		function = MakeFilterlessFusedAvgFunction<FilterlessExactAvgSumOperation<int64_t>>(
		    sum_type, BindFilterlessFusedDecimalAvg);
		break;
	case PhysicalType::INT128:
		function = MakeFilterlessFusedAvgFunction<FilterlessExactAvgSumOperation<hugeint_t>>(
		    sum_type, BindFilterlessFusedDecimalAvg);
		break;
	default:
		throw InternalException("priv_filterless_avg: unsupported DECIMAL physical type");
	}
	auto result = BindFilterless(context, arguments, 6, false, FILTERLESS_HUGEINT_BIN_COUNT);
	ConfigureFilterlessDecimalBind(result->Cast<FilterlessBindData>(), sum_type);
	return result;
}

template <class OPERATION>
static AggregateFunction MakeFilterlessLowerPairFunction(const string &name, const LogicalType &input_type,
                                                         const LogicalType &return_type) {
	auto function =
	    AggregateFunction(name, {input_type, LogicalType::BOOLEAN, LogicalType::BOOLEAN},
	                      FilterlessLowerPairType(return_type), FilterlessLowerPairStateSize<OPERATION>,
	                      FilterlessLowerPairInitialize<OPERATION>, FilterlessLowerPairScatterUpdate<OPERATION>,
	                      FilterlessLowerPairCombine<OPERATION>, FilterlessLowerPairFinalize<OPERATION>,
	                      FunctionNullHandling::SPECIAL_HANDLING, FilterlessLowerPairUpdate<OPERATION>);
	function.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
	return function;
}

template <class INPUT_TYPE, bool DECIMAL = false>
static AggregateFunction MakeFilterlessExactSumPairFunction(const LogicalType &input_type,
                                                            const LogicalType &return_type) {
	return MakeFilterlessLowerPairFunction<FilterlessExactSumPairOperation<INPUT_TYPE, DECIMAL>>(
	    "priv_filterless_sum_pair", input_type, return_type);
}

static unique_ptr<FunctionData> BindFilterlessDecimalSumPair(ClientContext &, AggregateFunction &function,
                                                             vector<unique_ptr<Expression>> &arguments) {
	auto input_type = arguments[0]->return_type;
	auto return_type = LogicalType::DECIMAL(Decimal::MAX_WIDTH_DECIMAL, DecimalType::GetScale(input_type));
	switch (input_type.InternalType()) {
	case PhysicalType::INT16:
		function = MakeFilterlessExactSumPairFunction<int16_t, true>(input_type, return_type);
		break;
	case PhysicalType::INT32:
		function = MakeFilterlessExactSumPairFunction<int32_t, true>(input_type, return_type);
		break;
	case PhysicalType::INT64:
		function = MakeFilterlessExactSumPairFunction<int64_t, true>(input_type, return_type);
		break;
	case PhysicalType::INT128:
		function = MakeFilterlessExactSumPairFunction<hugeint_t, true>(input_type, return_type);
		break;
	default:
		throw InternalException("priv_filterless_sum_pair: unsupported DECIMAL physical type");
	}
	return nullptr;
}

void RegisterFilterlessAggregateFunctions(ExtensionLoader &loader) {
	AggregateFunctionSet fused_avg_set("priv_filterless_avg");
	fused_avg_set.AddFunction(MakeFilterlessFusedAvgFunction<FilterlessApproxAvgSumOperation>(
	    LogicalType::DOUBLE, BindFilterlessFusedApproxAvg));
	fused_avg_set.AddFunction(MakeFilterlessFusedAvgFunction<FilterlessExactAvgSumOperation<hugeint_t>>(
	    LogicalType::HUGEINT, BindFilterlessFusedExactAvg));
	fused_avg_set.AddFunction(
	    AggregateFunction({LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalTypeId::DECIMAL, LogicalType::BIGINT,
	                       LogicalTypeId::DECIMAL, LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                      LogicalType::DOUBLE, nullptr, nullptr, nullptr, nullptr, nullptr,
	                      FunctionNullHandling::SPECIAL_HANDLING, nullptr, BindFilterlessFusedDecimalAvg));
	CreateAggregateFunctionInfo fused_avg_info(fused_avg_set);
	FunctionDescription fused_avg_description;
	fused_avg_description.description = "[INTERNAL] Fused filterless SUM and exact COUNT components for AVG.";
	fused_avg_info.descriptions.push_back(std::move(fused_avg_description));
	loader.RegisterFunction(std::move(fused_avg_info));

	AggregateFunction count_pair = MakeFilterlessLowerPairFunction<FilterlessCountPairOperation>(
	    "priv_filterless_count_pair", LogicalType::BOOLEAN, LogicalType::BIGINT);
	CreateAggregateFunctionInfo count_pair_info(count_pair);
	FunctionDescription count_pair_description;
	count_pair_description.description = "[INTERNAL] Fused filtered-answer and sampled-histogram COUNT partials.";
	count_pair_info.descriptions.push_back(std::move(count_pair_description));
	loader.RegisterFunction(std::move(count_pair_info));

	AggregateFunctionSet sum_pair_set("priv_filterless_sum_pair");
	sum_pair_set.AddFunction(MakeFilterlessLowerPairFunction<FilterlessApproxSumPairOperation>(
	    "priv_filterless_sum_pair", LogicalType::DOUBLE, LogicalType::DOUBLE));
	sum_pair_set.AddFunction(MakeFilterlessExactSumPairFunction<bool>(LogicalType::BOOLEAN, LogicalType::HUGEINT));
	sum_pair_set.AddFunction(MakeFilterlessExactSumPairFunction<int8_t>(LogicalType::TINYINT, LogicalType::HUGEINT));
	sum_pair_set.AddFunction(MakeFilterlessExactSumPairFunction<int16_t>(LogicalType::SMALLINT, LogicalType::HUGEINT));
	sum_pair_set.AddFunction(MakeFilterlessExactSumPairFunction<int32_t>(LogicalType::INTEGER, LogicalType::HUGEINT));
	sum_pair_set.AddFunction(MakeFilterlessExactSumPairFunction<int64_t>(LogicalType::BIGINT, LogicalType::HUGEINT));
	sum_pair_set.AddFunction(MakeFilterlessExactSumPairFunction<hugeint_t>(LogicalType::HUGEINT, LogicalType::HUGEINT));
	sum_pair_set.AddFunction(AggregateFunction(
	    {LogicalTypeId::DECIMAL, LogicalType::BOOLEAN, LogicalType::BOOLEAN},
	    FilterlessLowerPairType(LogicalType(LogicalTypeId::DECIMAL)), nullptr, nullptr, nullptr, nullptr, nullptr,
	    FunctionNullHandling::SPECIAL_HANDLING, nullptr, BindFilterlessDecimalSumPair));
	CreateAggregateFunctionInfo sum_pair_info(sum_pair_set);
	FunctionDescription sum_pair_description;
	sum_pair_description.description = "[INTERNAL] Fused filtered-answer and sampled-histogram SUM partials.";
	sum_pair_info.descriptions.push_back(std::move(sum_pair_description));
	loader.RegisterFunction(std::move(sum_pair_info));

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
	sum_set.AddFunction(AggregateFunction({LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalTypeId::DECIMAL,
	                                       LogicalTypeId::DECIMAL, LogicalType::DOUBLE, LogicalType::DOUBLE},
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
