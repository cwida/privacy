// Performance/coverage follows TPC-H's PAC output semantics: 64 internal worlds and scalar/categorical outputs.
// pac_mi=0 disables additive noise; priv_filter still uses PAC's deterministic majority decision.
// The timed AS path uses handwritten PAC SQL, with automatic compilation recorded separately for coverage.
// Per-world AS references are separate diagnostics, not compiler-equivalence or timing oracles.
#include "duckdb.hpp"
#include "benchmark_json.hpp"
#include "benchmark_paths.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace duckdb;

static string Read(const string &path) {
	std::ifstream file(path);
	if (!file) {
		throw std::runtime_error("Cannot read " + path);
	}
	std::ostringstream out;
	out << file.rdbuf();
	return out.str();
}

static string Quote(const string &value) {
	string out = "'";
	for (auto c : value) {
		out += c == '\'' ? "''" : string(1, c);
	}
	return out + "'";
}

static string Csv(const string &value) {
	string out = "\"";
	for (auto c : value) {
		out += c == '"' ? "\"\"" : string(1, c);
	}
	return out + "\"";
}

static unique_ptr<QueryResult> Query(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	if (!result || result->HasError()) {
		throw std::runtime_error(result ? result->GetError() : "No result");
	}
	return result;
}

static string Name(int query) {
	std::ostringstream name;
	name << 'q' << std::setw(2) << std::setfill('0') << query;
	return name.str();
}

// Separate per-world reference queries expand reduced aggregate results, never base tuples.
using WorldRows = vector<vector<vector<Value>>>;

static void SetHandwrittenAggregates(Connection &con, bool conditional, idx_t first_world, idx_t world_count) {
	for (auto &operation :
	     {string("sum"), string("count"), string("avg"), string("stddev_samp"), string("min"), string("max")}) {
		string expression;
		if (conditional) {
			expression = "[";
			for (idx_t world = 0; world < 64; world++) {
				if (world) {
					expression += ",";
				}
				if (world < first_world || world >= first_world + world_count) {
					expression += "NULL";
				} else {
					expression += operation + "(v) FILTER (WHERE (m & " + std::to_string(uint64_t(1) << world) +
					              "::UBIGINT) <> 0)";
				}
			}
			expression += "]";
		} else if (operation == "stddev_samp") {
			expression = "as_stddev_samp(m, v)";
		} else if (operation == "count") {
			expression = "list_transform(as_count(m, v), lambda x: x / 2.0)";
		} else {
			auto aggregate = operation == "avg" ? "sum" : operation;
			auto value = operation == "avg" ? "x[1] / x[2]" : operation == "sum" ? "x[1] / 2.0" : "x[1]";
			// MIN/MAX dispatch on their SQL input type during binding, including macro definition.
			auto argument = operation == "min" || operation == "max" ? "CAST(v AS DOUBLE)" : "v";
			expression = "list_transform(list_zip(as_" + string(aggregate) + "(m, " + argument +
			             "), as_count(m, v)), lambda x: CASE WHEN x[2] = 0 THEN NULL ELSE " + value + " END)";
		}
		Query(con, "CREATE OR REPLACE TEMP MACRO tpcds_" + operation + "(m, v) AS " + expression);
	}
}

static WorldRows CollectHandwritten(QueryResult &result) {
	WorldRows rows(64);
	for (idx_t r = 0; r < result.RowCount(); r++) {
		auto world = result.Collection().GetValue(0, r).GetValue<int64_t>();
		if (world < 0 || world >= 64) {
			throw std::runtime_error("Invalid handwritten world index");
		}
		vector<Value> row;
		for (idx_t col = 1; col < result.ColumnCount(); col++) {
			row.push_back(result.Collection().GetValue(col, r));
		}
		rows[world].push_back(std::move(row));
	}
	return rows;
}

