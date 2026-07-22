#include "aggregates/dp_laplace_noise.hpp"
#include "aggregates/as_aggregate.hpp"
#include "privacy_debug.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace duckdb {

static void DpSampleMaskFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	int sample_lanes = GetDpSampleLanes(state.GetContext());
	auto count = args.size();
	UnaryExecutor::Execute<uint64_t, uint64_t>(
	    args.data[0], result, count, [sample_lanes](uint64_t hash) { return DpSampleHash(hash, sample_lanes); });
}

struct DpSassStabilityStats {
	int32_t lane_count = 0;
	int32_t valid_count = 0;
	int32_t null_count = 0;
	double mean = 0.0;
	double stddev_pop = 0.0;
	double cv = 0.0;
	double min = 0.0;
	double median = 0.0;
	double max = 0.0;
	double range = 0.0;
	double mad = 0.0;
	double max_abs_dev = 0.0;
	bool cv_valid = true;
};

static bool ComputeDpSassStabilityStats(const list_entry_t &entry, const UnifiedVectorFormat &child_data,
                                        const PAC_FLOAT *child_values, DpSassStabilityStats &stats) {
	stats.lane_count = static_cast<int32_t>(entry.length);
	stats.null_count = stats.lane_count;
	if (entry.length == 0) {
		return false;
	}

	vector<double> values;
	values.reserve(entry.length);
	for (idx_t j = 0; j < entry.length; j++) {
		auto child_idx = child_data.sel->get_index(entry.offset + j);
		if (!child_data.validity.RowIsValid(child_idx)) {
			continue;
		}
		double value = static_cast<double>(child_values[child_idx]);
		if (!std::isfinite(value)) {
			continue;
		}
		values.push_back(value);
	}
	if (values.empty()) {
		return false;
	}

	stats.valid_count = static_cast<int32_t>(values.size());
	stats.null_count = stats.lane_count - stats.valid_count;

	double sum = 0.0;
	for (double value : values) {
		sum += value;
	}
	stats.mean = sum / static_cast<double>(values.size());

	double var_sum = 0.0;
	stats.max_abs_dev = 0.0;
	for (double value : values) {
		double dev = value - stats.mean;
		var_sum += dev * dev;
		stats.max_abs_dev = std::max(stats.max_abs_dev, std::abs(dev));
	}
	stats.stddev_pop = std::sqrt(var_sum / static_cast<double>(values.size()));
	if (std::abs(stats.mean) > 0.0) {
		stats.cv = stats.stddev_pop / std::abs(stats.mean);
	} else if (stats.stddev_pop == 0.0) {
		stats.cv = 0.0;
	} else {
		stats.cv_valid = false;
	}

	std::sort(values.begin(), values.end());
	stats.min = values.front();
	stats.max = values.back();
	stats.range = stats.max - stats.min;
	idx_t mid = values.size() / 2;
	if (values.size() % 2 == 0) {
		stats.median = (values[mid - 1] + values[mid]) / 2.0;
	} else {
		stats.median = values[mid];
	}

	vector<double> abs_devs;
	abs_devs.reserve(values.size());
	for (double value : values) {
		abs_devs.push_back(std::abs(value - stats.median));
	}
	std::sort(abs_devs.begin(), abs_devs.end());
	if (abs_devs.size() % 2 == 0) {
		stats.mad = (abs_devs[mid - 1] + abs_devs[mid]) / 2.0;
	} else {
		stats.mad = abs_devs[mid];
	}
	return true;
}

static void DpSassStabilityFunction(DataChunk &args, ExpressionState &, Vector &result) {
	idx_t count = args.size();
	auto &list_vec = args.data[0];
	UnifiedVectorFormat list_data;
	list_vec.ToUnifiedFormat(count, list_data);
	auto list_entries = UnifiedVectorFormat::GetData<list_entry_t>(list_data);

	auto &child_vec = ListVector::GetEntry(list_vec);
	UnifiedVectorFormat child_data;
	child_vec.ToUnifiedFormat(ListVector::GetListSize(list_vec), child_data);
	auto child_values = UnifiedVectorFormat::GetData<PAC_FLOAT>(child_data);

	auto &entries = StructVector::GetEntries(result);
	auto lane_count_data = FlatVector::GetData<int32_t>(*entries[0]);
	auto valid_count_data = FlatVector::GetData<int32_t>(*entries[1]);
	auto null_count_data = FlatVector::GetData<int32_t>(*entries[2]);
	auto mean_data = FlatVector::GetData<double>(*entries[3]);
	auto stddev_data = FlatVector::GetData<double>(*entries[4]);
	auto cv_data = FlatVector::GetData<double>(*entries[5]);
	auto min_data = FlatVector::GetData<double>(*entries[6]);
	auto median_data = FlatVector::GetData<double>(*entries[7]);
	auto max_data = FlatVector::GetData<double>(*entries[8]);
	auto range_data = FlatVector::GetData<double>(*entries[9]);
	auto mad_data = FlatVector::GetData<double>(*entries[10]);
	auto max_abs_dev_data = FlatVector::GetData<double>(*entries[11]);
	auto stable_stddev_1_data = FlatVector::GetData<bool>(*entries[12]);
	auto stable_mad_1_data = FlatVector::GetData<bool>(*entries[13]);

	auto &result_validity = FlatVector::Validity(result);
	auto &cv_validity = FlatVector::Validity(*entries[5]);
	for (idx_t i = 0; i < count; i++) {
		auto list_idx = list_data.sel->get_index(i);
		if (!list_data.validity.RowIsValid(list_idx)) {
			result_validity.SetInvalid(i);
			continue;
		}
		DpSassStabilityStats stats;
		if (!ComputeDpSassStabilityStats(list_entries[list_idx], child_data, child_values, stats)) {
			result_validity.SetInvalid(i);
			continue;
		}

		lane_count_data[i] = stats.lane_count;
		valid_count_data[i] = stats.valid_count;
		null_count_data[i] = stats.null_count;
		mean_data[i] = stats.mean;
		stddev_data[i] = stats.stddev_pop;
		cv_data[i] = stats.cv;
		if (!stats.cv_valid) {
			cv_validity.SetInvalid(i);
		}
		min_data[i] = stats.min;
		median_data[i] = stats.median;
		max_data[i] = stats.max;
		range_data[i] = stats.range;
		mad_data[i] = stats.mad;
		max_abs_dev_data[i] = stats.max_abs_dev;
		stable_stddev_1_data[i] = stats.stddev_pop < 1.0;
		stable_mad_1_data[i] = stats.mad < 1.0;
	}
}

static vector<DpSassStabilityQueryRecord> &DpSassStabilityQueryRecords() {
	static thread_local vector<DpSassStabilityQueryRecord> records;
	return records;
}

static vector<DpSassNoiseScaleQueryRecord> &DpSassNoiseScaleQueryRecords() {
	static thread_local vector<DpSassNoiseScaleQueryRecord> records;
	return records;
}

DpSassNoiseScaleQueryRecord DpSassNoiseScaleQueryRecord::Additive(int32_t aggregate_index, double noise_scale) {
	return {aggregate_index, Value::DOUBLE(noise_scale), Value(LogicalType::DOUBLE), Value(LogicalType::DOUBLE),
	        Value(LogicalType::INTEGER)};
}

DpSassNoiseScaleQueryRecord DpSassNoiseScaleQueryRecord::Ratio(int32_t aggregate_index, double numerator_noise_scale,
                                                               double denominator_noise_scale,
                                                               int32_t valid_lane_count) {
	return {aggregate_index, Value(LogicalType::DOUBLE), Value::DOUBLE(numerator_noise_scale),
	        Value::DOUBLE(denominator_noise_scale), Value::INTEGER(valid_lane_count)};
}

void ClearDpSassStabilityQueryRecords() {
	DpSassStabilityQueryRecords().clear();
}

vector<DpSassStabilityQueryRecord> TakeDpSassStabilityQueryRecords() {
	auto records = std::move(DpSassStabilityQueryRecords());
	DpSassStabilityQueryRecords().clear();
	return records;
}

void ClearDpSassNoiseScaleQueryRecords() {
	DpSassNoiseScaleQueryRecords().clear();
}

vector<DpSassNoiseScaleQueryRecord> TakeDpSassNoiseScaleQueryRecords() {
	auto records = std::move(DpSassNoiseScaleQueryRecords());
	DpSassNoiseScaleQueryRecords().clear();
	return records;
}

static vector<Value> DpSassStatsToValues(const DpSassStabilityStats &stats) {
	vector<Value> values;
	values.reserve(14);
	values.push_back(Value::INTEGER(stats.lane_count));
	values.push_back(Value::INTEGER(stats.valid_count));
	values.push_back(Value::INTEGER(stats.null_count));
	values.push_back(Value::DOUBLE(stats.mean));
	values.push_back(Value::DOUBLE(stats.stddev_pop));
	values.push_back(stats.cv_valid ? Value::DOUBLE(stats.cv) : Value(LogicalType::DOUBLE));
	values.push_back(Value::DOUBLE(stats.min));
	values.push_back(Value::DOUBLE(stats.median));
	values.push_back(Value::DOUBLE(stats.max));
	values.push_back(Value::DOUBLE(stats.range));
	values.push_back(Value::DOUBLE(stats.mad));
	values.push_back(Value::DOUBLE(stats.max_abs_dev));
	values.push_back(Value::BOOLEAN(stats.stddev_pop < 1.0));
	values.push_back(Value::BOOLEAN(stats.mad < 1.0));
	return values;
}

