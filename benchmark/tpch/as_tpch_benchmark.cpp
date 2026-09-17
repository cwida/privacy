//
// Created by ila on 12/24/25.
//

// Implement the TPCH benchmark runner.

#include "as_tpch_benchmark.hpp"
#include "as_query_rewriter.hpp"
#include "benchmark_json.hpp"
#include "benchmark_paths.hpp"

#include "duckdb.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/common/printer.hpp"
#include <cstdio>

#include <chrono>
#include <cmath>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <stdexcept>
#include <ctime>
#include <algorithm>
#include <iomanip>
#include <thread>
#include <dirent.h>
#include <map>
#include <set>

namespace duckdb {
static string Timestamp() {
	auto now = std::chrono::system_clock::now();
	std::time_t t = std::chrono::system_clock::to_time_t(now);
	char buf[64];
	std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
	return string(buf);
}

// Format a double without trailing zeros (e.g. 0.100000 -> 0.1, 2.5000 -> 2.5)
static string FormatNumber(double v) {
	std::ostringstream oss;
	// Use high precision to avoid losing significant digits, but we'll trim trailing zeros ourselves
	oss << std::setprecision(15) << std::defaultfloat << v;
	string s = oss.str();
	// If there's a decimal point, trim trailing zeros
	auto pos = s.find('.');
	if (pos != string::npos) {
		// remove trailing zeros
		while (!s.empty() && s.back() == '0') {
			s.pop_back();
		}
		// if decimal point is now last, remove it too
		if (!s.empty() && s.back() == '.') {
			s.pop_back();
		}
	}
	return s;
}

static vector<int> ParseIntListCSV(const string &list) {
	vector<int> values;
	std::stringstream ss(list);
	string tok;
	while (std::getline(ss, tok, ',')) {
		tok.erase(tok.begin(),
		          std::find_if(tok.begin(), tok.end(), [](unsigned char ch) { return !std::isspace(ch); }));
		tok.erase(std::find_if(tok.rbegin(), tok.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(),
		          tok.end());
		if (!tok.empty()) {
			values.push_back(std::stoi(tok));
		}
	}
	return values;
}

static bool IsValidSampleCount(int m) {
	return m >= 64 && m <= 512 && (m & (m - 1)) == 0;
}

static double Median(vector<double> v) {
	std::sort(v.begin(), v.end());
	size_t n = v.size();
	if (n % 2 == 1) {
		return v[n / 2];
	}
	return (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

static void Log(const string &msg) {
	Printer::Print("[" + Timestamp() + "] " + msg);
}

static bool FileExists(const string &path) {
	struct stat buffer;
	return (stat(path.c_str(), &buffer) == 0);
}

static string FindQueryFile(const string &dir, int qnum) {
	// files are named q01.sql .. q22.sql
	char buf[1024];
	snprintf(buf, sizeof(buf), "q%02d.sql", qnum);
	return dir + "/" + string(buf);
}

// Info about a discovered query file
struct QueryEntry {
	string filename;  // e.g. "q08-nolambda.sql"
	string label;     // e.g. "q08-nolambda" (for CSV output)
	int query_number; // e.g. 8 (for PRAGMA tpch baseline)
};

// Scan a directory for q*.sql files, parse query numbers, return sorted
static vector<QueryEntry> DiscoverQueryFiles(const string &dir) {
	vector<QueryEntry> entries;
	DIR *d = opendir(dir.c_str());
	if (!d) {
		return entries;
	}
	struct dirent *ent;
	while ((ent = readdir(d)) != nullptr) {
		string name = ent->d_name;
		if (name.size() < 5 || name[0] != 'q') {
			continue;
		}
		if (name.substr(name.size() - 4) != ".sql") {
			continue;
		}
		// Parse query number from digits after 'q'
		int qnum = 0;
		size_t i = 1;
		while (i < name.size() && name[i] >= '0' && name[i] <= '9') {
			qnum = qnum * 10 + (name[i] - '0');
			i++;
		}
		if (qnum == 0) {
			continue;
		}
		string label = name.substr(0, name.size() - 4);
		entries.push_back({name, label, qnum});
	}
	closedir(d);
	// Sort by query number, then by label length (shorter first, so q08 before q08-nolambda)
	std::sort(entries.begin(), entries.end(), [](const QueryEntry &a, const QueryEntry &b) {
		if (a.query_number != b.query_number) {
			return a.query_number < b.query_number;
		}
		return a.label < b.label;
	});
	return entries;
}

static string ReadFileToString(const string &path) {
	std::ifstream in(path);
	if (!in.is_open()) {
		return string();
	}
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

// WIP benchmark labels distinguish source formulas from manually adapted SQL.
static string VerdictMode(const string &sql) {
	if (sql.find("verdict-mode: exact-fallback") != string::npos) {
		return "VerdictDB-exact-fallback";
	}
	if (sql.find("verdict-mode: customer-adapted") != string::npos) {
		return "VerdictDB-customer-adapted";
	}
	return "VerdictDB-artifact-formula";
}

static string CsvField(const string &value) {
	if (value.find_first_of(",\"\n\r") == string::npos) {
		return value;
	}
	string escaped = "\"";
	for (auto ch : value) {
		if (ch == '\"') {
			escaped += "\"\"";
		} else {
			escaped += ch;
		}
	}
	escaped += "\"";
	return escaped;
}

static string UtilityOutputPath(const string &runtime_path) {
	auto dot = runtime_path.rfind('.');
	if (dot == string::npos) {
		return runtime_path + "_verdictdb_utility.csv";
	}
	return runtime_path.substr(0, dot) + "_verdictdb_utility" + runtime_path.substr(dot);
}

struct QuerySnapshot {
	vector<string> names;
	vector<vector<Value>> rows;
};

static bool TryDouble(const Value &value, double &out);

static bool ExecuteSnapshot(Connection &con, const string &sql, const string &label, QuerySnapshot &snapshot) {
	auto result = con.Query(sql);
	if (!result || result->HasError()) {
		Log(label + " error: " + (result ? result->GetError() : string("null query result")));
		return false;
	}
	snapshot.names = result->names;
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
			snapshot.rows.push_back(std::move(row));
		}
	}
	return true;
}

static bool SnapshotsApproximatelyEqual(const QuerySnapshot &expected, const QuerySnapshot &actual,
	                                    const string &label) {
	if (expected.names != actual.names) {
		Log(label + ": column names differ");
		return false;
	}
	if (expected.rows.size() != actual.rows.size()) {
		Log(label + ": row count differs (SID=" + std::to_string(expected.rows.size()) +
		    ", SIMD=" + std::to_string(actual.rows.size()) + ")");
		return false;
	}
	for (idx_t row = 0; row < expected.rows.size(); row++) {
		for (idx_t col = 0; col < expected.rows[row].size(); col++) {
			auto &lhs = expected.rows[row][col];
			auto &rhs = actual.rows[row][col];
			if (lhs.IsNull() || rhs.IsNull()) {
				if (lhs.IsNull() != rhs.IsNull()) {
					Log(label + ": NULL mismatch at row " + std::to_string(row) + ", column " +
					    expected.names[col]);
					return false;
				}
				continue;
			}
			double lhs_numeric;
			double rhs_numeric;
			if (TryDouble(lhs, lhs_numeric) && TryDouble(rhs, rhs_numeric)) {
				double tolerance = 1e-3 + 1e-4 * std::max(std::abs(lhs_numeric), std::abs(rhs_numeric));
				if (std::abs(lhs_numeric - rhs_numeric) > tolerance) {
					Log(label + ": numeric mismatch at row " + std::to_string(row) + ", column " +
					    expected.names[col] + " (SID=" + FormatNumber(lhs_numeric) +
					    ", SIMD=" + FormatNumber(rhs_numeric) + ")");
					return false;
				}
			} else if (lhs.ToString() != rhs.ToString()) {
				Log(label + ": value mismatch at row " + std::to_string(row) + ", column " +
				    expected.names[col] + " (SID=" + lhs.ToString() + ", SIMD=" + rhs.ToString() + ")");
				return false;
			}
		}
	}
	return true;
}

static bool TryDouble(const Value &value, double &out) {
	if (value.IsNull()) {
		return false;
	}
	try {
		out = value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
		return std::isfinite(out);
	} catch (const std::exception &) {
		return false;
	}
}

static string ResultLookupKey(const vector<Value> &row, idx_t group_columns) {
	if (group_columns == 0) {
		return string();
	}
	string key;
	for (idx_t col = 0; col < group_columns; col++) {
		string value = row[col].IsNull() ? "<NULL>" : row[col].ToString();
		key += std::to_string(value.size()) + ":" + value + "|";
	}
	return key;
}

static string ResultDisplayKey(const vector<Value> &row, idx_t group_columns) {
	if (group_columns == 0) {
		return "{}";
	}
	string key;
	for (idx_t col = 0; col < group_columns; col++) {
		if (col > 0) {
			key += ";";
		}
		key += row[col].IsNull() ? "<NULL>" : row[col].ToString();
	}
	return key;
}

static string NumericField(const Value &value) {
	double numeric;
	return TryDouble(value, numeric) ? FormatNumber(numeric) : string();
}

static void WriteUtilityRow(std::ofstream &csv, int trial, const string &query_label, const string &group_key,
	                        const string &metric, const string &status, const string &estimate,
	                        const string &error_bound, const string &exact, const string &absolute_error,
	                        const string &relative_error, const string &contains_exact) {
	csv << trial << "," << CsvField(query_label) << "," << CsvField(group_key) << "," << CsvField(metric) << ","
	    << status << "," << estimate << "," << error_bound << "," << exact << "," << absolute_error << ","
	    << relative_error << "," << contains_exact << "\n";
}

// WIP rebuttal diagnostic: compare the artifact-style VerdictDB estimate and
// reported 95% half-width with the exact TPC-H answer. Missing groups and NULL
// intervals remain explicit rows so downstream summaries cannot silently drop them.
static void WriteVerdictUtility(int trial, const string &query_label, const QuerySnapshot &estimate,
	                            const QuerySnapshot &exact, std::ofstream &csv) {
	idx_t first_measure = estimate.names.size();
	for (idx_t col = 0; col + 1 < estimate.names.size(); col++) {
		if (estimate.names[col + 1] == estimate.names[col] + "_error") {
			first_measure = col;
			break;
		}
	}
	if (first_measure == estimate.names.size()) {
		throw std::runtime_error("VerdictDB utility " + query_label + ": could not identify estimate/error column pairs");
	}

	vector<pair<idx_t, idx_t>> metric_columns;
	std::map<string, idx_t> exact_columns;
	for (idx_t col = 0; col < exact.names.size(); col++) {
		exact_columns[exact.names[col]] = col;
	}
	for (idx_t col = first_measure; col + 1 < estimate.names.size(); col += 2) {
		const string &metric = estimate.names[col];
		if (estimate.names[col + 1] != metric + "_error") {
			throw std::runtime_error("VerdictDB utility " + query_label + ": malformed estimate/error pair at " + metric);
		}
		auto exact_col = exact_columns.find(metric);
		if (exact_col == exact_columns.end()) {
			throw std::runtime_error("VerdictDB utility " + query_label + ": exact result lacks metric " + metric);
		}
		metric_columns.emplace_back(col, exact_col->second);
	}

	vector<idx_t> exact_group_columns;
	for (idx_t col = 0; col < first_measure; col++) {
		auto found = exact_columns.find(estimate.names[col]);
		if (found == exact_columns.end()) {
			throw std::runtime_error("Exact result lacks grouping column " + estimate.names[col]);
		}
		exact_group_columns.push_back(found->second);
	}
	auto exact_group = [&](const vector<Value> &row) {
		vector<Value> group;
		for (auto col : exact_group_columns) {
			group.push_back(row[col]);
		}
		return group;
	};
	std::map<string, const vector<Value> *> exact_rows;
	for (auto &row : exact.rows) {
		exact_rows[ResultLookupKey(exact_group(row), first_measure)] = &row;
	}
	std::set<string> estimated_keys;

	for (auto &row : estimate.rows) {
		auto lookup_key = ResultLookupKey(row, first_measure);
		auto group_key = ResultDisplayKey(row, first_measure);
		estimated_keys.insert(lookup_key);
		auto exact_row = exact_rows.find(lookup_key);
		if (exact_row == exact_rows.end()) {
			for (auto &columns : metric_columns) {
				auto estimate_col = columns.first;
				WriteUtilityRow(csv, trial, query_label, group_key, estimate.names[estimate_col], "missing_exact",
				                NumericField(row[estimate_col]), NumericField(row[estimate_col + 1]), string(), string(),
				                string(), string());
			}
			continue;
		}
		for (auto &columns : metric_columns) {
			auto col = columns.first;
			auto exact_col = columns.second;
			const string &metric = estimate.names[col];
			double estimate_value;
			double error_bound;
			double exact_value;
			if (!TryDouble((*exact_row->second)[exact_col], exact_value)) {
				WriteUtilityRow(csv, trial, query_label, group_key, metric, "null_exact", NumericField(row[col]),
				                NumericField(row[col + 1]), string(), string(), string(), string());
				continue;
			}
			if (!TryDouble(row[col], estimate_value)) {
				WriteUtilityRow(csv, trial, query_label, group_key, metric, "null_estimate", string(),
				                NumericField(row[col + 1]), FormatNumber(exact_value), string(), string(), string());
				continue;
			}
			double absolute_error = std::abs(estimate_value - exact_value);
			string relative_error = exact_value == 0 ? string() : FormatNumber(absolute_error / std::abs(exact_value));
			if (!TryDouble(row[col + 1], error_bound)) {
				WriteUtilityRow(csv, trial, query_label, group_key, metric, "null_interval",
				                FormatNumber(estimate_value), string(), FormatNumber(exact_value),
				                FormatNumber(absolute_error), relative_error, string());
				continue;
			}
			string status = error_bound == 0.0 ? "zero_width_interval" : "ok";
			WriteUtilityRow(csv, trial, query_label, group_key, metric, status, FormatNumber(estimate_value),
			                FormatNumber(error_bound), FormatNumber(exact_value), FormatNumber(absolute_error),
			                relative_error, absolute_error <= error_bound ? "true" : "false");
		}
	}

	for (auto &exact_entry : exact_rows) {
		if (estimated_keys.find(exact_entry.first) != estimated_keys.end()) {
			continue;
		}
		auto &row = *exact_entry.second;
		auto group_key = ResultDisplayKey(exact_group(row), first_measure);
		for (auto &columns : metric_columns) {
			auto estimate_col = columns.first;
			auto exact_col = columns.second;
			WriteUtilityRow(csv, trial, query_label, group_key, estimate.names[estimate_col], "missing_estimate",
			                string(), string(), NumericField(row[exact_col]), string(), string(), string());
		}
	}
}

struct ASTpchOptions {
	double scale_factor = 10.0;
	string db_path = "tpch.db";
	string queries_dir = "benchmark";
	string out_csv;
	bool run_simple_hash = false;
	bool run_grouped_baselines = false;
	bool skip_naive = false;
	int threads = 8;
	int verdictdb_utility_trials = 20;
	vector<int> as_m_values = {64};
};

static ASTpchOptions LoadASTpchOptions(const string &config_path) {
	auto root = JsonParser(ReadFileToString(config_path)).Parse();
	if (root.type != JsonType::OBJECT) {
		throw std::runtime_error("AS TPC-H config must contain a JSON object");
	}
	ASTpchOptions options;
	options.scale_factor = JsonNumber(root, "sf", options.scale_factor);
	options.db_path = JsonString(root, "db", options.db_path);
	options.queries_dir = JsonString(root, "queries_dir", options.queries_dir);
	options.out_csv = JsonString(root, "out", options.out_csv);
	options.run_simple_hash = JsonBool(root, "run_simple_hash", options.run_simple_hash);
	options.run_grouped_baselines = JsonBool(root, "run_grouped_baselines", options.run_grouped_baselines);
	options.skip_naive = JsonBool(root, "skip_naive", options.skip_naive);
	options.threads = static_cast<int>(JsonNumber(root, "threads", options.threads));
	options.verdictdb_utility_trials =
	    static_cast<int>(JsonNumber(root, "verdictdb_utility_trials", options.verdictdb_utility_trials));
	options.as_m_values = JsonIntList(root, "as_ms", "as_m", options.as_m_values);
	return options;
}

static string RewriteAsFunctionCall(const string &name, const vector<string> &args, int sample_count) {
	(void)sample_count;
	if (name == "as_noised") {
		if (args.size() != 1) {
			throw std::runtime_error("as_noised rewrite expected one argument");
		}
		return "list_avg(" + args[0] + ")";
	}
	if (name == "as_noised_count") {
		if (args.empty()) {
			throw std::runtime_error("as_noised_count rewrite expected at least one argument");
		}
		if (args.size() > 2) {
			throw std::runtime_error("as_noised_count variable-m rewrite supports at most two arguments");
		}
		std::ostringstream out;
		out << "list_avg(as_pac_m_count(";
		for (idx_t i = 0; i < args.size(); i++) {
			if (i > 0) {
				out << ", ";
			}
			out << args[i];
		}
		out << "))";
		return out.str();
	}
	if (name == "as_noised_sum") {
		if (args.size() != 2) {
			throw std::runtime_error("as_noised_sum rewrite expected two arguments");
		}
		return "list_avg(as_pac_m_sum(" + args[0] + ", CAST(" + args[1] + " AS DOUBLE)))";
	}
	if (name == "as_noised_avg") {
		if (args.size() != 2) {
			throw std::runtime_error("as_noised_avg rewrite expected two arguments");
		}
		return "list_avg(as_pac_m_avg(" + args[0] + ", CAST(" + args[1] + " AS DOUBLE), 1.0))";
	}
	if (name == "as_noised_min" || name == "as_noised_max") {
		throw std::runtime_error("variable-m PAC AS rewrite does not support MIN/MAX yet");
	}
	if (name == "as_sum") {
		if (args.size() != 2) {
			throw std::runtime_error("as_sum rewrite expected two arguments");
		}
		return "as_pac_m_sum(" + args[0] + ", CAST(" + args[1] + " AS DOUBLE))";
	}
	if (name == "as_count") {
		if (args.empty()) {
			throw std::runtime_error("as_count rewrite expected at least one argument");
		}
		if (args.size() > 2) {
			throw std::runtime_error("as_count variable-m rewrite supports at most two arguments");
		}
		std::ostringstream out;
		out << "as_pac_m_count(";
		for (idx_t i = 0; i < args.size(); i++) {
			if (i > 0) {
				out << ", ";
			}
			out << args[i];
		}
		out << ")";
		return out.str();
	}
	if (name == "as_min" || name == "as_max") {
		throw std::runtime_error("variable-m PAC AS rewrite does not support MIN/MAX yet");
	}
	if (name == "as_noised_div") {
		if (args.size() != 2) {
			throw std::runtime_error("as_noised_div rewrite expected two arguments");
		}
		vector<string> sum_args;
		vector<string> count_args;
		if (!benchmark::ParseEntireFunctionCall(args[0], "as_sum", sum_args) ||
		    !benchmark::ParseEntireFunctionCall(args[1], "as_count", count_args) || sum_args.size() != 2 ||
		    count_args.size() < 1 || benchmark::TrimSqlText(sum_args[0]) != benchmark::TrimSqlText(count_args[0])) {
			throw std::runtime_error("variable-m AS rewrite only supports as_noised_div(as_sum(key, value), "
			                         "as_count(key, value)) AVG patterns");
		}
		return "list_avg(as_pac_m_avg(" + sum_args[0] + ", CAST(" + sum_args[1] + " AS DOUBLE), 1.0))";
	}
	throw std::runtime_error("unsupported AS aggregate rewrite: " + name);
}

static string RewriteAsQueryForSampleAggregation(const string &sql, int sample_count) {
	static const vector<string> names = {"as_noised_count", "as_noised_sum", "as_noised_avg", "as_noised_min",
	                                     "as_noised_max",   "as_noised_div", "as_noised",     "as_sum",
	                                     "as_count",        "as_min",        "as_max"};
	static const vector<string> unsupported = {"as_noised", "as_sum", "as_count", "as_min", "as_max"};
	benchmark::AsQueryRewriteOptions options {names, unsupported, true, true, "as_noised_div", true};
	return benchmark::RewriteAsQuery(sql, sample_count, options, RewriteAsFunctionCall);
}

static bool TryRewriteSelectedTpchQuery(const string &label, int sample_count, string &rewritten_sql) {
	if (label == "q17") {
		rewritten_sql = R"(SELECT list_avg(as_pac_m_sum_if(
                 hash(o_custkey),
                 CAST(l_extendedprice AS DOUBLE),
                 list_transform(
                   (SELECT priv_div(as_pac_m_sum(hash(o_sub.o_custkey), CAST(l_sub.l_quantity AS DOUBLE)),
                                    as_pac_m_count(hash(o_sub.o_custkey), l_sub.l_quantity))
                      FROM lineitem AS l_sub JOIN orders AS o_sub ON l_sub.l_orderkey = o_sub.o_orderkey
                     WHERE l_sub.l_partkey = part.p_partkey),
                   lambda x: CAST(lineitem.l_quantity * 5 < x AS BOOLEAN)))) / 7.0 AS avg_yearly
  FROM lineitem JOIN part ON lineitem.l_partkey = part.p_partkey JOIN orders ON lineitem.l_orderkey = orders.o_orderkey
 WHERE part.p_brand = 'Brand#23' AND part.p_container = 'MED BOX')";
		return true;
	}
	if (label == "q22") {
		(void)sample_count;
		rewritten_sql = R"(SELECT cntrycode,
       list_avg(as_pac_m_count_if(as_hash, as_pred)) AS numcust,
       list_avg(as_pac_m_sum_if(as_hash, CAST(c_acctbal AS DOUBLE), as_pred)) AS totacctbal
FROM (SELECT substring(c_phone FROM 1 FOR 2) AS cntrycode,
             c_acctbal,
             hash(c_custkey) AS as_hash,
             list_transform(
               (SELECT priv_div(as_pac_m_sum(hash(c_custkey), CAST(c_acctbal AS DOUBLE)),
                                as_pac_m_count(hash(c_custkey), c_acctbal))
                  FROM customer
                 WHERE c_acctbal > 0.00
                   AND substring(c_phone FROM 1 FOR 2) IN ('13', '31', '23', '29', '30', '18', '17')),
               lambda x: CAST(c_acctbal > x AS BOOLEAN)) AS as_pred
        FROM customer
       WHERE substring(c_phone FROM 1 FOR 2) IN ('13', '31', '23', '29', '30', '18', '17')
         AND NOT EXISTS (FROM orders WHERE o_custkey = customer.c_custkey)) AS custsale
WHERE list_bool_or(as_pred)
GROUP BY ALL
ORDER BY ALL)";
		return true;
	}
	return false;
}