static void InstallSampledViews(Connection &con) {
	Query(con, "SET VARIABLE tpcds_world=0");
	Query(con, "CREATE SCHEMA IF NOT EXISTS tpcds_sampled");
	Query(con, "CREATE OR REPLACE VIEW tpcds_sampled.customer AS SELECT * FROM main.customer "
	           "WHERE ((priv_hash(hash(c_customer_sk)) >> getvariable('tpcds_world')) & 1) <> 0");
	for (const auto &owner : vector<std::pair<string, string>> {{"store_sales", "ss_customer_sk"},
	                                                            {"store_returns", "sr_customer_sk"},
	                                                            {"catalog_sales", "cs_bill_customer_sk"},
	                                                            {"catalog_returns", "cr_returning_customer_sk"},
	                                                            {"web_sales", "ws_bill_customer_sk"},
	                                                            {"web_returns", "wr_returning_customer_sk"}}) {
		Query(con, "CREATE OR REPLACE VIEW tpcds_sampled." + owner.first + " AS SELECT * FROM main." + owner.first +
		               " WHERE " + owner.second + " IN (SELECT c_customer_sk FROM tpcds_sampled.customer)");
	}
}

struct ValidationComparison {
	idx_t rows = 0;
	idx_t values = 0;
	idx_t exact_mismatches = 0;
	idx_t float_mismatches = 0;
	idx_t empty_worlds = 0;
	double max_absolute_error = 0;
	double max_relative_error = 0;
};

// SQL ORDER BY may leave ties unresolved. Compare result multisets, retaining the SQL's per-world LIMIT.
static vector<idx_t> CanonicalRowOrder(const vector<vector<Value>> &rows, const vector<bool> &keys) {
	vector<idx_t> order;
	vector<string> labels;
	for (idx_t row = 0; row < rows.size(); row++) {
		order.push_back(row);
		string key;
		for (idx_t col = 0; col < keys.size(); col++) {
			if (keys[col]) {
				auto value = rows[row][col].IsNull() ? string("NULL") : rows[row][col].ToString();
				key += std::to_string(value.size()) + ":" + value;
			}
		}
		labels.push_back(std::move(key));
	}
	std::sort(order.begin(), order.end(), [&](idx_t a, idx_t b) {
		if (labels[a] != labels[b]) {
			return labels[a] < labels[b];
		}
		for (idx_t col = 0; col < keys.size(); col++) {
			if (keys[col]) {
				continue;
			}
			auto &x = rows[a][col];
			auto &y = rows[b][col];
			if (x.IsNull() != y.IsNull()) {
				return x.IsNull();
			}
			if (!x.IsNull()) {
				auto xv = x.GetValue<double>(), yv = y.GetValue<double>();
				if (std::isnan(xv) != std::isnan(yv)) {
					return std::isnan(xv);
				}
				if (xv != yv && !(std::isnan(xv) && std::isnan(yv))) {
					return xv < yv;
				}
			}
		}
		return false;
	});
	return order;
}

