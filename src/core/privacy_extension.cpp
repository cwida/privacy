#define DUCKDB_EXTENSION_MAIN

#include "privacy_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

#include <cmath>
#include <fstream>
#include <unordered_set>

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/types.hpp"
#include "core/privacy_optimizer.hpp"
#include "aggregates/as_aggregate.hpp"
#include "aggregates/as_count.hpp"
#include "aggregates/as_sum.hpp"
#include "aggregates/as_clip_sum.hpp"
#include "aggregates/as_min_max.hpp"
#include "aggregates/as_clip_min_max.hpp"
#include "aggregates/dp_laplace_noise.hpp"
#include "categorical/pac_categorical.hpp"
#include "parser/privacy_parser.hpp"
#include "diff/pac_utility_diff.hpp"
#include "query_processing/pac_topk_rewriter.hpp"
#include "query_processing/pac_avg_rewriter.hpp"
#include "privacy_debug.hpp"

namespace duckdb {

// Pragma function to save PAC metadata to file
static void SavePrivacyMetadataPragma(ClientContext &context, const FunctionParameters &parameters) {
	auto filepath = parameters.values[0].ToString();
	PrivacyMetadataManager::Get().SaveToFile(filepath);
}

// Pragma function to load PAC metadata from file
static void LoadPrivacyMetadataPragma(ClientContext &context, const FunctionParameters &parameters) {
	auto filepath = parameters.values[0].ToString();
	PrivacyMetadataManager::Get().LoadFromFile(filepath);
}

// Pragma function to clear all PAC metadata (in-memory and file)
static void ClearPrivacyMetadataPragma(ClientContext &context, const FunctionParameters &parameters) {
	// Clear in-memory metadata
	PrivacyMetadataManager::Get().Clear();

	// Try to delete the metadata file if it exists
	try {
		string filepath = PrivacyMetadataManager::GetMetadataFilePath(context);
		if (!filepath.empty()) {
			// Check if file exists
			std::ifstream file_check(filepath);
			if (file_check.good()) {
				file_check.close();
				// File exists, try to delete it
				if (std::remove(filepath.c_str()) != 0) {
					throw IOException("Failed to delete PAC metadata file: " + filepath);
				}
			}
			// If file doesn't exist, do nothing (no error)
		}
	} catch (const IOException &e) {
		// Re-throw IO exceptions (file deletion failures)
		throw;
	} catch (...) {
		// Ignore other exceptions (e.g., can't determine file path for in-memory DB)
		// This is fine - just clear in-memory metadata
	}
}

static string QuoteIdentifier(const string &identifier) {
	string result = "\"";
	for (auto c : identifier) {
		if (c == '"') {
			result += "\"\"";
		} else {
			result += c;
		}
	}
	result += "\"";
	return result;
}

static void SetExtensionSetting(ClientContext &context, const string &name, const Value &value) {
	auto &config = DBConfig::GetConfig(context);
	ExtensionOption extension_option;
	if (!config.TryGetExtensionOption(name, extension_option)) {
		throw InternalException("refresh_dp_stats: extension setting '" + name + "' was not registered");
	}
	auto target_value = value.CastAs(context, extension_option.type);
	if (extension_option.set_function) {
		extension_option.set_function(context, SetScope::SESSION, target_value);
	}
	auto &client_config = ClientConfig::GetConfig(context);
	client_config.user_settings.SetUserSetting(extension_option.setting_index.GetIndex(), std::move(target_value));
}

static void ValidateDpSampleLanesSetting(ClientContext &, SetScope, Value &parameter) {
	ValidateDpSampleLanes(parameter.GetValue<int64_t>());
}

static double ComputePrivacyUnitCardinality(ClientContext &context) {
	auto table_names = PrivacyMetadataManager::Get().GetAllTableNames();
	vector<std::pair<string, PrivacyTableMetadata>> pu_tables;
	for (auto &table_name : table_names) {
		auto *metadata = PrivacyMetadataManager::Get().GetTableMetadata(table_name);
		if (metadata && metadata->is_privacy_unit) {
			pu_tables.emplace_back(table_name, *metadata);
		}
	}
	if (pu_tables.empty()) {
		throw InvalidInputException("refresh_dp_stats: no privacy unit table is declared");
	}
	if (pu_tables.size() > 1) {
		throw InvalidInputException("refresh_dp_stats: multiple privacy unit tables are declared; "
		                            "automatic DP stats currently require exactly one privacy unit table");
	}

	const auto &table_name = pu_tables[0].first;
	const auto &metadata = pu_tables[0].second;
	if (metadata.primary_key_columns.empty()) {
		throw InvalidInputException("refresh_dp_stats: privacy unit table '" + table_name +
		                            "' has no PRIVACY_KEY columns");
	}

	string key_list;
	for (idx_t i = 0; i < metadata.primary_key_columns.size(); i++) {
		if (i > 0) {
			key_list += ", ";
		}
		key_list += QuoteIdentifier(metadata.primary_key_columns[i]);
	}

	auto &db = DatabaseInstance::GetDatabase(context);
	Connection conn(db);
	auto disable_rewrite = conn.Query("SET priv_rewrite=false");
	if (!disable_rewrite || disable_rewrite->HasError()) {
		throw InvalidInputException("refresh_dp_stats: failed to disable privacy rewrite while computing stats" +
		                            (disable_rewrite ? (": " + disable_rewrite->GetError()) : ""));
	}

	string query = "SELECT COUNT(*) FROM (SELECT " + key_list + " FROM " + QuoteIdentifier(table_name) + " GROUP BY " +
	               key_list + ")";
	auto result = conn.Query(query);
	if (!result || result->HasError()) {
		throw InvalidInputException("refresh_dp_stats: failed to compute privacy unit cardinality for '" + table_name +
		                            "'" + (result ? (": " + result->GetError()) : ""));
	}
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() == 0) {
		return 0.0;
	}
	auto value = chunk->GetValue(0, 0);
	return value.IsNull() ? 0.0 : value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
}