static vector<Value> NullDpSassStatsValues() {
	vector<Value> values;
	values.reserve(14);
	values.push_back(Value(LogicalType::INTEGER));
	values.push_back(Value(LogicalType::INTEGER));
	values.push_back(Value(LogicalType::INTEGER));
	values.push_back(Value(LogicalType::DOUBLE));
	values.push_back(Value(LogicalType::DOUBLE));
	values.push_back(Value(LogicalType::DOUBLE));
	values.push_back(Value(LogicalType::DOUBLE));
	values.push_back(Value(LogicalType::DOUBLE));
	values.push_back(Value(LogicalType::DOUBLE));
	values.push_back(Value(LogicalType::DOUBLE));
	values.push_back(Value(LogicalType::DOUBLE));
	values.push_back(Value(LogicalType::DOUBLE));
	values.push_back(Value(LogicalType::BOOLEAN));
	values.push_back(Value(LogicalType::BOOLEAN));
	return values;
}

static void DpSassRecordStabilityFunction(DataChunk &args, ExpressionState &, Vector &result) {
	idx_t count = args.size();
	auto &list_vec = args.data[0];
	UnifiedVectorFormat list_data, aggregate_index_data;
	list_vec.ToUnifiedFormat(count, list_data);
	args.data[1].ToUnifiedFormat(count, aggregate_index_data);
	auto list_entries = UnifiedVectorFormat::GetData<list_entry_t>(list_data);
	auto aggregate_indexes = UnifiedVectorFormat::GetData<int32_t>(aggregate_index_data);

	auto &child_vec = ListVector::GetEntry(list_vec);
	UnifiedVectorFormat child_data;
	child_vec.ToUnifiedFormat(ListVector::GetListSize(list_vec), child_data);
	auto child_values = UnifiedVectorFormat::GetData<PAC_FLOAT>(child_data);

	auto result_data = FlatVector::GetData<double>(result);
	auto &result_validity = FlatVector::Validity(result);
	for (idx_t i = 0; i < count; i++) {
		auto list_idx = list_data.sel->get_index(i);
		auto aggregate_index_idx = aggregate_index_data.sel->get_index(i);
		if (!list_data.validity.RowIsValid(list_idx) ||
		    !aggregate_index_data.validity.RowIsValid(aggregate_index_idx)) {
			result_validity.SetInvalid(i);
			continue;
		}

		DpSassStabilityStats stats;
		bool valid = ComputeDpSassStabilityStats(list_entries[list_idx], child_data, child_values, stats);
		DpSassStabilityQueryRecords().push_back(
		    {aggregate_indexes[aggregate_index_idx], valid ? DpSassStatsToValues(stats) : NullDpSassStatsValues()});
		if (valid) {
			result_data[i] = stats.median;
		} else {
			result_validity.SetInvalid(i);
		}
	}
}

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
	if (!std::isfinite(scale)) {
		throw InvalidInputException("dp_noise: non-finite Laplace scale (sensitivity/ε overflow) — refusing to "
		                            "release a value without noise");
	}
	if (scale <= 0.0) {
		return value; // scale 0 = noise disabled / zero sensitivity → no noise
	}
	std::mt19937_64 gen(seed ^ (PAC_MAGIC_HASH * nonce));
	std::uniform_real_distribution<double> uni(-0.5, 0.5);
	double u = uni(gen);
	double sign = (u < 0.0) ? -1.0 : 1.0;
	double noise = -scale * sign * std::log(std::max(1e-300, 1.0 - 2.0 * std::abs(u)));
	return value + noise;
}

enum class DpSassNoiseChannel : uint64_t { ADDITIVE = 0, RATIO_NUMERATOR = 1, RATIO_DENOMINATOR = 2 };

static uint64_t DpSassCellNoiseNonce(uint64_t group_identity, uint64_t aggregate_index, uint64_t aggregate_count,
                                     DpSassNoiseChannel channel) {
	if (group_identity == 0 || aggregate_count == 0 || aggregate_index >= aggregate_count) {
		throw InternalException("dp_sass: invalid aggregate-cell identity");
	}
	uint64_t group_offset = group_identity - 1;
	if (group_offset > (std::numeric_limits<uint64_t>::max() - aggregate_index) / aggregate_count) {
		throw InvalidInputException("dp_sass: aggregate-cell identity overflow");
	}
	uint64_t cell_index = group_offset * aggregate_count + aggregate_index;
	constexpr uint64_t CHANNEL_COUNT = 3;
	uint64_t channel_index = static_cast<uint64_t>(channel);
	if (cell_index > (std::numeric_limits<uint64_t>::max() - channel_index) / CHANNEL_COUNT) {
		throw InvalidInputException("dp_sass: aggregate-cell nonce overflow");
	}
	return cell_index * CHANNEL_COUNT + channel_index;
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
	if (!std::isfinite(scale)) {
		throw InvalidInputException(
		    "dp_sass: non-finite smooth-sensitivity Laplace scale — refusing to release without noise");
	}
	if (scale <= 0.0) {
		return 0.0; // zero sensitivity (stable statistic) or noise disabled → no noise
	}
	std::mt19937_64 gen(seed);
	std::uniform_real_distribution<double> uni(-0.5, 0.5);
	double u = uni(gen);
	double sign = (u < 0.0) ? -1.0 : 1.0;
	return -scale * sign * std::log(std::max(1e-300, 1.0 - 2.0 * std::abs(u)));
}

static double MedianLocalSensitivity(const vector<double> &values, int changed_lanes) {
	const int median_idx = static_cast<int>((values.size() + 1) / 2 - 1);
	int lo_idx = median_idx - changed_lanes;
	int hi_idx = median_idx + changed_lanes;
	D_ASSERT(lo_idx >= 0);
	D_ASSERT(hi_idx < static_cast<int>(values.size()));
	return std::max(0.0, values[hi_idx] - values[lo_idx]);
}

static double SmoothMedianSensitivity(const vector<double> &values, double beta, int sample_lanes) {
	const int step_size = sample_lanes;
	const int median_idx = static_cast<int>((values.size() + 1) / 2 - 1);
	double smooth = 0.0;
	for (int distance = 0; distance < static_cast<int>(values.size()); distance++) {
		int changed_lanes = step_size * (distance + 1);
		if (changed_lanes > median_idx || median_idx + changed_lanes >= static_cast<int>(values.size())) {
			break;
		}
		double local = MedianLocalSensitivity(values, changed_lanes);
		smooth = std::max(smooth, local * std::exp(-beta * static_cast<double>(distance)));
	}
	return smooth;
}