static ValidationComparison CompareWorldRows(const WorldRows &expected, const WorldRows &actual, idx_t world_count = 64,
                                             bool integral_keys = true) {
	ValidationComparison comparison;
	for (idx_t world = 0; world < world_count; world++) {
		if (expected[world].size() != actual[world].size()) {
			throw std::runtime_error("World " + std::to_string(world) + " row count: expected " +
			                         std::to_string(expected[world].size()) + ", got " +
			                         std::to_string(actual[world].size()));
		}
		comparison.empty_worlds += expected[world].empty();
		comparison.rows += expected[world].size();
		if (expected[world].empty()) {
			continue;
		}
		if (expected[world][0].size() != actual[world][0].size()) {
			throw std::runtime_error("Handwritten output column count differs");
		}
		vector<bool> keys;
		for (idx_t col = 0; col < expected[world][0].size(); col++) {
			auto &e = expected[world][0][col].type();
			auto &a = actual[world][0][col].type();
			keys.push_back(
			    !e.IsNumeric() || !a.IsNumeric() ||
			    (integral_keys && e == a && e.id() != LogicalTypeId::DOUBLE && e.id() != LogicalTypeId::FLOAT));
		}
		auto eo = CanonicalRowOrder(expected[world], keys);
		auto ao = CanonicalRowOrder(actual[world], keys);
		for (idx_t row = 0; row < expected[world].size(); row++) {
			if (expected[world][eo[row]].size() != actual[world][ao[row]].size()) {
				throw std::runtime_error("Handwritten output column count differs");
			}
			for (idx_t col = 0; col < expected[world][row].size(); col++) {
				auto &e = expected[world][eo[row]][col];
				auto &a = actual[world][ao[row]][col];
				auto location = "world=" + std::to_string(world) + ", row=" + std::to_string(row) +
				                ", column=" + std::to_string(col);
				if (e.IsNull() != a.IsNull()) {
					throw std::runtime_error("NULL mismatch: " + location);
				}
				if (e.IsNull()) {
					continue;
				}
				if (!e.type().IsNumeric() || !a.type().IsNumeric()) {
					if (e.ToString() != a.ToString()) {
						throw std::runtime_error("Value/order mismatch: " + location);
					}
					continue;
				}
				double x = e.GetValue<double>(), y = a.GetValue<double>();
				if (x == y || (std::isnan(x) && std::isnan(y))) {
					comparison.values++;
					continue;
				}
				if (!std::isfinite(x) || !std::isfinite(y)) {
					throw std::runtime_error("Non-finite value mismatch: " + location);
				}
				comparison.exact_mismatches++;
				comparison.float_mismatches += static_cast<float>(x) != static_cast<float>(y);
				comparison.values++;
				auto error = std::abs(x - y);
				comparison.max_absolute_error = std::max(comparison.max_absolute_error, error);
				comparison.max_relative_error =
				    std::max(comparison.max_relative_error, error / std::max(1.0, std::abs(x)));
			}
		}
	}
	return comparison;
}

static void ValidateHandwritten(Connection &con, const JsonValue &config, int id) {
	auto name = Name(id);
	auto output = JsonString(config, "out_dir");
	auto directory = JsonString(config, "reference_queries_dir", "benchmark/tpcds/reference_queries");
	auto handwritten = Read(directory + "/" + name + ".sql");
	auto original = Read(JsonString(config, "queries_dir") + "/" + name + ".sql");
	Query(con, "SET priv_rewrite=false");
	InstallSampledViews(con);
	Query(con, "SET search_path='tpcds_sampled,main'");
	WorldRows expected(64);
	for (idx_t world = 0; world < 64; world++) {
		Query(con, "SET VARIABLE tpcds_world=" + std::to_string(world));
		auto result = Query(con, original);
		for (idx_t row = 0; row < result->RowCount(); row++) {
			vector<Value> values;
			for (idx_t col = 0; col < result->ColumnCount(); col++) {
				values.push_back(result->Collection().GetValue(col, row));
			}
			expected[world].push_back(std::move(values));
		}
	}
	std::ofstream csv(output + "/" + name + "_validation.csv");
	csv << "query,method,status,worlds,rows,numeric_values,exact_mismatches,float_rounded_mismatches,"
	       "max_absolute_error,max_relative_error,empty_worlds,error\n";
	for (bool conditional : {true, false}) {
		auto method = conditional ? "conditional_sql" : "simd_asa_handwritten";
		try {
			WorldRows actual(64);
			// Bound the ordinary conditional oracle's state size; this is validation, not timing.
			auto block_size = conditional ? idx_t(8) : idx_t(64);
			for (idx_t first = 0; first < 64; first += block_size) {
				SetHandwrittenAggregates(con, conditional, first, block_size);
				auto result = Query(con, handwritten);
				auto block = CollectHandwritten(*result);
				for (idx_t world = first; world < first + block_size; world++) {
					actual[world] = std::move(block[world]);
				}
			}
			auto comparison = CompareWorldRows(expected, actual);
			auto status = comparison.exact_mismatches     ? "numeric_difference"
			              : comparison.empty_worlds == 64 ? "empty_only"
			                                              : "exact_match";
			csv << name << ',' << method << ',' << status << ",64," << comparison.rows << ',' << comparison.values
			    << ',' << comparison.exact_mismatches << ',' << comparison.float_mismatches << ','
			    << std::setprecision(17) << comparison.max_absolute_error << ',' << comparison.max_relative_error << ','
			    << comparison.empty_worlds << ",\"\"\n";
		} catch (const std::exception &error) {
			csv << name << ',' << method << ",error,64,,,,,,,," << Csv(error.what()) << '\n';
		}
		csv.flush();
	}
	Query(con, "SET search_path='main'");
	Query(con, "DROP SCHEMA tpcds_sampled CASCADE");
}

