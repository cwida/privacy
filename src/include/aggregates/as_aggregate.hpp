#ifndef PAC_AGGREGATE_HPP
#define PAC_AGGREGATE_HPP

#include "duckdb.hpp"
#include "duckdb/common/random_engine.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include <type_traits>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <cmath>

// Cross-platform restrict keyword: MSVC uses __restrict, GCC/Clang use __restrict__
#if defined(_MSC_VER)
#define PAC_RESTRICT __restrict
#else
#define PAC_RESTRICT __restrict__
#endif

// Enable AVX2 vectorization for functions that get this preappended (useful for x86, harmless for arm)
// Only use __attribute__ on x86 with GCC/Clang - MSVC doesn't support this syntax
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER)
// On x86 targets with GCC/Clang, enable the attribute to allow function-level AVX2 codegen when available.
#define AUTOVECTORIZE __attribute__((target("avx2")))
#else
// On non-x86 targets (ARM, etc.) or Windows/MSVC, the attribute is invalid — make it a no-op.
#define AUTOVECTORIZE
#endif

#ifndef PAC_FLOAT
#define PAC_FLOAT float
#endif

#define PAC_MAGIC_HASH 2983746509182734091ULL

constexpr int DP_SAMPLE_DEFAULT_LANES = 1;
constexpr int DP_SAMPLE_MAX_LANES = 8;
constexpr int DP_SASS_DEFAULT_M = 64;
constexpr int DP_SASS_MAX_M = 512;

static inline int ValidateDpSampleLanes(int64_t sample_lanes) {
	if (sample_lanes < 1 || sample_lanes > DP_SAMPLE_MAX_LANES) {
		throw duckdb::InvalidInputException("dp_sample_lanes must be between 1 and 8");
	}
	return static_cast<int>(sample_lanes);
}

static inline int GetDpSampleLanes(duckdb::ClientContext &ctx) {
	duckdb::Value sample_lanes_val;
	if (ctx.TryGetCurrentSetting("dp_sample_lanes", sample_lanes_val) && !sample_lanes_val.IsNull()) {
		return ValidateDpSampleLanes(sample_lanes_val.GetValue<int64_t>());
	}
	return DP_SAMPLE_DEFAULT_LANES;
}

static inline int ValidateDpSassM(int64_t m) {
	if (m < DP_SASS_DEFAULT_M || m > DP_SASS_MAX_M || (m & (m - 1)) != 0) {
		throw duckdb::InvalidInputException("dp_sass_m must be a power of two between 64 and 512");
	}
	return static_cast<int>(m);
}

static inline int GetDpSassM(duckdb::ClientContext &ctx) {
	duckdb::Value m_val;
	if (ctx.TryGetCurrentSetting("dp_sass_m", m_val) && !m_val.IsNull()) {
		return ValidateDpSassM(m_val.GetValue<int64_t>());
	}
	return DP_SASS_DEFAULT_M;
}

static inline uint64_t DpSampleHash(uint64_t key_hash, int sample_lanes) {
	uint64_t sample_hash = 0;
	for (int i = 0; i < sample_lanes; i++) {
		sample_hash |= 1ULL << ((key_hash >> (i * 6)) & 63);
	}
	return sample_hash;
}

static inline double DpSampleLaneProbability(int sample_lanes) {
	return 1.0 - std::pow(63.0 / 64.0, static_cast<double>(sample_lanes));
}

static inline double DpSampleRescale(int sample_lanes) {
	return 1.0 / DpSampleLaneProbability(sample_lanes);
}

static inline bool GetDpSassRescale(duckdb::ClientContext &ctx) {
	duckdb::Value rescale_val;
	if (ctx.TryGetCurrentSetting("dp_sass_rescale", rescale_val) && !rescale_val.IsNull()) {
		return rescale_val.GetValue<bool>();
	}
	return true;
}

struct PacIdentityHash {
	static inline uint64_t Transform(uint64_t key_hash) {
		return key_hash;
	}
};