namespace {

constexpr idx_t SMOOTH_MEDIAN_64_N = 64;
constexpr idx_t SMOOTH_MEDIAN_64_P = 32;

template <class VALUES>
static idx_t SmoothMedianArgMax(const VALUES &x, double beta_eff, idx_t b, idx_t lower, idx_t upper) {
	idx_t best_j = lower;
	double best = -1.0;
	for (idx_t j = lower; j <= upper; j++) {
		double score = (x[j] - x[b]) * std::exp(-beta_eff * static_cast<double>(j - b - 1));
		if (score > best) {
			best = score;
			best_j = j;
		}
	}
	return best_j;
}

template <class VALUES, class INDEXES>
static void SmoothMedianJList(const VALUES &x, double beta_eff, INDEXES &j_star, idx_t first, idx_t last, idx_t lower,
                              idx_t upper) {
	if (last < first) {
		return;
	}
	idx_t midpoint = (first + last) / 2;
	idx_t best_j = SmoothMedianArgMax(x, beta_eff, midpoint, lower, upper);
	j_star[midpoint] = best_j;
	if (midpoint > first) {
		SmoothMedianJList(x, beta_eff, j_star, first, midpoint - 1, lower, best_j);
	}
	SmoothMedianJList(x, beta_eff, j_star, midpoint + 1, last, best_j, upper);
}

static double SmoothMedianSensitivityExact64(const vector<double> &values, double beta, int sample_lanes,
                                             double lower_bound, double upper_bound) {
	D_ASSERT(values.size() == SMOOTH_MEDIAN_64_N);
	double beta_eff = beta / static_cast<double>(sample_lanes);
	std::array<double, SMOOTH_MEDIAN_64_N + 2> x;
	x[0] = lower_bound;
	for (idx_t i = 0; i < SMOOTH_MEDIAN_64_N; i++) {
		x[i + 1] = values[i];
	}
	x[SMOOTH_MEDIAN_64_N + 1] = upper_bound;

	std::array<idx_t, SMOOTH_MEDIAN_64_P + 1> j_star;
	j_star.fill(SMOOTH_MEDIAN_64_P);
	SmoothMedianJList(x, beta_eff, j_star, 0, SMOOTH_MEDIAN_64_P, SMOOTH_MEDIAN_64_P, SMOOTH_MEDIAN_64_N + 1);

	double smooth = 0.0;
	for (idx_t i = 0; i <= SMOOTH_MEDIAN_64_P; i++) {
		idx_t j = j_star[i];
		double score = (x[j] - x[i]) * std::exp(-beta_eff * static_cast<double>(j - i - 1));
		smooth = std::max(smooth, score);
	}
	return std::max(0.0, smooth);
}
static double SmoothMedianSensitivityExactM(const vector<double> &values, double beta, int sample_lanes,
                                            double lower_bound, double upper_bound) {
	D_ASSERT(values.size() > SMOOTH_MEDIAN_64_N);
	// One PU can move up to `sample_lanes` lane answers, so a PU-neighbor at PU-distance d is an
	// element-distance ≤ sample_lanes·d change. Reweighting the element-level NRS envelope by
	// beta_eff = beta / sample_lanes makes the result (a) beta-smooth in PU distance —
	// S(D) ≤ e^{beta_eff·(sample_lanes·d)}·S(D') = e^{beta·d}·S(D') — and (b) a conservative upper
	// bound on the true PU-level smooth sensitivity: the PU terms are the subset k = sample_lanes·d of
	// max_k e^{-beta_eff·k}·LS^(k)_elem, and maxing over all k only grows it. So it over-noises if
	// anything. (Also valid with the public empty-lane padding: padded values stay in [L,Λ], so a
	// PU-neighbor still differs in ≤ sample_lanes bracketed positions.)
	idx_t n = values.size();
	idx_t p = (n + 1) / 2;
	double beta_eff = beta / static_cast<double>(sample_lanes);
	vector<double> x(n + 2);
	x[0] = lower_bound;
	for (idx_t i = 0; i < n; i++) {
		x[i + 1] = values[i];
	}
	x[n + 1] = upper_bound;

	vector<idx_t> j_star(p + 1, p);
	SmoothMedianJList(x, beta_eff, j_star, 0, p, p, n + 1);

	double smooth = 0.0;
	for (idx_t i = 0; i <= p; i++) {
		idx_t j = j_star[i];
		double score = (x[j] - x[i]) * std::exp(-beta_eff * static_cast<double>(j - i - 1));
		smooth = std::max(smooth, score);
	}
	return std::max(0.0, smooth);
}

} // namespace

// Shared per-row core for the sample-and-aggregate median release. Validates the inputs,
// clips (when bounded) and sorts the sample answers, takes the lower median, and computes
// the smooth-sensitivity Laplace scale. Returns false when the row is invalid.
static bool SmoothMedianStatsRow(const list_entry_t &entry, const UnifiedVectorFormat &child_data,
                                 const PAC_FLOAT *child_values, double epsilon, double delta, int sample_lanes_raw,
                                 bool bounded, double lower_bound, double upper_bound, double empty_default,
                                 double &median, double &scale) {
	if (entry.length < 64 || entry.length > DP_SASS_MAX_M || (entry.length & (entry.length - 1)) != 0) {
		return false;
	}
	int sample_lanes = ValidateDpSampleLanes(sample_lanes_raw);
	if (entry.length > 64 && sample_lanes != 1) {
		return false;
	}
	if (epsilon <= 0.0 || delta <= 0.0 || delta >= 0.5 || !std::isfinite(epsilon) || !std::isfinite(delta)) {
		return false;
	}
	if (bounded && (!std::isfinite(lower_bound) || !std::isfinite(upper_bound) || lower_bound >= upper_bound)) {
		return false;
	}
	vector<double> values;
	values.resize(entry.length);
	if (bounded) {
		// GUPT-style: fill EVERY lane. An empty subsample gets a public, in-range default, so the
		// number of populated lanes is data-independent (always m). This removes the data-dependent
		// NULL that would otherwise leak whether the group crossed the 32-lane support boundary;
		// group suppression is handled solely by the noised η'>τ partition-selection gate upstream.
		double fill = std::max(lower_bound, std::min(upper_bound, empty_default));
		for (idx_t j = 0; j < entry.length; j++) {
			auto child_idx = child_data.sel->get_index(entry.offset + j);
			values[j] = child_data.validity.RowIsValid(child_idx)
			                ? std::max(lower_bound, std::min(upper_bound, static_cast<double>(child_values[child_idx])))
			                : fill;
		}
		std::sort(values.begin(), values.end());
	} else {
		// Unbounded (legacy, no public domain to fill from): skip empty lanes; require ≥m/2 present.
		idx_t valid_count = 0;
		for (idx_t j = 0; j < entry.length; j++) {
			auto child_idx = child_data.sel->get_index(entry.offset + j);
			if (child_data.validity.RowIsValid(child_idx)) {
				values[valid_count++] = static_cast<double>(child_values[child_idx]);
			}
		}
		idx_t min_valid = entry.length / 2;
		if (valid_count < min_valid) {
			return false;
		}
		std::sort(values.begin(), values.begin() + static_cast<vector<double>::difference_type>(valid_count));
		for (idx_t j = valid_count; j < values.size(); j++) {
			values[j] = values[valid_count - 1];
		}
	}
	idx_t median_idx = (entry.length + 1) / 2 - 1;
	median = values[median_idx];
	double beta = epsilon / (2.0 * std::log(2.0 / delta));
	double smooth;
	if (!bounded) {
		smooth = SmoothMedianSensitivity(values, beta, sample_lanes);
	} else if (entry.length == SMOOTH_MEDIAN_64_N) {
		// Preserve the allocation-free fixed-width implementation for m=64.
		smooth = SmoothMedianSensitivityExact64(values, beta, sample_lanes, lower_bound, upper_bound);
	} else {
		// Larger m uses the same exact envelope over dynamically sized lane arrays.
		smooth = SmoothMedianSensitivityExactM(values, beta, sample_lanes, lower_bound, upper_bound);
	}
	scale = (2.0 * smooth) / epsilon;
	PRIVACY_DEBUG_PRINT("dp_sass smooth-median: median=" + std::to_string(median) +
	                    " smooth_sensitivity=" + std::to_string(smooth) + " scale=" + std::to_string(scale));
	return true;
}

static bool SmoothMedianNoiseRow(const list_entry_t &entry, const UnifiedVectorFormat &child_data,
                                 const PAC_FLOAT *child_values, double epsilon, double delta, int sample_lanes_raw,
                                 bool bounded, double lower_bound, double upper_bound, double empty_default,
                                 bool noise_enabled, uint64_t seed, uint64_t nonce, double &out) {
	double median = 0.0;
	double scale = 0.0;
	if (!SmoothMedianStatsRow(entry, child_data, child_values, epsilon, delta, sample_lanes_raw, bounded, lower_bound,
	                          upper_bound, empty_default, median, scale)) {
		return false;
	}
	if (!noise_enabled) {
		out = median;
		return true;
	}
	out = AddLaplaceNoise(median, scale, seed, nonce);
	return true;
}

