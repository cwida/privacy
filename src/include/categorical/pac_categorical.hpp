//
// PAC Categorical Query Support
//
// This file implements the mask-based approach for handling categorical queries
// (queries that don't return aggregates but use PAC aggregates in subquery predicates).
//
// Problem: When an inner query has a PAC aggregate and the outer query uses it in a comparison
// (e.g., WHERE value > as_sum(...)), picking ONE subsample for the inner aggregate leaks privacy
// because the outer query's filter decision is based on that specific subsample.
//
// Solution: The inner PAC aggregate returns ALL 64 counter values. The comparison is evaluated
// against all 64 values, producing a 64-bit mask where bit i = 1 if subsample i satisfies the
// condition. The final selection happens at the outermost categorical boundary using priv_filter().
//
// Key Functions:
// - as_sum / as_count / as_min / as_max (hash, value) -> LIST<FLOAT> : all 64 counter values (no noise/selection yet)
// - priv_filter_<cmp>(value, counters) -> BOOLEAN : true if the majority of counters satisfy value <cmp> counter,
//   for <cmp> in gt, gte, lt, lte, eq, neq
// - priv_select_<cmp>(hash, value, counters) -> UBIGINT : mask where value <cmp> counter holds, applied to hash
// - priv_select(hash, list<bool>) -> UBIGINT : Convert booleans to mask, combined with hash subsampling
// - priv_filter(list<bool>) -> BOOLEAN : Convert to mask, then filter on popcount(mask)/64

#ifndef PAC_CATEGORICAL_HPP
#define PAC_CATEGORICAL_HPP

#include "duckdb.hpp"
#include "aggregates/as_aggregate.hpp"

namespace duckdb {

// Register all PAC categorical functions with the extension loader
void RegisterPacCategoricalFunctions(ExtensionLoader &loader);

// Add a PAC list aggregate overload (LIST<FLOAT> → LIST<FLOAT>) to an existing function set.
// Used to merge the list variant into the same AggregateFunctionSet as the counters variant.
// aggr_type: "sum", "count", "min", or "max"
void AddPacListAggregateOverload(AggregateFunctionSet &set, const string &aggr_type);

// ============================================================================
// PAC_COUNTERS aggregate: Returns all 64 counters as a LIST for categorical queries
// ============================================================================
// This is a variant of as_sum that returns the raw counters instead of picking one.
// Used when the aggregate result will be used in a comparison in an outer categorical query.

// ============================================================================
// Comparison functions: Compare scalar value against 64 PAC counters
// ============================================================================
// Each returns a UBIGINT mask where bit i = 1 if the comparison holds for counter i

// pac_gt(value, counters) -> mask where bit i = 1 if value > counters[i]
// pac_lt(value, counters) -> mask where bit i = 1 if value < counters[i]
// pac_gte(value, counters) -> mask where bit i = 1 if value >= counters[i]
// pac_lte(value, counters) -> mask where bit i = 1 if value <= counters[i]
// pac_eq(value, counters) -> mask where bit i = 1 if value == counters[i] (approx equality for floats)

// ============================================================================
// PAC_SELECT: Convert list<bool> to bitmask, combined with hash subsampling
// ============================================================================
// priv_select(UBIGINT hash, list<bool>) -> UBIGINT
// Converts booleans to a mask and combines with the privacy-unit hash.
// Output is query_hash-compatible for downstream pac aggregates.

// ============================================================================
// PAC_FILTER: Final probabilistic selection based on mask
// ============================================================================
// priv_filter(mask) -> BOOLEAN
// Returns true with probability proportional to popcount(mask)/64
// This should be called at the outermost categorical query boundary

// priv_filter(mask, mi) -> BOOLEAN
// Same as above but with explicit mi parameter (mi=0 means deterministic: majority voting)

// priv_filter(list<bool>) -> BOOLEAN
// Convenience: converts list to mask, then applies filter logic

} // namespace duckdb

#endif // PAC_CATEGORICAL_HPP
