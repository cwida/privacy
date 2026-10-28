// WIP: explicit standard / naive BRR / conditional / AS SQL; query-only timing as in SSB.
#include "duckdb.hpp"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace duckdb;
using Clock = std::chrono::steady_clock;
using Results = std::map<string, std::vector<double>>;

static double Ms(Clock::time_point start) {
	return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
static string Quote(const string &s) {
	string out = "'";
	for (char c : s)
		out += c == '\'' ? "''" : string(1, c);
	return out + "'";
}
static string Read(const string &path) {
	std::ifstream f(path);
	if (!f)
		throw std::runtime_error("Cannot read " + path);
	std::ostringstream out;
	out << f.rdbuf();
	return out.str();
}
static void Save(const string &path, const string &sql) {
	std::ofstream f(path);
	if (!f)
		throw std::runtime_error("Cannot write " + path);
	f << sql;
}
static unique_ptr<MaterializedQueryResult> Query(Connection &con, const string &sql) {
	auto r = con.Query(sql);
	if (!r || r->HasError())
		throw std::runtime_error(r ? r->GetError() : "No query result");
	return r;
}
static void Require(Connection &con, const string &sql, const string &message) {
	if (!Query(con, sql)->GetValue(0, 0).GetValue<bool>())
		throw std::runtime_error(message);
}
static string ReplicateSQL(string sql, int rep) {
	const string token = "@REPLICATE@";
	auto pos = sql.find(token);
	if (pos == string::npos || sql.find(token, pos + token.size()) != string::npos)
		throw std::runtime_error("Expected exactly one BRR replicate token");
	sql.replace(pos, token.size(), std::to_string(rep + 1));
	return sql;
}
static double Number(const Value &v) {
	if (v.IsNull())
		throw std::runtime_error("Undefined estimate; empty replicate domains are not silently dropped");
	double x = v.GetValue<double>();
	if (!std::isfinite(x))
		throw std::runtime_error("Non-finite estimate");
	return x;
}
static Results Run(Connection &con, const string &sql, const string &mode, int worlds, double &elapsed) {
	Results out;
	elapsed = 0;
	for (int rep = 0; rep < (mode == "naive_as" ? worlds : 1); rep++) {
		string statement = mode == "naive_as" ? ReplicateSQL(sql, rep) : sql;
		auto start = Clock::now();
		auto r = Query(con, statement);
		// Estimate repeated execution from replicate 1, as in the TPC-H timing.
		// Evaluate remaining official replicates for validation and BRR SEs only.
		if (rep == 0)
			elapsed += Ms(start); // Planning, joins, masks, aggregates and materialization.
		if (r->RowCount() != 1)
			throw std::runtime_error("Expected one row of named scalar/list estimates");
		for (idx_t col = 0; col < r->ColumnCount(); col++) {
			string metric = r->names[col];
			auto value = r->GetValue(col, 0);
			if (mode == "duckdb" || mode == "naive_as") {
				out[metric].push_back(Number(value));
				continue;
			}
			if (mode == "conditional_sql") {
				if (value.IsNull())
					throw std::runtime_error("NULL conditional estimate list");
				for (const auto &lane : ListValue::GetChildren(value))
					out[metric].push_back(Number(lane));
				continue;
			}
			if (metric.size() < 3)
				throw std::runtime_error("Missing AS block suffix");
			string suffix = metric.substr(metric.size() - 3);
			if (suffix != "_lo" && suffix != "_hi")
				throw std::runtime_error("Missing AS block suffix");
			auto &values = out[metric.substr(0, metric.size() - 3)];
			if (values.size() != (suffix == "_lo" ? 0 : 64))
				throw std::runtime_error("AS blocks out of order");
			if (value.IsNull())
				throw std::runtime_error("NULL AS block");
			const auto &lanes = ListValue::GetChildren(value);
			if (lanes.size() != 64)
				throw std::runtime_error("Expected 64 AS lanes");
			for (const auto &lane : lanes)
				values.push_back(Number(lane));
		}
	}
	for (const auto &entry : out)
		if (entry.second.size() != idx_t(mode == "duckdb" ? 1 : worlds))
			throw std::runtime_error("Incomplete estimate output");
	if (mode == "naive_as")
		elapsed *= worlds;
	return out;
}
static double SE(const std::vector<double> &values, double center) {
	double ss = 0;
	for (double value : values)
		ss += (value - center) * (value - center);
	return std::sqrt(ss / values.size());
}
static double Mean(const std::vector<double> &values) {
	double sum = 0;
	for (double value : values)
		sum += value;
	return sum / values.size();
}
static double Compare(const Results &actual, const Results &reference) {
	if (actual.size() != reference.size())
		throw std::runtime_error("Metric count differs");
	double maximum = 0;
	for (const auto &entry : reference) {
		const auto &values = actual.at(entry.first);
		if (values.size() != entry.second.size())
			throw std::runtime_error("Replicate count differs");
		for (idx_t i = 0; i < values.size(); i++)
			maximum =
			    std::max(maximum, std::abs(values[i] - entry.second[i]) / std::max(1.0, std::abs(entry.second[i])));
	}
	return maximum;
}
int main(int argc, char **argv) {
	try {
		if (argc != 8) {
			std::cerr << "Usage: meps_brr_poc DATA_DIR NEW_DB QUERY_ROOT OUT_PREFIX THREADS REPETITIONS "
			             "damico2006|ahrq2016\n";
			return 2;
		}
		string data = argv[1], db_path = argv[2], root = argv[3], prefix = argv[4], workload = argv[7];
		int threads = std::stoi(argv[5]), repetitions = std::stoi(argv[6]);
		if (threads < 1 || repetitions < 1 || (workload != "damico2006" && workload != "ahrq2016"))
			throw std::runtime_error("Invalid workload, threads or repetitions");
		const int worlds = workload == "damico2006" ? 64 : 128;
		if (std::ifstream(db_path).good() || std::ifstream(prefix + "_timings.csv").good())
			throw std::runtime_error("Refusing to overwrite database/results");
		auto start = Clock::now();
		DuckDB db(db_path.c_str());
		Connection con(db);
		Query(con, "SET threads=" + std::to_string(threads));
		Query(con, "SET priv_rewrite=false");
		Query(con, "SET pac_sample_diversity_check=false");
		for (const auto &table : std::vector<string> {"persons_input", "brr", "events"}) {
			if (table == "events" && workload == "damico2006")
				continue;
			string file = table == "persons_input" ? "persons" : table;
			Query(con, "CREATE TABLE " + table + " AS SELECT * FROM read_csv(" + Quote(data + "/" + file + ".csv") +
			               ", header=true, sample_size=-1)");
		}
		double import_ms = Ms(start);
		start = Clock::now();
		Require(con, "SELECT count(*)=count(DISTINCT person_id) FROM persons_input", "Duplicate persons");
		Require(con, "SELECT count(*)=count(DISTINCT (person_id,panel)) FROM brr", "Duplicate BRR linkage");
		Require(con, "SELECT count(*)=0 FROM persons_input ANTI JOIN brr USING(person_id,panel)",
		        "Missing BRR linkage");
		Require(con,
		        "SELECT count(*)=0 FROM persons_input WHERE weight IS NULL OR weight<0 OR expenditure IS NULL "
		        "OR expenditure<0 OR age IS NULL",
		        "Invalid person input");
		if (workload == "ahrq2016") {
			Require(con, "SELECT count(*)=0 FROM events ANTI JOIN persons_input USING(person_id)", "Unmatched event");
			Require(con,
			        "SELECT count(*)=34655 AND count(*) FILTER(WHERE expenditure=0)=6933 AND "
			        "count(*) FILTER(WHERE weight>0)=33259 FROM persons_input",
			        "Incorrect 2016 source counts");
		}
		string invalid, masks[2];
		for (int rep = 0; rep < worlds; rep++) {
			string col = "BRR" + std::to_string(rep + 1);
			invalid += (rep ? " OR " : "") + col + " IS NULL OR " + col + " NOT IN (0,1)";
			auto &mask = masks[rep / 64];
			mask += (mask.empty() ? "" : " | ") + string("(") + col + "::UBIGINT << " + std::to_string(rep % 64) + ")";
		}
		Require(con, "SELECT count(*)=0 FROM brr WHERE " + invalid, "Non-binary/null BRR flags");
		string view = "CREATE VIEW persons_as AS SELECT p.*, (" + masks[0] + ") AS mask_lo";
		if (worlds > 64)
			view += ", (" + masks[1] + ") AS mask_hi";
		Query(con, view + " FROM persons_input p JOIN brr b USING(person_id,panel)");
		double validation_ms = Ms(start);
		std::ofstream timings(prefix + "_timings.csv"), estimates(prefix + "_estimates.csv"),
		    replicates(prefix + "_replicates.csv");
		if (!timings || !estimates || !replicates)
			throw std::runtime_error("Cannot open outputs");
		timings << "workload,query,mode,threads,run,worlds,query_ms,import_ms,validation_ms,max_normalized_error,"
		           "timing_scope\n";
		estimates << "workload,query,mode,metric,point_estimate,se_mse,se_replicate_mean\n";
		replicates << "workload,query,mode,metric,replicate,estimate\n";
		timings << std::setprecision(17);
		estimates << std::setprecision(17);
		replicates << std::setprecision(17);
		std::vector<string> names = workload == "damico2006"
		                                ? std::vector<string> {"q01_adults", "q02_nonelderly"}
		                                : std::vector<string> {"q01_office", "q02_office_physician", "q03_outpatient",
		                                                       "q04_outpatient_physician", "q05_visits_per_person"};
		const std::vector<string> modes = {"duckdb", "naive_as", "conditional_sql", "simd_as"};
		const std::vector<string> dirs = {"queries", "naive_queries", "conditional_queries", "as_queries"};
		for (const auto &name : names) {
			std::vector<string> sql;
			for (idx_t i = 0; i < modes.size(); i++) {
				sql.push_back(Read(root + "/" + workload + "/" + dirs[i] + "/" + name + ".sql"));
				Save(prefix + "_" + name + "_" + modes[i] + ".sql", i == 1 ? ReplicateSQL(sql.back(), 0) : sql.back());
				auto plan = Query(con, "EXPLAIN " + (i == 1 ? ReplicateSQL(sql.back(), 0) : sql.back()));
				Save(prefix + "_" + name + "_" + modes[i] + ".plan", plan->ToString());
			}
			Results point, reference;
			for (int run = 0; run < repetitions; run++) {
				for (idx_t step = 0; step < modes.size(); step++) {
					idx_t i = run == 0 ? step : (step + run) % modes.size();
					double elapsed;
					auto result = Run(con, sql[i], modes[i], worlds, elapsed);
					if (run == 0 && i == 0)
						point = result;
					if (run == 0 && i == 1)
						reference = result;
					double error = Compare(result, i == 0 ? point : reference);
					if (i != 0) {
						for (const auto &entry : reference) {
							const auto &actual = result.at(entry.first);
							double p = point.at(entry.first)[0];
							for (bool mse : {false, true}) {
								double expected = SE(entry.second, mse ? p : Mean(entry.second));
								double observed = SE(actual, mse ? p : Mean(actual));
								error =
								    std::max(error, std::abs(observed - expected) / std::max(1.0, std::abs(expected)));
							}
						}
					}
					// Preserve the prior FLOAT-output guard; record errors, do not claim exact equality.
					if (error > (modes[i] == "simd_as" ? 1e-3 : 1e-8))
						throw std::runtime_error("Estimate mismatch for " + name + "/" + modes[i]);
					timings << workload << ',' << name << ',' << modes[i] << ',' << threads << ',' << run << ','
					        << (i == 0 ? 1 : worlds) << ',' << elapsed << ',' << import_ms << ',' << validation_ms
					        << ',' << error << ',' << (i == 1 ? "replicate1_x_worlds" : "complete_query") << '\n';
					if (run == 0) {
						for (const auto &entry : result) {
							double p = point.at(entry.first)[0];
							double mse = i == 0 ? 0 : SE(entry.second, p);
							double centered = i == 0 ? 0 : SE(entry.second, Mean(entry.second));
							estimates << workload << ',' << name << ',' << modes[i] << ',' << entry.first << ',' << p
							          << ',' << mse << ',' << centered << '\n';
							if (i != 0)
								for (idx_t rep = 0; rep < entry.second.size(); rep++)
									replicates << workload << ',' << name << ',' << modes[i] << ',' << entry.first
									           << ',' << rep + 1 << ',' << entry.second[rep] << '\n';
							if (workload == "damico2006" && i == 1) {
								double expected_point = name == "q01_adults" ? 4009.1 : 3155.4;
								double expected_mse = name == "q01_adults" ? 83.6 : 77.5;
								double expected_centered = name == "q01_adults" ? 83.0 : 77.0;
								if (std::abs(p - expected_point) > 0.051 || std::abs(mse - expected_mse) > 0.051 ||
								    std::abs(centered - expected_centered) > 0.051)
									throw std::runtime_error(
									    "Damico Table 2 does not match published one-decimal values");
							}
						}
					}
					std::cout << workload << ' ' << name << ' ' << modes[i] << " run=" << run << " ms=" << elapsed
					          << " max_normalized_error=" << error << std::endl;
				}
			}
			timings.flush();
			estimates.flush();
			replicates.flush();
		}
		return 0;
	} catch (const std::exception &e) {
		std::cerr << "MEPS benchmark failed: " << e.what() << '\n';
		return 1;
	}
}