// Cross-platform popcount for 64-bit integers
// MSVC doesn't have __builtin_popcountll, so we need to handle it differently
#if defined(_MSC_VER)
#include <intrin.h>
static inline int pac_popcount64(uint64_t x) {
#if defined(_M_X64) || defined(_M_AMD64)
	return static_cast<int>(__popcnt64(x));
#else
	// Fallback for 32-bit MSVC: split into two 32-bit popcounts
	return static_cast<int>(__popcnt(static_cast<uint32_t>(x)) + __popcnt(static_cast<uint32_t>(x >> 32)));
#endif
}
// Cross-platform count leading zeros for 64-bit integers
static inline int pac_clzll(uint64_t x) {
	unsigned long index;
#if defined(_M_X64) || defined(_M_AMD64)
	if (_BitScanReverse64(&index, x)) {
		return 63 - static_cast<int>(index);
	}
#else
	// Fallback for 32-bit MSVC
	if (_BitScanReverse(&index, static_cast<uint32_t>(x >> 32))) {
		return 31 - static_cast<int>(index);
	}
	if (_BitScanReverse(&index, static_cast<uint32_t>(x))) {
		return 63 - static_cast<int>(index);
	}
#endif
	return 64; // x == 0
}
#else
// GCC/Clang have __builtin_popcountll
static inline int pac_popcount64(uint64_t x) {
	return __builtin_popcountll(x);
}
// GCC/Clang have __builtin_clzll
static inline int pac_clzll(uint64_t x) {
	return x ? __builtin_clzll(x) : 64;
}
#endif

namespace duckdb {

// Returns the DuckDB LogicalType corresponding to PAC_FLOAT (FLOAT or DOUBLE).
static inline LogicalType PacFloatLogicalType() {
	return std::is_same<PAC_FLOAT, float>::value ? LogicalType::FLOAT : LogicalType::DOUBLE;
}

// Header for privacy aggregate helpers and public declarations used across pac_* files.
// Contains bindings and small helpers shared between pac_aggregate, as_count and as_sum implementations.

// ============================================================================
// PacPState: per-query Bayesian posterior tracker for persistent secret composition
// ============================================================================
// Tracks a probability distribution p over the 64 worlds (bit positions).
// Shared across all aggregates in the same query via shared_ptr, so that
// noise calibration correctly composes information leaked by all cells.
// See: get_noise_var() and update_p() from the collaborator's reference.
struct PacPState {
	std::mutex mtx;
	double p[64];