static void DpSmoothMedianNoiseFunctionInternal(DataChunk &args, ExpressionState &state, Vector &result,
                                                bool emit_scale, bool record_scale = false) {
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
	// Bounded variants: (list, ε, δ, lanes, lower, upper[, empty_default]). The 7-arg form (compiler-
	// emitted) carries a per-aggregate-kind empty-lane default; the 6-arg form defaults it to the
	// midpoint. The 4-arg form is the legacy unbounded path.
	bool bounded = record_scale ? args.ColumnCount() >= 7 : args.ColumnCount() >= 6;
	bool has_cell_identity = !emit_scale && !record_scale && args.ColumnCount() == 10;
	bool has_empty_default = record_scale ? args.ColumnCount() == 8 : args.ColumnCount() == 7 || has_cell_identity;
	auto &list_vec = args.data[0];
	UnifiedVectorFormat list_data, epsilon_data, delta_data, sample_lanes_data, lower_data, upper_data, empty_data,
	    aggregate_index_data, aggregate_count_data, group_identity_data;
	list_vec.ToUnifiedFormat(count, list_data);
	args.data[1].ToUnifiedFormat(count, epsilon_data);
	args.data[2].ToUnifiedFormat(count, delta_data);
	args.data[3].ToUnifiedFormat(count, sample_lanes_data);
	if (bounded) {
		args.data[4].ToUnifiedFormat(count, lower_data);
		args.data[5].ToUnifiedFormat(count, upper_data);
	}
	if (has_empty_default) {
		args.data[6].ToUnifiedFormat(count, empty_data);
	}
	if (record_scale) {
		args.data[args.ColumnCount() - 1].ToUnifiedFormat(count, aggregate_index_data);
	} else if (has_cell_identity) {
		args.data[7].ToUnifiedFormat(count, aggregate_index_data);
		args.data[8].ToUnifiedFormat(count, aggregate_count_data);
		args.data[9].ToUnifiedFormat(count, group_identity_data);
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
	auto empty_values = has_empty_default ? UnifiedVectorFormat::GetData<double>(empty_data) : nullptr;
	auto aggregate_indexes =
	    (record_scale || has_cell_identity) ? UnifiedVectorFormat::GetData<int32_t>(aggregate_index_data) : nullptr;
	auto aggregate_counts = has_cell_identity ? UnifiedVectorFormat::GetData<int32_t>(aggregate_count_data) : nullptr;
	auto group_identities = has_cell_identity ? UnifiedVectorFormat::GetData<int64_t>(group_identity_data) : nullptr;
	auto result_data = FlatVector::GetData<double>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto list_idx = list_data.sel->get_index(i);
		auto eps_idx = epsilon_data.sel->get_index(i);
		auto delta_idx = delta_data.sel->get_index(i);
		auto sample_lanes_idx = sample_lanes_data.sel->get_index(i);
		auto lower_idx = bounded ? lower_data.sel->get_index(i) : 0;
		auto upper_idx = bounded ? upper_data.sel->get_index(i) : 0;
		auto aggregate_index_idx = (record_scale || has_cell_identity) ? aggregate_index_data.sel->get_index(i) : 0;
		auto aggregate_count_idx = has_cell_identity ? aggregate_count_data.sel->get_index(i) : 0;
		auto group_identity_idx = has_cell_identity ? group_identity_data.sel->get_index(i) : 0;
		if (!list_data.validity.RowIsValid(list_idx) || !epsilon_data.validity.RowIsValid(eps_idx) ||
		    !delta_data.validity.RowIsValid(delta_idx) || !sample_lanes_data.validity.RowIsValid(sample_lanes_idx) ||
		    (bounded && (!lower_data.validity.RowIsValid(lower_idx) || !upper_data.validity.RowIsValid(upper_idx))) ||
		    ((record_scale || has_cell_identity) && !aggregate_index_data.validity.RowIsValid(aggregate_index_idx)) ||
		    (has_cell_identity && (!aggregate_count_data.validity.RowIsValid(aggregate_count_idx) ||
		                           !group_identity_data.validity.RowIsValid(group_identity_idx)))) {
			result_validity.SetInvalid(i);
			continue;
		}
		auto &entry = list_entries[list_idx];
		double epsilon = epsilons[eps_idx];
		double delta = deltas[delta_idx];
		double lower_bound = bounded ? lower_values[lower_idx] : 0.0;
		double upper_bound = bounded ? upper_values[upper_idx] : 0.0;
		double empty_default = has_empty_default ? empty_values[empty_data.sel->get_index(i)]
		                                         : (lower_bound + upper_bound) / 2.0; // midpoint fallback
		if (emit_scale || record_scale) {
			double median = 0.0;
			double scale = 0.0;
			if (SmoothMedianStatsRow(entry, child_data, child_values, epsilon, delta,
			                         sample_lanes_values[sample_lanes_idx], bounded, lower_bound, upper_bound,
			                         empty_default, median, scale)) {
				if (record_scale) {
					DpSassNoiseScaleQueryRecords().push_back(
					    DpSassNoiseScaleQueryRecord::Additive(aggregate_indexes[aggregate_index_idx], scale));
					result_data[i] = median;
				} else {
					result_data[i] = scale;
				}
			} else {
				result_validity.SetInvalid(i);
			}
		} else {
			double released = 0.0;
			uint64_t nonce = has_cell_identity
			                     ? DpSassCellNoiseNonce(static_cast<uint64_t>(group_identities[group_identity_idx]),
			                                            static_cast<uint64_t>(aggregate_indexes[aggregate_index_idx]),
			                                            static_cast<uint64_t>(aggregate_counts[aggregate_count_idx]),
			                                            DpSassNoiseChannel::ADDITIVE)
			                     : list_entries[list_idx].offset;
			if (SmoothMedianNoiseRow(entry, child_data, child_values, epsilon, delta,
			                         sample_lanes_values[sample_lanes_idx], bounded, lower_bound, upper_bound,
			                         empty_default, noise_enabled, seed, nonce, released)) {
				result_data[i] = released;
			} else {
				result_validity.SetInvalid(i);
			}
		}
	}
}

static void DpSmoothMedianNoiseFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	DpSmoothMedianNoiseFunctionInternal(args, state, result, false);
}

static void DpSmoothMedianNoiseScaleFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	DpSmoothMedianNoiseFunctionInternal(args, state, result, true);
}

static void DpSmoothMedianRecordNoiseScaleFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	DpSmoothMedianNoiseFunctionInternal(args, state, result, false, true);
}

// dp_aggregate: scalar terminal for the "naive" DP sample-and-aggregate path. Mirrors the
// pac_aggregate signature so the naive PAC and DP queries are structurally identical, differing
// only in this terminal. Each row carries 64 per-lane sample answers; the second `counts` list is
// accepted purely for signature symmetry with pac_aggregate and is currently UNUSED by the median
// path (the per-lane answers already encode the aggregate). Always bounded (lower/upper required).
// Signature: dp_aggregate(LIST<FLOAT> samples, LIST<FLOAT> counts, DOUBLE epsilon, DOUBLE delta,
//                         INTEGER lanes, DOUBLE lower, DOUBLE upper) -> DOUBLE
static void DpAggregateFunction(DataChunk &args, ExpressionState &state, Vector &result) {
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
	auto &list_vec = args.data[0]; // samples (counts at args.data[1] is intentionally ignored)
	UnifiedVectorFormat list_data, epsilon_data, delta_data, sample_lanes_data, lower_data, upper_data;
	list_vec.ToUnifiedFormat(count, list_data);
	args.data[2].ToUnifiedFormat(count, epsilon_data);
	args.data[3].ToUnifiedFormat(count, delta_data);
	args.data[4].ToUnifiedFormat(count, sample_lanes_data);
	args.data[5].ToUnifiedFormat(count, lower_data);
	args.data[6].ToUnifiedFormat(count, upper_data);

	auto &child_vec = ListVector::GetEntry(list_vec);
	UnifiedVectorFormat child_data;
	child_vec.ToUnifiedFormat(ListVector::GetListSize(list_vec), child_data);
	auto child_values = UnifiedVectorFormat::GetData<PAC_FLOAT>(child_data);
	auto list_entries = UnifiedVectorFormat::GetData<list_entry_t>(list_data);
	auto epsilons = UnifiedVectorFormat::GetData<double>(epsilon_data);
	auto deltas = UnifiedVectorFormat::GetData<double>(delta_data);
	auto sample_lanes_values = UnifiedVectorFormat::GetData<int32_t>(sample_lanes_data);
	auto lower_values = UnifiedVectorFormat::GetData<double>(lower_data);
	auto upper_values = UnifiedVectorFormat::GetData<double>(upper_data);
	auto result_data = FlatVector::GetData<double>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto list_idx = list_data.sel->get_index(i);
		auto eps_idx = epsilon_data.sel->get_index(i);
		auto delta_idx = delta_data.sel->get_index(i);
		auto sample_lanes_idx = sample_lanes_data.sel->get_index(i);
		auto lower_idx = lower_data.sel->get_index(i);
		auto upper_idx = upper_data.sel->get_index(i);
		if (!list_data.validity.RowIsValid(list_idx) || !epsilon_data.validity.RowIsValid(eps_idx) ||
		    !delta_data.validity.RowIsValid(delta_idx) || !sample_lanes_data.validity.RowIsValid(sample_lanes_idx) ||
		    !lower_data.validity.RowIsValid(lower_idx) || !upper_data.validity.RowIsValid(upper_idx)) {
			result_validity.SetInvalid(i);
			continue;
		}
		double lower_bound = lower_values[lower_idx];
		double upper_bound = upper_values[upper_idx];
		double released = 0.0;
		if (SmoothMedianNoiseRow(list_entries[list_idx], child_data, child_values, epsilons[eps_idx], deltas[delta_idx],
		                         sample_lanes_values[sample_lanes_idx], /*bounded=*/true, lower_bound, upper_bound,
		                         /*empty_default=*/(lower_bound + upper_bound) / 2.0, noise_enabled, seed,
		                         list_entries[list_idx].offset, released)) {
			result_data[i] = released;
		} else {
			result_validity.SetInvalid(i);
		}
	}
}

