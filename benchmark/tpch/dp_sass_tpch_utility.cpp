// TPC-H utility benchmark for privacy_mode='dp_sass', 'dp_elastic', or 'dp_standard'.
//
// The runner executes a small FLEX-style count workload, compares each noised
// result against the exact answer through privacy_diffcols, and writes one CSV
// row per query/run. Unsupported query shapes are recorded as failures.

#include "duckdb.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/main/connection.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace duckdb {

enum class PrivacyProfile : uint8_t { CUSTOMER, CUSTOMER_ROW, PART, LINEITEM, ORDERS, PARTSUPP };
enum class BenchmarkProfile : uint8_t { ENTITY, FLEX_ROW };

struct QuerySpec {
	string name;
	string description;
	idx_t key_cols;
	PrivacyProfile privacy_profile;
	string sql;
};

struct Config {
	double scale_factor = 1.0;
	string dataset = "tpch"; // 'tpch' (standard) or 'jcch' (skewed: heavy-tailed per-customer volume)
	string db_path;
	string out_path;
	string mode = "dp_sass";
	vector<string> modes = {"dp_sass"};
	BenchmarkProfile profile = BenchmarkProfile::ENTITY;
	idx_t runs = 30;
	double epsilon = 0.1;
	double delta = 0.0;
	int sample_lanes = 1;
	int threads = 1;
	vector<double> bound_multipliers = {1.0};
	double public_count_bound = 1000.0;
	double public_group_bound = 10.0;
	vector<string> sass_releases = {"median"}; // dp_sass release methods to sweep: median and/or average (GUPT)
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

static string FormatNumber(double v) {
	std::ostringstream oss;
	oss << std::setprecision(15) << std::defaultfloat << v;
	string s = oss.str();
	// Only trim trailing zeros from a plain decimal fraction. In scientific notation a
	// trailing '0' belongs to the exponent (e.g. 7.2e-10), so stripping it would corrupt
	// the value (7.2e-10 -> 7.2e-1).
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

static string Trim(const string &s) {
	size_t start = s.find_first_not_of(" \t\r\n");
	if (start == string::npos) {
		return "";
	}
	size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(start, end - start + 1);
}

static bool ReadLastUtilityLine(const string &path, string &utility, string &recall, string &precision,
                                string &median_error_pct) {
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
	if (!(std::getline(ss, utility, ',') && std::getline(ss, recall, ',') && std::getline(ss, precision, ','))) {
		return false;
	}
	if (!std::getline(ss, median_error_pct, ',')) {
		median_error_pct = "";
	}
	return true;
}

static void RunStatement(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	if (!result || result->HasError()) {
		throw std::runtime_error((result ? result->GetError() : "null result") + " while running: " + sql);
	}
}

static unique_ptr<MaterializedQueryResult> RunQuery(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	if (!result || result->HasError()) {
		throw std::runtime_error(result ? result->GetError() : "null query result");
	}
	while (result->Fetch()) {
	}
	return result;
}

static bool TableHasRows(Connection &con, const string &table_name) {
	auto result = con.Query("SELECT COUNT(*) FROM " + table_name);
	if (!result || result->HasError()) {
		return false;
	}
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() == 0 || chunk->GetValue(0, 0).IsNull()) {
		return false;
	}
	return chunk->GetValue(0, 0).GetValue<int64_t>() > 0;
}

static bool ColumnExists(Connection &con, const string &table_name, const string &column_name) {
	auto result =
	    con.Query("SELECT COUNT(*) FROM information_schema.columns WHERE table_name = " + SqlQuote(table_name) +
	              " AND column_name = " + SqlQuote(column_name));
	if (!result || result->HasError()) {
		return false;
	}
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() == 0 || chunk->GetValue(0, 0).IsNull()) {
		return false;
	}
	return chunk->GetValue(0, 0).GetValue<int64_t>() > 0;
}

static void LoadTpch(Connection &con) {
	auto install = con.Query("INSTALL tpch");
	(void)install;
	RunStatement(con, "LOAD tpch");
}

// JCC-H-style skew (in-DuckDB approximation of the JCC-H benchmark, Boncz et al.): concentrate a
// fraction of orders onto a small set of "whale" customers, giving a heavy-tailed per-customer
// (and, via the orders->lineitem chain, per-customer lineitem) volume. This stresses the per-PU
// contribution bound that worst-case global-sensitivity DP must cover. Deterministic (no RNG),
// schema-preserving (only o_custkey is reassigned to existing whale custkeys).
static void GenerateJcchSkew(Connection &con, double scale_factor) {
	// Whales = first ~0.1% of customers (at least 10). ~30% of orders are reassigned to them.
	int64_t n_whales = std::max<int64_t>(10, static_cast<int64_t>(150.0 * scale_factor));
	Log("Applying JCC-H skew: concentrating ~30% of orders onto " + std::to_string(n_whales) +
	    " whale customers (c_custkey 1.." + std::to_string(n_whales) + ")");
	RunStatement(con, "UPDATE orders SET o_custkey = 1 + (o_orderkey % " + std::to_string(n_whales) +
	                      ") WHERE o_orderkey % 10 < 3");
	RunStatement(con, "CHECKPOINT");
}

