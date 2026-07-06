// Unified DP utility benchmark runner.
//
// This runner is intentionally DP-only. PAC benchmark binaries remain separate.
// It executes configured workloads under dp_standard, dp_elastic, and/or dp_sass,
// enables privacy_diffcols, and writes utility metrics plus the concrete privacy
// and bound parameters used for each row.

#include "duckdb.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/main/connection.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace duckdb {

enum class JsonType : uint8_t { NIL, BOOL, NUMBER, STRING, ARRAY, OBJECT };
enum class PrivacyProfile : uint8_t { CUSTOMER, PART };

struct JsonValue {
	JsonType type = JsonType::NIL;
	bool boolean = false;
	double number = 0;
	string str;
	vector<JsonValue> array;
	std::map<string, JsonValue> object;

	bool Has(const string &key) const {
		return type == JsonType::OBJECT && object.find(key) != object.end();
	}

	const JsonValue &Get(const string &key) const {
		static JsonValue nil;
		if (type != JsonType::OBJECT) {
			return nil;
		}
		auto it = object.find(key);
		return it == object.end() ? nil : it->second;
	}
};

struct QuerySpec {
	string name;
	string description;
	idx_t key_cols = 0;
	PrivacyProfile profile = PrivacyProfile::CUSTOMER;
	string sql;
	string path;

	QuerySpec() {
	}

	QuerySpec(string name_p, string description_p, idx_t key_cols_p, PrivacyProfile profile_p, string sql_p)
	    : name(std::move(name_p)), description(std::move(description_p)), key_cols(key_cols_p), profile(profile_p),
	      sql(std::move(sql_p)) {
	}
};

struct DatasetConfig {
	string name;
	string workload;
	string db_path;
	string queries_dir;
	string queries_file;
	vector<string> query_paths;
	vector<string> query_names;
	string privacy_sql;
	string setup_sql;
	string load_sql;
	double scale_factor = 1.0;
	idx_t key_cols = 0;
	idx_t query_limit = 0;
	bool allow_create = false;
	vector<double> epsilons;
	vector<double> deltas;
	vector<double> count_bounds;
	vector<double> sum_bounds;
	vector<double> sass_count_output_bounds;
	vector<double> sass_sum_output_bounds;
	vector<double> group_bounds;
	vector<double> c_u_values;
	vector<double> bound_multipliers;
	vector<int> sample_lanes;
	vector<string> modes;
	vector<string> sass_releases;
};

struct Config {
	string out_path = "benchmark/dp/dp_benchmark_results.csv";
	idx_t runs = 1;
	int threads = 1;
	uint64_t seed_base = 1000003;
	vector<string> modes = {"dp_sass"};
	vector<double> epsilons = {1.0};
	vector<double> deltas;
	vector<double> count_bounds = {1000.0};
	vector<double> sum_bounds = {1000.0};
	vector<double> sass_count_output_bounds;
	vector<double> sass_sum_output_bounds;
	vector<double> group_bounds = {1.0};
	vector<double> c_u_values;
	vector<double> bound_multipliers = {1.0};
	vector<int> sample_lanes = {1};
	vector<string> sass_releases = {"median"};
	vector<DatasetConfig> datasets;
	bool dry_run = false;
};

struct UtilityMetrics {
	string utility;
	string recall;
	string precision;
	string median_error_pct;
};

struct RunPoint {
	string mode;
	string release = "-";
	double epsilon = 1.0;
	double delta = 0.0;
	double count_bound = 1000.0;
	double sum_bound = 1000.0;
	double sass_count_output_bound = 0.0;
	double sass_sum_output_bound = 0.0;
	double group_bound = 1.0;
	double c_u = 1.0;
	double bound_multiplier = 1.0;
	int sample_lanes = 1;
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

static string Trim(const string &s) {
	size_t start = s.find_first_not_of(" \t\r\n");
	if (start == string::npos) {
		return "";
	}
	size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(start, end - start + 1);
}

static string FormatNumber(double v) {
	std::ostringstream oss;
	oss << std::setprecision(15) << std::defaultfloat << v;
	string s = oss.str();
	if (s.find('e') == string::npos && s.find('E') == string::npos) {
		auto pos = s.find('.');
		if (pos != string::npos) {
			while (!s.empty() && s.back() == '0') {
				s.pop_back();
			}
			if (!s.empty() && s.back() == '.') {
				s.pop_back();
			}
		}
	}
	return s;
}

static string CsvQuote(const string &s) {
	string out = "\"";
	for (char c : s) {
		if (c == '"') {
			out += "\"\"";
		} else if (c == '\n' || c == '\r') {
			out += ' ';
		} else {
			out += c;
		}
	}
	out += "\"";
	return out;
}

static string CleanError(string error) {
	auto stack_pos = error.find("\n\nStack Trace");
	if (stack_pos != string::npos) {
		error = error.substr(0, stack_pos);
	}
	if (error.size() > 500) {
		error = error.substr(0, 500);
	}
	return error;
}

static string SqlQuote(const string &s) {
	string out = "'";
	for (char c : s) {
		out += c == '\'' ? "''" : string(1, c);
	}
	out += "'";
	return out;
}

static bool FileExists(const string &path) {
	struct stat buffer;
	return stat(path.c_str(), &buffer) == 0;
}

static string ReadFileToString(const string &path) {
	std::ifstream in(path);
	if (!in.is_open()) {
		throw std::runtime_error("could not open file: " + path);
	}
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

static vector<string> SplitSqlStatements(const string &sql) {
	vector<string> statements;
	string current;
	bool in_string = false;
	for (idx_t i = 0; i < sql.size(); i++) {
		char c = sql[i];
		if (c == '\'') {
			current += c;
			if (in_string && i + 1 < sql.size() && sql[i + 1] == '\'') {
				current += sql[++i];
				continue;
			}
			in_string = !in_string;
			continue;
		}
		if (!in_string && c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
			while (i < sql.size() && sql[i] != '\n') {
				i++;
			}
			current += '\n';
			continue;
		}
		if (!in_string && c == ';') {
			string stmt = Trim(current);
			if (!stmt.empty()) {
				statements.push_back(stmt);
			}
			current.clear();
		} else {
			current += c;
		}
	}
	string stmt = Trim(current);
	if (!stmt.empty()) {
		statements.push_back(stmt);
	}
	return statements;
}

static void RunStatement(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	if (!result || result->HasError()) {
		throw std::runtime_error((result ? result->GetError() : "null result") + " while running: " + sql);
	}
}

static void RunScript(Connection &con, const string &sql) {
	for (const auto &stmt : SplitSqlStatements(sql)) {
		RunStatement(con, stmt);
	}
}

static void MaterializeQuery(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	if (!result || result->HasError()) {
		throw std::runtime_error(result ? result->GetError() : "null query result");
	}
	while (result->Fetch()) {
	}
}

static double ReadScalarDouble(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	if (!result || result->HasError()) {
		throw std::runtime_error(result ? result->GetError() : "null query result");
	}
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() == 0 || chunk->GetValue(0, 0).IsNull()) {
		return 0.0;
	}
	return chunk->GetValue(0, 0).DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
}

static bool TableHasRows(Connection &con, const string &table_name) {
	try {
		return ReadScalarDouble(con, "SELECT COUNT(*) FROM " + table_name) > 0.0;
	} catch (...) {
		return false;
	}
}

class JsonParser {
public:
	explicit JsonParser(string input_p) : input(std::move(input_p)) {
	}

