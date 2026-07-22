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

struct ASTpchOptions {
	double scale_factor = 10.0;
	string db_path = "tpch.db";
	string queries_dir = "benchmark";
	string out_csv;
	bool run_simple_hash = false;
	bool skip_naive = false;
	int threads = 8;
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
	options.skip_naive = JsonBool(root, "skip_naive", options.skip_naive);
	options.threads = static_cast<int>(JsonNumber(root, "threads", options.threads));
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

int RunASTPCHBenchmark(const string &db_path, const string &queries_dir, double sf, const string &out_csv,
                       bool run_simple_hash, int threads, const vector<int> &as_m_values, bool skip_naive) {
	try {
		Log(string("run_simple_hash flag: ") + (run_simple_hash ? string("true") : string("false")));
		Log(string("skip_naive flag: ") + (skip_naive ? string("true") : string("false")));
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

		// Cache baseline median per query number (avoid re-running for variants like q08-nolambda)
		std::map<int, double> baseline_cache;

		// Create sampling/helper tables only when Naive-AS is requested.
		if (!skip_naive) {
			Log("Creating Naive-AS sampling tables (if not already present)...");
			NaiveASCreateTables(con);
			Log("Naive-AS sampling tables ready.");
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
		}

		// Keep Naive-AS tables in the database so they are reused on the next run

		csv.close();
		Log(string("Benchmark finished. Results written to ") + actual_out);

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
	             "'tpch/tpch_as_simple_hash_queries' (simple hash), 'tpch/tpch_naive_as_queries' (Naive-AS)\n"
	          << "  out_csv: optional output CSV path (auto-named if omitted)\n"
	          << "  --config <json>: load benchmark options from a JSON config\n"
	          << "  --dry-run: validate options without opening the database\n"
	          << "  --run-simple-hash: optional flag to run a simple hash AS variant as well\n"
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
			          << ", m=";
			for (size_t i = 0; i < options.as_m_values.size(); i++) {
				std::cout << (i == 0 ? "" : ",") << options.as_m_values[i];
			}
			std::cout << "\n";
			return 0;
		}
		return duckdb::RunASTPCHBenchmark(options.db_path, options.queries_dir, options.scale_factor, options.out_csv,
		                                  options.run_simple_hash, options.threads, options.as_m_values,
		                                  options.skip_naive);
	} catch (std::exception &ex) {
		std::cerr << "error: " << ex.what() << "\n";
		return 1;
	}
}
