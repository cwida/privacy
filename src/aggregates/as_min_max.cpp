#include "duckdb/common/vector/list_vector.hpp"
#include "aggregates/as_min_max.hpp"
#include "aggregates/aggregate_state_export.hpp"
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

template <typename T, bool IS_MAX>
struct MinMaxStateCodec {
	using State = MinMaxState<T, IS_MAX>;
	using Inner = PacMinMaxState<T, IS_MAX>;
	static void WriteInner(AggregateStateWriter &out, const Inner &state) {
		out.Write(uint8_t(state.initialized));
		out.Write(state.update_count);
		out.Write(state.key_hash);
		out.Write(state.global_bound);
		out.Write(state.extremes);
	}
	static void ReadInner(AggregateStateReader &in, Inner &state) {
		state.initialized = in.ReadFlag();
		in.Read(state.update_count);
		in.Read(state.key_hash);
		in.Read(state.global_bound);
		in.Read(state.extremes);
	}
	static void Write(AggregateStateWriter &out, const State &state) {
#ifdef PAC_NOBUFFERING
		WriteInner(out, state);
#else
		auto buffered = state.n_buffered & State::BUF_MASK;
		out.Write(uint8_t(buffered));
		for (idx_t i = 0; i < buffered; i++) {
			out.Write(state.val_buf[i]);
			out.Write(state.hash_buf[i]);
		}
		auto *inner = state.GetState();
		out.Write(uint8_t(inner != nullptr));
		if (inner) {
			WriteInner(out, *inner);
		}
#endif
	}
	static void Read(AggregateStateReader &in, State &state, ArenaAllocator &allocator) {
		memset(&state, 0, sizeof(state));
#ifdef PAC_NOBUFFERING
		ReadInner(in, state);
#else
		uint8_t buffered;
		in.Read(buffered);
		if (buffered > State::BUF_SIZE) {
			throw InvalidInputException("Invalid stochastic MIN/MAX buffer size");
		}
		for (idx_t i = 0; i < buffered; i++) {
			in.Read(state.val_buf[i]);
			in.Read(state.hash_buf[i]);
		}
		if (in.ReadFlag()) {
			ReadInner(in, *state.EnsureState(allocator));
		}
		state.n_buffered |= buffered;
#endif
	}
};

// ============================================================================
// Update functions
// ============================================================================

// Non-grouped update
template <typename T, bool IS_MAX>
static void PacMinMaxUpdate(Vector inputs[], AggregateInputData &aggr, idx_t input_count, const ClusteredAggr &clusters,
                            idx_t count) {
	ExecutePrivClusters(inputs, input_count, clusters, [&](Vector inputs[], data_ptr_t state_p, idx_t count) {
		auto &agg = *reinterpret_cast<MinMaxState<T, IS_MAX> *>(state_p);
		int sample_lanes = GetPrivSampleLanes(aggr);

		UnifiedVectorFormat hash_data, value_data;
		inputs[0].ToUnifiedFormat(hash_data);
		inputs[1].ToUnifiedFormat(value_data);
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
	});
}

