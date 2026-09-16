// WIP rebuttal experiment: preliminary correctness/performance POC, not a publication-ready benchmark.
#include "duckdb.hpp"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace duckdb {

// AHRQ's standard MEPS BRR estimate for replicate r is
// SUM(BRRr * 2 * PERWT * outcome). as_sum applies the same half-sample correction.
static constexpr idx_t BRR_REPLICATES = 128;
static constexpr idx_t LANES_PER_MASK = 64;

struct MepsOptions {
	string persons_path;
	string replicates_path;
	string value_column = "TOTEXP24";
	string weight_column = "PERWT24F";
	string id_column = "DUPERSID";
	string panel_column = "PANEL";
};

static string QuoteIdentifier(const string &identifier) {
	string result = "\"";
	for (auto c : identifier) {
		result += c == '"' ? "\"\"" : string(1, c);
	}
	return result + "\"";
}

static string QuoteLiteral(const string &value) {
	string result = "'";
	for (auto c : value) {
		result += c == '\'' ? "''" : string(1, c);
	}
	return result + "'";
}

static string ReadRelation(const string &path) {
	if (path.size() >= 8 && path.substr(path.size() - 8) == ".parquet") {
		return "read_parquet(" + QuoteLiteral(path) + ")";
	}
	return "read_csv_auto(" + QuoteLiteral(path) + ", header = true, sample_size = -1)";
}

static unique_ptr<MaterializedQueryResult> QueryOrThrow(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	if (result->HasError()) {
		throw std::runtime_error(result->GetError() + "\nSQL: " + sql);
	}
	return result;
}

static string BrrColumn(idx_t replicate) {
	return "BRR" + std::to_string(replicate + 1);
}

static string MaskExpression(idx_t first_replicate) {
	std::ostringstream sql;
	for (idx_t lane = 0; lane < LANES_PER_MASK; lane++) {
		if (lane > 0) {
			sql << " | ";
		}
		sql << "(CAST(COALESCE(" << QuoteIdentifier(BrrColumn(first_replicate + lane)) << ", 0) AS UBIGINT) << " << lane
		    << ")";
	}
	return sql.str();
}

static void CreateSyntheticInputs(Connection &con) {
	QueryOrThrow(con, R"(
		CREATE TEMP TABLE persons_raw AS
		SELECT CAST(i AS VARCHAR) AS DUPERSID,
		       24 AS PANEL,
		       400.0 + CAST(hash(i, 7) % 1600 AS DOUBLE) AS PERWT24F,
		       CAST(hash(i, 11) % 5000000 AS DOUBLE) / 100.0 AS TOTEXP24
		FROM range(1, 20001) AS people(i)
	)");

	std::ostringstream sql;
	sql << "CREATE TEMP TABLE replicates_raw AS SELECT CAST(i AS VARCHAR) AS DUPERSID, 24 AS PANEL";
	for (idx_t replicate = 0; replicate < BRR_REPLICATES; replicate++) {
		sql << ", CAST((hash(i, " << replicate + 101 << ") & 1) AS UTINYINT) AS "
		    << QuoteIdentifier(BrrColumn(replicate));
	}
	sql << " FROM range(1, 20001) AS people(i)";
	QueryOrThrow(con, sql.str());
}

static void CreateFileInputs(Connection &con, const MepsOptions &options) {
	QueryOrThrow(con, "CREATE TEMP VIEW persons_raw AS SELECT * FROM " + ReadRelation(options.persons_path));
	QueryOrThrow(con, "CREATE TEMP VIEW replicates_raw AS SELECT * FROM " + ReadRelation(options.replicates_path));
}