	JsonValue Parse() {
		SkipWs();
		auto value = ParseValue();
		SkipWs();
		if (pos != input.size()) {
			throw std::runtime_error("unexpected trailing JSON at byte " + std::to_string(pos));
		}
		return value;
	}

private:
	string input;
	idx_t pos = 0;

	void SkipWs() {
		while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) {
			pos++;
		}
	}

	bool Consume(char c) {
		SkipWs();
		if (pos < input.size() && input[pos] == c) {
			pos++;
			return true;
		}
		return false;
	}

	void Expect(char c) {
		if (!Consume(c)) {
			throw std::runtime_error(string("expected '") + c + "' at byte " + std::to_string(pos));
		}
	}

	JsonValue ParseValue() {
		SkipWs();
		if (pos >= input.size()) {
			throw std::runtime_error("unexpected end of JSON");
		}
		char c = input[pos];
		if (c == '{') {
			return ParseObject();
		}
		if (c == '[') {
			return ParseArray();
		}
		if (c == '"') {
			JsonValue v;
			v.type = JsonType::STRING;
			v.str = ParseString();
			return v;
		}
		if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
			return ParseNumber();
		}
		if (input.compare(pos, 4, "true") == 0) {
			pos += 4;
			JsonValue v;
			v.type = JsonType::BOOL;
			v.boolean = true;
			return v;
		}
		if (input.compare(pos, 5, "false") == 0) {
			pos += 5;
			JsonValue v;
			v.type = JsonType::BOOL;
			v.boolean = false;
			return v;
		}
		if (input.compare(pos, 4, "null") == 0) {
			pos += 4;
			return JsonValue();
		}
		throw std::runtime_error("unexpected JSON token at byte " + std::to_string(pos));
	}

	JsonValue ParseObject() {
		JsonValue v;
		v.type = JsonType::OBJECT;
		Expect('{');
		if (Consume('}')) {
			return v;
		}
		while (true) {
			SkipWs();
			if (pos >= input.size() || input[pos] != '"') {
				throw std::runtime_error("expected object key at byte " + std::to_string(pos));
			}
			string key = ParseString();
			Expect(':');
			v.object[key] = ParseValue();
			if (Consume('}')) {
				break;
			}
			Expect(',');
		}
		return v;
	}

	JsonValue ParseArray() {
		JsonValue v;
		v.type = JsonType::ARRAY;
		Expect('[');
		if (Consume(']')) {
			return v;
		}
		while (true) {
			v.array.push_back(ParseValue());
			if (Consume(']')) {
				break;
			}
			Expect(',');
		}
		return v;
	}

	string ParseString() {
		Expect('"');
		string out;
		while (pos < input.size()) {
			char c = input[pos++];
			if (c == '"') {
				return out;
			}
			if (c == '\\') {
				if (pos >= input.size()) {
					throw std::runtime_error("unterminated JSON escape");
				}
				char esc = input[pos++];
				switch (esc) {
				case '"':
				case '\\':
				case '/':
					out += esc;
					break;
				case 'b':
					out += '\b';
					break;
				case 'f':
					out += '\f';
					break;
				case 'n':
					out += '\n';
					break;
				case 'r':
					out += '\r';
					break;
				case 't':
					out += '\t';
					break;
				default:
					throw std::runtime_error("unsupported JSON escape");
				}
			} else {
				out += c;
			}
		}
		throw std::runtime_error("unterminated JSON string");
	}

	JsonValue ParseNumber() {
		idx_t start = pos;
		if (input[pos] == '-') {
			pos++;
		}
		while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) {
			pos++;
		}
		if (pos < input.size() && input[pos] == '.') {
			pos++;
			while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) {
				pos++;
			}
		}
		if (pos < input.size() && (input[pos] == 'e' || input[pos] == 'E')) {
			pos++;
			if (pos < input.size() && (input[pos] == '+' || input[pos] == '-')) {
				pos++;
			}
			while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) {
				pos++;
			}
		}
		JsonValue v;
		v.type = JsonType::NUMBER;
		v.number = std::stod(input.substr(start, pos - start));
		return v;
	}
};

static string JsonString(const JsonValue &obj, const string &key, const string &default_value = "") {
	const auto &v = obj.Get(key);
	return v.type == JsonType::STRING ? v.str : default_value;
}

static double JsonNumber(const JsonValue &obj, const string &key, double default_value) {
	const auto &v = obj.Get(key);
	return v.type == JsonType::NUMBER ? v.number : default_value;
}

static idx_t JsonIdx(const JsonValue &obj, const string &key, idx_t default_value) {
	const auto &v = obj.Get(key);
	return v.type == JsonType::NUMBER ? static_cast<idx_t>(v.number) : default_value;
}

static bool JsonBool(const JsonValue &obj, const string &key, bool default_value) {
	const auto &v = obj.Get(key);
	return v.type == JsonType::BOOL ? v.boolean : default_value;
}

static vector<string> JsonStringList(const JsonValue &obj, const string &key, const vector<string> &default_value) {
	const auto &v = obj.Get(key);
	if (v.type == JsonType::STRING) {
		return {v.str};
	}
	if (v.type != JsonType::ARRAY) {
		return default_value;
	}
	vector<string> result;
	for (auto &entry : v.array) {
		if (entry.type == JsonType::STRING) {
			result.push_back(entry.str);
		}
	}
	return result.empty() ? default_value : result;
}