static void CreateLineitemEnhanced(Connection &con) {
	Log("Creating lineitem_enhanced...");
	{
		auto chk = con.Query("SELECT 1 FROM lineitem_enhanced LIMIT 1;");
		if (chk && !chk->HasError()) {
			Log("lineitem_enhanced already exists, skipping.");
		} else {
			auto r3 = con.Query("CREATE TABLE IF NOT EXISTS lineitem_enhanced AS "
			                    "SELECT l.l_orderkey, l.l_suppkey, l.l_linenumber,"
			                    "  c.rowid AS c_rowid, s.s_name AS s_name,"
			                    "  (l.l_receiptdate > l.l_commitdate) AS is_late,"
			                    "  (o.o_orderstatus = 'F') AS is_orderstatus_f,"
			                    "  (n.n_name = 'SAUDI ARABIA') AS is_nation_saudi_arabia "
			                    "FROM lineitem l "
			                    "JOIN orders o ON o.o_orderkey = l.l_orderkey "
			                    "JOIN customer c ON c.c_custkey = o.o_custkey "
			                    "JOIN supplier s ON s.s_suppkey = l.l_suppkey "
			                    "JOIN nation n ON s.s_nationkey = n.n_nationkey "
			                    "ORDER BY l.l_orderkey, l.l_linenumber;");
			if (r3 && r3->HasError()) {
				Log(string("lineitem_enhanced creation error: ") + r3->GetError());
			} else {
				Log("lineitem_enhanced ready.");
			}
		}
	}

	auto r4 = con.Query("CREATE INDEX IF NOT EXISTS idx_lineitem_enhanced_order_supp "
	                    "ON lineitem_enhanced(l_orderkey, l_suppkey);");
	if (r4 && r4->HasError()) {
		Log(string("idx_lineitem_enhanced_order_supp creation error: ") + r4->GetError());
	}
}

