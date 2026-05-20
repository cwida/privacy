// IMDb/JOB benchmark runner for the privacy extension.

#include "duckdb.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/main/connection.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace duckdb {

static constexpr const char *DEFAULT_DATA_URL = "https://bonsai.cedardb.com/job/imdb.tgz";

struct BenchmarkResult {
	int query_num = 0;
	string mode;
	int run = 0;
	double time_ms = 0;
	bool success = false;
	string error_msg;
	string utility;
	string recall;
	string precision;
	string epsilon;
	string delta_scenario;
	string dp_delta;
	string bound_scenario;
	string dp_sum_bound;
	string pac_mi;
	int64_t seed = 0;
};

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

static bool FileExists(const string &path) {
	struct stat buffer;
	return stat(path.c_str(), &buffer) == 0;
}

static bool DirectoryExists(const string &path) {
	struct stat buffer;
	return stat(path.c_str(), &buffer) == 0 && S_ISDIR(buffer.st_mode);
}

static string ReadFileToString(const string &path) {
	std::ifstream in(path);
	if (!in.is_open()) {
		return "";
	}
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

static string Trim(const string &s) {
	size_t start = s.find_first_not_of(" \t\r\n");
	if (start == string::npos) {
		return "";
	}
	size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(start, end - start + 1);
}

static string ToLowerAscii(string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

static string FormatNumber(double v) {
	std::ostringstream oss;
	oss << std::setprecision(8) << std::defaultfloat << v;
	string s = oss.str();
	auto pos = s.find('.');
	if (pos != string::npos && s.find('e') == string::npos && s.find('E') == string::npos) {
		while (!s.empty() && s.back() == '0') {
			s.pop_back();
		}
		if (!s.empty() && s.back() == '.') {
			s.pop_back();
		}
	}
	return s;
}

static string CsvQuote(const string &s) {
	string out = "\"";
	for (char c : s) {
		if (c == '"') {
			out += "\"\"";
		} else {
			out += c;
		}
	}
	out += "\"";
	return out;
}

static string SqlQuote(const string &s) {
	string out = "'";
	for (char c : s) {
		if (c == '\'') {
			out += "''";
		} else {
			out += c;
		}
	}
	out += "'";
	return out;
}

static string FindFile(const string &filename) {
	vector<string> candidates = {filename, "./" + filename, "../" + filename, "../../" + filename};
	for (auto &cand : candidates) {
		if (FileExists(cand)) {
			return cand;
		}
	}
	return "";
}

static vector<string> SplitSqlStatements(const string &content) {
	vector<string> statements;
	string current;
	bool in_single_quote = false;
	for (idx_t i = 0; i < content.size(); i++) {
		char c = content[i];
		current += c;
		if (c == '\'' && in_single_quote && i + 1 < content.size() && content[i + 1] == '\'') {
			current += content[++i];
			continue;
		}
		if (c == '\'') {
			in_single_quote = !in_single_quote;
		}
		if (c == ';' && !in_single_quote) {
			string stmt = Trim(current);
			if (!stmt.empty() && stmt.substr(0, 2) != "--") {
				statements.push_back(stmt);
			}
			current.clear();
		}
	}
	string stmt = Trim(current);
	if (!stmt.empty() && stmt.substr(0, 2) != "--") {
		statements.push_back(stmt);
	}
	return statements;
}

static vector<string> ReadQueryLines(const string &path) {
	vector<string> queries;
	std::istringstream in(ReadFileToString(path));
	string line;
	while (std::getline(in, line)) {
		line = Trim(line);
		if (line.empty() || line.substr(0, 2) == "--") {
			continue;
		}
		if (!line.empty() && line.back() == ';') {
			line.pop_back();
		}
		queries.push_back(line);
	}
	return queries;
}

static void ExecuteOrThrow(Connection &con, const string &sql, const string &context) {
	auto result = con.Query(sql);
	if (result && result->HasError()) {
		throw std::runtime_error(context + ": " + result->GetError());
	}
}

static void ExecuteScript(Connection &con, const string &script, const string &context) {
	for (auto &stmt : SplitSqlStatements(script)) {
		ExecuteOrThrow(con, stmt, context + " statement failed");
	}
}

static bool TableExists(Connection &con, const string &table_name) {
	auto result = con.Query("SELECT COUNT(*) FROM information_schema.tables WHERE table_name = " + SqlQuote(table_name));
	if (!result || result->HasError()) {
		return false;
	}
	auto chunk = result->Fetch();
	return chunk && chunk->size() > 0 && chunk->GetValue(0, 0).GetValue<int64_t>() > 0;
}

static int64_t CountRows(Connection &con, const string &table_name) {
	auto result = con.Query("SELECT COUNT(*) FROM " + table_name);
	if (!result || result->HasError()) {
		return 0;
	}
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() == 0) {
		return 0;
	}
	return chunk->GetValue(0, 0).GetValue<int64_t>();
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

static bool QueryHasSumOrAvg(const string &query) {
	string lower = ToLowerAscii(query);
	return lower.find("sum(") != string::npos || lower.find("avg(") != string::npos;
}

static size_t FindKeywordOutsideParens(const string &lower, const string &keyword, size_t start = 0) {
	int depth = 0;
	for (size_t i = start; i + keyword.size() <= lower.size(); i++) {
		char c = lower[i];
		if (c == '(') {
			depth++;
		} else if (c == ')' && depth > 0) {
			depth--;
		}
		if (depth == 0 && lower.compare(i, keyword.size(), keyword) == 0) {
			return i;
		}
	}
	return string::npos;
}

static vector<string> ExtractSumAvgArgs(const string &query) {
	vector<string> args;
	string lower = ToLowerAscii(query);
	size_t pos = 0;
	while (pos < lower.size()) {
		size_t sum_pos = lower.find("sum(", pos);
		size_t avg_pos = lower.find("avg(", pos);
		size_t fn_pos = string::npos;
		if (sum_pos != string::npos && (avg_pos == string::npos || sum_pos < avg_pos)) {
			fn_pos = sum_pos;
		} else if (avg_pos != string::npos) {
			fn_pos = avg_pos;
		}
		if (fn_pos == string::npos) {
			break;
		}
		size_t arg_start = fn_pos + 4;
		int depth = 1;
		size_t i = arg_start;
		for (; i < query.size(); i++) {
			if (query[i] == '(') {
				depth++;
			} else if (query[i] == ')') {
				depth--;
				if (depth == 0) {
					break;
				}
			}
		}
		if (i >= query.size()) {
			break;
		}
		string arg = Trim(query.substr(arg_start, i - arg_start));
		if (!arg.empty() && arg != "*") {
			args.push_back(arg);
		}
		pos = i + 1;
	}
	return args;
}

static string BuildBoundQuery(const string &query) {
	auto args = ExtractSumAvgArgs(query);
	if (args.empty()) {
		return "";
	}
	string lower = ToLowerAscii(query);
	size_t from_pos = FindKeywordOutsideParens(lower, " from ");
	if (from_pos == string::npos) {
		return "";
	}
	size_t end = query.size();
	vector<string> terminators = {" group by ", " order by ", " limit ", " offset "};
	for (auto &term : terminators) {
		size_t p = FindKeywordOutsideParens(lower, term, from_pos + 1);
		if (p != string::npos) {
			end = std::min(end, p);
		}
	}
	string from_where = Trim(query.substr(from_pos, end - from_pos));
	string expr;
	for (idx_t i = 0; i < args.size(); i++) {
		if (i > 0) {
			expr += ", ";
		}
		expr += "abs(CAST((" + args[i] + ") AS DOUBLE))";
	}
	if (args.size() > 1) {
		expr = "greatest(" + expr + ")";
	}
	return "SELECT max(" + expr + ") " + from_where;
}

static void MaterializeQuery(Connection &con, const string &query) {
	auto result = con.Query(query);
	if (!result) {
		throw std::runtime_error("unknown query error");
	}
	if (result->HasError()) {
		throw std::runtime_error(result->GetError());
	}
	while (result->Fetch()) {
	}
}

static BenchmarkResult RunMeasuredQuery(Connection &con, const string &query, int query_num, const string &mode, int run) {
	BenchmarkResult result;
	result.query_num = query_num;
	result.mode = mode;
	result.run = run;

	auto start = std::chrono::steady_clock::now();
	try {
		MaterializeQuery(con, query);
		result.success = true;
	} catch (std::exception &ex) {
		result.success = false;
		result.error_msg = ex.what();
		auto sp = result.error_msg.find("\n\nStack Trace");
		if (sp != string::npos) {
			result.error_msg = result.error_msg.substr(0, sp);
		}
		if (result.error_msg.size() > 400) {
			result.error_msg = result.error_msg.substr(0, 400);
		}
	}
	auto end = std::chrono::steady_clock::now();
	result.time_ms = std::chrono::duration<double, std::milli>(end - start).count();
	return result;
}

static void CreateMicroData(Connection &con, const string &schema_sql) {
	ExecuteScript(con, schema_sql, "create micro schema");
	ExecuteScript(con, R"SQL(
INSERT INTO name VALUES
	(1, 'Skewed Person', NULL, 1, 'm', NULL, NULL, NULL, NULL),
	(2, 'Occasional Person', NULL, 2, 'f', NULL, NULL, NULL, NULL),
	(3, 'Rare Person', NULL, 3, NULL, NULL, NULL, NULL, NULL),
	(4, 'Alias Heavy Person', NULL, 4, 'f', NULL, NULL, NULL, NULL);
INSERT INTO cast_info VALUES
	(1, 1, 10, NULL, NULL, 1, 1),
	(2, 1, 11, NULL, NULL, 2, 1),
	(3, 1, 12, NULL, NULL, 3, 1),
	(4, 1, 13, NULL, NULL, 4, 2),
	(5, 1, 14, NULL, NULL, 5, 2),
	(6, 1, 15, NULL, NULL, 6, 2),
	(7, 2, 11, NULL, NULL, 1, 1),
	(8, 2, 12, NULL, NULL, NULL, 3),
	(9, 3, 12, NULL, NULL, 2, 1),
	(10, 4, 13, NULL, NULL, 1, 4);
INSERT INTO aka_name VALUES
	(1, 1, 'S. Person', 'I', NULL, NULL, NULL, NULL),
	(2, 1, 'Skew Person', 'II', NULL, NULL, NULL, NULL),
	(3, 4, 'Alias A', 'I', NULL, NULL, NULL, NULL),
	(4, 4, 'Alias B', 'I', NULL, NULL, NULL, NULL),
	(5, 4, 'Alias C', NULL, NULL, NULL, NULL, NULL);
INSERT INTO person_info VALUES
	(1, 1, 10, 'birth date', NULL),
	(2, 1, 11, 'height', 'verified'),
	(3, 2, 10, 'birth date', NULL),
	(4, 4, 12, 'trivia', 'public note');
INSERT INTO title VALUES
	(10, 'T1', NULL, 1, 2000, NULL, NULL, NULL, NULL, NULL, NULL, NULL),
	(11, 'T2', NULL, 1, 2001, NULL, NULL, NULL, NULL, NULL, NULL, NULL),
	(12, 'T3', NULL, 2, 2001, NULL, NULL, NULL, NULL, NULL, NULL, NULL),
	(13, 'T4', NULL, 2, 2002, NULL, NULL, NULL, NULL, NULL, NULL, NULL),
	(14, 'T5', NULL, 3, 2003, NULL, NULL, NULL, NULL, NULL, NULL, NULL),
	(15, 'T6', NULL, 3, 2004, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
)SQL",
	              "insert micro data");
}

static void EnsureFullData(Connection &con, const string &schema_sql, const string &load_sql, const string &data_dir,
                           const string &download_url) {
	if (!DirectoryExists(data_dir) || !FileExists(data_dir + "/name.csv")) {
		string archive = data_dir + "/../imdb.tgz";
		ExecuteCommand("mkdir -p " + SqlQuote(data_dir));
		if (!FileExists(archive)) {
			string wget_cmd = "wget -O " + SqlQuote(archive) + " " + SqlQuote(download_url);
			int ret = ExecuteCommand(wget_cmd);
			if (ret != 0) {
				string curl_cmd = "curl -L -o " + SqlQuote(archive) + " " + SqlQuote(download_url);
				ret = ExecuteCommand(curl_cmd);
				if (ret != 0) {
					throw std::runtime_error("failed to download IMDb/JOB dataset");
				}
			}
		}
		int ret = ExecuteCommand("tar -xzf " + SqlQuote(archive) + " -C " + SqlQuote(data_dir));
		if (ret != 0) {
			throw std::runtime_error("failed to extract IMDb/JOB dataset");
		}
	}

	ExecuteScript(con, schema_sql, "create IMDb schema");
	string actual_load = std::regex_replace(load_sql, std::regex("\\{DATA_DIR\\}"), data_dir);
	ExecuteScript(con, actual_load, "load IMDb CSV data");
}

static double QueryDouble(Connection &con, const string &sql, const string &context) {
	auto result = con.Query(sql);
	if (!result || result->HasError()) {
		throw std::runtime_error(context + ": " + (result ? result->GetError() : "unknown error"));
	}
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() == 0 || chunk->GetValue(0, 0).IsNull()) {
		throw std::runtime_error(context + ": no value");
	}
	return chunk->GetValue(0, 0).DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
}

int RunIMDBBenchmark(const string &db_path, const string &queries_path, const string &out_csv, const string &data_dir,
                     const string &download_url, bool micro, int runs) {
	try {
		string schema_file = FindFile("benchmark/imdb/schema.sql");
		string setup_file = FindFile("benchmark/imdb/setup.sql");
		string load_file = FindFile("benchmark/imdb/load.sql");
		string query_file = FindFile(queries_path.empty() ? "benchmark/imdb/queries.sql" : queries_path);
		if (schema_file.empty() || setup_file.empty() || load_file.empty() || query_file.empty()) {
			throw std::runtime_error("missing IMDb benchmark SQL assets");
		}

		string actual_db = db_path.empty() ? (micro ? "imdb_micro.db" : "imdb.db") : db_path;
		string actual_out = out_csv.empty() ? (micro ? "benchmark/imdb_micro_results.csv" : "benchmark/imdb_results.csv") :
		                                    out_csv;
		Log("Database: " + actual_db);
		Log("Queries: " + query_file);

		DuckDB db(actual_db);
		Connection con(db);
		ExecuteOrThrow(con, "LOAD privacy", "load privacy extension");
		ExecuteOrThrow(con, "SET memory_limit='60GB'", "set memory limit");

		string schema_sql = ReadFileToString(schema_file);
		string setup_sql = ReadFileToString(setup_file);
		string load_sql = ReadFileToString(load_file);
		vector<string> queries = ReadQueryLines(query_file);
		if (queries.empty()) {
			throw std::runtime_error("no IMDb benchmark queries found");
		}

		if (!TableExists(con, "name") || CountRows(con, "name") == 0) {
			if (micro) {
				Log("Creating IMDb micro fixture");
				CreateMicroData(con, schema_sql);
			} else {
				Log("Creating and loading IMDb/JOB database");
				EnsureFullData(con, schema_sql, load_sql, data_dir.empty() ? "benchmark/imdb/data" : data_dir,
				               download_url.empty() ? DEFAULT_DATA_URL : download_url);
			}
			ExecuteOrThrow(con, "CHECKPOINT", "checkpoint after load");
		} else {
			Log("IMDb tables already exist, skipping load");
		}

		ExecuteOrThrow(con, "PRAGMA clear_privacy_metadata", "clear privacy metadata");
		double privacy_units = QueryDouble(con, "SELECT COUNT(DISTINCT id) FROM name", "compute person privacy units");
		if (privacy_units <= 1.0 || !std::isfinite(privacy_units)) {
			throw std::runtime_error("invalid IMDb person privacy unit count");
		}
		Log("IMDb privacy units (distinct name.id): " + FormatNumber(privacy_units));

		vector<double> perfect_bounds(queries.size(), 0.0);
		vector<bool> has_sum_avg(queries.size(), false);
		for (idx_t q = 0; q < queries.size(); q++) {
			has_sum_avg[q] = QueryHasSumOrAvg(queries[q]);
			if (!has_sum_avg[q]) {
				continue;
			}
			string bound_sql = BuildBoundQuery(queries[q]);
			if (!bound_sql.empty()) {
				try {
					perfect_bounds[q] = QueryDouble(con, bound_sql, "infer dp_sum_bound");
					Log("Q" + std::to_string(q + 1) + " perfect dp_sum_bound=" + FormatNumber(perfect_bounds[q]));
				} catch (std::exception &ex) {
					Log("Q" + std::to_string(q + 1) + " bound inference failed: " + ex.what());
				}
			}
		}

		vector<double> epsilons = micro ? vector<double> {1.0} : vector<double> {0.125, 0.25, 0.5, 1.0};
		vector<BenchmarkResult> results;
		const string pac_mi = "0.0078125";

		for (int run = 1; run <= runs; run++) {
			for (idx_t q = 0; q < queries.size(); q++) {
				int qnum = static_cast<int>(q + 1);
				const string &query = queries[q];

				ExecuteOrThrow(con, "SET privacy_mode='pac'", "baseline reset mode");
				ExecuteOrThrow(con, "PRAGMA clear_privacy_metadata", "baseline clear metadata");
				auto baseline = RunMeasuredQuery(con, query, qnum, "baseline", run);
				results.push_back(baseline);
				Log("Q" + std::to_string(qnum) + " baseline run " + std::to_string(run) +
				    (baseline.success ? " ok " : " failed ") + FormatNumber(baseline.time_ms) + " ms");

				ExecuteOrThrow(con, "PRAGMA clear_privacy_metadata", "PAC clear metadata");
				ExecuteScript(con, setup_sql, "PAC setup");
				ExecuteOrThrow(con, "SET privacy_mode='pac'", "PAC mode");
				ExecuteOrThrow(con, "SET pac_mi=" + pac_mi, "PAC MI");
				int64_t seed = static_cast<int64_t>(run * 997 + qnum);
				ExecuteOrThrow(con, "SET privacy_seed=" + std::to_string(seed), "PAC seed");
				auto pac = RunMeasuredQuery(con, query, qnum, "PAC", run);
				pac.pac_mi = pac_mi;
				pac.seed = seed;
				results.push_back(pac);
				Log("Q" + std::to_string(qnum) + " PAC run " + std::to_string(run) +
				    (pac.success ? " ok " : " failed ") + FormatNumber(pac.time_ms) + " ms");

				for (double epsilon : epsilons) {
					ExecuteOrThrow(con, "PRAGMA clear_privacy_metadata", "DP clear metadata");
					ExecuteScript(con, setup_sql, "DP setup");
					ExecuteOrThrow(con, "SET privacy_mode='dp_elastic'", "DP mode");
					ExecuteOrThrow(con, "SET privacy_seed=" + std::to_string(seed), "DP seed");
					ExecuteOrThrow(con, "PRAGMA refresh_dp_stats(" + FormatNumber(epsilon) + ")", "refresh DP stats");
					if (has_sum_avg[q]) {
						double bound = perfect_bounds[q] > 0 ? perfect_bounds[q] : 1.0;
						ExecuteOrThrow(con, "SET dp_sum_bound=" + FormatNumber(bound), "DP sum bound");
					} else {
						ExecuteOrThrow(con, "RESET dp_sum_bound", "reset DP sum bound");
					}
					auto dp = RunMeasuredQuery(con, query, qnum, "dp_elastic", run);
					dp.epsilon = FormatNumber(epsilon);
					dp.delta_scenario = "auto_flex";
					dp.dp_delta = FormatNumber(std::exp(-epsilon * std::pow(std::log(privacy_units), 2.0)));
					dp.bound_scenario = has_sum_avg[q] ? "perfect" : "";
					dp.dp_sum_bound = has_sum_avg[q] ? FormatNumber(perfect_bounds[q] > 0 ? perfect_bounds[q] : 1.0) : "";
					dp.seed = seed;
					results.push_back(dp);
					Log("Q" + std::to_string(qnum) + " dp_elastic epsilon=" + FormatNumber(epsilon) + " run " +
					    std::to_string(run) + (dp.success ? " ok " : " failed ") + FormatNumber(dp.time_ms) + " ms");
				}
			}
		}

		std::ofstream csv(actual_out, std::ofstream::out | std::ofstream::trunc);
		if (!csv.is_open()) {
			throw std::runtime_error("failed to open output CSV: " + actual_out);
		}
		csv << "query,mode,run,time_ms,success,error,utility,recall,precision,epsilon,delta_scenario,dp_delta,"
		       "bound_scenario,dp_sum_bound,pac_mi,seed\n";
		for (const auto &r : results) {
			csv << r.query_num << "," << r.mode << "," << r.run << "," << FormatNumber(r.time_ms) << ","
			    << (r.success ? "true" : "false") << "," << CsvQuote(r.error_msg) << "," << r.utility << ","
			    << r.recall << "," << r.precision << "," << r.epsilon << "," << r.delta_scenario << ","
			    << r.dp_delta << "," << r.bound_scenario << "," << r.dp_sum_bound << "," << r.pac_mi << ","
			    << r.seed << "\n";
		}
		Log("Results written to: " + actual_out);
		return 0;
	} catch (std::exception &ex) {
		Log("Error running IMDb benchmark: " + string(ex.what()));
		return 2;
	}
}

} // namespace duckdb

