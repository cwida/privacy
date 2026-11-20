#pragma once

#include "aggregates/as_aggregate.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "privacy_debug.hpp"
#include "utils/privacy_helpers.hpp"

namespace duckdb {

// DuckDB 2.0 creates an aggregate-state spill plan only when every aggregate in a hash table exposes the state
// export callbacks. With a plan, arena-backed states can spill, but aggregation also turns external as soon as the
// arena outgrows half of the per-thread memory reservation, which slows grouped aggregates that fit in memory. So
// the callbacks registered by AggregateStateExport::Register() stay on a bound SUM, COUNT/AVG or MIN/MAX only when
// priv_state_export is set (default off, in every build). Without export the states cannot leave the arena: if the
// arena alone outgrows the per-thread memory reservation, DuckDB 2.0 spills a new, nearly empty block set for every
// input chunk until the temp directory is full. Call this from the bind callback, after the implementation has
// been selected.
inline void ApplyStateExportSetting(ClientContext &ctx, BoundAggregateFunction &function) {
	if (GetBooleanSetting(ctx, "priv_state_export", false)) {
		PRIVACY_DEBUG_PRINT("Aggregate bind: state export enabled (priv_state_export=true)");
		return;
	}
	PRIVACY_DEBUG_PRINT("Aggregate bind: state export disabled (priv_state_export=false)");
	function.GetCallbacks().get_state_type = nullptr;
	function.SetExportAggregateStateCallback(nullptr);
	function.SetImportAggregateStateCallback(nullptr);
}

// Cold-path state transport shared by the stochastic aggregate implementations.
class AggregateStateWriter {
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

class AggregateStateReader {
public:
	explicit AggregateStateReader(const string_t &blob) : ptr(blob.GetData()), remaining(blob.GetSize()) {
	}

	template <class T>
	void Read(T &value) {
		ReadData(&value, sizeof(value));
	}

	void ReadData(void *value, idx_t size) {
		if (remaining < size) {
			throw InvalidInputException("Truncated stochastic aggregate state");
		}
		memcpy(value, ptr, size);
		ptr += size;
		remaining -= size;
	}

	bool ReadFlag() {
		uint8_t flag;
		Read(flag);
		if (flag > 1) {
			throw InvalidInputException("Invalid stochastic aggregate state flag");
		}
		return flag != 0;
	}

	void Finish() {
		if (remaining) {
			throw InvalidInputException("Unexpected trailing stochastic aggregate state data");
		}
	}

private:
	const char *ptr;
	idx_t remaining;
};

template <class STATE, class CODEC>
struct AggregateStateExport {
	static AggregateStateLayout Layout(AggregateLayoutInput &input) {
		auto layout = AggregateStateLayout(LogicalType::BLOB, sizeof(STATE));
		auto &arguments = input.function.GetLogicalArguments();
		if (arguments.size() == 3 && input.bind_data) {
			layout.constant_parameters.emplace(2, Value::DOUBLE(input.bind_data->Cast<PrivBindData>().correction));
		}
		return layout;
	}

	static void Export(Vector &states, AggregateFinalizeInputData &, Vector &result, idx_t count, idx_t offset) {
		UnifiedVectorFormat format;
		states.ToUnifiedFormat(format);
		auto addresses = UnifiedVectorFormat::GetData<STATE *>(format);
		auto writer = FlatVector::Writer<string_t>(result, count, offset);
		AggregateStateWriter out;
		for (idx_t i = 0; i < count; i++) {
			out.data.clear();
			CODEC::Write(out, *addresses[format.sel->get_index(i)]);
			writer.WriteValue(StringVector::AddStringOrBlob(result, out.data));
		}
	}

	static void Import(AggregateImportInputData &input) {
		auto values = input.input_vec.Values<string_t>();
		for (idx_t i = 0; i < input.input_vec.size(); i++) {
			auto value = values[i];
			if (!value.IsValid()) {
				// Keep the caller-initialized empty state. finalize propagates NULL,
				// while combine treats one NULL input as the identity.
				continue;
			}
			AggregateStateReader in(value.GetValue());
			auto &state = *reinterpret_cast<STATE *>(input.dest_buffer + i * input.layout.total_state_size);
			// Other aggregates can leave this shared arena at an odd byte offset.
			// Buffered stochastic states store their count in the pointer's low bits.
			input.allocator.AlignNext();
			CODEC::Read(in, state, input.allocator);
			in.Finish();
		}
	}

	static void Register(BaseAggregateFunction &function) {
		function.GetCallbacks().get_state_type = Layout;
		function.SetExportAggregateStateCallback(Export);
		function.SetImportAggregateStateCallback(Import);
	}
};

} // namespace duckdb