// GUPT-style release (pure-ε, no δ): the MEAN of all 64 lane (block) answers clipped to the public
// [lower,upper] envelope, plus Laplace noise. Every lane contributes — empty subsamples get a public
// in-range default — so the block count is a fixed, data-independent 64 (not the data-dependent
// number of populated lanes). Each PU's hash sets `sample_lanes` of the 64 lane bits, so removing one
// PU changes at most `sample_lanes` lane answers, each by at most (upper-lower); the mean over 64
// therefore has global sensitivity sample_lanes*(upper-lower)/64, and the Laplace scale is that
// divided by ε. This is the overlapping-subsample analogue of GUPT's disjoint-block mean (they
// coincide at sample_lanes = 1), mirroring how the median path reweights via beta_eff = beta/sample_lanes.
static bool GuptMeanStatsRow(const list_entry_t &entry, const UnifiedVectorFormat &child_data,
                             const PAC_FLOAT *child_values, double epsilon, int sample_lanes_raw, double lower_bound,
                             double upper_bound, double empty_default, double &mean, double &scale) {
	if (entry.length != 64) {
		return false;
	}
	int sample_lanes = ValidateDpSampleLanes(sample_lanes_raw);
	if (epsilon <= 0.0 || !std::isfinite(epsilon)) {
		return false;
	}
	if (!std::isfinite(lower_bound) || !std::isfinite(upper_bound) || lower_bound >= upper_bound) {
		return false;
	}
	// GUPT: every lane contributes — empty subsamples get a public, in-range default — so the block
	// count is a fixed, data-independent 64. Both the mean's denominator and the global-sensitivity
	// noise scale use 64, removing the data-dependent NULL and the data-dependent denominator/scale
	// that would otherwise leak group support.
	double fill = std::max(lower_bound, std::min(upper_bound, empty_default));
	double sum = 0.0;
	for (idx_t j = 0; j < 64; j++) {
		auto child_idx = child_data.sel->get_index(entry.offset + j);
		sum += child_data.validity.RowIsValid(child_idx)
		           ? std::max(lower_bound, std::min(upper_bound, static_cast<double>(child_values[child_idx])))
		           : fill;
	}
	mean = sum / 64.0;
	scale = (static_cast<double>(sample_lanes) * (upper_bound - lower_bound)) / (64.0 * epsilon);
	PRIVACY_DEBUG_PRINT("dp_sass gupt-mean: mean=" + std::to_string(mean) + " range=[" + std::to_string(lower_bound) +
	                    "," + std::to_string(upper_bound) + "] scale=" + std::to_string(scale));
	return true;
}

// Signature: dp_gupt_mean_noise(LIST<FLOAT> samples, DOUBLE epsilon, INTEGER lanes, DOUBLE lower,
//                               DOUBLE upper) -> DOUBLE. Pure ε-DP (no delta).
template <class STATS_ROW>
static void DpGuptMeanNoiseFunctionInternal(DataChunk &args, ExpressionState &state, Vector &result, bool emit_scale,
                                            bool record_scale, const STATS_ROW &stats_row) {
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
	// (list, ε, lanes, lower, upper[, empty_default]). The 6-arg form (compiler-emitted) carries a
	// per-aggregate-kind empty-lane default; the 5-arg form defaults it to the midpoint.
	bool has_cell_identity = !emit_scale && !record_scale && args.ColumnCount() == 9;
	bool has_empty_default = record_scale ? args.ColumnCount() == 7 : args.ColumnCount() == 6 || has_cell_identity;
	auto &list_vec = args.data[0];
	UnifiedVectorFormat list_data, epsilon_data, sample_lanes_data, lower_data, upper_data, empty_data,
	    aggregate_index_data, aggregate_count_data, group_identity_data;
	list_vec.ToUnifiedFormat(count, list_data);
	args.data[1].ToUnifiedFormat(count, epsilon_data);
	args.data[2].ToUnifiedFormat(count, sample_lanes_data);
	args.data[3].ToUnifiedFormat(count, lower_data);
	args.data[4].ToUnifiedFormat(count, upper_data);
	if (has_empty_default) {
		args.data[5].ToUnifiedFormat(count, empty_data);
	}
	if (record_scale) {
		args.data[args.ColumnCount() - 1].ToUnifiedFormat(count, aggregate_index_data);
	} else if (has_cell_identity) {
		args.data[6].ToUnifiedFormat(count, aggregate_index_data);
		args.data[7].ToUnifiedFormat(count, aggregate_count_data);
		args.data[8].ToUnifiedFormat(count, group_identity_data);
	}

	auto &child_vec = ListVector::GetEntry(list_vec);
	UnifiedVectorFormat child_data;
	child_vec.ToUnifiedFormat(ListVector::GetListSize(list_vec), child_data);
	auto child_values = UnifiedVectorFormat::GetData<PAC_FLOAT>(child_data);
	auto list_entries = UnifiedVectorFormat::GetData<list_entry_t>(list_data);
	auto epsilons = UnifiedVectorFormat::GetData<double>(epsilon_data);
	auto sample_lanes_values = UnifiedVectorFormat::GetData<int32_t>(sample_lanes_data);
	auto lower_values = UnifiedVectorFormat::GetData<double>(lower_data);
	auto upper_values = UnifiedVectorFormat::GetData<double>(upper_data);
	auto empty_values = has_empty_default ? UnifiedVectorFormat::GetData<double>(empty_data) : nullptr;
	auto aggregate_indexes =
	    (record_scale || has_cell_identity) ? UnifiedVectorFormat::GetData<int32_t>(aggregate_index_data) : nullptr;
	auto aggregate_counts = has_cell_identity ? UnifiedVectorFormat::GetData<int32_t>(aggregate_count_data) : nullptr;
	auto group_identities = has_cell_identity ? UnifiedVectorFormat::GetData<int64_t>(group_identity_data) : nullptr;
	auto result_data = FlatVector::GetData<double>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto list_idx = list_data.sel->get_index(i);
		auto eps_idx = epsilon_data.sel->get_index(i);
		auto sample_lanes_idx = sample_lanes_data.sel->get_index(i);
		auto lower_idx = lower_data.sel->get_index(i);
		auto upper_idx = upper_data.sel->get_index(i);
		auto aggregate_index_idx = (record_scale || has_cell_identity) ? aggregate_index_data.sel->get_index(i) : 0;
		auto aggregate_count_idx = has_cell_identity ? aggregate_count_data.sel->get_index(i) : 0;
		auto group_identity_idx = has_cell_identity ? group_identity_data.sel->get_index(i) : 0;
		if (!list_data.validity.RowIsValid(list_idx) || !epsilon_data.validity.RowIsValid(eps_idx) ||
		    !sample_lanes_data.validity.RowIsValid(sample_lanes_idx) || !lower_data.validity.RowIsValid(lower_idx) ||
		    !upper_data.validity.RowIsValid(upper_idx) ||
		    ((record_scale || has_cell_identity) && !aggregate_index_data.validity.RowIsValid(aggregate_index_idx)) ||
		    (has_cell_identity && (!aggregate_count_data.validity.RowIsValid(aggregate_count_idx) ||
		                           !group_identity_data.validity.RowIsValid(group_identity_idx)))) {
			result_validity.SetInvalid(i);
			continue;
		}
		double lower_bound = lower_values[lower_idx];
		double upper_bound = upper_values[upper_idx];
		double empty_default = has_empty_default ? empty_values[empty_data.sel->get_index(i)]
		                                         : (lower_bound + upper_bound) / 2.0; // midpoint fallback
		if (emit_scale || record_scale) {
			double mean = 0.0;
			double scale = 0.0;
			if (stats_row(list_entries[list_idx], child_data, child_values, epsilons[eps_idx],
			              sample_lanes_values[sample_lanes_idx], lower_bound, upper_bound, empty_default, mean,
			              scale)) {
				if (record_scale) {
					DpSassNoiseScaleQueryRecords().push_back(
					    DpSassNoiseScaleQueryRecord::Additive(aggregate_indexes[aggregate_index_idx], scale));
					result_data[i] = mean;
				} else {
					result_data[i] = scale;
				}
			} else {
				result_validity.SetInvalid(i);
			}
		} else {
			uint64_t nonce = has_cell_identity
			                     ? DpSassCellNoiseNonce(static_cast<uint64_t>(group_identities[group_identity_idx]),
			                                            static_cast<uint64_t>(aggregate_indexes[aggregate_index_idx]),
			                                            static_cast<uint64_t>(aggregate_counts[aggregate_count_idx]),
			                                            DpSassNoiseChannel::ADDITIVE)
			                     : list_entries[list_idx].offset;
			double mean = 0.0;
			double scale = 0.0;
			if (!stats_row(list_entries[list_idx], child_data, child_values, epsilons[eps_idx],
			               sample_lanes_values[sample_lanes_idx], lower_bound, upper_bound, empty_default, mean,
			               scale)) {
				result_validity.SetInvalid(i);
				continue;
			}
			result_data[i] = noise_enabled ? AddLaplaceNoise(mean, scale, seed, nonce) : mean;
		}
	}
}

static void DpGuptMeanNoiseFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	DpGuptMeanNoiseFunctionInternal(args, state, result, false, false, GuptMeanStatsRow);
}

static void DpGuptMeanNoiseScaleFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	DpGuptMeanNoiseFunctionInternal(args, state, result, true, false, GuptMeanStatsRow);
}

static void DpGuptMeanRecordNoiseScaleFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	DpGuptMeanNoiseFunctionInternal(args, state, result, false, true, GuptMeanStatsRow);
}