// Create all Naive-AS sampling tables once (customer-based, orders-based, q21 extras)
// Uses IF NOT EXISTS so tables persist across runs on existing databases.
static void NaiveASCreateTables(Connection &con) {
	Log("Creating random_samples (customer-based, batched)...");
	{
		auto chk = con.Query("SELECT 1 FROM random_samples LIMIT 1;");
		if (chk && !chk->HasError()) {
			Log("random_samples already exists, skipping.");
		} else {
			con.Query("CREATE TABLE IF NOT EXISTS random_samples ("
			          "sample_id INTEGER, row_id BIGINT, random_binary BOOLEAN);");
			bool batch_failed = false;
			for (int sid = 0; sid < 64; ++sid) {
				auto rb = con.Query("INSERT INTO random_samples "
				                    "SELECT " +
				                    std::to_string(sid) +
				                    " AS sample_id, "
				                    "customer.rowid AS row_id, "
				                    "(RANDOM() > 0.5)::BOOLEAN AS random_binary "
				                    "FROM customer;");
				if (rb && rb->HasError()) {
					Log("random_samples batch " + std::to_string(sid) + " error: " + rb->GetError());
					batch_failed = true;
					break;
				}
				if (sid % 16 == 15) {
					Log("  random_samples: " + std::to_string(sid + 1) + "/64 samples inserted.");
				}
			}
			if (!batch_failed) {
				Log("random_samples ready.");
			}
		}
	}

	Log("Creating random_samples_orders (orders-based, batched)...");
	{
		// Check if table already exists
		auto chk = con.Query("SELECT 1 FROM random_samples_orders LIMIT 1;");
		if (chk && !chk->HasError()) {
			Log("random_samples_orders already exists, skipping.");
		} else {
			// Build in batches of one sample_id at a time to avoid OOM on large SF
			con.Query("CREATE TABLE IF NOT EXISTS random_samples_orders ("
			          "sample_id INTEGER, row_id BIGINT, random_binary BOOLEAN);");
			bool batch_failed = false;
			for (int sid = 0; sid < 64; ++sid) {
				auto rb = con.Query("INSERT INTO random_samples_orders "
				                    "SELECT " +
				                    std::to_string(sid) +
				                    " AS sample_id, "
				                    "orders.rowid AS row_id, "
				                    "(RANDOM() > 0.5)::BOOLEAN AS random_binary "
				                    "FROM orders;");
				if (rb && rb->HasError()) {
					Log("random_samples_orders batch " + std::to_string(sid) + " error: " + rb->GetError());
					batch_failed = true;
					break;
				}
				if (sid % 16 == 15) {
					Log("  random_samples_orders: " + std::to_string(sid + 1) + "/64 samples inserted.");
				}
			}
			if (!batch_failed) {
				Log("random_samples_orders ready.");
			}
		}
	}

	CreateLineitemEnhanced(con);
}

