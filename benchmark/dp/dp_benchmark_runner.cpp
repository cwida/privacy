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
	vector<string> sum_bound_lists;
	vector<double> avg_lower_bounds;
	vector<double> avg_upper_bounds;
	vector<double> sass_count_output_bounds;
	vector<string> sass_count_output_bound_lists;
	vector<string> sass_count_output_lower_bound_lists;
	vector<string> sass_count_output_upper_bound_lists;
	vector<double> sass_sum_output_bounds;
	vector<string> sass_sum_output_bound_lists;
	vector<string> sass_sum_output_lower_bound_lists;
	vector<string> sass_sum_output_upper_bound_lists;
	vector<double> group_bounds;
	vector<double> support_thresholds;
	vector<double> c_u_values;
	vector<double> bound_multipliers;
	vector<int> sample_lanes;
	vector<int> sass_m_values;
	vector<bool> sass_rescale_values;
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
	vector<string> sum_bound_lists;
	vector<double> avg_lower_bounds;
	vector<double> avg_upper_bounds;
	vector<double> sass_count_output_bounds;
	vector<string> sass_count_output_bound_lists;
	vector<string> sass_count_output_lower_bound_lists;
	vector<string> sass_count_output_upper_bound_lists;
	vector<double> sass_sum_output_bounds;
	vector<string> sass_sum_output_bound_lists;
	vector<string> sass_sum_output_lower_bound_lists;
	vector<string> sass_sum_output_upper_bound_lists;
	vector<double> group_bounds = {1.0};
	vector<double> support_thresholds = {0.0};
	vector<double> c_u_values;
	vector<double> bound_multipliers = {1.0};
	vector<int> sample_lanes = {1};
	vector<int> sass_m_values = {64};
	vector<bool> sass_rescale_values = {true};
	vector<string> sass_releases = {"median"};
	vector<DatasetConfig> datasets;
	bool dry_run = false;
};

struct UtilityMetrics {
	string utility;
	string recall;
	string precision;
	string median_error_pct;
	string q1_sum_median_error_pct;
	string q1_avg_median_error_pct;
	string q1_count_median_error_pct;
	string saa_estimator_utility;
	string saa_estimator_recall;
	string saa_estimator_precision;
	string saa_estimator_median_error_pct;
	string saa_estimator_q1_sum_median_error_pct;
	string saa_estimator_q1_avg_median_error_pct;
	string saa_estimator_q1_count_median_error_pct;
	string saa_sampling_utility;
	string saa_sampling_recall;
	string saa_sampling_precision;
	string saa_sampling_median_error_pct;
	string saa_sampling_q1_sum_median_error_pct;
	string saa_sampling_q1_avg_median_error_pct;
	string saa_sampling_q1_count_median_error_pct;
	string saa_noise_scale_median;
	string saa_noise_scale_mean;
	string saa_noise_scale_max;
	string saa_noise_scale_q1_sum_median;
	string saa_noise_scale_q1_avg_median;
	string saa_noise_scale_q1_count_median;
	string saa_smooth_sensitivity_median;
	string saa_smooth_sensitivity_mean;
	string saa_smooth_sensitivity_max;
	string saa_smooth_sensitivity_q1_sum_median;
	string saa_smooth_sensitivity_q1_avg_median;
	string saa_smooth_sensitivity_q1_count_median;
};

