#include "aggregates/dp_laplace_noise.hpp"
#include "aggregates/as_aggregate.hpp"
#include "privacy_debug.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"

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

// Exact smooth sensitivity of the median via the Nissim-Raskhodnikova-Smith divide-and-conquer
// algorithm (Algorithm 1 SmoothSensitivityMedian + Algorithm 2 JList). The padded array `x` holds
// the 64 clipped+sorted sample answers in x[1..64], bracketed by domain sentinels x[0]=lower_bound
// and x[65]=upper_bound; the median index is p = ceil(64/2) = 32 (released median = values[31]).
// SCORE(i, j) = (x[j] - x[i]) * exp(-beta_eff * (j - i - 1)) over pairs i <= p <= j; JList exploits
// the monotonicity of the optimal j*(b) in b to find all maximizers in O(n log n).
namespace {

constexpr int SMOOTH_MEDIAN_N = 64;
constexpr int SMOOTH_MEDIAN_P = 32; // ceil(64/2): 1-indexed median rank

// argmax_{L <= j <= U} SCORE(b, j)
int SmoothMedianArgMax(const std::array<double, SMOOTH_MEDIAN_N + 2> &x, double beta_eff, int b, int lower, int upper) {
	int best_j = lower;
	double best = -1.0;
	for (int j = lower; j <= upper; j++) {
		double score = (x[j] - x[b]) * std::exp(-beta_eff * static_cast<double>(j - b - 1));
		if (score > best) {
			best = score;
			best_j = j;
		}
	}
	return best_j;
}

// JList(a, c, L, U): fills j_star[b] for b in [a, c] using the monotone search-range narrowing.
void SmoothMedianJList(const std::array<double, SMOOTH_MEDIAN_N + 2> &x, double beta_eff,
                       std::array<int, SMOOTH_MEDIAN_P + 1> &j_star, int a, int c, int lower, int upper) {
	if (c < a) {
		return;
	}
	int b = (a + c) / 2;
	int jb = SmoothMedianArgMax(x, beta_eff, b, lower, upper);
	j_star[b] = jb;
	SmoothMedianJList(x, beta_eff, j_star, a, b - 1, lower, jb);
	SmoothMedianJList(x, beta_eff, j_star, b + 1, c, jb, upper);
}

} // namespace

static double SmoothMedianSensitivityExact(const std::array<double, 64> &values, double beta, int sample_lanes,
                                           double lower_bound, double upper_bound) {
	// One PU can move up to `sample_lanes` lane answers, so the element-level NRS envelope is
	// reweighted to PU-distance via beta_eff = beta / sample_lanes (beta-smooth in PU distance).
	double beta_eff = beta / static_cast<double>(sample_lanes);
	std::array<double, SMOOTH_MEDIAN_N + 2> x;
	x[0] = lower_bound;
	for (int i = 0; i < SMOOTH_MEDIAN_N; i++) {
		x[i + 1] = values[i];
	}
	x[SMOOTH_MEDIAN_N + 1] = upper_bound;

	std::array<int, SMOOTH_MEDIAN_P + 1> j_star;
	j_star.fill(SMOOTH_MEDIAN_P);
	SmoothMedianJList(x, beta_eff, j_star, 0, SMOOTH_MEDIAN_P, SMOOTH_MEDIAN_P, SMOOTH_MEDIAN_N + 1);

	double smooth = 0.0;
	for (int i = 0; i <= SMOOTH_MEDIAN_P; i++) {
		int j = j_star[i];
		double score = (x[j] - x[i]) * std::exp(-beta_eff * static_cast<double>(j - i - 1));
		smooth = std::max(smooth, score);
	}
	return std::max(0.0, smooth);
}