// WIP rebuttal experiment: these representation baselines still require full semantic
// validation and reruns on the paper hardware before publication.
static bool ConfigureSidWorldViews(Connection &con, bool overlapping) {
	vector<string> statements;
	if (overlapping) {
		statements = {"CREATE OR REPLACE TEMP VIEW benchmark_customer_worlds AS "
		              "SELECT c.*, samples.sample_id FROM customer c, range(64) AS samples(sample_id) "
		              "WHERE (priv_hash(hash(c.c_custkey)) & (CAST(1 AS UBIGINT) << samples.sample_id)) <> 0",
		              "CREATE OR REPLACE TEMP VIEW benchmark_order_worlds AS "
		              "SELECT o.*, samples.sample_id FROM orders o, range(64) AS samples(sample_id) "
		              "WHERE (priv_hash(hash(o.o_custkey)) & (CAST(1 AS UBIGINT) << samples.sample_id)) <> 0",
		              "CREATE OR REPLACE TEMP VIEW benchmark_lineitem_worlds AS "
		              "SELECT l.*, samples.sample_id FROM lineitem_enhanced l JOIN customer c ON c.rowid = l.c_rowid, "
		              "range(64) AS samples(sample_id) "
		              "WHERE (priv_hash(hash(c.c_custkey)) & (CAST(1 AS UBIGINT) << samples.sample_id)) <> 0"};
	} else {
		statements = {"CREATE OR REPLACE TEMP VIEW benchmark_customer_worlds AS "
		              "SELECT customer.*, CAST(hash(c_custkey) % 64 AS INTEGER) AS sample_id FROM customer",
		              "CREATE OR REPLACE TEMP VIEW benchmark_order_worlds AS "
		              "SELECT orders.*, CAST(hash(o_custkey) % 64 AS INTEGER) AS sample_id FROM orders",
		              "CREATE OR REPLACE TEMP VIEW benchmark_lineitem_worlds AS "
		              "SELECT l.*, CAST(hash(c.c_custkey) % 64 AS INTEGER) AS sample_id "
		              "FROM lineitem_enhanced l JOIN customer c ON c.rowid = l.c_rowid"};
	}
	for (auto &statement : statements) {
		auto result = con.Query(statement);
		if (result && result->HasError()) {
			Log("SID baseline view setup error: " + result->GetError());
			return false;
		}
	}
	return true;
}

