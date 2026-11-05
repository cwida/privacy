#pragma once

#include "aggregates/as_aggregate.hpp"
#include "duckdb/common/vector/string_vector.hpp"

namespace duckdb {

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
		for (idx_t i = 0; i < count; i++) {
			AggregateStateWriter out;
			CODEC::Write(out, *addresses[format.sel->get_index(i)]);
			writer.WriteValue(StringVector::AddStringOrBlob(result, out.data));
		}
	}

	static void Import(AggregateImportInputData &input) {
		auto values = input.input_vec.Values<string_t>();
		for (idx_t i = 0; i < input.input_vec.size(); i++) {
			auto value = values[i];
			if (!value.IsValid()) {
				throw InvalidInputException("NULL stochastic aggregate state");
			}
			AggregateStateReader in(value.GetValue());
			auto &state = *reinterpret_cast<STATE *>(input.dest_buffer + i * input.layout.total_state_size);
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