static void CreateJoinedInput(Connection &con, const MepsOptions &options) {
	const auto id = QuoteIdentifier(options.id_column);
	const auto panel = QuoteIdentifier(options.panel_column);
	std::ostringstream sql;
	sql << "CREATE TEMP TABLE meps_brr AS SELECT "
	    << "CAST(p." << id << " AS VARCHAR) AS person_id, "
	    << "CAST(p." << panel << " AS BIGINT) AS panel, "
	    << "CAST(p." << QuoteIdentifier(options.weight_column) << " AS DOUBLE) AS person_weight, "
	    << "CAST(p." << QuoteIdentifier(options.value_column) << " AS DOUBLE) AS outcome";
	for (idx_t replicate = 0; replicate < BRR_REPLICATES; replicate++) {
		auto column = QuoteIdentifier(BrrColumn(replicate));
		sql << ", CAST(COALESCE(r." << column << ", 0) AS UTINYINT) AS " << column;
	}
	sql << " FROM persons_raw p JOIN replicates_raw r"
	    << " ON CAST(p." << id << " AS VARCHAR) = CAST(r." << id << " AS VARCHAR)"
	    << " AND CAST(p." << panel << " AS BIGINT) = CAST(r." << panel << " AS BIGINT)"
	    << " WHERE p." << QuoteIdentifier(options.weight_column) << " IS NOT NULL"
	    << " AND p." << QuoteIdentifier(options.value_column) << " IS NOT NULL";
	QueryOrThrow(con, sql.str());

	QueryOrThrow(con, "ALTER TABLE meps_brr ADD COLUMN mask_lo UBIGINT");
	QueryOrThrow(con, "ALTER TABLE meps_brr ADD COLUMN mask_hi UBIGINT");
	QueryOrThrow(con, "UPDATE meps_brr SET mask_lo = " + MaskExpression(0) + ", mask_hi = " + MaskExpression(64));
}

static vector<double> ReadList(const Value &value) {
	vector<double> result;
	for (const auto &child : ListValue::GetChildren(value)) {
		result.push_back(child.GetValue<double>());
	}
	return result;
}

static vector<double> RunSimd(Connection &con, double &elapsed_ms) {
	auto start = std::chrono::steady_clock::now();
	auto result = QueryOrThrow(con, R"(
		SELECT as_sum(mask_lo, person_weight * outcome),
		       as_sum(mask_hi, person_weight * outcome)
		FROM meps_brr
	)");
	elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

	auto totals = ReadList(result->GetValue(0, 0));
	auto high_totals = ReadList(result->GetValue(1, 0));
	totals.insert(totals.end(), high_totals.begin(), high_totals.end());
	return totals;
}

static vector<double> RunScalarOracle(Connection &con, double &elapsed_ms) {
	std::ostringstream sql;
	sql << "SELECT ";
	for (idx_t replicate = 0; replicate < BRR_REPLICATES; replicate++) {
		if (replicate > 0) {
			sql << ", ";
		}
		sql << "SUM(CASE WHEN " << QuoteIdentifier(BrrColumn(replicate))
		    << " <> 0 THEN 2.0 * person_weight * outcome ELSE 0.0 END)";
	}
	sql << " FROM meps_brr";

	auto start = std::chrono::steady_clock::now();
	auto result = QueryOrThrow(con, sql.str());
	elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

	vector<double> totals;
	for (idx_t replicate = 0; replicate < BRR_REPLICATES; replicate++) {
		totals.push_back(result->GetValue(replicate, 0).GetValue<double>());
	}
	return totals;
}

static double ReadScalar(Connection &con, const string &sql) {
	return QueryOrThrow(con, sql)->GetValue(0, 0).GetValue<double>();
}