static void RefreshDPStatsPragma(ClientContext &context, const FunctionParameters &parameters) {
	double epsilon = parameters.values[0].GetValue<double>();
	if (epsilon <= 0.0 || !std::isfinite(epsilon)) {
		throw InvalidInputException("refresh_dp_stats: epsilon must be a positive finite number");
	}

	double n = ComputePrivacyUnitCardinality(context);
	if (n <= 1.0 || !std::isfinite(n)) {
		throw InvalidInputException("refresh_dp_stats: privacy unit cardinality must be greater than 1");
	}

	double log_n = std::log(n);
	double delta = std::exp(-epsilon * log_n * log_n);
	if (delta <= 0.0 || !std::isfinite(delta) || delta >= 0.5) {
		throw InvalidInputException("refresh_dp_stats: computed dp_delta is outside (0, 0.5)");
	}

	SetExtensionSetting(context, "dp_epsilon", Value::DOUBLE(epsilon));
	SetExtensionSetting(context, "dp_delta", Value::DOUBLE(delta));
}

static void LoadInternal(ExtensionLoader &loader) {
	// Try to automatically load PAC metadata from database directory if it exists
	auto &db = loader.GetDatabaseInstance();
	try {
		// Clear any existing metadata first (in case extension is reloaded)
		PrivacyMetadataManager::Get().Clear();

		// Get all attached database paths and try to load metadata from the first one's directory
		auto paths = db.GetDatabaseManager().GetAttachedDatabasePaths();
		if (!paths.empty()) {
			const string &db_path = paths[0];

			// Extract database name from path (filename without extension)
			// Or use "memory" as default for in-memory databases
			string db_name = "memory";
			size_t last_slash = db_path.find_last_of("/\\");
			if (last_slash != string::npos && last_slash + 1 < db_path.length()) {
				string filename = db_path.substr(last_slash + 1);
				size_t dot_pos = filename.find_last_of('.');
				if (dot_pos != string::npos) {
					db_name = filename.substr(0, dot_pos);
				} else {
					db_name = filename;
				}
			} else if (last_slash == string::npos && !db_path.empty()) {
				// No slash, so path is just the filename
				size_t dot_pos = db_path.find_last_of('.');
				if (dot_pos != string::npos) {
					db_name = db_path.substr(0, dot_pos);
				} else {
					db_name = db_path;
				}
			}

			// Default schema is "main"
			string schema_name = DEFAULT_SCHEMA;

			// Build metadata path with db and schema names
			string metadata_path;
			string filename = "privacy_metadata_" + db_name + "_" + schema_name + ".json";

			if (last_slash != string::npos) {
				metadata_path = db_path.substr(0, last_slash + 1) + filename;
			} else {
				metadata_path = filename;
			}

#if PRIVACY_DEBUG
			std::cerr << "[PAC DEBUG] LoadInternal: Checking for metadata at: " << metadata_path << "\n";
#endif

			// Try to load the metadata file if it exists
			std::ifstream test_file(metadata_path);
			if (test_file.good()) {
				test_file.close();
				PrivacyMetadataManager::Get().LoadFromFile(metadata_path);
#if PRIVACY_DEBUG
				std::cerr << "[PAC DEBUG] LoadInternal: Successfully loaded metadata from " << metadata_path << "\n";
				std::cerr << "[PAC DEBUG] LoadInternal: Loaded "
				          << PrivacyMetadataManager::Get().GetAllTableNames().size() << " tables"
				          << "\n";
#endif
			} else {
#if PRIVACY_DEBUG
				std::cerr << "[PAC DEBUG] LoadInternal: Metadata file not found at " << metadata_path << "\n";
#endif
			}
		} else {
#if PRIVACY_DEBUG
			std::cerr << "[PAC DEBUG] LoadInternal: No database paths available (in-memory DB?)"
			          << "\n";
#endif
		}
	} catch (const std::exception &e) {
#if PRIVACY_DEBUG
		std::cerr << "[PAC DEBUG] LoadInternal: Failed to load metadata: " << e.what() << "\n";
#endif
	} catch (...) {
#if PRIVACY_DEBUG
		std::cerr << "[PAC DEBUG] LoadInternal: Failed to load metadata (unknown exception)"
		          << "\n";
#endif
	}

	// Register PAC optimizer rule
	auto priv_rewrite_rule = PACRewriteRule();
	// attach PAC-specific optimizer info so the extension can coordinate replan state
	auto pac_info = make_shared_ptr<PACOptimizerInfo>();
	priv_rewrite_rule.optimizer_info = pac_info;
	OptimizerExtension::Register(db.config, std::move(priv_rewrite_rule));

	// Register PAC DROP TABLE cleanup rule (separate rule to handle DROP TABLE operations)
	auto pac_drop_table_rule = PACDropTableRule();
	OptimizerExtension::Register(db.config, std::move(pac_drop_table_rule));

	// Register PAC Top-K pushdown rule (post-optimizer: rewrites TopN over PAC aggregates)
	auto pac_topk_rule = PACTopKRule();
	pac_topk_rule.optimizer_info = pac_info;
	OptimizerExtension::Register(db.config, std::move(pac_topk_rule));

	// Register as_avg rewrite rule (post-optimizer: decomposes as_noised_avg/as_avg into sum/count + division).
	// This post-optimizer handles user-written as_avg() in SQL. Compiler-generated as_noised_avg is already
	// decomposed during the pre-optimizer phase (in CompilePrivQuery) so this is a no-op for those.
	{
		OptimizerExtension pac_avg_rule;
		pac_avg_rule.optimize_function = RewritePacAvgToDiv;
		OptimizerExtension::Register(db.config, std::move(pac_avg_rule));
	}

	// Register derived_pu read rule (post-optimizer: inject priv_finalize for SELECT on derived_pu tables)
	{
		auto pac_derived_read_rule = PACDerivedReadRule();
		OptimizerExtension::Register(db.config, std::move(pac_derived_read_rule));
	}

	// Enable lenient implicit casting so the binder can resolve comparisons (>, <, etc.)
	// on derived_pu counter columns (LIST<FLOAT>) at bind time. Without this, the new
	// casting rules reject FLOAT[] vs scalar comparisons before our optimizer can rewrite
	// them to priv_filter_<cmp> calls.
	db.config.SetOptionByName("old_implicit_casting", Value::BOOLEAN(true));

	// ---- User-facing settings ----
	// Controls probabilistic vs deterministic mode for noise/NULL decisions
	db.config.AddExtensionOption(
	    "pac_mi",
	    "Mutual information bound controlling privacy-utility tradeoff (default: 1/128). "
	    "Lower values = more noise = more privacy. Set to 0 for deterministic (no noise) mode.",
	    LogicalType::DOUBLE, Value::DOUBLE(1.0 / 128));
	// Privacy mechanism selector — the single setting that picks the mechanism. The four modes
	// are 'pac' (default), 'dp_standard', 'dp_elastic', and 'dp_sass'. The dp_* settings below
	// tune the chosen mechanism; they do not select it.
	db.config.AddExtensionOption("privacy_mode",
	                             "Privacy mechanism: 'pac' (default), 'dp_standard', 'dp_elastic', or 'dp_sass'",
	                             LogicalType::VARCHAR, Value("pac"));
	db.config.AddExtensionOption(
	    "dp_sample_lanes", "Number of sample lanes a privacy unit contributes in privacy_mode='dp_sass'",
	    LogicalType::INTEGER, Value::INTEGER(DP_SAMPLE_DEFAULT_LANES), ValidateDpSampleLanesSetting);
	// Differential privacy budget (ε), used by the dp_standard / dp_elastic / dp_sass modes.
	db.config.AddExtensionOption("dp_epsilon",
	                             "Differential privacy budget ε (used by dp_standard, dp_elastic, and dp_sass)",
	                             LogicalType::DOUBLE, Value::DOUBLE(1.0));
	// Clipping bound for SUM/AVG, required when such an aggregate is present in any DP mode.
	// dp_standard bounds each PU's total contribution; dp_elastic/dp_sass clip per tuple.
	db.config.AddExtensionOption("dp_sum_bound",
	                             "Clipping bound for SUM/AVG in DP modes, required when such an aggregate is present "
	                             "(per-PU total for dp_standard; per-tuple for dp_elastic/dp_sass)",
	                             LogicalType::DOUBLE, Value(LogicalType::DOUBLE));
	db.config.AddExtensionOption("dp_count_bound",
	                             "Per-PU row-count bound: max rows one privacy unit contributes to a COUNT over a join "
	                             "(required for dp_standard COUNT-over-join; also bounds dp_sass COUNT/AVG components)",
	                             LogicalType::DOUBLE, Value(LogicalType::DOUBLE));
	// Privacy failure probability δ for (ε,δ)-DP smooth sensitivity. Required by dp_elastic and
	// dp_sass; ignored by dp_standard, which gives pure ε-DP. PRAGMA refresh_dp_stats(epsilon) can set it.
	db.config.AddExtensionOption(
	    "dp_delta",
	    "Privacy failure probability δ for (ε,δ)-DP smooth sensitivity (dp_elastic and dp_sass; "
	    "ignored by dp_standard). Use SET dp_delta=<value> or PRAGMA refresh_dp_stats(<epsilon>).",
	    LogicalType::DOUBLE, Value(LogicalType::DOUBLE));
	// Set deterministic RNG seed for PAC functions (useful for tests)
	db.config.AddExtensionOption("privacy_seed", "RNG seed for reproducible noised results", LogicalType::BIGINT);
	// Enable/disable PAC noise application (useful for testing, since noise affects result determinism)
	db.config.AddExtensionOption("privacy_noise", "Enable/disable PAC noise application (set to false for debugging)",
	                             LogicalType::BOOLEAN);
	// Correction factor: multiplies sum/avg/count results; reduces NULL probability for all aggregates
	db.config.AddExtensionOption("pac_correction", "Correction factor multiplied into aggregate results (default: 1.0)",
	                             LogicalType::DOUBLE, Value::DOUBLE(1.0));
	// Utility diff mode: number of key columns for matching + optional output path
	db.config.AddExtensionOption(
	    "privacy_diffcols",
	    "Measure utility: specify number of key columns and optional output path (e.g. '2:out.csv')",
	    LogicalType::VARCHAR);
	// Minimum support threshold. In dp_elastic mode this suppresses groups before value noise
	// unless they have at least this many contributing privacy units.
	// PAC aggregate functions still interpret this as their low-SNR utility NULLing threshold.
	db.config.AddExtensionOption("privacy_min_group_count",
	                             "Minimum privacy-unit support threshold for dp_elastic groups. "
	                             "PAC uses this setting as its low-SNR utility NULLing threshold. NULL disables.",
	                             LogicalType::DOUBLE, Value(LogicalType::DOUBLE));

	// ---- Internal settings ----
	// Enforce protected column access restrictions (prevents direct projection of protected columns)
	db.config.AddExtensionOption("priv_check", "[INTERNAL] Enforce protected column access restrictions",
	                             LogicalType::BOOLEAN, Value::BOOLEAN(true));
	// Enable privacy query plan rewriting (the core optimizer that injects noise)
	db.config.AddExtensionOption("priv_rewrite", "[INTERNAL] Enable privacy query plan rewriting", LogicalType::BOOLEAN,
	                             Value::BOOLEAN(true));
	// Number of per-sample subsets (m) used by PAC (default 128)
	db.config.AddExtensionOption("pac_m", "[INTERNAL] Number of per-sample subsets", LogicalType::INTEGER);
	// Toggle enforcement of per-sample array length == pac_m
	db.config.AddExtensionOption("enforce_m_values", "[INTERNAL] Enforce per-sample array length equals pac_m",
	                             LogicalType::BOOLEAN);
	// Path where compiled privacy artifacts (CTEs) are written
	db.config.AddExtensionOption("priv_compiled_path", "[INTERNAL] Path to write compiled privacy artifacts",
	                             LogicalType::VARCHAR);
	// Enable/disable join elimination (stop FK chain before reaching PU)
	db.config.AddExtensionOption("priv_join_elimination", "[INTERNAL] Eliminate final join to PU table",
	                             LogicalType::BOOLEAN, Value::BOOLEAN(true));
	// When false, unsupported operators skip privacy compilation instead of throwing
	db.config.AddExtensionOption(
	    "pac_conservative_mode",
	    "[INTERNAL] Throw errors for unsupported operators (when false, skip privacy compilation)",
	    LogicalType::BOOLEAN, Value::BOOLEAN(true));
	// Enable categorical query rewrites (comparisons against PAC aggregates)
	db.config.AddExtensionOption("pac_categorical", "[INTERNAL] Enable categorical query rewrites",
	                             LogicalType::BOOLEAN, Value::BOOLEAN(true));
	// Use priv_select for categorical filters below pac aggregates
	db.config.AddExtensionOption("priv_select",
	                             "[INTERNAL] Use priv_select for categorical filters below pac aggregates",
	                             LogicalType::BOOLEAN, Value::BOOLEAN(true));
	// Top-k pushdown: when true, top-k is applied on true aggregates before noising
	db.config.AddExtensionOption("pac_pushdown_topk", "[INTERNAL] Apply top-k before noise instead of after",
	                             LogicalType::BOOLEAN, Value::BOOLEAN(true));
	// Expansion factor for top-k superset approach: inner TopN selects ceil(c*K) candidates,
	// final TopN limits to K. With c=1 (default), no expansion. With c>1, more candidates
	// are considered before noising and re-ranking, improving utility.
	db.config.AddExtensionOption("pac_topk_expansion",
	                             "[INTERNAL] Expansion factor for top-k superset (1 = no expansion)",
	                             LogicalType::DOUBLE, Value::DOUBLE(1.0));
	// Control whether priv_hash() repairs hashes to exactly 32 bits set
	db.config.AddExtensionOption("pac_hash_repair", "[INTERNAL] priv_hash() repairs hash to exactly 32 bits set",
	                             LogicalType::BOOLEAN, Value::BOOLEAN(false));
	// Propagate PAC metadata through CREATE TABLE AS SELECT
	db.config.AddExtensionOption("pac_ctas", "[INTERNAL] Propagate PAC metadata through CTAS", LogicalType::BOOLEAN,
	                             Value::BOOLEAN(true));
	// Enable/disable persistent secret p-tracking for correct query-level privacy composition.
	// When enabled (default), noise calibration tracks a Bayesian posterior over worlds across all cells
	// in a query, providing query-level MIA protection. When disabled, each cell uses uniform variance
	// independently (cell-level MIA only). Only active when pac_mi > 0.
	db.config.AddExtensionOption("pac_ptracking", "[INTERNAL] Enable persistent secret p-tracking for query-level MIA",
	                             LogicalType::BOOLEAN, Value::BOOLEAN(true));

	db.config.AddExtensionOption("priv_clip_support",
	                             "Dynamic outlier clipping threshold for as_clip_sum. "
	                             "Levels with fewer than this many estimated distinct contributors are zeroed out. "
	                             "NULL (default) disables as_clip_sum; set to e.g. 64 to enable.",
	                             LogicalType::BIGINT, Value());

	db.config.AddExtensionOption("priv_clip_scale",
	                             "Scale unsupported outlier levels to nearest supported level instead of omitting. "
	                             "Default false (omit).",
	                             LogicalType::BOOLEAN, Value::BOOLEAN(false));

	// Register as_sum aggregate functions
	RegisterPacSumFunctions(loader);
	RegisterPacSumCountersFunctions(loader);
	RegisterDpSampleSumFunctions(loader);
	RegisterPacClipSumFunctions(loader);
	RegisterPacNoisedClipSumFunctions(loader);
	RegisterPacNoisedClipSumCountFunctions(loader);
	RegisterDpSampleClipSumFunctions(loader);
	RegisterPacCountFunctions(loader);
	RegisterPacCountCountersFunctions(loader);
	RegisterDpSampleCountFunctions(loader);
	RegisterPacClipCountFunctions(loader);
	RegisterPacNoisedClipCountFunctions(loader);
	// Register as_min/as_max aggregate functions
	RegisterPacMinFunctions(loader);
	RegisterPacMaxFunctions(loader);
	// Register _counters variants for categorical queries
	RegisterPacMinCountersFunctions(loader);
	RegisterPacMaxCountersFunctions(loader);
	RegisterDpSampleMinFunctions(loader);
	RegisterDpSampleMaxFunctions(loader);
	// Register clip synonyms for min/max
	RegisterPacClipMinFunctions(loader);
	RegisterPacClipMaxFunctions(loader);
	RegisterPacNoisedClipMinFunctions(loader);
	RegisterPacNoisedClipMaxFunctions(loader);

	// Register dummy as_noised_avg / as_avg (replaced by RewritePacAvgToDiv before execution)
	RegisterPacAvgFunctions(loader);

	// Register PAC categorical functions (priv_select, priv_filter, priv_filter_<cmp>, etc.)
	RegisterPacCategoricalFunctions(loader);

	// Register priv_mean scalar function (used by top-k pushdown for ordering)
	RegisterPacMeanFunction(loader);

	// Register priv_finalize scalar function (LIST<DOUBLE> -> DOUBLE, read-time noise for derived tables)
	RegisterPacFinalizeFunction(loader);

	// Register priv_hash scalar function (UBIGINT -> UBIGINT with exactly 32 bits set)
	RegisterPacHashFunction(loader);

	// Register pac_aggregate scalar function (naive PAC sample-and-aggregate terminal)
	RegisterPacAggregateFunctions(loader);

	// Register dp_noise scalar function (value, scale) -> value + Lap(scale)
	RegisterDpLaplaceNoiseFunction(loader);
	RegisterDpSmoothMedianNoiseFunction(loader);

	// Register PAC parser extension
	ParserExtension::Register(db.config, PrivacyParserExtension());

	// Register PAC metadata management pragmas
	auto save_privacy_metadata_pragma =
	    PragmaFunction::PragmaCall("save_privacy_metadata", SavePrivacyMetadataPragma, {LogicalType::VARCHAR});
	loader.RegisterFunction(save_privacy_metadata_pragma);

	auto load_privacy_metadata_pragma =
	    PragmaFunction::PragmaCall("load_privacy_metadata", LoadPrivacyMetadataPragma, {LogicalType::VARCHAR});
	loader.RegisterFunction(load_privacy_metadata_pragma);

	auto clear_privacy_metadata_pragma =
	    PragmaFunction::PragmaCall("clear_privacy_metadata", ClearPrivacyMetadataPragma, {});
	loader.RegisterFunction(clear_privacy_metadata_pragma);

	auto refresh_dp_stats_pragma =
	    PragmaFunction::PragmaCall("refresh_dp_stats", RefreshDPStatsPragma, {LogicalType::DOUBLE});
	loader.RegisterFunction(refresh_dp_stats_pragma);
}

void PrivacyExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
string PrivacyExtension::Name() {
	return "privacy";
}

string PrivacyExtension::Version() const {
#ifdef EXT_VERSION_PRIVACY
	return EXT_VERSION_PRIVACY;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(privacy, loader) {
	duckdb::LoadInternal(loader);
}
}
