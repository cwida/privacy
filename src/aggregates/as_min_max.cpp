#include "aggregates/as_min_max.hpp"
#include "categorical/pac_categorical.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"

#include <cmath>

namespace duckdb {

// ============================================================================
// State type selection
// ============================================================================
#ifdef PAC_NOBUFFERING
template <typename T, bool IS_MAX>
using MinMaxState = PacMinMaxState<T, IS_MAX>;
#else
template <typename T, bool IS_MAX>
using MinMaxState = PacMinMaxStateWrapper<T, IS_MAX>;
#endif

// ============================================================================
// Update functions
// ============================================================================

// Non-grouped update
template <typename T, bool IS_MAX>
static void PacMinMaxUpdate(Vector inputs[], AggregateInputData &aggr, idx_t, data_ptr_t state_p, idx_t count) {
	auto &agg = *reinterpret_cast<MinMaxState<T, IS_MAX> *>(state_p);
	int sample_lanes = GetPrivSampleLanes(aggr);

	UnifiedVectorFormat hash_data, value_data;
	inputs[0].ToUnifiedFormat(count, hash_data);
	inputs[1].ToUnifiedFormat(count, value_data);
	auto hashes = UnifiedVectorFormat::GetData<uint64_t>(hash_data);
	auto values = UnifiedVectorFormat::GetData<T>(value_data);

	for (idx_t i = 0; i < count; i++) {
		auto h_idx = hash_data.sel->get_index(i);
		auto v_idx = value_data.sel->get_index(i);
		if (hash_data.validity.RowIsValid(h_idx) && value_data.validity.RowIsValid(v_idx)) {
			// min/max do not use buffering for global aggregates (without GROUP BY) because it was  slower
			PacMinMaxUpdateOne<T, IS_MAX>(agg, TransformPacUpdateHash(hashes[h_idx], sample_lanes), values[v_idx],
			                              aggr.allocator);
		}
	}
}

// Grouped (scatter) update
template <typename T, bool IS_MAX>
static void PacMinMaxScatterUpdate(Vector inputs[], AggregateInputData &aggr, idx_t, Vector &states, idx_t count) {
	int sample_lanes = GetPrivSampleLanes(aggr);
	UnifiedVectorFormat hash_data, value_data, sdata;
	inputs[0].ToUnifiedFormat(count, hash_data);
	inputs[1].ToUnifiedFormat(count, value_data);
	states.ToUnifiedFormat(count, sdata);

	auto hashes = UnifiedVectorFormat::GetData<uint64_t>(hash_data);
	auto values = UnifiedVectorFormat::GetData<T>(value_data);
	auto state_ptrs = UnifiedVectorFormat::GetData<MinMaxState<T, IS_MAX> *>(sdata);

	for (idx_t i = 0; i < count; i++) {
		auto h_idx = hash_data.sel->get_index(i);
		auto v_idx = value_data.sel->get_index(i);
		if (hash_data.validity.RowIsValid(h_idx) && value_data.validity.RowIsValid(v_idx)) {
			auto state = state_ptrs[sdata.sel->get_index(i)];
			auto key_hash = TransformPacUpdateHash(hashes[h_idx], sample_lanes);
#ifdef PAC_NOBUFFERING
			PacMinMaxUpdateOne<T, IS_MAX>(*state, key_hash, values[v_idx], aggr.allocator);
#else
			PacMinMaxBufferOrUpdateOne<T, IS_MAX>(*state, key_hash, values[v_idx], aggr.allocator);
#endif
		}
	}
}

// ============================================================================
// Combine and Finalize
// ============================================================================

template <typename T, bool IS_MAX>
static void PacMinMaxCombine(Vector &src, Vector &dst, AggregateInputData &aggr, idx_t count) {
	auto src_states = FlatVector::GetData<MinMaxState<T, IS_MAX> *>(src);
	auto dst_states = FlatVector::GetData<MinMaxState<T, IS_MAX> *>(dst);

	for (idx_t i = 0; i < count; i++) {
#ifndef PAC_NOBUFFERING
		// flush the src buffer into dst (not into src: that could cause allocations there, we only want that in dst)
		src_states[i]->FlushBuffer(*dst_states[i], aggr.allocator);
#endif
		auto *ss = src_states[i]->GetState();
		if (ss && ss->initialized) { // propagate the min/max of src into dst
			auto &ds = *dst_states[i]->EnsureState(aggr.allocator);
			ds.CombineWith(*ss);
		}
	}
}

template <typename T, bool IS_MAX>
static void PacMinMaxFinalize(Vector &states, AggregateInputData &input, Vector &result, idx_t count, idx_t offset) {
	auto state_ptrs = FlatVector::GetData<MinMaxState<T, IS_MAX> *>(states);
	auto data = FlatVector::GetData<T>(result);
	auto &result_mask = FlatVector::Validity(result);

	double mi = input.bind_data ? input.bind_data->Cast<PrivBindData>().mi : 0.0;
	double correction = input.bind_data ? input.bind_data->Cast<PrivBindData>().correction : 1.0;
	uint64_t query_hash = input.bind_data ? input.bind_data->Cast<PrivBindData>().query_hash : 0;
	auto pstate = input.bind_data ? input.bind_data->Cast<PrivBindData>().pstate : nullptr;

	for (idx_t i = 0; i < count; i++) {
#ifndef PAC_NOBUFFERING
		// flush the buffer into yourself, possibly allocating the state (array with 64 totals)
		state_ptrs[i]->FlushBuffer(*state_ptrs[i], input.allocator);
#endif
		auto *s = state_ptrs[i]->GetState();
		// Check if we should return NULL based on key_hash
		uint64_t key_hash = s ? s->key_hash : 0;
		std::mt19937_64 gen(input.bind_data->Cast<PrivBindData>().seed);
		// mi controls noise mode, correction reduces NULL probability (but does NOT scale min/max values)
		if (PacNoiseInNull(key_hash, mi, correction, gen)) {
			result_mask.SetInvalid(offset + i);
			continue;
		}
		PAC_FLOAT buf[64];
		if (s && s->initialized) {
			s->GetTotals(buf);
		} else {
			memset(buf, 0, sizeof(buf));
		}
		CheckPacSampleDiversity(key_hash, buf, s ? s->update_count : 0, IS_MAX ? "as_noised_max" : "as_noised_min",
		                        input.bind_data->Cast<PrivBindData>());
		// Pass mi for noise, 1.0 as correction (no value scaling for min/max)
		double noise_var = 0.0;
		PAC_FLOAT result_val =
		    PacNoisySampleFrom64Counters(buf, mi, 1.0, gen, ~key_hash, query_hash, pstate, &noise_var);
		double utility_threshold = input.bind_data->Cast<PrivBindData>().utility_threshold;
		if (PacUtilityNull(static_cast<double>(result_val), noise_var, utility_threshold, gen)) {
			result_mask.SetInvalid(offset + i);
			continue;
		}
		data[offset + i] = FromDouble<T>(result_val);
	}
}

// ============================================================================
// State management
// ============================================================================

template <typename T, bool IS_MAX>
static idx_t PacMinMaxStateSize(const AggregateFunction &) {
	return sizeof(MinMaxState<T, IS_MAX>);
}

template <typename T, bool IS_MAX>
static void PacMinMaxInitialize(const AggregateFunction &, data_ptr_t p) {
	memset(p, 0, sizeof(MinMaxState<T, IS_MAX>));
}

// ============================================================================
// Bind function - selects implementation based on input type
// ============================================================================

template <bool IS_MAX>
static unique_ptr<FunctionData> PacMinMaxBind(ClientContext &ctx, AggregateFunction &function,
                                              vector<unique_ptr<Expression>> &args) {
	auto &value_type = args[1]->return_type;
	auto physical_type = value_type.InternalType();

	function.return_type = value_type;
	function.arguments[1] = value_type;

	// Select implementation based on physical type
#define BIND_TYPE(PHYS_TYPE, CPP_TYPE)                                                                                 \
	case PhysicalType::PHYS_TYPE:                                                                                      \
		function.state_size = PacMinMaxStateSize<CPP_TYPE, IS_MAX>;                                                    \
		function.initialize = PacMinMaxInitialize<CPP_TYPE, IS_MAX>;                                                   \
		function.update = PacMinMaxScatterUpdate<CPP_TYPE, IS_MAX>;                                                    \
		function.simple_update = PacMinMaxUpdate<CPP_TYPE, IS_MAX>;                                                    \
		function.combine = PacMinMaxCombine<CPP_TYPE, IS_MAX>;                                                         \
		function.finalize = PacMinMaxFinalize<CPP_TYPE, IS_MAX>;                                                       \
		break

	switch (physical_type) {
		BIND_TYPE(INT8, int8_t);
		BIND_TYPE(INT16, int16_t);
		BIND_TYPE(INT32, int32_t);
		BIND_TYPE(INT64, int64_t);
		BIND_TYPE(UINT8, uint8_t);
		BIND_TYPE(UINT16, uint16_t);
		BIND_TYPE(UINT32, uint32_t);
		BIND_TYPE(UINT64, uint64_t);
		BIND_TYPE(FLOAT, float);
		BIND_TYPE(DOUBLE, double);
		BIND_TYPE(INT128, hugeint_t);
		BIND_TYPE(UINT128, uhugeint_t);
	default:
		throw NotImplementedException("pac_%s not implemented for type %s", IS_MAX ? "max" : "min",
		                              value_type.ToString());
	}
#undef BIND_TYPE

	return MakePrivBindData(ctx, args, 2, IS_MAX ? "as_noised_max" : "as_noised_min");
}

// ============================================================================
// PAC_MIN/MAX_COUNTERS: Returns all 64 counters as LIST<DOUBLE> for categorical queries
// ============================================================================
template <typename T, bool IS_MAX>
static void PacMinMaxFinalizeCounters(Vector &states, AggregateInputData &input, Vector &result, idx_t count,
                                      idx_t offset) {
	auto state_ptrs = FlatVector::GetData<MinMaxState<T, IS_MAX> *>(states);

	// Result is LIST<DOUBLE>
	auto list_entries = FlatVector::GetData<list_entry_t>(result);
	auto &child_vec = ListVector::GetEntry(result);

	// Reserve space for all lists (64 elements each)
	idx_t total_elements = count * 64;
	ListVector::Reserve(result, total_elements);
	ListVector::SetListSize(result, total_elements);

	auto child_data = FlatVector::GetData<PAC_FLOAT>(child_vec);

	for (idx_t i = 0; i < count; i++) {
#ifndef PAC_NOBUFFERING
		// flush the buffer into yourself, possibly allocating the state (array with 64 totals)
		state_ptrs[i]->FlushBuffer(*state_ptrs[i], input.allocator);
#endif
		auto *s = state_ptrs[i]->GetState();

		// Set up the list entry - always needed even for NULL results
		list_entries[offset + i].offset = i * 64;
		list_entries[offset + i].length = 64;

		if (!s || !s->initialized) {
			// No values seen: output 64 zeros
			memset(child_data + i * 64, 0, 64 * sizeof(PAC_FLOAT));
			continue;
		}

		uint64_t key_hash = s->key_hash;
		PAC_FLOAT *dst = &child_data[i * 64];
		// Undo SWAR interleaving: extremes[swar_pos] corresponds to bit j of key_hash
		constexpr int SZ = sizeof(T) <= 8 ? static_cast<int>(sizeof(T)) : 8;
		constexpr int ELEMS = 8 / SZ;
		constexpr int SHIFTS = 64 / ELEMS;
		for (idx_t j = 0; j < 64; j++) {
			idx_t child_idx = i * 64 + j;
			int swar_pos = (static_cast<int>(j) % SHIFTS) * ELEMS + static_cast<int>(j) / SHIFTS;
			if ((key_hash >> j) & 1) {
				// Counter was updated - return the value (no scaling for min/max)
				dst[j] = ToDouble(s->extremes[swar_pos]);
			} else {
				// Counter was never updated - return 0
				D_ASSERT(s->extremes[swar_pos] ==
				         (IS_MAX ? std::numeric_limits<T>::lowest() : std::numeric_limits<T>::max()));
				dst[j] = 0.0;
			}
		}
		CheckPacSampleDiversity(key_hash, dst, s->update_count, IS_MAX ? "as_noised_max" : "as_noised_min",
		                        input.bind_data->Cast<PrivBindData>());
	}
}

// Bind function for as_min/max_counters - similar to PacMinMaxBind but returns LIST<DOUBLE>
template <bool IS_MAX>
static unique_ptr<FunctionData> PacMinMaxCountersBind(ClientContext &ctx, AggregateFunction &function,
                                                      vector<unique_ptr<Expression>> &args) {
	auto &value_type = args[1]->return_type;
	auto physical_type = value_type.InternalType();

	function.return_type = LogicalType::LIST(PacFloatLogicalType());
	function.arguments[1] = value_type;

	// Select implementation based on physical type
#define BIND_COUNTERS_TYPE(PHYS_TYPE, CPP_TYPE)                                                                        \
	case PhysicalType::PHYS_TYPE:                                                                                      \
		function.state_size = PacMinMaxStateSize<CPP_TYPE, IS_MAX>;                                                    \
		function.initialize = PacMinMaxInitialize<CPP_TYPE, IS_MAX>;                                                   \
		function.update = PacMinMaxScatterUpdate<CPP_TYPE, IS_MAX>;                                                    \
		function.simple_update = PacMinMaxUpdate<CPP_TYPE, IS_MAX>;                                                    \
		function.combine = PacMinMaxCombine<CPP_TYPE, IS_MAX>;                                                         \
		function.finalize = PacMinMaxFinalizeCounters<CPP_TYPE, IS_MAX>;                                               \
		break

	switch (physical_type) {
		BIND_COUNTERS_TYPE(INT8, int8_t);
		BIND_COUNTERS_TYPE(INT16, int16_t);
		BIND_COUNTERS_TYPE(INT32, int32_t);
		BIND_COUNTERS_TYPE(INT64, int64_t);
		BIND_COUNTERS_TYPE(UINT8, uint8_t);
		BIND_COUNTERS_TYPE(UINT16, uint16_t);
		BIND_COUNTERS_TYPE(UINT32, uint32_t);
		BIND_COUNTERS_TYPE(UINT64, uint64_t);
		BIND_COUNTERS_TYPE(FLOAT, float);
		BIND_COUNTERS_TYPE(DOUBLE, double);
		BIND_COUNTERS_TYPE(INT128, hugeint_t);
		BIND_COUNTERS_TYPE(UINT128, uhugeint_t);
	default:
		throw NotImplementedException("pac_%s_counters not implemented for type %s", IS_MAX ? "max" : "min",
		                              value_type.ToString());
	}
#undef BIND_COUNTERS_TYPE

	return MakePrivBindData(ctx, args, 2, IS_MAX ? "as_max" : "as_min");
}

// ============================================================================
// SASS MIN/MAX counters: Returns all 64 sample values as LIST<DOUBLE>
// ============================================================================
template <typename T, bool IS_MAX>
static void DpSampleMinMaxFinalizeCounters(Vector &states, AggregateInputData &input, Vector &result, idx_t count,
                                           idx_t offset) {
	auto state_ptrs = FlatVector::GetData<MinMaxState<T, IS_MAX> *>(states);
	auto list_entries = FlatVector::GetData<list_entry_t>(result);
	auto &child_vec = ListVector::GetEntry(result);

	idx_t total_elements = count * 64;
	ListVector::Reserve(result, total_elements);
	ListVector::SetListSize(result, total_elements);

	auto child_data = FlatVector::GetData<PAC_FLOAT>(child_vec);
	auto &child_validity = FlatVector::Validity(child_vec);
	child_validity.SetAllValid(total_elements);
	PAC_FLOAT buf[64];
	double scale_divisor = input.bind_data ? input.bind_data->Cast<PrivBindData>().scale_divisor : 1.0;

	for (idx_t i = 0; i < count; i++) {
#ifndef PAC_NOBUFFERING
		state_ptrs[i]->FlushBuffer(*state_ptrs[i], input.allocator);
#endif
		auto *s = state_ptrs[i]->GetState();

		list_entries[offset + i].offset = i * 64;
		list_entries[offset + i].length = 64;

		idx_t base = i * 64;
		if (!s || !s->initialized) {
			memset(child_data + base, 0, 64 * sizeof(PAC_FLOAT));
			for (int j = 0; j < 64; j++) {
				child_validity.SetInvalid(base + j);
			}
			continue;
		}

		uint64_t key_hash = s->key_hash;
		s->GetTotals(buf);
		for (int j = 0; j < 64; j++) {
			if ((key_hash >> j) & 1ULL) {
				child_data[base + j] = static_cast<PAC_FLOAT>(buf[j] / scale_divisor);
			} else {
				child_data[base + j] = 0.0;
				child_validity.SetInvalid(base + j);
			}
		}
	}
}

template <bool IS_MAX>
static unique_ptr<FunctionData> DpSampleMinMaxBind(ClientContext &ctx, AggregateFunction &function,
                                                   vector<unique_ptr<Expression>> &args) {
	auto &value_type = args[1]->return_type;
	auto physical_type = value_type.InternalType();
	double scale_divisor = value_type.id() == LogicalTypeId::DECIMAL
	                           ? std::pow(10.0, static_cast<double>(DecimalType::GetScale(value_type)))
	                           : 1.0;

	function.return_type = LogicalType::LIST(PacFloatLogicalType());
	function.arguments[1] = value_type;

#define BIND_DP_SAMPLE_TYPE(PHYS_TYPE, CPP_TYPE)                                                                       \
	case PhysicalType::PHYS_TYPE:                                                                                      \
		function.state_size = PacMinMaxStateSize<CPP_TYPE, IS_MAX>;                                                    \
		function.initialize = PacMinMaxInitialize<CPP_TYPE, IS_MAX>;                                                   \
		function.update = PacMinMaxScatterUpdate<CPP_TYPE, IS_MAX>;                                                    \
		function.simple_update = PacMinMaxUpdate<CPP_TYPE, IS_MAX>;                                                    \
		function.combine = PacMinMaxCombine<CPP_TYPE, IS_MAX>;                                                         \
		function.finalize = DpSampleMinMaxFinalizeCounters<CPP_TYPE, IS_MAX>;                                          \
		break

	switch (physical_type) {
		BIND_DP_SAMPLE_TYPE(INT8, int8_t);
		BIND_DP_SAMPLE_TYPE(INT16, int16_t);
		BIND_DP_SAMPLE_TYPE(INT32, int32_t);
		BIND_DP_SAMPLE_TYPE(INT64, int64_t);
		BIND_DP_SAMPLE_TYPE(UINT8, uint8_t);
		BIND_DP_SAMPLE_TYPE(UINT16, uint16_t);
		BIND_DP_SAMPLE_TYPE(UINT32, uint32_t);
		BIND_DP_SAMPLE_TYPE(UINT64, uint64_t);
		BIND_DP_SAMPLE_TYPE(FLOAT, float);
		BIND_DP_SAMPLE_TYPE(DOUBLE, double);
		BIND_DP_SAMPLE_TYPE(INT128, hugeint_t);
		BIND_DP_SAMPLE_TYPE(UINT128, uhugeint_t);
	default:
		throw NotImplementedException("as_sample_%s not implemented for type %s", IS_MAX ? "max" : "min",
		                              value_type.ToString());
	}
#undef BIND_DP_SAMPLE_TYPE

	return make_uniq<PrivBindData>(ctx, 0.0, 1.0, scale_divisor, false, GetDpSampleLanes(ctx));
}

// ============================================================================
// Registration
// ============================================================================

void RegisterPacMinFunctions(ExtensionLoader &loader) {
	AggregateFunctionSet fcn_set("as_noised_min");

	fcn_set.AddFunction(AggregateFunction("as_noised_min", {LogicalType::UBIGINT, LogicalType::ANY}, LogicalType::ANY,
	                                      nullptr, nullptr, nullptr, nullptr, nullptr,
	                                      FunctionNullHandling::DEFAULT_NULL_HANDLING, nullptr, PacMinMaxBind<false>));

	fcn_set.AddFunction(AggregateFunction("as_noised_min",
	                                      {LogicalType::UBIGINT, LogicalType::ANY, LogicalType::DOUBLE},
	                                      LogicalType::ANY, nullptr, nullptr, nullptr, nullptr, nullptr,
	                                      FunctionNullHandling::DEFAULT_NULL_HANDLING, nullptr, PacMinMaxBind<false>));

	CreateAggregateFunctionInfo info(fcn_set);
	FunctionDescription desc;
	desc.description = "Privacy-preserving MIN. Automatically injected by PAC for protected columns.";
	desc.examples = {
	    "SELECT c_mktsegment, as_noised_min(priv_hash(hash(c_custkey)), c_acctbal) FROM customer GROUP BY "
	    "c_mktsegment"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

void RegisterPacMaxFunctions(ExtensionLoader &loader) {
	AggregateFunctionSet fcn_set("as_noised_max");

	fcn_set.AddFunction(AggregateFunction("as_noised_max", {LogicalType::UBIGINT, LogicalType::ANY}, LogicalType::ANY,
	                                      nullptr, nullptr, nullptr, nullptr, nullptr,
	                                      FunctionNullHandling::DEFAULT_NULL_HANDLING, nullptr, PacMinMaxBind<true>));

	fcn_set.AddFunction(AggregateFunction(
	    "as_noised_max", {LogicalType::UBIGINT, LogicalType::ANY, LogicalType::DOUBLE}, LogicalType::ANY, nullptr,
	    nullptr, nullptr, nullptr, nullptr, FunctionNullHandling::DEFAULT_NULL_HANDLING, nullptr, PacMinMaxBind<true>));

	CreateAggregateFunctionInfo info(fcn_set);
	FunctionDescription desc;
	desc.description = "Privacy-preserving MAX. Automatically injected by PAC for protected columns.";
	desc.examples = {
	    "SELECT c_mktsegment, as_noised_max(priv_hash(hash(c_custkey)), c_acctbal) FROM customer GROUP BY "
	    "c_mktsegment"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

void RegisterPacMinCountersFunctions(ExtensionLoader &loader) {
	auto list_double_type = LogicalType::LIST(PacFloatLogicalType());
	AggregateFunctionSet fcn_set("as_min");

	fcn_set.AddFunction(AggregateFunction(
	    "as_min", {LogicalType::UBIGINT, LogicalType::ANY}, list_double_type, nullptr, nullptr, nullptr, nullptr,
	    nullptr, FunctionNullHandling::DEFAULT_NULL_HANDLING, nullptr, PacMinMaxCountersBind<false>));

	// Add list aggregate overload (LIST<DOUBLE> → LIST<DOUBLE>) for subquery/categorical contexts
	AddPacListAggregateOverload(fcn_set, "min");

	CreateAggregateFunctionInfo info(fcn_set);
	FunctionDescription desc;
	desc.description = "[INTERNAL] Returns 64 PAC subsample counters as LIST for categorical queries.";
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

void RegisterPacMaxCountersFunctions(ExtensionLoader &loader) {
	auto list_double_type = LogicalType::LIST(PacFloatLogicalType());
	AggregateFunctionSet fcn_set("as_max");

	fcn_set.AddFunction(AggregateFunction(
	    "as_max", {LogicalType::UBIGINT, LogicalType::ANY}, list_double_type, nullptr, nullptr, nullptr, nullptr,
	    nullptr, FunctionNullHandling::DEFAULT_NULL_HANDLING, nullptr, PacMinMaxCountersBind<true>));

	// Add list aggregate overload (LIST<DOUBLE> → LIST<DOUBLE>) for subquery/categorical contexts
	AddPacListAggregateOverload(fcn_set, "max");

	CreateAggregateFunctionInfo info(fcn_set);
	FunctionDescription desc;
	desc.description = "[INTERNAL] Returns 64 PAC subsample counters as LIST for categorical queries.";
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

void RegisterDpSampleMinFunctions(ExtensionLoader &loader) {
	auto list_type = LogicalType::LIST(PacFloatLogicalType());
	AggregateFunctionSet set("as_sample_min");
	set.AddFunction(AggregateFunction("as_sample_min", {LogicalType::UBIGINT, LogicalType::ANY}, list_type, nullptr,
	                                  nullptr, nullptr, nullptr, nullptr, FunctionNullHandling::DEFAULT_NULL_HANDLING,
	                                  nullptr, DpSampleMinMaxBind<false>));
	CreateAggregateFunctionInfo info(set);
	FunctionDescription desc;
	desc.description = "[INTERNAL] Returns 64 sample-median DP counters for MIN-like values.";
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

void RegisterDpSampleMaxFunctions(ExtensionLoader &loader) {
	auto list_type = LogicalType::LIST(PacFloatLogicalType());
	AggregateFunctionSet set("as_sample_max");
	set.AddFunction(AggregateFunction("as_sample_max", {LogicalType::UBIGINT, LogicalType::ANY}, list_type, nullptr,
	                                  nullptr, nullptr, nullptr, nullptr, FunctionNullHandling::DEFAULT_NULL_HANDLING,
	                                  nullptr, DpSampleMinMaxBind<true>));
	CreateAggregateFunctionInfo info(set);
	FunctionDescription desc;
	desc.description = "[INTERNAL] Returns 64 sample-median DP counters for MAX-like values.";
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

// ============================================================================
// Clip min/max registration moved to as_clip_min_max.cpp

// Explicit template instantiations
#define INST_ALL(T)                                                                                                    \
	template void PacMinMaxUpdate<T, false>(Vector[], AggregateInputData &, idx_t, data_ptr_t, idx_t);                 \
	template void PacMinMaxUpdate<T, true>(Vector[], AggregateInputData &, idx_t, data_ptr_t, idx_t);                  \
	template void PacMinMaxScatterUpdate<T, false>(Vector[], AggregateInputData &, idx_t, Vector &, idx_t);            \
	template void PacMinMaxScatterUpdate<T, true>(Vector[], AggregateInputData &, idx_t, Vector &, idx_t);             \
	template void PacMinMaxCombine<T, false>(Vector &, Vector &, AggregateInputData &, idx_t);                         \
	template void PacMinMaxCombine<T, true>(Vector &, Vector &, AggregateInputData &, idx_t)

INST_ALL(int8_t);
INST_ALL(int16_t);
INST_ALL(int32_t);
INST_ALL(int64_t);
INST_ALL(uint8_t);
INST_ALL(uint16_t);
INST_ALL(uint32_t);
INST_ALL(uint64_t);
INST_ALL(float);
INST_ALL(double);
INST_ALL(hugeint_t);
INST_ALL(uhugeint_t);

} // namespace duckdb
