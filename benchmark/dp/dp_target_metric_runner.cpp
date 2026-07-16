#define DP_BENCHMARK_RUNNER_NO_MAIN
#include "dp_benchmark_runner.cpp"

#include <set>

namespace duckdb {

struct TargetMetricOptions {
	string config_path;
	string out_path;
	string dataset_name = "tpch";
	double delta = 1e-6;
	vector<string> modes = {"dp_standard", "dp_elastic"};
	vector<string> query_names = {"q01", "q05", "q06", "q14", "q19"};
	vector<double> bound_multipliers;
	int threads = 4;
	string reference = "self";
	string saa_release = "average";
	int saa_m = 64;
	bool saa_rescale = false;
};

static vector<string> SplitCommaList(const string &text) {
	vector<string> result;
	for (auto &entry : StringUtil::Split(text, ',')) {
		StringUtil::Trim(entry);
		if (!entry.empty()) {
			result.push_back(entry);
		}
	}
	return result;
}

static vector<double> SplitDoubleList(const string &text) {
	vector<double> result;
	for (auto &entry : SplitCommaList(text)) {
		result.push_back(std::stod(entry));
	}
	return result;
}

static bool ParseBoolFlag(const string &value) {
	auto lower = StringUtil::Lower(value);
	if (lower == "true" || lower == "1" || lower == "yes") {
		return true;
	}
	if (lower == "false" || lower == "0" || lower == "no") {
		return false;
	}
	throw std::runtime_error("expected boolean value, got: " + value);
}

static bool ContainsTargetString(const vector<string> &values, const string &value) {
	return std::find(values.begin(), values.end(), value) != values.end();
}

static bool DoubleMatches(double lhs, double rhs) {
	return std::fabs(lhs - rhs) <= std::max(1e-15, std::fabs(rhs) * 1e-8);
}

static bool ContainsDouble(const vector<double> &values, double value) {
	if (values.empty()) {
		return true;
	}
	for (auto &candidate : values) {
		if (DoubleMatches(candidate, value)) {
			return true;
		}
	}
	return false;
}

static TargetMetricOptions ParseTargetMetricArgs(int argc, char **argv) {
	TargetMetricOptions options;
	for (int i = 1; i < argc; i++) {
		string arg = argv[i];
		auto read_value = [&](const string &name) -> string {
			if (i + 1 >= argc) {
				throw std::runtime_error(name + " requires a value");
			}
			return argv[++i];
		};
		if (arg == "--config") {
			options.config_path = read_value(arg);
		} else if (arg == "--out") {
			options.out_path = read_value(arg);
		} else if (arg == "--dataset") {
			options.dataset_name = read_value(arg);
		} else if (arg == "--delta") {
			options.delta = std::stod(read_value(arg));
		} else if (arg == "--modes") {
			options.modes = SplitCommaList(read_value(arg));
		} else if (arg == "--queries") {
			options.query_names = SplitCommaList(read_value(arg));
		} else if (arg == "--bound-multipliers") {
			options.bound_multipliers = SplitDoubleList(read_value(arg));
		} else if (arg == "--threads") {
			options.threads = std::stoi(read_value(arg));
		} else if (arg == "--reference") {
			options.reference = read_value(arg);
		} else if (arg == "--saa-release") {
			options.saa_release = read_value(arg);
		} else if (arg == "--saa-m") {
			options.saa_m = std::stoi(read_value(arg));
		} else if (arg == "--saa-rescale") {
			options.saa_rescale = ParseBoolFlag(read_value(arg));
		} else {
			auto eq = arg.find('=');
			if (eq == string::npos) {
				throw std::runtime_error("unknown argument: " + arg);
			}
			string key = arg.substr(0, eq);
			string value = arg.substr(eq + 1);
			if (key == "--config") {
				options.config_path = value;
			} else if (key == "--out") {
				options.out_path = value;
			} else if (key == "--dataset") {
				options.dataset_name = value;
			} else if (key == "--delta") {
				options.delta = std::stod(value);
			} else if (key == "--modes") {
				options.modes = SplitCommaList(value);
			} else if (key == "--queries") {
				options.query_names = SplitCommaList(value);
			} else if (key == "--bound-multipliers") {
				options.bound_multipliers = SplitDoubleList(value);
			} else if (key == "--threads") {
				options.threads = std::stoi(value);
			} else if (key == "--reference") {
				options.reference = value;
			} else if (key == "--saa-release") {
				options.saa_release = value;
			} else if (key == "--saa-m") {
				options.saa_m = std::stoi(value);
			} else if (key == "--saa-rescale") {
				options.saa_rescale = ParseBoolFlag(value);
			} else {
				throw std::runtime_error("unknown argument: " + arg);
			}
		}
	}
	if (options.config_path.empty()) {
		throw std::runtime_error("--config is required");
	}
	if (options.out_path.empty()) {
		throw std::runtime_error("--out is required");
	}
	if (options.reference != "self" && options.reference != "saa-estimator" &&
	    options.reference != "saa-estimator-plus-dp-noise") {
		throw std::runtime_error("--reference must be 'self', 'saa-estimator', or 'saa-estimator-plus-dp-noise'");
	}
	return options;
}

static void WriteTargetMetricHeader(std::ofstream &csv) {
	csv << "dataset,workload,query,mode,run,bound_multiplier,delta,seed,reference,reference_release,"
	       "reference_m,reference_rescale,"
	       "target_utility,target_recall,target_precision,target_median_error_pct,"
	       "target_q1_sum_median_error_pct,target_q1_avg_median_error_pct,"
	       "target_q1_count_median_error_pct\n";
}

static void WriteTargetMetricRow(std::ofstream &csv, const DatasetConfig &dataset, const QuerySpec &query,
                                 const RunPoint &point, idx_t run, double delta, uint64_t seed,
                                 const TargetMetricOptions &options, const UtilityMetrics &metrics) {
	csv << dataset.name << "," << dataset.workload << "," << query.name << "," << point.mode << "," << run << ","
	    << FormatNumber(point.bound_multiplier) << "," << FormatNumber(delta) << "," << seed << "," << options.reference
	    << "," << options.saa_release << "," << options.saa_m << "," << (options.saa_rescale ? "true" : "false") << ","
	    << metrics.utility << "," << metrics.recall << "," << metrics.precision << "," << metrics.median_error_pct
	    << "," << metrics.q1_sum_median_error_pct << "," << metrics.q1_avg_median_error_pct << ","
	    << metrics.q1_count_median_error_pct << "\n";
	csv.flush();
}

static bool DatasetContainsQuery(const DatasetConfig &dataset, const string &query_name) {
	return dataset.query_names.empty() || ContainsTargetString(dataset.query_names, query_name);
}

static string QuoteIdentifier(const string &identifier) {
	string result = "\"";
	for (char c : identifier) {
		if (c == '"') {
			result += "\"\"";
		} else {
			result += c;
		}
	}
	result += "\"";
	return result;
}

static vector<string> ReadColumnNames(Connection &con, const string &table_name) {
	auto result = con.Query("SELECT * FROM " + table_name + " LIMIT 0");
	if (!result || result->HasError()) {
		throw std::runtime_error(result ? result->GetError() : "null query result");
	}
	return result->names;
}

static void ResolvePointRuntimeSettings(Connection &con, const DatasetConfig &dataset, const QuerySpec &query,
                                        const RunPoint &point, double &delta, double &sass_count_output_bound,
                                        double &sass_sum_output_bound) {
	delta = point.delta;
	sass_count_output_bound = 0.0;
	sass_sum_output_bound = 0.0;
	if (point.mode == "duckdb" || point.mode == "dp_sass_bounded_ratio") {
		return;
	}
	double pu_count = ReadBenchmarkPrivacyUnitCount(con, dataset, query);
	if (delta <= 0.0) {
		delta = DefaultDelta(point.epsilon, pu_count);
	}
	if (point.mode == "dp_sass") {
		sass_count_output_bound =
		    point.sass_count_output_bound > 0.0
		        ? ScaleConfiguredSassOutputBound(point.sass_count_output_bound, point.bound_multiplier)
		        : DeriveSassOutputBound(point.count_bound, pu_count, point.sample_lanes, point.sass_m,
		                                point.sass_rescale, "dp_sass_count_output_bound");
		sass_sum_output_bound =
		    point.sass_sum_output_bound > 0.0
		        ? ScaleConfiguredSassOutputBound(point.sass_sum_output_bound, point.bound_multiplier)
		        : DeriveSassOutputBound(point.sum_bound, pu_count, point.sample_lanes, point.sass_m, point.sass_rescale,
		                                "dp_sass_sum_output_bound");
	}
}

static bool FindSaaReferencePoint(const Config &config, const DatasetConfig &dataset, const QuerySpec &query,
                                  const RunPoint &source_point, const TargetMetricOptions &options,
                                  DatasetConfig &reference_dataset, RunPoint &reference_point) {
	for (auto &candidate_dataset : config.datasets) {
		if (candidate_dataset.name != dataset.name || candidate_dataset.workload != dataset.workload ||
		    DefaultDbPath(candidate_dataset) != DefaultDbPath(dataset) ||
		    !DatasetContainsQuery(candidate_dataset, query.name)) {
			continue;
		}
		if (!ContainsTargetString(candidate_dataset.modes, "dp_sass")) {
			continue;
		}
		auto candidate_points = BuildRunPoints(candidate_dataset);
		for (auto &candidate_point : candidate_points) {
			if (candidate_point.mode != "dp_sass" || candidate_point.release != options.saa_release ||
			    candidate_point.sass_m != options.saa_m || candidate_point.sass_rescale != options.saa_rescale ||
			    !DoubleMatches(candidate_point.bound_multiplier, source_point.bound_multiplier) ||
			    !DoubleMatches(candidate_point.delta, source_point.delta) ||
			    !DoubleMatches(candidate_point.epsilon, source_point.epsilon)) {
				continue;
			}
			reference_dataset = candidate_dataset;
			reference_point = candidate_point;
			return true;
		}
	}
	return false;
}

static void FillSelfTargetMetric(Connection &con, const DatasetConfig &dataset, const QuerySpec &query,
                                 const RunPoint &point, uint64_t seed, UtilityMetrics &metrics) {
	string suffix = std::to_string(getpid());
	string private_table = "__dp_target_private_" + suffix;
	string target_table = "__dp_target_no_noise_" + suffix;
	SaaEstimatorMetricCleanup cleanup(con, {private_table, target_table}, seed);

	RunStatement(con, "SET privacy_diffcols=NULL");
	RunStatement(con, "SET privacy_seed=" + std::to_string(seed));
	RunStatement(con, "SET priv_rewrite=true");
	RunStatement(con, "SET privacy_noise=true");
	CreateTempResultTable(con, private_table, query.sql);

	RunStatement(con, "SET privacy_noise=false");
	CreateTempResultTable(con, target_table, query.sql);

	auto target_metrics = CompareResultTables(con, private_table, target_table, query.key_cols, "DP target");
	metrics.utility = target_metrics.utility;
	metrics.recall = target_metrics.recall;
	metrics.precision = target_metrics.precision;
	metrics.median_error_pct = target_metrics.median_error_pct;
	if (IsBuiltInTpchQ1(dataset, query)) {
		auto q1_metrics = CompareQ1AggregateFamilies(con, private_table, target_table, query.key_cols, "DP target Q1");
		metrics.q1_sum_median_error_pct = q1_metrics.sum_median_error_pct;
		metrics.q1_avg_median_error_pct = q1_metrics.avg_median_error_pct;
		metrics.q1_count_median_error_pct = q1_metrics.count_median_error_pct;
	}
}

static void FillSaaReferenceTargetMetric(Connection &con, const Config &config, const DatasetConfig &dataset,
                                         const QuerySpec &query, const RunPoint &point,
                                         const TargetMetricOptions &options, uint64_t seed, UtilityMetrics &metrics) {
	string suffix = std::to_string(getpid());
	string private_table = "__dp_target_private_" + suffix;
	string target_table = "__dp_target_saa_estimator_" + suffix;
	SaaEstimatorMetricCleanup cleanup(con, {private_table, target_table}, seed);

	RunStatement(con, "SET privacy_diffcols=NULL");
	RunStatement(con, "SET privacy_seed=" + std::to_string(seed));
	RunStatement(con, "SET priv_rewrite=true");
	RunStatement(con, "SET privacy_noise=true");
	CreateTempResultTable(con, private_table, query.sql);

	DatasetConfig reference_dataset;
	RunPoint reference_point;
	if (!FindSaaReferencePoint(config, dataset, query, point, options, reference_dataset, reference_point)) {
		throw std::runtime_error("could not find matching SAA reference point for " + dataset.name + " " + query.name +
		                         " release=" + options.saa_release + " m=" + std::to_string(options.saa_m));
	}

	ApplyDatasetPrivacy(con, reference_dataset, query);
	double reference_delta = 0.0;
	double reference_count_bound = 0.0;
	double reference_sum_bound = 0.0;
	ResolvePointRuntimeSettings(con, reference_dataset, query, reference_point, reference_delta, reference_count_bound,
	                            reference_sum_bound);
	ApplyDpSettings(con, reference_point, reference_delta, reference_count_bound, reference_sum_bound);
	RunStatement(con, "SET privacy_seed=" + std::to_string(seed));
	RunStatement(con, "SET privacy_diffcols=NULL");
	RunStatement(con, "SET privacy_noise=false");
	CreateTempResultTable(con, target_table, query.sql);

	auto target_metrics = CompareResultTables(con, private_table, target_table, query.key_cols, "SAA reference target");
	metrics.utility = target_metrics.utility;
	metrics.recall = target_metrics.recall;
	metrics.precision = target_metrics.precision;
	metrics.median_error_pct = target_metrics.median_error_pct;
	if (IsBuiltInTpchQ1(dataset, query)) {
		auto q1_metrics =
		    CompareQ1AggregateFamilies(con, private_table, target_table, query.key_cols, "SAA reference target Q1");
		metrics.q1_sum_median_error_pct = q1_metrics.sum_median_error_pct;
		metrics.q1_avg_median_error_pct = q1_metrics.avg_median_error_pct;
		metrics.q1_count_median_error_pct = q1_metrics.count_median_error_pct;
	}
}

enum class SyntheticMeasureKind { SUM, COUNT, AVG, PASSTHROUGH };

struct SyntheticMeasureSpec {
	SyntheticMeasureKind kind;
	idx_t count_measure_idx = DConstants::INVALID_INDEX;