static void PrintUsage() {
	std::cout << "Usage: imdb_benchmark [options]\n"
	          << "Options:\n"
	          << "  --micro             Run the built-in skewed micro fixture\n"
	          << "  --db <path>         DuckDB database path (default: imdb.db or imdb_micro.db)\n"
	          << "  --queries <path>    Query file (default: benchmark/imdb/queries.sql)\n"
	          << "  --out <csv>         Output CSV path\n"
	          << "  --data-dir <dir>    Directory containing/extracting JOB CSVs\n"
	          << "  --url <url>         Dataset URL (default: CedarDB JOB imdb.tgz mirror)\n"
	          << "  --runs <n>          Number of benchmark runs (default: 3, or 1 with --micro)\n"
	          << "  -h, --help          Show this help message\n";
}

int main(int argc, char **argv) {
	bool micro = false;
	std::string db_path;
	std::string queries_path;
	std::string out_csv;
	std::string data_dir;
	std::string download_url;
	int runs = 0;

	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "-h" || arg == "--help") {
			PrintUsage();
			return 0;
		} else if (arg == "--micro") {
			micro = true;
		} else if (arg == "--db" && i + 1 < argc) {
			db_path = argv[++i];
		} else if (arg == "--queries" && i + 1 < argc) {
			queries_path = argv[++i];
		} else if (arg == "--out" && i + 1 < argc) {
			out_csv = argv[++i];
		} else if (arg == "--data-dir" && i + 1 < argc) {
			data_dir = argv[++i];
		} else if (arg == "--url" && i + 1 < argc) {
			download_url = argv[++i];
		} else if (arg == "--runs" && i + 1 < argc) {
			runs = std::atoi(argv[++i]);
		} else {
			std::cerr << "Unknown option: " << arg << "\n";
			PrintUsage();
			return 1;
		}
	}

	if (runs <= 0) {
		runs = micro ? 1 : 3;
	}

	return duckdb::RunIMDBBenchmark(db_path, queries_path, out_csv, data_dir, download_url, micro, runs);
}