static int RunPoc(const MepsOptions &options) {
	DuckDB db(nullptr);
	Connection con(db);
	QueryOrThrow(con, "SET threads = 1");
	QueryOrThrow(con, "SET pac_sample_diversity_check = false");
	QueryOrThrow(con, "SET priv_rewrite = false");

	if (options.persons_path.empty()) {
		std::cout << "Using deterministic synthetic MEPS-shaped input.\n";
		CreateSyntheticInputs(con);
	} else {
		std::cout << "Reading MEPS person file: " << options.persons_path << "\n"
		          << "Reading MEPS BRR file:    " << options.replicates_path << "\n";
		CreateFileInputs(con, options);
	}
	CreateJoinedInput(con, options);

	auto rows = ReadScalar(con, "SELECT COUNT(*)::DOUBLE FROM meps_brr");
	auto point_estimate = ReadScalar(con, "SELECT SUM(person_weight * outcome) FROM meps_brr");
	double simd_ms;
	double scalar_ms;
	auto simd = RunSimd(con, simd_ms);
	auto scalar = RunScalarOracle(con, scalar_ms);
	if (simd.size() != BRR_REPLICATES || scalar.size() != BRR_REPLICATES) {
		throw std::runtime_error("expected exactly 128 BRR estimates");
	}

	double max_absolute_error = 0;
	double max_relative_error = 0;
	double squared_deviation_sum = 0;
	for (idx_t replicate = 0; replicate < BRR_REPLICATES; replicate++) {
		auto absolute_error = std::abs(simd[replicate] - scalar[replicate]);
		auto relative_error = absolute_error / std::max(1.0, std::abs(scalar[replicate]));
		max_absolute_error = std::max(max_absolute_error, absolute_error);
		max_relative_error = std::max(max_relative_error, relative_error);
		auto deviation = simd[replicate] - point_estimate;
		squared_deviation_sum += deviation * deviation;
	}
	auto standard_error = std::sqrt(squared_deviation_sum / static_cast<double>(BRR_REPLICATES));

	std::cout << std::fixed << std::setprecision(3) << "Joined persons:       " << static_cast<uint64_t>(rows) << "\n"
	          << "Full-sample estimate: " << point_estimate << "\n"
	          << "BRR standard error:   " << standard_error << "\n"
	          << "SIMD-ASA query:       " << simd_ms << " ms\n"
	          << "Scalar oracle query:  " << scalar_ms << " ms\n"
	          << "Max absolute error:   " << max_absolute_error << "\n"
	          << std::scientific << "Max relative error:   " << max_relative_error << "\n";

	// as_sum currently materializes FLOAT lanes, so agreement is checked at FLOAT precision.
	if (max_relative_error > 1e-5) {
		std::cerr << "FAIL: SIMD and scalar BRR estimates disagree beyond FLOAT tolerance.\n";
		return 1;
	}
	std::cout << "PASS: all 128 SIMD lanes agree with scalar BRR totals at FLOAT precision.\n";
	return 0;
}

} // namespace duckdb

static void PrintUsage() {
	std::cout << "Usage:\n"
	          << "  meps_brr_poc\n"
	          << "  meps_brr_poc <persons.csv|parquet> <brr.csv|parquet> [value-column] [weight-column]\n\n"
	          << "The real-data mode expects DUPERSID and PANEL in both files, BRR1..BRR128 in the BRR file,\n"
	          << "and defaults to the 2024 MEPS columns TOTEXP24 and PERWT24F. With no files, it runs a\n"
	          << "deterministic synthetic fixture. This is a correctness POC, not the final benchmark.\n";
}

int main(int argc, char **argv) {
	try {
		if (argc == 2 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
			PrintUsage();
			return 0;
		}
		if (argc != 1 && (argc < 3 || argc > 5)) {
			PrintUsage();
			return 2;
		}

		duckdb::MepsOptions options;
		if (argc >= 3) {
			options.persons_path = argv[1];
			options.replicates_path = argv[2];
		}
		if (argc >= 4) {
			options.value_column = argv[3];
		}
		if (argc >= 5) {
			options.weight_column = argv[4];
		}
		return duckdb::RunPoc(options);
	} catch (const std::exception &exception) {
		std::cerr << "MEPS BRR POC failed: " << exception.what() << "\n";
		return 1;
	}
}