// WIP rebuttal experiment: apply VerdictDB's stored-sample layout and SIGMOD'18
// rewrite to the same customer-level sampling unit as SIMD-ASA. Sample creation is
// offline and deliberately excluded from query timings. VerdictDB defaults to a 1%
// offline sample and 100 partitions. Domain-separated MD5 inputs provide reproducible
// pseudorandom assignments. Do not use hash(key, salt): DuckDB's final XOR preserves
// low-bit correlations between inclusion, partition assignment, and trials.
static bool ConfigureVerdictDBSample(Connection &con, int trial, bool log_statistics = true) {
	string sample_salt = "verdict_sample_" + std::to_string(trial);
	string partition_salt = "verdict_partition_" + std::to_string(trial);
	auto result = con.Query("CREATE OR REPLACE TEMP TABLE benchmark_verdict_customer_sample AS "
	                        "SELECT c.* FROM customer c "
	                        "WHERE md5_number_lower(concat('" +
	                        sample_salt + ":', CAST(c_custkey AS VARCHAR))) % 100 = 0;");
	if (!result || result->HasError()) {
		Log("VerdictDB customer offline sample setup error: " +
		    (result ? result->GetError() : string("null query result")));
		return false;
	}
	result = con.Query("CREATE OR REPLACE TEMP TABLE benchmark_verdict_customer AS "
	                   "SELECT c.*, "
	                   "       CAST(md5_number_lower(concat('" +
	                   partition_salt + ":', CAST(c_custkey AS VARCHAR))) % 100 AS SMALLINT) AS verdict_vpart, "
	                   "       CAST((SELECT count(*) FROM benchmark_verdict_customer_sample) AS DOUBLE) "
	                   "         / CAST((SELECT count(*) FROM customer) AS DOUBLE) AS verdict_vprob "
	                   "FROM benchmark_verdict_customer_sample c;");
	if (!result || result->HasError()) {
		Log("VerdictDB customer sample setup error: " +
		    (result ? result->GetError() : string("null query result")));
		return false;
	}
	if (log_statistics) {
		auto stats = con.Query("SELECT count(*), min(verdict_vprob), count(DISTINCT verdict_vpart) "
		                       "FROM benchmark_verdict_customer;");
		if (stats && !stats->HasError() && stats->RowCount() == 1) {
			Log("VerdictDB customer sample trial " + std::to_string(trial) + " ready (" +
			    stats->GetValue(0, 0).ToString() + " customers, realized probability " +
			    stats->GetValue(1, 0).ToString() + ", " + stats->GetValue(2, 0).ToString() +
			    " occupied of 100 variational partitions).");
		} else {
			Log("VerdictDB customer sample trial " + std::to_string(trial) +
			    " ready (1% uniform sample, 100 variational partitions).");
		}
	}
	return true;
}

static double BenchmarkSqlQuery(Connection &con, const string &sql, const string &label) {
	auto warmup = con.Query(sql);
	if (warmup && warmup->HasError()) {
		Log(label + " warmup error: " + warmup->GetError());
		return -1;
	}
	vector<double> times_ms;
	for (int run = 1; run <= 5; ++run) {
		auto start = std::chrono::steady_clock::now();
		auto result = con.Query(sql);
		auto end = std::chrono::steady_clock::now();
		if (result && result->HasError()) {
			Log(label + " run " + std::to_string(run) + " error: " + result->GetError());
			return -1;
		}
		double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
		times_ms.push_back(elapsed_ms);
		Log(label + " run " + std::to_string(run) + " time (ms): " + FormatNumber(elapsed_ms));
	}
	return Median(times_ms);
}