static bool GuptMeanMStatsRow(const list_entry_t &entry, const UnifiedVectorFormat &child_data,
                              const PAC_FLOAT *child_values, double epsilon, int sample_lanes_raw, double lower_bound,
                              double upper_bound, double empty_default, double &mean, double &scale) {
	if (entry.length <= DP_SASS_DEFAULT_M || entry.length > DP_SASS_MAX_M || (entry.length & (entry.length - 1)) != 0) {
		return false;
	}
	int sample_lanes = ValidateDpSampleLanes(sample_lanes_raw);
	if (sample_lanes != 1) {
		return false;
	}
	if (epsilon <= 0.0 || !std::isfinite(epsilon)) {
		return false;
	}
	if (!std::isfinite(lower_bound) || !std::isfinite(upper_bound) || lower_bound >= upper_bound) {
		return false;
	}

	// With one disjoint lane per PU, changing one PU changes one bounded lane answer.
	// Averaging m lanes therefore has sensitivity (upper-lower)/m.
	double fill = std::max(lower_bound, std::min(upper_bound, empty_default));
	double sum = 0.0;
	for (idx_t j = 0; j < entry.length; j++) {
		auto child_idx = child_data.sel->get_index(entry.offset + j);
		sum += child_data.validity.RowIsValid(child_idx)
		           ? std::max(lower_bound, std::min(upper_bound, static_cast<double>(child_values[child_idx])))
		           : fill;
	}
	double sample_count = static_cast<double>(entry.length);
	mean = sum / sample_count;
	scale = (upper_bound - lower_bound) / (sample_count * epsilon);
	PRIVACY_DEBUG_PRINT("dp_sass gupt-mean-m: m=" + std::to_string(entry.length) + " mean=" + std::to_string(mean) +
	                    " range=[" + std::to_string(lower_bound) + "," + std::to_string(upper_bound) +
	                    "] scale=" + std::to_string(scale));
	return true;
}

static void DpGuptMeanMNoiseFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	DpGuptMeanNoiseFunctionInternal(args, state, result, false, false, GuptMeanMStatsRow);
}

static void DpGuptMeanMNoiseScaleFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	DpGuptMeanNoiseFunctionInternal(args, state, result, true, false, GuptMeanMStatsRow);
}

static void DpGuptMeanMRecordNoiseScaleFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	DpGuptMeanNoiseFunctionInternal(args, state, result, false, true, GuptMeanMStatsRow);
}

struct NonEmptyMeanStats {
	double shifted_sum = 0.0;
	double valid_count = 0.0;
	double mean = 0.0;
	double numerator_noise_scale = 0.0;
	double denominator_noise_scale = 0.0;
};

static bool NonEmptyMeanStatsRow(const list_entry_t &entry, const UnifiedVectorFormat &child_data,
                                 const PAC_FLOAT *child_values, double epsilon, int sample_lanes_raw,
                                 double lower_bound, double upper_bound, double empty_baseline,
                                 double value_sensitivity, NonEmptyMeanStats &stats) {
	bool variable_m = entry.length > DP_SASS_DEFAULT_M;
	if (entry.length < DP_SASS_DEFAULT_M || entry.length > DP_SASS_MAX_M ||
	    (variable_m && (entry.length & (entry.length - 1)) != 0)) {
		return false;
	}
	int sample_lanes = ValidateDpSampleLanes(sample_lanes_raw);
	if (variable_m && sample_lanes != 1) {
		return false;
	}
	if (epsilon <= 0.0 || !std::isfinite(epsilon)) {
		return false;
	}
	if (!std::isfinite(lower_bound) || !std::isfinite(upper_bound) || lower_bound >= upper_bound) {
		return false;
	}
	if (!std::isfinite(empty_baseline) || empty_baseline < lower_bound || empty_baseline > upper_bound ||
	    !std::isfinite(value_sensitivity) || value_sensitivity <= 0.0) {
		return false;
	}

	for (idx_t j = 0; j < entry.length; j++) {
		auto child_idx = child_data.sel->get_index(entry.offset + j);
		if (!child_data.validity.RowIsValid(child_idx)) {
			continue;
		}
		double value = std::max(lower_bound, std::min(upper_bound, static_cast<double>(child_values[child_idx])));
		stats.shifted_sum += value - empty_baseline;
		stats.valid_count += 1.0;
	}

	// Split the cell budget equally across the shifted SUM and populated-lane COUNT.
	double component_epsilon = epsilon / 2.0;
	stats.numerator_noise_scale = (static_cast<double>(sample_lanes) * value_sensitivity) / component_epsilon;
	stats.denominator_noise_scale = static_cast<double>(sample_lanes) / component_epsilon;
	stats.mean = empty_baseline + stats.shifted_sum / std::max(1.0, stats.valid_count);
	stats.mean = std::max(lower_bound, std::min(upper_bound, stats.mean));
	return true;
}

static double ReleaseNonEmptyMean(const NonEmptyMeanStats &stats, double lower_bound, double upper_bound,
                                  double empty_baseline, bool noise_enabled, uint64_t seed, uint64_t group_identity,
                                  uint64_t aggregate_index, uint64_t aggregate_count) {
	if (!noise_enabled) {
		return stats.mean;
	}
	uint64_t numerator_nonce =
	    DpSassCellNoiseNonce(group_identity, aggregate_index, aggregate_count, DpSassNoiseChannel::RATIO_NUMERATOR);
	uint64_t denominator_nonce =
	    DpSassCellNoiseNonce(group_identity, aggregate_index, aggregate_count, DpSassNoiseChannel::RATIO_DENOMINATOR);
	double noised_sum = AddLaplaceNoise(stats.shifted_sum, stats.numerator_noise_scale, seed, numerator_nonce);
	double noised_count = AddLaplaceNoise(stats.valid_count, stats.denominator_noise_scale, seed, denominator_nonce);
	double released = empty_baseline + noised_sum / std::max(1.0, noised_count);
	return std::max(lower_bound, std::min(upper_bound, released));
}

