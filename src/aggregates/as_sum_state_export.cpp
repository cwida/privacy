#include "aggregates/as_sum_state_export.hpp"
#include "aggregates/as_sum.hpp"
#include "aggregates/aggregate_state_export.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"

namespace duckdb {
namespace {

// Spill the native counters, not finalized totals: finalization can round and
// applies sampling correction. No process-local pointers enter the exported BLOB.
template <class STATE>
struct SumStateExport;

template <bool SIGNED>
struct SumStateExport<PacSumIntState<SIGNED>> {
	using State = PacSumIntState<SIGNED>;

	template <class T, idx_t N>
	static void WriteLevel(AggregateStateWriter &out, const T *ptr) {
		out.Write(uint8_t(ptr != nullptr));
		if (ptr) {
			out.WriteData(ptr, sizeof(T) * N);
		}
	}

	template <class T, idx_t N>
	static void ReadLevel(AggregateStateReader &in, T *&ptr, ArenaAllocator &allocator) {
		if (!in.ReadFlag()) {
			return;
		}
		ptr = reinterpret_cast<T *>(allocator.Allocate(sizeof(T) * N));
		in.ReadData(ptr, sizeof(T) * N);
	}

	static void Write(AggregateStateWriter &out, const State &state) {
		out.Write(state.key_hash);
		out.Write(state.update_count);
#ifndef PAC_EXACTSUM
		out.Write(state.max_level_used);
		for (int k = 0; k <= state.max_level_used; k++) {
			out.Write(state.exact_total[k]);
			WriteLevel<uint64_t, PAC_APPROX_SWAR_ELEMENTS>(out, state.u.levels[k]);
		}
#elif defined(PAC_NOCASCADING)
		out.Write(state.probabilistic_total128);
#else
		out.Write(state.exact_total8);
		out.Write(state.exact_total16);
		out.Write(state.exact_total32);
		out.Write(state.exact_total64);
		WriteLevel<uint64_t, 8>(out, state.probabilistic_total8);
		WriteLevel<uint64_t, 16>(out, state.probabilistic_total16);
		WriteLevel<uint64_t, 32>(out, state.probabilistic_total32);
		WriteLevel<uint64_t, 64>(out, state.probabilistic_total64);
		WriteLevel<hugeint_t, 64>(out, state.probabilistic_total128);
#endif
	}

	static void Read(AggregateStateReader &in, State &state, ArenaAllocator &allocator) {
		memset(&state, 0, sizeof(state));
		in.Read(state.key_hash);
		in.Read(state.update_count);
#ifndef PAC_EXACTSUM
		in.Read(state.max_level_used);
		state.inline_level_idx = -1;
		if (state.max_level_used < -1 || state.max_level_used >= PAC_APPROX_NUM_LEVELS) {
			throw InvalidInputException("Invalid stochastic SUM cascade level");
		}
		for (int k = 0; k <= state.max_level_used; k++) {
			in.Read(state.exact_total[k]);
#ifdef PAC_SIGNEDSUM
			if (state.exact_total[k] < State::UNDERFLOW_THRESHOLD || state.exact_total[k] > State::OVERFLOW_THRESHOLD) {
#else
			if (state.exact_total[k] < 0 || state.exact_total[k] > PacSumIntState<false>::OVERFLOW_THRESHOLD) {
#endif
				throw InvalidInputException("Invalid stochastic SUM cascade budget");
			}
			if (!in.ReadFlag()) {
				throw InvalidInputException("Missing stochastic SUM cascade level");
			}
			// Retain the inline optimization when all pointer slots fit before it.
			if (state.max_level_used < 9) {
				state.AllocateLevel(allocator, k);
			} else {
				state.u.levels[k] =
				    reinterpret_cast<uint64_t *>(allocator.Allocate(sizeof(uint64_t) * PAC_APPROX_SWAR_ELEMENTS));
			}
			in.ReadData(state.u.levels[k], sizeof(uint64_t) * PAC_APPROX_SWAR_ELEMENTS);
		}
#elif defined(PAC_NOCASCADING)
		in.Read(state.probabilistic_total128);
#else
		in.Read(state.exact_total8);
		in.Read(state.exact_total16);
		in.Read(state.exact_total32);
		in.Read(state.exact_total64);
		ReadLevel<uint64_t, 8>(in, state.probabilistic_total8, allocator);
		ReadLevel<uint64_t, 16>(in, state.probabilistic_total16, allocator);
		ReadLevel<uint64_t, 32>(in, state.probabilistic_total32, allocator);
		ReadLevel<uint64_t, 64>(in, state.probabilistic_total64, allocator);
		ReadLevel<hugeint_t, 64>(in, state.probabilistic_total128, allocator);
#endif
	}
};

template <>
struct SumStateExport<PacSumDoubleState> {
	static void Write(AggregateStateWriter &out, const PacSumDoubleState &state) {
		out.Write(state.key_hash);
		out.Write(state.update_count);
		out.Write(state.probabilistic_total);
	}