// TPC-H-style equivalence check: original SQL through the compiler versus explicit PAC SQL.
static void ValidatePacQuery(Connection &con, const JsonValue &config, int id) {
	auto name = Name(id);
	auto output = JsonString(config, "out_dir");
	auto original = Read(JsonString(config, "queries_dir") + "/" + name + ".sql");
	auto handwritten = Read(JsonString(config, "as_queries_dir", "benchmark/tpcds/as_queries") + "/" + name + ".sql");
	std::ofstream csv(output + "/" + name + "_pac_validation.csv");
	csv << "query,status,compiled_rows,handwritten_rows,numeric_values,exact_mismatches,float_rounded_mismatches,"
	       "max_absolute_error,max_relative_error,error\n";
	idx_t compiled_rows = 0, handwritten_rows = 0;
	try {
		WorldRows expected(64), actual(64);
		for (bool automatic : {true, false}) {
			Query(con, "SET priv_rewrite=" + string(automatic ? "true" : "false"));
			auto &sql = automatic ? original : handwritten;
			auto plan = Query(con, "EXPLAIN " + sql);
			std::ofstream plan_file(output + "/" + name + (automatic ? "_compiled.plan.txt" : "_handwritten.plan.txt"));
			for (idx_t row = 0; row < plan->RowCount(); row++) {
				plan_file << plan->Collection().GetValue(1, row).ToString() << '\n';
			}
			auto result = Query(con, sql);
			if (automatic) {
				compiled_rows = result->RowCount();
			} else {
				handwritten_rows = result->RowCount();
			}
			auto &rows = automatic ? expected[0] : actual[0];
			for (idx_t row = 0; row < result->RowCount(); row++) {
				vector<Value> values;
				for (idx_t col = 0; col < result->ColumnCount(); col++) {
					values.push_back(result->Collection().GetValue(col, row));
				}
				rows.push_back(std::move(values));
			}
		}
		auto comparison = CompareWorldRows(expected, actual, 1, false);
		auto status = comparison.exact_mismatches ? "numeric_difference"
		              : comparison.empty_worlds   ? "empty_only"
		                                          : "exact_match";
		csv << name << ',' << status << ',' << compiled_rows << ',' << handwritten_rows << ',' << comparison.values
		    << ',' << comparison.exact_mismatches << ',' << comparison.float_mismatches << ',' << std::setprecision(17)
		    << comparison.max_absolute_error << ',' << comparison.max_relative_error << ",\"\"\n";
	} catch (const std::exception &error) {
		csv << name << ",error," << compiled_rows << ',' << handwritten_rows << ",,,,,," << Csv(error.what()) << '\n';
	}
	Query(con, "SET priv_rewrite=false");
}