// Shared per-row core for the sample-and-aggregate median release. Validates the inputs,
// clips (when bounded) and sorts the 64 sample answers, takes the median (values[31]),
// computes the smooth-sensitivity scale, and adds Laplace noise. Returns false when the row
// is invalid (caller marks the result NULL); otherwise writes the released value to `out`.
static bool SmoothMedianNoiseRow(const list_entry_t &entry, const UnifiedVectorFormat &child_data,
                                 const PAC_FLOAT *child_values, double epsilon, double delta, int sample_lanes_raw,
                                 bool bounded, double lower_bound, double upper_bound, bool noise_enabled,
                                 uint64_t seed, double &out) {
	if (entry.length != 64) {
		return false;
	}
	int sample_lanes = ValidateDpSampleLanes(sample_lanes_raw);
	if (epsilon <= 0.0 || delta <= 0.0 || delta >= 0.5 || !std::isfinite(epsilon) || !std::isfinite(delta)) {
		return false;
	}
	if (bounded && (!std::isfinite(lower_bound) || !std::isfinite(upper_bound) || lower_bound >= upper_bound)) {
		return false;
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
		return false;
	}
	std::sort(values.begin(), values.begin() + valid_count);
	for (idx_t j = valid_count; j < values.size(); j++) {
		values[j] = values[valid_count - 1];
	}
	double median = values[31];
	if (!noise_enabled) {
		out = median;
		return true;
	}
	double beta = epsilon / (2.0 * std::log(2.0 / delta));
	double smooth = bounded ? SmoothMedianSensitivityExact(values, beta, sample_lanes, lower_bound, upper_bound)
	                        : SmoothMedianSensitivity(values, beta, sample_lanes);
	PRIVACY_DEBUG_PRINT("dp_sass smooth-median: median=" + std::to_string(median) + " smooth_sensitivity=" +
	                    std::to_string(smooth) + " scale=" + std::to_string((2.0 * smooth) / epsilon));
	out = median + LaplaceNoise((2.0 * smooth) / epsilon, seed ^ (entry.offset * PAC_MAGIC_HASH));
	return true;
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
		double epsilon = epsilons[eps_idx];
		double delta = deltas[delta_idx];
		double lower_bound = bounded ? lower_values[lower_idx] : 0.0;
		double upper_bound = bounded ? upper_values[upper_idx] : 0.0;
		double released = 0.0;
		if (SmoothMedianNoiseRow(entry, child_data, child_values, epsilon, delta, sample_lanes_values[sample_lanes_idx],
		                         bounded, lower_bound, upper_bound, noise_enabled, seed, released)) {
			result_data[i] = released;
		} else {
			result_validity.SetInvalid(i);
		}
	}
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
		double released = 0.0;
		if (SmoothMedianNoiseRow(list_entries[list_idx], child_data, child_values, epsilons[eps_idx], deltas[delta_idx],
		                         sample_lanes_values[sample_lanes_idx], /*bounded=*/true, lower_values[lower_idx],
		                         upper_values[upper_idx], noise_enabled, seed, released)) {
			result_data[i] = released;
		} else {
			result_validity.SetInvalid(i);
		}
	}
}

// GUPT-style release: the MEAN of the valid lane (block) answers, clipped to [lower,upper], plus
// pure-ε Laplace noise (no δ). Each PU's hash sets `sample_lanes` of the 64 lane bits, so removing
// one PU can change up to `sample_lanes` lane answers, each by at most (upper-lower). The mean over
// `valid_count` lanes therefore has global sensitivity sample_lanes*(upper-lower)/valid_count, and
// the Laplace scale is that divided by ε. (This is the overlapping-subsample analogue of GUPT's
// disjoint-block mean, mirroring how the median path uses beta_eff = beta/sample_lanes.)
// Pure-ε exponential-mechanism quantile: estimate the value at order rank `target_rank` (0..n) of
// `sorted` (ascending, already clamped to [lo,hi]) over the continuous domain [lo,hi]. A candidate
// in gap j (between the j-th and (j+1)-th order statistic, with lo/hi as sentinels) has rank j, so
// its utility is u_j = -|j - target_rank|. One PU can move up to `sample_lanes` lane answers, so the
// rank/utility sensitivity is `sample_lanes`; the exponential weight is exp(ε·u_j/(2·sample_lanes))
// times the gap width. Samples a gap, then a uniform point inside it.
static double PrivateQuantileExpMech(const std::vector<double> &sorted, double lo, double hi, double target_rank,
                                     double epsilon, int sample_lanes, std::mt19937_64 &gen) {
	int n = static_cast<int>(sorted.size());
	std::vector<double> e;
	e.reserve(n + 2);
	e.push_back(lo);
	for (double v : sorted) {
		e.push_back(std::min(hi, std::max(lo, v)));
	}
	e.push_back(hi);
	double alpha = epsilon / (2.0 * static_cast<double>(sample_lanes));
	auto log_weight = [&](int j) {
		double width = e[j + 1] - e[j];
		if (width <= 0.0) {
			return -std::numeric_limits<double>::infinity();
		}
		return std::log(width) + alpha * (-std::fabs(static_cast<double>(j) - target_rank));
	};
	double best_logw = -std::numeric_limits<double>::infinity();
	for (int j = 0; j <= n; j++) {
		best_logw = std::max(best_logw, log_weight(j));
	}
	if (!std::isfinite(best_logw)) {
		return 0.5 * (lo + hi); // all gaps zero-width → degenerate
	}
	double total = 0.0;
	std::vector<std::pair<int, double>> cumulative; // (gap index, cumulative unnormalized weight)
	for (int j = 0; j <= n; j++) {
		double logw = log_weight(j);
		if (!std::isfinite(logw)) {
			continue;
		}
		total += std::exp(logw - best_logw);
		cumulative.emplace_back(j, total);
	}
	std::uniform_real_distribution<double> unif(0.0, 1.0);
	double r = unif(gen) * total;
	int chosen = cumulative.back().first;
	for (auto &c : cumulative) {
		if (r <= c.second) {
			chosen = c.first;
			break;
		}
	}
	std::uniform_real_distribution<double> within(e[chosen], e[chosen + 1]);
	return within(gen);
}

