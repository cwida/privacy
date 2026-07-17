#include "table_functions/dp_sass_stability_query.hpp"

#include "aggregates/dp_laplace_noise.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/query_result.hpp"

namespace duckdb {

namespace {

const vector<string> STABILITY_FIELD_NAMES = {
    "lane_count", "valid_count", "null_count", "mean",        "stddev_pop",      "cv",          "min", "median",
    "max",        "range",       "mad",        "max_abs_dev", "stable_stddev_1", "stable_mad_1"};

struct DpSassStabilityQueryRow {
	vector<Value> values;
};

struct DpSassStabilityQueryBindData : public TableFunctionData {
	vector<DpSassStabilityQueryRow> rows;

	explicit DpSassStabilityQueryBindData(vector<DpSassStabilityQueryRow> rows_p) : rows(std::move(rows_p)) {
	}
};

struct DpSassStabilityQueryState : public GlobalTableFunctionState {
	idx_t offset = 0;
};

static void AddOutputColumn(vector<LogicalType> &return_types, vector<string> &names, const string &name,
                            const LogicalType &type) {
	names.push_back(name);
	return_types.push_back(type);
}

static void AddStabilityOutputColumns(vector<LogicalType> &return_types, vector<string> &names) {
	AddOutputColumn(return_types, names, "group_row", LogicalType::BIGINT);
	AddOutputColumn(return_types, names, "aggregate_index", LogicalType::INTEGER);
	AddOutputColumn(return_types, names, "aggregate_name", LogicalType::VARCHAR);
	AddOutputColumn(return_types, names, "group_key_json", LogicalType::VARCHAR);
	AddOutputColumn(return_types, names, "lane_count", LogicalType::INTEGER);
	AddOutputColumn(return_types, names, "valid_count", LogicalType::INTEGER);
	AddOutputColumn(return_types, names, "null_count", LogicalType::INTEGER);
	AddOutputColumn(return_types, names, "mean", LogicalType::DOUBLE);
	AddOutputColumn(return_types, names, "stddev_pop", LogicalType::DOUBLE);
	AddOutputColumn(return_types, names, "cv", LogicalType::DOUBLE);
	AddOutputColumn(return_types, names, "min", LogicalType::DOUBLE);
	AddOutputColumn(return_types, names, "median", LogicalType::DOUBLE);
	AddOutputColumn(return_types, names, "max", LogicalType::DOUBLE);
	AddOutputColumn(return_types, names, "range", LogicalType::DOUBLE);
	AddOutputColumn(return_types, names, "mad", LogicalType::DOUBLE);
	AddOutputColumn(return_types, names, "max_abs_dev", LogicalType::DOUBLE);
	AddOutputColumn(return_types, names, "stable_stddev_1", LogicalType::BOOLEAN);
	AddOutputColumn(return_types, names, "stable_mad_1", LogicalType::BOOLEAN);
}

static void AddNoiseScaleOutputColumns(vector<LogicalType> &return_types, vector<string> &names) {
	AddOutputColumn(return_types, names, "group_row", LogicalType::BIGINT);
	AddOutputColumn(return_types, names, "aggregate_index", LogicalType::INTEGER);
	AddOutputColumn(return_types, names, "aggregate_name", LogicalType::VARCHAR);
	AddOutputColumn(return_types, names, "group_key_json", LogicalType::VARCHAR);
	AddOutputColumn(return_types, names, "noise_scale", LogicalType::DOUBLE);
	AddOutputColumn(return_types, names, "numerator_noise_scale", LogicalType::DOUBLE);
	AddOutputColumn(return_types, names, "denominator_noise_scale", LogicalType::DOUBLE);
	AddOutputColumn(return_types, names, "valid_lane_count", LogicalType::INTEGER);
}

static string EscapeJsonString(const string &input) {
	string result;
	result.reserve(input.size() + 8);
	for (auto c : input) {
		switch (c) {
		case '"':
			result += "\\\"";
			break;
		case '\\':
			result += "\\\\";
			break;
		case '\b':
			result += "\\b";
			break;
		case '\f':
			result += "\\f";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		default:
			result += c;
			break;
		}
	}
	return result;
}

static void RunSetting(Connection &conn, const string &sql) {
	auto result = conn.Query(sql);
	if (result->HasError()) {
		throw InvalidInputException("dp_sass_stability_query: failed to apply nested setting: " + result->GetError());
	}
}

static void CopySetting(ClientContext &context, Connection &conn, const string &setting_name) {
	Value value;
	if (!context.TryGetCurrentSetting(setting_name, value) || value.IsNull()) {
		return;
	}
	RunSetting(conn, "SET " + setting_name + " = " + value.ToSQLString());
}

static void ConfigureNestedConnection(ClientContext &context, Connection &conn, bool stability_query_mode) {
	const vector<string> copied_settings = {
	    "dp_epsilon",
	    "dp_delta",
	    "dp_sum_bound",
	    "dp_sum_bounds",
	    "dp_count_bound",
	    "dp_max_groups_contributed",
	    "dp_sass_count_output_bound",
	    "dp_sass_count_output_bounds",
	    "dp_sass_count_output_lower_bounds",
	    "dp_sass_count_output_upper_bounds",
	    "dp_sass_sum_output_bound",
	    "dp_sass_sum_output_bounds",
	    "dp_sass_sum_output_lower_bounds",
	    "dp_sass_sum_output_upper_bounds",
	    "dp_avg_lower_bound",
	    "dp_avg_upper_bound",
	    "dp_avg_lower_bounds",
	    "dp_avg_upper_bounds",
	    "dp_sass_avg_lower_bound",
	    "dp_sass_avg_upper_bound",
	    "dp_sass_minmax_lower_bound",
	    "dp_sass_minmax_upper_bound",
	    "dp_sass_release",
	    "dp_sass_avg_method",
	    "dp_sass_sum_method",
	    "dp_sass_rescale",
	    "dp_sample_lanes",
	    "dp_sass_m",
	    "privacy_min_group_count",
	    "privacy_seed",
	    "priv_check",
	    "priv_rewrite",
	    "priv_join_elimination",
	    "pac_categorical",
	};
	for (auto &setting_name : copied_settings) {
		CopySetting(context, conn, setting_name);
	}
	RunSetting(conn, "SET privacy_mode = 'dp_sass'");
	RunSetting(conn, "SET privacy_noise = false");
	RunSetting(conn, "SET threads = 1");
	if (stability_query_mode) {
		RunSetting(conn, "SET dp_sass_stability_query_mode = true");
	} else {
		RunSetting(conn, "SET dp_sass_noise_scale_query_mode = true");
	}
}

static string BuildGroupKeyJson(QueryResult &result, const vector<Value> &row, idx_t group_cols) {
	if (group_cols == 0) {
		return "{}";
	}
	string json = "{";
	for (idx_t col = 0; col < group_cols; col++) {
		if (col > 0) {
			json += ",";
		}
		json += "\"";
		json += EscapeJsonString(result.names[col]);
		json += "\":";
		auto &value = row[col];
		if (value.IsNull()) {
			json += "null";
		} else {
			json += "\"";
			json += EscapeJsonString(value.ToString());
			json += "\"";
		}
	}
	json += "}";
	return json;
}

static vector<DpSassStabilityQueryRow> ExecuteStabilityQuery(ClientContext &context, const string &sql) {
	auto &db = DatabaseInstance::GetDatabase(context);
	Connection conn(db);
	ConfigureNestedConnection(context, conn, true);
	ClearDpSassStabilityQueryRecords();

	auto result = conn.Query(sql);
	if (result->HasError()) {
		throw InvalidInputException("dp_sass_stability_query: " + result->GetError());
	}
	if (result->ColumnCount() == 0) {
		throw InvalidInputException("dp_sass_stability_query: query returned no columns");
	}

	vector<vector<Value>> result_rows;
	while (true) {
		auto chunk = result->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		for (idx_t row_idx = 0; row_idx < chunk->size(); row_idx++) {
			vector<Value> row;
			row.reserve(result->ColumnCount());
			for (idx_t col = 0; col < result->ColumnCount(); col++) {
				row.push_back(chunk->GetValue(col, row_idx));
			}
			result_rows.push_back(std::move(row));
		}
	}

	auto records = TakeDpSassStabilityQueryRecords();
	if (records.empty()) {
		throw InvalidInputException("dp_sass_stability_query: query produced no SASS aggregate stability records");
	}

	int32_t max_aggregate_index = -1;
	for (auto &record : records) {
		if (record.aggregate_index < 0) {
			throw InternalException("dp_sass_stability_query: negative aggregate index recorded");
		}
		max_aggregate_index = std::max(max_aggregate_index, record.aggregate_index);
	}
	idx_t aggregate_count = static_cast<idx_t>(max_aggregate_index + 1);
	if (aggregate_count == 0) {
		throw InvalidInputException("dp_sass_stability_query: query produced no SASS aggregate stability records");
	}
	bool result_columns_align = aggregate_count <= result->ColumnCount();
	idx_t group_cols = result_columns_align ? result->ColumnCount() - aggregate_count : 0;

	vector<string> group_keys;
	group_keys.reserve(result_rows.size());
	for (auto &row : result_rows) {
		group_keys.push_back(BuildGroupKeyJson(*result, row, group_cols));
	}
	vector<idx_t> next_row_for_aggregate(aggregate_count, 0);
	vector<DpSassStabilityQueryRow> rows;
	for (auto &record : records) {
		auto aggregate_index = static_cast<idx_t>(record.aggregate_index);
		auto group_row = next_row_for_aggregate[aggregate_index]++;
		string group_key_json = group_row < group_keys.size() ? group_keys[group_row] : "{}";

		vector<Value> values;
		values.reserve(4 + STABILITY_FIELD_NAMES.size());
		values.push_back(Value::BIGINT(static_cast<int64_t>(group_row)));
		values.push_back(Value::INTEGER(record.aggregate_index));
		idx_t result_name_idx = group_cols + aggregate_index;
		if (result_columns_align && result_name_idx < result->names.size()) {
			values.push_back(Value(result->names[result_name_idx]));
		} else {
			values.push_back(Value("agg_" + std::to_string(record.aggregate_index)));
		}
		values.push_back(Value(group_key_json));
		for (auto &stat : record.stats) {
			values.push_back(stat);
		}
		rows.push_back({std::move(values)});
	}
	return rows;
}

static vector<DpSassStabilityQueryRow> ExecuteNoiseScaleQuery(ClientContext &context, const string &sql) {
	auto &db = DatabaseInstance::GetDatabase(context);
	Connection conn(db);
	ConfigureNestedConnection(context, conn, false);
	ClearDpSassNoiseScaleQueryRecords();

	auto result = conn.Query(sql);
	if (result->HasError()) {
		throw InvalidInputException("dp_sass_noise_scale_query: " + result->GetError());
	}
	if (result->ColumnCount() == 0) {
		throw InvalidInputException("dp_sass_noise_scale_query: query returned no columns");
	}

	vector<vector<Value>> result_rows;
	while (true) {
		auto chunk = result->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		for (idx_t row_idx = 0; row_idx < chunk->size(); row_idx++) {
			vector<Value> row;
			row.reserve(result->ColumnCount());
			for (idx_t col = 0; col < result->ColumnCount(); col++) {
				row.push_back(chunk->GetValue(col, row_idx));
			}
			result_rows.push_back(std::move(row));
		}
	}

	auto records = TakeDpSassNoiseScaleQueryRecords();
	if (records.empty()) {
		throw InvalidInputException("dp_sass_noise_scale_query: query produced no SASS aggregate noise-scale records");
	}

	int32_t max_aggregate_index = -1;
	for (auto &record : records) {
		if (record.aggregate_index < 0) {
			throw InternalException("dp_sass_noise_scale_query: negative aggregate index recorded");
		}
		max_aggregate_index = std::max(max_aggregate_index, record.aggregate_index);
	}
	idx_t aggregate_count = static_cast<idx_t>(max_aggregate_index + 1);
	if (aggregate_count == 0) {
		throw InvalidInputException("dp_sass_noise_scale_query: query produced no SASS aggregate noise-scale records");
	}
	bool result_columns_align = aggregate_count <= result->ColumnCount();
	idx_t group_cols = result_columns_align ? result->ColumnCount() - aggregate_count : 0;

	vector<string> group_keys;
	group_keys.reserve(result_rows.size());
	for (auto &row : result_rows) {
		group_keys.push_back(BuildGroupKeyJson(*result, row, group_cols));
	}
	vector<idx_t> next_row_for_aggregate(aggregate_count, 0);
	vector<DpSassStabilityQueryRow> rows;
	for (auto &record : records) {
		auto aggregate_index = static_cast<idx_t>(record.aggregate_index);
		auto group_row = next_row_for_aggregate[aggregate_index]++;
		string group_key_json = group_row < group_keys.size() ? group_keys[group_row] : "{}";

		vector<Value> values;
		values.reserve(8);
		values.push_back(Value::BIGINT(static_cast<int64_t>(group_row)));
		values.push_back(Value::INTEGER(record.aggregate_index));
		idx_t result_name_idx = group_cols + aggregate_index;
		if (result_columns_align && result_name_idx < result->names.size()) {
			values.push_back(Value(result->names[result_name_idx]));
		} else {
			values.push_back(Value("agg_" + std::to_string(record.aggregate_index)));
		}
		values.push_back(Value(group_key_json));
		values.push_back(record.noise_scale);
		values.push_back(record.numerator_noise_scale);
		values.push_back(record.denominator_noise_scale);
		values.push_back(record.valid_lane_count);
		rows.push_back({std::move(values)});
	}
	return rows;
}

static unique_ptr<FunctionData> DpSassStabilityQueryBind(ClientContext &context, TableFunctionBindInput &input,
                                                         vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("dp_sass_stability_query requires a non-NULL SQL string");
	}
	AddStabilityOutputColumns(return_types, names);
	auto rows = ExecuteStabilityQuery(context, StringValue::Get(input.inputs[0]));
	return make_uniq<DpSassStabilityQueryBindData>(std::move(rows));
}

static unique_ptr<FunctionData> DpSassNoiseScaleQueryBind(ClientContext &context, TableFunctionBindInput &input,
                                                          vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("dp_sass_noise_scale_query requires a non-NULL SQL string");
	}
	AddNoiseScaleOutputColumns(return_types, names);
	auto rows = ExecuteNoiseScaleQuery(context, StringValue::Get(input.inputs[0]));
	return make_uniq<DpSassStabilityQueryBindData>(std::move(rows));
}

static unique_ptr<GlobalTableFunctionState> DpSassStabilityQueryInit(ClientContext &context,
                                                                     TableFunctionInitInput &input) {
	return make_uniq<DpSassStabilityQueryState>();
}

static void DpSassStabilityQueryFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<DpSassStabilityQueryBindData>();
	auto &state = data_p.global_state->Cast<DpSassStabilityQueryState>();
	idx_t count = 0;
	while (state.offset < bind_data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = bind_data.rows[state.offset++];
		for (idx_t col = 0; col < row.values.size(); col++) {
			output.SetValue(col, count, row.values[col]);
		}
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

void RegisterDpSassStabilityQueryFunction(ExtensionLoader &loader) {
	TableFunction function("dp_sass_stability_query", {LogicalType::VARCHAR}, DpSassStabilityQueryFunction,
	                       DpSassStabilityQueryBind, DpSassStabilityQueryInit);
	loader.RegisterFunction(function);

	TableFunction noise_scale_function("dp_sass_noise_scale_query", {LogicalType::VARCHAR},
	                                   DpSassStabilityQueryFunction, DpSassNoiseScaleQueryBind,
	                                   DpSassStabilityQueryInit);
	loader.RegisterFunction(noise_scale_function);
}

} // namespace duckdb
