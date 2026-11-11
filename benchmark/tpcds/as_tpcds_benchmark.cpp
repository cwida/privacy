// TPC-DS coverage and performance use the original SQL and the automatic rewriter.
// Execution success is not an independent validation of per-world correctness.
#include "duckdb.hpp"
#include "benchmark_json.hpp"
#include "benchmark_paths.hpp"

#include <chrono>
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

int main(int argc, char **argv) {
	try {
		if (argc != 3) {
			throw std::runtime_error("Usage: as_tpcds_benchmark CONFIG --prepare|QUERY_NUMBER");
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
		           "SET pac_sample_diversity_check=false; SET pac_conservative_mode=true;");
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
				std::ofstream file(queries + "/" + Name(id) + ".sql");
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
			            "pac_sample_diversity_check=false, pac_conservative_mode=true\n";
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
		std::ofstream csv(output + "/" + name + ".csv");
		if (!csv) {
			throw std::runtime_error("Cannot create result CSV");
		}
		csv << "query,mode,phase,status,rewritten,repetition,runtime_ms,rows,error\n";
		for (auto mode : {string("duckdb"), string("simd_asa_auto")}) {
			Query(con, "SET priv_rewrite=" + string(mode == "duckdb" ? "false" : "true"));
			bool rewritten = false;
			string phase = "explain";
			try {
				auto explain = Query(con, "EXPLAIN " + sql);
				string plan;
				for (idx_t row = 0; row < explain->RowCount(); row++) {
					plan += explain->Collection().GetValue(1, row).ToString() + "\n";
				}
				std::ofstream plan_file(output + "/" + name + "_" + mode + ".plan.txt");
				plan_file << plan;
				rewritten = plan.find("as_noised_") != string::npos || plan.find("as_sum(") != string::npos ||
				            plan.find("as_count(") != string::npos || plan.find("priv_select") != string::npos;
				if (mode != "duckdb" && !rewritten) {
					csv << name << ',' << mode << ",explain,no_stochastic_operator,0,0,,,\"\"\n";
					csv.flush();
					continue;
				}
				phase = "warmup";
				Query(con, sql);
				phase = "measure";
				for (int run = 1; run <= repetitions; run++) {
					auto start = std::chrono::steady_clock::now();
					auto result = Query(con, sql);
					auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
					csv << name << ',' << mode << ",measure,ok," << rewritten << ',' << run << ','
					    << std::setprecision(12) << elapsed.count() << ',' << result->RowCount() << ",\"\"\n";
					csv.flush();
				}
			} catch (const std::exception &error) {
				csv << name << ',' << mode << ',' << phase << ",error," << rewritten << ",0,,," << Csv(error.what())
				    << '\n';
				csv.flush();
			}
		}
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
