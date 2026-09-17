// WIP real-data MEPS BRR benchmark. Replaces the original single-query POC.
// Query definitions are contributions to a weighted total/ratio, not a general SQL rewriter.
#include "duckdb.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace duckdb;
using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point start) {
	return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
static string Quote(const string &s) {
	string out = "'";
	for (char c : s)
		out += c == '\'' ? "''" : string(1, c);
	return out + "'";
}
static unique_ptr<MaterializedQueryResult> Query(Connection &con, const string &sql) {
	auto r = con.Query(sql);
	if (!r || r->HasError())
		throw std::runtime_error(r ? r->GetError() : "No query result");
	return r;
}
static string Read(const string &path) {
	std::ifstream f(path);
	if (!f)
		throw std::runtime_error("Cannot read " + path);
	std::ostringstream out;
	out << f.rdbuf();
	return out.str();
}
static void Require(Connection &con, const string &sql, const string &message) {
	if (!Query(con, sql)->GetValue(0, 0).GetValue<bool>())
		throw std::runtime_error(message);
}
struct Cell {
	double point;
	std::vector<double> replicates;
};
using Results = std::map<string, Cell>;
static double Estimate(const Value &n, const Value &d, bool ratio) {
	if (n.IsNull() || (ratio && (d.IsNull() || d.GetValue<double>() == 0)))
		throw std::runtime_error("Undefined replicate estimate (empty domain); do not silently drop it");
	return n.GetValue<double>() / (ratio ? d.GetValue<double>() : 1.0);
}
static double StandardError(const Cell &c) {
	// BRR with full-estimate centering (survey mse=TRUE), not Taylor linearization.
	double ss = 0;
	for (double x : c.replicates)
		ss += (x - c.point) * (x - c.point);
	return std::sqrt(ss / 128.0);
}
static Results NativeReference(Connection &con, const string &source, int threads, double &extract_ms) {
	struct Group {
		string metric;
		bool ratio;
		std::vector<double> numerator, denominator;
		std::vector<uint64_t> lo, hi;
		Cell result;
	};
	// Extraction is included in native analysis time, not hidden in setup.
	auto extract_start = Clock::now();
	auto input = Query(con, "SELECT metric,numerator,denominator,is_ratio,mask_lo,mask_hi FROM (" + source +
	                            ") ORDER BY metric,person_id");
	std::vector<Group> groups;
	for (idx_t row = 0; row < input->RowCount(); row++) {
		string metric = input->GetValue(0, row).ToString();
		if (groups.empty() || groups.back().metric != metric)
			groups.push_back(Group {
			    metric, input->GetValue(3, row).GetValue<bool>(), {}, {}, {}, {}, {0, std::vector<double>(128)}});
		auto &g = groups.back();
		g.numerator.push_back(input->GetValue(1, row).GetValue<double>());
		g.denominator.push_back(input->GetValue(2, row).GetValue<double>());
		g.lo.push_back(input->GetValue(4, row).GetValue<uint64_t>());
		g.hi.push_back(input->GetValue(5, row).GetValue<uint64_t>());
	}
	extract_ms = Ms(extract_start);
	for (auto &g : groups) {
		double n = 0, d = 0;
		for (idx_t i = 0; i < g.numerator.size(); i++) {
			n += g.numerator[i];
			d += g.denominator[i];
		}
		g.result.point = n / (g.ratio ? d : 1.0);
	}
	// Replicate-major weighted reductions, parallelized over independent replicates.
	// No map mutation and each worker writes distinct, preallocated result elements.
	std::vector<std::thread> workers;
	int count = std::min(threads, 128);
	for (int worker = 0; worker < count; worker++)
		workers.emplace_back([&, worker]() {
			for (int rep = worker; rep < 128; rep += count)
				for (auto &g : groups) {
					double n = 0, d = 0;
					const auto &mask = rep < 64 ? g.lo : g.hi;
					for (idx_t i = 0; i < mask.size(); i++) {
						double weight = 2.0 * ((mask[i] >> (rep % 64)) & 1);
						n += weight * g.numerator[i];
						d += weight * g.denominator[i];
					}
					g.result.replicates[rep] = n / (g.ratio ? d : 1.0);
				}
		});
	for (auto &worker : workers)
		worker.join();
	Results out;
	for (auto &g : groups) {
		if (!std::isfinite(StandardError(g.result))) {
			throw std::runtime_error("Non-finite native BRR standard error");
		}
		out.emplace(g.metric, std::move(g.result));
	}
	return out;
}
static Results Analyze(Connection &con, const string &source, const string &mode, int threads, double &extract_ms) {
	extract_ms = 0;
	if (mode == "cpp_replicates")
		return NativeReference(con, source, threads, extract_ms);
	Results out;
	const string from = " FROM (" + source + ") contributions ";
	auto point = Query(con, "SELECT metric, sum(numerator), sum(denominator), bool_or(is_ratio)" + from +
	                            "GROUP BY metric ORDER BY metric");
	for (idx_t row = 0; row < point->RowCount(); row++) {
		out.emplace(point->GetValue(0, row).ToString(), Cell {Estimate(point->GetValue(1, row), point->GetValue(2, row),
		                                                               point->GetValue(3, row).GetValue<bool>()),
		                                                      {}});
	}
	if (mode == "asa") {
		auto r = Query(con, "SELECT metric, as_sum(mask_lo,numerator), as_sum(mask_hi,numerator),"
		                    "as_sum(mask_lo,denominator), as_sum(mask_hi,denominator), bool_or(is_ratio)" +
		                        from + "GROUP BY metric ORDER BY metric");
		for (idx_t row = 0; row < r->RowCount(); row++) {
			auto &c = out.at(r->GetValue(0, row).ToString());
			bool ratio = r->GetValue(5, row).GetValue<bool>();
			for (idx_t block = 0; block < 2; block++) {
				auto nv = r->GetValue(1 + block, row), dv = r->GetValue(3 + block, row);
				const auto &n = ListValue::GetChildren(nv), &d = ListValue::GetChildren(dv);
				if (n.size() != 64 || d.size() != 64)
					throw std::runtime_error("Expected 64 lanes");
				for (idx_t lane = 0; lane < 64; lane++)
					c.replicates.push_back(Estimate(n[lane], d[lane], ratio));
			}
		}
	} else if (mode == "repeated_sql") {
		// One complete conventional aggregate query per official replicate.
		for (int rep = 0; rep < 128; rep++) {
			string selected =
			    "((" + string(rep < 64 ? "mask_lo" : "mask_hi") + " >> " + std::to_string(rep % 64) + ") & 1) <> 0";
			auto r = Query(con, "SELECT metric, sum(CASE WHEN " + selected +
			                        " THEN 2.0*numerator ELSE 0 END), sum(CASE WHEN " + selected +
			                        " THEN 2.0*denominator ELSE 0 END), bool_or(is_ratio)" + from +
			                        "GROUP BY metric ORDER BY metric");
			for (idx_t row = 0; row < r->RowCount(); row++) {
				out.at(r->GetValue(0, row).ToString())
				    .replicates.push_back(
				        Estimate(r->GetValue(1, row), r->GetValue(2, row), r->GetValue(3, row).GetValue<bool>()));
			}
		}
	} else {
		// Stronger baseline: one shared relational query, 128 ordinary conditional SUM pairs.
		string sql = "SELECT metric, bool_or(is_ratio)";
		for (int rep = 0; rep < 128; rep++) {
			string selected =
			    "((" + string(rep < 64 ? "mask_lo" : "mask_hi") + " >> " + std::to_string(rep % 64) + ") & 1) <> 0";
			for (auto col : {"numerator", "denominator"})
				sql += ", sum(CASE WHEN " + selected + " THEN 2.0*" + col + " ELSE 0 END)";
		}
		auto r = Query(con, sql + from + "GROUP BY metric ORDER BY metric");
		for (idx_t row = 0; row < r->RowCount(); row++) {
			auto &c = out.at(r->GetValue(0, row).ToString());
			for (int rep = 0; rep < 128; rep++)
				c.replicates.push_back(Estimate(r->GetValue(2 + 2 * rep, row), r->GetValue(3 + 2 * rep, row),
				                                r->GetValue(1, row).GetValue<bool>()));
		}
	}
	for (auto &entry : out) {
		if (entry.second.replicates.size() != 128)
			throw std::runtime_error("Incomplete replicate output");
		if (!std::isfinite(StandardError(entry.second)))
			throw std::runtime_error("Non-finite BRR standard error");
	}
	return out;
}
static double Compare(const Results &actual, const Results &reference, double tolerance) {
	if (actual.size() != reference.size())
		throw std::runtime_error("Result group count differs");
	double maximum = 0;
	for (auto &entry : reference) {
		const auto &a = actual.at(entry.first), &b = entry.second;
		auto check = [&](double x, double y) {
			if (!std::isfinite(x) || !std::isfinite(y))
				throw std::runtime_error("Non-finite estimate");
			double err = std::abs(x - y) / std::max(1.0, std::abs(y));
			maximum = std::max(maximum, err);
			if (err > tolerance)
				throw std::runtime_error("Numerical tolerance exceeded for " + entry.first);
		};
		check(a.point, b.point);
		for (int rep = 0; rep < 128; rep++)
			check(a.replicates[rep], b.replicates[rep]);
		check(StandardError(a), StandardError(b));
	}
	return maximum;
}
int main(int argc, char **argv) {
	try {
		if (argc != 7) {
			std::cerr << "Usage: meps_brr_poc DATA_DIR NEW_DB_PATH QUERY_DIR OUT_PREFIX THREADS REPETITIONS\n"
			             "DATA_DIR contains persons.csv, events.csv, brr.csv from prepare.py.\n";
			return 2;
		}
		string data = argv[1], db_path = argv[2], queries = argv[3], prefix = argv[4];
		int threads = std::stoi(argv[5]), repetitions = std::stoi(argv[6]);
		if (threads < 1 || repetitions < 1)
			throw std::runtime_error("Threads and repetitions must be positive");
		if (std::ifstream(db_path).good() || std::ifstream(prefix + "_timings.csv").good())
			throw std::runtime_error("Use a new database and output prefix; refusing overwrite");
		auto start = Clock::now();
		DuckDB db(db_path.c_str());
		Connection con(db);
		Query(con, "SET threads=" + std::to_string(threads));
		Query(con, "SET priv_rewrite=false");
		Query(con, "SET pac_sample_diversity_check=false");
		Query(con, "SET pac_mi=0");
		for (auto table : {"persons_input", "events", "brr"}) {
			string filename = string(table) == "persons_input" ? "persons" : table;
			Query(con, "CREATE TABLE " + string(table) + " AS SELECT * FROM read_csv(" +
			               Quote(data + "/" + filename + ".csv") + ", header=true, sample_size=-1)");
		}
		double import_ms = Ms(start);
		start = Clock::now();
		Require(con, "SELECT count(*)=count(DISTINCT (person_id,panel)) FROM persons_input", "Duplicate person/panel");
		Require(con, "SELECT count(*)=count(DISTINCT person_id) FROM persons_input",
		        "Expected one survey year and unique person identifiers");
		Require(con, "SELECT count(*)=count(DISTINCT (person_id,panel)) FROM brr", "Duplicate BRR person/panel");
		Require(con, "SELECT count(*)=0 FROM persons_input p ANTI JOIN brr b USING(person_id,panel)",
		        "Missing BRR linkage");
		Require(con, "SELECT count(*)=0 FROM events e ANTI JOIN persons_input p USING(person_id)",
		        "Event without a person");
		Require(con,
		        "SELECT count(*)=0 FROM persons_input WHERE weight IS NULL OR weight<0 OR "
		        "expenditure IS NULL OR expenditure<0 OR age IS NULL OR age<0",
		        "Invalid person input");
		// AHRQ Exercise1a_OUTPUT.TXT reports 6,933 zero-expenditure observations
		// and 33,259 positive-weight observations out of 34,655 for this 2016 workload.
		Require(con,
		        "SELECT count(*)=34655 AND count(*) FILTER(WHERE expenditure=0)=6933 "
		        "AND count(*) FILTER(WHERE weight>0)=33259 FROM persons_input",
		        "Input differs from published 2016 MEPS counts; check SAS zero decoding");
		string invalid, lo, hi;
		for (int rep = 0; rep < 128; rep++) {
			string col = "BRR" + std::to_string(rep + 1);
			invalid += (rep ? " OR " : "") + col + " IS NULL OR " + col + " NOT IN (0,1)";
			auto &mask = rep < 64 ? lo : hi;
			if (!mask.empty())
				mask += " | ";
			mask += "(" + col + "::UBIGINT << " + std::to_string(rep % 64) + ")";
		}
		Require(con, "SELECT count(*)=0 FROM brr WHERE " + invalid, "BRR flags must be non-null binary values");
		Query(con, "CREATE TABLE persons AS SELECT p.*, (" + lo + ") AS mask_lo, (" + hi +
		               ") AS mask_hi FROM persons_input p JOIN brr b USING(person_id,panel)");
		Query(con, "CHECKPOINT");
		double prep_ms = Ms(start);
		std::ofstream timings(prefix + "_timings.csv"), estimates(prefix + "_estimates.csv"),
		    replicates(prefix + "_replicates.csv");
		if (!timings || !estimates || !replicates)
			throw std::runtime_error("Cannot open output files");
		timings << "query,mode,threads,run,analysis_ms,extract_ms,compute_ms,import_ms,prepare_ms,first_workflow_ms,"
		           "max_relative_error\n";
		estimates << "query,mode,metric,point_estimate,brr_standard_error\n";
		replicates << "query,metric,replicate,estimate\n";
		timings << std::setprecision(17);
		estimates << std::setprecision(17);
		replicates << std::setprecision(17);
		std::vector<string> modes = {"cpp_replicates", "repeated_sql", "conditional_sql", "asa"};
		std::vector<double> cold(modes.size(), import_ms + prep_ms);
		auto published =
		    Query(con, "SELECT * FROM read_csv(" + Quote(queries + "/expected_points.csv") + ", header=true)");
		for (auto name : {"q01_expenditures", "q02_age_groups", "q03_event_expenditures", "q04_visits_per_person"}) {
			string source = Read(queries + "/" + name + ".sql");
			// All methods reuse the same joined/derived data, as survey software does.
			// These total/ratio estimators permit exact per-person sufficient statistics.
			start = Clock::now();
			Query(con, "CREATE OR REPLACE TABLE analysis_input AS SELECT person_id, mask_lo, mask_hi, metric, "
			           "sum(numerator)::DOUBLE AS numerator, sum(denominator)::DOUBLE AS denominator, "
			           "bool_or(is_ratio) AS is_ratio FROM (" +
			               source + ") GROUP BY ALL");
			double query_prep_ms = Ms(start);
			for (auto &total : cold)
				total += query_prep_ms;
			source = "SELECT * FROM analysis_input";
			Results reference;
			for (int run = 0; run < repetitions; run++) {
				for (idx_t step = 0; step < modes.size(); step++) {
					idx_t mode_idx = run == 0 ? step : (step + run) % modes.size();
					const auto &mode = modes[mode_idx];
					start = Clock::now();
					double extract_ms;
					auto result = Analyze(con, source, mode, threads, extract_ms);
					double elapsed = Ms(start);
					for (idx_t row = 0; row < published->RowCount(); row++) {
						if (published->GetValue(0, row).ToString() != name)
							continue;
						const auto metric = published->GetValue(1, row).ToString();
						double expected = published->GetValue(2, row).GetValue<double>();
						double tolerance = published->GetValue(3, row).GetValue<double>();
						if (std::abs(result.at(metric).point - expected) > tolerance)
							throw std::runtime_error("Point estimate differs from AHRQ published output: " + metric);
					}
					if (run == 0 && mode_idx == 0)
						reference = result;
					double error = Compare(result, reference, mode == "asa" ? 1e-3 : 1e-8);
					timings << name << ',' << mode << ',' << threads << ',' << run << ',' << elapsed << ','
					        << extract_ms << ',' << elapsed - extract_ms << ',' << import_ms << ','
					        << prep_ms + query_prep_ms << ','
					        << (run == 0 ? import_ms + prep_ms + query_prep_ms + elapsed : 0) << ',' << error << '\n';
					if (run == 0) {
						cold[mode_idx] += elapsed;
						for (auto &entry : result)
							estimates << name << ',' << mode << ',' << entry.first << ',' << entry.second.point << ','
							          << StandardError(entry.second) << '\n';
						if (mode_idx == 0)
							for (auto &entry : result)
								for (int rep = 0; rep < 128; rep++)
									replicates << name << ',' << entry.first << ',' << rep + 1 << ','
									           << entry.second.replicates[rep] << '\n';
					}
					std::cout << name << ' ' << mode << " run=" << run << " ms=" << elapsed
					          << " max_relative_error=" << error << std::endl;
				}
			}
			timings.flush();
			estimates.flush();
		}
		for (idx_t i = 0; i < modes.size(); i++)
			std::cout << "Workflow total (shared import+preparation once), " << modes[i] << ": " << cold[i] << " ms\n";
		return 0;
	} catch (const std::exception &e) {
		std::cerr << "MEPS benchmark failed: " << e.what() << '\n';
		return 1;
	}
}