static vector<double> JsonNumberList(const JsonValue &obj, const string &array_key, const string &scalar_key,
                                     const vector<double> &default_value) {
	const auto &scalar = obj.Get(scalar_key);
	if (scalar.type == JsonType::NUMBER) {
		return {scalar.number};
	}
	const auto &array = obj.Get(array_key);
	if (array.type != JsonType::ARRAY) {
		return default_value;
	}
	vector<double> result;
	for (auto &entry : array.array) {
		if (entry.type == JsonType::NUMBER) {
			result.push_back(entry.number);
		}
	}
	return result.empty() ? default_value : result;
}

static vector<int> JsonIntList(const JsonValue &obj, const string &array_key, const string &scalar_key,
                               const vector<int> &default_value) {
	const auto &scalar = obj.Get(scalar_key);
	if (scalar.type == JsonType::NUMBER) {
		return {static_cast<int>(scalar.number)};
	}
	const auto &array = obj.Get(array_key);
	if (array.type != JsonType::ARRAY) {
		return default_value;
	}
	vector<int> result;
	for (auto &entry : array.array) {
		if (entry.type == JsonType::NUMBER) {
			result.push_back(static_cast<int>(entry.number));
		}
	}
	return result.empty() ? default_value : result;
}

static bool IsValidMode(const string &mode) {
	return mode == "dp_standard" || mode == "dp_elastic" || mode == "dp_sass";
}

static vector<string> NormalizeModes(vector<string> modes) {
	if (modes.size() == 1 && modes[0] == "all") {
		modes = {"dp_standard", "dp_elastic", "dp_sass"};
	}
	for (auto &mode : modes) {
		if (!IsValidMode(mode)) {
			throw std::runtime_error("unknown DP mode: " + mode);
		}
	}
	return modes;
}

static Config LoadConfig(const string &path) {
	JsonParser parser(ReadFileToString(path));
	auto root = parser.Parse();
	if (root.type != JsonType::OBJECT) {
		throw std::runtime_error("config root must be a JSON object");
	}

	Config config;
	config.out_path = JsonString(root, "out", config.out_path);
	config.runs = JsonIdx(root, "runs", config.runs);
	config.threads = static_cast<int>(JsonNumber(root, "threads", config.threads));
	config.seed_base = static_cast<uint64_t>(JsonNumber(root, "seed_base", static_cast<double>(config.seed_base)));
	config.dry_run = JsonBool(root, "dry_run", config.dry_run);
	config.modes = NormalizeModes(JsonStringList(root, "modes", config.modes));
	config.epsilons = JsonNumberList(root, "epsilons", "epsilon", config.epsilons);
	config.deltas = JsonNumberList(root, "deltas", "delta", config.deltas);
	config.count_bounds = JsonNumberList(root, "count_bounds", "count_bound", config.count_bounds);
	config.sum_bounds = JsonNumberList(root, "sum_bounds", "sum_bound", config.sum_bounds);
	config.sass_count_output_bounds =
	    JsonNumberList(root, "sass_count_output_bounds", "sass_count_output_bound", config.sass_count_output_bounds);
	config.sass_sum_output_bounds =
	    JsonNumberList(root, "sass_sum_output_bounds", "sass_sum_output_bound", config.sass_sum_output_bounds);
	config.group_bounds = JsonNumberList(root, "group_bounds", "group_bound", config.group_bounds);
	config.c_u_values = JsonNumberList(root, "c_u", "c_u", config.c_u_values);
	config.bound_multipliers = JsonNumberList(root, "bound_multipliers", "bound_multiplier", config.bound_multipliers);
	config.sample_lanes = JsonIntList(root, "sample_lanes", "sample_lanes", config.sample_lanes);
	config.sass_releases = JsonStringList(root, "sass_releases", config.sass_releases);

	const auto &datasets = root.Get("datasets");
	if (datasets.type != JsonType::ARRAY || datasets.array.empty()) {
		throw std::runtime_error("config must contain a non-empty datasets array");
	}
	for (auto &entry : datasets.array) {
		if (entry.type != JsonType::OBJECT) {
			throw std::runtime_error("dataset entries must be JSON objects");
		}
		DatasetConfig ds;
		ds.name = JsonString(entry, "name");
		ds.workload = JsonString(entry, "workload", ds.name);
		ds.db_path = JsonString(entry, "db");
		ds.queries_dir = JsonString(entry, "queries_dir");
		ds.queries_file = JsonString(entry, "queries_file");
		ds.query_paths = JsonStringList(entry, "queries", {});
		ds.query_names = JsonStringList(entry, "query_names", {});
		ds.privacy_sql = JsonString(entry, "privacy_sql");
		ds.setup_sql = JsonString(entry, "setup_sql");
		ds.load_sql = JsonString(entry, "load_sql");
		ds.scale_factor = JsonNumber(entry, "sf", ds.scale_factor);
		ds.key_cols = JsonIdx(entry, "key_cols", ds.key_cols);
		ds.query_limit = JsonIdx(entry, "query_limit", ds.query_limit);
		ds.allow_create = JsonBool(entry, "allow_create", ds.allow_create);
		ds.modes = NormalizeModes(JsonStringList(entry, "modes", config.modes));
		ds.epsilons = JsonNumberList(entry, "epsilons", "epsilon", config.epsilons);
		ds.deltas = JsonNumberList(entry, "deltas", "delta", config.deltas);
		ds.count_bounds = JsonNumberList(entry, "count_bounds", "count_bound", config.count_bounds);
		ds.sum_bounds = JsonNumberList(entry, "sum_bounds", "sum_bound", config.sum_bounds);
		ds.sass_count_output_bounds = JsonNumberList(entry, "sass_count_output_bounds", "sass_count_output_bound",
		                                             config.sass_count_output_bounds);
		ds.sass_sum_output_bounds =
		    JsonNumberList(entry, "sass_sum_output_bounds", "sass_sum_output_bound", config.sass_sum_output_bounds);
		ds.group_bounds = JsonNumberList(entry, "group_bounds", "group_bound", config.group_bounds);
		ds.c_u_values = JsonNumberList(entry, "c_u", "c_u", config.c_u_values);
		ds.bound_multipliers = JsonNumberList(entry, "bound_multipliers", "bound_multiplier", config.bound_multipliers);
		ds.sample_lanes = JsonIntList(entry, "sample_lanes", "sample_lanes", config.sample_lanes);
		ds.sass_releases = JsonStringList(entry, "sass_releases", config.sass_releases);
		if (ds.name.empty()) {
			throw std::runtime_error("dataset is missing name");
		}
		config.datasets.push_back(std::move(ds));
	}
	return config;
}