static void DpNonEmptyMeanNoiseFunctionInternal(DataChunk &args, ExpressionState &state, Vector &result,
                                                bool record_scales) {
	auto &context = state.GetContext();
	uint64_t seed = GetDpNoiseSeed(context);
	bool noise_enabled = true;
	Value noise_val;
	if (context.TryGetCurrentSetting("privacy_noise", noise_val) && !noise_val.IsNull()) {
		noise_enabled = noise_val.GetValue<bool>();
	}

	idx_t count = args.size();
	auto &list_vec = args.data[0];
	UnifiedVectorFormat list_data, epsilon_data, sample_lanes_data, lower_data, upper_data, baseline_data,
	    sensitivity_data, aggregate_index_data;
	UnifiedVectorFormat aggregate_count_data, group_identity_data;
	list_vec.ToUnifiedFormat(count, list_data);
	args.data[1].ToUnifiedFormat(count, epsilon_data);
	args.data[2].ToUnifiedFormat(count, sample_lanes_data);
	args.data[3].ToUnifiedFormat(count, lower_data);
	args.data[4].ToUnifiedFormat(count, upper_data);
	args.data[5].ToUnifiedFormat(count, baseline_data);
	args.data[6].ToUnifiedFormat(count, sensitivity_data);
	args.data[7].ToUnifiedFormat(count, aggregate_index_data);
	if (!record_scales) {
		args.data[8].ToUnifiedFormat(count, aggregate_count_data);
		args.data[9].ToUnifiedFormat(count, group_identity_data);
	}

	auto &child_vec = ListVector::GetEntry(list_vec);
	UnifiedVectorFormat child_data;
	child_vec.ToUnifiedFormat(ListVector::GetListSize(list_vec), child_data);
	auto child_values = UnifiedVectorFormat::GetData<PAC_FLOAT>(child_data);
	auto list_entries = UnifiedVectorFormat::GetData<list_entry_t>(list_data);
	auto epsilons = UnifiedVectorFormat::GetData<double>(epsilon_data);
	auto sample_lanes_values = UnifiedVectorFormat::GetData<int32_t>(sample_lanes_data);
	auto lower_values = UnifiedVectorFormat::GetData<double>(lower_data);
	auto upper_values = UnifiedVectorFormat::GetData<double>(upper_data);
	auto baseline_values = UnifiedVectorFormat::GetData<double>(baseline_data);
	auto sensitivity_values = UnifiedVectorFormat::GetData<double>(sensitivity_data);
	auto aggregate_indexes = UnifiedVectorFormat::GetData<int32_t>(aggregate_index_data);
	auto aggregate_counts = record_scales ? nullptr : UnifiedVectorFormat::GetData<int32_t>(aggregate_count_data);
	auto group_identities = record_scales ? nullptr : UnifiedVectorFormat::GetData<int64_t>(group_identity_data);
	auto result_data = FlatVector::GetData<double>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto list_idx = list_data.sel->get_index(i);
		auto epsilon_idx = epsilon_data.sel->get_index(i);
		auto sample_lanes_idx = sample_lanes_data.sel->get_index(i);
		auto lower_idx = lower_data.sel->get_index(i);
		auto upper_idx = upper_data.sel->get_index(i);
		auto baseline_idx = baseline_data.sel->get_index(i);
		auto sensitivity_idx = sensitivity_data.sel->get_index(i);
		auto aggregate_index_idx = aggregate_index_data.sel->get_index(i);
		auto aggregate_count_idx = record_scales ? 0 : aggregate_count_data.sel->get_index(i);
		auto group_identity_idx = record_scales ? 0 : group_identity_data.sel->get_index(i);
		if (!list_data.validity.RowIsValid(list_idx) || !epsilon_data.validity.RowIsValid(epsilon_idx) ||
		    !sample_lanes_data.validity.RowIsValid(sample_lanes_idx) || !lower_data.validity.RowIsValid(lower_idx) ||
		    !upper_data.validity.RowIsValid(upper_idx) || !baseline_data.validity.RowIsValid(baseline_idx) ||
		    !sensitivity_data.validity.RowIsValid(sensitivity_idx) ||
		    !aggregate_index_data.validity.RowIsValid(aggregate_index_idx) ||
		    (!record_scales && (!aggregate_count_data.validity.RowIsValid(aggregate_count_idx) ||
		                        !group_identity_data.validity.RowIsValid(group_identity_idx)))) {
			result_validity.SetInvalid(i);
			continue;
		}

		NonEmptyMeanStats stats;
		double lower_bound = lower_values[lower_idx];
		double upper_bound = upper_values[upper_idx];
		double empty_baseline = baseline_values[baseline_idx];
		if (!NonEmptyMeanStatsRow(list_entries[list_idx], child_data, child_values, epsilons[epsilon_idx],
		                          sample_lanes_values[sample_lanes_idx], lower_bound, upper_bound, empty_baseline,
		                          sensitivity_values[sensitivity_idx], stats)) {
			result_validity.SetInvalid(i);
			continue;
		}
		if (record_scales) {
			DpSassNoiseScaleQueryRecords().push_back(DpSassNoiseScaleQueryRecord::Ratio(
			    aggregate_indexes[aggregate_index_idx], stats.numerator_noise_scale, stats.denominator_noise_scale,
			    static_cast<int32_t>(stats.valid_count)));
			result_data[i] = stats.mean;
		} else {
			if (aggregate_counts[aggregate_count_idx] <= 0 || group_identities[group_identity_idx] <= 0) {
				throw InternalException("dp_nonempty_mean_noise: invalid aggregate-cell identity");
			}
			result_data[i] = ReleaseNonEmptyMean(stats, lower_bound, upper_bound, empty_baseline, noise_enabled, seed,
			                                     static_cast<uint64_t>(group_identities[group_identity_idx]),
			                                     static_cast<uint64_t>(aggregate_indexes[aggregate_index_idx]),
			                                     static_cast<uint64_t>(aggregate_counts[aggregate_count_idx]));
		}
		PRIVACY_DEBUG_PRINT("dp_sass nonempty-lane mean: valid=" + std::to_string(stats.valid_count) +
		                    " numerator_scale=" + std::to_string(stats.numerator_noise_scale) +
		                    " denominator_scale=" + std::to_string(stats.denominator_noise_scale));
	}
}

static void DpNonEmptyMeanNoiseFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	DpNonEmptyMeanNoiseFunctionInternal(args, state, result, false);
}

static void DpNonEmptyMeanRecordNoiseScaleFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	DpNonEmptyMeanNoiseFunctionInternal(args, state, result, true);
}

