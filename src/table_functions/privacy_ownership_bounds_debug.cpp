#include "table_functions/privacy_ownership_bounds_debug.hpp"

#include "compiler/dp_ownership_graph.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

namespace {

struct OwnershipBoundsDebugBindData : public TableFunctionData {
	string fact_table;

	explicit OwnershipBoundsDebugBindData(string fact_table_p) : fact_table(std::move(fact_table_p)) {
	}
};

struct OwnershipBoundsDebugState : public GlobalTableFunctionState {
	DPOwnershipAnalysis analysis;
	idx_t offset = 0;

	explicit OwnershipBoundsDebugState(DPOwnershipAnalysis analysis_p) : analysis(std::move(analysis_p)) {
	}
};

static unique_ptr<FunctionData> OwnershipBoundsDebugBind(ClientContext &, TableFunctionBindInput &input,
                                                         vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("privacy_ownership_bounds_debug requires a non-NULL fact table name");
	}
	names = {"fact_table",
	         "pu_table",
	         "child_table",
	         "local_columns",
	         "parent_table",
	         "referenced_columns",
	         "observed_max_frequency",
	         "child_max_owned_rows",
	         "child_max_owners_per_row",
	         "fact_graph_max_affected_pus",
	         "fact_observed_max_affected_pus"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR),
	                LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR),
	                LogicalType::UBIGINT, LogicalType::UBIGINT,
	                LogicalType::UBIGINT, LogicalType::UBIGINT,
	                LogicalType::UBIGINT};
	return make_uniq<OwnershipBoundsDebugBindData>(StringValue::Get(input.inputs[0]));
}

static unique_ptr<GlobalTableFunctionState> OwnershipBoundsDebugInit(ClientContext &context,
                                                                     TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<OwnershipBoundsDebugBindData>();
	auto analysis = AnalyzeObservedDPOwnershipGraph(context, bind.fact_table);
	return make_uniq<OwnershipBoundsDebugState>(std::move(analysis));
}

static Value StringListValue(const vector<string> &strings) {
	vector<Value> values;
	values.reserve(strings.size());
	for (const auto &value : strings) {
		values.emplace_back(value);
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(values));
}

static void OwnershipBoundsDebugFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<OwnershipBoundsDebugState>();
	idx_t count = 0;
	while (state.offset < state.analysis.edges.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &edge = state.analysis.edges[state.offset++];
		output.SetValue(0, count, Value(state.analysis.fact_table));
		output.SetValue(1, count, Value(state.analysis.pu_table));
		output.SetValue(2, count, Value(edge.child_table));
		output.SetValue(3, count, StringListValue(edge.local_columns));
		output.SetValue(4, count, Value(edge.parent_table));
		output.SetValue(5, count, StringListValue(edge.referenced_columns));
		output.SetValue(6, count, Value::UBIGINT(edge.observed_max_frequency));
		output.SetValue(7, count, Value::UBIGINT(edge.child_max_owned_rows));
		output.SetValue(8, count, Value::UBIGINT(edge.child_max_owners_per_row));
		output.SetValue(9, count, Value::UBIGINT(state.analysis.graph_max_affected_pus));
		output.SetValue(10, count, Value::UBIGINT(state.analysis.observed_max_affected_pus));
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

void RegisterPrivacyOwnershipBoundsDebugFunction(ExtensionLoader &loader) {
	TableFunction function("privacy_ownership_bounds_debug", {LogicalType::VARCHAR}, OwnershipBoundsDebugFunction,
	                       OwnershipBoundsDebugBind, OwnershipBoundsDebugInit);
	loader.RegisterFunction(function);
}

} // namespace duckdb