static vector<string> CollectQueryFiles(const string &dir, idx_t limit) {
	vector<string> files;
	DIR *d = opendir(dir.c_str());
	if (!d) {
		throw std::runtime_error("could not open queries_dir: " + dir);
	}
	struct dirent *entry;
	while ((entry = readdir(d)) != nullptr) {
		string name = entry->d_name;
		if (name.size() > 4 && name.substr(name.size() - 4) == ".sql") {
			files.push_back(dir + "/" + name);
		}
	}
	closedir(d);
	std::sort(files.begin(), files.end());
	if (limit > 0 && files.size() > limit) {
		files.resize(limit);
	}
	return files;
}

static string QueryNameFromPath(const string &path) {
	auto pos = path.find_last_of('/');
	string name = pos == string::npos ? path : path.substr(pos + 1);
	if (name.size() > 4 && name.substr(name.size() - 4) == ".sql") {
		name = name.substr(0, name.size() - 4);
	}
	return name;
}

static vector<QuerySpec> ReadQueriesFromFile(const string &path, idx_t key_cols, idx_t limit) {
	vector<QuerySpec> queries;
	auto statements = SplitSqlStatements(ReadFileToString(path));
	for (idx_t i = 0; i < statements.size(); i++) {
		QuerySpec query;
		query.name = "q" + std::to_string(i + 1);
		query.description = path;
		query.key_cols = key_cols;
		query.sql = statements[i];
		query.path = path;
		queries.push_back(std::move(query));
		if (limit > 0 && queries.size() >= limit) {
			break;
		}
	}
	return queries;
}

static vector<QuerySpec> ReadQueriesFromPaths(const vector<string> &paths, idx_t key_cols, idx_t limit) {
	vector<QuerySpec> queries;
	for (auto &path : paths) {
		QuerySpec query;
		query.name = QueryNameFromPath(path);
		query.description = path;
		query.key_cols = key_cols;
		query.sql = ReadFileToString(path);
		query.path = path;
		queries.push_back(std::move(query));
		if (limit > 0 && queries.size() >= limit) {
			break;
		}
	}
	return queries;
}