	static void Read(AggregateStateReader &in, PacSumDoubleState &state, ArenaAllocator &) {
		in.Read(state.key_hash);
		in.Read(state.update_count);
		in.Read(state.probabilistic_total);
	}
};

#ifndef PAC_NOBUFFERING
template <class INNER, class VALUE>
struct SumStateExport<PacSumStateWrapper<INNER, VALUE>> {
	using State = PacSumStateWrapper<INNER, VALUE>;

	static void Write(AggregateStateWriter &out, const State &state) {
		auto buffered = state.n_buffered & State::BUF_MASK;
		out.Write(uint8_t(buffered));
		for (idx_t i = 0; i < buffered; i++) {
			out.Write(state.val_buf[i]);
			out.Write(state.hash_buf[i]);
		}
		auto *inner = state.GetState();
		out.Write(uint8_t(inner != nullptr));
		if (inner) {
			SumStateExport<INNER>::Write(out, *inner);
		}
#ifndef PAC_SIGNEDSUM
		out.Write(uint8_t(state.neg_state != nullptr));
		if (state.neg_state) {
			SumStateExport<PacSumIntState<false>>::Write(out, *state.neg_state);
		}
#endif
	}

	static void Read(AggregateStateReader &in, State &state, ArenaAllocator &allocator) {
		memset(&state, 0, sizeof(state));
		uint8_t buffered;
		in.Read(buffered);
		if (buffered > State::BUF_SIZE) {
			throw InvalidInputException("Invalid stochastic SUM buffer size");
		}
		for (idx_t i = 0; i < buffered; i++) {
			in.Read(state.val_buf[i]);
			in.Read(state.hash_buf[i]);
		}
		if (in.ReadFlag()) {
			auto *inner = state.EnsureState(allocator);
			SumStateExport<INNER>::Read(in, *inner, allocator);
		}
		state.n_buffered |= buffered;
#ifndef PAC_SIGNEDSUM
		if (in.ReadFlag()) {
			auto *negative = state.EnsureNegState(allocator);
			SumStateExport<PacSumIntState<false>>::Read(in, *negative, allocator);
		}
#endif
	}
};
#endif

} // namespace

void RegisterSumStateExport(AggregateFunction &function) {
	auto value_type = function.GetSignature().GetParameter(1).GetType().id();
	if (value_type == LogicalTypeId::FLOAT || value_type == LogicalTypeId::DOUBLE ||
	    value_type == LogicalTypeId::UHUGEINT
#ifndef PAC_EXACTSUM
	    || value_type == LogicalTypeId::HUGEINT
#endif
	) {
		AggregateStateExport<ScatterDoubleState, SumStateExport<ScatterDoubleState>>::Register(function);
	} else if (value_type == LogicalTypeId::UTINYINT || value_type == LogicalTypeId::USMALLINT ||
	           value_type == LogicalTypeId::UINTEGER || value_type == LogicalTypeId::UBIGINT) {
		AggregateStateExport<ScatterIntState<false>, SumStateExport<ScatterIntState<false>>>::Register(function);
	} else if (value_type == LogicalTypeId::BOOLEAN || value_type == LogicalTypeId::TINYINT ||
	           value_type == LogicalTypeId::SMALLINT || value_type == LogicalTypeId::INTEGER ||
	           value_type == LogicalTypeId::BIGINT || value_type == LogicalTypeId::HUGEINT) {
		AggregateStateExport<ScatterIntState<true>, SumStateExport<ScatterIntState<true>>>::Register(function);
	} else {
		throw InternalException("Unsupported stochastic SUM state type %s", LogicalType(value_type).ToString());
	}
}

} // namespace duckdb
