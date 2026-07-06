#include "aggregates/as_aggregate.hpp"

#include "duckdb.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#include <random>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cmath>
#include <cstring>
#include <type_traits>
#include <limits>

// Every argument to pac_aggregate is the output of a query evaluated on a random subsample of the privacy unit

namespace duckdb {

// ============================================================================
// Global PacPState map for cross-aggregate p-tracking within a query
// ============================================================================
static std::mutex g_pstate_map_mutex; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
static std::unordered_map<uint64_t, std::weak_ptr<PacPState>>
    g_pstate_map; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

std::shared_ptr<PacPState> GetOrCreatePState(uint64_t query_hash) {
	std::lock_guard<std::mutex> lock(g_pstate_map_mutex);
	auto it = g_pstate_map.find(query_hash);
	if (it != g_pstate_map.end()) {
		auto sp = it->second.lock();
		if (sp) {
			return sp;
		}
	}
	auto sp = std::make_shared<PacPState>();
	g_pstate_map[query_hash] = sp;
	// Lazily clean up expired entries (cheap: just scan for expired weak_ptrs)
	for (auto iter = g_pstate_map.begin(); iter != g_pstate_map.end();) {
		if (iter->second.expired()) {
			iter = g_pstate_map.erase(iter);
		} else {
			++iter;
		}
	}
	return sp;
}

// Helper function to generate a random seed - implementation
uint64_t PacGenerateRandomSeed() {
	return std::random_device {}();
}

// Check for absence of sample diversity in PAC aggregates.
// Accumulates per-group stats into bind_data, then checks the population-level condition.
void CheckPacSampleDiversity(uint64_t key_hash, const PAC_FLOAT *buf, uint64_t update_count, const char *aggr_name,
                             PrivBindData &bind_data) {
	if (!bind_data.sample_diversity_check) {
		return;
	}

	bind_data.total_update_count += update_count;

	// Classify this group as suspicious or not
	bool suspicious = false;
	int zero_bits = 64 - pac_popcount64(key_hash);
	if (zero_bits >= 29 && zero_bits <= 35) {
		// Check if all active counters have the same value
		bool found_first = false;
		PAC_FLOAT first_val = 0;
		suspicious = true;
		for (int j = 0; j < 64; j++) {
			if ((key_hash >> j) & 1ULL) {
				if (!found_first) {
					first_val = buf[j];
					found_first = true;
				} else if (buf[j] != first_val) {
					suspicious = false;
					break;
				}
			}
		}
		if (!found_first) {
			suspicious = false;
		}
	}

	if (suspicious) {
		bind_data.suspicious_count++;
	} else {
		bind_data.nonsuspicious_count++;
	}

	// Check population-level condition
	if (bind_data.total_update_count >= PAC_SUSPICIOUS_THRESHOLD &&
	    bind_data.suspicious_count > 2 * bind_data.nonsuspicious_count) {
		throw InvalidInputException("%s detected absence of sample diversity -- which clearly is privacy unsafe",
		                            aggr_name);
	}
}

// ============================================================================
// NOTE: as_count implementation was moved to src/as_count.cpp / src/include/as_count.hpp.
// The noisy-sample computation used by multiple aggregates is kept here and exported.
// ============================================================================

// Forward declaration for the internal variance helper so it can be used above
static double ComputeSecondMomentVariance(const vector<PAC_FLOAT> &values);

// PacNoiseInNull: probabilistically returns true based on bit count in key_hash
// mi: controls probabilistic (mi>0) vs deterministic (mi<=0) mode
// correction: reduces NULL probability by this factor (considers correction times more non-nulls)
bool PacNoiseInNull(uint64_t key_hash, double mi, double correction, std::mt19937_64 &gen) {
	int popcount = pac_popcount64(key_hash);
	// Effective popcount considering correction factor (correction times more non-nulls)
	double effective_popcount = popcount * correction;

	if (mi <= 0.0) {
		// Deterministic mode: NULL when effective_popcount < 1
		return effective_popcount < 1.0;
	}
	// Probabilistic mode: P(NULL) = popcount(~key_hash) / (64 * correction)
	// Equivalently: NULL if popcount(~key_hash) > threshold, where threshold is in [0, 64*correction)
	uint64_t range = static_cast<uint64_t>(64.0 * correction);
	if (range == 0) {
		range = 1;
	}
	int threshold = static_cast<int>(gen() % range);
	return pac_popcount64(~key_hash) > threshold;
}

// ============================================================================
// Utility NULLing: probabilistically NULL low-SNR cells (post-processing)
// ============================================================================
bool PacUtilityNull(double noised_value, double noise_variance, double threshold, std::mt19937_64 &gen) {
	if (std::isnan(threshold)) {
		return false; // disabled (setting is NULL)
	}
	if (noise_variance <= 0.0 || !std::isfinite(noise_variance)) {
		return false; // no noise was added, keep the value
	}
	double noise_std = std::sqrt(noise_variance);
	double z = std::abs(noised_value) / noise_std;

	// P(keep) = sigmoid(steepness * (z - threshold))
	constexpr double steepness = 3.0;
	double exponent = -steepness * (z - threshold);
	// Clamp exponent to avoid overflow in exp()
	if (exponent > 30.0) {
		return true; // P(keep) ≈ 0
	}
	if (exponent < -30.0) {
		return false; // P(keep) ≈ 1
	}
	double p_keep = 1.0 / (1.0 + std::exp(exponent));

	// Draw uniform [0,1) and NULL if random > p_keep
	std::uniform_real_distribution<double> dist(0.0, 1.0);
	return dist(gen) >= p_keep;
}

// Finalize: compute noisy sample from the 64 counters (works on double array)
// is_null: bitmask where bit i=1 means counter i should be excluded (compacted out)
// mi: mutual information parameter for noise calculation
// correction: factor to multiply values by after compacting but before noising
// pstate: optional p-tracking state for persistent secret composition (Bayesian posterior over worlds)
// Returns: correction*yJ + noise where yJ is a randomly selected counter
PAC_FLOAT PacNoisySampleFrom64Counters(const PAC_FLOAT counters[64], double mi, double correction, std::mt19937_64 &gen,
                                       uint64_t is_null, uint64_t counter_selector,
                                       const std::shared_ptr<PacPState> &pstate, double *out_noise_variance) {
	D_ASSERT(~is_null != 0); // at least one bit must be valid

	// The vals array will always have 64 elements. If a counter is NULL, we push 0.
	vector<PAC_FLOAT> vals;
	vals.reserve(64);
	for (int i = 0; i < 64; i++) {
		vals.push_back(((is_null >> i) & 1) ? 0 : counters[i]);
	}

	// Apply correction factor to all values (after compacting NULLs, before noising)
	for (auto &v : vals) {
		v *= static_cast<PAC_FLOAT>(correction);
	}

	// mi <= 0 means no noise - return counter[0] directly (no RNG consumption)
	if (mi <= 0.0) {
		if (out_noise_variance) {
			*out_noise_variance = 0.0;
		}
		return vals[0];
	}

	// Pick counter index J in [0, 63] deterministically from counter_selector.
	int J = static_cast<int>(counter_selector % 64);
	PAC_FLOAT yJ = vals[J];

	// ---- P-tracking path: use p-weighted variance and Bayesian update ----
	if (pstate) {
		std::lock_guard<std::mutex> lock(pstate->mtx);

		double w_mean = 0.0;
		for (int k = 0; k < 64; k++) {
			w_mean += static_cast<double>(vals[k]) * pstate->p[k];
		}

		double w_var = 0.0;
		for (int k = 0; k < 64; k++) {
			double d = static_cast<double>(vals[k]) - w_mean;
			w_var += d * d * pstate->p[k];
		}

		double noise_var = w_var / (2.0 * mi);

		if (noise_var <= 0.0 || !std::isfinite(noise_var)) {
			if (out_noise_variance) {
				*out_noise_variance = 0.0;
			}
			return yJ;
		}

		std::normal_distribution<double> normal_dist(0.0, std::sqrt(noise_var));
		double noisy_result = static_cast<double>(yJ) + normal_dist(gen);

		// Bayesian posterior update: log_p[k] = log(p[k]) + log_likelihood[k], then log-sum-exp normalize
		double log_p[64];
		double max_log = -std::numeric_limits<double>::infinity();
		for (int k = 0; k < 64; k++) {
			double diff = static_cast<double>(vals[k]) - noisy_result;
			log_p[k] = std::log(pstate->p[k] + 1e-300) + (-0.5 * diff * diff / noise_var);
			if (log_p[k] > max_log) {
				max_log = log_p[k];
			}
		}
		double sum_exp = 0.0;
		for (int k = 0; k < 64; k++) {
			sum_exp += std::exp(log_p[k] - max_log);
		}
		double log_sum = max_log + std::log(sum_exp);
		for (int k = 0; k < 64; k++) {
			pstate->p[k] = std::exp(log_p[k] - log_sum);
		}

		if (out_noise_variance) {
			*out_noise_variance = noise_var;
		}
		return static_cast<PAC_FLOAT>(noisy_result);
	}

	// ---- Uniform path (no p-tracking) ----
	double delta = ComputeDeltaFromValues(vals, mi);

	if (delta <= 0.0 || !std::isfinite(delta)) {
		if (out_noise_variance) {
			*out_noise_variance = 0.0;
		}
		return yJ;
	}

	if (out_noise_variance) {
		*out_noise_variance = delta;
	}
	std::normal_distribution<double> normal_dist(0.0, std::sqrt(delta));
	return static_cast<PAC_FLOAT>(static_cast<double>(yJ) + normal_dist(gen));
}

// Compute second-moment variance (not unbiased estimator)
// Internal math stays double for numerical stability
static double ComputeSecondMomentVariance(const vector<PAC_FLOAT> &values) {
	idx_t n = values.size();
	if (n <= 1) {
		return 0.0;
	}

	double mean = 0.0;
	for (auto v : values) {
		mean += static_cast<double>(v);
	}
	mean /= static_cast<double>(n);

	double var = 0.0;
	for (auto v : values) {
		double d = static_cast<double>(v) - mean;
		var += d * d;
	}
	// Use sample variance (divide by n-1) to make the estimator unbiased for finite samples
	return var / static_cast<double>(n - 1);
}

// Exported helper: compute the PAC noise variance (delta) from values and mi.
// This implements the header-declared ComputeDeltaFromValues and is reused by other files.
// Internal math stays double for numerical stability.
double ComputeDeltaFromValues(const vector<PAC_FLOAT> &values, double mi) {
	if (mi < 0.0) {
		throw InvalidInputException("ComputeDeltaFromValues: mi must be >= 0");
	}
	double sigma2 = ComputeSecondMomentVariance(values);
	double delta = sigma2 / (2.0 * mi);
	return delta;
}

// ============================================================================
// priv_hash: UBIGINT -> UBIGINT with exactly 32 bits set
// ============================================================================

// 64-bit prime with exactly 32 bits set (irregular pattern), used for hashing in hash32_32
#define PAC_HASH_PRIME 0xB2833106536E95DFULL

static uint64_t hash32_32(uint64_t num) {
	for (int round = 0; round < 16; ++round) {
		uint64_t next = PAC_HASH_PRIME * (num ^ PAC_HASH_PRIME);
		uint64_t flip = static_cast<uint64_t>(static_cast<int64_t>(32 - pac_popcount64(num)) >> 63);
		num ^= flip; // conditionally negate without branching
		int pop = pac_popcount64(num);
		if (pop >= 26) {
			for (int iter = 0; iter < 10; ++iter) {
				if (pop == 32) {
					return num;
				}
				uint64_t mask = 1ULL << ((next >> (6 * iter)) & 63);
				if ((num & mask) == 0) {
					num |= mask;
					++pop;
				}
			}
		}
		num = next; // fallback to next hash if repair fails
	}
	return 0xAAAAAAAAAAAAAAAAULL; // 1010...10 pattern: exactly 32 bits set
}

// ============================================================================
// pac_aggregate: scalar terminal for the "naive" PAC sample-and-aggregate path.
//
// Each argument list is the output of the same SQL aggregate evaluated on many
// random subsamples of the privacy unit (see benchmark/*/*_naive_pac_queries).
// pac_aggregate collapses those per-subsample answers into one PAC-noised scalar:
// it selects counter J (deterministic from query_hash), computes the empirical
// (second-moment) variance over the subsample answers, and adds Gaussian noise
// calibrated to that variance and the per-row mutual-information budget `mi`.
// The fused/vectorized counterpart is as_noised_sum/as_noised_count.
//
// Signature: pac_aggregate(LIST<vals>, LIST<cnts>, DOUBLE mi, INTEGER k) -> DOUBLE
//   vals  : per-subsample aggregate answers (up to 64 worlds, padded to 64)
//   cnts  : per-subsample row counts (used only for the min-support `k` gate)
//   mi    : mutual-information budget in nats (<=0 disables noise)
//   k     : minimum per-subsample support; cells below it are released as NULL
// ============================================================================
struct PacAggregateLocalState : public FunctionLocalState {
	explicit PacAggregateLocalState(uint64_t seed) : gen(seed) {
	}
	std::mt19937_64 gen;
};

static unique_ptr<FunctionLocalState> PacAggregateInit(ExpressionState &, const BoundFunctionExpression &,
                                                       FunctionData *bind_data) {
	// Single source-of-truth: prefer seed supplied via bind_data (PrivBindData).
	uint64_t seed = std::random_device {}();
	if (bind_data) {
		seed = bind_data->Cast<PrivBindData>().seed;
	}
	return make_uniq<PacAggregateLocalState>(seed);
}

// Templated over values element type T and counts element type C so we can accept
// e.g. DOUBLE[] values with BIGINT[] counts without casts in the query.
template <class T, class C>
static void PacAggregateScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &vals = args.data[0];
	auto &cnts = args.data[1];
	auto &mi_vec = args.data[2];
	auto &k_vec = args.data[3];

	idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto res = FlatVector::GetData<double>(result);
	FlatVector::Validity(result).SetAllValid(count);

	auto &local = ExecuteFunctionState::GetFunctionState(state)->Cast<PacAggregateLocalState>();
	auto &gen = local.gen;

	// query_hash and pstate from bind data (deterministic counter pick + p-tracking).
	uint64_t query_hash = 0;
	std::shared_ptr<PacPState> pstate;
	auto &bound_expr = state.expr.Cast<BoundFunctionExpression>();
	if (bound_expr.bind_info) {
		auto &bd = bound_expr.bind_info->Cast<PrivBindData>();
		query_hash = bd.query_hash;
		pstate = bd.pstate;
	}

	UnifiedVectorFormat vvals, vcnts;
	vals.ToUnifiedFormat(count, vvals);
	cnts.ToUnifiedFormat(count, vcnts);

	UnifiedVectorFormat kvals;
	k_vec.ToUnifiedFormat(count, kvals);
	auto *kdata = UnifiedVectorFormat::GetData<int32_t>(kvals);

	for (idx_t row = 0; row < count; row++) {
		bool refuse = false;

		double mi = mi_vec.GetValue(row).GetValue<double>();
		if (mi < 0.0) {
			throw InvalidInputException("pac_aggregate: mi must be >= 0");
		}
		idx_t kidx = kvals.sel ? kvals.sel->get_index(row) : row;
		int k = kdata[kidx];

		idx_t r = vvals.sel ? vvals.sel->get_index(row) : row;
		if (!vvals.validity.RowIsValid(r) || !vcnts.validity.RowIsValid(r)) {
			result.SetValue(row, Value());
			continue;
		}

		auto *vals_entries = UnifiedVectorFormat::GetData<list_entry_t>(vvals);
		auto *cnts_entries = UnifiedVectorFormat::GetData<list_entry_t>(vcnts);
		auto ve = vals_entries[r];
		auto ce = cnts_entries[r];
		if (ve.length != ce.length) {
			throw InvalidInputException("pac_aggregate: values and counts length mismatch");
		}
		idx_t vals_len = ve.length;
		idx_t cnts_len = ce.length;

		auto &vals_child = ListVector::GetEntry(vals);
		auto &cnts_child = ListVector::GetEntry(cnts);
		vals_child.Flatten(ve.offset + ve.length);
		cnts_child.Flatten(ce.offset + ce.length);
		auto *vdata = FlatVector::GetData<T>(vals_child);
		auto *cdata = FlatVector::GetData<C>(cnts_child);

		vector<PAC_FLOAT> values;
		values.reserve(vals_len);
		int64_t max_count = 0;
		for (idx_t i = 0; i < vals_len; i++) {
			if (!FlatVector::Validity(vals_child).RowIsValid(ve.offset + i)) {
				refuse = true;
				break;
			}
			values.push_back(ToDouble<T>(vdata[ve.offset + i]));
			int64_t cnt_val = 0;
			if (i < cnts_len) {
				cnt_val = static_cast<int64_t>(cdata[ce.offset + i]);
			}
			max_count = std::max<int64_t>(max_count, cnt_val);
		}

		// Need exactly 64 worlds for variance/p-tracking; pad up to 64 with 0.
		while (values.size() < 64) {
			values.push_back(0.0);
		}

		if (refuse || values.empty() || max_count < static_cast<int64_t>(k)) {
			result.SetValue(row, Value());
			continue;
		}

		// mi <= 0 means no noise - return counter[0] directly.
		if (mi <= 0.0) {
			res[row] = values[0];
			continue;
		}

		idx_t N = values.size();
		idx_t J = static_cast<idx_t>(query_hash % static_cast<uint64_t>(N));
		PAC_FLOAT yJ = values[J];

		// P-tracking path (only when the list fits in 64 worlds).
		if (pstate && N == 64) {
			std::lock_guard<std::mutex> lock(pstate->mtx);
			double w_mean = 0.0;
			for (idx_t kk = 0; kk < 64; kk++) {
				w_mean += static_cast<double>(values[kk]) * pstate->p[kk];
			}
			double w_var = 0.0;
			for (idx_t kk = 0; kk < 64; kk++) {
				double d = static_cast<double>(values[kk]) - w_mean;
				w_var += d * d * pstate->p[kk];
			}
			double noise_var = w_var / (2.0 * mi);
			if (noise_var > 0.0 && std::isfinite(noise_var)) {
				double noise = std::normal_distribution<double>(0.0, std::sqrt(noise_var))(gen);
				double noisy_result = static_cast<double>(yJ) + noise;
				double log_p[64];
				double max_log = -std::numeric_limits<double>::infinity();
				for (idx_t kk = 0; kk < 64; kk++) {
					double diff = static_cast<double>(values[kk]) - noisy_result;
					log_p[kk] = std::log(pstate->p[kk] + 1e-300) + (-0.5 * diff * diff / noise_var);
					if (log_p[kk] > max_log) {
						max_log = log_p[kk];
					}
				}
				double sum_exp = 0.0;
				for (idx_t kk = 0; kk < 64; kk++) {
					sum_exp += std::exp(log_p[kk] - max_log);
				}
				double log_sum = max_log + std::log(sum_exp);
				for (idx_t kk = 0; kk < 64; kk++) {
					pstate->p[kk] = std::exp(log_p[kk] - log_sum);
				}
				res[row] = noisy_result;
				continue;
			}
			// Fall through to uniform path if no variance.
		}

		double delta = ComputeDeltaFromValues(values, mi);
		if (delta <= 0.0 || !std::isfinite(delta)) {
			res[row] = yJ;
			continue;
		}
		res[row] = static_cast<double>(yJ) + std::normal_distribution<double>(0.0, std::sqrt(delta))(gen);
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

static unique_ptr<FunctionData> PacAggregateBind(ClientContext &ctx, ScalarFunction &,
                                                 vector<unique_ptr<Expression>> &) {
	// mi is supplied per-row; the bind-time value is unused for noise but seeds the RNG.
	return make_uniq<PrivBindData>(ctx, 128.0);
}

void RegisterPacAggregateFunctions(ExtensionLoader &loader) {
	auto make_and_register = [&](const LogicalType &vals_elem, const LogicalType &cnts_elem,
	                             const scalar_function_t &fn) {
		auto fun = ScalarFunction(
		    "pac_aggregate",
		    {LogicalType::LIST(vals_elem), LogicalType::LIST(cnts_elem), LogicalType::DOUBLE, LogicalType::INTEGER},
		    LogicalType::DOUBLE, fn, PacAggregateBind, nullptr, nullptr, PacAggregateInit);
		fun.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
		loader.RegisterFunction(fun);
	};

#define REG_COUNTS_FOR(V_LOG, V_CPP)                                                                                   \
	make_and_register(V_LOG, LogicalType::TINYINT, PacAggregateScalar<V_CPP, int8_t>);                                 \
	make_and_register(V_LOG, LogicalType::SMALLINT, PacAggregateScalar<V_CPP, int16_t>);                               \
	make_and_register(V_LOG, LogicalType::INTEGER, PacAggregateScalar<V_CPP, int32_t>);                                \
	make_and_register(V_LOG, LogicalType::BIGINT, PacAggregateScalar<V_CPP, int64_t>);                                 \
	make_and_register(V_LOG, LogicalType::HUGEINT, PacAggregateScalar<V_CPP, hugeint_t>);                              \
	make_and_register(V_LOG, LogicalType::UTINYINT, PacAggregateScalar<V_CPP, uint8_t>);                               \
	make_and_register(V_LOG, LogicalType::USMALLINT, PacAggregateScalar<V_CPP, uint16_t>);                             \
	make_and_register(V_LOG, LogicalType::UINTEGER, PacAggregateScalar<V_CPP, uint32_t>);                              \
	make_and_register(V_LOG, LogicalType::UBIGINT, PacAggregateScalar<V_CPP, uint64_t>);                               \
	make_and_register(V_LOG, LogicalType::UHUGEINT, PacAggregateScalar<V_CPP, uhugeint_t>);                            \
	make_and_register(V_LOG, LogicalType::FLOAT, PacAggregateScalar<V_CPP, float>);                                    \
	make_and_register(V_LOG, LogicalType::DOUBLE, PacAggregateScalar<V_CPP, double>);

	REG_COUNTS_FOR(LogicalType::BOOLEAN, int8_t)
	REG_COUNTS_FOR(LogicalType::TINYINT, int8_t)
	REG_COUNTS_FOR(LogicalType::SMALLINT, int16_t)
	REG_COUNTS_FOR(LogicalType::INTEGER, int32_t)
	REG_COUNTS_FOR(LogicalType::BIGINT, int64_t)
	REG_COUNTS_FOR(LogicalType::HUGEINT, hugeint_t)
	REG_COUNTS_FOR(LogicalType::UTINYINT, uint8_t)
	REG_COUNTS_FOR(LogicalType::USMALLINT, uint16_t)
	REG_COUNTS_FOR(LogicalType::UINTEGER, uint32_t)
	REG_COUNTS_FOR(LogicalType::UBIGINT, uint64_t)
	REG_COUNTS_FOR(LogicalType::UHUGEINT, uhugeint_t)
	REG_COUNTS_FOR(LogicalType::FLOAT, float)
	REG_COUNTS_FOR(LogicalType::DOUBLE, double)

#undef REG_COUNTS_FOR
}

static unique_ptr<FunctionData> PacHashBind(ClientContext &ctx, ScalarFunction &, vector<unique_ptr<Expression>> &) {
	double mi = GetPacMiFromSetting(ctx);
	bool hash_repair = true;
	Value hr_val;
	if (ctx.TryGetCurrentSetting("pac_hash_repair", hr_val) && !hr_val.IsNull()) {
		hash_repair = hr_val.GetValue<bool>();
	}
	return make_uniq<PrivBindData>(ctx, mi, 1.0, 1.0, hash_repair);
}

static void PacHashFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input = args.data[0];
	auto count = args.size();
	auto &function = state.expr.Cast<BoundFunctionExpression>();
	uint64_t query_hash = 0;
	bool hash_repair = true;
	if (function.bind_info) {
		auto &bind_data = function.bind_info->Cast<PrivBindData>();
		query_hash = bind_data.query_hash;
		hash_repair = bind_data.hash_repair;
	}
	UnaryExecutor::Execute<uint64_t, uint64_t>(input, result, count, [query_hash, hash_repair](uint64_t val) {
		uint64_t xored = val ^ query_hash;
		return hash_repair ? hash32_32(xored) : xored;
	});
}

void RegisterPacHashFunction(ExtensionLoader &loader) {
	ScalarFunction priv_hash("priv_hash", {LogicalType::UBIGINT}, LogicalType::UBIGINT, PacHashFunction, PacHashBind);
	CreateScalarFunctionInfo info(priv_hash);
	FunctionDescription desc;
	desc.description = "[INTERNAL] Hashes a privacy unit key for PAC subsample assignment.";
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace duckdb
