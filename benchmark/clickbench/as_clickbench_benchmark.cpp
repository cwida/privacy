//
// Created by ila on 07/07/26.
//

#include "as_clickbench_benchmark.hpp"

#include "duckdb.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/main/connection.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits.h>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace duckdb {

static string Timestamp() {
	auto now = std::chrono::system_clock::now();
	std::time_t t = std::chrono::system_clock::to_time_t(now);
	char buf[64];
	std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
	return string(buf);
}

static void Log(const string &msg) {
	Printer::Print("[" + Timestamp() + "] " + msg);
}

static string FormatNumber(double v) {
	std::ostringstream oss;
	oss << std::setprecision(15) << std::defaultfloat << v;
	string s = oss.str();
	auto pos = s.find('.');
	if (pos != string::npos) {
		while (!s.empty() && s.back() == '0') {
			s.pop_back();
		}
		if (!s.empty() && s.back() == '.') {
			s.pop_back();
		}
	}
	return s;
}

static double Median(vector<double> v) {
	std::sort(v.begin(), v.end());
	if (v.empty()) {
		return -1;
	}
	idx_t n = v.size();
	if (n % 2 == 1) {
		return v[n / 2];
	}
	return (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

static bool FileExists(const string &path) {
	struct stat buffer;
	return stat(path.c_str(), &buffer) == 0;
}

static string Trim(const string &s) {
	size_t start = s.find_first_not_of(" \t\r\n");
	if (start == string::npos) {
		return "";
	}
	size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(start, end - start + 1);
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

static vector<string> SplitLines(const string &content) {
	vector<string> lines;
	std::istringstream iss(content);
	string line;
	while (std::getline(iss, line)) {
		string trimmed = Trim(line);
		if (!trimmed.empty() && trimmed.substr(0, 2) != "--") {
			lines.push_back(trimmed);
		}
	}
	return lines;
}

static bool IsSelectedQuery(int qnum, const std::set<int> &query_filter) {
	return query_filter.empty() || query_filter.count(qnum) > 0;
}

static std::set<int> ParseIntSetCSV(const string &list) {
	std::set<int> values;
	std::stringstream ss(list);
	string tok;
	while (std::getline(ss, tok, ',')) {
		tok = Trim(tok);
		if (!tok.empty()) {
			values.insert(std::stoi(tok));
		}
	}
	return values;
}

static vector<int> ParseIntListCSV(const string &list) {
	vector<int> values;
	std::stringstream ss(list);
	string tok;
	while (std::getline(ss, tok, ',')) {
		tok = Trim(tok);
		if (!tok.empty()) {
			values.push_back(std::stoi(tok));
		}
	}
	return values;
}

static bool IsIdentifierChar(char c) {
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

static bool IsValidSampleCount(int m) {
	return m >= 64 && m <= 512 && (m & (m - 1)) == 0;
}

static idx_t FindMatchingParen(const string &sql, idx_t open_pos) {
	int depth = 0;
	bool in_single_quote = false;
	bool in_double_quote = false;
	for (idx_t i = open_pos; i < sql.size(); i++) {
		char c = sql[i];
		if (in_single_quote) {
			if (c == '\'' && i + 1 < sql.size() && sql[i + 1] == '\'') {
				i++;
			} else if (c == '\'') {
				in_single_quote = false;
			}
			continue;
		}
		if (in_double_quote) {
			if (c == '"' && i + 1 < sql.size() && sql[i + 1] == '"') {
				i++;
			} else if (c == '"') {
				in_double_quote = false;
			}
			continue;
		}
		if (c == '\'') {
			in_single_quote = true;
		} else if (c == '"') {
			in_double_quote = true;
		} else if (c == '(') {
			depth++;
		} else if (c == ')') {
			depth--;
			if (depth == 0) {
				return i;
			}
		}
	}
	return string::npos;
}

static vector<string> SplitTopLevelArgs(const string &args) {
	vector<string> result;
	idx_t start = 0;
	int depth = 0;
	bool in_single_quote = false;
	bool in_double_quote = false;
	for (idx_t i = 0; i < args.size(); i++) {
		char c = args[i];
		if (in_single_quote) {
			if (c == '\'' && i + 1 < args.size() && args[i + 1] == '\'') {
				i++;
			} else if (c == '\'') {
				in_single_quote = false;
			}
			continue;
		}
		if (in_double_quote) {
			if (c == '"' && i + 1 < args.size() && args[i + 1] == '"') {
				i++;
			} else if (c == '"') {
				in_double_quote = false;
			}
			continue;
		}
		if (c == '\'') {
			in_single_quote = true;
		} else if (c == '"') {
			in_double_quote = true;
		} else if (c == '(') {
			depth++;
		} else if (c == ')') {
			depth--;
		} else if (c == ',' && depth == 0) {
			result.push_back(Trim(args.substr(start, i - start)));
			start = i + 1;
		}
	}
	result.push_back(Trim(args.substr(start)));
	return result;
}

static bool StartsWithFunctionCall(const string &sql, idx_t pos, const string &name, idx_t &open_pos) {
	if (pos > 0 && IsIdentifierChar(sql[pos - 1])) {
		return false;
	}
	if (sql.compare(pos, name.size(), name) != 0) {
		return false;
	}
	idx_t next = pos + name.size();
	if (next < sql.size() && IsIdentifierChar(sql[next])) {
		return false;
	}
	while (next < sql.size() && std::isspace(static_cast<unsigned char>(sql[next]))) {
		next++;
	}
	if (next >= sql.size() || sql[next] != '(') {
		return false;
	}
	open_pos = next;
	return true;
}

static string StripFunctionCalls(const string &sql, const string &name) {
	string result;
	idx_t pos = 0;
	while (pos < sql.size()) {
		idx_t open_pos;
		if (StartsWithFunctionCall(sql, pos, name, open_pos)) {
			idx_t close_pos = FindMatchingParen(sql, open_pos);
			if (close_pos == string::npos) {
				throw std::runtime_error("could not find closing parenthesis for " + name);
			}
			result += StripFunctionCalls(sql.substr(open_pos + 1, close_pos - open_pos - 1), name);
			pos = close_pos + 1;
			continue;
		}
		result.push_back(sql[pos++]);
	}
	return result;
}

static string RewriteAsFunctionCall(const string &name, const vector<string> &args, int sample_count) {
	string sample_suffix = sample_count == 64 ? "" : "_m";
	if (name == "as_noised_count") {
		if (args.empty()) {
			throw std::runtime_error("as_noised_count rewrite expected at least one argument");
		}
		std::ostringstream out;
		out << "list_avg(as_sample" << sample_suffix << "_count(";
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
		return "list_avg(as_sample" + sample_suffix + "_sum(" + args[0] + ", CAST(" + args[1] + " AS DOUBLE)))";
	}
	if (name == "as_noised_avg") {
		if (args.size() != 2) {
			throw std::runtime_error("as_noised_avg rewrite expected two arguments");
		}
		return "list_avg(as_sample" + sample_suffix + "_avg(" + args[0] + ", CAST(" + args[1] + " AS DOUBLE), 1.0))";
	}
	if (name == "as_noised_min" || name == "as_noised_max") {
		if (args.size() != 2) {
			throw std::runtime_error(name + " rewrite expected two arguments");
		}
		string sample_name = name == "as_noised_min" ? "min" : "max";
		return "list_avg(as_sample" + sample_suffix + "_" + sample_name + "(" + args[0] + ", " + args[1] + "))";
	}
	throw std::runtime_error("unsupported AS aggregate rewrite: " + name);
}

static bool ContainsVariableMUnsupportedAggregate(const string &sql) {
	return sql.find("as_min") != string::npos || sql.find("as_max") != string::npos;
}

static string RewriteAsQueryForSampleAggregation(const string &sql, int sample_count) {
	static const vector<string> names = {"as_noised_count", "as_noised_sum", "as_noised_avg", "as_noised_min",
	                                     "as_noised_max"};
	string raw_hash_sql = StripFunctionCalls(sql, "priv_hash");
	string result;
	idx_t pos = 0;
	while (pos < raw_hash_sql.size()) {
		bool rewritten = false;
		for (auto &name : names) {
			idx_t open_pos;
			if (!StartsWithFunctionCall(raw_hash_sql, pos, name, open_pos)) {
				continue;
			}
			idx_t close_pos = FindMatchingParen(raw_hash_sql, open_pos);
			if (close_pos == string::npos) {
				throw std::runtime_error("could not find closing parenthesis for " + name);
			}
			auto args = SplitTopLevelArgs(raw_hash_sql.substr(open_pos + 1, close_pos - open_pos - 1));
			result += RewriteAsFunctionCall(name, args, sample_count);
			pos = close_pos + 1;
			rewritten = true;
			break;
		}
		if (!rewritten) {
			result.push_back(raw_hash_sql[pos++]);
		}
	}
	return result;
}

static int ExecuteCommand(const string &cmd) {
	Log("Executing: " + cmd);
	int ret = system(cmd.c_str());
	if (ret == -1) {
		return -1;
	}
	if (WIFEXITED(ret)) {
		return WEXITSTATUS(ret);
	}
	return ret;
}

static string FindFile(const string &filename) {
	vector<string> candidates = {filename,
	                             "./" + filename,
	                             "../" + filename,
	                             "../../" + filename,
	                             "benchmark/" + filename,
	                             "../benchmark/" + filename,
	                             "../../benchmark/" + filename};
	for (auto &cand : candidates) {
		if (FileExists(cand)) {
			return cand;
		}
	}

	char exe_path[PATH_MAX];
	ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
	if (len != -1) {
		exe_path[len] = '\0';
		string dir = string(exe_path);
		auto pos = dir.find_last_of('/');
		if (pos != string::npos) {
			dir = dir.substr(0, pos);
		}
		for (int i = 0; i < 6; i++) {
			string cand = dir + "/" + filename;
			if (FileExists(cand)) {
				return cand;
			}
			cand = dir + "/benchmark/" + filename;
			if (FileExists(cand)) {
				return cand;
			}
			auto p = dir.find_last_of('/');
			if (p == string::npos) {
				break;
			}
			dir = dir.substr(0, p);
		}
	}
	return "";
}

struct QueryEntry {
	string filename;
	string label;
	int query_number;
};

static vector<QueryEntry> DiscoverQueryFiles(const string &dir) {
	vector<QueryEntry> entries;
	DIR *d = opendir(dir.c_str());
	if (!d) {
		return entries;
	}
	struct dirent *ent;
	while ((ent = readdir(d)) != nullptr) {
		string name = ent->d_name;
		if (name.size() < 7 || name[0] != 'q' || name.substr(name.size() - 4) != ".sql") {
			continue;
		}
		int qnum = 0;
		size_t i = 1;
		while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) {
			qnum = qnum * 10 + (name[i] - '0');
			i++;
		}
		if (qnum == 0) {
			continue;
		}
		entries.push_back({name, name.substr(0, name.size() - 4), qnum});
	}
	closedir(d);
	std::sort(entries.begin(), entries.end(), [](const QueryEntry &a, const QueryEntry &b) {
		if (a.query_number != b.query_number) {
			return a.query_number < b.query_number;
		}
		return a.label < b.label;
	});
	return entries;
}

static bool RunSql(Connection &con, const string &sql, string &error) {
	error.clear();
	try {
		auto result = con.Query(sql);
		if (result && result->HasError()) {
			error = result->GetError();
			return false;
		}
		while (result && result->Fetch()) {
		}
		return true;
	} catch (std::exception &ex) {
		error = ex.what();
		return false;
	}
}

static double TimeSql(Connection &con, const string &sql, string &error) {
	auto start = std::chrono::steady_clock::now();
	bool ok = RunSql(con, sql, error);
	auto end = std::chrono::steady_clock::now();
	if (!ok) {
		return -1;
	}
	return std::chrono::duration<double, std::milli>(end - start).count();
}

static double RunMedian(Connection &con, const string &sql, int hot_runs, const string &label) {
	vector<double> times;
	string error;
	for (int run = 1; run <= hot_runs; run++) {
		double time_ms = TimeSql(con, sql, error);
		if (time_ms < 0) {
			Log(label + " run " + std::to_string(run) + " error: " + error);
			return -1;
		}
		times.push_back(time_ms);
		Log(label + " run " + std::to_string(run) + " time (ms): " + FormatNumber(time_ms));
	}
	double median = Median(times);
	Log(label + " median (ms): " + FormatNumber(median));
	return median;
}

int RunASClickBenchBenchmark(const string &db_path, const string &queries_dir, const string &out_csv, bool micro,
                             const std::set<int> &query_filter, int threads, const string &memory_limit,
                             const std::vector<int> &as_m_values) {
	try {
		if (as_m_values.empty()) {
			throw std::runtime_error("at least one AS m value is required");
		}
		for (auto m : as_m_values) {
			if (!IsValidSampleCount(m)) {
				throw std::runtime_error("AS m values must be powers of two between 64 and 512");
			}
		}

		char cwd[PATH_MAX];
		if (!getcwd(cwd, sizeof(cwd))) {
			throw std::runtime_error("Failed to get current working directory");
		}
		string work_dir = string(cwd);
		string parquet_path = work_dir + "/hits.parquet";
		string db_actual = !db_path.empty() ? db_path : (micro ? "clickbench_micro.db" : "clickbench.db");

		string create_file = FindFile(queries_dir + "/create.sql");
		string load_file = FindFile(queries_dir + "/load.sql");
		string queries_file = FindFile(queries_dir + "/queries.sql");
		if (create_file.empty() || load_file.empty() || queries_file.empty()) {
			throw std::runtime_error("Cannot find ClickBench create/load/queries files in " + queries_dir);
		}
		string queries_dir_resolved = queries_file.substr(0, queries_file.find_last_of('/'));
		string clickbench_root = queries_dir_resolved.substr(0, queries_dir_resolved.find_last_of('/'));
		string as_dir = clickbench_root + "/clickbench_as_queries";

		Log("Using AS queries dir: " + as_dir);

		string create_sql = ReadFileToString(create_file);
		string load_sql = ReadFileToString(load_file);
		vector<string> baseline_queries = SplitLines(ReadFileToString(queries_file));
		if (baseline_queries.empty()) {
			throw std::runtime_error("No baseline ClickBench queries found");
		}

		DuckDB db(db_actual.c_str());
		Connection con(db);
		con.Query("LOAD privacy;");
		con.Query("SET threads TO " + std::to_string(threads) + ";");
		if (!memory_limit.empty()) {
			con.Query("SET memory_limit='" + memory_limit + "';");
		}
		con.Query("SET pac_sample_diversity_check=false;");
		con.Query("SET privacy_seed=42;");
		con.Query("SET temp_directory='" + db_actual + ".tmp';");

		bool table_exists = false;
		auto check = con.Query("SELECT COUNT(*) FROM information_schema.tables WHERE table_name = 'hits';");
		if (check && !check->HasError()) {
			auto chunk = check->Fetch();
			if (chunk && chunk->size() > 0) {
				table_exists = chunk->GetValue(0, 0).GetValue<int64_t>() > 0;
			}
		}
		if (!table_exists) {
			if (!FileExists(parquet_path)) {
				Log("Downloading ClickBench hits parquet...");
				string cmd =
				    "wget -O \"" + parquet_path + "\" https://datasets.clickhouse.com/hits_compatible/hits.parquet";
				int ret = ExecuteCommand(cmd);
				if (ret != 0) {
					cmd = "curl -L -o \"" + parquet_path +
					      "\" https://datasets.clickhouse.com/hits_compatible/hits.parquet";
					ret = ExecuteCommand(cmd);
				}
				if (ret != 0) {
					throw std::runtime_error("Failed to download hits.parquet");
				}
			}
			Log("Creating hits table...");
			string error;
			if (!RunSql(con, create_sql, error)) {
				throw std::runtime_error("Failed to create hits table: " + error);
			}
			string actual_load_sql =
			    std::regex_replace(load_sql, std::regex("'hits\\.parquet'"), "'" + parquet_path + "'");
			if (micro) {
				actual_load_sql = std::regex_replace(
				    actual_load_sql, std::regex("(FROM\\s+read_parquet\\([^)]+\\)[^;]*)"), "$1 LIMIT 5000000");
			}
			Log("Loading hits table...");
			if (!RunSql(con, actual_load_sql, error)) {
				throw std::runtime_error("Failed to load hits table: " + error);
			}
			con.Query("CHECKPOINT;");
		} else {
			Log("hits table already exists, skipping load.");
		}

		auto entries = DiscoverQueryFiles(as_dir);
		if (entries.empty()) {
			throw std::runtime_error("No AS query files found in " + as_dir);
		}
		vector<QueryEntry> selected_entries;
		for (auto &entry : entries) {
			if (IsSelectedQuery(entry.query_number, query_filter)) {
				selected_entries.push_back(entry);
			}
		}
		if (selected_entries.empty()) {
			throw std::runtime_error("query filter selected no AS ClickBench queries");
		}
		Log("Selected " + std::to_string(selected_entries.size()) + " AS ClickBench queries.");
		bool sample_aggregation_sweep = as_m_values.size() > 1 || as_m_values[0] != 64;

		string actual_out = out_csv;
		if (actual_out.empty()) {
			actual_out = micro ? "benchmark/clickbench/as_clickbench_micro_results.csv"
			                   : "benchmark/clickbench/as_clickbench_results.csv";
		}
		std::ofstream csv(actual_out, std::ofstream::out | std::ofstream::trunc);
		if (!csv.is_open()) {
			throw std::runtime_error("Failed to open output CSV: " + actual_out);
		}
		csv << "query,mode,m,median_ms\n";

		std::map<int, double> baseline_cache;
		for (auto &entry : selected_entries) {
			Log("=== " + entry.label + " (Q" + std::to_string(entry.query_number) + ") ===");
			if (entry.query_number < 1 || static_cast<idx_t>(entry.query_number) > baseline_queries.size()) {
				throw std::runtime_error("AS query number out of baseline query range");
			}

			if (baseline_cache.find(entry.query_number) == baseline_cache.end()) {
				string baseline_sql = baseline_queries[entry.query_number - 1];
				string error;
				RunSql(con, baseline_sql, error); // cold run
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				baseline_cache[entry.query_number] =
				    RunMedian(con, baseline_sql, 5, "DuckDB Q" + std::to_string(entry.query_number));
			}
			csv << entry.label << ",baseline,0," << FormatNumber(baseline_cache[entry.query_number]) << "\n";

			string as_sql = ReadFileToString(as_dir + "/" + entry.filename);
			if (as_sql.empty()) {
				for (auto as_m : as_m_values) {
					csv << entry.label << ",SIMD-AS," << as_m << ",-1\n";
				}
			} else {
				con.Query("SET priv_rewrite=false;");
				for (auto as_m : as_m_values) {
					con.Query("SET dp_sass_m=" + std::to_string(as_m) + ";");
					string query_sql = as_sql;
					if (sample_aggregation_sweep) {
						if (ContainsVariableMUnsupportedAggregate(as_sql)) {
							Log("SIMD-AS " + entry.label + " m=" + std::to_string(as_m) +
							    " skipped: sample-aggregation sweep supports COUNT/SUM/AVG only");
							csv << entry.label << ",SIMD-AS," << as_m << ",-1\n";
							continue;
						}
						query_sql = RewriteAsQueryForSampleAggregation(as_sql, as_m);
					}
					double as_median =
					    RunMedian(con, query_sql, 5, "SIMD-AS " + entry.label + " m=" + std::to_string(as_m));
					csv << entry.label << ",SIMD-AS," << as_m << "," << FormatNumber(as_median) << "\n";
				}
				con.Query("SET priv_rewrite=true;");
			}
			csv.flush();
		}

		csv.close();
		Log("Benchmark finished. Results written to " + actual_out);
		return 0;
	} catch (std::exception &ex) {
		Log("Error running benchmark: " + string(ex.what()));
		return 2;
	}
}

} // namespace duckdb

static void PrintUsageMain() {
	std::cout << "Usage: as_clickbench_benchmark [options]\n"
	          << "Options:\n"
	          << "  --micro           Run with clickbench_micro.db / 5M-row load\n"
	          << "  --db <path>       DuckDB database file\n"
	          << "  --queries <dir>   Directory containing create.sql, load.sql, queries.sql\n"
	          << "  --out <csv>       Output CSV path\n"
	          << "  --query-list <csv>  1-based ClickBench query ids to run\n"
	          << "  --as-ms <csv>     AS sample counts to run, e.g. 64,512 (default 64)\n"
	          << "  --threads=N       DuckDB thread count (default 8)\n"
	          << "  --memory-limit <limit>  DuckDB memory limit, e.g. 16GB\n";
}

int main(int argc, char **argv) {
	bool micro = false;
	std::string db_path;
	std::string queries_dir = "benchmark/clickbench/clickbench_queries";
	std::string out_csv;
	std::string memory_limit;
	std::set<int> query_filter;
	std::vector<int> as_m_values = {64};
	int threads = 8;

	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "-h" || arg == "--help") {
			PrintUsageMain();
			return 0;
		} else if (arg == "--micro") {
			micro = true;
		} else if (arg == "--db" && i + 1 < argc) {
			db_path = argv[++i];
		} else if (arg == "--queries" && i + 1 < argc) {
			queries_dir = argv[++i];
		} else if (arg == "--out" && i + 1 < argc) {
			out_csv = argv[++i];
		} else if (arg == "--query-list" && i + 1 < argc) {
			query_filter = duckdb::ParseIntSetCSV(argv[++i]);
		} else if (arg == "--as-ms" && i + 1 < argc) {
			as_m_values = duckdb::ParseIntListCSV(argv[++i]);
		} else if (arg.rfind("--as-ms=", 0) == 0) {
			as_m_values = duckdb::ParseIntListCSV(arg.substr(8));
		} else if (arg == "--memory-limit" && i + 1 < argc) {
			memory_limit = argv[++i];
		} else if (arg.rfind("--threads=", 0) == 0) {
			threads = std::stoi(arg.substr(10));
		} else {
			std::cerr << "Unknown option: " << arg << "\n";
			PrintUsageMain();
			return 1;
		}
	}

	return duckdb::RunASClickBenchBenchmark(db_path, queries_dir, out_csv, micro, query_filter, threads, memory_limit,
	                                        as_m_values);
}