int RunASTPCHBenchmark(const string &db_path, const string &queries_dir, double sf, const string &out_csv,
                       bool run_simple_hash, int threads, const vector<int> &as_m_values, bool skip_naive,
                       bool run_grouped_baselines, int verdictdb_utility_trials) {
	try {
		Log(string("run_simple_hash flag: ") + (run_simple_hash ? string("true") : string("false")));
		Log(string("run_grouped_baselines flag: ") + (run_grouped_baselines ? string("true") : string("false")));
		Log(string("skip_naive flag: ") + (skip_naive ? string("true") : string("false")));
		Log("VerdictDB utility trials: " + std::to_string(verdictdb_utility_trials));
		string actual_out = out_csv;
		if (actual_out.empty()) {
			string sf_token = FormatNumber(sf);
			sf_token.erase(std::remove(sf_token.begin(), sf_token.end(), '.'), sf_token.end());
			sf_token.erase(std::remove(sf_token.begin(), sf_token.end(), '_'), sf_token.end());
			char ofn[256];
			snprintf(ofn, sizeof(ofn), "benchmark/tpch/tpch_benchmark_results_sf%s.csv", sf_token.c_str());
			actual_out = string(ofn);
		}
		EnsureOutputParentDirectory(actual_out);

		// Open (file-backed) DuckDB database
		// Decide whether the caller explicitly provided a DB path (not the default) so we can
		// decide whether to warn and/or re-create tables in an existing DB.
		bool user_provided_db = !(db_path.empty() || db_path == "tpch.db");
		// Compute actual DB filename: use user-provided path, otherwise derive from scale factor
		string db_actual = db_path;
		if (!user_provided_db) {
			string sf_token = FormatNumber(sf);
			sf_token.erase(std::remove(sf_token.begin(), sf_token.end(), '.'), sf_token.end());
			sf_token.erase(std::remove(sf_token.begin(), sf_token.end(), '_'), sf_token.end());
			char dbfn[256];
			snprintf(dbfn, sizeof(dbfn), "tpch_sf%s.db", sf_token.c_str());
			db_actual = string(dbfn);
		}
		bool db_exists = FileExists(db_actual);
		if (db_exists) {
			Log(string("Connecting to existing DuckDB database: ") + db_actual +
			    string(" (skipping data generation)."));
		} else {
			Log(string("Will create/populate DuckDB database: ") + db_actual);
		}
		DuckDB db(db_actual.c_str());
		Connection con(db);

		// Set thread count
		Log("Setting threads to " + std::to_string(threads));
		con.Query("SET threads TO " + std::to_string(threads) + ";");
		con.Query("SET pac_sample_diversity_check=false;");
		con.Query("SET priv_rewrite=false;");

		// Enable spilling to disk when memory is insufficient
		auto r_temp = con.Query("SET temp_directory='/tmp/duckdb_temp';");
		if (r_temp && r_temp->HasError()) {
			Log(string("SET temp_directory error: ") + r_temp->GetError());
		}

		Log("Installing TPCH extension...");
		auto r_install = con.Query("INSTALL tpch;");
		if (r_install && r_install->HasError()) {
			Log(string("INSTALL tpch error: ") + r_install->GetError());
		}
		auto r_load = con.Query("LOAD tpch;");
		if (r_load && r_load->HasError()) {
			Log(string("LOAD tpch error: ") + r_load->GetError());
		}

		Log("Generating TPCH data (sf=" + FormatNumber(sf) + ")... this may take a while");
		// call dbgen with requested scale factor (may be fractional):
		// - If the DB file doesn't exist -> create it by calling dbgen
		// - If the DB file exists and the user explicitly provided a path -> warn and re-run dbgen (may recreate
		// tables)
		// - If the DB file exists and the DB was derived from sf (user didn't provide a path) -> skip dbgen
		if (!db_exists) {
			char callbuf[128];
			snprintf(callbuf, sizeof(callbuf), "CALL dbgen(sf=%g);", sf);
			auto r_dbgen = con.Query(callbuf);
			if (r_dbgen && r_dbgen->HasError()) {
				Log(string("CALL dbgen error: ") + r_dbgen->GetError());
			}
		} else {
			Log(string("Skipping CALL dbgen since database file already exists: ") + db_actual);
		}

		// Locate AS query directories
		string bitslice_dir = queries_dir + string("/tpch/tpch_as_queries");
		string simple_hash_dir = queries_dir + string("/tpch/tpch_as_simple_hash_queries");
		string naive_as_dir = queries_dir + string("/tpch/tpch_naive_as_queries");
		string sid_queries_dir = queries_dir + string("/tpch/tpch_sid_queries");
		string matched_simd_dir = queries_dir + string("/tpch/tpch_matched_simd_queries");
		string verdictdb_dir = queries_dir + string("/tpch/tpch_verdictdb_queries");

		// Discover all .sql files in the bitslice directory
		auto query_entries = DiscoverQueryFiles(bitslice_dir);
		if (query_entries.empty()) {
			throw std::runtime_error("No query files found in " + bitslice_dir);
		}
		Log("Discovered " + std::to_string(query_entries.size()) + " query files in " + bitslice_dir);
		for (auto &e : query_entries) {
			Log("  " + e.label + " (baseline Q" + std::to_string(e.query_number) + ")");
		}
		std::ofstream csv(actual_out, std::ofstream::out | std::ofstream::trunc);
		if (!csv.is_open()) {
			throw std::runtime_error("Failed to open output CSV: " + actual_out);
		}
		csv << "query,mode,m,median_ms\n";
		string utility_out = UtilityOutputPath(actual_out);
		std::ofstream utility_csv;
		if (run_grouped_baselines) {
			EnsureOutputParentDirectory(utility_out);
			utility_csv.open(utility_out, std::ofstream::out | std::ofstream::trunc);
			if (!utility_csv.is_open()) {
				throw std::runtime_error("Failed to open VerdictDB utility CSV: " + utility_out);
			}
			utility_csv << "trial,query,group_key,metric,status,estimate,error_bound_95,exact,absolute_error,"
			               "relative_error,contains_exact\n";
		}

		// Cache baseline median per query number (avoid re-running for variants like q08-nolambda)
		std::map<int, double> baseline_cache;

		// Only Naive-AS uses materialized Bernoulli membership tables. Expanded-Worlds derives
		// the same membership bits as SIMD-AS directly from priv_hash at query time.
		if (!skip_naive) {
			Log("Creating Naive-AS sampling tables (if not already present)...");
			NaiveASCreateTables(con);
			Log("Naive-AS sampling tables ready.");
		} else if (run_grouped_baselines) {
			CreateLineitemEnhanced(con);
		}
		bool verdictdb_ready = false;
		if (run_grouped_baselines) {
			verdictdb_ready = ConfigureVerdictDBSample(con, 0);
		}

		for (auto &entry : query_entries) {
			Log("=== " + entry.label + " (Q" + std::to_string(entry.query_number) + ") ===");

			// Run baseline (PRAGMA tpch) if not already cached for this query number
			if (baseline_cache.find(entry.query_number) == baseline_cache.end()) {
				double baseline_median = -1;
				try {
					string pragma = "PRAGMA tpch(" + std::to_string(entry.query_number) + ");";

					// Cold run (do not time)
					{
						auto r_cold = con.Query(pragma);
						if (r_cold && r_cold->HasError()) {
							Log(string("Cold PRAGMA tpch(") + std::to_string(entry.query_number) +
							    ") error: " + r_cold->GetError());
						}
						Log("Cold run completed for TPCH query " + std::to_string(entry.query_number));
					}

					// 1st warm run (do not time) to warm caches
					{
						auto r_warm_init = con.Query(pragma);
						if (r_warm_init && r_warm_init->HasError()) {
							Log(string("Warm (init) PRAGMA tpch(") + std::to_string(entry.query_number) +
							    ") error: " + r_warm_init->GetError());
						}
						Log("Warm (init) run completed for TPCH query " + std::to_string(entry.query_number));
					}

					// Wait 0.5s before timed hot runs
					std::this_thread::sleep_for(std::chrono::milliseconds(500));

					// Hot runs: 5 runs, take median
					vector<double> tpch_times_ms;
					bool hot_run_failed = false;
					for (int t = 1; t <= 5; ++t) {
						auto t0 = std::chrono::steady_clock::now();
						auto r_warm = con.Query(pragma);
						auto t1 = std::chrono::steady_clock::now();
						double tpch_time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
						if (r_warm && r_warm->HasError()) {
							Log(string("TPCH hot run error for query ") + std::to_string(entry.query_number) + ": " +
							    r_warm->GetError());
							hot_run_failed = true;
							break;
						}
						tpch_times_ms.push_back(tpch_time_ms);
						Log("TPCH query " + std::to_string(entry.query_number) + " hot run " + std::to_string(t) +
						    " time (ms): " + FormatNumber(tpch_time_ms));
					}
					if (!hot_run_failed && !tpch_times_ms.empty()) {
						baseline_median = Median(tpch_times_ms);
						Log("TPCH query " + std::to_string(entry.query_number) +
						    " median (ms): " + FormatNumber(baseline_median));
					}
				} catch (const std::exception &e) {
					Log("Baseline Q" + std::to_string(entry.query_number) + " exception: " + string(e.what()));
					baseline_median = -1;
				}
				baseline_cache[entry.query_number] = baseline_median;
			}

			// Write baseline row for this entry (reuse cached median)
			csv << entry.label << ",baseline,0," << FormatNumber(baseline_cache[entry.query_number]) << "\n";

			// Read the AS query file
			string as_sql_bits = ReadFileToString(bitslice_dir + "/" + entry.filename);
			if (as_sql_bits.empty()) {
				throw std::runtime_error("AS query file " + entry.filename + " is empty or unreadable");
			}

			// Build variant list: bitslice always; simple_hash if requested and file exists.
			vector<pair<string, string>> as_variants;
			as_variants.emplace_back("SIMD AS", as_sql_bits);
			if (run_simple_hash) {
				string sh_qfile = FindQueryFile(simple_hash_dir, entry.query_number);
				if (FileExists(sh_qfile)) {
					string sql = ReadFileToString(sh_qfile);
					if (!sql.empty()) {
						as_variants.emplace_back("simple hash AS", sql);
					}
				}
			}

			bool rewrite_for_m_sweep = as_m_values.size() > 1 || std::any_of(as_m_values.begin(), as_m_values.end(),
			                                                                 [](int as_m) { return as_m != 64; });
			for (auto &pv : as_variants) {
				const string &mode_str = pv.first;
				const string &as_sql = pv.second;
				for (int as_m : as_m_values) {
					try {
						string query_sql = as_sql;
						if (rewrite_for_m_sweep && as_m != 64) {
							if (mode_str != "SIMD AS" || !TryRewriteSelectedTpchQuery(entry.label, as_m, query_sql)) {
								query_sql = RewriteAsQueryForSampleAggregation(as_sql, as_m);
							}
						}
						con.Query("SET pac_m=" + std::to_string(as_m) + ";");
						con.Query("SET priv_rewrite=false;");
						auto r_warmup = con.Query(query_sql);
						if (r_warmup && r_warmup->HasError()) {
							Log("AS (" + mode_str + ") " + entry.label + " m=" + std::to_string(as_m) +
							    " warmup error: " + r_warmup->GetError());
							csv << entry.label << "," << mode_str << "," << as_m << ",-1\n";
							continue;
						}
						Log("AS (" + mode_str + ") " + entry.label + " m=" + std::to_string(as_m) + " warmup complete");
						// Run AS query 5 times, take median
						vector<double> as_times_ms;
						bool as_failed = false;
						for (int run = 1; run <= 5; ++run) {
							con.Query("SET pac_m=" + std::to_string(as_m) + ";");
							con.Query("SET priv_rewrite=false;");
							auto t0 = std::chrono::steady_clock::now();
							auto r_as = con.Query(query_sql);
							auto t1 = std::chrono::steady_clock::now();
							con.Query("SET priv_rewrite=false;");
							double as_time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
							if (r_as && r_as->HasError()) {
								Log("AS (" + mode_str + ") " + entry.label + " m=" + std::to_string(as_m) + " run " +
								    std::to_string(run) + " error: " + r_as->GetError());
								as_failed = true;
								break;
							}
							as_times_ms.push_back(as_time_ms);
							Log("AS (" + mode_str + ") " + entry.label + " m=" + std::to_string(as_m) + " run " +
							    std::to_string(run) + " time (ms): " + FormatNumber(as_time_ms));
						}
						if (as_failed) {
							csv << entry.label << "," << mode_str << "," << as_m << ",-1\n";
						} else {
							double as_median = Median(as_times_ms);
							Log("AS (" + mode_str + ") " + entry.label + " m=" + std::to_string(as_m) +
							    " median (ms): " + FormatNumber(as_median));
							csv << entry.label << "," << mode_str << "," << as_m << "," << FormatNumber(as_median)
							    << "\n";
						}
					} catch (const std::exception &e) {
						Log("AS (" + mode_str + ") " + entry.label + " m=" + std::to_string(as_m) +
						    " exception: " + string(e.what()));
						csv << entry.label << "," << mode_str << "," << as_m << ",-1\n";
					}
				}
			}

			if (run_grouped_baselines) {
				struct GroupedBaseline {
					string mode;
					bool overlapping;
				};
				vector<GroupedBaseline> grouped_baselines = {
				    {"Disjoint-SID", false},
				    {"Expanded-Worlds", true},
				};
				for (auto &baseline : grouped_baselines) {
					string query_file = FindQueryFile(sid_queries_dir, entry.query_number);
					if (!FileExists(query_file)) {
						Log(baseline.mode + " " + entry.label + ": no query file found at " + query_file +
						    ", skipping");
						continue;
					}
					string sql = ReadFileToString(query_file);
					if (sql.empty() || !ConfigureSidWorldViews(con, baseline.overlapping)) {
						csv << entry.label << "," << baseline.mode << ",64,-1\n";
						continue;
					}
					double median_ms = BenchmarkSqlQuery(con, sql, baseline.mode + " " + entry.label);
					csv << entry.label << "," << baseline.mode << ",64," << FormatNumber(median_ms) << "\n";
				}

				string matched_simd_file = FindQueryFile(matched_simd_dir, entry.query_number);
				if (!FileExists(matched_simd_file)) {
					Log("Matched-SIMD64 " + entry.label + ": no query file found at " + matched_simd_file +
					    ", skipping");
				} else {
					string matched_simd_sql = ReadFileToString(matched_simd_file);
					string disjoint_sql = ReadFileToString(FindQueryFile(sid_queries_dir, entry.query_number));
					QuerySnapshot disjoint_result;
					QuerySnapshot matched_result;
					bool equivalent = ConfigureSidWorldViews(con, false) &&
					                  ExecuteSnapshot(con, disjoint_sql, "Disjoint-SID validation " + entry.label,
					                                  disjoint_result) &&
					                  ExecuteSnapshot(con, matched_simd_sql, "Matched-SIMD64 validation " + entry.label,
					                                  matched_result) &&
					                  SnapshotsApproximatelyEqual(disjoint_result, matched_result,
					                                              "Matched representation " + entry.label);
					if (!equivalent) {
						Log("Matched-SIMD64 " + entry.label + ": semantic validation failed");
						csv << entry.label << ",Matched-SIMD64,64,-1\n";
					} else {
						Log("Matched-SIMD64 " + entry.label + ": matches Disjoint-SID");
						double median_ms = BenchmarkSqlQuery(con, matched_simd_sql, "Matched-SIMD64 " + entry.label);
						csv << entry.label << ",Matched-SIMD64,64," << FormatNumber(median_ms) << "\n";
					}
				}

				string verdictdb_query_file = FindQueryFile(verdictdb_dir, entry.query_number);
				if (!FileExists(verdictdb_query_file)) {
					Log("VerdictDB-sigmod18 " + entry.label + ": no supported query file found at " +
					    verdictdb_query_file + ", skipping");
				} else if (!verdictdb_ready) {
					csv << entry.label << ",VerdictDB-sigmod18,100,-1\n";
				} else {
					string verdictdb_sql = ReadFileToString(verdictdb_query_file);
					auto mode = VerdictMode(verdictdb_sql);
					double median_ms = BenchmarkSqlQuery(con, verdictdb_sql, mode + " " + entry.label);
					csv << entry.label << "," << mode << ","
					    << (mode == "VerdictDB-exact-fallback" ? 0 : 100) << "," << FormatNumber(median_ms) << "\n";
				}
			}

			// Naive-AS mode: run on a 50% random sample, multiply median by 64.
			string naive_as_qfile = FindQueryFile(naive_as_dir, entry.query_number);
			if (!skip_naive && FileExists(naive_as_qfile)) {
				string naive_as_sql = ReadFileToString(naive_as_qfile);
				if (!naive_as_sql.empty()) {
					try {
						// Split at EXECUTE to get prepare_sql and execute_sql
						auto exec_pos = naive_as_sql.rfind("EXECUTE");
						if (exec_pos == string::npos) {
							Log("Naive-AS " + entry.label + ": no EXECUTE found in query file, skipping");
							csv << entry.label << ",Naive-AS,64,-1\n";
						} else {
							string prepare_sql = naive_as_sql.substr(0, exec_pos);
							string execute_sql = naive_as_sql.substr(exec_pos);

							// Prepare the query (not timed)
							bool setup_ok = true;
							con.Query("DEALLOCATE PREPARE run_query;");
							auto r_prep = con.Query(prepare_sql);
							if (r_prep && r_prep->HasError()) {
								Log("Naive-AS " + entry.label + " prepare error: " + r_prep->GetError());
								setup_ok = false;
							}

							if (!setup_ok) {
								csv << entry.label << ",Naive-AS,64,-1\n";
							} else {
								auto r_warmup = con.Query(execute_sql);
								if (r_warmup && r_warmup->HasError()) {
									Log("Naive-AS " + entry.label + " warmup error: " + r_warmup->GetError());
									csv << entry.label << ",Naive-AS,64,-1\n";
									con.Query("DEALLOCATE PREPARE run_query;");
									continue;
								}
								Log("Naive-AS " + entry.label + " warmup complete");
								// Time EXECUTE 5 times, take median
								vector<double> naive_as_times_ms;
								bool naive_as_failed = false;
								for (int run = 1; run <= 5; ++run) {
									auto t0 = std::chrono::steady_clock::now();
									auto r_exec = con.Query(execute_sql);
									auto t1 = std::chrono::steady_clock::now();
									double t_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
									if (r_exec && r_exec->HasError()) {
										Log("Naive-AS " + entry.label + " run " + std::to_string(run) +
										    " error: " + r_exec->GetError());
										naive_as_failed = true;
										break;
									}
									naive_as_times_ms.push_back(t_ms);
									Log("Naive-AS " + entry.label + " run " + std::to_string(run) +
									    " time (ms): " + FormatNumber(t_ms));
								}
								if (naive_as_failed) {
									csv << entry.label << ",Naive-AS,64,-1\n";
								} else {
									double naive_as_median = Median(naive_as_times_ms) * 64.0;
									Log("Naive-AS " + entry.label +
									    " median*64 (ms): " + FormatNumber(naive_as_median));
									csv << entry.label << ",Naive-AS,64," << FormatNumber(naive_as_median) << "\n";
								}
							}
							con.Query("DEALLOCATE PREPARE run_query;");
						}
					} catch (const std::exception &e) {
						Log("Naive-AS " + entry.label + " exception: " + string(e.what()));
						csv << entry.label << ",Naive-AS,64,-1\n";
					}
				}
			} else if (!skip_naive) {
				Log("Naive-AS " + entry.label + ": no query file found at " + naive_as_qfile + ", skipping");
			}
			csv.flush();
		}

		// Utility is deliberately separate from timing. Repeated independent customer samples permit empirical
		// coverage summaries; the runtime experiment above remains fixed to trial zero for reproducibility.
		if (run_grouped_baselines && verdictdb_utility_trials > 0) {
			std::map<int, QuerySnapshot> exact_cache;
			for (int trial = 0; trial < verdictdb_utility_trials; trial++) {
				if (!ConfigureVerdictDBSample(con, trial, trial == 0 || (trial + 1) % 10 == 0)) {
					throw std::runtime_error("VerdictDB utility trial " + std::to_string(trial) + ": sample setup failed");
				}
				std::set<int> completed_queries;
				for (auto &entry : query_entries) {
					if (!completed_queries.insert(entry.query_number).second) {
						continue;
					}
					string verdictdb_query_file = FindQueryFile(verdictdb_dir, entry.query_number);
					if (!FileExists(verdictdb_query_file)) {
						throw std::runtime_error("VerdictDB utility: missing query file for " + entry.label);
					}
					string verdictdb_sql = ReadFileToString(verdictdb_query_file);
					if (VerdictMode(verdictdb_sql) == "VerdictDB-exact-fallback") {
						continue; // Exact executions have no sampling interval to evaluate.
					}
					QuerySnapshot estimate;
					if (!ExecuteSnapshot(con, verdictdb_sql,
					                     "VerdictDB utility trial " + std::to_string(trial) + " estimate " + entry.label,
					                     estimate)) {
						throw std::runtime_error("VerdictDB utility trial " + std::to_string(trial) +
						                         ": estimate failed for " + entry.label);
					}
					auto exact_entry = exact_cache.find(entry.query_number);
					if (exact_entry == exact_cache.end()) {
						QuerySnapshot exact;
						string exact_sql = "PRAGMA tpch(" + std::to_string(entry.query_number) + ");";
						if (!ExecuteSnapshot(con, exact_sql, "VerdictDB utility exact " + entry.label, exact)) {
							throw std::runtime_error("VerdictDB utility: exact reference failed for " + entry.label);
						}
						exact_entry = exact_cache.emplace(entry.query_number, std::move(exact)).first;
					}
					WriteVerdictUtility(trial, entry.label, estimate, exact_entry->second, utility_csv);
				}
				if ((trial + 1) % 10 == 0 || trial + 1 == verdictdb_utility_trials) {
					Log("Completed VerdictDB utility trial " + std::to_string(trial + 1) + "/" +
					    std::to_string(verdictdb_utility_trials));
				}
				utility_csv.flush();
			}
		}

		// Keep Naive-AS tables in the database so they are reused on the next run

		csv.close();
		Log(string("Benchmark finished. Results written to ") + actual_out);
		if (utility_csv.is_open()) {
			utility_csv.close();
			Log("VerdictDB utility results written to " + utility_out);
		}

		return 0;
	} catch (std::exception &ex) {
		Log(string("Error running benchmark: ") + ex.what());
		return 2;
	}
}

} // namespace duckdb