// Output-semantics diagnostic only: ordinary world-zero SQL is not an equivalence oracle for PAC output.
// With mi=0, scalar terminals select lane 0, but PAC terminal filters may use majority decisions and SUM/COUNT
// retain their scaling. Differences recorded here must not be interpreted as compiler-correctness failures.
static void ValidateCompiled(Connection &con, const JsonValue &config, int id) {
	auto name = Name(id);
	auto output = JsonString(config, "out_dir");
	auto original = Read(JsonString(config, "queries_dir") + "/" + name + ".sql");
	Query(con, "SET priv_rewrite=false");
	InstallSampledViews(con);
	Query(con, "SET search_path='tpcds_sampled,main'");
	auto reference = Query(con, original);
	WorldRows expected(64), actual(64);
	for (idx_t row = 0; row < reference->RowCount(); row++) {
		vector<Value> values;
		for (idx_t col = 0; col < reference->ColumnCount(); col++) {
			values.push_back(reference->Collection().GetValue(col, row));
		}
		expected[0].push_back(std::move(values));
	}
	Query(con, "SET search_path='main'; SET priv_rewrite=true");
	std::ofstream csv(output + "/" + name + "_compiled_validation.csv");
	csv << "query,status,reference_world,reference_rows,compiled_rows,numeric_values,exact_mismatches,"
	       "float_rounded_mismatches,max_absolute_error,max_relative_error,error\n";
	idx_t compiled_rows = 0;
	bool executed = false;
	try {
		auto explain = Query(con, "EXPLAIN " + original);
		std::ofstream plan(output + "/" + name + "_compiled_validation.plan.txt");
		for (idx_t row = 0; row < explain->RowCount(); row++) {
			plan << explain->Collection().GetValue(1, row).ToString() << '\n';
		}
		auto compiled = Query(con, original);
		compiled_rows = compiled->RowCount();
		executed = true;
		for (idx_t row = 0; row < compiled_rows; row++) {
			vector<Value> values;
			for (idx_t col = 0; col < compiled->ColumnCount(); col++) {
				values.push_back(compiled->Collection().GetValue(col, row));
			}
			actual[0].push_back(std::move(values));
		}
		auto comparison = CompareWorldRows(expected, actual, 1, false);
		auto status = comparison.exact_mismatches ? "numeric_difference"
		              : comparison.empty_worlds   ? "empty_only"
		                                          : "exact_match";
		csv << name << ',' << status << ",0," << reference->RowCount() << ',' << compiled_rows << ','
		    << comparison.values << ',' << comparison.exact_mismatches << ',' << comparison.float_mismatches << ','
		    << std::setprecision(17) << comparison.max_absolute_error << ',' << comparison.max_relative_error
		    << ",\"\"\n";
	} catch (const std::exception &error) {
		csv << name << (executed ? ",result_mismatch,0," : ",execution_error,0,") << reference->RowCount() << ','
		    << compiled_rows << ",,,,,," << Csv(error.what()) << '\n';
	}
	Query(con, "SET priv_rewrite=false; SET search_path='main'");
	Query(con, "DROP SCHEMA tpcds_sampled CASCADE");
}