// Privately estimate a data-covering range [out_lo,out_hi] within the public envelope [pub_lo,pub_hi]
// by spending `epsilon_range` total across a min-rank and max-rank exponential-mechanism quantile
// (ε_range/2 each). Returns false (caller falls back to the public envelope) if the estimate is
// degenerate or inverted.
static bool PrivateRangeEstimate(const std::vector<double> &sorted, double pub_lo, double pub_hi, double epsilon_range,
                                 int sample_lanes, std::mt19937_64 &gen, double &out_lo, double &out_hi) {
	if (sorted.empty() || epsilon_range <= 0.0 || !std::isfinite(epsilon_range)) {
		return false;
	}
	double eq = epsilon_range / 2.0;
	double n = static_cast<double>(sorted.size());
	out_lo = PrivateQuantileExpMech(sorted, pub_lo, pub_hi, 0.0, eq, sample_lanes, gen);
	out_hi = PrivateQuantileExpMech(sorted, pub_lo, pub_hi, n, eq, sample_lanes, gen);
	return out_lo < out_hi;
}

// GUPT mean release with optional pure-ε private range estimation. `range_fraction` ∈ [0,1): when
// >0, that fraction of ε is spent privately estimating the clip range from the lane answers (within
// the public [lower_bound,upper_bound] envelope); the remaining ε calibrates the mean's Laplace
// noise. The whole path stays pure ε-DP.
static bool GuptMeanNoiseRow(const list_entry_t &entry, const UnifiedVectorFormat &child_data,
                             const PAC_FLOAT *child_values, double epsilon, int sample_lanes_raw, double lower_bound,
                             double upper_bound, double range_fraction, bool noise_enabled, uint64_t seed,
                             double &out) {
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
	std::vector<double> vals;
	vals.reserve(64);
	for (idx_t j = 0; j < 64; j++) {
		auto child_idx = child_data.sel->get_index(entry.offset + j);
		if (child_data.validity.RowIsValid(child_idx)) {
			double value = static_cast<double>(child_values[child_idx]);
			vals.push_back(std::max(lower_bound, std::min(upper_bound, value)));
		}
	}
	if (vals.size() < 32) {
		return false;
	}

	double eff_lo = lower_bound;
	double eff_hi = upper_bound;
	double release_eps = epsilon;
	if (noise_enabled && range_fraction > 0.0) {
		std::sort(vals.begin(), vals.end());
		std::mt19937_64 gen(seed ^ (entry.offset * 0x9E3779B97F4A7C15ULL));
		double est_lo = eff_lo;
		double est_hi = eff_hi;
		if (PrivateRangeEstimate(vals, lower_bound, upper_bound, range_fraction * epsilon, sample_lanes, gen, est_lo,
		                         est_hi)) {
			eff_lo = est_lo;
			eff_hi = est_hi;
			release_eps = epsilon * (1.0 - range_fraction);
		}
	}

	double sum = 0.0;
	for (double v : vals) {
		sum += std::max(eff_lo, std::min(eff_hi, v));
	}
	double mean = sum / static_cast<double>(vals.size());
	if (!noise_enabled) {
		out = mean;
		return true;
	}
	double scale =
	    (static_cast<double>(sample_lanes) * (eff_hi - eff_lo)) / (static_cast<double>(vals.size()) * release_eps);
	PRIVACY_DEBUG_PRINT("dp_sass gupt-mean: mean=" + std::to_string(mean) + " range=[" + std::to_string(eff_lo) + "," +
	                    std::to_string(eff_hi) + "] scale=" + std::to_string(scale));
	out = mean + LaplaceNoise(scale, seed ^ (entry.offset * PAC_MAGIC_HASH));
	return true;
}