// Add a small helper for printing usage (placed outside of namespace to avoid analyzer warnings)
static void PrintUsageMain() {
	std::cout << "Usage: as_tpch_benchmark [sf] [db_path] [queries_dir] [out_csv] [options]\n"
	          << "  sf: TPCH scale factor (int, default 10)\n"
	          << "  db_path: DuckDB database file (default 'tpch.db')\n"
	          << "  queries_dir: root directory containing AS SQL variants (default 'benchmark').\n"
	          << "               subdirectories expected: 'tpch/tpch_as_queries' (bitslice), "
	             "'tpch/tpch_as_simple_hash_queries' (simple hash), 'tpch/tpch_naive_as_queries' (Naive-AS),\n"
	          << "               'tpch/tpch_sid_queries' (shared disjoint/expanded SID queries), "
	             "'tpch/tpch_verdictdb_queries' (VerdictDB variational rewrites)\n"
	          << "  out_csv: optional output CSV path (auto-named if omitted)\n"
	          << "  --config <json>: load benchmark options from a JSON config\n"
	          << "  --dry-run: validate options without opening the database\n"
	          << "  --run-simple-hash: optional flag to run a simple hash AS variant as well\n"
	          << "  --run-grouped-baselines: run Disjoint-SID, Expanded-Worlds, and VerdictDB baselines\n"
	          << "  --verdictdb-utility-trials=N: independent VerdictDB utility samples (default 20)\n"
	          << "  --as-ms <csv>: AS sample counts to run, e.g. 64,512 (default 64)\n"
	          << "  --skip-naive: skip Naive-AS setup and runs\n"
	          << "  --threads=N: number of DuckDB threads (default 8)\n";
}