static void EnsureTpchData(Connection &con, double scale_factor, const string &dataset) {
	LoadTpch(con);
	if (TableHasRows(con, "customer")) {
		Log(dataset + " tables already exist, skipping dbgen.");
		return;
	}
	Log("Generating TPC-H data at SF " + FormatNumber(scale_factor));
	RunStatement(con, "CALL dbgen(sf=" + FormatNumber(scale_factor) + ")");
	if (dataset == "jcch") {
		GenerateJcchSkew(con, scale_factor);
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

static double InferCountBound(Connection &con, const QuerySpec &query) {
	auto result = RunQuery(con, query.sql);
	double max_value = 0.0;
	for (idx_t row = 0; row < result->RowCount(); row++) {
		for (idx_t col = query.key_cols; col < result->ColumnCount(); col++) {
			auto value = result->GetValue(col, row);
			if (value.IsNull()) {
				continue;
			}
			double d = value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
			if (std::isfinite(d)) {
				max_value = std::max(max_value, std::fabs(d));
			}
		}
	}
	return std::max(1.0, std::ceil(max_value));
}

static double PublicCountOutputBound(const QuerySpec &query, double scale_factor) {
	if (query.name == "q1" || query.name == "q21") {
		return std::max(1.0, std::ceil(6000000.0 * scale_factor));
	}
	if (query.name == "q4") {
		return std::max(1.0, std::ceil(1500000.0 * scale_factor));
	}
	if (query.name == "q13") {
		return std::max(1.0, std::ceil(150000.0 * scale_factor));
	}
	if (query.name == "q16") {
		return std::max(1.0, std::ceil(10000.0 * scale_factor));
	}
	return std::max(1.0, std::ceil(6000000.0 * scale_factor));
}

static vector<double> ParseNumberList(const string &value) {
	vector<double> result;
	std::istringstream ss(value);
	string part;
	while (std::getline(ss, part, ',')) {
		part = Trim(part);
		if (part.empty()) {
			continue;
		}
		double v = std::stod(part);
		if (v <= 0.0 || !std::isfinite(v)) {
			throw std::runtime_error("bound multipliers must be positive finite numbers");
		}
		result.push_back(v);
	}
	if (result.empty()) {
		throw std::runtime_error("empty bound multiplier list");
	}
	return result;
}

static bool IsValidMode(const string &mode) {
	return mode == "dp_sass" || mode == "dp_elastic" || mode == "dp_standard";
}

static vector<string> ParseModeList(const string &value) {
	if (value == "all") {
		return {"dp_sass", "dp_elastic", "dp_standard"};
	}
	vector<string> result;
	std::istringstream ss(value);
	string part;
	while (std::getline(ss, part, ',')) {
		part = Trim(part);
		if (part.empty()) {
			continue;
		}
		if (!IsValidMode(part)) {
			throw std::runtime_error("Unknown mode: " + part + " (expected dp_sass, dp_elastic, or dp_standard)");
		}
		result.push_back(part);
	}
	if (result.empty()) {
		throw std::runtime_error("empty mode list");
	}
	return result;
}

static bool ContainsMode(const vector<string> &modes, const string &mode) {
	return std::find(modes.begin(), modes.end(), mode) != modes.end();
}

static double InferPerPuCountBound(Connection &con, const QuerySpec &query, PrivacyProfile profile) {
	switch (profile) {
	case PrivacyProfile::CUSTOMER_ROW:
	case PrivacyProfile::LINEITEM:
	case PrivacyProfile::ORDERS:
	case PrivacyProfile::PARTSUPP:
		return 1.0;
	default:
		break;
	}

	if (query.name == "q1") {
		return std::max(1.0, std::ceil(ReadScalarDouble(con, R"(
SELECT COALESCE(MAX(cnt), 1)
FROM (
  SELECT o_custkey, l_returnflag, l_linestatus, COUNT(*) AS cnt
  FROM lineitem, orders
  WHERE l_orderkey = o_orderkey
    AND l_shipdate <= DATE '1998-12-01' - INTERVAL 90 DAY
  GROUP BY o_custkey, l_returnflag, l_linestatus
) t)")));
	}
	if (query.name == "q4") {
		return std::max(1.0, std::ceil(ReadScalarDouble(con, R"(
SELECT COALESCE(MAX(cnt), 1)
FROM (
  SELECT o_custkey, o_orderpriority, COUNT(*) AS cnt
  FROM orders
  WHERE o_orderdate >= DATE '1993-07-01'
    AND o_orderdate < DATE '1993-07-01' + INTERVAL 3 MONTH
    AND EXISTS (
      SELECT *
      FROM lineitem
      WHERE l_orderkey = o_orderkey
        AND l_commitdate < l_receiptdate
    )
  GROUP BY o_custkey, o_orderpriority
) t)")));
	}
	if (query.name == "q13") {
		return 1.0;
	}
	if (query.name == "q16") {
		return std::max(1.0, std::ceil(ReadScalarDouble(con, R"(
SELECT COALESCE(MAX(cnt), 1)
FROM (
  SELECT ps_partkey, COUNT(DISTINCT ps_suppkey) AS cnt
  FROM partsupp, part
  WHERE p_partkey = ps_partkey
    AND p_brand <> 'Brand#45'
    AND p_type NOT LIKE 'MEDIUM POLISHED%'
    AND p_size IN (49, 14, 23, 45, 19, 3, 36, 9)
    AND ps_suppkey NOT IN (
      SELECT s_suppkey
      FROM supplier
      WHERE s_comment LIKE '%Customer%Complaints%'
    )
  GROUP BY ps_partkey
) t)")));
	}
	if (query.name == "q21") {
		return std::max(1.0, std::ceil(ReadScalarDouble(con, R"(
SELECT COALESCE(MAX(cnt), 1)
FROM (
  SELECT o_custkey, s_name, COUNT(*) AS cnt
  FROM supplier, lineitem l1, orders, nation
  WHERE s_suppkey = l1.l_suppkey
    AND o_orderkey = l1.l_orderkey
    AND o_orderstatus = 'F'
    AND l1.l_receiptdate > l1.l_commitdate
    AND EXISTS (
      SELECT *
      FROM lineitem l2
      WHERE l2.l_orderkey = l1.l_orderkey
        AND l2.l_suppkey <> l1.l_suppkey
    )
    AND NOT EXISTS (
      SELECT *
      FROM lineitem l3
      WHERE l3.l_orderkey = l1.l_orderkey
        AND l3.l_suppkey <> l1.l_suppkey
        AND l3.l_receiptdate > l3.l_commitdate
    )
    AND s_nationkey = n_nationkey
    AND n_name = 'SAUDI ARABIA'
  GROUP BY o_custkey, s_name
) t)")));
	}
	return InferCountBound(con, query);
}

static double InferPerPuGroupBound(Connection &con, const QuerySpec &query, PrivacyProfile profile) {
	switch (profile) {
	case PrivacyProfile::CUSTOMER_ROW:
	case PrivacyProfile::LINEITEM:
	case PrivacyProfile::ORDERS:
	case PrivacyProfile::PARTSUPP:
		return 1.0;
	default:
		break;
	}

	if (query.name == "q1") {
		return std::max(1.0, std::ceil(ReadScalarDouble(con, R"(
SELECT COALESCE(MAX(groups_contributed), 1)
FROM (
  SELECT o_custkey, COUNT(*) AS groups_contributed
  FROM (
    SELECT DISTINCT o_custkey, l_returnflag, l_linestatus
    FROM lineitem, orders
    WHERE l_orderkey = o_orderkey
      AND l_shipdate <= DATE '1998-12-01' - INTERVAL 90 DAY
  ) t
  GROUP BY o_custkey
) u)")));
	}
	if (query.name == "q4") {
		return std::max(1.0, std::ceil(ReadScalarDouble(con, R"(
SELECT COALESCE(MAX(groups_contributed), 1)
FROM (
  SELECT o_custkey, COUNT(*) AS groups_contributed
  FROM (
    SELECT DISTINCT o_custkey, o_orderpriority
    FROM orders
    WHERE o_orderdate >= DATE '1993-07-01'
      AND o_orderdate < DATE '1993-07-01' + INTERVAL 3 MONTH
      AND EXISTS (
        SELECT *
        FROM lineitem
        WHERE l_orderkey = o_orderkey
          AND l_commitdate < l_receiptdate
      )
  ) t
  GROUP BY o_custkey
) u)")));
	}
	if (query.name == "q13") {
		return 1.0;
	}
	if (query.name == "q16") {
		return 1.0;
	}
	if (query.name == "q21") {
		return std::max(1.0, std::ceil(ReadScalarDouble(con, R"(
SELECT COALESCE(MAX(groups_contributed), 1)
FROM (
  SELECT o_custkey, COUNT(*) AS groups_contributed
  FROM (
    SELECT DISTINCT o_custkey, s_name
    FROM supplier, lineitem l1, orders, nation
    WHERE s_suppkey = l1.l_suppkey
      AND o_orderkey = l1.l_orderkey
      AND o_orderstatus = 'F'
      AND l1.l_receiptdate > l1.l_commitdate
      AND EXISTS (
        SELECT *
        FROM lineitem l2
        WHERE l2.l_orderkey = l1.l_orderkey
          AND l2.l_suppkey <> l1.l_suppkey
      )
      AND NOT EXISTS (
        SELECT *
        FROM lineitem l3
        WHERE l3.l_orderkey = l1.l_orderkey
          AND l3.l_suppkey <> l1.l_suppkey
          AND l3.l_receiptdate > l3.l_commitdate
      )
      AND s_nationkey = n_nationkey
      AND n_name = 'SAUDI ARABIA'
  ) t
  GROUP BY o_custkey
) u)")));
	}
	return 1.0;
}

static void SetupCustomerPrivacy(Connection &con) {
	RunStatement(con, "PRAGMA clear_privacy_metadata");
	RunStatement(con, "ALTER TABLE customer ADD PRIVACY_KEY (c_custkey)");
	RunStatement(con, "ALTER TABLE customer SET PU");
	RunStatement(con, "ALTER TABLE orders ADD PRIVACY_LINK (o_custkey) REFERENCES customer(c_custkey)");
	RunStatement(con, "ALTER TABLE lineitem ADD PRIVACY_LINK (l_orderkey) REFERENCES orders(o_orderkey)");
}

static void SetupCustomerRowPrivacy(Connection &con) {
	RunStatement(con, "PRAGMA clear_privacy_metadata");
	RunStatement(con, "CREATE TABLE IF NOT EXISTS customer_row_pu(c_custkey BIGINT)");
	RunStatement(con, "INSERT INTO customer_row_pu "
	                  "SELECT c_custkey FROM customer "
	                  "EXCEPT SELECT c_custkey FROM customer_row_pu");
	RunStatement(con, "ALTER TABLE customer_row_pu ADD PRIVACY_KEY (c_custkey)");
	RunStatement(con, "ALTER TABLE customer_row_pu SET PU");
	RunStatement(con, "ALTER TABLE customer ADD PRIVACY_LINK (c_custkey) REFERENCES customer_row_pu(c_custkey)");
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

static void SetupLineitemPrivacy(Connection &con) {
	RunStatement(con, "PRAGMA clear_privacy_metadata");
	if (!ColumnExists(con, "lineitem", "l_privacy_key")) {
		RunStatement(con, "ALTER TABLE lineitem ADD COLUMN l_privacy_key UBIGINT");
	}
	RunStatement(con, "UPDATE lineitem SET l_privacy_key = CAST(l_orderkey AS UBIGINT) * 100 + "
	                  "CAST(l_linenumber AS UBIGINT) WHERE l_privacy_key IS NULL");
	RunStatement(con, "CREATE TABLE IF NOT EXISTS lineitem_row_pu(l_privacy_key UBIGINT)");
	RunStatement(con, "INSERT INTO lineitem_row_pu "
	                  "SELECT l_privacy_key FROM lineitem WHERE l_privacy_key IS NOT NULL "
	                  "EXCEPT SELECT l_privacy_key FROM lineitem_row_pu");
	RunStatement(con, "ALTER TABLE lineitem_row_pu ADD PRIVACY_KEY (l_privacy_key)");
	RunStatement(con, "ALTER TABLE lineitem_row_pu SET PU");
	RunStatement(con,
	             "ALTER TABLE lineitem ADD PRIVACY_LINK (l_privacy_key) REFERENCES lineitem_row_pu(l_privacy_key)");
}

static void SetupOrdersPrivacy(Connection &con) {
	RunStatement(con, "PRAGMA clear_privacy_metadata");
	RunStatement(con, "CREATE TABLE IF NOT EXISTS orders_row_pu(o_orderkey BIGINT)");
	RunStatement(con, "INSERT INTO orders_row_pu "
	                  "SELECT o_orderkey FROM orders "
	                  "EXCEPT SELECT o_orderkey FROM orders_row_pu");
	RunStatement(con, "ALTER TABLE orders_row_pu ADD PRIVACY_KEY (o_orderkey)");
	RunStatement(con, "ALTER TABLE orders_row_pu SET PU");
	RunStatement(con, "ALTER TABLE orders ADD PRIVACY_LINK (o_orderkey) REFERENCES orders_row_pu(o_orderkey)");
	RunStatement(con, "ALTER TABLE lineitem ADD PRIVACY_LINK (l_orderkey) REFERENCES orders(o_orderkey)");
}

static void SetupPartsuppPrivacy(Connection &con) {
	RunStatement(con, "PRAGMA clear_privacy_metadata");
	if (!ColumnExists(con, "partsupp", "ps_privacy_key")) {
		RunStatement(con, "ALTER TABLE partsupp ADD COLUMN ps_privacy_key UBIGINT");
	}
	RunStatement(con, "UPDATE partsupp SET ps_privacy_key = CAST(ps_partkey AS UBIGINT) * 10000000 + "
	                  "CAST(ps_suppkey AS UBIGINT) WHERE ps_privacy_key IS NULL");
	RunStatement(con, "CREATE TABLE IF NOT EXISTS partsupp_row_pu(ps_privacy_key UBIGINT)");
	RunStatement(con, "INSERT INTO partsupp_row_pu "
	                  "SELECT ps_privacy_key FROM partsupp WHERE ps_privacy_key IS NOT NULL "
	                  "EXCEPT SELECT ps_privacy_key FROM partsupp_row_pu");
	RunStatement(con, "ALTER TABLE partsupp_row_pu ADD PRIVACY_KEY (ps_privacy_key)");
	RunStatement(con, "ALTER TABLE partsupp_row_pu SET PU");
	RunStatement(con,
	             "ALTER TABLE partsupp ADD PRIVACY_LINK (ps_privacy_key) REFERENCES partsupp_row_pu(ps_privacy_key)");
	RunStatement(con, "ALTER TABLE part ADD PRIVACY_KEY (p_partkey)");
}

static void SetupPrivacy(Connection &con, PrivacyProfile profile) {
	switch (profile) {
	case PrivacyProfile::CUSTOMER:
		SetupCustomerPrivacy(con);
		return;
	case PrivacyProfile::CUSTOMER_ROW:
		SetupCustomerRowPrivacy(con);
		return;
	case PrivacyProfile::PART:
		SetupPartPrivacy(con);
		return;
	case PrivacyProfile::LINEITEM:
		SetupLineitemPrivacy(con);
		return;
	case PrivacyProfile::ORDERS:
		SetupOrdersPrivacy(con);
		return;
	case PrivacyProfile::PARTSUPP:
		SetupPartsuppPrivacy(con);
		return;
	}
	throw std::runtime_error("Unknown privacy profile");
}

static double ReadPrivacyUnitCount(Connection &con, PrivacyProfile profile) {
	switch (profile) {
	case PrivacyProfile::CUSTOMER:
		return ReadScalarDouble(con, "SELECT COUNT(DISTINCT c_custkey) FROM customer");
	case PrivacyProfile::CUSTOMER_ROW:
		return ReadScalarDouble(con, "SELECT COUNT(DISTINCT c_custkey) FROM customer");
	case PrivacyProfile::PART:
		return ReadScalarDouble(con, "SELECT COUNT(DISTINCT p_partkey) FROM part");
	case PrivacyProfile::LINEITEM:
		return ReadScalarDouble(con, "SELECT COUNT(*) FROM lineitem");
	case PrivacyProfile::ORDERS:
		return ReadScalarDouble(con, "SELECT COUNT(DISTINCT o_orderkey) FROM orders");
	case PrivacyProfile::PARTSUPP:
		return ReadScalarDouble(con, "SELECT COUNT(*) FROM partsupp");
	}
	throw std::runtime_error("Unknown privacy profile");
}

static PrivacyProfile SelectPrivacyProfile(const QuerySpec &query, BenchmarkProfile profile) {
	if (profile == BenchmarkProfile::ENTITY) {
		return query.privacy_profile;
	}
	if (query.name == "q1") {
		return PrivacyProfile::LINEITEM;
	}
	if (query.name == "q4") {
		return PrivacyProfile::ORDERS;
	}
	if (query.name == "q13") {
		return PrivacyProfile::CUSTOMER_ROW;
	}
	if (query.name == "q16") {
		return PrivacyProfile::PARTSUPP;
	}
	if (query.name == "q21") {
		return PrivacyProfile::LINEITEM;
	}
	return query.privacy_profile;
}

static double DefaultDelta(double epsilon, double pu_count) {
	if (pu_count <= 1.0 || !std::isfinite(pu_count)) {
		throw std::runtime_error("Invalid privacy-unit count");
	}
	double log_pu = std::log(pu_count);
	return std::exp(-epsilon * log_pu * log_pu);
}

static vector<QuerySpec> GetQueries() {
	return {{"q1", "TPC-H Q1 count by return flag/status", 2, PrivacyProfile::CUSTOMER,
	         R"(SELECT l_returnflag, l_linestatus, COUNT(*) AS count_order
FROM lineitem
WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL 90 DAY
GROUP BY l_returnflag, l_linestatus
ORDER BY l_returnflag, l_linestatus)"},
	        {"q4", "TPC-H Q4 count by order priority", 1, PrivacyProfile::CUSTOMER,
	         R"(SELECT o_orderpriority, COUNT(*) AS order_count
FROM orders
WHERE o_orderdate >= DATE '1993-07-01'
  AND o_orderdate < DATE '1993-07-01' + INTERVAL 3 MONTH
  AND EXISTS (
    SELECT *
    FROM lineitem
    WHERE l_orderkey = o_orderkey
      AND l_commitdate < l_receiptdate
  )
GROUP BY o_orderpriority
ORDER BY o_orderpriority)"},
	        {"q13", "TPC-H Q13 customer order-count histogram", 1, PrivacyProfile::CUSTOMER,
	         R"(SELECT c_count, COUNT(*) AS custdist
FROM (
  SELECT c_custkey, COUNT(o_orderkey) AS c_count
  FROM customer
  LEFT OUTER JOIN orders
    ON c_custkey = o_custkey
   AND o_comment NOT LIKE '%special%requests%'
  GROUP BY c_custkey
) AS c_orders
GROUP BY c_count
ORDER BY custdist DESC, c_count DESC)"},
	        {"q16", "TPC-H Q16 distinct suppliers by part attributes", 3, PrivacyProfile::PART,
	         R"(SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS supplier_cnt
FROM partsupp, part
WHERE p_partkey = ps_partkey
  AND p_brand <> 'Brand#45'
  AND p_type NOT LIKE 'MEDIUM POLISHED%'
  AND p_size IN (49, 14, 23, 45, 19, 3, 36, 9)
  AND ps_suppkey NOT IN (
    SELECT s_suppkey
    FROM supplier
    WHERE s_comment LIKE '%Customer%Complaints%'
)
GROUP BY p_brand, p_type, p_size
ORDER BY supplier_cnt DESC, p_brand, p_type, p_size)"},
	        {"q21", "TPC-H Q21 suppliers who kept orders waiting", 1, PrivacyProfile::CUSTOMER,
	         R"(SELECT s_name, COUNT(*) AS numwait
FROM supplier, lineitem l1, orders, nation
WHERE s_suppkey = l1.l_suppkey
  AND o_orderkey = l1.l_orderkey
  AND o_orderstatus = 'F'
  AND l1.l_receiptdate > l1.l_commitdate
  AND EXISTS (
    SELECT *
    FROM lineitem l2
    WHERE l2.l_orderkey = l1.l_orderkey
      AND l2.l_suppkey <> l1.l_suppkey
  )
  AND NOT EXISTS (
    SELECT *
    FROM lineitem l3
    WHERE l3.l_orderkey = l1.l_orderkey
      AND l3.l_suppkey <> l1.l_suppkey
      AND l3.l_receiptdate > l3.l_commitdate
  )
  AND s_nationkey = n_nationkey
  AND n_name = 'SAUDI ARABIA'
GROUP BY s_name
ORDER BY numwait DESC, s_name
LIMIT 100)"}};
}

static string GetQuerySqlForMode(const QuerySpec &query, const string &mode, BenchmarkProfile profile) {
	if (mode == "dp_standard" && profile == BenchmarkProfile::ENTITY && query.name == "q1") {
		return R"(SELECT l_returnflag, l_linestatus, COUNT(*) AS count_order
FROM lineitem, orders
WHERE l_orderkey = o_orderkey
  AND l_shipdate <= DATE '1998-12-01' - INTERVAL 90 DAY
GROUP BY l_returnflag, l_linestatus
ORDER BY l_returnflag, l_linestatus)";
	}
	if (mode == "dp_standard" && query.name == "q13") {
		return R"(WITH public_bins(c_count_bucket) AS (
  SELECT i FROM range(0, 9) t(i)
),
c_orders AS (
  SELECT c_custkey,
         CASE
           WHEN COUNT(o_orderkey) = 0 THEN 0
           WHEN COUNT(o_orderkey) = 1 THEN 1
           WHEN COUNT(o_orderkey) = 2 THEN 2
           WHEN COUNT(o_orderkey) = 3 THEN 3
           WHEN COUNT(o_orderkey) = 4 THEN 4
           WHEN COUNT(o_orderkey) = 5 THEN 5
           WHEN COUNT(o_orderkey) <= 10 THEN 6
           WHEN COUNT(o_orderkey) <= 20 THEN 7
           ELSE 8
         END AS c_count_bucket
  FROM customer
  LEFT OUTER JOIN orders
    ON c_custkey = o_custkey
   AND o_comment NOT LIKE '%special%requests%'
  GROUP BY c_custkey
)
SELECT public_bins.c_count_bucket, COUNT(c_orders.c_custkey) AS custdist
FROM public_bins
LEFT JOIN c_orders ON public_bins.c_count_bucket = c_orders.c_count_bucket
GROUP BY public_bins.c_count_bucket
ORDER BY public_bins.c_count_bucket)";
	}
	return query.sql;
}

static string DefaultDbPath(double sf, const string &dataset) {
	return dataset + "_sass_sf" + FormatNumber(sf) + ".db";
}

static string DefaultOutPath(double sf, const string &dataset) {
	return "benchmark/tpch/dp_sass_" + dataset + "_utility_sf" + FormatNumber(sf) + ".csv";
}

static string BenchmarkProfileName(BenchmarkProfile profile) {
	switch (profile) {
	case BenchmarkProfile::ENTITY:
		return "entity";
	case BenchmarkProfile::FLEX_ROW:
		return "flex-row";
	}
	throw std::runtime_error("Unknown benchmark profile");
}

static Config ParseArgs(int argc, char **argv) {
	Config config;
	for (int i = 1; i < argc; i++) {
		string arg = argv[i];
		auto eq = arg.find('=');
		string key = eq == string::npos ? arg : arg.substr(0, eq);
		string value = eq == string::npos ? "" : arg.substr(eq + 1);
		if (key == "--sf") {
			config.scale_factor = std::stod(value);
		} else if (key == "--dataset") {
			if (value != "tpch" && value != "jcch") {
				throw std::runtime_error("--dataset must be 'tpch' or 'jcch'");
			}
			config.dataset = value;
		} else if (key == "--db") {
			config.db_path = value;
		} else if (key == "--out") {
			config.out_path = value;
		} else if (key == "--mode") {
			config.mode = value;
			config.modes = ParseModeList(value);
		} else if (key == "--modes") {
			config.mode = value;
			config.modes = ParseModeList(value);
		} else if (key == "--profile") {
			if (value == "entity") {
				config.profile = BenchmarkProfile::ENTITY;
			} else if (value == "flex-row") {
				config.profile = BenchmarkProfile::FLEX_ROW;
			} else {
				throw std::runtime_error("Unknown --profile: " + value + " (expected entity or flex-row)");
			}
		} else if (key == "--runs") {
			config.runs = static_cast<idx_t>(std::stoull(value));
		} else if (key == "--epsilon") {
			config.epsilon = std::stod(value);
		} else if (key == "--delta") {
			config.delta = std::stod(value);
		} else if (key == "--sample-lanes") {
			config.sample_lanes = std::stoi(value);
		} else if (key == "--threads") {
			config.threads = std::stoi(value);
		} else if (key == "--bound-multiplier") {
			config.bound_multipliers = ParseNumberList(value);
		} else if (key == "--bound-sweep") {
			config.bound_multipliers = ParseNumberList(value);
		} else if (key == "--count-bound") {
			config.public_count_bound = std::stod(value);
		} else if (key == "--group-bound") {
			config.public_group_bound = std::stod(value);
		} else if (key == "--release") {
			if (value == "both") {
				config.sass_releases = {"median", "average"};
			} else if (value == "median" || value == "average") {
				config.sass_releases = {value};
			} else {
				throw std::runtime_error("--release must be 'median', 'average', or 'both'");
			}
		} else if (arg == "--help" || arg == "-h") {
			std::cout << "Usage: dp_sass_tpch_utility [--sf=N] [--db=PATH] [--out=PATH] [--mode=MODE]\n"
			          << "                            [--runs=N] [--epsilon=E] [--delta=D]\n"
			          << "                            [--sample-lanes=N] [--threads=N] [--profile=PROFILE]\n"
			          << "                            [--bound-sweep=CSV] [--count-bound=N] [--group-bound=N]\n"
			          << "                            [--release=median|average|both] [--dataset=tpch|jcch]\n"
			          << "  MODE is dp_sass, dp_elastic, dp_standard, all, or a comma-separated list\n"
			          << "  --dataset=jcch applies JCC-H-style skew (heavy-tailed per-customer volume)\n"
			          << "  --release selects the dp_sass release method(s); 'both' compares median vs average\n";
			std::cout << "  PROFILE is entity or flex-row\n";
			std::cout << "  --bound-sweep multiplies public count bounds, e.g. 1,2,5,10,100\n";
			std::exit(0);
		} else {
			throw std::runtime_error("Unknown argument: " + arg);
		}
	}
	if (config.db_path.empty()) {
		config.db_path = DefaultDbPath(config.scale_factor, config.dataset);
	}
	if (config.out_path.empty()) {
		config.out_path = DefaultOutPath(config.scale_factor, config.dataset);
	}
	if (config.public_count_bound <= 0.0 || !std::isfinite(config.public_count_bound)) {
		throw std::runtime_error("--count-bound must be a positive finite number");
	}
	if (config.public_group_bound <= 0.0 || !std::isfinite(config.public_group_bound)) {
		throw std::runtime_error("--group-bound must be a positive finite number");
	}
	return config;
}

static void WriteResult(std::ofstream &csv, const QuerySpec &query, idx_t run, bool success, const string &error,
                        const string &utility, const string &recall, const string &precision,
                        const string &median_error_pct, double time_ms, double epsilon, double delta,
                        double count_bound, double group_bound, double sass_count_output_bound, double bound_multiplier,
                        const string &mode, const string &release, const string &profile, int sample_lanes,
                        uint64_t seed) {
	csv << query.name << "," << mode << "," << release << "," << profile << "," << CsvQuote(query.description) << ","
	    << query.key_cols << "," << run << "," << (success ? "true" : "false") << "," << CsvQuote(error) << ","
	    << utility << "," << recall << "," << precision << "," << median_error_pct << "," << FormatNumber(time_ms)
	    << "," << FormatNumber(epsilon) << "," << FormatNumber(delta) << "," << FormatNumber(count_bound) << ","
	    << FormatNumber(group_bound) << "," << FormatNumber(sass_count_output_bound) << ","
	    << FormatNumber(bound_multiplier) << "," << sample_lanes << "," << seed << "\n";
	csv.flush();
}

static int Main(int argc, char **argv) {
	Config config = ParseArgs(argc, argv);
	DuckDB db(config.db_path);
	Connection con(db);

	EnsureTpchData(con, config.scale_factor, config.dataset);
	RunStatement(con, "LOAD privacy");
	RunStatement(con, "SET threads=" + std::to_string(config.threads));
	RunStatement(con, "SET privacy_noise=true");

	auto queries = GetQueries();
	RunStatement(con, "PRAGMA clear_privacy_metadata");
	vector<double> sass_output_bounds(queries.size(), 1.0);
	vector<double> deltas(queries.size(), config.delta);
	for (idx_t i = 0; i < queries.size(); i++) {
		auto privacy_profile = SelectPrivacyProfile(queries[i], config.profile);
		double pu_count = ReadPrivacyUnitCount(con, privacy_profile);
		if (config.delta <= 0.0) {
			deltas[i] = DefaultDelta(config.epsilon, pu_count);
		}
		Log(queries[i].name + " privacy units: " + FormatNumber(pu_count) + ", dp_delta=" + FormatNumber(deltas[i]));
		sass_output_bounds[i] = PublicCountOutputBound(queries[i], config.scale_factor);
		Log(queries[i].name + " public dp_sass_count_output_bound=" + FormatNumber(sass_output_bounds[i]));
	}
	Log("dataset=" + config.dataset);
	Log("privacy_mode=" + config.mode);
	Log("benchmark_profile=" + BenchmarkProfileName(config.profile));
	Log("dp_epsilon=" + FormatNumber(config.epsilon));
	Log("public dp_count_bound=" + FormatNumber(config.public_count_bound));
	Log("public dp_max_groups_contributed=" + FormatNumber(config.public_group_bound));

	std::ofstream csv(config.out_path);
	if (!csv.is_open()) {
		throw std::runtime_error("Could not open output CSV: " + config.out_path);
	}
	csv << "query,mode,release,profile,description,key_cols,run,success,error,utility,recall,precision,"
	       "median_error_pct,time_ms,epsilon,delta,"
	       "dp_count_bound,dp_max_groups_contributed,dp_sass_count_output_bound,bound_multiplier,"
	       "dp_sample_lanes,seed\n";

	string utility_path = "/tmp/dp_tpch_utility_" + std::to_string(getpid()) + ".csv";
	for (const auto &mode : config.modes) {
		for (idx_t run = 0; run < config.runs; run++) {
			for (idx_t qi = 0; qi < queries.size(); qi++) {
				const auto &query = queries[qi];
				const auto &multipliers =
				    (mode == "dp_standard" || mode == "dp_sass") ? config.bound_multipliers : vector<double> {1.0};
				// dp_sass sweeps the requested release method(s); other modes have no release dimension.
				const vector<string> releases = (mode == "dp_sass") ? config.sass_releases : vector<string> {"-"};
				for (const auto &release : releases)
					for (idx_t mi = 0; mi < multipliers.size(); mi++) {
					double bound_multiplier = multipliers[mi];
					double count_bound = config.public_count_bound * bound_multiplier;
					double group_bound = config.public_group_bound;
					uint64_t seed = 1000003ULL + 9973ULL * static_cast<uint64_t>(run) + static_cast<uint64_t>(qi);
					std::remove(utility_path.c_str());
					string utility;
					string recall;
					string precision;
					string median_error_pct;
					string error;
					bool success = false;
					double time_ms = 0.0;
					try {
						SetupPrivacy(con, SelectPrivacyProfile(query, config.profile));
						RunStatement(con, "SET privacy_mode='" + mode + "'");
						RunStatement(con, "SET dp_epsilon=" + FormatNumber(config.epsilon));
						if (mode == "dp_standard") {
							RunStatement(con, "SET dp_delta=NULL");
							RunStatement(con, "SET dp_count_bound=" + FormatNumber(count_bound));
							RunStatement(con, "SET dp_max_groups_contributed=" + FormatNumber(group_bound));
						} else {
							RunStatement(con, "SET dp_delta=" + FormatNumber(deltas[qi]));
							if (mode == "dp_sass") {
								RunStatement(con, "SET dp_sample_lanes=" + std::to_string(config.sample_lanes));
								RunStatement(con, "SET dp_count_bound=" + FormatNumber(count_bound));
								RunStatement(con, "SET dp_max_groups_contributed=" + FormatNumber(group_bound));
								RunStatement(con,
								             "SET dp_sass_count_output_bound=" + FormatNumber(sass_output_bounds[qi]));
								RunStatement(con, "SET dp_sass_release='" + release + "'");
								if (release == "average") {
									// GUPT mean release is pure ε-DP — no δ needed.
									RunStatement(con, "SET dp_delta=NULL");
								}
							} else {
								RunStatement(con, "SET dp_count_bound=NULL");
							}
						}
						RunStatement(con, "SET privacy_seed=" + std::to_string(seed));
						RunStatement(con, "SET privacy_diffcols=" +
						                      SqlQuote(std::to_string(queries[qi].key_cols) + ":" + utility_path));
						auto start = std::chrono::steady_clock::now();
						RunQuery(con, GetQuerySqlForMode(query, mode, config.profile));
						auto end = std::chrono::steady_clock::now();
						time_ms = std::chrono::duration<double, std::milli>(end - start).count();
						if (!ReadLastUtilityLine(utility_path, utility, recall, precision, median_error_pct)) {
							throw std::runtime_error("privacy_diffcols did not write utility output; query may not "
							                         "touch the customer PU chain");
						}
						success = true;
						Log(mode + (mode == "dp_sass" ? "/" + release : "") + " " + query.name + " run " +
						    std::to_string(run) + " bound_multiplier=" + FormatNumber(bound_multiplier) +
						    " utility=" + utility);
					} catch (const std::exception &e) {
						error = e.what();
						if (error.size() > 500) {
							error = error.substr(0, 500);
						}
						Log(mode + (mode == "dp_sass" ? "/" + release : "") + " " + query.name + " run " +
						    std::to_string(run) + " bound_multiplier=" + FormatNumber(bound_multiplier) +
						    " failed: " + error);
					}
					RunStatement(con, "SET privacy_diffcols=NULL");
					WriteResult(csv, query, run, success, error, utility, recall, precision, median_error_pct, time_ms,
					            config.epsilon, mode == "dp_standard" ? 0.0 : deltas[qi],
					            mode == "dp_elastic" ? 0.0 : count_bound, mode == "dp_elastic" ? 0.0 : group_bound,
					            mode == "dp_sass" ? sass_output_bounds[qi] : 0.0, bound_multiplier, mode, release,
					            BenchmarkProfileName(config.profile), mode == "dp_sass" ? config.sample_lanes : 0,
					            seed);
				}
			}
		}
	}
	std::remove(utility_path.c_str());
	Log("Wrote " + config.out_path);
	return 0;
}

} // namespace duckdb

int main(int argc, char **argv) {
	try {
		return duckdb::Main(argc, argv);
	} catch (const std::exception &e) {
		std::cerr << "error: " << e.what() << std::endl;
		return 1;
	}
}