// Grouped (scatter) update
template <typename T, bool IS_MAX>
static void PacMinMaxScatterUpdate(Vector inputs[], AggregateInputData &aggr, idx_t, Vector &states, idx_t count) {
	int sample_lanes = GetPrivSampleLanes(aggr);
	UnifiedVectorFormat hash_data, value_data, sdata;
	inputs[0].ToUnifiedFormat(hash_data);
	inputs[1].ToUnifiedFormat(value_data);
	states.ToUnifiedFormat(sdata);

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
static void PacMinMaxFinalize(Vector &states, AggregateFinalizeInputData &input, Vector &result, idx_t count,
                              idx_t offset) {
	auto state_ptrs = FlatVector::GetData<MinMaxState<T, IS_MAX> *>(states);
	auto data = FlatVector::GetDataMutable<T>(result);
	auto &result_mask = FlatVector::ValidityMutable(result);

	double mi = input.bind_data ? input.bind_data->Cast<PrivBindData>().mi : 0.0;
	double correction = input.bind_data ? input.bind_data->Cast<PrivBindData>().correction : 1.0;
	uint64_t query_hash = input.bind_data ? input.bind_data->Cast<PrivBindData>().query_hash : 0;
	auto pstate = input.bind_data ? input.bind_data->Cast<PrivBindData>().pstate : nullptr;
	auto gen = input.bind_data ? PacRandomStream(input.bind_data->Cast<PrivBindData>().seed,
	                                             *input.bind_data->Cast<PrivBindData>().random_state)
	                           : PacRandomEngine(0, 0);

	for (idx_t i = 0; i < count; i++) {
#ifndef PAC_NOBUFFERING
		// flush the buffer into yourself, possibly allocating the state (array with 64 totals)
		state_ptrs[i]->FlushBuffer(*state_ptrs[i], input.allocator);
#endif
		auto *s = state_ptrs[i]->GetState();
		// Check if we should return NULL based on key_hash
		uint64_t key_hash = s ? s->key_hash : 0;
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
static idx_t PacMinMaxStateSize(AggregateStateInput &) {
	return sizeof(MinMaxState<T, IS_MAX>);
}

template <typename T, bool IS_MAX>
static void PacMinMaxInitialize(AggregateStateInput &, data_ptr_t *states, idx_t count) {
	for (idx_t i = 0; i < count; i++) {
		auto p = states[i];
		memset(p, 0, sizeof(MinMaxState<T, IS_MAX>));
	}
}

// ============================================================================
// Bind function - selects implementation based on input type
// ============================================================================

template <bool IS_MAX>
static unique_ptr<FunctionData> PacMinMaxBind(BindAggregateFunctionInput &bind_input) {
	auto &ctx = bind_input.GetClientContext();
	auto &function = bind_input.GetBoundFunction();
	auto &args = bind_input.GetArguments();
	auto &value_type = args[1]->GetReturnType();
	auto physical_type = value_type.InternalType();

	function.SetReturnType(value_type);
	function.GetArguments()[1] = value_type;

	// Select implementation based on physical type
#define BIND_TYPE(PHYS_TYPE, CPP_TYPE)                                                                                 \
	case PhysicalType::PHYS_TYPE:                                                                                      \
		function.SetStateSizeCallback(PacMinMaxStateSize<CPP_TYPE, IS_MAX>);                                           \
		function.SetStateInitCallback(PacMinMaxInitialize<CPP_TYPE, IS_MAX>);                                          \
		function.SetStateUpdateCallback(PacMinMaxScatterUpdate<CPP_TYPE, IS_MAX>);                                     \
		function.SetStateClusterUpdateCallback(PacMinMaxUpdate<CPP_TYPE, IS_MAX>);                                     \
		function.SetStateCombineCallback(PacMinMaxCombine<CPP_TYPE, IS_MAX>);                                          \
		function.SetStateFinalizeCallback(PacMinMaxFinalize<CPP_TYPE, IS_MAX>);                                        \
		AggregateStateExport<MinMaxState<CPP_TYPE, IS_MAX>, MinMaxStateCodec<CPP_TYPE, IS_MAX>>::Register(function);   \
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
static void PacMinMaxFinalizeCounters(Vector &states, AggregateFinalizeInputData &input, Vector &result, idx_t count,
                                      idx_t offset) {
	auto state_ptrs = FlatVector::GetData<MinMaxState<T, IS_MAX> *>(states);

	// Result is LIST<DOUBLE>
	auto list_entries = FlatVector::GetDataMutable<list_entry_t>(result);
	auto &child_vec = ListVector::GetChildMutable(result);

	// Reserve space for all lists (64 elements each)
	idx_t total_elements = (offset + count) * 64;
	ListVector::Reserve(result, total_elements);
	ListVector::SetListSize(result, total_elements);

	auto child_data = FlatVector::GetDataMutable<PAC_FLOAT>(child_vec);

	for (idx_t i = 0; i < count; i++) {
#ifndef PAC_NOBUFFERING
		// flush the buffer into yourself, possibly allocating the state (array with 64 totals)
		state_ptrs[i]->FlushBuffer(*state_ptrs[i], input.allocator);
#endif
		auto *s = state_ptrs[i]->GetState();

		// Set up the list entry - always needed even for NULL results
		idx_t base = (offset + i) * 64;
		list_entries[offset + i].offset = base;
		list_entries[offset + i].length = 64;

		if (!s || !s->initialized) {
			// No values seen: output 64 zeros
			memset(child_data + base, 0, 64 * sizeof(PAC_FLOAT));
			continue;
		}

		uint64_t key_hash = s->key_hash;
		PAC_FLOAT *dst = &child_data[base];
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
static unique_ptr<FunctionData> PacMinMaxCountersBind(BindAggregateFunctionInput &bind_input) {
	auto &ctx = bind_input.GetClientContext();
	auto &function = bind_input.GetBoundFunction();
	auto &args = bind_input.GetArguments();
	auto &value_type = args[1]->GetReturnType();
	auto physical_type = value_type.InternalType();

	function.SetReturnType(LogicalType::LIST(PacFloatLogicalType()));
	function.GetArguments()[1] = value_type;

	// Select implementation based on physical type
#define BIND_COUNTERS_TYPE(PHYS_TYPE, CPP_TYPE)                                                                        \
	case PhysicalType::PHYS_TYPE:                                                                                      \
		function.SetStateSizeCallback(PacMinMaxStateSize<CPP_TYPE, IS_MAX>);                                           \
		function.SetStateInitCallback(PacMinMaxInitialize<CPP_TYPE, IS_MAX>);                                          \
		function.SetStateUpdateCallback(PacMinMaxScatterUpdate<CPP_TYPE, IS_MAX>);                                     \
		function.SetStateClusterUpdateCallback(PacMinMaxUpdate<CPP_TYPE, IS_MAX>);                                     \
		function.SetStateCombineCallback(PacMinMaxCombine<CPP_TYPE, IS_MAX>);                                          \
		function.SetStateFinalizeCallback(PacMinMaxFinalizeCounters<CPP_TYPE, IS_MAX>);                                \
		AggregateStateExport<MinMaxState<CPP_TYPE, IS_MAX>, MinMaxStateCodec<CPP_TYPE, IS_MAX>>::Register(function);   \
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
static void DpSampleMinMaxFinalizeCounters(Vector &states, AggregateFinalizeInputData &input, Vector &result,
                                           idx_t count, idx_t offset) {
	auto state_ptrs = FlatVector::GetData<MinMaxState<T, IS_MAX> *>(states);
	auto list_entries = FlatVector::GetDataMutable<list_entry_t>(result);
	auto &child_vec = ListVector::GetChildMutable(result);

	idx_t total_elements = (offset + count) * 64;
	ListVector::Reserve(result, total_elements);
	ListVector::SetListSize(result, total_elements);

	auto child_data = FlatVector::GetDataMutable<PAC_FLOAT>(child_vec);
	auto &child_validity = FlatVector::ValidityMutable(child_vec);
	child_validity.SetAllValid(total_elements);
	PAC_FLOAT buf[64];
	double scale_divisor = input.bind_data ? input.bind_data->Cast<PrivBindData>().scale_divisor : 1.0;

	for (idx_t i = 0; i < count; i++) {
#ifndef PAC_NOBUFFERING
		state_ptrs[i]->FlushBuffer(*state_ptrs[i], input.allocator);
#endif
		auto *s = state_ptrs[i]->GetState();

		idx_t base = (offset + i) * 64;
		list_entries[offset + i].offset = base;
		list_entries[offset + i].length = 64;

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
static unique_ptr<FunctionData> DpSampleMinMaxBind(BindAggregateFunctionInput &bind_input) {
	auto &ctx = bind_input.GetClientContext();
	auto &function = bind_input.GetBoundFunction();
	auto &args = bind_input.GetArguments();
	auto &value_type = args[1]->GetReturnType();
	auto physical_type = value_type.InternalType();
	double scale_divisor = value_type.id() == LogicalTypeId::DECIMAL
	                           ? std::pow(10.0, static_cast<double>(DecimalType::GetScale(value_type)))
	                           : 1.0;

	function.SetReturnType(LogicalType::LIST(PacFloatLogicalType()));
	function.GetArguments()[1] = value_type;

#define BIND_DP_SAMPLE_TYPE(PHYS_TYPE, CPP_TYPE)                                                                       \
	case PhysicalType::PHYS_TYPE:                                                                                      \
		function.SetStateSizeCallback(PacMinMaxStateSize<CPP_TYPE, IS_MAX>);                                           \
		function.SetStateInitCallback(PacMinMaxInitialize<CPP_TYPE, IS_MAX>);                                          \
		function.SetStateUpdateCallback(PacMinMaxScatterUpdate<CPP_TYPE, IS_MAX>);                                     \
		function.SetStateClusterUpdateCallback(PacMinMaxUpdate<CPP_TYPE, IS_MAX>);                                     \
		function.SetStateCombineCallback(PacMinMaxCombine<CPP_TYPE, IS_MAX>);                                          \
		function.SetStateFinalizeCallback(DpSampleMinMaxFinalizeCounters<CPP_TYPE, IS_MAX>);                           \
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
// Variable-m SASS MIN/MAX counters. The raw PU hash selects one entry in an
// explicitly allocated m-lane state; empty lanes remain NULL for the release terminal
// to fill from the public output domain. MIN/MAX are never rescaled by m.
// ============================================================================
template <typename T, bool IS_MAX>
struct DpSampleMMinMaxState {
	T *values;
	bool *valid;
};

template <typename T, bool IS_MAX>
static idx_t DpSampleMMinMaxStateSize(AggregateStateInput &) {
	return sizeof(DpSampleMMinMaxState<T, IS_MAX>);
}

template <typename T, bool IS_MAX>
static void DpSampleMMinMaxInitialize(AggregateStateInput &, data_ptr_t *states, idx_t count) {
	for (idx_t i = 0; i < count; i++) {
		auto state_ptr = states[i];
		memset(state_ptr, 0, sizeof(DpSampleMMinMaxState<T, IS_MAX>));
	}
}

template <typename T, bool IS_MAX>
static void EnsureDpSampleMMinMaxArrays(DpSampleMMinMaxState<T, IS_MAX> &state, ArenaAllocator &allocator,
                                        int sample_count) {
	if (!state.values) {
		state.values = reinterpret_cast<T *>(allocator.AllocateAligned(sample_count * sizeof(T)));
		state.valid = reinterpret_cast<bool *>(allocator.AllocateAligned(sample_count * sizeof(bool)));
		memset(state.valid, 0, sample_count * sizeof(bool));
	}
}

template <typename T, bool IS_MAX>
static void DpSampleMMinMaxUpdateOne(DpSampleMMinMaxState<T, IS_MAX> &state, ArenaAllocator &allocator, uint64_t hash,
                                     T value, int sample_count) {
	EnsureDpSampleMMinMaxArrays(state, allocator, sample_count);
	idx_t lane = DpSassLaneIndex(hash, sample_count);
	if (!state.valid[lane] || PAC_IS_BETTER(value, state.values[lane])) {
		state.values[lane] = value;
		state.valid[lane] = true;
	}
}

template <typename T, bool IS_MAX>
static void DpSampleMMinMaxUpdate(Vector inputs[], AggregateInputData &aggr, idx_t input_count,
                                  const ClusteredAggr &clusters, idx_t count) {
	ExecutePrivClusters(inputs, input_count, clusters, [&](Vector inputs[], data_ptr_t state_ptr, idx_t count) {
		auto &state = *reinterpret_cast<DpSampleMMinMaxState<T, IS_MAX> *>(state_ptr);
		int sample_count = GetPrivSampleCount(aggr);
		UnifiedVectorFormat hash_data, value_data;
		inputs[0].ToUnifiedFormat(hash_data);
		inputs[1].ToUnifiedFormat(value_data);
		auto hashes = UnifiedVectorFormat::GetData<uint64_t>(hash_data);
		auto values = UnifiedVectorFormat::GetData<T>(value_data);
		for (idx_t i = 0; i < count; i++) {
			auto h_idx = hash_data.sel->get_index(i);
			auto v_idx = value_data.sel->get_index(i);
			if (hash_data.validity.RowIsValid(h_idx) && value_data.validity.RowIsValid(v_idx)) {
				DpSampleMMinMaxUpdateOne(state, aggr.allocator, hashes[h_idx], values[v_idx], sample_count);
			}
		}
	});
}

template <typename T, bool IS_MAX>
static void DpSampleMMinMaxScatterUpdate(Vector inputs[], AggregateInputData &aggr, idx_t, Vector &states,
                                         idx_t count) {
	int sample_count = GetPrivSampleCount(aggr);
	UnifiedVectorFormat hash_data, value_data, state_data;
	inputs[0].ToUnifiedFormat(hash_data);
	inputs[1].ToUnifiedFormat(value_data);
	states.ToUnifiedFormat(state_data);
	auto hashes = UnifiedVectorFormat::GetData<uint64_t>(hash_data);
	auto values = UnifiedVectorFormat::GetData<T>(value_data);
	auto state_ptrs = UnifiedVectorFormat::GetData<DpSampleMMinMaxState<T, IS_MAX> *>(state_data);
	for (idx_t i = 0; i < count; i++) {
		auto h_idx = hash_data.sel->get_index(i);
		auto v_idx = value_data.sel->get_index(i);
		if (hash_data.validity.RowIsValid(h_idx) && value_data.validity.RowIsValid(v_idx)) {
			auto *state = state_ptrs[state_data.sel->get_index(i)];
			DpSampleMMinMaxUpdateOne(*state, aggr.allocator, hashes[h_idx], values[v_idx], sample_count);
		}
	}
}

template <typename T, bool IS_MAX>
static void DpSampleMMinMaxCombine(Vector &src, Vector &dst, AggregateInputData &aggr, idx_t count) {
	int sample_count = GetPrivSampleCount(aggr);
	auto src_states = FlatVector::GetData<DpSampleMMinMaxState<T, IS_MAX> *>(src);
	auto dst_states = FlatVector::GetData<DpSampleMMinMaxState<T, IS_MAX> *>(dst);
	for (idx_t i = 0; i < count; i++) {
		auto *s = src_states[i];
		if (!s->values) {
			continue;
		}
		auto *d = dst_states[i];
		EnsureDpSampleMMinMaxArrays(*d, aggr.allocator, sample_count);
		for (int j = 0; j < sample_count; j++) {
			if (s->valid[j] && (!d->valid[j] || PAC_IS_BETTER(s->values[j], d->values[j]))) {
				d->values[j] = s->values[j];
				d->valid[j] = true;
			}
		}
	}
}

template <typename T, bool IS_MAX>
static void DpSampleMMinMaxFinalize(Vector &states, AggregateFinalizeInputData &input, Vector &result, idx_t count,
                                    idx_t offset) {
	int sample_count = GetPrivSampleCount(input);
	auto state_ptrs = FlatVector::GetData<DpSampleMMinMaxState<T, IS_MAX> *>(states);
	auto list_entries = FlatVector::GetDataMutable<list_entry_t>(result);
	auto &child_vec = ListVector::GetChildMutable(result);

	idx_t total_elements = (offset + count) * static_cast<idx_t>(sample_count);
	ListVector::Reserve(result, total_elements);
	ListVector::SetListSize(result, total_elements);

	auto child_data = FlatVector::GetDataMutable<PAC_FLOAT>(child_vec);
	auto &child_validity = FlatVector::ValidityMutable(child_vec);
	child_validity.SetAllValid(total_elements);
	double scale_divisor = input.bind_data ? input.bind_data->Cast<PrivBindData>().scale_divisor : 1.0;

	for (idx_t i = 0; i < count; i++) {
		auto *state = state_ptrs[i];
		idx_t base = (offset + i) * static_cast<idx_t>(sample_count);
		list_entries[offset + i].offset = base;
		list_entries[offset + i].length = sample_count;
		for (int j = 0; j < sample_count; j++) {
			if (state->values && state->valid[j]) {
				child_data[base + j] = static_cast<PAC_FLOAT>(ToDouble(state->values[j]) / scale_divisor);
			} else {
				child_data[base + j] = 0.0;
				child_validity.SetInvalid(base + j);
			}
		}
	}
}

template <bool IS_MAX>
static unique_ptr<FunctionData> DpSampleMMinMaxBind(BindAggregateFunctionInput &bind_input) {
	auto &ctx = bind_input.GetClientContext();
	auto &function = bind_input.GetBoundFunction();
	auto &args = bind_input.GetArguments();
	int sample_lanes = GetDpSampleLanes(ctx);
	if (sample_lanes != 1) {
		throw InvalidInputException("as_sample_m_%s: dp_sample_lanes must be 1 when dp_sass_m > 64",
		                            IS_MAX ? "max" : "min");
	}
	auto &value_type = args[1]->GetReturnType();
	auto physical_type = value_type.InternalType();
	double scale_divisor = value_type.id() == LogicalTypeId::DECIMAL
	                           ? std::pow(10.0, static_cast<double>(DecimalType::GetScale(value_type)))
	                           : 1.0;

	function.SetReturnType(LogicalType::LIST(PacFloatLogicalType()));
	function.GetArguments()[1] = value_type;

#define BIND_DP_SAMPLE_M_TYPE(PHYS_TYPE, CPP_TYPE)                                                                     \
	case PhysicalType::PHYS_TYPE:                                                                                      \
		function.SetStateSizeCallback(DpSampleMMinMaxStateSize<CPP_TYPE, IS_MAX>);                                     \
		function.SetStateInitCallback(DpSampleMMinMaxInitialize<CPP_TYPE, IS_MAX>);                                    \
		function.SetStateUpdateCallback(DpSampleMMinMaxScatterUpdate<CPP_TYPE, IS_MAX>);                               \
		function.SetStateClusterUpdateCallback(DpSampleMMinMaxUpdate<CPP_TYPE, IS_MAX>);                               \
		function.SetStateCombineCallback(DpSampleMMinMaxCombine<CPP_TYPE, IS_MAX>);                                    \
		function.SetStateFinalizeCallback(DpSampleMMinMaxFinalize<CPP_TYPE, IS_MAX>);                                  \
		break

	switch (physical_type) {
		BIND_DP_SAMPLE_M_TYPE(INT8, int8_t);
		BIND_DP_SAMPLE_M_TYPE(INT16, int16_t);
		BIND_DP_SAMPLE_M_TYPE(INT32, int32_t);
		BIND_DP_SAMPLE_M_TYPE(INT64, int64_t);
		BIND_DP_SAMPLE_M_TYPE(UINT8, uint8_t);
		BIND_DP_SAMPLE_M_TYPE(UINT16, uint16_t);
		BIND_DP_SAMPLE_M_TYPE(UINT32, uint32_t);
		BIND_DP_SAMPLE_M_TYPE(UINT64, uint64_t);
		BIND_DP_SAMPLE_M_TYPE(FLOAT, float);
		BIND_DP_SAMPLE_M_TYPE(DOUBLE, double);
		BIND_DP_SAMPLE_M_TYPE(INT128, hugeint_t);
		BIND_DP_SAMPLE_M_TYPE(UINT128, uhugeint_t);
	default:
		throw NotImplementedException("as_sample_m_%s not implemented for type %s", IS_MAX ? "max" : "min",
		                              value_type.ToString());
	}
#undef BIND_DP_SAMPLE_M_TYPE

	return make_uniq<PrivBindData>(ctx, 0.0, 1.0, scale_divisor, false, GetDpSampleLanes(ctx), GetDpSassM(ctx));
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
	desc.examples = {"SELECT c_mktsegment, as_noised_min(priv_hash(hash(c_custkey)), c_acctbal) FROM customer GROUP BY "
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
	desc.examples = {"SELECT c_mktsegment, as_noised_max(priv_hash(hash(c_custkey)), c_acctbal) FROM customer GROUP BY "
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

void RegisterDpSampleMMinFunctions(ExtensionLoader &loader) {
	auto list_type = LogicalType::LIST(PacFloatLogicalType());
	AggregateFunctionSet set("as_sample_m_min");
	set.AddFunction(AggregateFunction("as_sample_m_min", {LogicalType::UBIGINT, LogicalType::ANY}, list_type, nullptr,
	                                  nullptr, nullptr, nullptr, nullptr, FunctionNullHandling::DEFAULT_NULL_HANDLING,
	                                  nullptr, DpSampleMMinMaxBind<false>));
	CreateAggregateFunctionInfo info(set);
	FunctionDescription desc;
	desc.description = "[INTERNAL] Returns variable-m sample-median DP counters for MIN-like values.";
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

void RegisterDpSampleMMaxFunctions(ExtensionLoader &loader) {
	auto list_type = LogicalType::LIST(PacFloatLogicalType());
	AggregateFunctionSet set("as_sample_m_max");
	set.AddFunction(AggregateFunction("as_sample_m_max", {LogicalType::UBIGINT, LogicalType::ANY}, list_type, nullptr,
	                                  nullptr, nullptr, nullptr, nullptr, FunctionNullHandling::DEFAULT_NULL_HANDLING,
	                                  nullptr, DpSampleMMinMaxBind<true>));
	CreateAggregateFunctionInfo info(set);
	FunctionDescription desc;
	desc.description = "[INTERNAL] Returns variable-m sample-median DP counters for MAX-like values.";
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

// ============================================================================
// Clip min/max registration moved to as_clip_min_max.cpp

// Explicit template instantiations
#define INST_ALL(T)                                                                                                    \
	template void PacMinMaxUpdate<T, false>(Vector[], AggregateInputData &, idx_t, const ClusteredAggr &, idx_t);      \
	template void PacMinMaxUpdate<T, true>(Vector[], AggregateInputData &, idx_t, const ClusteredAggr &, idx_t);       \
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