static vector<QuerySpec> GetTpchStockDpQueries() {
	return {{"q01", "TPC-H Q1 pricing summary report", 2, PrivacyProfile::CUSTOMER,
	         R"(SELECT
    l_returnflag,
    l_linestatus,
    sum(l_quantity) AS sum_qty,
    sum(l_extendedprice) AS sum_base_price,
    sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
    sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
    avg(l_quantity) AS avg_qty,
    avg(l_extendedprice) AS avg_price,
    avg(l_discount) AS avg_disc,
    count(*) AS count_order
FROM lineitem
WHERE l_shipdate <= CAST('1998-09-02' AS date)
GROUP BY l_returnflag, l_linestatus
ORDER BY l_returnflag, l_linestatus)"},
	        {"q05", "TPC-H Q5 local supplier volume", 1, PrivacyProfile::CUSTOMER,
	         R"(SELECT
    n_name,
    sum(l_extendedprice * (1 - l_discount)) AS revenue
FROM customer, orders, lineitem, supplier, nation, region
WHERE c_custkey = o_custkey
  AND l_orderkey = o_orderkey
  AND l_suppkey = s_suppkey
  AND c_nationkey = s_nationkey
  AND s_nationkey = n_nationkey
  AND n_regionkey = r_regionkey
  AND r_name = 'ASIA'
  AND o_orderdate >= CAST('1994-01-01' AS date)
  AND o_orderdate < CAST('1995-01-01' AS date)
GROUP BY n_name
ORDER BY revenue DESC)"},
	        {"q06", "TPC-H Q6 forecast revenue change", 0, PrivacyProfile::CUSTOMER,
	         R"(SELECT
    sum(l_extendedprice * l_discount) AS revenue
FROM lineitem
WHERE l_shipdate >= CAST('1994-01-01' AS date)
  AND l_shipdate < CAST('1995-01-01' AS date)
  AND l_discount BETWEEN 0.05 AND 0.07
  AND l_quantity < 24)"},
	        {"q14", "TPC-H Q14 promotion effect", 0, PrivacyProfile::CUSTOMER,
	         R"(SELECT
    100.00 * sum(CASE
        WHEN p_type LIKE 'PROMO%'
        THEN l_extendedprice * (1 - l_discount)
        ELSE 0
    END) / sum(l_extendedprice * (1 - l_discount)) AS promo_revenue
FROM lineitem, part
WHERE l_partkey = p_partkey
  AND l_shipdate >= CAST('1995-09-01' AS date)
  AND l_shipdate < CAST('1995-10-01' AS date))"},
	        {"q19", "TPC-H Q19 discount revenue", 0, PrivacyProfile::CUSTOMER,
	         R"(SELECT
    sum(l_extendedprice * (1 - l_discount)) AS revenue
FROM lineitem, part
WHERE p_partkey = l_partkey
  AND l_shipmode IN ('AIR', 'AIR REG')
  AND l_shipinstruct = 'DELIVER IN PERSON'
  AND (
    (
      p_brand = 'Brand#12'
      AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG')
      AND l_quantity >= 1 AND l_quantity <= 11
      AND p_size BETWEEN 1 AND 5
    )
    OR (
      p_brand = 'Brand#23'
      AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK')
      AND l_quantity >= 10 AND l_quantity <= 20
      AND p_size BETWEEN 1 AND 10
    )
    OR (
      p_brand = 'Brand#34'
      AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG')
      AND l_quantity >= 20 AND l_quantity <= 30
      AND p_size BETWEEN 1 AND 15
    )
  ))"}};
}

static void LoadTpch(Connection &con) {
	RunStatement(con, "INSTALL tpch");
	RunStatement(con, "LOAD tpch");
}

static void GenerateJcchSkew(Connection &con, double scale_factor) {
	int64_t n_whales = std::max<int64_t>(10, static_cast<int64_t>(150.0 * scale_factor));
	Log("Applying JCC-H skew to " + std::to_string(n_whales) + " whale customers");
	RunStatement(con, "UPDATE orders SET o_custkey = 1 + (o_orderkey % " + std::to_string(n_whales) +
	                      ") WHERE o_orderkey % 10 < 3");
	RunStatement(con, "CHECKPOINT");
}

static void EnsureTpchData(Connection &con, const DatasetConfig &dataset) {
	if (TableHasRows(con, "customer")) {
		return;
	}
	LoadTpch(con);
	Log("Generating " + dataset.name + " data at SF " + FormatNumber(dataset.scale_factor));
	RunStatement(con, "CALL dbgen(sf=" + FormatNumber(dataset.scale_factor) + ")");
	if (dataset.name == "jcch") {
		GenerateJcchSkew(con, dataset.scale_factor);
	}
}

static void SetupCustomerPrivacy(Connection &con) {
	RunStatement(con, "PRAGMA clear_privacy_metadata");
	RunStatement(con, "ALTER TABLE customer ADD PRIVACY_KEY (c_custkey)");
	RunStatement(con, "ALTER TABLE customer SET PU");
	RunStatement(con, "ALTER TABLE orders ADD PRIVACY_LINK (o_custkey) REFERENCES customer(c_custkey)");
	RunStatement(con, "ALTER TABLE lineitem ADD PRIVACY_LINK (l_orderkey) REFERENCES orders(o_orderkey)");
}

static void SetupPartPrivacy(Connection &con) {
	RunStatement(con, "PRAGMA clear_privacy_metadata");
	RunStatement(con, "ALTER TABLE part ADD PRIVACY_KEY (p_partkey)");
	RunStatement(con, "ALTER TABLE part SET PU");
	RunStatement(con, "ALTER PU TABLE part ADD PROTECTED (p_partkey)");
	RunStatement(con, "ALTER TABLE partsupp ADD PRIVACY_LINK (ps_partkey) REFERENCES part(p_partkey)");
}

static void SetupTpchPrivacy(Connection &con, PrivacyProfile profile) {
	if (profile == PrivacyProfile::PART) {
		SetupPartPrivacy(con);
		return;
	}
	SetupCustomerPrivacy(con);
}

static double ReadTpchPrivacyUnitCount(Connection &con, PrivacyProfile profile) {
	if (profile == PrivacyProfile::PART) {
		return ReadScalarDouble(con, "SELECT COUNT(DISTINCT p_partkey) FROM part");
	}
	return ReadScalarDouble(con, "SELECT COUNT(DISTINCT c_custkey) FROM customer");
}

static double DefaultDelta(double epsilon, double privacy_units) {
	if (privacy_units <= 1.0 || !std::isfinite(privacy_units)) {
		return 1e-6;
	}
	double log_pu = std::log(privacy_units);
	return std::exp(-epsilon * log_pu * log_pu);
}

static double DeriveSassOutputBound(double per_pu_bound, double pu_count, int sample_lanes,
                                    const string &setting_name) {
	(void)sample_lanes;
	if (pu_count <= 0.0 || !std::isfinite(pu_count)) {
		throw std::runtime_error("cannot derive " + setting_name + " without a finite privacy unit count");
	}
	// SASS aggregate finalizers rescale every lane by DpSampleRescale(sample_lanes)
	// before dp_smooth_median_noise / dp_gupt_mean_noise clips to this public output
	// domain. Therefore the terminal bound must be on the rescaled full-output scale,
	// not the raw per-lane sample scale.
	return std::max(1.0, per_pu_bound * std::ceil(pu_count));
}

static bool ReadLastUtilityLine(const string &path, UtilityMetrics &metrics) {
	std::ifstream in(path);
	if (!in.is_open()) {
		return false;
	}
	string line;
	string last;
	while (std::getline(in, line)) {
		line = Trim(line);
		if (!line.empty()) {
			last = line;
		}
	}
	if (last.empty()) {
		return false;
	}
	std::istringstream ss(last);
	if (!(std::getline(ss, metrics.utility, ',') && std::getline(ss, metrics.recall, ',') &&
	      std::getline(ss, metrics.precision, ','))) {
		return false;
	}
	if (!std::getline(ss, metrics.median_error_pct, ',')) {
		metrics.median_error_pct = "";
	}
	return true;
}

static string DefaultDbPath(const DatasetConfig &dataset) {
	if (!dataset.db_path.empty()) {
		return dataset.db_path;
	}
	if (dataset.name == "tpch" || dataset.name == "jcch") {
		return dataset.name + "_dp_sf" + FormatNumber(dataset.scale_factor) + ".db";
	}
	if (dataset.name == "sqlstorm_tpch") {
		return "tpch_sf" + FormatNumber(dataset.scale_factor) + "_sqlstorm_dp.db";
	}
	if (dataset.name == "sqlstorm_stackoverflow") {
		return "stackoverflow_dba_sqlstorm.db";
	}
	return dataset.name + "_dp.db";
}

static vector<QuerySpec> LoadDatasetQueries(const DatasetConfig &dataset) {
	if (dataset.name == "tpch" || dataset.name == "jcch") {
		if (dataset.workload != "tpch_stock_dp" && dataset.workload != "jcch_stock_dp") {
			throw std::runtime_error("unknown built-in TPC-H/JCC-H workload: " + dataset.workload);
		}
		auto queries = GetTpchStockDpQueries();
		if (dataset.query_limit > 0 && queries.size() > dataset.query_limit) {
			queries.resize(dataset.query_limit);
		}
		return queries;
	}
	if (!dataset.query_paths.empty()) {
		return ReadQueriesFromPaths(dataset.query_paths, dataset.key_cols, dataset.query_limit);
	}
	if (!dataset.queries_file.empty()) {
		return ReadQueriesFromFile(dataset.queries_file, dataset.key_cols, dataset.query_limit);
	}
	string dir = dataset.queries_dir;
	if (dir.empty() && dataset.name == "sqlstorm_tpch") {
		dir = "benchmark/sqlstorm/SQLStorm/v1.0/tpch/queries";
	}
	if (dir.empty() && dataset.name == "sqlstorm_stackoverflow") {
		dir = "benchmark/sqlstorm/SQLStorm/v1.0/stackoverflow/queries";
	}
	if (dir.empty() && dataset.name == "clickbench") {
		return ReadQueriesFromFile("benchmark/clickbench/clickbench_queries/queries.sql", dataset.key_cols,
		                           dataset.query_limit);
	}
	if (dir.empty() && dataset.name == "imdb") {
		return ReadQueriesFromFile("benchmark/imdb/queries.sql", dataset.key_cols, dataset.query_limit);
	}
	if (dir.empty()) {
		throw std::runtime_error("dataset " + dataset.name + " needs queries_dir, queries_file, or queries");
	}
	return ReadQueriesFromPaths(CollectQueryFiles(dir, dataset.query_limit), dataset.key_cols, dataset.query_limit);
}

static string DefaultPrivacySqlPath(const DatasetConfig &dataset) {
	if (!dataset.privacy_sql.empty()) {
		return dataset.privacy_sql;
	}
	if (dataset.name == "sqlstorm_stackoverflow") {
		return "benchmark/sqlstorm/pac_stackoverflow_schema.sql";
	}
	if (dataset.name == "clickbench") {
		return "benchmark/clickbench/clickbench_queries/setup.sql";
	}
	if (dataset.name == "imdb") {
		return "benchmark/imdb/setup.sql";
	}
	return "";
}

static void ApplyGenericPrivacy(Connection &con, const DatasetConfig &dataset) {
	RunStatement(con, "PRAGMA clear_privacy_metadata");
	string setup_path = dataset.setup_sql;
	if (!setup_path.empty()) {
		RunScript(con, ReadFileToString(setup_path));
	}
	string privacy_path = DefaultPrivacySqlPath(dataset);
	if (!privacy_path.empty()) {
		RunScript(con, ReadFileToString(privacy_path));
	}
}

static double ReadGenericPrivacyUnitCount(Connection &con, const DatasetConfig &dataset) {
	if (dataset.name == "sqlstorm_stackoverflow") {
		return ReadScalarDouble(con, "SELECT COUNT(DISTINCT Id) FROM Users");
	}
	if (dataset.name == "clickbench") {
		return ReadScalarDouble(con, "SELECT COUNT(DISTINCT UserID) FROM hits");
	}
	if (dataset.name == "imdb") {
		return ReadScalarDouble(con, "SELECT COUNT(DISTINCT id) FROM name");
	}
	return 0.0;
}

static vector<double> NonEmpty(const vector<double> &values, const vector<double> &fallback) {
	return values.empty() ? fallback : values;
}

static vector<int> NonEmptyInt(const vector<int> &values, const vector<int> &fallback) {
	return values.empty() ? fallback : values;
}

static vector<string> NonEmptyString(const vector<string> &values, const vector<string> &fallback) {
	return values.empty() ? fallback : values;
}

static bool ContainsString(const vector<string> &values, const string &needle) {
	return std::find(values.begin(), values.end(), needle) != values.end();
}

static vector<QuerySpec> FilterQueriesByName(vector<QuerySpec> queries, const vector<string> &names) {
	if (names.empty()) {
		return queries;
	}
	vector<QuerySpec> filtered;
	for (auto &query : queries) {
		if (ContainsString(names, query.name)) {
			filtered.push_back(std::move(query));
		}
	}
	for (auto &name : names) {
		bool found = false;
		for (auto &query : filtered) {
			if (query.name == name) {
				found = true;
				break;
			}
		}
		if (!found) {
			throw std::runtime_error("query_names requested unknown query: " + name);
		}
	}
	return filtered;
}

static vector<RunPoint> BuildRunPoints(const DatasetConfig &dataset) {
	vector<RunPoint> points;
	auto c_u_values = dataset.c_u_values.empty() ? dataset.group_bounds : dataset.c_u_values;
	if (c_u_values.empty()) {
		c_u_values.push_back(1.0);
	}
	for (auto &mode : dataset.modes) {
		auto releases = mode == "dp_sass" ? dataset.sass_releases : vector<string> {"-"};
		auto sass_count_output_bounds =
		    mode == "dp_sass" ? NonEmpty(dataset.sass_count_output_bounds, {0.0}) : vector<double> {0.0};
		auto sass_sum_output_bounds =
		    mode == "dp_sass" ? NonEmpty(dataset.sass_sum_output_bounds, {0.0}) : vector<double> {0.0};
		for (double epsilon : dataset.epsilons) {
			auto deltas = dataset.deltas.empty() ? vector<double> {0.0} : dataset.deltas;
			for (double delta : deltas) {
				for (double count_bound : dataset.count_bounds) {
					for (double sum_bound : dataset.sum_bounds) {
						for (double c_u : c_u_values) {
							for (double multiplier : dataset.bound_multipliers) {
								for (double sass_count_output_bound : sass_count_output_bounds) {
									for (double sass_sum_output_bound : sass_sum_output_bounds) {
										for (int lanes : NonEmptyInt(dataset.sample_lanes, {1})) {
											for (auto &release : releases) {
												RunPoint point;
												point.mode = mode;
												point.release = release;
												point.epsilon = epsilon;
												point.delta = delta;
												point.count_bound = count_bound * multiplier;
												point.sum_bound = sum_bound * multiplier;
												point.sass_count_output_bound = sass_count_output_bound;
												point.sass_sum_output_bound = sass_sum_output_bound;
												point.group_bound = c_u;
												point.c_u = c_u;
												point.bound_multiplier = multiplier;
												point.sample_lanes = lanes;
												points.push_back(std::move(point));
											}
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}
	return points;
}

static void ApplyDpSettings(Connection &con, const RunPoint &point, double delta, double sass_count_output_bound,
                            double sass_sum_output_bound) {
	RunStatement(con, "SET privacy_mode='" + point.mode + "'");
	RunStatement(con, "SET dp_epsilon=" + FormatNumber(point.epsilon));
	if (delta <= 0.0 && (point.mode == "dp_standard" || (point.mode == "dp_sass" && point.release == "average"))) {
		RunStatement(con, "SET dp_delta=NULL");
	} else {
		RunStatement(con, "SET dp_delta=" + FormatNumber(delta));
	}
	RunStatement(con, "SET dp_sum_bound=" + FormatNumber(point.sum_bound));
	RunStatement(con, "SET dp_count_bound=" + FormatNumber(point.count_bound));
	RunStatement(con, "SET dp_max_groups_contributed=" + FormatNumber(point.group_bound));
	if (point.mode == "dp_sass") {
		RunStatement(con, "SET dp_sample_lanes=" + std::to_string(point.sample_lanes));
		RunStatement(con, "SET dp_sass_release='" + point.release + "'");
		RunStatement(con, "SET dp_sass_count_output_bound=" + FormatNumber(sass_count_output_bound));
		RunStatement(con, "SET dp_sass_sum_output_bound=" + FormatNumber(sass_sum_output_bound));
		RunStatement(con, "SET dp_sass_avg_lower_bound=0");
		RunStatement(con, "SET dp_sass_avg_upper_bound=" + FormatNumber(point.sum_bound));
		RunStatement(con, "SET dp_sass_minmax_lower_bound=0");
		RunStatement(con, "SET dp_sass_minmax_upper_bound=" + FormatNumber(point.sum_bound));
	}
}

static void WriteHeader(std::ofstream &csv) {
	csv << "dataset,workload,query,query_id,mode,release,profile,run,success,error,primary_metric,time_ms,"
	       "utility,recall,precision,median_error_pct,epsilon,delta,sf,dp_sum_bound,dp_count_bound,"
	       "dp_max_groups_contributed,c_u,dp_sass_count_output_bound,dp_sass_sum_output_bound,bound_multiplier,"
	       "dp_sample_lanes,seed,db_path,query_path\n";
}

static void WriteRow(std::ofstream &csv, const DatasetConfig &dataset, const QuerySpec &query, idx_t query_id,
                     const RunPoint &point, idx_t run, bool success, const string &error, const UtilityMetrics &metrics,
                     double time_ms, double delta, double sass_count_output_bound, double sass_sum_output_bound,
                     uint64_t seed, const string &db_path) {
	csv << dataset.name << "," << dataset.workload << "," << query.name << "," << query_id << "," << point.mode << ","
	    << point.release << "," << (query.profile == PrivacyProfile::PART ? "part" : "customer") << "," << run << ","
	    << (success ? "true" : "false") << "," << CsvQuote(error) << ",utility," << FormatNumber(time_ms) << ","
	    << metrics.utility << "," << metrics.recall << "," << metrics.precision << "," << metrics.median_error_pct
	    << "," << FormatNumber(point.epsilon) << "," << FormatNumber(delta) << "," << FormatNumber(dataset.scale_factor)
	    << "," << FormatNumber(point.sum_bound) << "," << FormatNumber(point.count_bound) << ","
	    << FormatNumber(point.group_bound) << "," << FormatNumber(point.c_u) << ","
	    << FormatNumber(sass_count_output_bound) << "," << FormatNumber(sass_sum_output_bound) << ","
	    << FormatNumber(point.bound_multiplier) << "," << point.sample_lanes << "," << seed << "," << CsvQuote(db_path)
	    << "," << CsvQuote(query.path) << "\n";
	csv.flush();
}

static void ValidateDataset(const DatasetConfig &dataset) {
	if (dataset.name == "reddit") {
		if (dataset.db_path.empty() && !dataset.allow_create) {
			throw std::runtime_error("reddit requires db or allow_create=true");
		}
		if (dataset.privacy_sql.empty()) {
			throw std::runtime_error("reddit requires privacy_sql");
		}
	}
	if (!dataset.privacy_sql.empty() && !FileExists(dataset.privacy_sql)) {
		throw std::runtime_error("privacy_sql not found: " + dataset.privacy_sql);
	}
	if (!dataset.setup_sql.empty() && !FileExists(dataset.setup_sql)) {
		throw std::runtime_error("setup_sql not found: " + dataset.setup_sql);
	}
	if (!dataset.queries_file.empty() && !FileExists(dataset.queries_file)) {
		throw std::runtime_error("queries_file not found: " + dataset.queries_file);
	}
	for (auto &path : dataset.query_paths) {
		if (!FileExists(path)) {
			throw std::runtime_error("query file not found: " + path);
		}
	}
}

static void PrepareDataset(Connection &con, const DatasetConfig &dataset, bool dry_run) {
	if (dataset.name == "tpch" || dataset.name == "jcch" || dataset.name == "sqlstorm_tpch") {
		if (!dry_run) {
			EnsureTpchData(con, dataset);
		}
		return;
	}
	if (!dataset.load_sql.empty() && !dry_run) {
		RunScript(con, ReadFileToString(dataset.load_sql));
	}
}

static void ApplyDatasetPrivacy(Connection &con, const DatasetConfig &dataset, const QuerySpec &query) {
	if (dataset.name == "tpch" || dataset.name == "jcch") {
		SetupTpchPrivacy(con, query.profile);
		return;
	}
	if (dataset.name == "sqlstorm_tpch") {
		SetupCustomerPrivacy(con);
		return;
	}
	ApplyGenericPrivacy(con, dataset);
}

static double ReadPrivacyUnitCount(Connection &con, const DatasetConfig &dataset, const QuerySpec &query) {
	if (dataset.name == "tpch" || dataset.name == "jcch" || dataset.name == "sqlstorm_tpch") {
		return ReadTpchPrivacyUnitCount(con, query.profile);
	}
	return ReadGenericPrivacyUnitCount(con, dataset);
}

static double ReadBenchmarkPrivacyUnitCount(Connection &con, const DatasetConfig &dataset, const QuerySpec &query) {
	RunStatement(con, "SET priv_rewrite=false");
	try {
		double count = ReadPrivacyUnitCount(con, dataset, query);
		RunStatement(con, "SET priv_rewrite=true");
		return count;
	} catch (...) {
		try {
			RunStatement(con, "SET priv_rewrite=true");
		} catch (...) {
		}
		throw;
	}
}

static void RunDataset(const Config &config, const DatasetConfig &dataset, std::ofstream &csv) {
	ValidateDataset(dataset);
	auto queries = FilterQueriesByName(LoadDatasetQueries(dataset), dataset.query_names);
	if (queries.empty()) {
		throw std::runtime_error("dataset " + dataset.name + " has no queries");
	}
	string db_path = DefaultDbPath(dataset);

	Log("Dataset " + dataset.name + ": " + std::to_string(queries.size()) + " queries");
	if (config.dry_run) {
		for (idx_t i = 0; i < queries.size(); i++) {
			Log("  dry-run query " + std::to_string(i + 1) + ": " + queries[i].name);
		}
		return;
	}

	if (!(dataset.name == "tpch" || dataset.name == "jcch" || dataset.name == "sqlstorm_tpch") &&
	    !dataset.allow_create && !FileExists(db_path)) {
		throw std::runtime_error("database does not exist for dataset " + dataset.name + ": " + db_path);
	}

	DuckDB db(db_path);
	Connection con(db);
	RunStatement(con, "LOAD privacy");
	RunStatement(con, "SET threads=" + std::to_string(config.threads));
	RunStatement(con, "SET privacy_noise=true");
	PrepareDataset(con, dataset, false);

	auto points = BuildRunPoints(dataset);
	string utility_path = "/tmp/dp_benchmark_utility_" + std::to_string(getpid()) + ".csv";
	for (idx_t run = 0; run < config.runs; run++) {
		for (idx_t qi = 0; qi < queries.size(); qi++) {
			const auto &query = queries[qi];
			for (auto &point : points) {
				UtilityMetrics metrics;
				string error;
				bool success = false;
				double time_ms = 0.0;
				uint64_t seed = config.seed_base + 9973ULL * static_cast<uint64_t>(run) +
				                131ULL * static_cast<uint64_t>(qi) + static_cast<uint64_t>(points.size());
				double delta = point.delta;
				double sass_count_output_bound = 0.0;
				double sass_sum_output_bound = 0.0;
				std::remove(utility_path.c_str());
				try {
					ApplyDatasetPrivacy(con, dataset, query);
					double pu_count = ReadBenchmarkPrivacyUnitCount(con, dataset, query);
					if (delta <= 0.0 && point.mode != "dp_standard" &&
					    !(point.mode == "dp_sass" && point.release == "average")) {
						delta = DefaultDelta(point.epsilon, pu_count);
					}
					if (point.mode == "dp_sass") {
						sass_count_output_bound =
						    point.sass_count_output_bound > 0.0
						        ? point.sass_count_output_bound
						        : DeriveSassOutputBound(point.count_bound, pu_count, point.sample_lanes,
						                                "dp_sass_count_output_bound");
						sass_sum_output_bound =
						    point.sass_sum_output_bound > 0.0
						        ? point.sass_sum_output_bound
						        : DeriveSassOutputBound(point.sum_bound, pu_count, point.sample_lanes,
						                                "dp_sass_sum_output_bound");
					}
					ApplyDpSettings(con, point, delta, sass_count_output_bound, sass_sum_output_bound);
					RunStatement(con, "SET privacy_seed=" + std::to_string(seed));
					RunStatement(con, "SET privacy_diffcols=" +
					                      SqlQuote(std::to_string(query.key_cols) + ":" + utility_path));
					auto start = std::chrono::steady_clock::now();
					MaterializeQuery(con, query.sql);
					auto end = std::chrono::steady_clock::now();
					time_ms = std::chrono::duration<double, std::milli>(end - start).count();
					if (!ReadLastUtilityLine(utility_path, metrics)) {
						throw std::runtime_error("privacy_diffcols did not write utility output");
					}
					success = true;
					Log(dataset.name + " " + point.mode + " " + query.name + " run " + std::to_string(run) +
					    " utility=" + metrics.utility);
				} catch (std::exception &ex) {
					error = CleanError(ex.what());
					Log(dataset.name + " " + point.mode + " " + query.name + " run " + std::to_string(run) +
					    " failed: " + error);
				}
				try {
					RunStatement(con, "SET privacy_diffcols=NULL");
				} catch (...) {
				}
				WriteRow(csv, dataset, query, qi + 1, point, run, success, error, metrics, time_ms, delta,
				         sass_count_output_bound, sass_sum_output_bound, seed, db_path);
			}
		}
	}
	std::remove(utility_path.c_str());
}

static void PrintUsage() {
	std::cout << "Usage: dp_benchmark_runner --config PATH [--dry-run] [--out PATH]\n"
	          << "JSON supports scalar or array sweeps for epsilon(s), delta(s), count_bound(s), sum_bound(s),\n"
	          << "group_bound(s), c_u, bound_multiplier(s), sample_lanes, modes, and sass_releases.\n";
}

static int Main(int argc, char **argv) {
	string config_path;
	string out_override;
	bool dry_run = false;
	for (int i = 1; i < argc; i++) {
		string arg = argv[i];
		if (arg == "--help" || arg == "-h") {
			PrintUsage();
			return 0;
		}
		if (arg == "--dry-run") {
			dry_run = true;
			continue;
		}
		if (arg == "--config" && i + 1 < argc) {
			config_path = argv[++i];
			continue;
		}
		if (arg == "--out" && i + 1 < argc) {
			out_override = argv[++i];
			continue;
		}
		auto eq = arg.find('=');
		if (eq != string::npos) {
			string key = arg.substr(0, eq);
			string value = arg.substr(eq + 1);
			if (key == "--config") {
				config_path = value;
				continue;
			}
			if (key == "--out") {
				out_override = value;
				continue;
			}
		}
		throw std::runtime_error("unknown argument: " + arg);
	}
	if (config_path.empty()) {
		throw std::runtime_error("--config is required");
	}

	auto config = LoadConfig(config_path);
	if (!out_override.empty()) {
		config.out_path = out_override;
	}
	if (dry_run) {
		config.dry_run = true;
	}

	std::ofstream csv;
	if (!config.dry_run) {
		csv.open(config.out_path, std::ofstream::out | std::ofstream::trunc);
		if (!csv.is_open()) {
			throw std::runtime_error("could not open output CSV: " + config.out_path);
		}
		WriteHeader(csv);
	}
	for (auto &dataset : config.datasets) {
		RunDataset(config, dataset, csv);
	}
	if (config.dry_run) {
		Log("Dry-run succeeded for " + std::to_string(config.datasets.size()) + " datasets");
	} else {
		Log("Wrote " + config.out_path);
	}
	return 0;
}

} // namespace duckdb

int main(int argc, char **argv) {
	try {
		return duckdb::Main(argc, argv);
	} catch (std::exception &ex) {
		std::cerr << "error: " << ex.what() << std::endl;
		return 1;
	}
}