	PacPState() {
		for (int i = 0; i < 64; i++) {
			p[i] = 1.0 / 64.0;
		}
	}
};

// Global map for cross-aggregate PacPState sharing within a query.
// Keyed by query_hash. Uses weak_ptr so lifetime is tied to PrivBindData instances.
std::shared_ptr<PacPState> GetOrCreatePState(uint64_t query_hash);

// Compute the PAC noise variance (delta) from per-sample values and mutual information budget mi.
// Throws InvalidInputException if mi < 0.
double ComputeDeltaFromValues(const vector<PAC_FLOAT> &values, double mi);

// Register priv_hash scalar function (UBIGINT -> UBIGINT with exactly 32 bits set)
void RegisterPacHashFunction(ExtensionLoader &loader);

// Register pac_aggregate scalar function (LIST vals, LIST cnts, DOUBLE mi, INTEGER k -> DOUBLE):
// terminal for the naive PAC sample-and-aggregate path (collapses per-subsample answers + PAC noise)
void RegisterPacAggregateFunctions(ExtensionLoader &loader);

// Register priv_finalize scalar function (LIST<DOUBLE> -> DOUBLE, read-time noise for derived tables)
void RegisterPacFinalizeFunction(ExtensionLoader &loader);

// Declare the noisy-sample helper so other translation units (as_count.cpp) can call it.
// is_null: bitmask where bit i=1 means counter i should be excluded (compacted out)
// mi: mutual information parameter for noise calculation
// correction: factor to multiply values by after compacting NULLs but before adding noise
// pstate: optional shared p-tracking state for persistent secret composition (may be nullptr)
// out_noise_variance: if non-null, receives the noise variance used (for utility NULLing)
PAC_FLOAT PacNoisySampleFrom64Counters(const PAC_FLOAT counters[64], double mi, double correction, std::mt19937_64 &gen,
                                       uint64_t is_null = 0, uint64_t counter_selector = 0,
                                       const std::shared_ptr<PacPState> &pstate = nullptr,
                                       double *out_noise_variance = nullptr);

// PacNoisedSelect: returns true with probability proportional to popcount(key_hash)/64
// Uses rnd&63 as threshold, returns true if bitcount > threshold
static inline bool PacNoisedSelect(uint64_t key_hash, uint64_t rnd) {
	return pac_popcount64(key_hash) > static_cast<int>(rnd & 63);
}

// PacNoiseInNull: probabilistically returns true based on bit count in key_hash.
// mi: controls probabilistic (mi>0) vs deterministic (mi<=0) mode
// correction: reduces NULL probability by this factor (considers correction times more non-nulls)
// Probabilistic: P(NULL) = popcount(~key_hash) / (64 * correction)
// Deterministic: NULL when popcount(key_hash) * correction < 1
bool PacNoiseInNull(uint64_t key_hash, double mi, double correction, std::mt19937_64 &gen);

// PacUtilityNull: post-processing NULLing of low-SNR cells.
// Returns true (NULL the cell) probabilistically based on z-score = |noised_value| / noise_std_dev.
// P(NULL) = 1 - sigmoid(steepness * (z - threshold)) = 1 - 1/(1 + exp(-steepness*(z - threshold)))
// Safe by data processing inequality: this is a function of the already-noised output + independent coins.
// threshold: z-score center (e.g., 4.0 means ~20% expected relative error cutoff). NaN = disabled.
// noise_variance: the PAC noise variance used for this cell (delta = sigma^2 / (2*mi))
bool PacUtilityNull(double noised_value, double noise_variance, double threshold, std::mt19937_64 &gen);

// Minimum total rows across all groups before the diversity check applies
// With hash32_32 (hash_repair), single-PU groups always have exactly 32 zero bits
// (in the [29,35] suspicious range), so small test datasets need a higher threshold.
#define PAC_SUSPICIOUS_THRESHOLD 500

struct PrivBindData; // forward declaration

// Check for absence of sample diversity in PAC aggregates.
// Accumulates per-group exact_count into bind_data totals, classifies the group as
// suspicious or not, and throws if total_exact_count >= threshold AND suspicious > nonsuspicious.
void CheckPacSampleDiversity(uint64_t key_hash, const PAC_FLOAT *buf, uint64_t update_count, const char *aggr_name,
                             PrivBindData &bind_data);

// Helper function to generate a random seed (defined in as_aggregate.cpp)
// This avoids including <random> in the header for std::random_device
uint64_t PacGenerateRandomSeed();

// Helper to read pac_mi from session setting.
// Returns 1.0/128 if not found or null.
inline double GetPacMiFromSetting(ClientContext &ctx) {
	Value pac_mi_val;
	if (ctx.TryGetCurrentSetting("pac_mi", pac_mi_val) && !pac_mi_val.IsNull()) {
		return pac_mi_val.GetValue<double>();
	}
	return 1.0 / 128; // default
}

// Bind data used by privacy aggregates to carry `mi`, `correction`, and sampling parameters.
// Reads seed from privacy_seed setting (or uses query-id if not set) and computes query_hash.
// query_hash is XOR'd with per-row key_hash inside priv_hash() (centralized) and used as
// the counter selector for PacNoisySampleFrom64Counters in finalize functions.
struct PrivBindData : public FunctionData {
	double mi;                   // mutual information parameter from pac_mi setting (controls noise/NULL probability)
	double correction;           // correction factor: multiplies sum/avg/count results, reduces NULL prob for min/max
	uint64_t seed;               // RNG seed: privacy_seed setting value, or query-id if not set
	uint64_t query_hash;         // derived from seed: used inside priv_hash() for XOR and as counter selector
	double scale_divisor;        // for DECIMAL as_avg: divide result by 10^scale (default 1.0)
	bool hash_repair;            // if true, priv_hash() repairs hash to exactly 32 bits set
	bool sample_diversity_check; // if true, reject aggregates without sample diversity
	int sample_lanes;            // SASS mode: number of sampled lanes per PU hash; 0 means identity hash
	int sample_count;            // SASS mode: total number of sample lanes (m); 64 uses the SIMD bitmask path
	bool sample_rescale;         // SASS mode: rescale SUM/COUNT samples to full-dataset-estimator scale
	double utility_threshold;    // z-score threshold for utility NULLing (NaN = disabled, any value = enabled)

