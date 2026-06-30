#include <locale>
#include "aggregates/as_count.hpp"
#include "categorical/pac_categorical.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"

namespace duckdb {

static unique_ptr<FunctionData> PacCountBind(ClientContext &ctx, AggregateFunction &func,
                                             vector<unique_ptr<Expression>> &args) {
	// Correction is always the last arg when it's a foldable numeric constant
	idx_t corr_idx = args.size(); // sentinel: no correction
	if (args.size() >= 2) {
		auto &last = args.back();
		if (last->IsFoldable() && (last->return_type.IsNumeric() || last->return_type.id() == LogicalTypeId::UNKNOWN)) {
			corr_idx = args.size() - 1;
		}
	}
	return MakePrivBindData(ctx, args, corr_idx, "as_noised_count");
}

// State types: simple (non-scatter) always uses PacCountState directly
// Scatter uses PacCountStateWrapper for buffering (unless NOBUFFERING or NOCASCADING)
#if defined(PAC_NOBUFFERING) || defined(PAC_NOCASCADING)
using ScatterState = PacCountState;
#else
using ScatterState = PacCountStateWrapper;
#endif

// State functions - uses ScatterState for both simple and scatter
static idx_t PacCountStateSize(const AggregateFunction &) {
	return sizeof(ScatterState);
}

static void PacCountInitialize(const AggregateFunction &, data_ptr_t state_ptr) {
	memset(state_ptr, 0, sizeof(ScatterState));
}

void PacCountUpdate(Vector inputs[], AggregateInputData &aggr, idx_t, data_ptr_t state_ptr, idx_t count) {
	ScatterState &agg = *reinterpret_cast<ScatterState *>(state_ptr);
	int sample_lanes = GetPrivSampleLanes(aggr);
	UnifiedVectorFormat idata;
	inputs[0].ToUnifiedFormat(count, idata);
	auto input_data = UnifiedVectorFormat::GetData<uint64_t>(idata);
	for (idx_t i = 0; i < count; i++) {
		auto idx = idata.sel->get_index(i);
		if (idata.validity.RowIsValid(idx)) {
			auto key_hash = TransformPacUpdateHash(input_data[idx], sample_lanes);
#if defined(PAC_NOBUFFERING) || defined(PAC_NOCASCADING)
			PacCountUpdateOne(agg, key_hash, aggr.allocator);
#else
			PacCountBufferOrUpdateOne(agg, key_hash, aggr.allocator);
#endif
		}
	}
}

void PacCountColumnUpdate(Vector inputs[], AggregateInputData &aggr, idx_t, data_ptr_t state_ptr, idx_t count) {
	ScatterState &agg = *reinterpret_cast<ScatterState *>(state_ptr);
	int sample_lanes = GetPrivSampleLanes(aggr);
	UnifiedVectorFormat hash_data, col_data;
	inputs[0].ToUnifiedFormat(count, hash_data);
	inputs[1].ToUnifiedFormat(count, col_data);
	auto hashes = UnifiedVectorFormat::GetData<uint64_t>(hash_data);
	for (idx_t i = 0; i < count; i++) {
		auto h_idx = hash_data.sel->get_index(i);
		auto c_idx = col_data.sel->get_index(i);
		if (hash_data.validity.RowIsValid(h_idx) && col_data.validity.RowIsValid(c_idx)) {
			auto key_hash = TransformPacUpdateHash(hashes[h_idx], sample_lanes);
#if defined(PAC_NOBUFFERING) || defined(PAC_NOCASCADING)
			PacCountUpdateOne(agg, key_hash, aggr.allocator);
#else
			PacCountBufferOrUpdateOne(agg, key_hash, aggr.allocator);
#endif
		}
	}
}

void PacCountScatterUpdate(Vector inputs[], AggregateInputData &aggr, idx_t, Vector &states, idx_t count) {
	int sample_lanes = GetPrivSampleLanes(aggr);
	UnifiedVectorFormat idata, sdata;
	inputs[0].ToUnifiedFormat(count, idata);
	states.ToUnifiedFormat(count, sdata);
	auto input_data = UnifiedVectorFormat::GetData<uint64_t>(idata);
	auto state_p = UnifiedVectorFormat::GetData<ScatterState *>(sdata);
	for (idx_t i = 0; i < count; i++) {
		auto idx = idata.sel->get_index(i);
		if (idata.validity.RowIsValid(idx)) { // to protect against very many groups, thus uses buffering
			auto key_hash = TransformPacUpdateHash(input_data[idx], sample_lanes);
#if defined(PAC_NOBUFFERING) || defined(PAC_NOCASCADING)
			PacCountUpdateOne(*state_p[sdata.sel->get_index(i)], key_hash, aggr.allocator);
#else
			PacCountBufferOrUpdateOne(*state_p[sdata.sel->get_index(i)], key_hash, aggr.allocator);
#endif
		}
	}
}

void PacCountColumnScatterUpdate(Vector inputs[], AggregateInputData &aggr, idx_t, Vector &states, idx_t count) {
	int sample_lanes = GetPrivSampleLanes(aggr);
	UnifiedVectorFormat hash_data, col_data, sdata;
	inputs[0].ToUnifiedFormat(count, hash_data);
	inputs[1].ToUnifiedFormat(count, col_data);
	states.ToUnifiedFormat(count, sdata);
	auto hashes = UnifiedVectorFormat::GetData<uint64_t>(hash_data);
	auto state_p = UnifiedVectorFormat::GetData<ScatterState *>(sdata);
	for (idx_t i = 0; i < count; i++) {
		auto h_idx = hash_data.sel->get_index(i);
		auto c_idx = col_data.sel->get_index(i);
		if (hash_data.validity.RowIsValid(h_idx) && col_data.validity.RowIsValid(c_idx)) {
			// to protect against very many groups, thus uses buffering
			auto key_hash = TransformPacUpdateHash(hashes[h_idx], sample_lanes);
#if defined(PAC_NOBUFFERING) || defined(PAC_NOCASCADING)
			PacCountUpdateOne(*state_p[sdata.sel->get_index(i)], key_hash, aggr.allocator);
#else
			PacCountBufferOrUpdateOne(*state_p[sdata.sel->get_index(i)], key_hash, aggr.allocator);
#endif
		}
	}
}

static void DpSampleCountMaskUpdate(Vector inputs[], AggregateInputData &aggr, idx_t, data_ptr_t state_ptr,
                                    idx_t count) {
	ScatterState &agg = *reinterpret_cast<ScatterState *>(state_ptr);
	UnifiedVectorFormat idata;
	inputs[0].ToUnifiedFormat(count, idata);
	auto input_data = UnifiedVectorFormat::GetData<uint64_t>(idata);
	for (idx_t i = 0; i < count; i++) {
		auto idx = idata.sel->get_index(i);
		if (idata.validity.RowIsValid(idx)) {
#if defined(PAC_NOBUFFERING) || defined(PAC_NOCASCADING)
			PacCountUpdateOne(agg, input_data[idx], aggr.allocator);
#else
			PacCountBufferOrUpdateOne(agg, input_data[idx], aggr.allocator);
#endif
		}
	}
}

static void DpSampleCountMaskScatterUpdate(Vector inputs[], AggregateInputData &aggr, idx_t, Vector &states,
                                           idx_t count) {
	UnifiedVectorFormat idata, sdata;
	inputs[0].ToUnifiedFormat(count, idata);
	states.ToUnifiedFormat(count, sdata);
	auto input_data = UnifiedVectorFormat::GetData<uint64_t>(idata);
	auto state_p = UnifiedVectorFormat::GetData<ScatterState *>(sdata);
	for (idx_t i = 0; i < count; i++) {
		auto idx = idata.sel->get_index(i);
		if (idata.validity.RowIsValid(idx)) {
#if defined(PAC_NOBUFFERING) || defined(PAC_NOCASCADING)
			PacCountUpdateOne(*state_p[sdata.sel->get_index(i)], input_data[idx], aggr.allocator);
#else
			PacCountBufferOrUpdateOne(*state_p[sdata.sel->get_index(i)], input_data[idx], aggr.allocator);
#endif
		}
	}
}

static void CombineScatterState(ScatterState &src, ScatterState &dst, AggregateInputData &aggr) {
#if !defined(PAC_NOBUFFERING) && !defined(PAC_NOCASCADING)
	src.FlushBuffer(dst, aggr.allocator);
#endif
	PacCountState *ss = src.GetState();
	if (!ss) {
		return;
	}
	PacCountState &ds = *dst.EnsureState(aggr.allocator);
	ds.key_hash |= ss->key_hash;
#ifndef PAC_NOCASCADING
	bool src_has_totals = (ss->probabilistic_total != nullptr) || (ss->swar_fill > 0);
#else
	bool src_has_totals = (ss->probabilistic_total != nullptr);
#endif
	if (src_has_totals) {
		ds.FlushLevel(aggr.allocator);
		uint64_t *dst_totals = ds.EnsureTotals(aggr.allocator);
		ss->FlushSWARInto(dst_totals);
		if (ss->probabilistic_total) {
			for (int j = 0; j < 64; j++) {
				dst_totals[j] += ss->probabilistic_total[j];
			}
		}
	}
	ds.update_count += ss->update_count;
}

// Combine - flush src's buffer into dst (don't allocate src), then merge states
void PacCountCombine(Vector &src, Vector &dst, AggregateInputData &aggr, idx_t count) {
	auto sa = FlatVector::GetData<ScatterState *>(src);
	auto da = FlatVector::GetData<ScatterState *>(dst);
	for (idx_t i = 0; i < count; i++) {
#if !defined(PAC_NOBUFFERING) && !defined(PAC_NOCASCADING)
		// flush buffered values into dst (not into src -- it would trigger allocations)
		sa[i]->FlushBuffer(*da[i], aggr.allocator);
#endif
		PacCountState *ss = sa[i]->GetState();
		if (ss) { // we have an allocated state: flush it into dst
			PacCountState &ds = *da[i]->EnsureState(aggr.allocator);
			ds.key_hash |= ss->key_hash;
#ifndef PAC_NOCASCADING
			bool src_has_totals = (ss->probabilistic_total != nullptr) || (ss->swar_fill > 0);
#else
			bool src_has_totals = (ss->probabilistic_total != nullptr);
#endif
			if (src_has_totals) {
				ds.FlushLevel(aggr.allocator); // flush dst SWAR first
				uint64_t *dst_totals = ds.EnsureTotals(aggr.allocator);
				ss->FlushSWARInto(dst_totals); // src SWAR → dst totals (no src alloc!)
				if (ss->probabilistic_total) {
					for (int j = 0; j < 64; j++) {
						dst_totals[j] += ss->probabilistic_total[j];
					}
				}
			}
			ds.update_count += ss->update_count; // safe: FlushSWARInto moved swar_fill into update_count
		}
	}
}

void PacCountFinalize(Vector &states, AggregateInputData &input, Vector &result, idx_t count, idx_t offset) {
	auto aggs = FlatVector::GetData<ScatterState *>(states);
	auto data = FlatVector::GetData<int64_t>(result);
	auto &result_mask = FlatVector::Validity(result);
	auto &bind = input.bind_data->Cast<PrivBindData>();
	std::mt19937_64 gen(bind.seed);
	double mi = bind.mi;
	double correction = bind.correction;
	uint64_t query_hash = bind.query_hash;
	auto pstate = bind.pstate;
	PAC_FLOAT buf[64];
	for (idx_t i = 0; i < count; i++) {
#if !defined(PAC_NOBUFFERING) && !defined(PAC_NOCASCADING)
		aggs[i]->FlushBuffer(*aggs[i], input.allocator); // flush values into yourself
#endif
		PacCountState *s = aggs[i]->GetState();
		uint64_t key_hash = s ? s->key_hash : 0;
		// Check if we should return NULL based on key_hash (uses mi and correction)
		if (PacNoiseInNull(key_hash, mi, correction, gen)) {
			result_mask.SetInvalid(offset + i);
			continue;
		}
		if (s) {
			s->GetTotalsWithSWAR(buf);
		} else {
			memset(buf, 0, sizeof(buf));
		}
		CheckPacSampleDiversity(key_hash, buf, s ? s->GetUpdateCount() : 0, "as_noised_count",
		                        input.bind_data->Cast<PrivBindData>());
		// Multiply by 2 to compensate for 50% sampling, then apply correction
		double noise_var = 0.0;
		double result_val = static_cast<double>(PacNoisySampleFrom64Counters(buf, mi, correction, gen, ~key_hash,
		                                                                     query_hash, pstate, &noise_var)) *
		                    2.0;
		// Noise variance scales by 4x (2x on value means 4x on variance)
		double utility_threshold = input.bind_data->Cast<PrivBindData>().utility_threshold;
		if (PacUtilityNull(result_val, noise_var * 4.0, utility_threshold, gen)) {
			result_mask.SetInvalid(offset + i);
			continue;
		}
		data[offset + i] = static_cast<int64_t>(result_val);
	}
}

// ============================================================================
// PAC_COUNT_COUNTERS: Returns all 64 counters as LIST<DOUBLE> for categorical queries
// ============================================================================
// This variant is used when the count result will be used in a comparison
// in an outer categorical query. Instead of picking one counter and adding noise,
// it returns all 64 counters so the outer query can evaluate the comparison
// against all subsamples and produce a mask.

// Returns LIST<DOUBLE> with exactly 64 elements (no NULLs).
// Position j is 0 if key_hash bit j is 0, otherwise value * 2 * correction.
void PacCountFinalizeCounters(Vector &states, AggregateInputData &input, Vector &result, idx_t count, idx_t offset) {
	auto aggs = FlatVector::GetData<ScatterState *>(states);
	double correction = input.bind_data ? input.bind_data->Cast<PrivBindData>().correction : 1.0;

	// Result is LIST<DOUBLE>
	auto list_entries = FlatVector::GetData<list_entry_t>(result);
	auto &child_vec = ListVector::GetEntry(result);

	// Reserve space for all lists (64 elements each)
	idx_t total_elements = (offset + count) * 64;
	ListVector::Reserve(result, total_elements);
	ListVector::SetListSize(result, total_elements);

	auto child_data = FlatVector::GetData<PAC_FLOAT>(child_vec);
	PAC_FLOAT buf[64];

	for (idx_t i = 0; i < count; i++) {
#if !defined(PAC_NOBUFFERING) && !defined(PAC_NOCASCADING)
		aggs[i]->FlushBuffer(*aggs[i], input.allocator); // flush values into yourself
#endif
		PacCountState *s = aggs[i]->GetState();

		// Set up the list entry - always needed even for NULL results
		idx_t base = (offset + i) * 64;
		list_entries[offset + i].offset = base;
		list_entries[offset + i].length = 64;

		if (!s) {
			// No values seen: output 64 zeros
			memset(child_data + base, 0, 64 * sizeof(PAC_FLOAT));
			continue;
		}

		uint64_t key_hash = s->key_hash;
		s->GetTotalsWithSWAR(buf);
		CheckPacSampleDiversity(key_hash, buf, s->GetUpdateCount(), "as_noised_count",
		                        input.bind_data->Cast<PrivBindData>());

		// Copy counters to list: 0 where key_hash bit is 0, value * 2 * correction otherwise
		// The 2x factor compensates for 50% sampling, correction is user-specified multiplier
		for (int j = 0; j < 64; j++) {
			if ((key_hash >> j) & 1ULL) {
				child_data[base + j] =
				    static_cast<PAC_FLOAT>(buf[j] * 2.0 * correction); // 2x for 50% sampling, then correction
			} else {
				child_data[base + j] = 0.0; // 0 for positions not sampled
			}
		}
	}
}

void RegisterPacCountFunctions(ExtensionLoader &loader) {
	AggregateFunctionSet fcn_set("as_noised_count");

	fcn_set.AddFunction(AggregateFunction("as_noised_count", {LogicalType::UBIGINT}, LogicalType::BIGINT,
	                                      PacCountStateSize, PacCountInitialize, PacCountScatterUpdate, PacCountCombine,
	                                      PacCountFinalize, FunctionNullHandling::SPECIAL_HANDLING, PacCountUpdate,
	                                      PacCountBind));

	fcn_set.AddFunction(AggregateFunction("as_noised_count", {LogicalType::UBIGINT, LogicalType::DOUBLE},
	                                      LogicalType::BIGINT, PacCountStateSize, PacCountInitialize,
	                                      PacCountScatterUpdate, PacCountCombine, PacCountFinalize,
	                                      FunctionNullHandling::SPECIAL_HANDLING, PacCountUpdate, PacCountBind));

	fcn_set.AddFunction(AggregateFunction("as_noised_count", {LogicalType::UBIGINT, LogicalType::ANY},
	                                      LogicalType::BIGINT, PacCountStateSize, PacCountInitialize,
	                                      PacCountColumnScatterUpdate, PacCountCombine, PacCountFinalize,
	                                      FunctionNullHandling::SPECIAL_HANDLING, PacCountColumnUpdate, PacCountBind));

	fcn_set.AddFunction(AggregateFunction(
	    "as_noised_count", {LogicalType::UBIGINT, LogicalType::ANY, LogicalType::DOUBLE}, LogicalType::BIGINT,
	    PacCountStateSize, PacCountInitialize, PacCountColumnScatterUpdate, PacCountCombine, PacCountFinalize,
	    FunctionNullHandling::SPECIAL_HANDLING, PacCountColumnUpdate, PacCountBind));

	CreateAggregateFunctionInfo info(fcn_set);
	FunctionDescription desc;
	desc.description = "Privacy-preserving COUNT. Automatically injected by PAC for protected columns.";
	desc.examples = {
	    "SELECT c_mktsegment, as_noised_count(priv_hash(hash(c_custkey))) FROM customer GROUP BY c_mktsegment"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

// ============================================================================
// SASS counter Finalize: fills all 64 positions with rescaled counts so that
// downstream smooth-median / median-filter logic sees a complete distribution.
// PAC's `PacCountFinalizeCounters` zeroes positions where (key_hash >> j) & 1 == 0;
// SASS samples each lane independently via DpSampleHash, so all 64 are meaningful.
// ============================================================================
static void DpSampleCountFinalizeCounters(Vector &states, AggregateInputData &input, Vector &result, idx_t count,
                                          idx_t offset) {
	auto aggs = FlatVector::GetData<ScatterState *>(states);
	int sample_lanes = input.bind_data ? input.bind_data->Cast<PrivBindData>().sample_lanes : 1;
	double rescale = DpSampleRescale(sample_lanes);

	auto list_entries = FlatVector::GetData<list_entry_t>(result);
	auto &child_vec = ListVector::GetEntry(result);

	idx_t total_elements = (offset + count) * 64;
	ListVector::Reserve(result, total_elements);
	ListVector::SetListSize(result, total_elements);

	auto child_data = FlatVector::GetData<PAC_FLOAT>(child_vec);
	PAC_FLOAT buf[64];

	for (idx_t i = 0; i < count; i++) {
#if !defined(PAC_NOBUFFERING) && !defined(PAC_NOCASCADING)
		aggs[i]->FlushBuffer(*aggs[i], input.allocator);
#endif
		PacCountState *s = aggs[i]->GetState();

		idx_t base = (offset + i) * 64;
		list_entries[offset + i].offset = base;
		list_entries[offset + i].length = 64;

		if (!s) {
			memset(child_data + base, 0, 64 * sizeof(PAC_FLOAT));
			continue;
		}

		s->GetTotalsWithSWAR(buf);
		for (int j = 0; j < 64; j++) {
			child_data[base + j] = static_cast<PAC_FLOAT>(buf[j] * rescale);
		}
	}
}

static void DpSampleCountMaskFinalizeCounters(Vector &states, AggregateInputData &input, Vector &result, idx_t count,
                                              idx_t offset) {
	auto aggs = FlatVector::GetData<ScatterState *>(states);
	auto list_entries = FlatVector::GetData<list_entry_t>(result);
	auto &child_vec = ListVector::GetEntry(result);

	idx_t total_elements = (offset + count) * 64;
	ListVector::Reserve(result, total_elements);
	ListVector::SetListSize(result, total_elements);

	auto child_data = FlatVector::GetData<PAC_FLOAT>(child_vec);
	PAC_FLOAT buf[64];

	for (idx_t i = 0; i < count; i++) {
#if !defined(PAC_NOBUFFERING) && !defined(PAC_NOCASCADING)
		aggs[i]->FlushBuffer(*aggs[i], input.allocator);
#endif
		PacCountState *s = aggs[i]->GetState();

		idx_t base = (offset + i) * 64;
		list_entries[offset + i].offset = base;
		list_entries[offset + i].length = 64;

		if (!s) {
			memset(child_data + base, 0, 64 * sizeof(PAC_FLOAT));
			continue;
		}

		s->GetTotalsWithSWAR(buf);
		for (int j = 0; j < 64; j++) {
			child_data[base + j] = buf[j];
		}
	}
}

static unique_ptr<FunctionData> DpSampleCountBind(ClientContext &ctx, AggregateFunction &,
                                                  vector<unique_ptr<Expression>> &) {
	return MakeDpSampleBindData(ctx);
}

void RegisterDpSampleCountFunctions(ExtensionLoader &loader) {
	auto list_type = LogicalType::LIST(PacFloatLogicalType());
	AggregateFunctionSet set("as_sample_count");
	set.AddFunction(AggregateFunction("as_sample_count", {LogicalType::UBIGINT}, list_type, PacCountStateSize,
	                                  PacCountInitialize, PacCountScatterUpdate, PacCountCombine,
	                                  DpSampleCountFinalizeCounters, FunctionNullHandling::DEFAULT_NULL_HANDLING,
	                                  PacCountUpdate, DpSampleCountBind));
	set.AddFunction(AggregateFunction(
	    "as_sample_count", {LogicalType::UBIGINT, LogicalType::ANY}, list_type, PacCountStateSize, PacCountInitialize,
	    PacCountColumnScatterUpdate, PacCountCombine, DpSampleCountFinalizeCounters,
	    FunctionNullHandling::DEFAULT_NULL_HANDLING, PacCountColumnUpdate, DpSampleCountBind));
	CreateAggregateFunctionInfo info(set);
	FunctionDescription desc;
	desc.description = "[INTERNAL] Returns 64 sample-median DP counters for COUNT-like values.";
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));

	AggregateFunctionSet mask_set("as_sample_count_mask");
	mask_set.AddFunction(AggregateFunction(
	    "as_sample_count_mask", {LogicalType::UBIGINT}, list_type, PacCountStateSize, PacCountInitialize,
	    DpSampleCountMaskScatterUpdate, PacCountCombine, DpSampleCountMaskFinalizeCounters,
	    FunctionNullHandling::DEFAULT_NULL_HANDLING, DpSampleCountMaskUpdate, DpSampleCountBind));
	CreateAggregateFunctionInfo mask_info(mask_set);
	FunctionDescription mask_desc;
	mask_desc.description = "[INTERNAL] Returns 64 sample-median DP counters from precomputed sample masks.";
	mask_info.descriptions.push_back(std::move(mask_desc));
	loader.RegisterFunction(std::move(mask_info));
}

// ============================================================================
// PAC_COUNT_COUNTERS: Returns all 64 counters as LIST<DOUBLE> for categorical queries
// ============================================================================
void RegisterPacCountCountersFunctions(ExtensionLoader &loader) {
	auto list_double_type = LogicalType::LIST(PacFloatLogicalType());
	AggregateFunctionSet counters_set("as_count");

	counters_set.AddFunction(AggregateFunction("as_count", {LogicalType::UBIGINT}, list_double_type, PacCountStateSize,
	                                           PacCountInitialize, PacCountScatterUpdate, PacCountCombine,
	                                           PacCountFinalizeCounters, FunctionNullHandling::DEFAULT_NULL_HANDLING,
	                                           PacCountUpdate, PacCountBind));

	counters_set.AddFunction(
	    AggregateFunction("as_count", {LogicalType::UBIGINT, LogicalType::ANY}, list_double_type, PacCountStateSize,
	                      PacCountInitialize, PacCountColumnScatterUpdate, PacCountCombine, PacCountFinalizeCounters,
	                      FunctionNullHandling::DEFAULT_NULL_HANDLING, PacCountColumnUpdate, PacCountBind));

	// Add list aggregate overload (LIST<DOUBLE> → LIST<DOUBLE>) for subquery/categorical contexts
	AddPacListAggregateOverload(counters_set, "count");

	CreateAggregateFunctionInfo counters_info(counters_set);
	FunctionDescription counters_desc;
	counters_desc.description = "[INTERNAL] Returns 64 PAC subsample counters as LIST for categorical queries.";
	counters_info.descriptions.push_back(std::move(counters_desc));
	loader.RegisterFunction(std::move(counters_info));
}

// ============================================================================
// as_noised_avg / as_avg: reuse as_noised_count / as_count implementations.
// RewritePacAvgToDiv replaces these with sum/count + division before execution.
// ============================================================================
void RegisterPacAvgFunctions(ExtensionLoader &loader) {
	// as_noised_avg(UBIGINT, value_type[, correction]) → DOUBLE — same implementation as as_noised_count(UBIGINT,
	// ANY)
	AggregateFunctionSet noised_set("as_noised_avg");
	noised_set.AddFunction(AggregateFunction(
	    "as_noised_avg", {LogicalType::UBIGINT, LogicalType::ANY}, LogicalType::DOUBLE, PacCountStateSize,
	    PacCountInitialize, PacCountColumnScatterUpdate, PacCountCombine, PacCountFinalize,
	    FunctionNullHandling::DEFAULT_NULL_HANDLING, PacCountColumnUpdate, PacCountBind));
	noised_set.AddFunction(AggregateFunction(
	    "as_noised_avg", {LogicalType::UBIGINT, LogicalType::ANY, LogicalType::DOUBLE}, LogicalType::DOUBLE,
	    PacCountStateSize, PacCountInitialize, PacCountColumnScatterUpdate, PacCountCombine, PacCountFinalize,
	    FunctionNullHandling::DEFAULT_NULL_HANDLING, PacCountColumnUpdate, PacCountBind));
	CreateAggregateFunctionInfo avg_info(noised_set);
	FunctionDescription avg_desc;
	avg_desc.description = "Privacy-preserving AVG. Automatically injected by PAC for protected columns.";
	avg_desc.examples = {"SELECT c_mktsegment, as_noised_avg(priv_hash(hash(c_custkey)), c_acctbal) FROM customer "
	                     "GROUP BY c_mktsegment"};
	avg_info.descriptions.push_back(std::move(avg_desc));
	loader.RegisterFunction(std::move(avg_info));

	// as_avg(UBIGINT, value_type[, correction]) → LIST<FLOAT> — same implementation as as_count(UBIGINT, ANY)
	auto list_float_type = LogicalType::LIST(PacFloatLogicalType());
	AggregateFunctionSet counters_set("as_avg");
	counters_set.AddFunction(
	    AggregateFunction("as_avg", {LogicalType::UBIGINT, LogicalType::ANY}, list_float_type, PacCountStateSize,
	                      PacCountInitialize, PacCountColumnScatterUpdate, PacCountCombine, PacCountFinalizeCounters,
	                      FunctionNullHandling::DEFAULT_NULL_HANDLING, PacCountColumnUpdate, PacCountBind));
	counters_set.AddFunction(AggregateFunction(
	    "as_avg", {LogicalType::UBIGINT, LogicalType::ANY, LogicalType::DOUBLE}, list_float_type, PacCountStateSize,
	    PacCountInitialize, PacCountColumnScatterUpdate, PacCountCombine, PacCountFinalizeCounters,
	    FunctionNullHandling::DEFAULT_NULL_HANDLING, PacCountColumnUpdate, PacCountBind));
	CreateAggregateFunctionInfo avg_counters_info(counters_set);
	FunctionDescription avg_counters_desc;
	avg_counters_desc.description = "[INTERNAL] Returns 64 PAC subsample counters as LIST for categorical queries.";
	avg_counters_info.descriptions.push_back(std::move(avg_counters_desc));
	loader.RegisterFunction(std::move(avg_counters_info));
}

// ============================================================================
// Clip synonyms: as_noised_clip_count = as_noised_count,
//                as_clip_count = as_count
// ============================================================================
void RegisterPacNoisedClipCountFunctions(ExtensionLoader &loader) {
	AggregateFunctionSet fcn_set("as_noised_clip_count");

	fcn_set.AddFunction(AggregateFunction("as_noised_clip_count", {LogicalType::UBIGINT}, LogicalType::BIGINT,
	                                      PacCountStateSize, PacCountInitialize, PacCountScatterUpdate, PacCountCombine,
	                                      PacCountFinalize, FunctionNullHandling::SPECIAL_HANDLING, PacCountUpdate,
	                                      PacCountBind));
	fcn_set.AddFunction(AggregateFunction("as_noised_clip_count", {LogicalType::UBIGINT, LogicalType::DOUBLE},
	                                      LogicalType::BIGINT, PacCountStateSize, PacCountInitialize,
	                                      PacCountScatterUpdate, PacCountCombine, PacCountFinalize,
	                                      FunctionNullHandling::SPECIAL_HANDLING, PacCountUpdate, PacCountBind));
	fcn_set.AddFunction(AggregateFunction("as_noised_clip_count", {LogicalType::UBIGINT, LogicalType::ANY},
	                                      LogicalType::BIGINT, PacCountStateSize, PacCountInitialize,
	                                      PacCountColumnScatterUpdate, PacCountCombine, PacCountFinalize,
	                                      FunctionNullHandling::SPECIAL_HANDLING, PacCountColumnUpdate, PacCountBind));
	fcn_set.AddFunction(AggregateFunction(
	    "as_noised_clip_count", {LogicalType::UBIGINT, LogicalType::ANY, LogicalType::DOUBLE}, LogicalType::BIGINT,
	    PacCountStateSize, PacCountInitialize, PacCountColumnScatterUpdate, PacCountCombine, PacCountFinalize,
	    FunctionNullHandling::SPECIAL_HANDLING, PacCountColumnUpdate, PacCountBind));

	CreateAggregateFunctionInfo info(fcn_set);
	loader.RegisterFunction(std::move(info));
}

void RegisterPacClipCountFunctions(ExtensionLoader &loader) {
	auto list_double_type = LogicalType::LIST(PacFloatLogicalType());
	AggregateFunctionSet fcn_set("as_clip_count");

	fcn_set.AddFunction(AggregateFunction("as_clip_count", {LogicalType::UBIGINT}, list_double_type, PacCountStateSize,
	                                      PacCountInitialize, PacCountScatterUpdate, PacCountCombine,
	                                      PacCountFinalizeCounters, FunctionNullHandling::DEFAULT_NULL_HANDLING,
	                                      PacCountUpdate, PacCountBind));
	fcn_set.AddFunction(AggregateFunction(
	    "as_clip_count", {LogicalType::UBIGINT, LogicalType::ANY}, list_double_type, PacCountStateSize,
	    PacCountInitialize, PacCountColumnScatterUpdate, PacCountCombine, PacCountFinalizeCounters,
	    FunctionNullHandling::DEFAULT_NULL_HANDLING, PacCountColumnUpdate, PacCountBind));
	AddPacListAggregateOverload(fcn_set, "clip_count");

	CreateAggregateFunctionInfo info(fcn_set);
	loader.RegisterFunction(std::move(info));
}

} // namespace duckdb