	explicit SyntheticMeasureSpec(SyntheticMeasureKind kind_p) : kind(kind_p) {
	}

	SyntheticMeasureSpec(SyntheticMeasureKind kind_p, idx_t count_measure_idx_p)
	    : kind(kind_p), count_measure_idx(count_measure_idx_p) {
	}
};

static constexpr uint64_t DP_TARGET_MAGIC_HASH = 2983746509182734091ULL;

static vector<SyntheticMeasureSpec> InferSyntheticMeasureSpecs(const DatasetConfig &dataset, const QuerySpec &query,
                                                               idx_t measure_count) {
	if (IsBuiltInTpchQ1(dataset, query) && measure_count == 8) {
		return vector<SyntheticMeasureSpec> {
		    SyntheticMeasureSpec(SyntheticMeasureKind::SUM),    SyntheticMeasureSpec(SyntheticMeasureKind::SUM),
		    SyntheticMeasureSpec(SyntheticMeasureKind::SUM),    SyntheticMeasureSpec(SyntheticMeasureKind::SUM),
		    SyntheticMeasureSpec(SyntheticMeasureKind::AVG, 7), SyntheticMeasureSpec(SyntheticMeasureKind::AVG, 7),
		    SyntheticMeasureSpec(SyntheticMeasureKind::AVG, 7), SyntheticMeasureSpec(SyntheticMeasureKind::COUNT)};
	}
	if ((query.name == "q01_avg_onegroup" || query.name == "q01_avg_onegroup_with_count") && query.key_cols == 0 &&
	    measure_count == 2) {
		return vector<SyntheticMeasureSpec> {SyntheticMeasureSpec(SyntheticMeasureKind::AVG, 1),
		                                     SyntheticMeasureSpec(SyntheticMeasureKind::PASSTHROUGH)};
	}
	if (measure_count == 1 && (query.name == "q05" || query.name == "q06" || query.name == "q19")) {
		return vector<SyntheticMeasureSpec> {SyntheticMeasureSpec(SyntheticMeasureKind::SUM)};
	}
	throw std::runtime_error("saa-estimator-plus-dp-noise does not know how to assign DP noise scales for " +
	                         dataset.name + " " + query.name + " with " + std::to_string(measure_count) +
	                         " measure columns");
}

static double SyntheticBudgetUnits(const QuerySpec &query, idx_t measure_count) {
	if ((query.name == "q01_avg_onegroup" || query.name == "q01_avg_onegroup_with_count") && measure_count == 2) {
		return 1.0; // COUNT is an auxiliary denominator for the AVG diagnostic, not a reported DP cell.
	}
	double units = static_cast<double>(measure_count);
	if (query.key_cols > 0) {
		units += 1.0; // grouped DP-Bounded also reserves one unit for private partition selection.
	}
	return units;
}

static string BuildNoisedSyntheticExpression(const string &value_expr, double scale, uint64_t nonce) {
	if (scale <= 0.0) {
		return value_expr;
	}
	return "dp_noise(" + value_expr + ", " + FormatNumber(scale) + ", " + std::to_string(nonce) + "::UBIGINT)";
}

static vector<double> ParseScaledPositiveList(const string &bounds_text, double multiplier,
                                              const string &setting_name) {
	string scaled = ScaleConfiguredBoundList(bounds_text, multiplier, setting_name);
	vector<double> result;
	if (scaled.empty()) {
		return result;
	}
	for (auto &entry : StringUtil::Split(scaled, ',')) {
		StringUtil::Trim(entry);
		if (!entry.empty()) {
			result.push_back(std::stod(entry));
		}
	}
	return result;
}

static double GetSyntheticSumBound(const RunPoint &point, const vector<double> &sum_bounds, idx_t sum_idx) {
	if (sum_idx < sum_bounds.size()) {
		return sum_bounds[sum_idx];
	}
	return point.sum_bound;
}

static void CreateSyntheticDpNoiseTable(Connection &con, const string &private_table, const string &target_table,
                                        const DatasetConfig &dataset, const QuerySpec &query, const RunPoint &point,
                                        uint64_t seed) {
	if (point.mode != "dp_standard") {
		throw std::runtime_error("saa-estimator-plus-dp-noise is currently implemented for dp_standard only");
	}
	auto column_names = ReadColumnNames(con, target_table);
	if (query.key_cols >= column_names.size()) {
		throw std::runtime_error("saa-estimator-plus-dp-noise target has no measure columns");
	}
	idx_t measure_count = column_names.size() - query.key_cols;
	auto specs = InferSyntheticMeasureSpecs(dataset, query, measure_count);
	if (specs.size() != measure_count) {
		throw std::runtime_error("internal synthetic measure spec mismatch");
	}
	double budget_units = SyntheticBudgetUnits(query, measure_count);
	double epsilon = point.epsilon;
	if (epsilon <= 0.0 || !std::isfinite(epsilon)) {
		throw std::runtime_error("saa-estimator-plus-dp-noise requires finite positive epsilon");
	}
	auto sum_bounds = ParseScaledPositiveList(point.sum_bound_list, point.bound_multiplier, "sum_bound_lists");
	idx_t sum_idx = 0;
	idx_t avg_idx = 0;

	vector<string> projections;
	projections.reserve(column_names.size());
	for (idx_t i = 0; i < query.key_cols; i++) {
		auto quoted = QuoteIdentifier(column_names[i]);
		projections.push_back(quoted);
	}
	for (idx_t mi = 0; mi < measure_count; mi++) {
		idx_t col_idx = query.key_cols + mi;
		auto quoted = QuoteIdentifier(column_names[col_idx]);
		string value_expr = "CAST(" + quoted + " AS DOUBLE)";
		const auto &spec = specs[mi];
		string noised_expr;
		uint64_t nonce = (seed ^ (DP_TARGET_MAGIC_HASH * static_cast<uint64_t>(mi + 1)));
		switch (spec.kind) {
		case SyntheticMeasureKind::SUM: {
			double scale = GetSyntheticSumBound(point, sum_bounds, sum_idx++) * budget_units / epsilon;
			noised_expr = BuildNoisedSyntheticExpression(value_expr, scale, nonce);
			break;
		}
		case SyntheticMeasureKind::COUNT: {
			double scale = point.count_bound * budget_units / epsilon;
			noised_expr = BuildNoisedSyntheticExpression(value_expr, scale, nonce);
			break;
		}
		case SyntheticMeasureKind::AVG: {
			if (spec.count_measure_idx == DConstants::INVALID_INDEX || spec.count_measure_idx >= measure_count) {
				throw std::runtime_error("saa-estimator-plus-dp-noise AVG requires a target COUNT denominator");
			}
			double avg_lower = avg_idx < point.avg_lower_bounds.size() ? point.avg_lower_bounds[avg_idx] : 0.0;
			double avg_upper =
			    avg_idx < point.avg_upper_bounds.size() ? point.avg_upper_bounds[avg_idx] : point.sum_bound;
			avg_idx++;
			if (!(avg_lower < avg_upper)) {
				throw std::runtime_error("saa-estimator-plus-dp-noise AVG bounds must satisfy lower < upper");
			}
			double avg_midpoint = (avg_lower + avg_upper) / 2.0;
			double avg_normalized_bound = (avg_upper - avg_lower) / 2.0;
			auto count_col = QuoteIdentifier(column_names[query.key_cols + spec.count_measure_idx]);
			string count_expr = "greatest(CAST(" + count_col + " AS DOUBLE), 1.0)";
			double sum_scale = avg_normalized_bound * point.count_bound * budget_units * 2.0 / epsilon;
			double count_scale = point.count_bound * budget_units * 2.0 / epsilon;
			string centered_sum = "((" + value_expr + " - " + FormatNumber(avg_midpoint) + ") * " + count_expr + ")";
			string noised_sum = BuildNoisedSyntheticExpression(centered_sum, sum_scale, nonce ^ DP_TARGET_MAGIC_HASH);
			string noised_count =
			    "greatest(" +
			    BuildNoisedSyntheticExpression(count_expr, count_scale, nonce ^ (DP_TARGET_MAGIC_HASH << 1)) + ", 1.0)";
			noised_expr = "(" + FormatNumber(avg_midpoint) + " + (" + noised_sum + " / " + noised_count + "))";
			break;
		}
		case SyntheticMeasureKind::PASSTHROUGH:
			noised_expr = value_expr;
			break;
		}
		projections.push_back(noised_expr + " AS " + quoted);
	}

	RunStatement(con, "SET privacy_seed=" + std::to_string(seed));
	RunStatement(con, "DROP TABLE IF EXISTS " + private_table);
	RunStatement(con, "CREATE TEMP TABLE " + private_table + " AS SELECT " + StringUtil::Join(projections, ", ") +
	                      " FROM " + target_table);
}

static string QualifiedColumn(const string &alias, const string &column_name) {
	return alias + "." + QuoteIdentifier(column_name);
}

static void CreateResidualDpNoiseTable(Connection &con, const string &private_table, const string &target_table,
                                       const string &dp_private_table, const string &dp_no_noise_table,
                                       const DatasetConfig &dataset, const QuerySpec &query, const RunPoint &point,
                                       uint64_t seed) {
	ApplyDatasetPrivacy(con, dataset, query);
	double delta = 0.0;
	double sass_count_output_bound = 0.0;
	double sass_sum_output_bound = 0.0;
	ResolvePointRuntimeSettings(con, dataset, query, point, delta, sass_count_output_bound, sass_sum_output_bound);
	ApplyDpSettings(con, point, delta, sass_count_output_bound, sass_sum_output_bound);
	RunStatement(con, "SET privacy_seed=" + std::to_string(seed));
	RunStatement(con, "SET privacy_diffcols=NULL");

	RunStatement(con, "SET privacy_noise=true");
	CreateTempResultTable(con, dp_private_table, query.sql);
	RunStatement(con, "SET privacy_noise=false");
	CreateTempResultTable(con, dp_no_noise_table, query.sql);

	auto target_columns = ReadColumnNames(con, target_table);
	auto dp_private_columns = ReadColumnNames(con, dp_private_table);
	auto dp_no_noise_columns = ReadColumnNames(con, dp_no_noise_table);
	if (target_columns.size() != dp_private_columns.size() || target_columns.size() != dp_no_noise_columns.size()) {
		throw std::runtime_error("SAA target and DP residual tables have different column counts for " + query.name);
	}
	if (query.key_cols >= target_columns.size()) {
		throw std::runtime_error("SAA target residual comparison has no measure columns for " + query.name);
	}

	vector<string> projections;
	projections.reserve(target_columns.size());
	for (idx_t i = 0; i < query.key_cols; i++) {
		projections.push_back(QualifiedColumn("t", target_columns[i]) + " AS " + QuoteIdentifier(target_columns[i]));
	}
	for (idx_t i = query.key_cols; i < target_columns.size(); i++) {
		string target_expr = "CAST(" + QualifiedColumn("t", target_columns[i]) + " AS DOUBLE)";
		string private_expr = "CAST(" + QualifiedColumn("p", dp_private_columns[i]) + " AS DOUBLE)";
		string no_noise_expr = "CAST(" + QualifiedColumn("n", dp_no_noise_columns[i]) + " AS DOUBLE)";
		projections.push_back("(" + target_expr + " + (" + private_expr + " - " + no_noise_expr + ")) AS " +
		                      QuoteIdentifier(target_columns[i]));
	}

	string from_clause = target_table + " t";
	if (query.key_cols == 0) {
		from_clause += ", " + dp_private_table + " p, " + dp_no_noise_table + " n";
	} else {
		vector<string> private_conditions;
		vector<string> no_noise_conditions;
		for (idx_t i = 0; i < query.key_cols; i++) {
			private_conditions.push_back(QualifiedColumn("t", target_columns[i]) + " IS NOT DISTINCT FROM " +
			                             QualifiedColumn("p", dp_private_columns[i]));
			no_noise_conditions.push_back(QualifiedColumn("t", target_columns[i]) + " IS NOT DISTINCT FROM " +
			                              QualifiedColumn("n", dp_no_noise_columns[i]));
		}
		from_clause += " JOIN " + dp_private_table + " p ON " + StringUtil::Join(private_conditions, " AND ");
		from_clause += " JOIN " + dp_no_noise_table + " n ON " + StringUtil::Join(no_noise_conditions, " AND ");
	}

	RunStatement(con, "SET privacy_seed=" + std::to_string(seed));
	RunStatement(con, "DROP TABLE IF EXISTS " + private_table);
	RunStatement(con, "CREATE TEMP TABLE " + private_table + " AS SELECT " + StringUtil::Join(projections, ", ") +
	                      " FROM " + from_clause);
}

static string CompareMeasureError(Connection &con, const string &released_table, const string &reference_table,
                                  idx_t key_cols, idx_t measure_idx, const string &label) {
	auto released = ReadResultSnapshot(con, released_table);
	auto reference = ReadResultSnapshot(con, reference_table);
	if (released.column_count != reference.column_count) {
		throw std::runtime_error(label + " measure comparison column-count mismatch");
	}
	idx_t col = key_cols + measure_idx;
	if (col >= reference.column_count) {
		throw std::runtime_error(label + " measure comparison column out of range");
	}
	auto released_by_key = RowsByKey(released, key_cols, label + " released");
	auto reference_by_key = RowsByKey(reference, key_cols, label + " reference");
	vector<double> errors;
	for (auto &entry : reference_by_key) {
		auto found = released_by_key.find(entry.first);
		if (found == released_by_key.end()) {
			continue;
		}
		double released_value;
		double reference_value;
		if (!TryCastDouble(found->second[col], released_value) || !TryCastDouble(entry.second[col], reference_value)) {
			continue;
		}
		double abs_ref = std::max(0.00001, std::fabs(reference_value));
		errors.push_back(100.0 * std::fabs(released_value - reference_value) / abs_ref);
	}
	return MedianErrorString(std::move(errors));
}

static void FillSaaReferencePlusDpNoiseMetric(Connection &con, const Config &config, const DatasetConfig &dataset,
                                              const QuerySpec &query, const RunPoint &point,
                                              const TargetMetricOptions &options, uint64_t seed,
                                              UtilityMetrics &metrics) {
	string suffix = std::to_string(getpid());
	string private_table = "__dp_target_saa_plus_dp_noise_" + suffix;
	string target_table = "__dp_target_saa_estimator_" + suffix;
	string dp_private_table = "__dp_target_dp_private_" + suffix;
	string dp_no_noise_table = "__dp_target_dp_no_noise_" + suffix;
	SaaEstimatorMetricCleanup cleanup(con, {private_table, target_table, dp_private_table, dp_no_noise_table}, seed);

	DatasetConfig reference_dataset;
	RunPoint reference_point;
	if (!FindSaaReferencePoint(config, dataset, query, point, options, reference_dataset, reference_point)) {
		throw std::runtime_error("could not find matching SAA reference point for " + dataset.name + " " + query.name +
		                         " release=" + options.saa_release + " m=" + std::to_string(options.saa_m));
	}

	ApplyDatasetPrivacy(con, reference_dataset, query);
	double reference_delta = 0.0;
	double reference_count_bound = 0.0;
	double reference_sum_bound = 0.0;
	ResolvePointRuntimeSettings(con, reference_dataset, query, reference_point, reference_delta, reference_count_bound,
	                            reference_sum_bound);
	ApplyDpSettings(con, reference_point, reference_delta, reference_count_bound, reference_sum_bound);
	RunStatement(con, "SET privacy_seed=" + std::to_string(seed));
	RunStatement(con, "SET privacy_diffcols=NULL");
	RunStatement(con, "SET privacy_noise=false");
	CreateTempResultTable(con, target_table, query.sql);

	if (point.mode == "dp_standard" && query.name != "q14") {
		CreateSyntheticDpNoiseTable(con, private_table, target_table, dataset, query, point, seed);
	} else if (point.mode == "dp_standard" || point.mode == "dp_elastic") {
		CreateResidualDpNoiseTable(con, private_table, target_table, dp_private_table, dp_no_noise_table, dataset,
		                           query, point, seed);
	} else {
		throw std::runtime_error("saa-estimator-plus-dp-noise does not support mode " + point.mode);
	}

	auto target_metrics =
	    CompareResultTables(con, private_table, target_table, query.key_cols, "SAA estimator + DP noise target");
	metrics.utility = target_metrics.utility;
	metrics.recall = target_metrics.recall;
	metrics.precision = target_metrics.precision;
	metrics.median_error_pct = target_metrics.median_error_pct;
	if (IsBuiltInTpchQ1(dataset, query)) {
		auto q1_metrics = CompareQ1AggregateFamilies(con, private_table, target_table, query.key_cols,
		                                             "SAA estimator + DP noise target Q1");
		metrics.q1_sum_median_error_pct = q1_metrics.sum_median_error_pct;
		metrics.q1_avg_median_error_pct = q1_metrics.avg_median_error_pct;
		metrics.q1_count_median_error_pct = q1_metrics.count_median_error_pct;
	} else if (query.name == "q01_avg_onegroup" || query.name == "q01_avg_onegroup_with_count") {
		metrics.q1_avg_median_error_pct =
		    CompareMeasureError(con, private_table, target_table, query.key_cols, 0, "q01 one-group AVG");
		metrics.q1_count_median_error_pct =
		    CompareMeasureError(con, private_table, target_table, query.key_cols, 1, "q01 one-group COUNT");
	}
}

static void FillTargetMetric(Connection &con, const Config &config, const DatasetConfig &dataset,
                             const QuerySpec &query, const RunPoint &point, const TargetMetricOptions &options,
                             uint64_t seed, UtilityMetrics &metrics) {
	if (options.reference == "self") {
		FillSelfTargetMetric(con, dataset, query, point, seed, metrics);
	} else if (options.reference == "saa-estimator") {
		FillSaaReferenceTargetMetric(con, config, dataset, query, point, options, seed, metrics);
	} else {
		FillSaaReferencePlusDpNoiseMetric(con, config, dataset, query, point, options, seed, metrics);
	}
}

static void RunTargetMetricDataset(const Config &config, const DatasetConfig &dataset,
                                   const TargetMetricOptions &options, std::ofstream &csv) {
	if (dataset.name != options.dataset_name) {
		return;
	}
	ValidateDataset(dataset);
	auto queries = FilterQueriesByName(LoadDatasetQueries(dataset), dataset.query_names);
	if (queries.empty()) {
		return;
	}
	string db_path = DefaultDbPath(dataset);
	DuckDB db(db_path);
	Connection con(db);
	RunStatement(con, "LOAD privacy");
	RunStatement(con, "SET threads=" + std::to_string(options.threads));
	RunStatement(con, "SET privacy_noise=true");
	PrepareDataset(con, dataset, false);

	auto points = BuildRunPoints(dataset);
	for (idx_t run = 0; run < config.runs; run++) {
		for (idx_t qi = 0; qi < queries.size(); qi++) {
			const auto &query = queries[qi];
			if (!ContainsTargetString(options.query_names, query.name)) {
				continue;
			}
			for (auto &point : points) {
				if (!ContainsTargetString(options.modes, point.mode) || !DoubleMatches(point.delta, options.delta) ||
				    !ContainsDouble(options.bound_multipliers, point.bound_multiplier)) {
					continue;
				}
				ApplyDatasetPrivacy(con, dataset, query);
				double delta = 0.0;
				double sass_count_output_bound = 0.0;
				double sass_sum_output_bound = 0.0;
				ResolvePointRuntimeSettings(con, dataset, query, point, delta, sass_count_output_bound,
				                            sass_sum_output_bound);
				ApplyDpSettings(con, point, delta, sass_count_output_bound, sass_sum_output_bound);
				uint64_t seed = config.seed_base + 9973ULL * static_cast<uint64_t>(run) +
				                131ULL * static_cast<uint64_t>(qi) + static_cast<uint64_t>(points.size());
				UtilityMetrics metrics;
				FillTargetMetric(con, config, dataset, query, point, options, seed, metrics);
				WriteTargetMetricRow(csv, dataset, query, point, run, delta, seed, options, metrics);
				Log("target " + dataset.name + " " + point.mode + " " + query.name + " reference=" + options.reference +
				    " bound=" + FormatNumber(point.bound_multiplier) + " median=" + metrics.median_error_pct);
			}
		}
	}
}

static int TargetMetricMain(int argc, char **argv) {
	auto options = ParseTargetMetricArgs(argc, argv);
	auto config = LoadConfig(options.config_path);
	std::ofstream csv(options.out_path, std::ofstream::out | std::ofstream::trunc);
	if (!csv.is_open()) {
		throw std::runtime_error("could not open output CSV: " + options.out_path);
	}
	WriteTargetMetricHeader(csv);
	for (auto &dataset : config.datasets) {
		RunTargetMetricDataset(config, dataset, options, csv);
	}
	Log("Wrote " + options.out_path);
	return 0;
}

} // namespace duckdb

int main(int argc, char **argv) {
	try {
		return duckdb::TargetMetricMain(argc, argv);
	} catch (std::exception &ex) {
		std::cerr << "error: " << ex.what() << std::endl;
		return 1;
	}
}