	// Persistent secret p-tracking: shared across all aggregates in the same query (same query_hash).
	// When active (mi > 0 and pac_ptracking enabled), noise calibration uses p-weighted variance
	// and updates p after each noisy release for correct privacy composition.
	std::shared_ptr<PacPState> pstate;

	// Runtime diversity tracking (accumulated across groups during finalize, not bind-time config)
	mutable uint64_t total_update_count;  // sum of update_counts across all finalized groups
	mutable uint64_t suspicious_count;    // groups with [29..35] zero bits AND all-same-value
	mutable uint64_t nonsuspicious_count; // groups that are not suspicious

	// Primary constructor - reads seed from privacy_seed setting, or uses query-id if not set.
	// All aggregates in the same query get the same seed and query_hash.
	explicit PrivBindData(ClientContext &ctx, double mi_val, double correction_val = 1.0, double scale_div = 1.0,
	                      bool hash_repair_val = false, int sample_lanes_val = 0,
	                      int sample_count_val = DP_SASS_DEFAULT_M, bool sample_rescale_val = true)
	    : mi(mi_val), correction(correction_val), scale_divisor(scale_div), hash_repair(hash_repair_val),
	      sample_diversity_check(true), sample_lanes(sample_lanes_val), sample_count(sample_count_val),
	      sample_rescale(sample_rescale_val), utility_threshold(std::numeric_limits<double>::quiet_NaN()),
	      total_update_count(0), suspicious_count(0), nonsuspicious_count(0) {
		Value sd_val;
		if (ctx.TryGetCurrentSetting("pac_sample_diversity_check", sd_val) && !sd_val.IsNull()) {
			sample_diversity_check = sd_val.GetValue<bool>();
		}
		// Read utility threshold: if set (non-null), enables probabilistic NULLing of low-SNR cells
		Value ut_val;
		if (ctx.TryGetCurrentSetting("privacy_min_group_count", ut_val) && !ut_val.IsNull()) {
			utility_threshold = ut_val.GetValue<double>();
		}
		Value pac_seed_val;
		if (ctx.TryGetCurrentSetting("privacy_seed", pac_seed_val) && !pac_seed_val.IsNull()) {
			seed = uint64_t(pac_seed_val.GetValue<int64_t>());
		} else {
			seed = 42;
		}
		if (mi != 0.0) { // randomize seed per query (not in deterministic mode aka mi==0)
			seed ^= PAC_MAGIC_HASH * static_cast<uint64_t>(ctx.ActiveTransaction().GetActiveQuery());
		}
		query_hash = (seed * PAC_MAGIC_HASH) ^ PAC_MAGIC_HASH;

		// Initialize p-tracking if mi > 0 and pac_ptracking is enabled
		if (mi > 0.0) {
			Value pt_val;
			bool ptracking_enabled = true; // default: enabled
			if (ctx.TryGetCurrentSetting("pac_ptracking", pt_val) && !pt_val.IsNull()) {
				ptracking_enabled = pt_val.GetValue<bool>();
			}
			if (ptracking_enabled) {
				pstate = GetOrCreatePState(query_hash);
			}
		}
	}

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<PrivBindData>(*this); // copies shared_ptr (shares pstate)
		copy->total_update_count = 0;               // reset runtime diversity counters
		copy->suspicious_count = 0;
		copy->nonsuspicious_count = 0;
		return copy;
	}
	bool Equals(const FunctionData &other) const override {
		auto &o = other.Cast<PrivBindData>();
		return mi == o.mi && correction == o.correction && seed == o.seed && query_hash == o.query_hash &&
		       scale_divisor == o.scale_divisor && hash_repair == o.hash_repair && sample_lanes == o.sample_lanes &&
		       sample_count == o.sample_count && sample_rescale == o.sample_rescale &&
		       utility_threshold == o.utility_threshold;
	}
};

static inline int GetPrivSampleLanes(AggregateInputData &aggr) {
	return aggr.bind_data ? aggr.bind_data->Cast<PrivBindData>().sample_lanes : 0;
}