// Signature: dp_gupt_mean_noise(LIST<FLOAT> samples, DOUBLE epsilon, INTEGER lanes, DOUBLE lower,
//                               DOUBLE upper, DOUBLE range_fraction) -> DOUBLE. Pure ε-DP (no delta).
// range_fraction=0 uses the public [lower,upper]; >0 spends that fraction of ε on private range
// estimation within the public envelope.
static void DpGuptMeanNoiseFunction(DataChunk &args, ExpressionState &state, Vector &result) {
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
	UnifiedVectorFormat list_data, epsilon_data, sample_lanes_data, lower_data, upper_data, range_data;
	list_vec.ToUnifiedFormat(count, list_data);
	args.data[1].ToUnifiedFormat(count, epsilon_data);
	args.data[2].ToUnifiedFormat(count, sample_lanes_data);
	args.data[3].ToUnifiedFormat(count, lower_data);
	args.data[4].ToUnifiedFormat(count, upper_data);
	args.data[5].ToUnifiedFormat(count, range_data);

	auto &child_vec = ListVector::GetEntry(list_vec);
	UnifiedVectorFormat child_data;
	child_vec.ToUnifiedFormat(ListVector::GetListSize(list_vec), child_data);
	auto child_values = UnifiedVectorFormat::GetData<PAC_FLOAT>(child_data);
	auto list_entries = UnifiedVectorFormat::GetData<list_entry_t>(list_data);
	auto epsilons = UnifiedVectorFormat::GetData<double>(epsilon_data);
	auto sample_lanes_values = UnifiedVectorFormat::GetData<int32_t>(sample_lanes_data);
	auto lower_values = UnifiedVectorFormat::GetData<double>(lower_data);
	auto upper_values = UnifiedVectorFormat::GetData<double>(upper_data);
	auto range_values = UnifiedVectorFormat::GetData<double>(range_data);
	auto result_data = FlatVector::GetData<double>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto list_idx = list_data.sel->get_index(i);
		auto eps_idx = epsilon_data.sel->get_index(i);
		auto sample_lanes_idx = sample_lanes_data.sel->get_index(i);
		auto lower_idx = lower_data.sel->get_index(i);
		auto upper_idx = upper_data.sel->get_index(i);
		auto range_idx = range_data.sel->get_index(i);
		if (!list_data.validity.RowIsValid(list_idx) || !epsilon_data.validity.RowIsValid(eps_idx) ||
		    !sample_lanes_data.validity.RowIsValid(sample_lanes_idx) || !lower_data.validity.RowIsValid(lower_idx) ||
		    !upper_data.validity.RowIsValid(upper_idx) || !range_data.validity.RowIsValid(range_idx)) {
			result_validity.SetInvalid(i);
			continue;
		}
		double released = 0.0;
		if (GuptMeanNoiseRow(list_entries[list_idx], child_data, child_values, epsilons[eps_idx],
		                     sample_lanes_values[sample_lanes_idx], lower_values[lower_idx], upper_values[upper_idx],
		                     range_values[range_idx], noise_enabled, seed, released)) {
			result_data[i] = released;
		} else {
			result_validity.SetInvalid(i);
		}
	}
}

void RegisterDpSmoothMedianNoiseFunction(ExtensionLoader &loader) {
	ScalarFunction mask_function("dp_sample_mask", {LogicalType::UBIGINT}, LogicalType::UBIGINT, DpSampleMaskFunction);
	CreateScalarFunctionInfo mask_info(mask_function);
	FunctionDescription mask_desc;
	mask_desc.description = "[INTERNAL] Maps a PU hash to the 64-bit SASS sample-lane mask.";
	mask_info.descriptions.push_back(std::move(mask_desc));
	loader.RegisterFunction(std::move(mask_info));

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

	// GUPT-style mean release: mean of the 64 lane answers + pure-ε Laplace noise. No delta.
	ScalarFunction gupt_mean("dp_gupt_mean_noise",
	                         {list_type, LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::DOUBLE,
	                          LogicalType::DOUBLE, LogicalType::DOUBLE},
	                         LogicalType::DOUBLE, DpGuptMeanNoiseFunction);
	CreateScalarFunctionInfo gupt_info(gupt_mean);
	FunctionDescription gupt_desc;
	gupt_desc.description = "Applies GUPT-style mean release (pure-ε Laplace) to 64 sample lane answers.";
	gupt_info.descriptions.push_back(std::move(gupt_desc));
	loader.RegisterFunction(std::move(gupt_info));

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
