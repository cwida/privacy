#include "aggregates/as_sum_state_export.hpp"
#include "aggregates/as_sum.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"

namespace duckdb {
namespace {

// Spill the native counters, not finalized totals: finalization can round and
// applies sampling correction. No process-local pointers enter the exported BLOB.
class SumSpillWriter {
public:
	string data;

	template <class T>
	void Write(const T &value) {
		WriteData(&value, sizeof(value));
	}

	void WriteData(const void *ptr, idx_t size) {
		data.append(reinterpret_cast<const char *>(ptr), size);
	}
};

class SumSpillReader {
public:
	explicit SumSpillReader(const string_t &blob) : ptr(blob.GetData()), remaining(blob.GetSize()) {
	}

	template <class T>
	void Read(T &value) {
		ReadData(&value, sizeof(value));
	}

	void ReadData(void *value, idx_t size) {
		if (remaining < size) {
			throw InvalidInputException("Truncated stochastic SUM state");
		}
		memcpy(value, ptr, size);
		ptr += size;
		remaining -= size;
	}

	bool ReadFlag() {
		uint8_t flag;
		Read(flag);
		if (flag > 1) {
			throw InvalidInputException("Invalid stochastic SUM state flag");
		}
		return flag != 0;
	}

	void Finish() {
		if (remaining) {
			throw InvalidInputException("Unexpected trailing stochastic SUM state data");
		}
	}

private:
	const char *ptr;
	idx_t remaining;
};

template <class STATE>
struct SumStateExport;

template <bool SIGNED>
struct SumStateExport<PacSumIntState<SIGNED>> {
	using State = PacSumIntState<SIGNED>;

	template <class T, idx_t N>
	static void WriteLevel(SumSpillWriter &out, const T *ptr) {
		out.Write(uint8_t(ptr != nullptr));
		if (ptr) {
			out.WriteData(ptr, sizeof(T) * N);
		}
	}

	template <class T, idx_t N>
	static void ReadLevel(SumSpillReader &in, T *&ptr, ArenaAllocator &allocator) {
		if (!in.ReadFlag()) {
			return;
		}
		ptr = reinterpret_cast<T *>(allocator.Allocate(sizeof(T) * N));
		in.ReadData(ptr, sizeof(T) * N);
	}

	static void Write(SumSpillWriter &out, const State &state) {
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

	static void Read(SumSpillReader &in, State &state, ArenaAllocator &allocator) {
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
			if (in.ReadFlag()) {
				// Retain the inline optimization when all pointer slots fit before it.
				if (state.max_level_used < 9) {
					state.AllocateLevel(allocator, k);
				} else {
					state.u.levels[k] =
					    reinterpret_cast<uint64_t *>(allocator.Allocate(sizeof(uint64_t) * PAC_APPROX_SWAR_ELEMENTS));
				}
				in.ReadData(state.u.levels[k], sizeof(uint64_t) * PAC_APPROX_SWAR_ELEMENTS);
			}
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
	static void Write(SumSpillWriter &out, const PacSumDoubleState &state) {
		out.Write(state.key_hash);
		out.Write(state.update_count);
		out.Write(state.probabilistic_total);
	}

	static void Read(SumSpillReader &in, PacSumDoubleState &state, ArenaAllocator &) {
		in.Read(state.key_hash);
		in.Read(state.update_count);
		in.Read(state.probabilistic_total);
	}
};

#ifndef PAC_NOBUFFERING
template <class INNER, class VALUE>
struct SumStateExport<PacSumStateWrapper<INNER, VALUE>> {
	using State = PacSumStateWrapper<INNER, VALUE>;

	static void Write(SumSpillWriter &out, const State &state) {
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

	static void Read(SumSpillReader &in, State &state, ArenaAllocator &allocator) {
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

template <class STATE>
struct SumSpillCallbacks {
	static AggregateStateLayout Layout(AggregateLayoutInput &input) {
		auto layout = AggregateStateLayout(LogicalType::BLOB, sizeof(STATE));
		if (input.function.GetLogicalArguments().size() == 3 && input.bind_data) {
			layout.constant_parameters.emplace(2, Value::DOUBLE(input.bind_data->Cast<PrivBindData>().correction));
		}
		return layout;
	}

	static void Export(Vector &states, AggregateFinalizeInputData &, Vector &result, idx_t count, idx_t offset) {
		UnifiedVectorFormat format;
		states.ToUnifiedFormat(format);
		auto addresses = UnifiedVectorFormat::GetData<STATE *>(format);
		auto writer = FlatVector::Writer<string_t>(result, count, offset);
		for (idx_t i = 0; i < count; i++) {
			SumSpillWriter out;
			SumStateExport<STATE>::Write(out, *addresses[format.sel->get_index(i)]);
			writer.WriteValue(StringVector::AddStringOrBlob(result, out.data));
		}
	}

	static void Import(AggregateImportInputData &input) {
		auto values = input.input_vec.Values<string_t>();
		for (idx_t i = 0; i < input.input_vec.size(); i++) {
			auto value = values[i];
			if (!value.IsValid()) {
				throw InvalidInputException("NULL stochastic SUM state");
			}
			SumSpillReader in(value.GetValue());
			auto &state = *reinterpret_cast<STATE *>(input.dest_buffer + i * input.layout.total_state_size);
			SumStateExport<STATE>::Read(in, state, input.allocator);
			in.Finish();
		}
	}

	static void Register(AggregateFunction &function) {
		function.SetStateExportCallbacks(Layout, Export, Import);
	}
};

} // namespace

void RegisterSumStateExport(AggregateFunction &function, bool double_state) {
	if (double_state) {
		SumSpillCallbacks<ScatterDoubleState>::Register(function);
	} else {
		SumSpillCallbacks<ScatterIntState<true>>::Register(function);
	}
}

} // namespace duckdb