int main(int argc, char **argv) {
	try {
		if (argc != 3 && !(argc == 4 && (string(argv[3]) == "--validate" || string(argv[3]) == "--validate-worlds" ||
		                                 string(argv[3]) == "--validate-compiled"))) {
			throw std::runtime_error("Usage: as_tpcds_benchmark CONFIG --prepare|QUERY_NUMBER "
			                         "[--validate|--validate-worlds|--validate-compiled]");
		}
		auto config = JsonParser(Read(argv[1])).Parse();
		auto db_path = JsonString(config, "db");
		auto queries = JsonString(config, "queries_dir");
		auto output = JsonString(config, "out_dir");
		auto schema = JsonString(config, "schema", "benchmark/tpcds/schema.sql");
		auto threads = static_cast<int>(JsonNumber(config, "threads", 4));
		auto repetitions = static_cast<int>(JsonNumber(config, "repetitions", 5));
		if (threads < 1 || repetitions < 1 || db_path.empty() || queries.empty() || output.empty()) {
			throw std::runtime_error("Positive threads/repetitions and db/queries_dir/out_dir are required");
		}
		EnsureOutputParentDirectory(db_path);
		EnsureDirectory(queries);
		EnsureDirectory(output);
		DuckDB db(db_path.c_str());
		Connection con(db);
		Query(con, "SET threads=" + std::to_string(threads));
		Query(con, "SET memory_limit=" + Quote(JsonString(config, "memory_limit", "4GB")));
		// privacy_noise=false bypasses PAC compilation; mi=0 disables additive noise without bypassing it.
		Query(con, "SET priv_rewrite=false; SET pac_mi=0; SET privacy_noise=true; SET privacy_seed=42; "
		           "SET pac_sample_diversity_check=false; SET pac_conservative_mode=true; SET priv_check=false;");
		if (string(argv[2]) == "--prepare") {
			auto extension_dir = JsonString(config, "extension_directory");
			if (!extension_dir.empty()) {
				Query(con, "SET extension_directory=" + Quote(extension_dir));
			}
			Query(con, "INSTALL tpcds; LOAD tpcds;");
			auto tables = Query(con, "SELECT count(*) FROM duckdb_tables() WHERE schema_name='main' "
			                         "AND table_name IN ('customer','store_sales','catalog_sales','web_sales')");
			auto present = tables->Collection().GetValue(0, 0).GetValue<int64_t>();
			auto sf_value = JsonNumber(config, "sf", 1);
			if (sf_value <= 0) {
				throw std::runtime_error("Scale factor must be positive");
			}
			if (present == 0) {
				std::ostringstream sf;
				sf << std::setprecision(15) << sf_value;
				Query(con, "CALL dsdgen(sf=" + sf.str() + ")");
			} else if (present != 4) {
				throw std::runtime_error("Incomplete TPC-DS database; use a fresh output directory");
			}
			auto workload = Query(con, "SELECT query_nr, query FROM tpcds_queries() ORDER BY query_nr");
			if (workload->RowCount() != 99) {
				throw std::runtime_error("Expected all 99 TPC-DS queries");
			}
			for (idx_t row = 0; row < workload->RowCount(); row++) {
				auto id = workload->Collection().GetValue(0, row).GetValue<int>();
				auto query_path = queries + "/" + Name(id) + ".sql";
				// Preserve prepared SQL: generator releases can change query semantics.
				if (std::ifstream(query_path).good()) {
					continue;
				}
				std::ofstream file(query_path);
				if (!file) {
					throw std::runtime_error("Cannot export original query");
				}
				file << workload->Collection().GetValue(1, row).ToString();
			}
			std::ofstream metadata(output + "/environment.txt");
			metadata << "DuckDB: " << Query(con, "SELECT version()")->Collection().GetValue(0, 0).ToString() << '\n'
			         << "Config:\n"
			         << Read(argv[1]) << "\nOwnership:\n"
			         << Read(schema);
#ifdef __clang_version__
			metadata << "\nCompiler: " << __clang_version__ << '\n';
#endif
#ifdef PAC_EXACTSUM
			metadata << "SUM: exact cascading\n";
#else
			metadata << "SUM: approximate\n";
#endif
			metadata << "Settings: m=64, privacy_seed=42, pac_mi=0, privacy_noise=true, "
			            "pac_sample_diversity_check=false, pac_conservative_mode=true, priv_check=false\n";
			Query(con, "CHECKPOINT");
			return 0;
		}
		auto id = std::stoi(argv[2]);
		if (id < 1 || id > 99) {
			throw std::runtime_error("TPC-DS query number must be 1..99");
		}
		auto name = Name(id);
		auto sql = Read(queries + "/" + name + ".sql");
		// Execute the reset before parsing declarations: parser validation reads current metadata.
		Query(con, "PRAGMA clear_privacy_metadata");
		// The schema file contains one declaration per line. Parse each against the updated metadata.
		std::istringstream declarations(Read(schema));
		string declaration;
		while (std::getline(declarations, declaration)) {
			if (!declaration.empty()) {
				Query(con, declaration);
			}
		}
		if (argc == 4) {
			if (string(argv[3]) == "--validate-compiled") {
				ValidateCompiled(con, config, id);
				return 0;
			}
			if (string(argv[3]) == "--validate-worlds") {
				ValidateHandwritten(con, config, id);
			} else {
				ValidatePacQuery(con, config, id);
			}
			return 0;
		}
		std::ofstream csv(output + "/" + name + ".csv");
		if (!csv) {
			throw std::runtime_error("Cannot create result CSV");
		}
		csv << "query,mode,phase,status,rewritten,repetition,runtime_ms,rows,world_multiplier,error\n";
		for (auto mode :
		     {string("duckdb"), string("simd_asa_handwritten"), string("simd_asa_auto"), string("naive_as")}) {
			Query(con, "SET priv_rewrite=" + string(mode == "simd_asa_auto" ? "true" : "false"));
			string query_sql = sql;
			if (mode == "simd_asa_handwritten") {
				auto path = JsonString(config, "as_queries_dir", "benchmark/tpcds/as_queries") + "/" + name + ".sql";
				std::ifstream handwritten(path);
				if (!handwritten) {
					csv << name << ',' << mode << ",setup,not_available,0,0,,,,\"\"\n";
					continue;
				}
				query_sql = Read(path);
			}
			auto multiplier = mode == "naive_as" ? 64 : 1;
			bool rewritten = false;
			string phase = "explain";
			try {
				if (mode == "naive_as") {
					// Match TPC-H: prepare once outside timing, time one sampled-world execution,
					// and multiply by 64. Views apply customer sampling at query time to every fact scan.
					InstallSampledViews(con);
					Query(con, "SET search_path='tpcds_sampled,main'");
					Query(con, "PREPARE tpcds_naive AS " + sql);
					query_sql = "EXECUTE tpcds_naive";
				}
				auto explain = Query(con, "EXPLAIN " + query_sql);
				string plan;
				for (idx_t row = 0; row < explain->RowCount(); row++) {
					plan += explain->Collection().GetValue(1, row).ToString() + "\n";
				}
				std::ofstream plan_file(output + "/" + name + "_" + mode + ".plan.txt");
				plan_file << plan;
				rewritten = plan.find("as_noised_") != string::npos || plan.find("as_sum(") != string::npos ||
				            plan.find("as_count(") != string::npos || plan.find("priv_select") != string::npos;
				if ((mode == "simd_asa_auto" || mode == "simd_asa_handwritten") && !rewritten) {
					csv << name << ',' << mode << ",explain,no_stochastic_operator,0,0,,,,\"\"\n";
					csv.flush();
					continue;
				}
				phase = "warmup";
				Query(con, query_sql);
				phase = "measure";
				for (int run = 1; run <= repetitions; run++) {
					auto start = std::chrono::steady_clock::now();
					auto result = Query(con, query_sql);
					auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
					csv << name << ',' << mode << ",measure,ok," << rewritten << ',' << run << ','
					    << std::setprecision(12) << elapsed.count() * multiplier << ',' << result->RowCount() << ','
					    << multiplier << ",\"\"\n";
					csv.flush();
				}
			} catch (const std::exception &error) {
				csv << name << ',' << mode << ',' << phase << ",error," << rewritten << ",0,,,," << Csv(error.what())
				    << '\n';
				csv.flush();
			}
			if (mode == "naive_as") {
				Query(con, "SET search_path='main'");
				Query(con, "DROP SCHEMA IF EXISTS tpcds_sampled CASCADE");
			}
		}
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
