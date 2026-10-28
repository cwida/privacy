// WIP SSB benchmark: original and explicitly rewritten joined SQL queries.
// Explicit AS execution and automatic-rewriter coverage are separate measurements.
#include "duckdb.hpp"
#include "../include/as_query_rewriter.hpp"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

using namespace duckdb;
using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point start) {
	return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
static string Read(const string &path) {
	std::ifstream f(path);
	if (!f)
		throw std::runtime_error("Cannot read " + path);
	std::ostringstream out;
	out << f.rdbuf();
	return out.str();
}
static string Quote(const string &s) {
	string out = "'";
	for (char c : s)
		out += c == '\'' ? "''" : string(1, c);
	return out + "'";
}
static string Csv(const string &s) {
	string out = "\"";
	for (char c : s)
		out += c == '"' ? "\"\"" : string(1, c);
	return out + "\"";
}
static unique_ptr<MaterializedQueryResult> Query(Connection &con, const string &sql) {
	auto r = con.Query(sql);
	if (!r || r->HasError())
		throw std::runtime_error(r ? r->GetError() : "No result");
	return r;
}
static void Require(Connection &con, const string &sql, const string &why) {
	if (!Query(con, sql)->GetValue(0, 0).GetValue<bool>())
		throw std::runtime_error(why);
}
static string InputQuery(string sql, int world) {
	// Only the supplied, audited SSB SQL files are accepted, not arbitrary SQL.
	auto pos = sql.find("lineorder");
	if (pos == string::npos || sql.find("lineorder", pos + 9) != string::npos)
		throw std::runtime_error("Expected exactly one lineorder relation");
	// Filter at query time with the same customer-membership expression as AS.
	sql.replace(pos, 9,
	            "(SELECT * FROM lineorder WHERE ((priv_hash(hash(lo_custkey)) >> " + std::to_string(world) +
	                ") & 1) <> 0) AS ssb_input");
	return sql;
}
static string QuerySQL(const string &sql) {
	string result = benchmark::TrimSqlText(sql);
	if (!result.empty() && result.back() == ';')
		result.pop_back();
	return result;
}
using Results = std::map<string, Value>;
enum class Mode { FULL, REPEATED, ASA };
static void Collect(MaterializedQueryResult &r, Results &out, int world, bool is_as, bool grouped) {
	idx_t aggregate = r.ColumnCount();
	for (idx_t col = 0; col < r.ColumnCount(); col++)
		if (r.names[col] == "revenue" || r.names[col] == "profit")
			aggregate = col;
	if (aggregate == r.ColumnCount())
		throw std::runtime_error("Missing aggregate column");
	for (idx_t row = 0; row < r.RowCount(); row++) {
		string key = std::to_string(world) + ":";
		for (idx_t col = 0; col < r.ColumnCount() - (is_as ? 1 : 0); col++) {
			if (col == aggregate)
				continue;
			auto v = r.GetValue(col, row);
			string s = v.IsNull() ? "NULL" : v.type().ToString() + ":" + v.ToString();
			key += std::to_string(s.size()) + ":" + s;
		}
		Value value = r.GetValue(aggregate, row);
		if (is_as) {
			auto pv = r.GetValue(r.ColumnCount() - 1, row);
			bool present = false;
			if (!pv.IsNull()) {
				const auto &presence = ListValue::GetChildren(pv);
				if (presence.size() != 64)
					throw std::runtime_error("Expected 64 presence lanes");
				present = !presence[world].IsNull() && presence[world].GetValue<double>() > 0;
			}
			if (!present && grouped)
				continue;
			if (!present)
				value = Value();
			else {
				const auto &values = ListValue::GetChildren(value);
				if (values.size() != 64)
					throw std::runtime_error("Expected 64 sum lanes");
				// as_sum exposes twice the selected sum; this is not a sampling estimator comparison.
				value = values[world].type().id() == LogicalTypeId::FLOAT
				            ? Value::FLOAT(values[world].GetValue<float>() / 2.0f)
				            : Value::DOUBLE(values[world].GetValue<double>() / 2.0);
			}
		}
		if (!out.emplace(key, value).second)
			throw std::runtime_error("Duplicate world/group key");
	}
}
static Results Run(Connection &con, const string &sql, const string &as_sql, Mode mode, double &elapsed) {
	Results out;
	bool is_as = mode == Mode::ASA;
	bool grouped = sql.find("GROUP BY") != string::npos;
	string aggregate_sql = QuerySQL(is_as ? as_sql : sql);
	elapsed = 0;
	if (mode == Mode::FULL) {
		auto start = Clock::now();
		auto r = Query(con, aggregate_sql);
		elapsed = Ms(start);
		Collect(*r, out, 0, false, grouped);
		return out;
	}
	for (int world = 0; world < (is_as ? 1 : 64); world++) {
		string query_sql = is_as ? aggregate_sql : InputQuery(aggregate_sql, world);
		// Hashing, sampling, joins, planning, and result materialization are timed.
		auto start = Clock::now();
		auto r = Query(con, query_sql);
		// Match TPC-H's extrapolation: time world 0 and scale by 64.
		// Other worlds are executed for correctness only, outside reported time.
		if (world == 0)
			elapsed += Ms(start);
		// C++ validation/normalization is outside timing, equally for both methods.
		if (is_as) {
			for (int lane = 0; lane < 64; lane++)
				Collect(*r, out, lane, true, grouped);
		} else
			Collect(*r, out, world, false, grouped);
	}
	if (mode == Mode::REPEATED)
		elapsed *= 64.0;
	return out;
}
struct Comparison {
	double max_relative_error = 0;
	double total_relative_error = 0;
	double max_absolute_error = 0;
	idx_t compared_values = 0;
	idx_t rounded_mismatches = 0;
};
static Comparison Compare(const Results &expected, const Results &actual, bool require_rounded_match) {
	if (expected.size() != actual.size())
		throw std::runtime_error("World/group cardinality differs");
	Comparison comparison;
	for (const auto &entry : expected) {
		auto found = actual.find(entry.first);
		if (found == actual.end())
			throw std::runtime_error("Missing world/group " + entry.first);
		const auto &a = entry.second, &b = found->second;
		if (a.IsNull() != b.IsNull())
			throw std::runtime_error("NULL mismatch " + entry.first);
		if (a.IsNull())
			continue;
		double x = a.GetValue<double>(), y = b.GetValue<double>();
		// Exact integer cascading rounds only when exposing the FLOAT output lane.
		// Compare with that same representation, rather than an arbitrary error tolerance.
		double rounded = b.type().id() == LogicalTypeId::FLOAT ? static_cast<double>(static_cast<float>(x)) : x;
		double error = std::abs(x - y) / std::max(1.0, std::abs(x));
		if (!std::isfinite(error))
			throw std::runtime_error("Non-finite result " + entry.first);
		if (y != rounded) {
			comparison.rounded_mismatches++;
			if (require_rounded_match)
				throw std::runtime_error("AS result mismatch " + entry.first + " error=" + std::to_string(error) +
				                         "; exact output-precision agreement required");
		}
		comparison.compared_values++;
		comparison.max_relative_error = std::max(comparison.max_relative_error, error);
		comparison.total_relative_error += error;
		comparison.max_absolute_error = std::max(comparison.max_absolute_error, std::abs(x - y));
	}
	return comparison;
}
int main(int argc, char **argv) {
	try {
		if (argc != 7 && !(argc == 8 && string(argv[7]) == "--as-only")) {
			std::cerr << "Usage: as_ssb_benchmark DATA_DIR NEW_DB_PATH SSB_DIR OUT_PREFIX THREADS REPETITIONS "
			             "[--as-only]\n";
			return 2;
		}
		const bool as_only = argc == 8;
		string data = argv[1], db_path = argv[2], ssb = argv[3], prefix = argv[4];
		if (!std::ifstream(data + "/generation_complete").good())
			throw std::runtime_error("Missing generation_complete: do not use partial generator output");
		// prepare.sh writes these only after successful generation, never after a crash.
		std::cerr << "SSB generator commit: " << Read(data + "/generator_commit.txt")
		          << "Scale factor: " << Read(data + "/scale_factor.txt");
		int threads = std::stoi(argv[5]), repetitions = std::stoi(argv[6]);
		if (threads < 1 || repetitions < 1)
			throw std::runtime_error("Positive threads/repetitions required");
		for (const auto &path : {db_path, prefix + "_timings.csv", prefix + "_coverage.csv", prefix + "_plans.txt",
		                         prefix + "_queries.sql"})
			if (std::ifstream(path).good())
				throw std::runtime_error("Refusing overwrite: " + path);
		std::ofstream timings(prefix + "_timings.csv"), coverage(prefix + "_coverage.csv"),
		    plans(prefix + "_plans.txt"), queries(prefix + "_queries.sql");
		if (!timings || !coverage || !plans || !queries)
			throw std::runtime_error("Cannot create output files");
		timings << "query,mode,sum_kernel,membership_mode,threads,memory_limit,run,query_ms,import_ms,world_groups,max_"
		           "relative_error,"
		           "mean_relative_error,max_absolute_error,compared_values,rounded_mismatches,timing_scope\n"
		        << std::setprecision(17);
#ifdef PAC_EXACTSUM
		const char *sum_kernel = "exact";
		const bool exact_sum = true;
#else
		const char *sum_kernel = "approximate";
		const bool exact_sum = false;
#endif
		coverage << "query,rewritten,executed,error\n";
		auto start = Clock::now();
		DuckDB db(db_path.c_str());
		Connection con(db);
		Query(con, "SET threads=" + std::to_string(threads));
		if (const char *memory_limit = std::getenv("SSB_MEMORY_LIMIT")) {
			Query(con, "SET memory_limit=" + Quote(memory_limit));
		}
		string memory_limit = Query(con, "SELECT current_setting('memory_limit')")->GetValue(0, 0).ToString();
		std::cerr << "DuckDB memory limit: " << memory_limit << '\n';
		Query(con, "SET priv_rewrite=false; SET pac_sample_diversity_check=false; SET pac_mi=0; SET privacy_seed=42; "
		           "SET pac_hash_repair=true");
		Query(con, Read(ssb + "/schema.sql"));
		auto manifest = Query(con, "SELECT \"table\", rows::UBIGINT FROM read_csv(" + Quote(data + "/row_counts.csv") +
		                               ", header=true)");
		std::map<string, uint64_t> row_counts;
		for (idx_t row = 0; row < manifest->RowCount(); row++) {
			if (!row_counts
			         .emplace(manifest->GetValue(0, row).ToString(), manifest->GetValue(1, row).GetValue<uint64_t>())
			         .second)
				throw std::runtime_error("Duplicate table in row-count manifest");
		}
		if (row_counts.size() != 5)
			throw std::runtime_error("Expected five manifest tables");
		for (auto table : {"customer", "supplier", "part", "date", "lineorder"}) {
			Query(con, "COPY " + string(table) + " FROM " + Quote(data + "/" + table + ".tbl") +
			               " (FORMAT CSV, DELIMITER '|', HEADER false)");
			if (row_counts.at(table) == 0)
				throw std::runtime_error("Empty input table");
			Require(con, "SELECT count(*)=" + std::to_string(row_counts.at(table)) + " FROM " + table,
			        "Loaded row count differs from generation manifest");
		}
		for (auto spec : {"customer:c_custkey:lo_custkey", "supplier:s_suppkey:lo_suppkey", "part:p_partkey:lo_partkey",
		                  "date:d_datekey:lo_orderdate"}) {
			string s = spec;
			auto a = s.find(':'), b = s.find(':', a + 1);
			string table = s.substr(0, a), key = s.substr(a + 1, b - a - 1), fk = s.substr(b + 1);
			Require(con, "SELECT count(*)=count(DISTINCT " + key + ") FROM " + table, "Duplicate dimension keys");
			Require(con, "SELECT count(*)=0 FROM lineorder ANTI JOIN " + table + " ON " + fk + "=" + key,
			        "Missing dimension keys");
		}
		double import_ms = Ms(start);
		Query(con, "CHECKPOINT");
		// Explicit column protection permits grouping by customer city/nation while retaining key protection.
		Query(con, "ALTER TABLE customer ADD PRIVACY_KEY(c_custkey); ALTER TABLE customer SET PU; "
		           "ALTER TABLE customer ADD PROTECTED(c_custkey); "
		           "ALTER TABLE lineorder ADD PRIVACY_LINK(lo_custkey) REFERENCES customer(c_custkey)");
		for (auto name : {"q11", "q12", "q13", "q21", "q22", "q23", "q31", "q32", "q33", "q34", "q41", "q42", "q43"}) {
			string sql = Read(ssb + "/queries/" + name + ".sql");
			string as_sql = Read(ssb + "/as_queries/" + name + ".sql");
			string aggregate_sql = QuerySQL(sql);
			string as_aggregate_sql = QuerySQL(as_sql);
			queries << "-- " << name << ": ordinary full-data SQL\n"
			        << aggregate_sql << ";\n"
			        << "-- " << name << ": query-time AS rewrite\n"
			        << as_aggregate_sql << ";\n"
			        << "-- " << name << ": independent sampled world 0 (repeat for bits 0--63)\n"
			        << InputQuery(aggregate_sql, 0) << ";\n";
			queries.flush();
			for (const auto &variant : {std::make_pair("full", aggregate_sql), std::make_pair("asa", as_aggregate_sql),
			                            std::make_pair("world0", InputQuery(aggregate_sql, 0))}) {
				auto explain = Query(con, "EXPLAIN " + variant.second);
				plans << "\n--- " << name << " timed " << variant.first << " ---\n";
				for (idx_t row = 0; row < explain->RowCount(); row++) {
					plans << explain->GetValue(1, row).ToString() << '\n';
				}
			}
			bool rewritten = false, executed = false;
			string error;
			Query(con, "SET priv_rewrite=true");
			try {
				auto explain = Query(con, "EXPLAIN " + sql);
				string plan;
				for (idx_t row = 0; row < explain->RowCount(); row++)
					plan += explain->GetValue(1, row).ToString();
				plans << "\n--- " << name << " ---\n" << plan << '\n';
				rewritten = plan.find("as_noised_sum") != string::npos || plan.find("as_sum(") != string::npos;
				if (!rewritten)
					throw std::runtime_error("No private aggregate visible in EXPLAIN");
				Query(con, sql);
				executed = true;
			} catch (const std::exception &e) {
				error = e.what();
			}
			Query(con, "SET priv_rewrite=false");
			coverage << name << ',' << rewritten << ',' << executed << ',' << Csv(error) << '\n';
			coverage.flush();
			double elapsed;
			auto full = Run(con, sql, as_sql, Mode::FULL, elapsed);
			auto expected = Run(con, sql, as_sql, Mode::REPEATED, elapsed); // Warmup and 64-world reference.
			auto warmup = Compare(expected, Run(con, sql, as_sql, Mode::ASA, elapsed), exact_sum);
			const Mode modes[] = {Mode::FULL, Mode::REPEATED, Mode::ASA};
			const char *names[] = {"duckdb_full", "repeated_sql", "asa"};
			for (int run = 0; run < repetitions; run++) {
				// Reuse baseline timings across SUM builds; keep the independent
				// 64-world reference above for correctness even in AS-only runs.
				for (int slot = 0; slot < (as_only ? 1 : 3); slot++) {
					int index = as_only ? 2 : (run + slot) % 3;
					auto actual = Run(con, sql, as_sql, modes[index], elapsed);
					auto comparison = Compare(modes[index] == Mode::FULL ? full : expected, actual,
					                          modes[index] != Mode::ASA || exact_sum);
					double mean =
					    comparison.compared_values ? comparison.total_relative_error / comparison.compared_values : 0;
					timings << name << ',' << names[index] << ',' << sum_kernel << ",query_time_priv_hash," << threads
					        << ',' << Csv(memory_limit) << ',' << run << ',' << elapsed << ',' << import_ms << ','
					        << actual.size() << ',' << comparison.max_relative_error << ',' << mean << ','
					        << comparison.max_absolute_error << ',' << comparison.compared_values << ','
					        << comparison.rounded_mismatches << ','
					        << (modes[index] == Mode::REPEATED ? "world0_x64" : "complete_query") << '\n';
					timings.flush();
				}
			}
			std::cerr << name << " checked " << expected.size() << " world/group results; kernel=" << sum_kernel
			          << "; max_relative_error=" << warmup.max_relative_error
			          << "; rounded_mismatches=" << warmup.rounded_mismatches << "; compiler=" << executed << '\n';
		}
		return 0;
	} catch (const std::exception &e) {
		std::cerr << "SSB benchmark failed: " << e.what() << '\n';
		return 1;
	}
}