void RegisterDpSmoothMedianNoiseFunction(ExtensionLoader &loader) {
	ScalarFunction mask_function("dp_sample_mask", {LogicalType::UBIGINT}, LogicalType::UBIGINT, DpSampleMaskFunction);
	CreateScalarFunctionInfo mask_info(mask_function);
	FunctionDescription mask_desc;
	mask_desc.description = "[INTERNAL] Maps a PU hash to the 64-bit SASS sample-lane mask.";
	mask_info.descriptions.push_back(std::move(mask_desc));
	loader.RegisterFunction(std::move(mask_info));

	auto list_type = LogicalType::LIST(PacFloatLogicalType());
	child_list_t<LogicalType> stability_children;
	stability_children.emplace_back("lane_count", LogicalType::INTEGER);
	stability_children.emplace_back("valid_count", LogicalType::INTEGER);
	stability_children.emplace_back("null_count", LogicalType::INTEGER);
	stability_children.emplace_back("mean", LogicalType::DOUBLE);
	stability_children.emplace_back("stddev_pop", LogicalType::DOUBLE);
	stability_children.emplace_back("cv", LogicalType::DOUBLE);
	stability_children.emplace_back("min", LogicalType::DOUBLE);
	stability_children.emplace_back("median", LogicalType::DOUBLE);
	stability_children.emplace_back("max", LogicalType::DOUBLE);
	stability_children.emplace_back("range", LogicalType::DOUBLE);
	stability_children.emplace_back("mad", LogicalType::DOUBLE);
	stability_children.emplace_back("max_abs_dev", LogicalType::DOUBLE);
	stability_children.emplace_back("stable_stddev_1", LogicalType::BOOLEAN);
	stability_children.emplace_back("stable_mad_1", LogicalType::BOOLEAN);
	ScalarFunction stability("dp_sass_stability", {list_type}, LogicalType::STRUCT(std::move(stability_children)),
	                         DpSassStabilityFunction);
	CreateScalarFunctionInfo stability_info(stability);
	FunctionDescription stability_desc;
	stability_desc.description = "Returns descriptive stability statistics over SASS's 64 lane answers.";
	stability_info.descriptions.push_back(std::move(stability_desc));
	loader.RegisterFunction(std::move(stability_info));

	ScalarFunction record_stability("dp_sass_record_stability", {list_type, LogicalType::INTEGER}, LogicalType::DOUBLE,
	                                DpSassRecordStabilityFunction);
	CreateScalarFunctionInfo record_info(record_stability);
	FunctionDescription record_desc;
	record_desc.description =
	    "[INTERNAL] Records SASS lane stability stats for dp_sass_stability_query and returns the sample median.";
	record_info.descriptions.push_back(std::move(record_desc));
	loader.RegisterFunction(std::move(record_info));

	ScalarFunctionSet set("dp_smooth_median_noise");
	set.AddFunction(ScalarFunction("dp_smooth_median_noise",
	                               {list_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER},
	                               LogicalType::DOUBLE, DpSmoothMedianNoiseFunction));
	set.AddFunction(ScalarFunction("dp_smooth_median_noise",
	                               {list_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER,
	                                LogicalType::DOUBLE, LogicalType::DOUBLE},
	                               LogicalType::DOUBLE, DpSmoothMedianNoiseFunction));
	// 7-arg: trailing empty-lane default (per aggregate kind) — compiler-emitted bounded form.
	set.AddFunction(ScalarFunction("dp_smooth_median_noise",
	                               {list_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER,
	                                LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                               LogicalType::DOUBLE, DpSmoothMedianNoiseFunction));
	// 10-arg: compiler-emitted form with aggregate index, aggregate count, and group identity.
	set.AddFunction(ScalarFunction("dp_smooth_median_noise",
	                               {list_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER,
	                                LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER,
	                                LogicalType::INTEGER, LogicalType::BIGINT},
	                               LogicalType::DOUBLE, DpSmoothMedianNoiseFunction));
	CreateScalarFunctionInfo info(set);
	FunctionDescription desc;
	desc.description = "Applies smooth-sensitivity median release to SASS sample counters.";
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));

	ScalarFunctionSet scale_set("dp_smooth_median_noise_scale");
	scale_set.AddFunction(ScalarFunction("dp_smooth_median_noise_scale",
	                                     {list_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER},
	                                     LogicalType::DOUBLE, DpSmoothMedianNoiseScaleFunction));
	scale_set.AddFunction(ScalarFunction("dp_smooth_median_noise_scale",
	                                     {list_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER,
	                                      LogicalType::DOUBLE, LogicalType::DOUBLE},
	                                     LogicalType::DOUBLE, DpSmoothMedianNoiseScaleFunction));
	scale_set.AddFunction(ScalarFunction("dp_smooth_median_noise_scale",
	                                     {list_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER,
	                                      LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                                     LogicalType::DOUBLE, DpSmoothMedianNoiseScaleFunction));
	CreateScalarFunctionInfo scale_info(scale_set);
	FunctionDescription scale_desc;
	scale_desc.description = "[INTERNAL] Returns the smooth-median SASS Laplace noise scale.";
	scale_info.descriptions.push_back(std::move(scale_desc));
	loader.RegisterFunction(std::move(scale_info));

	ScalarFunction record_scale("dp_smooth_median_record_noise_scale",
	                            {list_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER,
	                             LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER},
	                            LogicalType::DOUBLE, DpSmoothMedianRecordNoiseScaleFunction);
	CreateScalarFunctionInfo record_scale_info(record_scale);
	FunctionDescription record_scale_desc;
	record_scale_desc.description =
	    "[INTERNAL] Records the smooth-median SASS Laplace scale and returns the non-private estimator.";
	record_scale_info.descriptions.push_back(std::move(record_scale_desc));
	loader.RegisterFunction(std::move(record_scale_info));

	// GUPT-style mean release: mean of the 64 lane answers + pure-ε Laplace noise. No delta.
	ScalarFunctionSet gupt_set("dp_gupt_mean_noise");
	gupt_set.AddFunction(
	    ScalarFunction("dp_gupt_mean_noise",
	                   {list_type, LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                   LogicalType::DOUBLE, DpGuptMeanNoiseFunction));
	// 6-arg: trailing empty-lane default (per aggregate kind) — compiler-emitted form.
	gupt_set.AddFunction(ScalarFunction("dp_gupt_mean_noise",
	                                    {list_type, LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::DOUBLE,
	                                     LogicalType::DOUBLE, LogicalType::DOUBLE},
	                                    LogicalType::DOUBLE, DpGuptMeanNoiseFunction));
	gupt_set.AddFunction(
	    ScalarFunction("dp_gupt_mean_noise",
	                   {list_type, LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::DOUBLE, LogicalType::DOUBLE,
	                    LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::BIGINT},
	                   LogicalType::DOUBLE, DpGuptMeanNoiseFunction));
	CreateScalarFunctionInfo gupt_info(gupt_set);
	FunctionDescription gupt_desc;
	gupt_desc.description = "Applies GUPT-style mean release (pure-ε Laplace) to 64 sample lane answers.";
	gupt_info.descriptions.push_back(std::move(gupt_desc));
	loader.RegisterFunction(std::move(gupt_info));

	ScalarFunctionSet gupt_scale_set("dp_gupt_mean_noise_scale");
	gupt_scale_set.AddFunction(
	    ScalarFunction("dp_gupt_mean_noise_scale",
	                   {list_type, LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                   LogicalType::DOUBLE, DpGuptMeanNoiseScaleFunction));
	gupt_scale_set.AddFunction(ScalarFunction("dp_gupt_mean_noise_scale",
	                                          {list_type, LogicalType::DOUBLE, LogicalType::INTEGER,
	                                           LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                                          LogicalType::DOUBLE, DpGuptMeanNoiseScaleFunction));
	CreateScalarFunctionInfo gupt_scale_info(gupt_scale_set);
	FunctionDescription gupt_scale_desc;
	gupt_scale_desc.description = "[INTERNAL] Returns the GUPT-style SASS mean-release Laplace noise scale.";
	gupt_scale_info.descriptions.push_back(std::move(gupt_scale_desc));
	loader.RegisterFunction(std::move(gupt_scale_info));

	ScalarFunction gupt_record_scale("dp_gupt_mean_record_noise_scale",
	                                 {list_type, LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::DOUBLE,
	                                  LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER},
	                                 LogicalType::DOUBLE, DpGuptMeanRecordNoiseScaleFunction);
	CreateScalarFunctionInfo gupt_record_scale_info(gupt_record_scale);
	FunctionDescription gupt_record_scale_desc;
	gupt_record_scale_desc.description =
	    "[INTERNAL] Records the GUPT-style SASS mean-release scale and returns the non-private estimator.";
	gupt_record_scale_info.descriptions.push_back(std::move(gupt_record_scale_desc));
	loader.RegisterFunction(std::move(gupt_record_scale_info));

	ScalarFunctionSet gupt_m_set("dp_gupt_mean_m_noise");
	gupt_m_set.AddFunction(
	    ScalarFunction("dp_gupt_mean_m_noise",
	                   {list_type, LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                   LogicalType::DOUBLE, DpGuptMeanMNoiseFunction));
	gupt_m_set.AddFunction(ScalarFunction("dp_gupt_mean_m_noise",
	                                      {list_type, LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::DOUBLE,
	                                       LogicalType::DOUBLE, LogicalType::DOUBLE},
	                                      LogicalType::DOUBLE, DpGuptMeanMNoiseFunction));
	gupt_m_set.AddFunction(
	    ScalarFunction("dp_gupt_mean_m_noise",
	                   {list_type, LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::DOUBLE, LogicalType::DOUBLE,
	                    LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::BIGINT},
	                   LogicalType::DOUBLE, DpGuptMeanMNoiseFunction));
	CreateScalarFunctionInfo gupt_m_info(gupt_m_set);
	FunctionDescription gupt_m_desc;
	gupt_m_desc.description = "Applies experimental variable-m GUPT-style mean release to sample lane answers.";
	gupt_m_info.descriptions.push_back(std::move(gupt_m_desc));
	loader.RegisterFunction(std::move(gupt_m_info));

	ScalarFunctionSet gupt_m_scale_set("dp_gupt_mean_m_noise_scale");
	gupt_m_scale_set.AddFunction(
	    ScalarFunction("dp_gupt_mean_m_noise_scale",
	                   {list_type, LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                   LogicalType::DOUBLE, DpGuptMeanMNoiseScaleFunction));
	gupt_m_scale_set.AddFunction(ScalarFunction("dp_gupt_mean_m_noise_scale",
	                                            {list_type, LogicalType::DOUBLE, LogicalType::INTEGER,
	                                             LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                                            LogicalType::DOUBLE, DpGuptMeanMNoiseScaleFunction));
	CreateScalarFunctionInfo gupt_m_scale_info(gupt_m_scale_set);
	FunctionDescription gupt_m_scale_desc;
	gupt_m_scale_desc.description = "[INTERNAL] Returns the variable-m GUPT-style SASS Laplace noise scale.";
	gupt_m_scale_info.descriptions.push_back(std::move(gupt_m_scale_desc));
	loader.RegisterFunction(std::move(gupt_m_scale_info));

	ScalarFunction gupt_m_record_scale("dp_gupt_mean_m_record_noise_scale",
	                                   {list_type, LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::DOUBLE,
	                                    LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER},
	                                   LogicalType::DOUBLE, DpGuptMeanMRecordNoiseScaleFunction);
	CreateScalarFunctionInfo gupt_m_record_scale_info(gupt_m_record_scale);
	FunctionDescription gupt_m_record_scale_desc;
	gupt_m_record_scale_desc.description =
	    "[INTERNAL] Records the variable-m GUPT-style SASS mean-release scale and returns the non-private estimator.";
	gupt_m_record_scale_info.descriptions.push_back(std::move(gupt_m_record_scale_desc));
	loader.RegisterFunction(std::move(gupt_m_record_scale_info));

	ScalarFunction nonempty_mean("dp_nonempty_mean_noise",
	                             {list_type, LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::DOUBLE,
	                              LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::INTEGER,
	                              LogicalType::INTEGER, LogicalType::BIGINT},
	                             LogicalType::DOUBLE, DpNonEmptyMeanNoiseFunction);
	CreateScalarFunctionInfo nonempty_mean_info(nonempty_mean);
	FunctionDescription nonempty_mean_desc;
	nonempty_mean_desc.description =
	    "Releases the mean of non-empty SASS lanes using independently-noised shifted SUM and lane COUNT.";
	nonempty_mean_info.descriptions.push_back(std::move(nonempty_mean_desc));
	loader.RegisterFunction(std::move(nonempty_mean_info));

	ScalarFunction nonempty_mean_record("dp_nonempty_mean_record_noise_scale",
	                                    {list_type, LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::DOUBLE,
	                                     LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE,
	                                     LogicalType::INTEGER},
	                                    LogicalType::DOUBLE, DpNonEmptyMeanRecordNoiseScaleFunction);
	CreateScalarFunctionInfo nonempty_mean_record_info(nonempty_mean_record);
	FunctionDescription nonempty_mean_record_desc;
	nonempty_mean_record_desc.description =
	    "[INTERNAL] Records numerator/count noise scales for the non-empty-lane SASS mean.";
	nonempty_mean_record_info.descriptions.push_back(std::move(nonempty_mean_record_desc));
	loader.RegisterFunction(std::move(nonempty_mean_record_info));

	// dp_aggregate: naive DP sample-and-aggregate terminal, mirroring pac_aggregate's shape
	// (samples, counts, ...). `counts` is accepted for symmetry and ignored by the median path.
	ScalarFunction dp_aggregate("dp_aggregate",
	                            {list_type, LogicalType::LIST(LogicalType::DOUBLE), LogicalType::DOUBLE,
	                             LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                            LogicalType::DOUBLE, DpAggregateFunction);
	CreateScalarFunctionInfo agg_info(dp_aggregate);
	FunctionDescription agg_desc;
	agg_desc.description =
	    "Naive DP sample-and-aggregate terminal: median + smooth-sensitivity Laplace over 64 lane answers.";
	agg_info.descriptions.push_back(std::move(agg_desc));
	loader.RegisterFunction(std::move(agg_info));
}

} // namespace duckdb