int main(int argc, char **argv) {
	try {
		std::string config_path = duckdb::FindConfigPathArgument(argc, argv);
		bool dry_run = false;

		duckdb::ASTpchOptions options =
		    config_path.empty() ? duckdb::ASTpchOptions() : duckdb::LoadASTpchOptions(config_path);
		std::vector<std::string> positional;
		for (int i = 1; i < argc; i++) {
			std::string arg = argv[i];
			if (arg == "-h" || arg == "--help") {
				PrintUsageMain();
				return 0;
			}
			if (arg == "--config") {
				i++;
				continue;
			}
			if (arg.rfind("--config=", 0) == 0) {
				continue;
			}
			if (arg == "--dry-run") {
				dry_run = true;
			} else if (arg == "--run-simple-hash") {
				options.run_simple_hash = true;
			} else if (arg == "--run-grouped-baselines") {
				options.run_grouped_baselines = true;
			} else if (arg == "--skip-naive") {
				options.skip_naive = true;
			} else if (arg == "--as-ms" && i + 1 < argc) {
				options.as_m_values = duckdb::ParseIntListCSV(argv[++i]);
			} else if (arg.rfind("--as-ms=", 0) == 0) {
				options.as_m_values = duckdb::ParseIntListCSV(arg.substr(8));
			} else if (arg == "--threads" && i + 1 < argc) {
				options.threads = std::stoi(argv[++i]);
			} else if (arg.rfind("--threads=", 0) == 0) {
				options.threads = std::stoi(arg.substr(10));
			} else if (arg == "--verdictdb-utility-trials" && i + 1 < argc) {
				options.verdictdb_utility_trials = std::stoi(argv[++i]);
			} else if (arg.rfind("--verdictdb-utility-trials=", 0) == 0) {
				options.verdictdb_utility_trials =
				    std::stoi(arg.substr(std::string("--verdictdb-utility-trials=").size()));
			} else if (arg.rfind("--", 0) == 0) {
				throw std::runtime_error("unknown option: " + arg);
			} else {
				positional.push_back(arg);
			}
		}
		if (positional.size() > 4) {
			throw std::runtime_error("expected at most four positional arguments");
		}
		if (positional.size() > 0) {
			options.scale_factor = std::stod(positional[0]);
		}
		if (positional.size() > 1) {
			options.db_path = positional[1];
		}
		if (positional.size() > 2) {
			options.queries_dir = positional[2];
		}
		if (positional.size() > 3) {
			options.out_csv = positional[3];
		}
		if (options.threads <= 0) {
			throw std::runtime_error("threads must be positive");
		}
		if (options.verdictdb_utility_trials < 0) {
			throw std::runtime_error("VerdictDB utility trials must be non-negative");
		}
		if (options.as_m_values.empty()) {
			throw std::runtime_error("AS sample-count list must not be empty");
		}
		for (int as_m : options.as_m_values) {
			if (!duckdb::IsValidSampleCount(as_m)) {
				throw std::runtime_error("AS sample counts must be powers of two between 64 and 512");
			}
		}
		if (dry_run) {
			std::cout << "Valid AS TPC-H config: sf=" << options.scale_factor << ", threads=" << options.threads
			          << ", VerdictDB utility trials=" << options.verdictdb_utility_trials << ", m=";
			for (size_t i = 0; i < options.as_m_values.size(); i++) {
				std::cout << (i == 0 ? "" : ",") << options.as_m_values[i];
			}
			std::cout << "\n";
			return 0;
		}
		return duckdb::RunASTPCHBenchmark(options.db_path, options.queries_dir, options.scale_factor, options.out_csv,
		                                  options.run_simple_hash, options.threads, options.as_m_values,
		                                  options.skip_naive, options.run_grouped_baselines,
		                                  options.verdictdb_utility_trials);
	} catch (std::exception &ex) {
		std::cerr << "error: " << ex.what() << "\n";
		return 1;
	}
}