static inline int GetPrivSampleCount(AggregateInputData &aggr) {
	return aggr.bind_data ? aggr.bind_data->Cast<PrivBindData>().sample_count : DP_SASS_DEFAULT_M;
}

static inline uint64_t TransformPacUpdateHash(uint64_t key_hash, int sample_lanes) {
	return sample_lanes > 0 ? DpSampleHash(key_hash, sample_lanes) : key_hash;
}

static inline idx_t DpSassLaneIndex(uint64_t key_hash, int sample_count) {
	D_ASSERT(sample_count > 0);
	return static_cast<idx_t>(key_hash & static_cast<uint64_t>(sample_count - 1));
}

// Shared bind for SASS sample-* aggregates: no mi/correction, just the active
// dp_sample_lanes setting. Used by as_sample_sum, as_sample_count, etc.
inline unique_ptr<FunctionData> MakeDpSampleBindData(ClientContext &ctx) {
	return make_uniq<PrivBindData>(ctx, 0.0, 1.0, 1.0, false, GetDpSampleLanes(ctx), DP_SASS_DEFAULT_M,
	                               GetDpSassRescale(ctx));
}

inline unique_ptr<FunctionData> MakeDpSampleMBindData(ClientContext &ctx) {
	return make_uniq<PrivBindData>(ctx, 0.0, 1.0, 1.0, false, GetDpSampleLanes(ctx), GetDpSassM(ctx),
	                               GetDpSassRescale(ctx));
}

// Common bind helper: reads mi from setting, extracts correction from args[correction_arg_index],
// validates it (foldable constant >= 0), and returns a PrivBindData.
// Returns correction=1.0 if correction_arg_index >= args.size().
inline unique_ptr<FunctionData> MakePrivBindData(ClientContext &ctx, vector<unique_ptr<Expression>> &args,
                                                 idx_t correction_arg_index, const char *func_name,
                                                 double scale_divisor = 1.0) {
	double mi = GetPacMiFromSetting(ctx);
	double correction = 1.0;
	if (correction_arg_index < args.size()) {
		if (!args[correction_arg_index]->IsFoldable()) {
			throw InvalidInputException("%s: correction parameter must be a constant", func_name);
		}
		auto val = ExpressionExecutor::EvaluateScalar(ctx, *args[correction_arg_index]);
		correction = val.GetValue<double>();
		if (correction < 0.0) {
			throw InvalidInputException("%s: correction must be >= 0", func_name);
		}
	}
	return make_uniq<PrivBindData>(ctx, mi, correction, scale_divisor);
}

// Helper to convert double to accumulator type (used by as_sum finalizers)
template <class T>
static inline T FromDouble(double val) {
	return static_cast<T>(val);
}

// Specializations for hugeint_t and uhugeint_t
template <>
inline hugeint_t FromDouble<hugeint_t>(double val) {
	return Hugeint::Convert(val); // Use direct double-to-hugeint conversion (handles values > INT64_MAX)
}

// Helper to convert any numeric type to double for variance calculation
template <class T>
static inline double ToDouble(const T &val) {
	return static_cast<double>(val);
}

template <>
inline double ToDouble<hugeint_t>(const hugeint_t &val) {
	return Hugeint::Cast<double>(val);
}

// Specialization for unsigned hugeint (uhugeint_t)
template <>
inline double ToDouble<uhugeint_t>(const uhugeint_t &val) {
	return Uhugeint::Cast<double>(val);
}

// Helper to convert any totals array to PAC_FLOAT[64]
template <class T>
static inline void ToDoubleArray(const T *src, PAC_FLOAT *dst) {
	for (int i = 0; i < 64; i++) {
		dst[i] = static_cast<PAC_FLOAT>(ToDouble(src[i]));
	}
}

// Helper to convert input type to value type (for unified Update methods)
template <class VALUE_TYPE>
struct ConvertValue {
	template <class INPUT_TYPE>
	static inline VALUE_TYPE convert(const INPUT_TYPE &val) {
		return static_cast<VALUE_TYPE>(val);
	}
};

template <>
struct ConvertValue<double> {
	template <class INPUT_TYPE>
	static inline double convert(const INPUT_TYPE &val) {
		return ToDouble(val);
	}
};
} // namespace duckdb

#endif // PAC_AGGREGATE_HPP