struct RunPoint {
	string mode;
	string release = "-";
	double epsilon = 1.0;
	double delta = 0.0;
	double count_bound = 1000.0;
	double sum_bound = 1000.0;
	string sum_bound_list;
	vector<double> avg_lower_bounds;
	vector<double> avg_upper_bounds;
	double sass_count_output_bound = 0.0;
	string sass_count_output_bound_list;
	string sass_count_output_lower_bound_list;
	string sass_count_output_upper_bound_list;
	double sass_sum_output_bound = 0.0;
	string sass_sum_output_bound_list;
	string sass_sum_output_lower_bound_list;
	string sass_sum_output_upper_bound_list;
	double group_bound = 1.0;
	double support_threshold = 0.0;
	double c_u = 1.0;
	double bound_multiplier = 1.0;
	int sample_lanes = 1;
	int sass_m = 64;
	bool sass_rescale = true;
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

static string FormatNumberList(const vector<double> &values) {
	string out;
	for (idx_t i = 0; i < values.size(); i++) {
		if (i > 0) {
			out += ",";
		}
		out += FormatNumber(values[i]);
	}
	return out;
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

static vector<bool> JsonBoolList(const JsonValue &obj, const string &array_key, const string &scalar_key,
                                 const vector<bool> &default_value) {
	const auto &scalar = obj.Get(scalar_key);
	if (scalar.type == JsonType::BOOL) {
		return {scalar.boolean};
	}
	const auto &array = obj.Get(array_key);
	if (array.type != JsonType::ARRAY) {
		return default_value;
	}
	vector<bool> result;
	for (auto &entry : array.array) {
		if (entry.type == JsonType::BOOL) {
			result.push_back(entry.boolean);
		}
	}
	return result.empty() ? default_value : result;
}

static bool IsValidMode(const string &mode) {
	return mode == "duckdb" || mode == "dp_standard" || mode == "dp_elastic" || mode == "dp_sass" ||
	       mode == "dp_sass_bounded_ratio";
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
	config.sum_bound_lists = JsonStringList(root, "sum_bound_lists", {});
	config.avg_lower_bounds = JsonNumberList(root, "avg_lower_bounds", "avg_lower_bound", config.avg_lower_bounds);
	config.avg_upper_bounds = JsonNumberList(root, "avg_upper_bounds", "avg_upper_bound", config.avg_upper_bounds);
	config.sass_count_output_bounds =
	    JsonNumberList(root, "sass_count_output_bounds", "sass_count_output_bound", config.sass_count_output_bounds);
	config.sass_count_output_bound_lists = JsonStringList(root, "sass_count_output_bound_lists", {});
	config.sass_count_output_lower_bound_lists = JsonStringList(root, "sass_count_output_lower_bound_lists", {});
	config.sass_count_output_upper_bound_lists = JsonStringList(root, "sass_count_output_upper_bound_lists", {});
	config.sass_sum_output_bounds =
	    JsonNumberList(root, "sass_sum_output_bounds", "sass_sum_output_bound", config.sass_sum_output_bounds);
	config.sass_sum_output_bound_lists = JsonStringList(root, "sass_sum_output_bound_lists", {});
	config.sass_sum_output_lower_bound_lists = JsonStringList(root, "sass_sum_output_lower_bound_lists", {});
	config.sass_sum_output_upper_bound_lists = JsonStringList(root, "sass_sum_output_upper_bound_lists", {});
	config.group_bounds = JsonNumberList(root, "group_bounds", "group_bound", config.group_bounds);
	config.support_thresholds =
	    JsonNumberList(root, "support_thresholds", "support_threshold", config.support_thresholds);
	config.c_u_values = JsonNumberList(root, "c_u", "c_u", config.c_u_values);
	config.bound_multipliers = JsonNumberList(root, "bound_multipliers", "bound_multiplier", config.bound_multipliers);
	config.sample_lanes = JsonIntList(root, "sample_lanes", "sample_lanes", config.sample_lanes);
	config.sass_m_values = JsonIntList(root, "sass_ms", "sass_m", config.sass_m_values);
	config.sass_rescale_values = JsonBoolList(root, "sass_rescales", "sass_rescale", config.sass_rescale_values);
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
		ds.sum_bound_lists = JsonStringList(entry, "sum_bound_lists", config.sum_bound_lists);
		ds.avg_lower_bounds = JsonNumberList(entry, "avg_lower_bounds", "avg_lower_bound", config.avg_lower_bounds);
		ds.avg_upper_bounds = JsonNumberList(entry, "avg_upper_bounds", "avg_upper_bound", config.avg_upper_bounds);
		ds.sass_count_output_bounds = JsonNumberList(entry, "sass_count_output_bounds", "sass_count_output_bound",
		                                             config.sass_count_output_bounds);
		ds.sass_count_output_bound_lists =
		    JsonStringList(entry, "sass_count_output_bound_lists", config.sass_count_output_bound_lists);
		ds.sass_count_output_lower_bound_lists =
		    JsonStringList(entry, "sass_count_output_lower_bound_lists", config.sass_count_output_lower_bound_lists);
		ds.sass_count_output_upper_bound_lists =
		    JsonStringList(entry, "sass_count_output_upper_bound_lists", config.sass_count_output_upper_bound_lists);
		ds.sass_sum_output_bounds =
		    JsonNumberList(entry, "sass_sum_output_bounds", "sass_sum_output_bound", config.sass_sum_output_bounds);
		ds.sass_sum_output_bound_lists =
		    JsonStringList(entry, "sass_sum_output_bound_lists", config.sass_sum_output_bound_lists);
		ds.sass_sum_output_lower_bound_lists =
		    JsonStringList(entry, "sass_sum_output_lower_bound_lists", config.sass_sum_output_lower_bound_lists);
		ds.sass_sum_output_upper_bound_lists =
		    JsonStringList(entry, "sass_sum_output_upper_bound_lists", config.sass_sum_output_upper_bound_lists);
		ds.group_bounds = JsonNumberList(entry, "group_bounds", "group_bound", config.group_bounds);
		ds.support_thresholds =
		    JsonNumberList(entry, "support_thresholds", "support_threshold", config.support_thresholds);
		ds.c_u_values = JsonNumberList(entry, "c_u", "c_u", config.c_u_values);
		ds.bound_multipliers = JsonNumberList(entry, "bound_multipliers", "bound_multiplier", config.bound_multipliers);
		ds.sample_lanes = JsonIntList(entry, "sample_lanes", "sample_lanes", config.sample_lanes);
		ds.sass_m_values = JsonIntList(entry, "sass_ms", "sass_m", config.sass_m_values);
		ds.sass_rescale_values = JsonBoolList(entry, "sass_rescales", "sass_rescale", config.sass_rescale_values);
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
	int64_t n_hot_customers = std::max<int64_t>(10, static_cast<int64_t>(150.0 * scale_factor));
	Log("Applying JCC-H skew to " + std::to_string(n_hot_customers) + " hot customers");
	RunStatement(con, "UPDATE orders SET o_custkey = 1 + (o_orderkey % " + std::to_string(n_hot_customers) +
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
	(void)epsilon;
	if (privacy_units <= 1.0 || !std::isfinite(privacy_units)) {
		return 1e-6;
	}
	return 1.0 / privacy_units;
}

static double DpSampleLaneProbability(int sample_lanes) {
	return 1.0 - std::pow(63.0 / 64.0, static_cast<double>(sample_lanes));
}

static double DeriveSassOutputBound(double per_pu_bound, double pu_count, int sample_lanes, int sample_count,
                                    bool rescale, const string &setting_name) {
	if (pu_count <= 0.0 || !std::isfinite(pu_count)) {
		throw std::runtime_error("cannot derive " + setting_name + " without a finite privacy unit count");
	}
	if (rescale) {
		return std::max(1.0, per_pu_bound * std::ceil(pu_count));
	}
	double lane_probability =
	    sample_count > 64 ? (1.0 / static_cast<double>(sample_count)) : DpSampleLaneProbability(sample_lanes);
	return std::max(1.0, per_pu_bound * std::ceil(pu_count * lane_probability));
}

static double ScaleConfiguredSassOutputBound(double output_bound, double multiplier) {
	if (output_bound <= 0.0) {
		return 0.0;
	}
	return output_bound * multiplier;
}

static string ScaleConfiguredBoundList(const string &bounds_text, double multiplier, const string &setting_name) {
	string text = Trim(bounds_text);
	if (text.empty()) {
		return "";
	}
	auto parts = StringUtil::Split(text, ',');
	vector<double> scaled;
	scaled.reserve(parts.size());
	for (auto &part : parts) {
		StringUtil::Trim(part);
		if (part.empty()) {
			throw std::runtime_error(setting_name + " contains an empty entry");
		}
		size_t parsed = 0;
		double value = 0.0;
		try {
			value = std::stod(part, &parsed);
		} catch (std::exception &) {
			throw std::runtime_error("could not parse " + setting_name + " entry '" + part + "'");
		}
		if (parsed != part.size() || !std::isfinite(value) || value <= 0.0) {
			throw std::runtime_error("invalid " + setting_name + " entry '" + part + "'");
		}
		scaled.push_back(value * multiplier);
	}
	return FormatNumberList(scaled);
}

static string ScaleConfiguredFiniteList(const string &bounds_text, double multiplier, const string &setting_name) {
	string text = Trim(bounds_text);
	if (text.empty()) {
		return "";
	}
	auto parts = StringUtil::Split(text, ',');
	vector<double> scaled;
	scaled.reserve(parts.size());
	for (auto &part : parts) {
		StringUtil::Trim(part);
		if (part.empty()) {
			throw std::runtime_error(setting_name + " contains an empty entry");
		}
		size_t parsed = 0;
		double value = 0.0;
		try {
			value = std::stod(part, &parsed);
		} catch (std::exception &) {
			throw std::runtime_error("could not parse " + setting_name + " entry '" + part + "'");
		}
		if (parsed != part.size() || !std::isfinite(value)) {
			throw std::runtime_error("invalid " + setting_name + " entry '" + part + "'");
		}
		scaled.push_back(value * multiplier);
	}
	return FormatNumberList(scaled);
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

static vector<bool> NonEmptyBool(const vector<bool> &values, const vector<bool> &fallback) {
	return values.empty() ? fallback : values;
}

static vector<string> NonEmptyString(const vector<string> &values, const vector<string> &fallback) {
	return values.empty() ? fallback : values;
}

static bool ContainsString(const vector<string> &values, const string &needle) {
	return std::find(values.begin(), values.end(), needle) != values.end();
}

static string BuildQ14BoundedSampledRatioSql(double epsilon, int sample_lanes) {
	return "WITH lanes AS ("
	       "SELECT "
	       "as_sample_sum(hash(o_custkey)::UBIGINT, "
	       "CASE WHEN p_type LIKE 'PROMO%' THEN (l_extendedprice * (1 - l_discount))::DOUBLE ELSE 0.0 END) AS promo, "
	       "as_sample_sum(hash(o_custkey)::UBIGINT, (l_extendedprice * (1 - l_discount))::DOUBLE) AS total "
	       "FROM lineitem JOIN orders ON l_orderkey = o_orderkey JOIN part ON l_partkey = p_partkey "
	       "WHERE l_shipdate >= CAST('1995-09-01' AS date) "
	       "AND l_shipdate < CAST('1995-10-01' AS date)) "
	       "SELECT dp_gupt_mean_noise("
	       "list_transform(range(1, 65), lambda i: "
	       "CASE WHEN list_extract(total, i) <= 0 THEN 0.0 "
	       "ELSE least(100.0, greatest(0.0, 100.0 * list_extract(promo, i) / list_extract(total, i))) END), " +
	       FormatNumber(epsilon) + ", " + std::to_string(sample_lanes) +
	       ", 0.0, 100.0, 0.0) AS promo_revenue "
	       "FROM lanes";
}

static void FillScalarUtility(double released_value, double reference_value, UtilityMetrics &metrics) {
	double abs_ref = std::max(0.00001, std::fabs(reference_value));
	double error_pct = 100.0 * std::fabs(released_value - reference_value) / abs_ref;
	metrics.utility = FormatNumber(error_pct);
	metrics.recall = "1";
	metrics.precision = "1";
	metrics.median_error_pct = FormatNumber(error_pct);
}

static bool TryCastDouble(const Value &value, double &out) {
	if (value.IsNull()) {
		return false;
	}
	try {
		out = value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
		return std::isfinite(out);
	} catch (...) {
		return false;
	}
}

static string ComparisonKey(const vector<Value> &row, idx_t key_cols, idx_t row_index) {
	if (key_cols == 0) {
		return "#" + std::to_string(row_index);
	}
	string key;
	for (idx_t col = 0; col < key_cols; col++) {
		string part = row[col].IsNull() ? "<NULL>" : row[col].ToString();
		key += std::to_string(part.size()) + ":" + part + "|";
	}
	return key;
}

struct ResultSnapshot {
	idx_t column_count = 0;
	vector<vector<Value>> rows;
};

static ResultSnapshot ReadResultSnapshot(Connection &con, const string &table_name) {
	auto result = con.Query("SELECT * FROM " + table_name);
	if (!result || result->HasError()) {
		throw std::runtime_error(result ? result->GetError() : "null query result");
	}
	ResultSnapshot snapshot;
	snapshot.column_count = result->ColumnCount();
	while (true) {
		auto chunk = result->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		for (idx_t row_idx = 0; row_idx < chunk->size(); row_idx++) {
			vector<Value> row;
			row.reserve(snapshot.column_count);
			for (idx_t col = 0; col < snapshot.column_count; col++) {
				row.push_back(chunk->GetValue(col, row_idx));
			}
			snapshot.rows.push_back(std::move(row));
		}
	}
	return snapshot;
}

static std::map<string, vector<Value>> RowsByKey(const ResultSnapshot &snapshot, idx_t key_cols, const string &label) {
	if (key_cols >= snapshot.column_count) {
		throw std::runtime_error(label + " comparison has no measure columns");
	}
	std::map<string, vector<Value>> by_key;
	for (idx_t row_idx = 0; row_idx < snapshot.rows.size(); row_idx++) {
		auto key = ComparisonKey(snapshot.rows[row_idx], key_cols, row_idx);
		if (!by_key.emplace(key, snapshot.rows[row_idx]).second) {
			throw std::runtime_error(label + " comparison found duplicate output key: " + key);
		}
	}
	return by_key;
}

static UtilityMetrics CompareResultTables(Connection &con, const string &released_table, const string &reference_table,
                                          idx_t key_cols, const string &label) {
	auto released = ReadResultSnapshot(con, released_table);
	auto reference = ReadResultSnapshot(con, reference_table);
	if (released.column_count != reference.column_count) {
		throw std::runtime_error(label + " comparison column-count mismatch");
	}
	if (reference.column_count == 0) {
		throw std::runtime_error(label + " comparison saw zero output columns");
	}
	auto released_by_key = RowsByKey(released, key_cols, label + " released");
	auto reference_by_key = RowsByKey(reference, key_cols, label + " reference");

	idx_t measure_count = reference.column_count - key_cols;
	vector<double> utility_sum(measure_count, 0.0);
	vector<idx_t> utility_count(measure_count, 0);
	vector<double> cell_errors;
	idx_t matched_rows = 0;
	idx_t missing_rows = 0;
	idx_t released_only_rows = 0;

	for (auto &entry : reference_by_key) {
		auto found = released_by_key.find(entry.first);
		if (found == released_by_key.end()) {
			missing_rows++;
			continue;
		}
		auto &released_row = found->second;
		auto &reference_row = entry.second;
		bool any_released_measure_null = false;
		for (idx_t col = key_cols; col < reference.column_count; col++) {
			if (released_row[col].IsNull()) {
				any_released_measure_null = true;
				break;
			}
		}
		if (any_released_measure_null) {
			released_only_rows++;
			continue;
		}
		matched_rows++;
		for (idx_t col = key_cols; col < reference.column_count; col++) {
			double released_value;
			double reference_value;
			if (!TryCastDouble(released_row[col], released_value) ||
			    !TryCastDouble(reference_row[col], reference_value)) {
				continue;
			}
			double abs_ref = std::max(0.00001, std::fabs(reference_value));
			double error_pct = 100.0 * std::fabs(released_value - reference_value) / abs_ref;
			idx_t metric_idx = col - key_cols;
			utility_sum[metric_idx] += error_pct;
			utility_count[metric_idx]++;
			cell_errors.push_back(error_pct);
		}
	}
	for (auto &entry : released_by_key) {
		if (reference_by_key.find(entry.first) == reference_by_key.end()) {
			released_only_rows++;
		}
	}

	double recall = (matched_rows + missing_rows) > 0
	                    ? static_cast<double>(matched_rows) / static_cast<double>(matched_rows + missing_rows)
	                    : 1.0;
	double precision = (matched_rows + released_only_rows) > 0
	                       ? static_cast<double>(matched_rows) / static_cast<double>(matched_rows + released_only_rows)
	                       : 1.0;
	double utility = 0.0;
	idx_t cols_with_data = 0;
	for (idx_t i = 0; i < measure_count; i++) {
		if (utility_count[i] > 0) {
			utility += utility_sum[i] / static_cast<double>(utility_count[i]);
			cols_with_data++;
		}
	}
	if (cols_with_data > 0) {
		utility /= static_cast<double>(cols_with_data);
	}
	double median_error_pct = 0.0;
	if (!cell_errors.empty()) {
		std::sort(cell_errors.begin(), cell_errors.end());
		idx_t mid = cell_errors.size() / 2;
		median_error_pct =
		    (cell_errors.size() % 2) ? cell_errors[mid] : (cell_errors[mid - 1] + cell_errors[mid]) / 2.0;
	}

	UtilityMetrics metrics;
	metrics.utility = FormatNumber(utility);
	metrics.recall = FormatNumber(recall);
	metrics.precision = FormatNumber(precision);
	metrics.median_error_pct = FormatNumber(median_error_pct);
	return metrics;
}

struct Q1AggregateMetrics {
	string sum_median_error_pct;
	string avg_median_error_pct;
	string count_median_error_pct;
};

struct NoiseScaleSummary {
	string median;
	string mean;
	string max;
};

static string MedianErrorString(vector<double> values) {
	if (values.empty()) {
		return "";
	}
	std::sort(values.begin(), values.end());
	idx_t mid = values.size() / 2;
	double median = (values.size() % 2) ? values[mid] : (values[mid - 1] + values[mid]) / 2.0;
	return FormatNumber(median);
}

static NoiseScaleSummary SummarizeNoiseScaleValues(vector<double> values) {
	NoiseScaleSummary summary;
	if (values.empty()) {
		return summary;
	}
	double sum = 0.0;
	double max_value = 0.0;
	for (double value : values) {
		sum += value;
		max_value = std::max(max_value, value);
	}
	std::sort(values.begin(), values.end());
	idx_t mid = values.size() / 2;
	double median = (values.size() % 2) ? values[mid] : (values[mid - 1] + values[mid]) / 2.0;
	summary.median = FormatNumber(median);
	summary.mean = FormatNumber(sum / static_cast<double>(values.size()));
	summary.max = FormatNumber(max_value);
	return summary;
}

static NoiseScaleSummary SummarizeNoiseScaleRecordTable(Connection &con, const string &table_name,
                                                        double value_multiplier = 1.0) {
	auto snapshot = ReadResultSnapshot(con, table_name);
	vector<double> values;
	for (auto &row : snapshot.rows) {
		if (row.size() < 5) {
			continue;
		}
		double value;
		if (TryCastDouble(row[4], value)) {
			values.push_back(std::fabs(value) * value_multiplier);
		}
	}
	return SummarizeNoiseScaleValues(std::move(values));
}

static Q1AggregateMetrics SummarizeQ1NoiseScaleRecordFamilies(Connection &con, const string &table_name,
                                                              double value_multiplier = 1.0) {
	auto snapshot = ReadResultSnapshot(con, table_name);
	Q1AggregateMetrics metrics;
	vector<double> sum_scales;
	vector<double> avg_scales;
	vector<double> count_scales;
	for (auto &row : snapshot.rows) {
		if (row.size() < 5) {
			continue;
		}
		double aggregate_index_value;
		double noise_scale;
		if (!TryCastDouble(row[1], aggregate_index_value) || !TryCastDouble(row[4], noise_scale)) {
			continue;
		}
		idx_t aggregate_index = static_cast<idx_t>(aggregate_index_value);
		if (aggregate_index < 4) {
			sum_scales.push_back(std::fabs(noise_scale) * value_multiplier);
		} else if (aggregate_index < 7) {
			avg_scales.push_back(std::fabs(noise_scale) * value_multiplier);
		} else if (aggregate_index == 7) {
			count_scales.push_back(std::fabs(noise_scale) * value_multiplier);
		}
	}
	metrics.sum_median_error_pct = MedianErrorString(std::move(sum_scales));
	metrics.avg_median_error_pct = MedianErrorString(std::move(avg_scales));
	metrics.count_median_error_pct = MedianErrorString(std::move(count_scales));
	return metrics;
}

static idx_t CountNoiseScaleAggregates(Connection &con, const string &table_name) {
	auto snapshot = ReadResultSnapshot(con, table_name);
	idx_t aggregate_count = 0;
	for (auto &row : snapshot.rows) {
		if (row.size() < 2) {
			continue;
		}
		double aggregate_index_value;
		if (!TryCastDouble(row[1], aggregate_index_value) || aggregate_index_value < 0.0) {
			continue;
		}
		auto aggregate_index = static_cast<idx_t>(aggregate_index_value);
		aggregate_count = std::max(aggregate_count, aggregate_index + 1);
	}
	return aggregate_count;
}

static Q1AggregateMetrics CompareQ1AggregateFamilies(Connection &con, const string &released_table,
                                                     const string &reference_table, idx_t key_cols,
                                                     const string &label) {
	auto released = ReadResultSnapshot(con, released_table);
	auto reference = ReadResultSnapshot(con, reference_table);
	if (released.column_count != reference.column_count) {
		throw std::runtime_error(label + " Q1 comparison column-count mismatch");
	}
	if (reference.column_count < key_cols + 8) {
		throw std::runtime_error(label + " Q1 comparison expected 8 measure columns");
	}
	auto released_by_key = RowsByKey(released, key_cols, label + " Q1 released");
	auto reference_by_key = RowsByKey(reference, key_cols, label + " Q1 reference");

	vector<double> sum_errors;
	vector<double> avg_errors;
	vector<double> count_errors;
	for (auto &entry : reference_by_key) {
		auto found = released_by_key.find(entry.first);
		if (found == released_by_key.end()) {
			continue;
		}
		auto &released_row = found->second;
		auto &reference_row = entry.second;
		for (idx_t col = key_cols; col < key_cols + 8; col++) {
			if (released_row[col].IsNull()) {
				continue;
			}
			double released_value;
			double reference_value;
			if (!TryCastDouble(released_row[col], released_value) ||
			    !TryCastDouble(reference_row[col], reference_value)) {
				continue;
			}
			double abs_ref = std::max(0.00001, std::fabs(reference_value));
			double error_pct = 100.0 * std::fabs(released_value - reference_value) / abs_ref;
			idx_t measure_idx = col - key_cols;
			if (measure_idx < 4) {
				sum_errors.push_back(error_pct);
			} else if (measure_idx < 7) {
				avg_errors.push_back(error_pct);
			} else {
				count_errors.push_back(error_pct);
			}
		}
	}

	Q1AggregateMetrics metrics;
	metrics.sum_median_error_pct = MedianErrorString(std::move(sum_errors));
	metrics.avg_median_error_pct = MedianErrorString(std::move(avg_errors));
	metrics.count_median_error_pct = MedianErrorString(std::move(count_errors));
	return metrics;
}

static string StripTrailingSemicolon(string sql) {
	sql = Trim(sql);
	while (!sql.empty() && sql.back() == ';') {
		sql.pop_back();
		sql = Trim(sql);
	}
	return sql;
}

static void CreateTempResultTable(Connection &con, const string &table_name, const string &sql) {
	RunStatement(con, "DROP TABLE IF EXISTS " + table_name);
	RunStatement(con, "CREATE TEMP TABLE " + table_name + " AS " + StripTrailingSemicolon(sql));
}

struct SaaEstimatorMetricCleanup {
	Connection &con;
	vector<string> table_names;
	uint64_t seed;

	SaaEstimatorMetricCleanup(Connection &con_p, vector<string> table_names_p, uint64_t seed_p)
	    : con(con_p), table_names(std::move(table_names_p)), seed(seed_p) {
	}

	~SaaEstimatorMetricCleanup() {
		for (auto &table_name : table_names) {
			TryRun("DROP TABLE IF EXISTS " + table_name);
		}
		TryRun("SET privacy_diffcols=NULL");
		TryRun("SET priv_rewrite=true");
		TryRun("SET privacy_noise=true");
		TryRun("SET dp_sass_noise_scale_query_mode=false");
		TryRun("SET privacy_seed=" + std::to_string(seed));
	}

private:
	void TryRun(const string &sql) {
		try {
			RunStatement(con, sql);
		} catch (...) {
		}
	}
};

static bool IsBuiltInTpchQ1(const DatasetConfig &dataset, const QuerySpec &query) {
	bool built_in_tpch = (dataset.name == "tpch" && dataset.workload == "tpch_stock_dp") ||
	                     (dataset.name == "jcch" && dataset.workload == "jcch_stock_dp");
	return built_in_tpch && query.name == "q01" && query.key_cols == 2;
}

static void FillQ1FullAnswerMetrics(Connection &con, const DatasetConfig &dataset, const QuerySpec &query,
                                    uint64_t seed, UtilityMetrics &metrics) {
	if (!IsBuiltInTpchQ1(dataset, query)) {
		return;
	}
	string suffix = std::to_string(getpid());
	string private_table = "__q1_private_" + suffix;
	string full_table = "__q1_full_" + suffix;
	SaaEstimatorMetricCleanup cleanup(con, {private_table, full_table}, seed);

	RunStatement(con, "SET privacy_diffcols=NULL");
	RunStatement(con, "SET privacy_seed=" + std::to_string(seed));
	RunStatement(con, "SET priv_rewrite=true");
	RunStatement(con, "SET privacy_noise=true");
	CreateTempResultTable(con, private_table, query.sql);

	RunStatement(con, "SET priv_rewrite=false");
	RunStatement(con, "SET privacy_noise=false");
	CreateTempResultTable(con, full_table, query.sql);

	auto q1_metrics = CompareQ1AggregateFamilies(con, private_table, full_table, query.key_cols, "Q1 full-answer");
	metrics.q1_sum_median_error_pct = q1_metrics.sum_median_error_pct;
	metrics.q1_avg_median_error_pct = q1_metrics.avg_median_error_pct;
	metrics.q1_count_median_error_pct = q1_metrics.count_median_error_pct;
}

static void FillSaaEstimatorMetrics(Connection &con, const DatasetConfig &dataset, const QuerySpec &query,
                                    uint64_t seed, UtilityMetrics &metrics) {
	string suffix = std::to_string(getpid());
	string private_table = "__dp_sass_private_" + suffix;
	string estimator_table = "__dp_sass_estimator_" + suffix;
	string full_table = "__dp_sass_full_" + suffix;
	SaaEstimatorMetricCleanup cleanup(con, {private_table, estimator_table, full_table}, seed);

	RunStatement(con, "SET privacy_diffcols=NULL");
	RunStatement(con, "SET privacy_seed=" + std::to_string(seed));
	RunStatement(con, "SET priv_rewrite=true");
	RunStatement(con, "SET privacy_noise=true");
	CreateTempResultTable(con, private_table, query.sql);

	RunStatement(con, "SET privacy_noise=false");
	CreateTempResultTable(con, estimator_table, query.sql);

	RunStatement(con, "SET priv_rewrite=false");
	RunStatement(con, "SET privacy_noise=false");
	CreateTempResultTable(con, full_table, query.sql);

	auto estimator_metrics = CompareResultTables(con, private_table, estimator_table, query.key_cols, "SAA estimator");
	auto sampling_metrics = CompareResultTables(con, estimator_table, full_table, query.key_cols, "SAA sampling");
	metrics.saa_estimator_utility = estimator_metrics.utility;
	metrics.saa_estimator_recall = estimator_metrics.recall;
	metrics.saa_estimator_precision = estimator_metrics.precision;
	metrics.saa_estimator_median_error_pct = estimator_metrics.median_error_pct;
	metrics.saa_sampling_utility = sampling_metrics.utility;
	metrics.saa_sampling_recall = sampling_metrics.recall;
	metrics.saa_sampling_precision = sampling_metrics.precision;
	metrics.saa_sampling_median_error_pct = sampling_metrics.median_error_pct;

	if (IsBuiltInTpchQ1(dataset, query)) {
		auto estimator_q1 =
		    CompareQ1AggregateFamilies(con, private_table, estimator_table, query.key_cols, "SAA estimator Q1");
		auto sampling_q1 =
		    CompareQ1AggregateFamilies(con, estimator_table, full_table, query.key_cols, "SAA sampling Q1");
		metrics.saa_estimator_q1_sum_median_error_pct = estimator_q1.sum_median_error_pct;
		metrics.saa_estimator_q1_avg_median_error_pct = estimator_q1.avg_median_error_pct;
		metrics.saa_estimator_q1_count_median_error_pct = estimator_q1.count_median_error_pct;
		metrics.saa_sampling_q1_sum_median_error_pct = sampling_q1.sum_median_error_pct;
		metrics.saa_sampling_q1_avg_median_error_pct = sampling_q1.avg_median_error_pct;
		metrics.saa_sampling_q1_count_median_error_pct = sampling_q1.count_median_error_pct;
	}
}

static void FillSaaNoiseScaleMetrics(Connection &con, const DatasetConfig &dataset, const QuerySpec &query,
                                     const RunPoint &point, uint64_t seed, UtilityMetrics &metrics) {
	string suffix = std::to_string(getpid());
	string scale_table = "__dp_sass_noise_scale_" + suffix;
	SaaEstimatorMetricCleanup cleanup(con, {scale_table}, seed);

	RunStatement(con, "SET privacy_diffcols=NULL");
	RunStatement(con, "SET privacy_seed=" + std::to_string(seed));
	CreateTempResultTable(con, scale_table, "SELECT * FROM dp_sass_noise_scale_query(" + SqlQuote(query.sql) + ")");

	auto summary = SummarizeNoiseScaleRecordTable(con, scale_table);
	metrics.saa_noise_scale_median = summary.median;
	metrics.saa_noise_scale_mean = summary.mean;
	metrics.saa_noise_scale_max = summary.max;

	if (IsBuiltInTpchQ1(dataset, query)) {
		auto q1_metrics = SummarizeQ1NoiseScaleRecordFamilies(con, scale_table);
		metrics.saa_noise_scale_q1_sum_median = q1_metrics.sum_median_error_pct;
		metrics.saa_noise_scale_q1_avg_median = q1_metrics.avg_median_error_pct;
		metrics.saa_noise_scale_q1_count_median = q1_metrics.count_median_error_pct;
	}

	if (point.release == "median") {
		idx_t aggregate_count = CountNoiseScaleAggregates(con, scale_table);
		if (aggregate_count > 0) {
			bool grouped = query.key_cols > 0;
			double group_bound = grouped ? point.group_bound : 1.0;
			double budget_divisor =
			    static_cast<double>(aggregate_count + (grouped ? 1 : 0)) * std::max(1.0, group_bound);
			double epsilon_cell = point.epsilon / budget_divisor;
			double smooth_multiplier = epsilon_cell / 2.0;
			auto smooth_summary = SummarizeNoiseScaleRecordTable(con, scale_table, smooth_multiplier);
			metrics.saa_smooth_sensitivity_median = smooth_summary.median;
			metrics.saa_smooth_sensitivity_mean = smooth_summary.mean;
			metrics.saa_smooth_sensitivity_max = smooth_summary.max;
			if (IsBuiltInTpchQ1(dataset, query)) {
				auto q1_smooth = SummarizeQ1NoiseScaleRecordFamilies(con, scale_table, smooth_multiplier);
				metrics.saa_smooth_sensitivity_q1_sum_median = q1_smooth.sum_median_error_pct;
				metrics.saa_smooth_sensitivity_q1_avg_median = q1_smooth.avg_median_error_pct;
				metrics.saa_smooth_sensitivity_q1_count_median = q1_smooth.count_median_error_pct;
			}
		}
	}
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
		if (mode == "duckdb") {
			RunPoint point;
			point.mode = mode;
			points.push_back(std::move(point));
			continue;
		}
		auto releases = mode == "dp_sass"
		                    ? dataset.sass_releases
		                    : (mode == "dp_sass_bounded_ratio" ? vector<string> {"average"} : vector<string> {"-"});
		auto sum_bound_lists =
		    mode == "dp_sass_bounded_ratio" ? vector<string> {""} : NonEmptyString(dataset.sum_bound_lists, {""});
		auto support_thresholds = mode == "duckdb" ? vector<double> {0.0} : NonEmpty(dataset.support_thresholds, {0.0});
		auto sass_count_output_bounds =
		    mode == "dp_sass" ? NonEmpty(dataset.sass_count_output_bounds, {0.0}) : vector<double> {0.0};
		auto sass_count_output_bound_lists =
		    mode == "dp_sass" ? NonEmptyString(dataset.sass_count_output_bound_lists, {""}) : vector<string> {""};
		auto sass_count_output_lower_bound_lists =
		    mode == "dp_sass" ? NonEmptyString(dataset.sass_count_output_lower_bound_lists, {""}) : vector<string> {""};
		auto sass_count_output_upper_bound_lists =
		    mode == "dp_sass" ? NonEmptyString(dataset.sass_count_output_upper_bound_lists, {""}) : vector<string> {""};
		auto sass_sum_output_bounds =
		    mode == "dp_sass" ? NonEmpty(dataset.sass_sum_output_bounds, {0.0}) : vector<double> {0.0};
		auto sass_sum_output_bound_lists =
		    mode == "dp_sass" ? NonEmptyString(dataset.sass_sum_output_bound_lists, {""}) : vector<string> {""};
		auto sass_sum_output_lower_bound_lists =
		    mode == "dp_sass" ? NonEmptyString(dataset.sass_sum_output_lower_bound_lists, {""}) : vector<string> {""};
		auto sass_sum_output_upper_bound_lists =
		    mode == "dp_sass" ? NonEmptyString(dataset.sass_sum_output_upper_bound_lists, {""}) : vector<string> {""};
		auto lane_values = (mode == "dp_sass" || mode == "dp_sass_bounded_ratio")
		                       ? NonEmptyInt(dataset.sample_lanes, {1})
		                       : vector<int> {1};
		auto sass_m_values = mode == "dp_sass" ? NonEmptyInt(dataset.sass_m_values, {64}) : vector<int> {64};
		auto sass_rescale_values =
		    mode == "dp_sass" ? NonEmptyBool(dataset.sass_rescale_values, {true}) : vector<bool> {true};
		for (double epsilon : dataset.epsilons) {
			auto deltas = dataset.deltas.empty() ? vector<double> {0.0} : dataset.deltas;
			for (double delta : deltas) {
				for (double count_bound : dataset.count_bounds) {
					for (double sum_bound : dataset.sum_bounds) {
						for (auto &sum_bound_list : sum_bound_lists) {
							for (double c_u : c_u_values) {
								for (double support_threshold : support_thresholds) {
									for (double multiplier : dataset.bound_multipliers) {
										for (double sass_count_output_bound : sass_count_output_bounds) {
											for (auto &sass_count_output_bound_list : sass_count_output_bound_lists) {
												for (auto &sass_count_output_lower_bound_list :
												     sass_count_output_lower_bound_lists) {
													for (auto &sass_count_output_upper_bound_list :
													     sass_count_output_upper_bound_lists) {
														for (double sass_sum_output_bound : sass_sum_output_bounds) {
															for (auto &sass_sum_output_bound_list :
															     sass_sum_output_bound_lists) {
																for (auto &sass_sum_output_lower_bound_list :
																     sass_sum_output_lower_bound_lists) {
																	for (auto &sass_sum_output_upper_bound_list :
																	     sass_sum_output_upper_bound_lists) {
																		for (int lanes : lane_values) {
																			for (int sass_m : sass_m_values) {
																				for (bool sass_rescale :
																				     sass_rescale_values) {
																					for (auto &release : releases) {
																						RunPoint point;
																						point.mode = mode;
																						point.release = release;
																						point.epsilon = epsilon;
																						point.delta = delta;
																						point.count_bound =
																						    count_bound * multiplier;
																						point.sum_bound =
																						    sum_bound * multiplier;
																						point.sum_bound_list =
																						    sum_bound_list;
																						point.avg_lower_bounds =
																						    dataset.avg_lower_bounds;
																						point.avg_upper_bounds =
																						    dataset.avg_upper_bounds;
																						point.sass_count_output_bound =
																						    sass_count_output_bound;
																						point
																						    .sass_count_output_bound_list =
																						    sass_count_output_bound_list;
																						point
																						    .sass_count_output_lower_bound_list =
																						    sass_count_output_lower_bound_list;
																						point
																						    .sass_count_output_upper_bound_list =
																						    sass_count_output_upper_bound_list;
																						point.sass_sum_output_bound =
																						    sass_sum_output_bound;
																						point
																						    .sass_sum_output_bound_list =
																						    sass_sum_output_bound_list;
																						point
																						    .sass_sum_output_lower_bound_list =
																						    sass_sum_output_lower_bound_list;
																						point
																						    .sass_sum_output_upper_bound_list =
																						    sass_sum_output_upper_bound_list;
																						point.group_bound = c_u;
																						point.support_threshold =
																						    support_threshold;
																						point.c_u = c_u;
																						point.bound_multiplier =
																						    multiplier;
																						point.sample_lanes = lanes;
																						point.sass_m = sass_m;
																						point.sass_rescale =
																						    sass_rescale;
																						points.push_back(
																						    std::move(point));
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

static void ApplyAvgBoundSettings(Connection &con, const RunPoint &point) {
	RunStatement(con, "SET dp_avg_lower_bounds=NULL");
	RunStatement(con, "SET dp_avg_upper_bounds=NULL");
	if (!point.avg_lower_bounds.empty() || !point.avg_upper_bounds.empty()) {
		if (point.avg_lower_bounds.empty() || point.avg_upper_bounds.empty() ||
		    point.avg_lower_bounds.size() != point.avg_upper_bounds.size()) {
			throw std::runtime_error("avg_lower_bounds and avg_upper_bounds must have the same non-zero length");
		}
		RunStatement(con, "SET dp_avg_lower_bound=NULL");
		RunStatement(con, "SET dp_avg_upper_bound=NULL");
		RunStatement(con, "SET dp_avg_lower_bounds=" + SqlQuote(FormatNumberList(point.avg_lower_bounds)));
		RunStatement(con, "SET dp_avg_upper_bounds=" + SqlQuote(FormatNumberList(point.avg_upper_bounds)));
		return;
	}
	RunStatement(con, "SET dp_avg_lower_bound=0");
	RunStatement(con, "SET dp_avg_upper_bound=" + FormatNumber(point.sum_bound));
}

static void ApplyDpSettings(Connection &con, const RunPoint &point, double delta, double sass_count_output_bound,
                            double sass_sum_output_bound) {
	if (point.mode == "duckdb") {
		RunStatement(con, "SET priv_rewrite=false");
		RunStatement(con, "SET privacy_diffcols=NULL");
		RunStatement(con, "SET privacy_min_group_count=NULL");
		return;
	}
	if (point.mode == "dp_sass_bounded_ratio") {
		RunStatement(con, "SET priv_rewrite=false");
		RunStatement(con, "SET privacy_diffcols=NULL");
		RunStatement(con, "SET dp_sample_lanes=" + std::to_string(point.sample_lanes));
		return;
	}
	RunStatement(con, "SET priv_rewrite=true");
	RunStatement(con, "SET privacy_mode='" + point.mode + "'");
	RunStatement(con, "SET dp_epsilon=" + FormatNumber(point.epsilon));
	if (delta <= 0.0 && (point.mode == "dp_standard" || (point.mode == "dp_sass" && point.release == "average"))) {
		RunStatement(con, "SET dp_delta=NULL");
	} else {
		RunStatement(con, "SET dp_delta=" + FormatNumber(delta));
	}
	RunStatement(con, "SET dp_sum_bound=" + FormatNumber(point.sum_bound));
	RunStatement(con, "SET dp_sum_bounds=NULL");
	if (!point.sum_bound_list.empty()) {
		RunStatement(con, "SET dp_sum_bounds=" + SqlQuote(ScaleConfiguredBoundList(
		                                             point.sum_bound_list, point.bound_multiplier, "sum_bound_lists")));
	}
	RunStatement(con, "SET dp_count_bound=" + FormatNumber(point.count_bound));
	RunStatement(con, "SET dp_max_groups_contributed=" + FormatNumber(point.group_bound));
	if (point.support_threshold > 0.0) {
		RunStatement(con, "SET privacy_min_group_count=" + FormatNumber(point.support_threshold));
	} else {
		RunStatement(con, "SET privacy_min_group_count=NULL");
	}
	ApplyAvgBoundSettings(con, point);
	if (point.mode == "dp_sass") {
		RunStatement(con, "SET dp_sample_lanes=" + std::to_string(point.sample_lanes));
		RunStatement(con, "SET dp_sass_m=" + std::to_string(point.sass_m));
		RunStatement(con, string("SET dp_sass_rescale=") + (point.sass_rescale ? "true" : "false"));
		RunStatement(con, "SET dp_sass_release='" + point.release + "'");
		RunStatement(con, "SET dp_sass_count_output_bound=" + FormatNumber(sass_count_output_bound));
		RunStatement(con, "SET dp_sass_sum_output_bound=" + FormatNumber(sass_sum_output_bound));
		RunStatement(con, "SET dp_sass_count_output_bounds=NULL");
		RunStatement(con, "SET dp_sass_count_output_lower_bounds=NULL");
		RunStatement(con, "SET dp_sass_count_output_upper_bounds=NULL");
		if (!point.sass_count_output_bound_list.empty()) {
			RunStatement(
			    con, "SET dp_sass_count_output_bounds=" +
			             SqlQuote(ScaleConfiguredBoundList(point.sass_count_output_bound_list, point.bound_multiplier,
			                                               "sass_count_output_bound_lists")));
		}
		if (!point.sass_count_output_lower_bound_list.empty() || !point.sass_count_output_upper_bound_list.empty()) {
			if (point.sass_count_output_lower_bound_list.empty() || point.sass_count_output_upper_bound_list.empty()) {
				throw std::runtime_error("sass_count_output_lower_bound_lists and "
				                         "sass_count_output_upper_bound_lists must be set together");
			}
			RunStatement(con, "SET dp_sass_count_output_lower_bounds=" +
			                      SqlQuote(ScaleConfiguredFiniteList(point.sass_count_output_lower_bound_list,
			                                                         point.bound_multiplier,
			                                                         "sass_count_output_lower_bound_lists")));
			RunStatement(con, "SET dp_sass_count_output_upper_bounds=" +
			                      SqlQuote(ScaleConfiguredBoundList(point.sass_count_output_upper_bound_list,
			                                                        point.bound_multiplier,
			                                                        "sass_count_output_upper_bound_lists")));
		}
		RunStatement(con, "SET dp_sass_sum_output_bounds=NULL");
		RunStatement(con, "SET dp_sass_sum_output_lower_bounds=NULL");
		RunStatement(con, "SET dp_sass_sum_output_upper_bounds=NULL");
		if (!point.sass_sum_output_bound_list.empty()) {
			RunStatement(con,
			             "SET dp_sass_sum_output_bounds=" +
			                 SqlQuote(ScaleConfiguredBoundList(point.sass_sum_output_bound_list, point.bound_multiplier,
			                                                   "sass_sum_output_bound_lists")));
		}
		if (!point.sass_sum_output_lower_bound_list.empty() || !point.sass_sum_output_upper_bound_list.empty()) {
			if (point.sass_sum_output_lower_bound_list.empty() || point.sass_sum_output_upper_bound_list.empty()) {
				throw std::runtime_error("sass_sum_output_lower_bound_lists and sass_sum_output_upper_bound_lists "
				                         "must be set together");
			}
			RunStatement(con, "SET dp_sass_sum_output_lower_bounds=" +
			                      SqlQuote(ScaleConfiguredFiniteList(point.sass_sum_output_lower_bound_list,
			                                                         point.bound_multiplier,
			                                                         "sass_sum_output_lower_bound_lists")));
			RunStatement(con, "SET dp_sass_sum_output_upper_bounds=" +
			                      SqlQuote(ScaleConfiguredFiniteList(point.sass_sum_output_upper_bound_list,
			                                                         point.bound_multiplier,
			                                                         "sass_sum_output_upper_bound_lists")));
		}
		RunStatement(con, "SET dp_sass_avg_lower_bound=" +
		                      FormatNumber(point.avg_lower_bounds.empty() ? 0.0 : point.avg_lower_bounds[0]));
		RunStatement(con,
		             "SET dp_sass_avg_upper_bound=" +
		                 FormatNumber(point.avg_upper_bounds.empty() ? point.sum_bound : point.avg_upper_bounds[0]));
		RunStatement(con, "SET dp_sass_minmax_lower_bound=0");
		RunStatement(con, "SET dp_sass_minmax_upper_bound=" + FormatNumber(point.sum_bound));
	}
}

static void WriteHeader(std::ofstream &csv) {
	csv << "dataset,workload,query,query_id,mode,release,profile,run,success,error,primary_metric,time_ms,"
	       "utility,recall,precision,median_error_pct,q1_sum_median_error_pct,q1_avg_median_error_pct,"
	       "q1_count_median_error_pct,saa_estimator_utility,saa_estimator_recall,saa_estimator_precision,"
	       "saa_estimator_median_error_pct,saa_estimator_q1_sum_median_error_pct,"
	       "saa_estimator_q1_avg_median_error_pct,saa_estimator_q1_count_median_error_pct,saa_sampling_utility,"
	       "saa_sampling_recall,saa_sampling_precision,saa_sampling_median_error_pct,"
	       "saa_sampling_q1_sum_median_error_pct,saa_sampling_q1_avg_median_error_pct,"
	       "saa_sampling_q1_count_median_error_pct,saa_noise_scale_median,saa_noise_scale_mean,"
	       "saa_noise_scale_max,saa_noise_scale_q1_sum_median,saa_noise_scale_q1_avg_median,"
	       "saa_noise_scale_q1_count_median,saa_smooth_sensitivity_median,saa_smooth_sensitivity_mean,"
	       "saa_smooth_sensitivity_max,saa_smooth_sensitivity_q1_sum_median,"
	       "saa_smooth_sensitivity_q1_avg_median,saa_smooth_sensitivity_q1_count_median,epsilon,delta,sf,"
	       "dp_sum_bound,dp_sum_bounds,dp_count_bound,dp_max_groups_contributed,privacy_min_group_count,"
	       "c_u,dp_sass_count_output_bound,dp_sass_sum_output_bound,dp_avg_lower_bounds,dp_avg_upper_bounds,"
	       "dp_sass_count_output_bounds,dp_sass_sum_output_bounds,dp_sass_count_output_lower_bounds,"
	       "dp_sass_count_output_upper_bounds,dp_sass_sum_output_lower_bounds,dp_sass_sum_output_upper_bounds,"
	       "bound_multiplier,dp_sample_lanes,dp_sass_m,dp_sass_rescale,seed,db_path,query_path\n";
}

static void WriteRow(std::ofstream &csv, const DatasetConfig &dataset, const QuerySpec &query, idx_t query_id,
                     const RunPoint &point, idx_t run, bool success, const string &error, const UtilityMetrics &metrics,
                     double time_ms, double delta, double sass_count_output_bound, double sass_sum_output_bound,
                     uint64_t seed, const string &db_path) {
	csv << dataset.name << "," << dataset.workload << "," << query.name << "," << query_id << "," << point.mode << ","
	    << point.release << "," << (query.profile == PrivacyProfile::PART ? "part" : "customer") << "," << run << ","
	    << (success ? "true" : "false") << "," << CsvQuote(error) << ",utility," << FormatNumber(time_ms) << ","
	    << metrics.utility << "," << metrics.recall << "," << metrics.precision << "," << metrics.median_error_pct
	    << "," << metrics.q1_sum_median_error_pct << "," << metrics.q1_avg_median_error_pct << ","
	    << metrics.q1_count_median_error_pct << "," << metrics.saa_estimator_utility << ","
	    << metrics.saa_estimator_recall << "," << metrics.saa_estimator_precision << ","
	    << metrics.saa_estimator_median_error_pct << "," << metrics.saa_estimator_q1_sum_median_error_pct << ","
	    << metrics.saa_estimator_q1_avg_median_error_pct << "," << metrics.saa_estimator_q1_count_median_error_pct
	    << "," << metrics.saa_sampling_utility << "," << metrics.saa_sampling_recall << ","
	    << metrics.saa_sampling_precision << "," << metrics.saa_sampling_median_error_pct << ","
	    << metrics.saa_sampling_q1_sum_median_error_pct << "," << metrics.saa_sampling_q1_avg_median_error_pct << ","
	    << metrics.saa_sampling_q1_count_median_error_pct << "," << metrics.saa_noise_scale_median << ","
	    << metrics.saa_noise_scale_mean << "," << metrics.saa_noise_scale_max << ","
	    << metrics.saa_noise_scale_q1_sum_median << "," << metrics.saa_noise_scale_q1_avg_median << ","
	    << metrics.saa_noise_scale_q1_count_median << "," << metrics.saa_smooth_sensitivity_median << ","
	    << metrics.saa_smooth_sensitivity_mean << "," << metrics.saa_smooth_sensitivity_max << ","
	    << metrics.saa_smooth_sensitivity_q1_sum_median << "," << metrics.saa_smooth_sensitivity_q1_avg_median << ","
	    << metrics.saa_smooth_sensitivity_q1_count_median << "," << FormatNumber(point.epsilon) << ","
	    << FormatNumber(delta) << "," << FormatNumber(dataset.scale_factor) << "," << FormatNumber(point.sum_bound)
	    << "," << CsvQuote(ScaleConfiguredBoundList(point.sum_bound_list, point.bound_multiplier, "sum_bound_lists"))
	    << "," << FormatNumber(point.count_bound) << "," << FormatNumber(point.group_bound) << ","
	    << FormatNumber(point.support_threshold) << "," << FormatNumber(point.c_u) << ","
	    << FormatNumber(sass_count_output_bound) << "," << FormatNumber(sass_sum_output_bound) << ","
	    << CsvQuote(FormatNumberList(point.avg_lower_bounds)) << ","
	    << CsvQuote(FormatNumberList(point.avg_upper_bounds)) << ","
	    << CsvQuote(ScaleConfiguredBoundList(point.sass_count_output_bound_list, point.bound_multiplier,
	                                         "sass_count_output_bound_lists"))
	    << ","
	    << CsvQuote(ScaleConfiguredBoundList(point.sass_sum_output_bound_list, point.bound_multiplier,
	                                         "sass_sum_output_bound_lists"))
	    << ","
	    << CsvQuote(ScaleConfiguredFiniteList(point.sass_count_output_lower_bound_list, point.bound_multiplier,
	                                          "sass_count_output_lower_bound_lists"))
	    << ","
	    << CsvQuote(ScaleConfiguredBoundList(point.sass_count_output_upper_bound_list, point.bound_multiplier,
	                                         "sass_count_output_upper_bound_lists"))
	    << ","
	    << CsvQuote(ScaleConfiguredFiniteList(point.sass_sum_output_lower_bound_list, point.bound_multiplier,
	                                          "sass_sum_output_lower_bound_lists"))
	    << ","
	    << CsvQuote(ScaleConfiguredFiniteList(point.sass_sum_output_upper_bound_list, point.bound_multiplier,
	                                          "sass_sum_output_upper_bound_lists"))
	    << "," << FormatNumber(point.bound_multiplier) << "," << point.sample_lanes << "," << point.sass_m << ","
	    << (point.sass_rescale ? "true" : "false") << "," << seed << "," << CsvQuote(db_path) << ","
	    << CsvQuote(query.path) << "\n";
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
					if (point.mode == "dp_sass_bounded_ratio") {
						if (query.name != "q14") {
							throw std::runtime_error("dp_sass_bounded_ratio is only implemented for built-in q14");
						}
					} else if (point.mode == "dp_sass") {
						ApplyDatasetPrivacy(con, dataset, query);
						double pu_count = ReadBenchmarkPrivacyUnitCount(con, dataset, query);
						if (delta <= 0.0) {
							delta = DefaultDelta(point.epsilon, pu_count);
						}
						sass_count_output_bound =
						    point.sass_count_output_bound > 0.0
						        ? ScaleConfiguredSassOutputBound(point.sass_count_output_bound, point.bound_multiplier)
						        : DeriveSassOutputBound(point.count_bound, pu_count, point.sample_lanes, point.sass_m,
						                                point.sass_rescale, "dp_sass_count_output_bound");
						sass_sum_output_bound =
						    point.sass_sum_output_bound > 0.0
						        ? ScaleConfiguredSassOutputBound(point.sass_sum_output_bound, point.bound_multiplier)
						        : DeriveSassOutputBound(point.sum_bound, pu_count, point.sample_lanes, point.sass_m,
						                                point.sass_rescale, "dp_sass_sum_output_bound");
					} else if (point.mode != "duckdb") {
						ApplyDatasetPrivacy(con, dataset, query);
						double pu_count = ReadBenchmarkPrivacyUnitCount(con, dataset, query);
						if (delta <= 0.0) {
							delta = DefaultDelta(point.epsilon, pu_count);
						}
					}
					ApplyDpSettings(con, point, delta, sass_count_output_bound, sass_sum_output_bound);
					RunStatement(con, "SET privacy_seed=" + std::to_string(seed));
					if (point.mode != "duckdb" && point.mode != "dp_sass_bounded_ratio") {
						RunStatement(con, "SET privacy_diffcols=" +
						                      SqlQuote(std::to_string(query.key_cols) + ":" + utility_path));
					}
					auto start = std::chrono::steady_clock::now();
					double released_scalar = 0.0;
					double reference_scalar = 0.0;
					if (point.mode == "dp_sass_bounded_ratio") {
						reference_scalar = ReadScalarDouble(con, query.sql);
						released_scalar =
						    ReadScalarDouble(con, BuildQ14BoundedSampledRatioSql(point.epsilon, point.sample_lanes));
					} else {
						MaterializeQuery(con, query.sql);
					}
					auto end = std::chrono::steady_clock::now();
					time_ms = std::chrono::duration<double, std::milli>(end - start).count();
					if (point.mode == "duckdb") {
						metrics.utility = "0";
						metrics.recall = "1";
						metrics.precision = "1";
						metrics.median_error_pct = "0";
						if (IsBuiltInTpchQ1(dataset, query)) {
							metrics.q1_sum_median_error_pct = "0";
							metrics.q1_avg_median_error_pct = "0";
							metrics.q1_count_median_error_pct = "0";
						}
					} else if (point.mode == "dp_sass_bounded_ratio") {
						FillScalarUtility(released_scalar, reference_scalar, metrics);
					} else if (!ReadLastUtilityLine(utility_path, metrics)) {
						throw std::runtime_error("privacy_diffcols did not write utility output");
					}
					if (point.mode != "duckdb" && point.mode != "dp_sass_bounded_ratio") {
						FillQ1FullAnswerMetrics(con, dataset, query, seed, metrics);
					}
					if (point.mode == "dp_sass") {
						FillSaaEstimatorMetrics(con, dataset, query, seed, metrics);
						FillSaaNoiseScaleMetrics(con, dataset, query, point, seed, metrics);
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

static idx_t ParseRunCount(const string &value) {
	size_t parsed = 0;
	auto runs = std::stoull(value, &parsed);
	if (parsed != value.size() || runs == 0) {
		throw std::runtime_error("--runs must be a positive integer");
	}
	return static_cast<idx_t>(runs);
}

static void PrintUsage() {
	std::cout << "Usage: dp_benchmark_runner --config PATH [--dry-run] [--out PATH] [--runs N]\n"
	          << "JSON supports scalar or array sweeps for epsilon(s), delta(s), count_bound(s), sum_bound(s),\n"
	          << "sum_bound_lists, group_bound(s), c_u, bound_multiplier(s), sample_lanes, sass_ms, modes,\n"
	          << "sass_releases, sass_rescale(s), sass_count_output_bound_lists, sass_sum_output_bound_lists,\n"
	          << "sass_count/sum_output_lower_bound_lists, and sass_count/sum_output_upper_bound_lists.\n";
}

static int Main(int argc, char **argv) {
	string config_path;
	string out_override;
	idx_t runs_override = 0;
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
		if (arg == "--runs" && i + 1 < argc) {
			runs_override = ParseRunCount(argv[++i]);
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
			if (key == "--runs") {
				runs_override = ParseRunCount(value);
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
	if (runs_override > 0) {
		config.runs = runs_override;
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

#ifndef DP_BENCHMARK_RUNNER_NO_MAIN
int main(int argc, char **argv) {
	try {
		return duckdb::Main(argc, argv);
	} catch (std::exception &ex) {
		std::cerr << "error: " << ex.what() << std::endl;
		return 1;
	}
}
#endif
