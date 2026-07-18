#include "compiler/privacy_mechanisms.hpp"
#include "aggregates/as_aggregate.hpp"
#include "utils/privacy_helpers.hpp"
#include "compiler/privacy_compiler.hpp"
#include "compiler/privacy_compiler_helpers.hpp"
#include "query_processing/privacy_plan_traversal.hpp"
#include "query_processing/privacy_expression_builder.hpp"
#include "privacy_debug.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace duckdb {

// ----------------------------------------------------------------------------
// FK chain structure — ordered from most-distant non-PU table to PU
// ----------------------------------------------------------------------------

struct DPFKChain {
	// tables[0..n-2] are non-PU; tables[n-1] is the PU
	// For single-table PU queries (no joins): tables = [pu_name], fk_cols empty
	vector<string> tables;
	// fk_cols[i] = FK column on tables[i] linking to tables[i+1]
	vector<string> fk_cols;
};

// ----------------------------------------------------------------------------
// AVG decomposition — each AVG(x) is rewritten to SUM(x) + COUNT(*) so we can
// apply Laplace noise independently with ε/2 budget split on each component.
// ----------------------------------------------------------------------------

struct AvgInfo {
	idx_t sum_pos;           // position of the AVG->SUM in the rewritten aggregate (in-place)
	idx_t count_pos;         // position of the appended COUNT(*) in the rewritten aggregate
	double midpoint;         // added back after releasing the centered SUM / COUNT ratio
	double normalized_bound; // per-row bound after midpoint centering: (upper - lower) / 2
	double output_lower_bound = 0.0;
	double output_upper_bound = 0.0;
};

struct AvgDomain {
	double lower;
	double upper;
};

struct AvgBounds {
	vector<AvgDomain> domains;
};

static unique_ptr<Expression> BindScalarLocal(OptimizerExtensionInput &input, const string &func_name,
                                              vector<unique_ptr<Expression>> children) {
	FunctionBinder function_binder(input.context);
	ErrorData error;
	auto expr = function_binder.BindScalarFunction(DEFAULT_SCHEMA, func_name, std::move(children), error);
	if (error.HasError()) {
		throw InternalException("dp: failed to bind scalar function '" + func_name + "': " + error.Message());
	}
	return expr;
}

static unique_ptr<Expression> BindAggregateLocal(OptimizerExtensionInput &input, const string &func_name,
                                                 vector<unique_ptr<Expression>> children) {
	FunctionBinder function_binder(input.context);
	ErrorData error;
	vector<LogicalType> arg_types;
	arg_types.reserve(children.size());
	for (auto &child : children) {
		arg_types.push_back(child->return_type);
	}

	auto &entry = Catalog::GetSystemCatalog(input.context)
	                  .GetEntry<AggregateFunctionCatalogEntry>(input.context, DEFAULT_SCHEMA, func_name);
	auto best = function_binder.BindFunction(entry.name, entry.functions, arg_types, error);
	if (!best.IsValid()) {
		throw InternalException("dp: failed to bind aggregate function '" + func_name + "': " + error.Message());
	}
	auto func = entry.functions.GetFunctionByOffset(best.GetIndex());
	return function_binder.BindAggregateFunction(func, std::move(children), nullptr, AggregateType::NON_DISTINCT);
}

static unique_ptr<Expression> ClipToBounds(OptimizerExtensionInput &input, unique_ptr<Expression> expr, double lo,
                                           double hi, const LogicalType &restore_type);

// Replace each AVG(x) with SUM(x) in-place and append a COUNT(*) at the end.
// Returns info pairing each (sum_pos, count_pos) for post-processing.
static vector<AvgInfo> RewriteAvgAggregates(OptimizerExtensionInput &input, LogicalAggregate *agg,
                                            bool use_bounded_mean, const AvgBounds *avg_bounds = nullptr) {
	vector<AvgInfo> avg_infos;
	idx_t n_original = agg->expressions.size();
	idx_t avg_idx = 0;
	for (idx_t i = 0; i < n_original; i++) {
		auto &aggr = agg->expressions[i]->Cast<BoundAggregateExpression>();
		if (aggr.function.name != "avg") {
			continue;
		}
		// Preserve `FILTER (WHERE ...)` from the original AVG: both the rewritten SUM
		// and the appended COUNT(*) must apply the same predicate so the ratio matches
		// the user-visible AVG semantics.
		auto arg = std::move(aggr.children[0]);
		auto filter_for_sum = std::move(aggr.filter);
		auto filter_for_count = filter_for_sum ? filter_for_sum->Copy() : nullptr;
		AvgDomain domain {0.0, 0.0};
		if (avg_bounds) {
			D_ASSERT(avg_idx < avg_bounds->domains.size());
			domain = avg_bounds->domains[avg_idx];
		}
		double midpoint = 0.0;
		double normalized_bound = 0.0;
		if (use_bounded_mean) {
			// Google DP's bounded mean centers bounded values around the midpoint before
			// adding SUM noise, then shifts the noised ratio back.
			D_ASSERT(avg_bounds);
			midpoint = (domain.lower + domain.upper) / 2.0;
			normalized_bound = (domain.upper - domain.lower) / 2.0;
			arg = ClipToBounds(input, std::move(arg), domain.lower, domain.upper, LogicalType::DOUBLE);
			auto midpoint_const = make_uniq<BoundConstantExpression>(Value::DOUBLE(midpoint));
			arg = input.optimizer.BindScalarFunction("-", std::move(arg), std::move(midpoint_const));
		}

		agg->expressions[i] = BindPlainAggregate(input, "sum", std::move(arg));
		agg->expressions[i]->Cast<BoundAggregateExpression>().filter = std::move(filter_for_sum);

		idx_t count_pos = agg->expressions.size();
		agg->expressions.push_back(BindPlainAggregate(input, "count_star", nullptr));
		agg->expressions.back()->Cast<BoundAggregateExpression>().filter = std::move(filter_for_count);

		AvgInfo info;
		info.sum_pos = i;
		info.count_pos = count_pos;
		info.midpoint = midpoint;
		info.normalized_bound = normalized_bound;
		info.output_lower_bound = domain.lower;
		info.output_upper_bound = domain.upper;
		avg_infos.push_back(info);
		avg_idx++;
	}
	if (!avg_infos.empty()) {
		agg->ResolveOperatorTypes();
	}
	return avg_infos;
}

// ----------------------------------------------------------------------------
// FK chain extraction — validates linear chain structure, detects self-joins
// ----------------------------------------------------------------------------

static DPFKChain ExtractFKChain(const PrivacyCompatibilityResult &check, const vector<LogicalGet *> &gets,
                                const vector<string> &privacy_units, bool include_fk_cols, bool allow_self_joins) {
	(void)privacy_units;

	// Self-join check: no table name may appear more than once in the plan
	if (!allow_self_joins) {
		std::unordered_map<string, int> name_count;
		for (auto *g : gets) {
			auto table = g->GetTable();
			if (!table) {
				continue;
			}
			string name = StringUtil::Lower(table->name);
			name_count[name]++;
			if (name_count[name] > 1) {
				throw InvalidInputException("dp: self-joins are not supported (table '" + table->name +
				                            "' appears more than once)");
			}
		}
	}

	// Fail-OPEN heuristic: a table with no privacy annotations (no PRIVACY_KEY/LINK, no protected
	// columns) is treated as a public side input and joined in without privacy accounting. This relies
	// on the analyst annotating every private table — an UNannotated private table would silently get
	// no protection. (Matches Google DP's "public side tables allowed", but the discipline is required.)
	auto is_public_side_table = [&](const string &table_name) {
		auto meta_it = check.table_metadata.find(table_name);
		if (meta_it == check.table_metadata.end()) {
			return true;
		}
		auto protected_it = check.protected_columns.find(StringUtil::Lower(table_name));
		bool has_protected = protected_it != check.protected_columns.end() && !protected_it->second.empty();
		return meta_it->second.pks.empty() && meta_it->second.fks.empty() && !has_protected;
	};

	vector<string> private_non_pu_tables;
	private_non_pu_tables.reserve(check.scanned_non_pu_tables.size());
	for (auto &non_pu : check.scanned_non_pu_tables) {
		if (is_public_side_table(non_pu)) {
			PRIVACY_DEBUG_PRINT("[DP] treating unannotated table '" + non_pu + "' as public side input");
			continue;
		}
		private_non_pu_tables.push_back(non_pu);
	}

	// Single-table case: no non-PU tables → chain is just the PU itself, ES = 1
	if (private_non_pu_tables.empty()) {
		if (check.scanned_pu_tables.size() != 1) {
			throw InvalidInputException("dp: expected exactly one privacy unit table, found " +
			                            std::to_string(check.scanned_pu_tables.size()));
		}
		return {{check.scanned_pu_tables[0]}, {}};
	}

	// At most one PU may be scanned directly. Zero is fine — the PU is then inferred from
	// PRIVACY_LINK metadata via fk_paths (no explicit join required).
	if (check.scanned_pu_tables.size() > 1) {
		string pu_list;
		for (auto &pu : check.scanned_pu_tables) {
			if (!pu_list.empty()) {
				pu_list += ", ";
			}
			pu_list += "'" + pu + "'";
		}
		throw InvalidInputException("dp: query touches multiple privacy unit tables (" + pu_list +
		                            "). Only one privacy unit is supported per query.");
	}

	// Find non-PU table with the longest FK path — this is the root of the chain.
	// fk_paths contains the full path to the PU (including the PU itself) for every scanned
	// non-PU table, whether or not the PU is explicitly joined in the query.
	const vector<string> *longest_path = nullptr;
	for (auto &non_pu : private_non_pu_tables) {
		auto it = check.fk_paths.find(non_pu);
		if (it == check.fk_paths.end() || it->second.empty()) {
			PRIVACY_DEBUG_PRINT("[DP] rejecting unlinked non-PU table '" + non_pu + "'");
			throw InvalidInputException("dp: table '" + non_pu +
			                            "' has no PRIVACY_LINK path to a privacy unit table. "
			                            "Declare the link with ALTER TABLE ADD PRIVACY_LINK.");
		}
		if (!longest_path || it->second.size() > longest_path->size()) {
			longest_path = &it->second;
		}
	}
	if (!longest_path) {
		throw InvalidInputException("dp: query scans non-PU tables but none has a PRIVACY_LINK path to a privacy "
		                            "unit table. Declare the link with ALTER TABLE ADD PRIVACY_LINK.");
	}

	// Build a set of tables in the longest path for membership checks
	std::unordered_set<string> path_set;
	for (auto &t : *longest_path) {
		path_set.insert(StringUtil::Lower(t));
	}

	// All non-PU scanned tables must appear in the longest path (linear chain requirement)
	for (auto &non_pu : private_non_pu_tables) {
		auto path_it = check.fk_paths.find(non_pu);
		if (path_it == check.fk_paths.end() || path_it->second.empty()) {
			PRIVACY_DEBUG_PRINT("[DP] rejecting unlinked non-PU table '" + non_pu + "'");
			throw InvalidInputException("dp: table '" + non_pu +
			                            "' has no PRIVACY_LINK path to a privacy unit table. "
			                            "Declare the link with ALTER TABLE ADD PRIVACY_LINK.");
		}
		if (path_set.find(StringUtil::Lower(non_pu)) == path_set.end()) {
			throw InvalidInputException("dp: star/diamond joins are not yet supported. Table '" + non_pu +
			                            "' is not in the linear FK chain to the privacy unit. "
			                            "All joined tables must form a single linear chain.");
		}
	}

	// Build fk_cols: for each consecutive pair in the chain, find the FK column
	DPFKChain chain;
	chain.tables = *longest_path;
	if (!include_fk_cols) {
		return chain;
	}
	chain.fk_cols.reserve(chain.tables.size() - 1);

	for (idx_t i = 0; i + 1 < chain.tables.size(); i++) {
		const string &from_table = chain.tables[i];
		const string &to_table = chain.tables[i + 1];

		auto meta_it = check.table_metadata.find(from_table);
		if (meta_it == check.table_metadata.end()) {
			throw InternalException("dp: no metadata for table '" + from_table + "'");
		}
		bool found_fk = false;
		for (auto &fk : meta_it->second.fks) {
			if (StringUtil::Lower(fk.first) == StringUtil::Lower(to_table)) {
				if (fk.second.empty()) {
					throw InternalException("dp: empty FK column list for " + from_table + " → " + to_table);
				}
				chain.fk_cols.push_back(fk.second[0]);
				found_fk = true;
				break;
			}
		}
		if (!found_fk) {
			throw InternalException("dp: could not find FK column from '" + from_table + "' to '" + to_table + "'");
		}
	}

	return chain;
}

static void CheckDPAggregateNode(LogicalAggregate *agg, const string &mechanism_name, bool allow_min_max);
static bool IsCountAggregate(const BoundAggregateExpression &aggr);

static LogicalAggregate *CheckDPAggregates(unique_ptr<LogicalOperator> &plan, const string &mechanism_name) {
	// dp_sass handles MIN/MAX via sample-and-aggregate; dp_standard via bounded global sensitivity
	// (values clipped to [dp_minmax_lower_bound, dp_minmax_upper_bound], sensitivity = U-L). dp_elastic
	// cannot: elastic sensitivity is defined only for counting queries, not order statistics.
	bool allow_min_max = mechanism_name == "dp_sass" || mechanism_name == "dp_standard";
	vector<LogicalAggregate *> aggs;
	FindAllAggregates(plan, aggs);
	if (aggs.size() != 1) {
		PRIVACY_DEBUG_PRINT("[" + mechanism_name + "] rejecting query with " + std::to_string(aggs.size()) +
		                    " aggregate nodes");
		throw InvalidInputException(mechanism_name + ": expected exactly one aggregate, found " +
		                            std::to_string(aggs.size()));
	}
	auto *agg = aggs[0];
	CheckDPAggregateNode(agg, mechanism_name, allow_min_max);
	return agg;
}

static void CheckDPAggregateNode(LogicalAggregate *agg, const string &mechanism_name, bool allow_min_max) {
	for (auto &expr : agg->expressions) {
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			throw InvalidInputException(mechanism_name + ": unsupported non-aggregate expression in aggregate node");
		}
		auto &aggr = expr->Cast<BoundAggregateExpression>();
		string name = StringUtil::Lower(aggr.function.name);
		bool is_count = name == "count" || name == "count_star";
		if (aggr.IsDistinct()) {
			if ((mechanism_name != "dp_sass" && mechanism_name != "dp_elastic" && mechanism_name != "dp_standard") ||
			    !is_count || aggr.children.size() != 1 || aggr.filter) {
				throw InvalidInputException(mechanism_name + ": only COUNT(DISTINCT x) is supported for DISTINCT");
			}
			continue;
		}
		bool is_sum_avg = name == "sum" || name == "avg";
		bool is_min_max = name == "min" || name == "max";
		if (is_min_max && !allow_min_max) {
			throw InvalidInputException(
			    mechanism_name +
			    ": MIN/MAX are not supported (elastic sensitivity is defined only for counting queries). "
			    "Use dp_standard or dp_sass for MIN/MAX.");
		}
		if (!is_count && !is_sum_avg && !(allow_min_max && is_min_max)) {
			string supported = allow_min_max ? "COUNT, SUM, AVG, MIN, and MAX" : "COUNT, SUM, and AVG";
			throw InvalidInputException(mechanism_name + ": only " + supported + " are supported (got '" +
			                            aggr.function.name + "')");
		}
		if ((is_sum_avg || is_min_max) && aggr.children.size() != 1) {
			throw InvalidInputException(mechanism_name + ": " + name + " must have exactly one argument");
		}
		if ((is_sum_avg || is_min_max) && !aggr.children[0]->return_type.IsNumeric()) {
			throw InvalidInputException(mechanism_name + ": " + name + " argument must be a numeric type (got '" +
			                            aggr.children[0]->return_type.ToString() + "')");
		}
	}
}

static DPFKChain ExtractDPFKChain(unique_ptr<LogicalOperator> &plan, const vector<string> &privacy_units,
                                  const PrivacyCompatibilityResult &check, bool allow_self_joins = false) {
	vector<LogicalGet *> gets;
	FindAllGetNodes(plan.get(), gets);
	return ExtractFKChain(check, gets, privacy_units, true, allow_self_joins);
}

static bool IsRightSideHiddenJoin(JoinType join_type) {
	return join_type == JoinType::MARK || join_type == JoinType::SEMI || join_type == JoinType::ANTI ||
	       join_type == JoinType::SINGLE;
}

static bool IsLeftSideHiddenJoin(JoinType join_type) {
	return join_type == JoinType::RIGHT_SEMI || join_type == JoinType::RIGHT_ANTI;
}

static void CountAccessibleTableScans(LogicalOperator *op, std::unordered_map<string, idx_t> &counts) {
	if (!op) {
		return;
	}
	if (op->type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op->Cast<LogicalGet>();
		auto table = get.GetTable();
		if (table) {
			counts[StringUtil::Lower(table->name)]++;
		}
		return;
	}

	if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
	    op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN || op->type == LogicalOperatorType::LOGICAL_ANY_JOIN) {
		auto &join = op->Cast<LogicalJoin>();
		if (IsRightSideHiddenJoin(join.join_type)) {
			if (!op->children.empty()) {
				CountAccessibleTableScans(op->children[0].get(), counts);
			}
			return;
		}
		if (IsLeftSideHiddenJoin(join.join_type)) {
			if (op->children.size() >= 2) {
				CountAccessibleTableScans(op->children[1].get(), counts);
			}
			return;
		}
	}

	for (auto &child : op->children) {
		CountAccessibleTableScans(child.get(), counts);
	}
}

static double ValidateDPSelfJoins(unique_ptr<LogicalOperator> &plan, const string &mech) {
	vector<LogicalGet *> gets;
	FindAllGetNodes(plan.get(), gets);
	std::unordered_map<string, idx_t> all_counts;
	for (auto *get : gets) {
		auto table = get->GetTable();
		if (table) {
			all_counts[StringUtil::Lower(table->name)]++;
		}
	}

	std::unordered_map<string, idx_t> accessible_counts;
	CountAccessibleTableScans(plan.get(), accessible_counts);

	idx_t max_duplicate_count = 1;
	for (auto &kv : all_counts) {
		if (kv.second <= 1) {
			continue;
		}
		max_duplicate_count = std::max(max_duplicate_count, kv.second);
		if (accessible_counts[kv.first] > 1) {
			throw InvalidInputException(mech +
			                            ": self-joins are only supported inside SEMI/ANTI/MARK subquery branches");
		}
	}
	return static_cast<double>(max_duplicate_count);
}

// ----------------------------------------------------------------------------
// mf_K computation — max frequency of fk_col values in table (one auxiliary query)
// ----------------------------------------------------------------------------

static double ComputeMfK(ClientContext &context, const string &table_name, const string &fk_col) {
	auto &db = DatabaseInstance::GetDatabase(context);
	Connection conn(db);
	string query = "SELECT COALESCE(CAST(MAX(cnt) AS DOUBLE), 0.0) "
	               "FROM (SELECT COUNT(*) AS cnt FROM " +
	               table_name + " GROUP BY " + fk_col + ")";
	auto result = conn.Query(query);
	if (!result || result->HasError()) {
		throw InvalidInputException("dp: failed to compute mf_K for " + table_name + "." + fk_col +
		                            (result ? (": " + result->GetError()) : ": null result"));
	}
	if (result->RowCount() == 0) {
		// Table is empty: no rows contribute → max frequency is 0
		return 0.0;
	}
	auto val = result->GetValue(0, 0);
	return val.IsNull() ? 0.0 : val.GetValue<double>();
}

// Collect mf_K values for all FK hops (one per non-PU table in the chain)
static vector<double> CollectMfKValues(ClientContext &context, const DPFKChain &chain) {
	vector<double> mf_values;
	mf_values.reserve(chain.fk_cols.size());
	for (idx_t i = 0; i + 1 < chain.tables.size(); i++) {
		double mf_k = ComputeMfK(context, chain.tables[i], chain.fk_cols[i]);
		PRIVACY_DEBUG_PRINT("[DP_ELASTIC] mf_K(" + chain.tables[i] + "." + chain.fk_cols[i] +
		                    ") = " + std::to_string(mf_k));
		mf_values.push_back(mf_k);
	}
	return mf_values;
}

// SES_β(D) = max_{k≥0} [product_i(mf_i + k) * exp(-β * k)]
// The maximum is at k* ≈ n/β − mean(mf_i). k_max must reach at least k*.
static double ComputeSmoothElasticSensitivity(const vector<double> &mf_values, double beta) {
	double ses = 1.0;
	for (auto m : mf_values) {
		ses *= m;
	}
	// FLEX (Johnson, Near & Song 2018): the maximizer of ∏ᵢ(mfᵢ+k)·e^(−βk) lies in k ∈ [0, m] with
	// m on the order of (polynomial degree)/β. The elastic sensitivity here is a degree-n product
	// (n = number of join hops), and its log-derivative Σ 1/(mfᵢ+k) − β = 0 gives k* ≈ n/β. Search
	// that full window with a margin — NOT a fixed ceiling, which for small β (small ε/δ) would stop
	// before k* and under-estimate the sensitivity (under-noise). The window depends on the query (n)
	// and budget (β), not the database size, per FLEX.
	double n = static_cast<double>(mf_values.size());
	double window = n / beta + 200.0;
	// An astronomically large window means ε (with δ) is too small to compute this tractably; refuse
	// rather than silently truncate the search (under-noise) or hang.
	if (!std::isfinite(window) || window > 1e9) {
		throw InvalidInputException(
		    "dp_elastic: ε (with δ) is too small to compute smooth elastic sensitivity tractably — the FLEX "
		    "search window n/β exceeds 1e9. Increase dp_epsilon or dp_delta.");
	}
	int64_t k_max = std::max(static_cast<int64_t>(window), static_cast<int64_t>(1000));
	for (int64_t k = 1; k <= k_max; k++) {
		double decay = std::exp(-beta * static_cast<double>(k));
		if (decay < 1e-15) {
			break;
		}
		double ls_k = 1.0;
		for (auto m : mf_values) {
			ls_k *= (m + static_cast<double>(k));
		}
		double term = ls_k * decay;
		if (term > ses) {
			ses = term;
		}
	}
	return ses;
}

// Compute ES as the noise multiplier (divide by epsilon in the caller to get Laplace scale).
// Smooth elastic sensitivity: ES = 2·SES_β with β = ε/(2·ln(2/δ)), achieves (ε,δ)-DP.
static double ComputeElasticSensitivity(ClientContext &context, const DPFKChain &chain, double epsilon) {
	auto mf_values = CollectMfKValues(context, chain);

	double delta = 0.0;
	if (!TryGetDpDelta(context, delta)) {
		throw InvalidInputException(
		    "dp: dp_delta must be set. Use SET dp_delta=<value> or PRAGMA refresh_dp_stats(<epsilon>).");
	}
	if (delta <= 0.0 || !std::isfinite(delta)) {
		throw InvalidInputException("dp: dp_delta must be a positive finite number (got " + std::to_string(delta) +
		                            ")");
	}
	if (delta >= 0.5) {
		throw InvalidInputException("dp: dp_delta must be < 0.5 for meaningful (ε,δ)-DP (got " + std::to_string(delta) +
		                            ")");
	}

	double beta = epsilon / (2.0 * std::log(2.0 / delta));
	double ses = ComputeSmoothElasticSensitivity(mf_values, beta);
	PRIVACY_DEBUG_PRINT("[DP_ELASTIC] smooth sensitivity: beta=" + std::to_string(beta) +
	                    " SES=" + std::to_string(ses));
	return 2.0 * ses;
}

static double ComputeWilsonPartitionThreshold(double epsilon_eta, double delta_eta, double cu) {
	if (!(epsilon_eta > 0.0) || !(delta_eta > 0.0) || !(delta_eta < 0.5) || !(cu > 0.0)) {
		throw InvalidInputException("dp partition selection: invalid epsilon, delta, or C_u");
	}
	// Wilson et al.: tau = 1 - C_u * log(2 - 2(1 - delta_eta)^(1/C_u)) / epsilon_eta.
	// Use log1p/expm1 so tiny delta_eta/C_u does not round (1 - delta_eta)^(1/C_u) to 1.
	double log_survival_per_group = std::log1p(-delta_eta) / cu;
	double log_release_probability = std::log(2.0) + std::log(-std::expm1(log_survival_per_group));
	double tau = 1.0 - (cu * log_release_probability) / epsilon_eta;
	if (!std::isfinite(tau)) {
		throw InvalidInputException("dp partition selection: computed non-finite threshold");
	}
	return tau;
}

// Which clipping + sensitivity strategy the shared Laplace pipeline uses.
//
// GLOBAL (standard DP): bound each privacy unit's *total* contribution to a fixed constant
//   (dp_sum_bound / dp_count_bound) via per-PU pre-aggregation, so the global sensitivity is
//   exactly that constant — data-independent → pure ε-DP, no dp_delta. See ApplyPerPuClipping.
// ELASTIC_SMOOTH (elastic DP): per-row clip + smooth elastic sensitivity 2·SES_β derived from
//   the (data-dependent) join max-frequencies; (ε,δ)-DP, requires dp_delta.
enum class DPSensitivityKind { GLOBAL, ELASTIC_SMOOTH };

// ----------------------------------------------------------------------------
// Aggregate helpers
// ----------------------------------------------------------------------------

// Returns true if any aggregate requires dp_sum_bound (SUM or AVG, before AVG→SUM rewrite).
static bool AggregateContainsSum(const LogicalAggregate *agg) {
	for (auto &expr : agg->expressions) {
		auto &a = expr->Cast<BoundAggregateExpression>();
		if (a.function.name == "sum" || a.function.name == "avg") {
			return true;
		}
	}
	return false;
}

static bool AggregateContainsAvg(const LogicalAggregate *agg) {
	for (auto &expr : agg->expressions) {
		auto &a = expr->Cast<BoundAggregateExpression>();
		if (a.function.name == "avg") {
			return true;
		}
	}
	return false;
}

static idx_t CountAvgAggregates(const LogicalAggregate *agg) {
	idx_t count = 0;
	for (auto &expr : agg->expressions) {
		auto &a = expr->Cast<BoundAggregateExpression>();
		if (a.function.name == "avg") {
			count++;
		}
	}
	return count;
}

static idx_t CountExplicitSumAggregates(const LogicalAggregate *agg) {
	idx_t count = 0;
	for (auto &expr : agg->expressions) {
		auto &a = expr->Cast<BoundAggregateExpression>();
		if (a.function.name == "sum") {
			count++;
		}
	}
	return count;
}

static const AvgInfo *FindAvgInfoForSumPos(const vector<AvgInfo> &avg_infos, idx_t aggregate_pos) {
	for (auto &info : avg_infos) {
		if (info.sum_pos == aggregate_pos) {
			return &info;
		}
	}
	return nullptr;
}

static const AvgInfo *FindAvgInfoForCountPos(const vector<AvgInfo> &avg_infos, idx_t aggregate_pos) {
	for (auto &info : avg_infos) {
		if (info.count_pos == aggregate_pos) {
			return &info;
		}
	}
	return nullptr;
}

static double StandardSumContributionBound(idx_t aggregate_pos, const vector<AvgInfo> &avg_infos, bool is_join,
                                           double sum_bound, double count_bound) {
	auto *avg_info = FindAvgInfoForSumPos(avg_infos, aggregate_pos);
	if (!avg_info || avg_info->normalized_bound <= 0.0) {
		return sum_bound;
	}
	return avg_info->normalized_bound * (is_join ? count_bound : 1.0);
}

static double GetRequiredDpBound(ClientContext &context, const string &setting_name, const string &mechanism_name);
static vector<double> ParseFiniteSettingList(ClientContext &context, const string &setting_name, bool &found);

static vector<double> GetDpSumContributionBounds(ClientContext &context, const string &mechanism_name,
                                                 const LogicalAggregate *agg, const vector<AvgInfo> &avg_infos,
                                                 bool is_join, double count_bound) {
	idx_t explicit_sum_count = 0;
	for (idx_t i = 0; i < agg->expressions.size(); i++) {
		auto &aggr = agg->expressions[i]->Cast<BoundAggregateExpression>();
		if (aggr.function.name == "sum" && !FindAvgInfoForSumPos(avg_infos, i)) {
			explicit_sum_count++;
		}
	}

	bool has_sum_list = false;
	auto sum_list = ParseFiniteSettingList(context, "dp_sum_bounds", has_sum_list);
	if (has_sum_list) {
		if (sum_list.size() != explicit_sum_count) {
			throw InvalidInputException(mechanism_name + ": expected " + std::to_string(explicit_sum_count) +
			                            " explicit SUM contribution bounds, but dp_sum_bounds has " +
			                            std::to_string(sum_list.size()) + " entries");
		}
		for (auto bound : sum_list) {
			if (bound <= 0.0) {
				throw InvalidInputException(mechanism_name + ": every dp_sum_bounds entry must be positive");
			}
		}
	}

	bool needs_scalar_sum_bound = false;
	for (idx_t i = 0; i < agg->expressions.size(); i++) {
		auto &aggr = agg->expressions[i]->Cast<BoundAggregateExpression>();
		if (aggr.function.name != "sum") {
			continue;
		}
		if (FindAvgInfoForSumPos(avg_infos, i)) {
			auto bound = StandardSumContributionBound(i, avg_infos, is_join, 0.0, count_bound);
			if (bound <= 0.0) {
				needs_scalar_sum_bound = true;
			}
		} else if (!has_sum_list) {
			needs_scalar_sum_bound = true;
		}
	}
	double scalar_sum_bound =
	    needs_scalar_sum_bound ? GetRequiredDpBound(context, "dp_sum_bound", mechanism_name) : 0.0;
	vector<double> result(agg->expressions.size(), 0.0);
	idx_t sum_list_idx = 0;
	for (idx_t i = 0; i < agg->expressions.size(); i++) {
		auto &aggr = agg->expressions[i]->Cast<BoundAggregateExpression>();
		if (aggr.function.name != "sum") {
			continue;
		}
		if (FindAvgInfoForSumPos(avg_infos, i)) {
			result[i] = StandardSumContributionBound(i, avg_infos, is_join, scalar_sum_bound, count_bound);
		} else {
			result[i] = has_sum_list ? sum_list[sum_list_idx++] : scalar_sum_bound;
		}
	}
	return result;
}

// Clip a non-NULL expression to [lo, hi] in DOUBLE domain, then cast back to `restore_type`.
// DuckDB's least/greatest ignore NULL arguments, so preserve NULL explicitly for SQL aggregate semantics.
static unique_ptr<Expression> ClipToBounds(OptimizerExtensionInput &input, unique_ptr<Expression> expr, double lo,
                                           double hi, const LogicalType &restore_type) {
	auto as_double = BoundCastExpression::AddCastToType(input.context, std::move(expr), LogicalType::DOUBLE);
	auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
	is_null->children.push_back(as_double->Copy());
	auto hi_const = make_uniq<BoundConstantExpression>(Value::DOUBLE(hi));
	auto lo_const = make_uniq<BoundConstantExpression>(Value::DOUBLE(lo));
	auto upper = input.optimizer.BindScalarFunction("least", std::move(as_double), std::move(hi_const));
	auto clipped = input.optimizer.BindScalarFunction("greatest", std::move(upper), std::move(lo_const));
	auto preserve_null = make_uniq<BoundCaseExpression>(LogicalType::DOUBLE);
	BoundCaseCheck null_check;
	null_check.when_expr = std::move(is_null);
	null_check.then_expr = make_uniq<BoundConstantExpression>(Value(LogicalType::DOUBLE));
	preserve_null->case_checks.push_back(std::move(null_check));
	preserve_null->else_expr = std::move(clipped);
	return BoundCastExpression::AddCastToType(input.context, std::move(preserve_null), restore_type);
}

static void ClipSumInputs(OptimizerExtensionInput &input, LogicalAggregate *agg, const vector<double> &sum_bounds) {
	for (idx_t i = 0; i < agg->expressions.size(); i++) {
		auto &expr = agg->expressions[i];
		auto &aggr = expr->Cast<BoundAggregateExpression>();
		if (aggr.function.name != "sum" || aggr.children.empty()) {
			continue;
		}
		auto original_type = aggr.children[0]->return_type;
		D_ASSERT(i < sum_bounds.size());
		double sum_bound = sum_bounds[i];
		aggr.children[0] = ClipToBounds(input, std::move(aggr.children[0]), -sum_bound, sum_bound, original_type);
	}
	agg->ResolveOperatorTypes();
}

static double GetRequiredDpBound(ClientContext &context, const string &setting_name, const string &mechanism_name) {
	double bound = 0.0;
	bool found = false;
	if (setting_name == "dp_sum_bound") {
		found = TryGetDpSumBound(context, bound);
	} else if (setting_name == "dp_count_bound") {
		found = TryGetDpCountBound(context, bound);
	} else if (setting_name == "dp_max_groups_contributed") {
		found = TryGetDpMaxGroupsContributed(context, bound);
	} else if (setting_name == "dp_sass_count_output_bound" || setting_name == "dp_sass_sum_output_bound" ||
	           setting_name == "dp_sass_avg_lower_bound" || setting_name == "dp_sass_avg_upper_bound") {
		Value val;
		found = context.TryGetCurrentSetting(setting_name, val) && !val.IsNull();
		if (found) {
			bound = val.GetValue<double>();
		}
	} else {
		throw InternalException("Unknown DP bound setting '" + setting_name + "'");
	}
	if (!found) {
		throw InvalidInputException(mechanism_name + ": " + setting_name + " must be set");
	}
	if (bound <= 0.0 || !std::isfinite(bound)) {
		throw InvalidInputException(mechanism_name + ": " + setting_name + " must be a positive finite number (got " +
		                            std::to_string(bound) + ")");
	}
	return bound;
}

static int64_t GetRequiredDpIntegerBound(ClientContext &context, const string &setting_name,
                                         const string &mechanism_name) {
	double bound = GetRequiredDpBound(context, setting_name, mechanism_name);
	double rounded = std::round(bound);
	if (std::fabs(bound - rounded) > 1e-9 || rounded < 1.0 ||
	    rounded > static_cast<double>(NumericLimits<int64_t>::Maximum())) {
		throw InvalidInputException(mechanism_name + ": " + setting_name +
		                            " must be a positive integer-valued bound (got " + std::to_string(bound) + ")");
	}
	return static_cast<int64_t>(rounded);
}

static double GetRequiredFiniteSetting(ClientContext &context, const string &setting_name,
                                       const string &mechanism_name) {
	Value val;
	if (!context.TryGetCurrentSetting(setting_name, val) || val.IsNull()) {
		throw InvalidInputException(mechanism_name + ": " + setting_name + " must be set");
	}
	double bound = val.GetValue<double>();
	if (!std::isfinite(bound)) {
		throw InvalidInputException(mechanism_name + ": " + setting_name + " must be finite (got " +
		                            std::to_string(bound) + ")");
	}
	return bound;
}

static bool TryGetFiniteSetting(ClientContext &context, const string &setting_name, double &bound) {
	Value val;
	if (!context.TryGetCurrentSetting(setting_name, val) || val.IsNull()) {
		return false;
	}
	bound = val.GetValue<double>();
	if (!std::isfinite(bound)) {
		throw InvalidInputException("dp: " + setting_name + " must be finite (got " + std::to_string(bound) + ")");
	}
	return true;
}

static vector<double> ParseFiniteSettingList(ClientContext &context, const string &setting_name, bool &found) {
	found = false;
	Value val;
	if (!context.TryGetCurrentSetting(setting_name, val) || val.IsNull()) {
		return {};
	}
	string text = val.ToString();
	StringUtil::Trim(text);
	if (text.empty()) {
		return {};
	}
	found = true;
	vector<double> result;
	auto parts = StringUtil::Split(text, ',');
	for (auto &part : parts) {
		StringUtil::Trim(part);
		if (part.empty()) {
			throw InvalidInputException("dp: " + setting_name + " contains an empty entry");
		}
		size_t parsed = 0;
		double value = 0.0;
		try {
			value = std::stod(part, &parsed);
		} catch (std::exception &) {
			throw InvalidInputException("dp: could not parse " + setting_name + " entry '" + part + "' as DOUBLE");
		}
		if (parsed != part.size() || !std::isfinite(value)) {
			throw InvalidInputException("dp: could not parse " + setting_name + " entry '" + part + "' as DOUBLE");
		}
		result.push_back(value);
	}
	return result;
}

static AvgBounds GetAvgBounds(ClientContext &context, const string &mechanism_name, idx_t avg_count,
                              bool allow_sass_legacy) {
	bool has_lower_list = false;
	bool has_upper_list = false;
	auto lower_list = ParseFiniteSettingList(context, "dp_avg_lower_bounds", has_lower_list);
	auto upper_list = ParseFiniteSettingList(context, "dp_avg_upper_bounds", has_upper_list);
	if (has_lower_list || has_upper_list) {
		if (!has_lower_list || !has_upper_list || lower_list.size() != upper_list.size()) {
			throw InvalidInputException("dp: dp_avg_lower_bounds and dp_avg_upper_bounds must have the same length");
		}
		if (lower_list.size() != avg_count) {
			throw InvalidInputException("dp: expected " + std::to_string(avg_count) +
			                            " AVG bounds, but dp_avg_lower_bounds has " +
			                            std::to_string(lower_list.size()) + " entries");
		}
		AvgBounds bounds;
		bounds.domains.reserve(avg_count);
		for (idx_t i = 0; i < avg_count; i++) {
			if (lower_list[i] >= upper_list[i]) {
				throw InvalidInputException("dp: every AVG lower bound must be smaller than its upper bound");
			}
			bounds.domains.push_back({lower_list[i], upper_list[i]});
		}
		return bounds;
	}

	double lower = 0.0;
	double upper = 0.0;
	bool used_sass_legacy = false;
	bool has_lower = TryGetFiniteSetting(context, "dp_avg_lower_bound", lower);
	bool has_upper = TryGetFiniteSetting(context, "dp_avg_upper_bound", upper);
	if (has_lower || has_upper) {
		if (!has_lower || !has_upper) {
			throw InvalidInputException("dp: dp_avg_lower_bound and dp_avg_upper_bound must be set together");
		}
	} else if (allow_sass_legacy) {
		lower = GetRequiredFiniteSetting(context, "dp_sass_avg_lower_bound", mechanism_name);
		upper = GetRequiredFiniteSetting(context, "dp_sass_avg_upper_bound", mechanism_name);
		used_sass_legacy = true;
	} else {
		lower = 0.0;
		upper = GetRequiredDpBound(context, "dp_sum_bound", mechanism_name);
	}
	if (lower >= upper) {
		if (used_sass_legacy) {
			throw InvalidInputException("dp_sass: dp_sass_avg_lower_bound must be smaller than "
			                            "dp_sass_avg_upper_bound");
		}
		throw InvalidInputException(mechanism_name + ": AVG lower bound must be smaller than AVG upper bound");
	}
	AvgBounds bounds;
	bounds.domains.reserve(avg_count);
	for (idx_t i = 0; i < avg_count; i++) {
		bounds.domains.push_back({lower, upper});
	}
	return bounds;
}

static vector<double> GetDpSassOutputBounds(ClientContext &context, idx_t output_count, const string &list_setting,
                                            const string &scalar_setting, const string &aggregate_name) {
	bool has_list = false;
	auto bounds = ParseFiniteSettingList(context, list_setting, has_list);
	if (has_list) {
		if (bounds.size() != output_count) {
			throw InvalidInputException("dp_sass: expected " + std::to_string(output_count) + " " + aggregate_name +
			                            " output bounds, but " + list_setting + " has " +
			                            std::to_string(bounds.size()) + " entries");
		}
		for (auto bound : bounds) {
			if (bound <= 0.0) {
				throw InvalidInputException("dp_sass: every " + list_setting + " entry must be positive");
			}
		}
		return bounds;
	}
	if (output_count == 0) {
		return {};
	}
	double scalar = GetRequiredDpBound(context, scalar_setting, "dp_sass");
	return vector<double>(output_count, scalar);
}

static vector<AvgDomain> GetDpSassOutputDomains(ClientContext &context, idx_t output_count,
                                                const string &lower_list_setting, const string &upper_list_setting,
                                                const string &legacy_list_setting, const string &legacy_scalar_setting,
                                                double legacy_lower_default, bool symmetric_legacy,
                                                const string &aggregate_name) {
	bool has_lower_list = false;
	bool has_upper_list = false;
	auto lower_list = ParseFiniteSettingList(context, lower_list_setting, has_lower_list);
	auto upper_list = ParseFiniteSettingList(context, upper_list_setting, has_upper_list);
	if (has_lower_list || has_upper_list) {
		if (!has_lower_list || !has_upper_list || lower_list.size() != upper_list.size()) {
			throw InvalidInputException("dp_sass: " + lower_list_setting + " and " + upper_list_setting +
			                            " must have the same length");
		}
		if (lower_list.size() != output_count) {
			throw InvalidInputException("dp_sass: expected " + std::to_string(output_count) + " " + aggregate_name +
			                            " output domains, but " + lower_list_setting + " has " +
			                            std::to_string(lower_list.size()) + " entries");
		}
		vector<AvgDomain> domains;
		domains.reserve(output_count);
		for (idx_t i = 0; i < output_count; i++) {
			if (lower_list[i] >= upper_list[i]) {
				throw InvalidInputException("dp_sass: every " + aggregate_name +
				                            " output-domain lower bound must be smaller than its upper bound");
			}
			domains.push_back({lower_list[i], upper_list[i]});
		}
		return domains;
	}

	auto legacy_uppers =
	    GetDpSassOutputBounds(context, output_count, legacy_list_setting, legacy_scalar_setting, aggregate_name);
	vector<AvgDomain> domains;
	domains.reserve(output_count);
	for (auto upper : legacy_uppers) {
		double lower = symmetric_legacy ? -upper : legacy_lower_default;
		domains.push_back({lower, upper});
	}
	return domains;
}

static vector<AvgDomain> GetDpSassCountOutputDomains(ClientContext &context, idx_t count_count) {
	return GetDpSassOutputDomains(context, count_count, "dp_sass_count_output_lower_bounds",
	                              "dp_sass_count_output_upper_bounds", "dp_sass_count_output_bounds",
	                              "dp_sass_count_output_bound", 0.0, false, "COUNT");
}

static vector<AvgDomain> GetDpSassSumOutputDomains(ClientContext &context, idx_t sum_count) {
	return GetDpSassOutputDomains(context, sum_count, "dp_sass_sum_output_lower_bounds",
	                              "dp_sass_sum_output_upper_bounds", "dp_sass_sum_output_bounds",
	                              "dp_sass_sum_output_bound", 0.0, true, "SUM");
}

// Public domain [L, U] for MIN/MAX under dp_standard. Values are clipped to this range, so the
// global sensitivity of MIN/MAX (change from removing/adding one PU) is exactly U - L.
static std::pair<double, double> GetDpMinMaxBounds(ClientContext &context, const string &mechanism_name) {
	double lower = GetRequiredFiniteSetting(context, "dp_minmax_lower_bound", mechanism_name);
	double upper = GetRequiredFiniteSetting(context, "dp_minmax_upper_bound", mechanism_name);
	if (lower >= upper) {
		throw InvalidInputException(mechanism_name + ": dp_minmax_lower_bound must be smaller than "
		                                             "dp_minmax_upper_bound");
	}
	return {lower, upper};
}

// Elastic Laplace scale = sensitivity / epsilon; sensitivity = es for COUNT, es * value_bound for SUM.
static double ElasticSensitivity(idx_t aggregate_pos, const BoundAggregateExpression &aggr,
                                 const vector<double> &sum_bounds, double es) {
	const string &name = aggr.function.name;
	if (name == "count" || name == "count_star") {
		return es;
	}
	if (name == "sum") {
		D_ASSERT(aggregate_pos < sum_bounds.size());
		return es * sum_bounds[aggregate_pos];
	}
	throw InternalException("dp: ElasticSensitivity received unsupported '" + name + "'");
}

static bool IsCountAggregate(const BoundAggregateExpression &aggr) {
	return aggr.function.name == "count" || aggr.function.name == "count_star";
}

// Does the aggregate contain any COUNT/COUNT(*) (after AVG→SUM+COUNT rewrite)?
static bool AggregateContainsCount(const LogicalAggregate *agg) {
	for (auto &expr : agg->expressions) {
		if (IsCountAggregate(expr->Cast<BoundAggregateExpression>())) {
			return true;
		}
	}
	return false;
}

static bool IsMinMaxAggregate(const BoundAggregateExpression &aggr) {
	return aggr.function.name == "min" || aggr.function.name == "max";
}

static bool AggregateContainsMinMax(const LogicalAggregate *agg) {
	for (auto &expr : agg->expressions) {
		if (IsMinMaxAggregate(expr->Cast<BoundAggregateExpression>())) {
			return true;
		}
	}
	return false;
}

static void EnsurePuAdjacentTableAvailable(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                                           LogicalAggregate *agg, const PrivacyCompatibilityResult &check,
                                           const DPFKChain &chain, const string &mech) {
	if (chain.tables.size() < 2) {
		return;
	}
	const string &adj_table = chain.tables[chain.tables.size() - 2];
	if (FindTableScanInSubtree(agg->children[0].get(), adj_table)) {
		return;
	}

	optional_idx start_pos;
	for (idx_t i = 0; i + 1 < chain.tables.size(); i++) {
		if (FindTableScanInSubtree(agg->children[0].get(), chain.tables[i])) {
			start_pos = i;
			break;
		}
	}
	if (!start_pos.IsValid()) {
		throw InternalException(mech + ": could not find a table from the PRIVACY_LINK chain in aggregate input");
	}

	vector<string> tables_to_join;
	for (idx_t i = start_pos.GetIndex() + 1; i + 1 < chain.tables.size(); i++) {
		tables_to_join.push_back(chain.tables[i]);
	}
	if (tables_to_join.empty()) {
		return;
	}

	auto *start_ref = FindNodeRefByTable(&agg->children[0], chain.tables[start_pos.GetIndex()]);
	if (!start_ref) {
		throw InternalException(mech + ": could not find join attachment table '" + chain.tables[start_pos.GetIndex()] +
		                        "' in aggregate input");
	}

	PRIVACY_DEBUG_PRINT("[" + mech + "] adding missing FK joins for per-PU clipping from '" +
	                    chain.tables[start_pos.GetIndex()] + "' to '" + adj_table + "'");
	auto &binder = input.optimizer.binder;
	unique_ptr<LogicalOperator> current = (*start_ref)->Copy(input.context);
	for (auto &table : tables_to_join) {
		idx_t table_index = binder.GenerateTableIndex();
		auto get = CreateLogicalGet(input.context, plan, table, table_index);
		current = CreateLogicalJoin(check, input.context, std::move(current), std::move(get));
	}
	ReplaceNode(agg->children[0], *start_ref, current, &binder);
}

static unique_ptr<Expression> BuildPerPuGroupExpression(OptimizerExtensionInput &input,
                                                        unique_ptr<LogicalOperator> &plan, LogicalAggregate *agg,
                                                        const PrivacyCompatibilityResult &check, const DPFKChain &chain,
                                                        const string &mech) {
	if (chain.tables.empty()) {
		throw InternalException(mech + ": empty privacy-unit FK chain");
	}

	string key_table;
	vector<string> key_columns;
	if (chain.tables.size() >= 2) {
		key_table = chain.tables[chain.tables.size() - 2];
		if (chain.fk_cols.empty()) {
			throw InternalException(mech + ": missing FK columns for privacy-unit grouping");
		}
		key_columns.push_back(chain.fk_cols.back());
	} else {
		key_table = chain.tables[0];
		auto meta_it = check.table_metadata.find(key_table);
		if (meta_it == check.table_metadata.end() || meta_it->second.pks.empty()) {
			throw InternalException(mech + ": PU table '" + key_table + "' has no PRIVACY_KEY metadata");
		}
		key_columns = meta_it->second.pks;
	}

	LogicalGet *key_get = FindTableScanInSubtree(agg, key_table);
	if (!key_get) {
		throw InternalException(mech + ": could not find PU key source table '" + key_table +
		                        "' below aggregate input");
	}

	vector<unique_ptr<Expression>> key_exprs;
	key_exprs.reserve(key_columns.size());
	for (auto &key_column : key_columns) {
		idx_t proj_idx = EnsureProjectedColumn(*key_get, key_column);
		if (proj_idx == DConstants::INVALID_INDEX) {
			throw InternalException(mech + ": failed to project PU key column '" + key_column + "'");
		}
		auto col_index = key_get->GetColumnIds()[proj_idx];
		auto col_type = key_get->GetColumnType(col_index);
		ColumnBinding source_binding(key_get->table_index, proj_idx);
		auto propagated = PropagateSingleBinding(*plan, key_get->table_index, source_binding, col_type, agg);
		if (propagated.table_index == DConstants::INVALID_INDEX) {
			throw InternalException(mech + ": could not propagate PU key column '" + key_column +
			                        "' to aggregate input");
		}
		key_exprs.push_back(make_uniq<BoundColumnRefExpression>(col_type, propagated));
	}

	auto pu_group_expr = BuildXorHash(input, std::move(key_exprs));
	pu_group_expr = BoundCastExpression::AddCastToType(input.context, std::move(pu_group_expr), LogicalType::UBIGINT);
	pu_group_expr->alias = mech + "_pu_key";
	return pu_group_expr;
}

static unique_ptr<Expression> BuildLowerGroupRef(LogicalAggregate *lower_agg, idx_t lower_group_index, idx_t column) {
	return make_uniq<BoundColumnRefExpression>(lower_agg->types[column], ColumnBinding(lower_group_index, column));
}

static unique_ptr<Expression> BuildIsNullPredicate(unique_ptr<Expression> expr) {
	auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
	is_null->children.push_back(std::move(expr));
	return is_null;
}

// One ranking cap: `rank_type` OVER (PARTITION BY partition_cols ORDER BY hash(order_cols)) <= cap.
// WINDOW_ROW_NUMBER caps rows per partition (compiles to a per-partition top-K of capacity `cap`,
// so keep `cap` small); WINDOW_RANK_DENSE caps distinct order-key groups per partition. Column
// indices reference the group output of the source aggregate.
struct RankCapSpec {
	ExpressionType rank_type;
	vector<idx_t> partition_cols;
	vector<idx_t> order_cols;
	vector<idx_t> keep_null_cols;
	int64_t cap;
};

// Wrap `child` in a single LogicalWindow carrying one ranking expression per spec, then a single
// LogicalFilter keeping rows where every rank <= its cap. Column indices reference `src`'s group
// output (table index `src_group_index`). Using one window (rather than stacking several) keeps
// every reference exactly one level above the source, which DuckDB's binding resolver requires.
static unique_ptr<LogicalOperator> BuildRankCapFilter(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> child,
                                                      LogicalAggregate *src, idx_t src_group_index,
                                                      const vector<RankCapSpec> &specs) {
	if (specs.empty()) {
		return child;
	}
	auto &binder = input.optimizer.binder;
	idx_t window_index = binder.GenerateTableIndex();
	auto window = make_uniq<LogicalWindow>(window_index);

	for (auto &spec : specs) {
		auto rank = make_uniq<BoundWindowExpression>(spec.rank_type, LogicalType::BIGINT, nullptr, nullptr);
		for (auto col : spec.partition_cols) {
			rank->partitions.push_back(BuildLowerGroupRef(src, src_group_index, col));
		}
		for (auto col : spec.keep_null_cols) {
			rank->orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_LAST,
			                          BuildIsNullPredicate(BuildLowerGroupRef(src, src_group_index, col)));
		}
		vector<unique_ptr<Expression>> hash_children;
		hash_children.reserve(spec.order_cols.size());
		for (auto col : spec.order_cols) {
			hash_children.push_back(BuildLowerGroupRef(src, src_group_index, col));
		}
		auto hash_expr = BindScalarLocal(input, "hash", std::move(hash_children));
		rank->orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_LAST, std::move(hash_expr));
		rank->start = WindowBoundary::UNBOUNDED_PRECEDING;
		rank->end = WindowBoundary::CURRENT_ROW_ROWS;
		window->expressions.push_back(std::move(rank));
	}
	window->children.push_back(std::move(child));

	vector<unique_ptr<Expression>> keep_conditions;
	for (idx_t i = 0; i < specs.size(); i++) {
		auto rank_ref = make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT, ColumnBinding(window_index, i));
		auto cap_const = make_uniq<BoundConstantExpression>(Value::BIGINT(specs[i].cap));
		unique_ptr<Expression> keep_condition = make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_LESSTHANOREQUALTO, std::move(rank_ref), std::move(cap_const));
		for (auto col : specs[i].keep_null_cols) {
			keep_condition = make_uniq<BoundConjunctionExpression>(
			    ExpressionType::CONJUNCTION_OR, std::move(keep_condition),
			    BuildIsNullPredicate(BuildLowerGroupRef(src, src_group_index, col)));
		}
		keep_conditions.push_back(std::move(keep_condition));
	}
	unique_ptr<Expression> keep_expr;
	if (keep_conditions.size() == 1) {
		keep_expr = std::move(keep_conditions[0]);
	} else {
		auto conj = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		conj->children = std::move(keep_conditions);
		keep_expr = std::move(conj);
	}
	auto filter = make_uniq<LogicalFilter>(std::move(keep_expr));
	filter->children.push_back(std::move(window));
	return filter;
}

static unique_ptr<Expression> BuildIsNotNullPredicate(unique_ptr<Expression> expr) {
	auto not_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
	not_null->children.push_back(std::move(expr));
	return not_null;
}

static unique_ptr<Expression> ConjoinPredicates(vector<unique_ptr<Expression>> predicates) {
	vector<unique_ptr<Expression>> present;
	for (auto &predicate : predicates) {
		if (predicate) {
			present.push_back(std::move(predicate));
		}
	}
	if (present.empty()) {
		return nullptr;
	}
	if (present.size() == 1) {
		return std::move(present[0]);
	}
	auto conjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	conjunction->children = std::move(present);
	return std::move(conjunction);
}

static vector<unique_ptr<Expression>> PrepareVariableMDistinctListFilters(OptimizerExtensionInput &input,
                                                                          LogicalAggregate *agg,
                                                                          const Expression &pu_hash_expr,
                                                                          int64_t count_bound) {
	vector<unique_ptr<Expression>> filters(agg->expressions.size());
	if (count_bound <= 0) {
		return filters;
	}

	auto &binder = input.optimizer.binder;
	idx_t window_index = binder.GenerateTableIndex();
	auto window = make_uniq<LogicalWindow>(window_index);
	vector<idx_t> distinct_filter_indices;

	for (idx_t ai = 0; ai < agg->expressions.size(); ai++) {
		auto &aggr = agg->expressions[ai]->Cast<BoundAggregateExpression>();
		string name = StringUtil::Lower(aggr.function.name);
		if (name != "count" || !aggr.IsDistinct() || aggr.children.empty()) {
			continue;
		}

		auto rank =
		    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_RANK_DENSE, LogicalType::BIGINT, nullptr, nullptr);
		for (auto &group : agg->groups) {
			rank->partitions.push_back(group->Copy());
		}
		rank->partitions.push_back(pu_hash_expr.Copy());
		rank->orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_LAST, aggr.children[0]->Copy());
		rank->start = WindowBoundary::UNBOUNDED_PRECEDING;
		rank->end = WindowBoundary::CURRENT_ROW_ROWS;
		idx_t rank_pos = window->expressions.size();
		window->expressions.push_back(std::move(rank));

		auto rank_ref = make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT, ColumnBinding(window_index, rank_pos));
		auto cap_const = make_uniq<BoundConstantExpression>(Value::BIGINT(count_bound));
		auto in_cap = make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO,
		                                                   std::move(rank_ref), std::move(cap_const));
		vector<unique_ptr<Expression>> predicates;
		predicates.push_back(BuildIsNotNullPredicate(aggr.children[0]->Copy()));
		predicates.push_back(std::move(in_cap));
		filters[ai] = ConjoinPredicates(std::move(predicates));
		distinct_filter_indices.push_back(ai);
	}

	if (distinct_filter_indices.empty()) {
		return filters;
	}
	window->children.push_back(std::move(agg->children[0]));
	agg->children[0] = std::move(window);
	return filters;
}

static void ApplyMaxGroupsContributed(OptimizerExtensionInput &input, LogicalAggregate *agg,
                                      const PuPreAggregationInfo &pre, int64_t max_groups) {
	if (pre.num_original_groups == 0) {
		return;
	}
	// Each input row is already one (PU, group) partial, so a per-PU row-number cap keeps at most
	// `max_groups` groups per PU. Partition by the PU key, order by hash(PU, group columns).
	idx_t pu_group_col = pre.num_original_groups;
	RankCapSpec spec;
	spec.rank_type = ExpressionType::WINDOW_ROW_NUMBER;
	spec.partition_cols = {pu_group_col};
	spec.order_cols.reserve(pre.num_original_groups + 1);
	spec.order_cols.push_back(pu_group_col);
	for (idx_t i = 0; i < pre.num_original_groups; i++) {
		spec.order_cols.push_back(i);
	}
	spec.cap = max_groups;
	vector<RankCapSpec> specs;
	specs.push_back(std::move(spec));
	agg->children[0] =
	    BuildRankCapFilter(input, std::move(agg->children[0]), pre.lower_agg, pre.lower_agg->group_index, specs);
}

// Standard (global-sensitivity) clipping. Bounds each privacy unit's total contribution to a
// fixed constant so global sensitivity equals that constant (data-independent → pure ε-DP for
// ungrouped queries; grouped queries additionally need private partition selection).
//
//  - Every query is pre-aggregated per PU. If the scanned table is the PU table, the PRIVACY_KEY
//    identifies the PU; if the scanned table is linked by PRIVACY_LINK, the PU-adjacent FK identifies
//    the PU.
//  - Each per-PU partial is clipped, then the final aggregate sums/clamps the clipped partials.
//    SUM→dp_sum_bound, COUNT→dp_count_bound.
//  - Grouped queries multiply vector sensitivity by dp_max_groups_contributed (Google DP's L0
//    contribution bound), and partition selection reserves an extra budget cell in FinalizeDPLaplace.
//
// Fold an aggregate's FILTER (WHERE cond) into its value: value → CASE WHEN cond THEN value END
// Fold an aggregate's FILTER (WHERE cond) into its value: value → CASE WHEN cond THEN value ELSE e.
// This matches FILTER semantics while avoiding a separate filter clause, which mis-binds in DuckDB's
// aggregate planner (the filter resolves to the value column) when the aggregate sits under a per-PU
// pre-aggregation. The ELSE branch differs by aggregate:
//   - SUM: ELSE 0. A non-matching row must contribute 0. Using NULL would make an all-non-matching
//     PU's partial NULL, and the downstream greatest/least clip turns NULL into the *bound* (those
//     functions ignore NULLs), inflating the sum.
//   - COUNT: ELSE NULL, so non-matching rows are skipped (a 0 would be counted).
// Returns the value unchanged when there is no filter.
static unique_ptr<Expression> FoldFilterIntoValue(const BoundAggregateExpression &aggr, unique_ptr<Expression> value,
                                                  bool else_zero) {
	if (!aggr.filter) {
		return value;
	}
	auto value_type = value->return_type;
	auto case_expr = make_uniq<BoundCaseExpression>(value_type);
	BoundCaseCheck check;
	check.when_expr = aggr.filter->Copy();
	check.then_expr = std::move(value);
	case_expr->case_checks.push_back(std::move(check));
	case_expr->else_expr = make_uniq<BoundConstantExpression>(else_zero ? Value::Numeric(value_type, 0)
	                                                                    : Value(value_type)); // 0, or NULL of the type
	return std::move(case_expr);
}

// Returns the per-aggregate sensitivity vector (one entry per current agg->expression) and sets
// `per_pu_out` true when a per-PU pre-aggregation was inserted (so support counts PU groups).
static vector<double> ApplyPerPuClipping(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                                         LogicalAggregate *agg, const PrivacyCompatibilityResult &check,
                                         const DPFKChain &chain, const string &mech, const vector<AvgInfo> &avg_infos,
                                         bool &per_pu_out) {
	bool has_sum = AggregateContainsSum(agg);
	bool has_count = AggregateContainsCount(agg);
	per_pu_out = true;

	double count_bound = has_count ? GetRequiredDpBound(input.context, "dp_count_bound", mech) : 0.0;
	vector<double> sum_bounds = has_sum
	                                ? GetDpSumContributionBounds(input.context, mech, agg, avg_infos, true, count_bound)
	                                : vector<double>(agg->expressions.size(), 0.0);
	int64_t group_bound =
	    !agg->groups.empty() ? GetRequiredDpIntegerBound(input.context, "dp_max_groups_contributed", mech) : 1;

	// MIN/MAX: values are clipped to the public domain [mm_lower, mm_upper], so removing/adding one PU
	// moves the extremum by at most the range mm_range = mm_upper - mm_lower (per affected group).
	bool has_min_max = AggregateContainsMinMax(agg);
	double mm_lower = 0.0;
	double mm_upper = 0.0;
	if (has_min_max) {
		std::tie(mm_lower, mm_upper) = GetDpMinMaxBounds(input.context, mech);
	}
	double mm_range = mm_upper - mm_lower;

	idx_t n = agg->expressions.size();
	vector<double> sens;
	sens.reserve(n);

	// Build plain per-PU lower aggregates mirroring each top aggregate.
	EnsurePuAdjacentTableAvailable(input, plan, agg, check, chain, mech);
	auto pu_key = BuildPerPuGroupExpression(input, plan, agg, check, chain, mech);
	vector<bool> is_count;
	is_count.reserve(n);
	vector<unique_ptr<Expression>> lower_exprs;
	lower_exprs.reserve(n);
	for (idx_t i = 0; i < n; i++) {
		auto &aggr = agg->expressions[i]->Cast<BoundAggregateExpression>();
		is_count.push_back(IsCountAggregate(aggr));
		if (aggr.IsDistinct()) {
			lower_exprs.push_back(agg->expressions[i]->Copy()); // filtered DISTINCT is rejected upstream
		} else if (IsCountAggregate(aggr)) {
			if (aggr.function.name == "count" && !aggr.children.empty()) {
				lower_exprs.push_back(BindPlainAggregate(
				    input, "count", FoldFilterIntoValue(aggr, aggr.children[0]->Copy(), /*else_zero=*/false)));
			} else {
				// COUNT(*): no value child, so a FILTER clause binds correctly — attach it directly.
				lower_exprs.push_back(BindPlainAggregate(input, "count_star", nullptr));
				if (aggr.filter) {
					lower_exprs.back()->Cast<BoundAggregateExpression>().filter = aggr.filter->Copy();
				}
			}
		} else if (IsMinMaxAggregate(aggr)) {
			// Per-PU MIN/MAX partial. else_zero=false → filtered-out rows become NULL, which MIN/MAX skip.
			lower_exprs.push_back(BindPlainAggregate(
			    input, aggr.function.name, FoldFilterIntoValue(aggr, aggr.children[0]->Copy(), /*else_zero=*/false)));
		} else { // sum
			lower_exprs.push_back(BindPlainAggregate(
			    input, "sum", FoldFilterIntoValue(aggr, aggr.children[0]->Copy(), /*else_zero=*/true)));
		}
	}

	auto pre = InsertPuPreAggregation(input, agg, std::move(lower_exprs), std::move(pu_key));
	ApplyMaxGroupsContributed(input, agg, pre, group_bound);

	// Rewrite each top aggregate: SUM/COUNT → SUM(clip(partial)); MIN/MAX → MIN/MAX(clip(partial)).
	for (idx_t i = 0; i < n; i++) {
		// agg->expressions[i] still holds the original aggregate until overwritten below.
		auto &orig = agg->expressions[i]->Cast<BoundAggregateExpression>();
		bool pos_min_max = IsMinMaxAggregate(orig);
		string min_max_name = pos_min_max ? orig.function.name : string();
		auto lower_type = pre.lower_agg->types[pre.num_original_groups + 1 + i];
		unique_ptr<Expression> lower_ref =
		    make_uniq<BoundColumnRefExpression>(lower_type, ColumnBinding(pre.lower_agg_index, i));
		if (pos_min_max) {
			// Clip the per-PU partial to [mm_lower, mm_upper], then take the overall MIN/MAX. Removing one
			// PU shifts the extremum by at most mm_range per group → L1 sensitivity mm_range · group_bound.
			PRIVACY_DEBUG_PRINT("[" + mech + "] join MIN/MAX rewrite: " + min_max_name + "(clip partial) sensitivity=" +
			                    std::to_string(mm_range * static_cast<double>(group_bound)));
			auto clipped = ClipToBounds(input, std::move(lower_ref), mm_lower, mm_upper, lower_type);
			agg->expressions[i] = BindPlainAggregate(input, min_max_name, std::move(clipped));
			sens.push_back(mm_range * static_cast<double>(group_bound));
		} else {
			double bound = is_count[i] ? count_bound : sum_bounds[i];
			double lo = is_count[i] ? 0.0 : -bound; // counts are non-negative
			auto clipped = ClipToBounds(input, std::move(lower_ref), lo, bound, lower_type);
			agg->expressions[i] = BindPlainAggregate(input, "sum", std::move(clipped));
			sens.push_back(bound * static_cast<double>(group_bound));
		}
	}
	agg->ResolveOperatorTypes();
	return sens;
}

static unique_ptr<Expression> BuildSupportKeyExpression(unique_ptr<LogicalOperator> &plan, const DPFKChain &chain) {
	if (chain.tables.size() > 2) {
		throw InvalidInputException("dp: privacy_min_group_count currently supports single-table PU queries "
		                            "and one-hop PRIVACY_LINK chains only");
	}
	if (chain.tables.size() == 1) {
		return nullptr;
	}

	auto *get = FindTableScanInSubtree(plan.get(), chain.tables[0]);
	if (!get) {
		throw InternalException("dp: could not find support source table '" + chain.tables[0] + "'");
	}
	idx_t proj_idx = EnsureProjectedColumn(*get, chain.fk_cols[0]);
	if (proj_idx == DConstants::INVALID_INDEX) {
		throw InternalException("dp: failed to project support key column '" + chain.fk_cols[0] + "'");
	}
	auto col_index = get->GetColumnIds()[proj_idx];
	auto col_type = get->GetColumnType(col_index);
	return make_uniq<BoundColumnRefExpression>(col_type, ColumnBinding(get->table_index, proj_idx));
}

static optional_idx AddSupportAggregate(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                                        LogicalAggregate *agg, const DPFKChain &chain, double threshold, bool per_pu) {
	if (threshold <= 0.0 || !std::isfinite(threshold)) {
		return optional_idx();
	}

	// After a per-PU pre-aggregation the aggregate input already has exactly one row per
	// privacy unit, so a plain COUNT(*) over those rows is the per-group PU support. Without
	// pre-aggregation we count DISTINCT PU keys (or rows, on a single PU table).
	if (per_pu) {
		agg->expressions.push_back(BindPlainAggregate(input, "count_star", nullptr));
	} else {
		auto support_key = BuildSupportKeyExpression(plan, chain);
		if (support_key) {
			agg->expressions.push_back(
			    BindPlainAggregate(input, "count", std::move(support_key), AggregateType::DISTINCT));
		} else {
			agg->expressions.push_back(BindPlainAggregate(input, "count_star", nullptr));
		}
	}
	agg->ResolveOperatorTypes();
	return optional_idx(agg->expressions.size() - 1);
}

static LogicalFilter *ApplySupportFilter(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                                         LogicalAggregate *agg, idx_t support_pos, double threshold,
                                         double noise_scale = 0.0) {
	auto support_ref = make_uniq<BoundColumnRefExpression>(agg->types[agg->groups.size() + support_pos],
	                                                       ColumnBinding(agg->aggregate_index, support_pos));
	unique_ptr<Expression> support_expr =
	    BoundCastExpression::AddCastToType(input.context, std::move(support_ref), LogicalType::DOUBLE);
	if (noise_scale > 0.0 && std::isfinite(noise_scale)) {
		unique_ptr<Expression> nonce = make_uniq<BoundConstantExpression>(
		    Value::UBIGINT(PAC_MAGIC_HASH ^ (static_cast<uint64_t>(agg->aggregate_index) * PAC_MAGIC_HASH) ^
		                   static_cast<uint64_t>(support_pos + 1)));
		for (idx_t gi = 0; gi < agg->groups.size(); gi++) {
			unique_ptr<Expression> group_ref =
			    make_uniq<BoundColumnRefExpression>(agg->types[gi], ColumnBinding(agg->group_index, gi));
			auto group_hash = input.optimizer.BindScalarFunction("hash", std::move(group_ref));
			nonce = input.optimizer.BindScalarFunction("xor", std::move(nonce), std::move(group_hash));
		}
		vector<unique_ptr<Expression>> noise_children;
		noise_children.push_back(std::move(support_expr));
		noise_children.push_back(make_uniq<BoundConstantExpression>(Value::DOUBLE(noise_scale)));
		noise_children.push_back(std::move(nonce));
		support_expr = BindScalarLocal(input, "dp_noise", std::move(noise_children));
	}
	auto threshold_const = make_uniq<BoundConstantExpression>(Value::DOUBLE(threshold));
	auto condition = make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
	                                                      std::move(support_expr), std::move(threshold_const));

	auto filter = make_uniq<LogicalFilter>();
	filter->expressions.push_back(std::move(condition));

	auto *slot = FindOperatorSlotByPointer(plan, agg);
	if (!slot) {
		throw InternalException("dp: could not locate aggregate slot for support filter");
	}
	filter->children.push_back(std::move(*slot));
	filter->ResolveOperatorTypes();
	auto *filter_ptr = filter.get();
	*slot = std::move(filter);
	return filter_ptr;
}

// ----------------------------------------------------------------------------
// Wrap aggregate output — inserts LogicalProjection above `agg` with
// dp_noise applied to each DP-target column, cast back to original type.
// Returns the inserted projection so the caller can layer additional rewrites.
// ----------------------------------------------------------------------------

struct NoiseProjection {
	idx_t proj_idx;
	LogicalProjection *proj_ptr;
	vector<LogicalType> proj_types; // all output types [groups..., noised_aggs...]

	NoiseProjection(idx_t idx, LogicalProjection *ptr, vector<LogicalType> types)
	    : proj_idx(idx), proj_ptr(ptr), proj_types(std::move(types)) {
	}
};

static std::unordered_set<idx_t> BuildAvgComponentSet(const vector<AvgInfo> &avg_infos) {
	std::unordered_set<idx_t> avg_components;
	for (auto &info : avg_infos) {
		avg_components.insert(info.sum_pos);
		avg_components.insert(info.count_pos);
	}
	return avg_components;
}

static NoiseProjection InsertProjectionAndRemap(unique_ptr<LogicalOperator> &plan,
                                                unique_ptr<LogicalProjection> projection, LogicalOperator *insert_above,
                                                idx_t proj_idx, idx_t source_group_idx, idx_t source_agg_idx,
                                                idx_t n_groups, idx_t n_aggs, idx_t source_agg_offset,
                                                const string &error_message) {
	auto *slot = FindOperatorSlotByPointer(plan, insert_above);
	if (!slot) {
		throw InternalException(error_message);
	}
	auto old_node = std::move(*slot);
	projection->children.push_back(std::move(old_node));
	projection->ResolveOperatorTypes();
	auto *proj_ptr = projection.get();
	auto proj_types = projection->types;
	*slot = std::move(projection);

	ColumnBindingReplacer replacer;
	for (idx_t gi = 0; gi < n_groups; gi++) {
		replacer.replacement_bindings.emplace_back(ColumnBinding(source_group_idx, gi), ColumnBinding(proj_idx, gi),
		                                           proj_types[gi]);
	}
	for (idx_t ai = 0; ai < n_aggs; ai++) {
		replacer.replacement_bindings.emplace_back(ColumnBinding(source_agg_idx, source_agg_offset + ai),
		                                           ColumnBinding(proj_idx, n_groups + ai), proj_types[n_groups + ai]);
	}
	replacer.stop_operator = proj_ptr;
	replacer.VisitOperator(*plan);

	return {proj_idx, proj_ptr, std::move(proj_types)};
}

// `output_types` gives the desired final type of each DP aggregate column. It differs from the
// live agg type only for standard-DP per-PU clipping, where e.g. COUNT becomes SUM(clipped count)
// (BIGINT→HUGEINT); casting the noised value back to the original type keeps upstream column
// references valid.
static NoiseProjection WrapAggregateWithLaplace(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                                                LogicalAggregate *agg, LogicalOperator *insert_above,
                                                const vector<double> &agg_scales,
                                                const vector<LogicalType> &output_types) {
	idx_t n_groups = agg->groups.size();
	idx_t n_aggs = agg_scales.size();
	idx_t group_idx = agg->group_index;
	idx_t agg_idx = agg->aggregate_index;

	auto agg_types = agg->types;
	D_ASSERT(agg_types.size() >= n_groups + n_aggs);
	D_ASSERT(output_types.size() >= n_aggs);

	vector<unique_ptr<Expression>> proj_exprs;
	proj_exprs.reserve(n_groups + n_aggs);

	// MIN/MAX (dp_standard) releases are clamped back to the public domain after noise — DP-free
	// post-processing that keeps outputs in [mm_lower, mm_upper].
	bool has_min_max = AggregateContainsMinMax(agg);
	double mm_lower = 0.0;
	double mm_upper = 0.0;
	if (has_min_max) {
		// Only dp_standard reaches here with MIN/MAX (dp_elastic rejects them in the checker). If MIN/MAX
		// is ever extended to another Laplace mode with its own bounds settings, thread the mechanism name
		// through instead of hardcoding "dp_standard".
		std::tie(mm_lower, mm_upper) = GetDpMinMaxBounds(input.context, "dp_standard");
	}

	for (idx_t gi = 0; gi < n_groups; gi++) {
		proj_exprs.push_back(make_uniq<BoundColumnRefExpression>(agg_types[gi], ColumnBinding(group_idx, gi)));
	}

	for (idx_t ai = 0; ai < n_aggs; ai++) {
		auto agg_col_type = agg_types[n_groups + ai];
		const auto &out_type = output_types[ai];
		auto col_ref = make_uniq<BoundColumnRefExpression>(agg_col_type, ColumnBinding(agg_idx, ai));
		double scale = agg_scales[ai];
		if (!std::isfinite(scale)) {
			// A non-finite scale means the sensitivity or ε overflowed (e.g. an elastic-sensitivity
			// product that blew up). Releasing the aggregate without noise would violate DP, so we
			// refuse rather than silently emit the raw value.
			throw InvalidInputException(
			    "dp: computed Laplace noise scale is not finite (sensitivity/ε overflow) — refusing to "
			    "release the aggregate without noise. Check dp_sum_bound / dp_epsilon and the join structure.");
		}
		if (scale <= 0.0) {
			// scale == 0 is the intentional noise-disabled path (privacy_noise=false / zero sensitivity):
			// pass through, still casting back to the original output type if the rewrite changed it.
			unique_ptr<Expression> passthrough = std::move(col_ref);
			if (agg_col_type != out_type) {
				passthrough = BoundCastExpression::AddCastToType(input.context, std::move(passthrough), out_type);
			}
			proj_exprs.push_back(std::move(passthrough));
			continue;
		}
		unique_ptr<Expression> value_expr =
		    BoundCastExpression::AddCastToType(input.context, std::move(col_ref), LogicalType::DOUBLE);
		auto scale_expr = make_uniq<BoundConstantExpression>(Value::DOUBLE(scale));
		unique_ptr<Expression> nonce = make_uniq<BoundConstantExpression>(Value::UBIGINT(
		    PAC_MAGIC_HASH ^ (static_cast<uint64_t>(agg_idx) * PAC_MAGIC_HASH) ^ static_cast<uint64_t>(ai + 1)));
		for (idx_t gi = 0; gi < n_groups; gi++) {
			unique_ptr<Expression> group_ref =
			    make_uniq<BoundColumnRefExpression>(agg_types[gi], ColumnBinding(group_idx, gi));
			auto group_hash = input.optimizer.BindScalarFunction("hash", std::move(group_ref));
			nonce = input.optimizer.BindScalarFunction("xor", std::move(nonce), std::move(group_hash));
		}
		vector<unique_ptr<Expression>> noise_children;
		noise_children.push_back(std::move(value_expr));
		noise_children.push_back(std::move(scale_expr));
		noise_children.push_back(std::move(nonce));
		unique_ptr<Expression> noised = BindScalarLocal(input, "dp_noise", std::move(noise_children));
		if (has_min_max) {
			if (IsMinMaxAggregate(agg->expressions[ai]->Cast<BoundAggregateExpression>())) {
				noised = ClipToBounds(input, std::move(noised), mm_lower, mm_upper, LogicalType::DOUBLE);
			}
		}
		if (out_type != LogicalType::DOUBLE) {
			noised = BoundCastExpression::AddCastToType(input.context, std::move(noised), out_type);
		}
		proj_exprs.push_back(std::move(noised));
	}

	idx_t proj_idx = input.optimizer.binder.GenerateTableIndex();
	auto projection = make_uniq<LogicalProjection>(proj_idx, std::move(proj_exprs));
	return InsertProjectionAndRemap(plan, std::move(projection), insert_above, proj_idx, group_idx, agg_idx, n_groups,
	                                n_aggs, 0, "dp: could not locate noise insertion slot in plan");
}

static bool IsPlainCountStarAggregate(const BoundAggregateExpression &aggr) {
	return aggr.function.name == "count_star" && !aggr.IsDistinct() && aggr.children.empty() && !aggr.filter;
}

static bool IsDPSassFastCountStarEligible(const LogicalAggregate *agg, const DPFKChain &chain) {
	if (!agg->groups.empty() || agg->expressions.size() != 1) {
		return false;
	}
	if (chain.tables.size() != 1 || !chain.fk_cols.empty()) {
		return false;
	}
	auto &aggr = agg->expressions[0]->Cast<BoundAggregateExpression>();
	return IsPlainCountStarAggregate(aggr);
}

static bool TryCompileDPSassFastCountStar(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                                          LogicalAggregate *agg, const DPFKChain &chain, double epsilon) {
	if (!GetBooleanSetting(input.context, "dp_sass_fast_count_star", false)) {
		return false;
	}
	if (!IsDPSassFastCountStarEligible(agg, chain)) {
		PRIVACY_DEBUG_PRINT("[DP_SAMPLE_MEDIAN] dp_sass_fast_count_star enabled but query is not eligible");
		return false;
	}

	// This is intentionally not an SAA release. The internal flag is for COUNT(*) workloads such as
	// ClickBench Q1 where the user accepts dp_count_bound as a public validity assumption.
	double count_bound = GetRequiredDpBound(input.context, "dp_count_bound", "dp_sass");
	bool noise_enabled = GetBooleanSetting(input.context, "privacy_noise", true);
	double scale = noise_enabled ? (count_bound / epsilon) : 0.0;

	agg->ResolveOperatorTypes();
	vector<double> agg_scales {scale};
	vector<LogicalType> output_types {agg->types[agg->groups.size()]};
	PRIVACY_DEBUG_PRINT("[DP_SAMPLE_MEDIAN] using fast COUNT(*) Laplace path; scale=" + std::to_string(scale));
	WrapAggregateWithLaplace(input, plan, agg, agg, agg_scales, output_types);
	return true;
}

// ----------------------------------------------------------------------------
// AVG ratio projection — inserts a LogicalProjection above `insert_above` that
// computes noised_SUM / max(1, noised_COUNT) for each AVG-rewritten position.
// Non-AVG columns are passed through unchanged.
// Returns {ratio_proj_idx, ratio_proj_ptr} for downstream use.
// ----------------------------------------------------------------------------

static std::pair<idx_t, LogicalProjection *>
WrapAvgRatioProjection(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                       const NoiseProjection &noise_proj, const vector<AvgInfo> &avg_infos, idx_t n_groups,
                       idx_t n_original_aggs, LogicalOperator *insert_above, bool null_on_nonpositive_count,
                       bool clamp_output = false, double clamp_lo = 0.0, double clamp_hi = 0.0) {
	D_ASSERT(!avg_infos.empty());

	// dp_sass 'ratio' clamps the released mean back into its public output domain [clamp_lo, clamp_hi]
	// (free DP post-processing), matching the lane_average estimator's domain. Off for the Laplace modes.
	auto maybe_clamp = [&](unique_ptr<Expression> v, const AvgInfo &info) -> unique_ptr<Expression> {
		if (!clamp_output) {
			return v;
		}
		double lo = info.output_lower_bound < info.output_upper_bound ? info.output_lower_bound : clamp_lo;
		double hi = info.output_lower_bound < info.output_upper_bound ? info.output_upper_bound : clamp_hi;
		return ClipToBounds(input, std::move(v), lo, hi, LogicalType::DOUBLE);
	};

	// Build lookup: original-agg-position → AvgInfo
	vector<const AvgInfo *> avg_lookup(n_original_aggs, nullptr);
	for (auto &info : avg_infos) {
		D_ASSERT(info.sum_pos < n_original_aggs);
		avg_lookup[info.sum_pos] = &info;
	}

	vector<unique_ptr<Expression>> proj_exprs;
	proj_exprs.reserve(n_groups + n_original_aggs);

	// Pass-through group columns
	for (idx_t gi = 0; gi < n_groups; gi++) {
		proj_exprs.push_back(
		    make_uniq<BoundColumnRefExpression>(noise_proj.proj_types[gi], ColumnBinding(noise_proj.proj_idx, gi)));
	}

	// For each original agg position: pass-through or compute AVG ratio
	for (idx_t ai = 0; ai < n_original_aggs; ai++) {
		auto *avg_info = avg_lookup[ai];
		if (!avg_info) {
			// Non-AVG: pass the noised value through
			const auto &col_type = noise_proj.proj_types[n_groups + ai];
			proj_exprs.push_back(
			    make_uniq<BoundColumnRefExpression>(col_type, ColumnBinding(noise_proj.proj_idx, n_groups + ai)));
		} else {
			// AVG: compute CAST(noised_sum AS DOUBLE) / CAST(noised_count AS DOUBLE)
			const AvgInfo &info = *avg_info;
			const auto &sum_type = noise_proj.proj_types[n_groups + info.sum_pos];
			const auto &cnt_type = noise_proj.proj_types[n_groups + info.count_pos];

			auto make_sum_dbl = [&]() {
				auto sum_ref = make_uniq<BoundColumnRefExpression>(
				    sum_type, ColumnBinding(noise_proj.proj_idx, n_groups + info.sum_pos));
				return BoundCastExpression::AddCastToType(input.context, std::move(sum_ref), LogicalType::DOUBLE);
			};
			auto make_cnt_dbl = [&]() {
				auto cnt_ref = make_uniq<BoundColumnRefExpression>(
				    cnt_type, ColumnBinding(noise_proj.proj_idx, n_groups + info.count_pos));
				return BoundCastExpression::AddCastToType(input.context, std::move(cnt_ref), LogicalType::DOUBLE);
			};
			auto add_midpoint = [&](unique_ptr<Expression> value) -> unique_ptr<Expression> {
				if (info.midpoint == 0.0) {
					return value;
				}
				auto midpoint = make_uniq<BoundConstantExpression>(Value::DOUBLE(info.midpoint));
				return input.optimizer.BindScalarFunction("+", std::move(value), std::move(midpoint));
			};

			// DuckDB registers "/" as "divide" for scalar functions
			auto denominator = make_cnt_dbl();
			if (!null_on_nonpositive_count) {
				auto one = make_uniq<BoundConstantExpression>(Value::DOUBLE(1.0));
				denominator = input.optimizer.BindScalarFunction("greatest", std::move(denominator), std::move(one));
				auto ratio = input.optimizer.BindScalarFunction("/", make_sum_dbl(), std::move(denominator));
				proj_exprs.push_back(maybe_clamp(add_midpoint(std::move(ratio)), info));
			} else {
				auto ratio = input.optimizer.BindScalarFunction("/", make_sum_dbl(), std::move(denominator));
				auto case_expr = make_uniq<BoundCaseExpression>(LogicalType::DOUBLE);
				BoundCaseCheck check;
				check.when_expr =
				    make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO, make_cnt_dbl(),
				                                         make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0)));
				check.then_expr = make_uniq<BoundConstantExpression>(Value(LogicalType::DOUBLE));
				case_expr->case_checks.push_back(std::move(check));
				case_expr->else_expr = maybe_clamp(add_midpoint(std::move(ratio)), info);
				proj_exprs.push_back(std::move(case_expr));
			}
		}
	}

	idx_t ratio_idx = input.optimizer.binder.GenerateTableIndex();
	auto ratio_proj = make_uniq<LogicalProjection>(ratio_idx, std::move(proj_exprs));
	auto inserted = InsertProjectionAndRemap(plan, std::move(ratio_proj), insert_above, ratio_idx, noise_proj.proj_idx,
	                                         noise_proj.proj_idx, n_groups, n_original_aggs, n_groups,
	                                         "dp: could not locate insert point for AVG ratio projection");
	return {inserted.proj_idx, inserted.proj_ptr};
}

static string GetDpSassReleaseMethod(ClientContext &context) {
	Value v;
	if (!context.TryGetCurrentSetting("dp_sass_release", v) || v.IsNull()) {
		return "median";
	}
	string method = StringUtil::Lower(v.ToString());
	if (method != "median" && method != "average") {
		throw InvalidInputException("dp_sass: dp_sass_release must be 'median' or 'average' (got '" + method + "')");
	}
	return method;
}

static string GetDpSassAvgMethod(ClientContext &context) {
	Value v;
	if (!context.TryGetCurrentSetting("dp_sass_avg_method", v) || v.IsNull()) {
		return "lane_average";
	}
	string method = StringUtil::Lower(v.ToString());
	if (method != "lane_average" && method != "nonempty_lane_average" && method != "ratio") {
		throw InvalidInputException("dp_sass: dp_sass_avg_method must be 'lane_average', "
		                            "'nonempty_lane_average', or 'ratio' (got '" +
		                            method + "')");
	}
	return method;
}

static string GetDpSassSumMethod(ClientContext &context) {
	Value v;
	if (!context.TryGetCurrentSetting("dp_sass_sum_method", v) || v.IsNull()) {
		return "lane_average";
	}
	string method = StringUtil::Lower(v.ToString());
	if (method != "lane_average" && method != "nonempty_lane_average") {
		throw InvalidInputException("dp_sass: dp_sass_sum_method must be 'lane_average' or "
		                            "'nonempty_lane_average' (got '" +
		                            method + "')");
	}
	return method;
}

static idx_t InsertDpSassNoiseIdentityWindow(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                                             LogicalOperator *&insert_above) {
	auto *slot = FindOperatorSlotByPointer(plan, insert_above);
	if (!slot) {
		throw InternalException("dp_sass: could not locate insertion point for aggregate-cell identity");
	}
	idx_t window_index = input.optimizer.binder.GenerateTableIndex();
	auto window = make_uniq<LogicalWindow>(window_index);
	auto row_number =
	    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT, nullptr, nullptr);
	row_number->start = WindowBoundary::UNBOUNDED_PRECEDING;
	row_number->end = WindowBoundary::CURRENT_ROW_ROWS;
	window->expressions.push_back(std::move(row_number));
	window->children.push_back(std::move(*slot));
	window->ResolveOperatorTypes();
	insert_above = window.get();
	*slot = std::move(window);
	PRIVACY_DEBUG_PRINT("[DP_SAMPLE_MEDIAN] added chunk-global group identity for independent aggregate-cell noise");
	return window_index;
}

static NoiseProjection WrapSampleMedianProjection(
    OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan, LogicalAggregate *agg,
    const vector<double> &epsilons, const vector<double> &deltas, const vector<LogicalType> &output_types,
    const vector<double> &lower_bounds, const vector<double> &upper_bounds, const vector<double> &empty_defaults,
    LogicalOperator *insert_above, const string &release_method, const vector<idx_t> &agg_positions,
    const vector<bool> &nonempty_lane_releases, const vector<double> &nonempty_lane_baselines,
    const vector<double> &nonempty_lane_sensitivities) {
	idx_t n_groups = agg->groups.size();
	idx_t n_aggs = output_types.size();
	D_ASSERT(epsilons.size() == n_aggs);
	D_ASSERT(deltas.size() == n_aggs);
	D_ASSERT(lower_bounds.size() == n_aggs);
	D_ASSERT(upper_bounds.size() == n_aggs);
	D_ASSERT(empty_defaults.size() == n_aggs);
	D_ASSERT(agg_positions.size() == n_aggs);
	D_ASSERT(nonempty_lane_releases.size() == n_aggs);
	D_ASSERT(nonempty_lane_baselines.size() == n_aggs);
	D_ASSERT(nonempty_lane_sensitivities.size() == n_aggs);
	bool stability_query_mode = GetBooleanSetting(input.context, "dp_sass_stability_query_mode", false);
	bool noise_scale_query_mode = GetBooleanSetting(input.context, "dp_sass_noise_scale_query_mode", false);
	bool average_release = release_method == "average";
	bool variable_m = GetDpSassM(input.context) > DP_SASS_DEFAULT_M;
	optional_idx noise_identity_index;
	if (!stability_query_mode && !noise_scale_query_mode &&
	    std::any_of(nonempty_lane_releases.begin(), nonempty_lane_releases.end(),
	                [](bool enabled) { return enabled; })) {
		if (n_aggs > static_cast<idx_t>(std::numeric_limits<int32_t>::max())) {
			throw InvalidInputException("dp_sass: too many aggregate cells for independent noise assignment");
		}
		noise_identity_index = InsertDpSassNoiseIdentityWindow(input, plan, insert_above);
	}

	vector<unique_ptr<Expression>> proj_exprs;
	proj_exprs.reserve(n_groups + n_aggs);
	for (idx_t gi = 0; gi < n_groups; gi++) {
		proj_exprs.push_back(make_uniq<BoundColumnRefExpression>(agg->types[gi], ColumnBinding(agg->group_index, gi)));
	}

	int sample_lanes = GetDpSampleLanes(input.context);
	for (idx_t ai = 0; ai < n_aggs; ai++) {
		idx_t agg_pos = agg_positions[ai];
		vector<unique_ptr<Expression>> children;
		children.push_back(make_uniq<BoundColumnRefExpression>(agg->types[n_groups + agg_pos],
		                                                       ColumnBinding(agg->aggregate_index, agg_pos)));
		if (stability_query_mode) {
			children.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(static_cast<int32_t>(ai))));
			unique_ptr<Expression> recorded = BindScalarLocal(input, "dp_sass_record_stability", std::move(children));
			if (output_types[ai] != LogicalType::DOUBLE) {
				recorded = BoundCastExpression::AddCastToType(input.context, std::move(recorded), output_types[ai]);
			}
			proj_exprs.push_back(std::move(recorded));
			continue;
		}
		children.push_back(make_uniq<BoundConstantExpression>(Value::DOUBLE(epsilons[ai])));
		if (!average_release) {
			// median path: dp_smooth_median_noise(list, ε, δ, lanes, lower, upper)
			children.push_back(make_uniq<BoundConstantExpression>(Value::DOUBLE(deltas[ai])));
		}
		// average path: dp_gupt_mean_noise(list, ε, lanes, lower, upper) — pure ε, no δ.
		children.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(sample_lanes)));
		children.push_back(make_uniq<BoundConstantExpression>(Value::DOUBLE(lower_bounds[ai])));
		children.push_back(make_uniq<BoundConstantExpression>(Value::DOUBLE(upper_bounds[ai])));
		if (!nonempty_lane_releases[ai]) {
			// Per-kind public default used to fill empty lanes so the populated-lane count is
			// data-independent (no NULL leak; fixed GUPT denominator).
			children.push_back(make_uniq<BoundConstantExpression>(Value::DOUBLE(empty_defaults[ai])));
		} else {
			children.push_back(make_uniq<BoundConstantExpression>(Value::DOUBLE(nonempty_lane_baselines[ai])));
			children.push_back(make_uniq<BoundConstantExpression>(Value::DOUBLE(nonempty_lane_sensitivities[ai])));
		}
		const char *terminal = nullptr;
		if (nonempty_lane_releases[ai]) {
			if (noise_scale_query_mode) {
				children.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(static_cast<int32_t>(ai))));
				terminal = "dp_nonempty_mean_record_noise_scale";
			} else {
				D_ASSERT(noise_identity_index.IsValid());
				children.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(static_cast<int32_t>(ai))));
				children.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(static_cast<int32_t>(n_aggs))));
				children.push_back(make_uniq<BoundColumnRefExpression>(
				    LogicalType::BIGINT, ColumnBinding(noise_identity_index.GetIndex(), 0)));
				terminal = "dp_nonempty_mean_noise";
			}
		} else if (noise_scale_query_mode) {
			children.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(static_cast<int32_t>(ai))));
			terminal = average_release
			               ? (variable_m ? "dp_gupt_mean_m_record_noise_scale" : "dp_gupt_mean_record_noise_scale")
			               : "dp_smooth_median_record_noise_scale";
		} else {
			terminal = average_release ? (variable_m ? "dp_gupt_mean_m_noise" : "dp_gupt_mean_noise")
			                           : "dp_smooth_median_noise";
		}
		unique_ptr<Expression> noised = BindScalarLocal(input, terminal, std::move(children));
		if (output_types[ai] != LogicalType::DOUBLE) {
			noised = BoundCastExpression::AddCastToType(input.context, std::move(noised), output_types[ai]);
		}
		proj_exprs.push_back(std::move(noised));
	}

	idx_t proj_idx = input.optimizer.binder.GenerateTableIndex();
	auto projection = make_uniq<LogicalProjection>(proj_idx, std::move(proj_exprs));
	return InsertProjectionAndRemap(plan, std::move(projection), insert_above, proj_idx, agg->group_index,
	                                agg->aggregate_index, n_groups, n_aggs, 0,
	                                "dp_sample_median: could not locate aggregate slot for median projection");
}

static bool IsDpSampleCountDistinct(const LogicalAggregate *agg) {
	if (agg->expressions.size() != 1 || agg->expressions[0]->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	auto &aggr = agg->expressions[0]->Cast<BoundAggregateExpression>();
	string name = StringUtil::Lower(aggr.function.name);
	return aggr.IsDistinct() && (name == "count" || name == "count_star") && aggr.children.size() == 1;
}

static bool IsSinglePuKeyDistinctCount(unique_ptr<LogicalOperator> &plan, LogicalAggregate *agg,
                                       const PrivacyCompatibilityResult &check, const vector<string> &privacy_units,
                                       const PrivPUHashingSetup &pu_setup, bool allow_self_joins) {
	if (!IsDpSampleCountDistinct(agg) || !agg->groups.empty() || pu_setup.pu_names.size() != 1) {
		return false;
	}
	const auto &pu_table_name = pu_setup.pu_names[0];
	auto meta_it = check.table_metadata.find(pu_table_name);
	if (meta_it == check.table_metadata.end() || meta_it->second.pks.size() != 1) {
		return false;
	}

	LogicalGet *key_get = FindAccessibleGetInSubtree(plan, agg, pu_table_name);
	vector<string> key_columns = meta_it->second.pks;
	if (!key_get) {
		vector<LogicalGet *> gets;
		FindAllGetNodes(plan.get(), gets);
		auto chain = ExtractFKChain(check, gets, privacy_units, /*include_fk_cols=*/true, allow_self_joins);
		if (chain.tables.size() >= 2 && !chain.fk_cols.empty()) {
			const string &linked_table = chain.tables[chain.tables.size() - 2];
			key_get = FindAccessibleGetInSubtree(plan, agg, linked_table);
			if (key_get) {
				key_columns = {chain.fk_cols.back()};
			}
		}
	}
	if (!key_get || key_columns.size() != 1) {
		return false;
	}

	idx_t proj_idx = EnsureProjectedColumn(*key_get, key_columns[0]);
	if (proj_idx == DConstants::INVALID_INDEX) {
		throw InternalException("dp_sass: failed to project PU key column '" + key_columns[0] + "'");
	}
	auto col_index = key_get->GetColumnIds()[proj_idx];
	auto col_type = key_get->GetColumnType(col_index);
	ColumnBinding source_binding(key_get->table_index, proj_idx);
	auto propagated = PropagateSingleBinding(*plan, key_get->table_index, source_binding, col_type, agg);
	if (propagated.table_index == DConstants::INVALID_INDEX) {
		return false;
	}

	auto &aggr = agg->expressions[0]->Cast<BoundAggregateExpression>();
	auto key_ref = make_uniq<BoundColumnRefExpression>(col_type, propagated);
	return Expression::Equals(*aggr.children[0], *key_ref);
}

struct DpSassContributionBounds {
	double count_bound = 0.0;
	double sum_bound = 0.0;
	vector<double> sum_bounds_by_aggregate;
	int64_t count_bound_integer = 0;
	bool has_count_bound = false;
	bool has_sum_bound = false;
	bool has_integer_count_bound = false;
};

static double GetDpSassSumContributionBound(const DpSassContributionBounds &bounds, idx_t aggregate_pos) {
	if (aggregate_pos < bounds.sum_bounds_by_aggregate.size() && bounds.sum_bounds_by_aggregate[aggregate_pos] > 0.0) {
		return bounds.sum_bounds_by_aggregate[aggregate_pos];
	}
	return bounds.sum_bound;
}

static void RequireDpSassCountBound(ClientContext &context, DpSassContributionBounds &bounds, bool integer) {
	if (integer) {
		if (!bounds.has_integer_count_bound) {
			bounds.count_bound_integer = GetRequiredDpIntegerBound(context, "dp_count_bound", "dp_sass");
			bounds.has_integer_count_bound = true;
			if (!bounds.has_count_bound) {
				bounds.count_bound = static_cast<double>(bounds.count_bound_integer);
				bounds.has_count_bound = true;
			}
		}
		return;
	}
	if (!bounds.has_count_bound) {
		bounds.count_bound = GetRequiredDpBound(context, "dp_count_bound", "dp_sass");
		bounds.has_count_bound = true;
	}
}

static void RequireDpSassSumBound(ClientContext &context, DpSassContributionBounds &bounds) {
	if (!bounds.has_sum_bound) {
		bounds.sum_bound = GetRequiredDpBound(context, "dp_sum_bound", "dp_sass");
		bounds.has_sum_bound = true;
	}
}

static DpSassContributionBounds GetDpSassContributionBounds(ClientContext &context, const LogicalAggregate *agg,
                                                            const vector<AvgInfo> &avg_infos) {
	DpSassContributionBounds bounds;
	bounds.sum_bounds_by_aggregate.assign(agg->expressions.size(), 0.0);
	idx_t explicit_sum_count = 0;
	for (idx_t i = 0; i < agg->expressions.size(); i++) {
		auto &aggr = agg->expressions[i]->Cast<BoundAggregateExpression>();
		string name = StringUtil::Lower(aggr.function.name);
		if (name == "sum" && !FindAvgInfoForSumPos(avg_infos, i)) {
			explicit_sum_count++;
		}
	}
	bool has_sum_list = false;
	auto sum_list = ParseFiniteSettingList(context, "dp_sum_bounds", has_sum_list);
	if (has_sum_list) {
		if (sum_list.size() != explicit_sum_count) {
			throw InvalidInputException("dp_sass: expected " + std::to_string(explicit_sum_count) +
			                            " explicit SUM contribution bounds, but dp_sum_bounds has " +
			                            std::to_string(sum_list.size()) + " entries");
		}
		for (auto bound : sum_list) {
			if (bound <= 0.0) {
				throw InvalidInputException("dp_sass: every dp_sum_bounds entry must be positive");
			}
		}
	}

	idx_t sum_list_idx = 0;
	for (idx_t i = 0; i < agg->expressions.size(); i++) {
		auto &aggr = agg->expressions[i]->Cast<BoundAggregateExpression>();
		string name = StringUtil::Lower(aggr.function.name);
		if (FindAvgInfoForSumPos(avg_infos, i)) {
			RequireDpSassSumBound(context, bounds);
			bounds.sum_bounds_by_aggregate[i] = bounds.sum_bound;
		} else if (FindAvgInfoForCountPos(avg_infos, i)) {
			RequireDpSassCountBound(context, bounds, false);
		} else if (name == "count" || name == "count_star") {
			RequireDpSassCountBound(context, bounds, aggr.IsDistinct());
		} else if (name == "sum") {
			if (has_sum_list) {
				bounds.sum_bounds_by_aggregate[i] = sum_list[sum_list_idx++];
			} else {
				RequireDpSassSumBound(context, bounds);
				bounds.sum_bounds_by_aggregate[i] = bounds.sum_bound;
			}
		} else if (name == "avg") {
			RequireDpSassCountBound(context, bounds, false);
			RequireDpSassSumBound(context, bounds);
		}
	}
	return bounds;
}

static void RewriteDistinctCountToSampleMedian(OptimizerExtensionInput &input, LogicalAggregate *agg,
                                               unique_ptr<Expression> pu_hash_expr, int64_t count_bound,
                                               int64_t group_bound, bool apply_support_filter, bool variable_m) {
	auto &binder = input.optimizer.binder;
	auto &old_aggr = agg->expressions[0]->Cast<BoundAggregateExpression>();
	auto distinct_value = old_aggr.children[0]->Copy();
	auto distinct_type = distinct_value->return_type;

	idx_t inner_group_index = binder.GenerateTableIndex();
	idx_t inner_agg_index = binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> inner_expressions;
	if (!variable_m) {
		vector<unique_ptr<Expression>> mask_children;
		mask_children.push_back(pu_hash_expr->Copy());
		auto sample_mask = BindScalarLocal(input, "dp_sample_mask", std::move(mask_children));
		auto bit_or_expr = BindBitOrAggregate(input, std::move(sample_mask), distinct_value->Copy());
		inner_expressions.push_back(std::move(bit_or_expr));
	}
	auto inner_agg = make_uniq<LogicalAggregate>(inner_group_index, inner_agg_index, std::move(inner_expressions));

	idx_t num_original_groups = agg->groups.size();
	for (auto &group : agg->groups) {
		inner_agg->groups.push_back(group->Copy());
	}
	inner_agg->groups.push_back(std::move(pu_hash_expr));
	inner_agg->groups.push_back(std::move(distinct_value));
	inner_agg->children.push_back(std::move(agg->children[0]));
	inner_agg->ResolveOperatorTypes();

	auto *inner_agg_ptr = inner_agg.get();
	idx_t inner_gi = inner_agg_ptr->group_index;
	unique_ptr<LogicalOperator> distinct_rows = std::move(inner_agg);

	// Each inner row is one (PU, group, distinct value). Bound a PU's contribution with two
	// independent caps so neither becomes a large per-partition top-K (which would OOM):
	//   (1) L0 — at most `group_bound` distinct groups per PU (dense-rank over the group columns
	//       within each PU; only meaningful for grouped queries), and
	//   (2) per-cell — at most `count_bound` distinct values per (PU, group) (row-number over the
	//       distinct value within each (PU, group) partition; cap capacity = count_bound).
	// This mirrors dp_standard's factored clipping rather than a single count_bound*group_bound cap.
	idx_t pu_group_col = num_original_groups;
	idx_t distinct_col = num_original_groups + 1;
	vector<RankCapSpec> caps;
	if (num_original_groups > 0 && group_bound > 0) {
		// L0: at most `group_bound` distinct groups per PU (dense-rank over the group columns).
		RankCapSpec group_cap;
		group_cap.rank_type = ExpressionType::WINDOW_RANK_DENSE;
		group_cap.partition_cols = {pu_group_col};
		group_cap.order_cols.reserve(num_original_groups);
		for (idx_t gi = 0; gi < num_original_groups; gi++) {
			group_cap.order_cols.push_back(gi);
		}
		group_cap.cap = group_bound;
		caps.push_back(std::move(group_cap));
	}
	if (count_bound > 0) {
		// Per-cell: at most `count_bound` distinct values per (PU, group) (row-number, cap = count_bound).
		RankCapSpec value_cap;
		value_cap.rank_type = ExpressionType::WINDOW_ROW_NUMBER;
		value_cap.partition_cols.reserve(num_original_groups + 1);
		value_cap.partition_cols.push_back(pu_group_col);
		for (idx_t gi = 0; gi < num_original_groups; gi++) {
			value_cap.partition_cols.push_back(gi);
		}
		value_cap.order_cols = {distinct_col};
		value_cap.keep_null_cols = {distinct_col};
		value_cap.cap = count_bound;
		caps.push_back(std::move(value_cap));
	}
	distinct_rows = BuildRankCapFilter(input, std::move(distinct_rows), inner_agg_ptr, inner_gi, caps);

	optional_idx support_window_index;
	if (apply_support_filter) {
		idx_t window_index = binder.GenerateTableIndex();
		auto window = make_uniq<LogicalWindow>(window_index);
		auto rn =
		    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT, nullptr, nullptr);
		for (idx_t gi = 0; gi < num_original_groups; gi++) {
			rn->partitions.push_back(BuildLowerGroupRef(inner_agg_ptr, inner_gi, gi));
		}
		rn->partitions.push_back(BuildLowerGroupRef(inner_agg_ptr, inner_gi, pu_group_col));
		vector<unique_ptr<Expression>> distinct_hash_children;
		distinct_hash_children.push_back(BuildLowerGroupRef(inner_agg_ptr, inner_gi, distinct_col));
		auto distinct_hash = BindScalarLocal(input, "hash", std::move(distinct_hash_children));
		rn->orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_LAST, std::move(distinct_hash));
		rn->start = WindowBoundary::UNBOUNDED_PRECEDING;
		rn->end = WindowBoundary::CURRENT_ROW_ROWS;
		window->expressions.push_back(std::move(rn));
		window->children.push_back(std::move(distinct_rows));
		distinct_rows = std::move(window);
		support_window_index = optional_idx(window_index);
	}

	if (variable_m) {
		idx_t lane_group_index = binder.GenerateTableIndex();
		idx_t lane_agg_index = binder.GenerateTableIndex();
		vector<unique_ptr<Expression>> lane_expressions;
		if (apply_support_filter) {
			auto rn_ref = make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT,
			                                                  ColumnBinding(support_window_index.GetIndex(), 0));
			auto is_first = make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(rn_ref),
			                                                     make_uniq<BoundConstantExpression>(Value::BIGINT(1)));
			auto marker = make_uniq<BoundCaseExpression>(LogicalType::BIGINT);
			BoundCaseCheck check;
			check.when_expr = std::move(is_first);
			check.then_expr = make_uniq<BoundConstantExpression>(Value::BIGINT(1));
			marker->case_checks.push_back(std::move(check));
			marker->else_expr = make_uniq<BoundConstantExpression>(Value::BIGINT(0));
			lane_expressions.push_back(BindPlainAggregate(input, "sum", std::move(marker)));
		}

		auto lane_agg = make_uniq<LogicalAggregate>(lane_group_index, lane_agg_index, std::move(lane_expressions));
		for (idx_t gi = 0; gi < num_original_groups; gi++) {
			lane_agg->groups.push_back(make_uniq<BoundColumnRefExpression>(agg->groups[gi]->return_type,
			                                                               ColumnBinding(inner_group_index, gi)));
		}
		auto lane_source = BuildLowerGroupRef(inner_agg_ptr, inner_gi, pu_group_col);
		auto lane_mask =
		    make_uniq<BoundConstantExpression>(Value::UBIGINT(static_cast<uint64_t>(GetDpSassM(input.context) - 1)));
		lane_agg->groups.push_back(
		    input.optimizer.BindScalarFunction("&", std::move(lane_source), std::move(lane_mask)));
		lane_agg->groups.push_back(
		    make_uniq<BoundColumnRefExpression>(distinct_type, ColumnBinding(inner_group_index, distinct_col)));
		lane_agg->children.push_back(std::move(distinct_rows));
		lane_agg->ResolveOperatorTypes();

		for (idx_t gi = 0; gi < num_original_groups; gi++) {
			agg->groups[gi] =
			    make_uniq<BoundColumnRefExpression>(lane_agg->types[gi], ColumnBinding(lane_group_index, gi));
		}
		auto lane_ref = make_uniq<BoundColumnRefExpression>(lane_agg->types[num_original_groups],
		                                                    ColumnBinding(lane_group_index, num_original_groups));
		auto distinct_ref = make_uniq<BoundColumnRefExpression>(
		    lane_agg->types[num_original_groups + 1], ColumnBinding(lane_group_index, num_original_groups + 1));
		LogicalType support_type;
		if (apply_support_filter) {
			support_type = lane_agg->types[lane_agg->groups.size()];
		}
		agg->children[0] = std::move(lane_agg);

		vector<unique_ptr<Expression>> top_expressions;
		vector<unique_ptr<Expression>> count_children;
		count_children.push_back(std::move(lane_ref));
		count_children.push_back(BuildIsNotNullPredicate(std::move(distinct_ref)));
		top_expressions.push_back(
		    BindAggregateLocal(input, "as_sample_m_count_distinct_if", std::move(count_children)));
		if (apply_support_filter) {
			auto support_ref = make_uniq<BoundColumnRefExpression>(support_type, ColumnBinding(lane_agg_index, 0));
			top_expressions.push_back(BindPlainAggregate(input, "sum", std::move(support_ref)));
		}
		agg->expressions = std::move(top_expressions);
		agg->ResolveOperatorTypes();
		return;
	}

	idx_t merge_group_index = binder.GenerateTableIndex();
	idx_t merge_agg_index = binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> merge_expressions;
	auto mask_ref = make_uniq<BoundColumnRefExpression>(LogicalType::UBIGINT, ColumnBinding(inner_agg_index, 0));
	merge_expressions.push_back(BindPlainAggregate(input, "bit_or", std::move(mask_ref)));
	if (apply_support_filter) {
		auto rn_ref =
		    make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT, ColumnBinding(support_window_index.GetIndex(), 0));
		auto is_first = make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(rn_ref),
		                                                     make_uniq<BoundConstantExpression>(Value::BIGINT(1)));
		auto marker = make_uniq<BoundCaseExpression>(LogicalType::BIGINT);
		BoundCaseCheck check;
		check.when_expr = std::move(is_first);
		check.then_expr = make_uniq<BoundConstantExpression>(Value::BIGINT(1));
		marker->case_checks.push_back(std::move(check));
		marker->else_expr = make_uniq<BoundConstantExpression>(Value::BIGINT(0));
		merge_expressions.push_back(BindPlainAggregate(input, "sum", std::move(marker)));
	}
	auto merge_agg = make_uniq<LogicalAggregate>(merge_group_index, merge_agg_index, std::move(merge_expressions));
	for (idx_t gi = 0; gi < num_original_groups; gi++) {
		merge_agg->groups.push_back(
		    make_uniq<BoundColumnRefExpression>(agg->groups[gi]->return_type, ColumnBinding(inner_group_index, gi)));
	}
	merge_agg->groups.push_back(
	    make_uniq<BoundColumnRefExpression>(distinct_type, ColumnBinding(inner_group_index, num_original_groups + 1)));
	merge_agg->children.push_back(std::move(distinct_rows));
	merge_agg->ResolveOperatorTypes();

	idx_t distinct_proj_index = binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> distinct_proj_exprs;
	distinct_proj_exprs.reserve(num_original_groups + 1 + (apply_support_filter ? 1 : 0));
	for (idx_t gi = 0; gi < num_original_groups; gi++) {
		distinct_proj_exprs.push_back(
		    make_uniq<BoundColumnRefExpression>(merge_agg->types[gi], ColumnBinding(merge_group_index, gi)));
	}
	if (apply_support_filter) {
		distinct_proj_exprs.push_back(make_uniq<BoundColumnRefExpression>(
		    merge_agg->types[merge_agg->groups.size() + 1], ColumnBinding(merge_agg_index, 1)));
	}
	distinct_proj_exprs.push_back(make_uniq<BoundColumnRefExpression>(merge_agg->types[merge_agg->groups.size()],
	                                                                  ColumnBinding(merge_agg_index, 0)));
	auto distinct_proj = make_uniq<LogicalProjection>(distinct_proj_index, std::move(distinct_proj_exprs));
	distinct_proj->children.push_back(std::move(merge_agg));
	distinct_proj->ResolveOperatorTypes();
	unique_ptr<LogicalOperator> final_input = std::move(distinct_proj);

	for (idx_t gi = 0; gi < num_original_groups; gi++) {
		agg->groups[gi] =
		    make_uniq<BoundColumnRefExpression>(final_input->types[gi], ColumnBinding(distinct_proj_index, gi));
	}

	idx_t support_proj_pos = num_original_groups;
	idx_t mask_proj_pos = apply_support_filter ? num_original_groups + 1 : num_original_groups;
	auto merged_mask_ref =
	    make_uniq<BoundColumnRefExpression>(LogicalType::UBIGINT, ColumnBinding(distinct_proj_index, mask_proj_pos));
	agg->children[0] = std::move(final_input);
	vector<unique_ptr<Expression>> top_expressions;
	// expr[0] = the 64-counter mask, released via the smooth-sensitivity median. When partition
	// selection is active, expr[1] = the per-group COUNT(DISTINCT pu) support (sum of the per-value
	// markers), which the caller noises (Laplace C_u/ε_η) and τ-thresholds via ApplySupportFilter —
	// the same projection-based path the generic aggregates use, so the noise is keyed on the group.
	top_expressions.push_back(BindPlainAggregate(input, "as_sample_count_mask", std::move(merged_mask_ref)));
	if (apply_support_filter) {
		auto support_ref = make_uniq<BoundColumnRefExpression>(agg->children[0]->types[support_proj_pos],
		                                                       ColumnBinding(distinct_proj_index, support_proj_pos));
		top_expressions.push_back(BindPlainAggregate(input, "sum", std::move(support_ref)));
	}
	agg->expressions = std::move(top_expressions);
	agg->ResolveOperatorTypes();
}

static void RewritePuDistinctCountToSampleMedian(OptimizerExtensionInput &input, LogicalAggregate *agg,
                                                 unique_ptr<Expression> pu_hash_expr, bool variable_m) {
	auto &binder = input.optimizer.binder;
	auto &old_aggr = agg->expressions[0]->Cast<BoundAggregateExpression>();
	auto distinct_value = old_aggr.children[0]->Copy();
	auto distinct_type = distinct_value->return_type;

	idx_t inner_group_index = binder.GenerateTableIndex();
	idx_t inner_agg_index = binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> inner_expressions;
	auto inner_agg = make_uniq<LogicalAggregate>(inner_group_index, inner_agg_index, std::move(inner_expressions));
	inner_agg->groups.push_back(std::move(pu_hash_expr));
	inner_agg->groups.push_back(std::move(distinct_value));
	inner_agg->children.push_back(std::move(agg->children[0]));
	inner_agg->ResolveOperatorTypes();

	auto hash_ref = make_uniq<BoundColumnRefExpression>(LogicalType::UBIGINT, ColumnBinding(inner_group_index, 0));
	auto value_ref = make_uniq<BoundColumnRefExpression>(distinct_type, ColumnBinding(inner_group_index, 1));
	vector<unique_ptr<Expression>> top_expressions;
	vector<unique_ptr<Expression>> count_children;
	count_children.push_back(std::move(hash_ref));
	count_children.push_back(std::move(value_ref));
	top_expressions.push_back(
	    BindAggregateLocal(input, variable_m ? "as_sample_m_count" : "as_sample_count", std::move(count_children)));

	agg->children[0] = std::move(inner_agg);
	agg->expressions = std::move(top_expressions);
	agg->ResolveOperatorTypes();
	PRIVACY_DEBUG_PRINT("[DP_SAMPLE_MEDIAN] specialized COUNT(DISTINCT PU key) -> distinct PU rows + " +
	                    string(variable_m ? "as_sample_m_count" : "as_sample_count"));
}

static void RemapExpressionBindingsLocal(Expression &e, const std::unordered_map<idx_t, idx_t> &map) {
	if (e.type == ExpressionType::BOUND_COLUMN_REF) {
		auto &col_ref = e.Cast<BoundColumnRefExpression>();
		auto it = map.find(col_ref.binding.table_index);
		if (it != map.end()) {
			col_ref.binding.table_index = it->second;
		}
	}
	ExpressionIterator::EnumerateChildren(e, [&](Expression &child) { RemapExpressionBindingsLocal(child, map); });
}

static unique_ptr<Expression> CopyMaybeRemapped(const Expression &expr,
                                                const std::unordered_map<idx_t, idx_t> *index_map) {
	auto result = expr.Copy();
	if (index_map) {
		RemapExpressionBindingsLocal(*result, *index_map);
	}
	return result;
}

static unique_ptr<Expression> CopyFilterMaybeRemapped(const BoundAggregateExpression &aggr,
                                                      const std::unordered_map<idx_t, idx_t> *index_map) {
	if (!aggr.filter) {
		return nullptr;
	}
	return CopyMaybeRemapped(*aggr.filter, index_map);
}

struct SampleAggregateRewrite {
	string sample_name;
	idx_t lower_value_pos;
	optional_idx lower_count_pos;
	bool clip_value;
	double value_lower;
	double value_upper;
	bool clip_count;
	double count_upper;
	bool raw_lower_value;
};

static void BuildSampleMedianLowerExpression(OptimizerExtensionInput &input, const BoundAggregateExpression &aggr,
                                             const std::unordered_map<idx_t, idx_t> *index_map,
                                             const DpSassContributionBounds &bounds,
                                             vector<unique_ptr<Expression>> &lower_expressions, idx_t aggregate_pos,
                                             vector<SampleAggregateRewrite> &rewrites, bool variable_m,
                                             bool nullable_sum, const Expression *extra_filter = nullptr) {
	string name = StringUtil::Lower(aggr.function.name);
	string sample_sum_name = nullable_sum ? (variable_m ? "as_sample_m_nullable_sum" : "as_sample_nullable_sum")
	                                      : (variable_m ? "as_sample_m_sum" : "as_sample_sum");
	string sample_avg_name = variable_m ? "as_sample_m_avg" : "as_sample_avg";
	string sample_min_name = variable_m ? "as_sample_m_min" : "as_sample_min";
	string sample_max_name = variable_m ? "as_sample_m_max" : "as_sample_max";
	vector<unique_ptr<Expression>> children;
	idx_t lower_value_pos = lower_expressions.size();
	if (name == "count_star") {
		lower_expressions.push_back(BindPlainAggregate(input, "count_star", std::move(children)));
		rewrites.push_back(
		    {sample_sum_name, lower_value_pos, optional_idx(), true, 0.0, bounds.count_bound, false, 0.0, false});
	} else if (name == "count") {
		if (!aggr.children.empty()) {
			children.push_back(CopyMaybeRemapped(*aggr.children[0], index_map));
		}
		if (variable_m && aggr.IsDistinct()) {
			lower_expressions.push_back(
			    BindPlainAggregate(input, "list", std::move(children), AggregateType::DISTINCT));
			vector<unique_ptr<Expression>> predicates;
			predicates.push_back(CopyFilterMaybeRemapped(aggr, index_map));
			if (extra_filter) {
				predicates.push_back(extra_filter->Copy());
			}
			lower_expressions.back()->Cast<BoundAggregateExpression>().filter =
			    ConjoinPredicates(std::move(predicates));
			rewrites.push_back({"as_sample_m_count_distinct_values", lower_value_pos, optional_idx(), false, 0.0, 0.0,
			                    false, 0.0, true});
			return;
		} else {
			lower_expressions.push_back(
			    BindPlainAggregate(input, "count", std::move(children),
			                       aggr.IsDistinct() ? AggregateType::DISTINCT : AggregateType::NON_DISTINCT));
			rewrites.push_back(
			    {sample_sum_name, lower_value_pos, optional_idx(), true, 0.0, bounds.count_bound, false, 0.0, false});
		}
	} else if (name == "sum") {
		children.push_back(CopyMaybeRemapped(*aggr.children[0], index_map));
		lower_expressions.push_back(BindPlainAggregate(input, "sum", std::move(children)));
		double sum_bound = GetDpSassSumContributionBound(bounds, aggregate_pos);
		rewrites.push_back(
		    {sample_sum_name, lower_value_pos, optional_idx(), true, -sum_bound, sum_bound, false, 0.0, false});
	} else if (name == "avg") {
		auto sum_arg = CopyMaybeRemapped(*aggr.children[0], index_map);
		auto count_arg = CopyMaybeRemapped(*aggr.children[0], index_map);
		auto filter_for_sum = CopyFilterMaybeRemapped(aggr, index_map);
		auto filter_for_count = CopyFilterMaybeRemapped(aggr, index_map);

		lower_expressions.push_back(BindPlainAggregate(input, "sum", std::move(sum_arg)));
		lower_expressions.back()->Cast<BoundAggregateExpression>().filter = std::move(filter_for_sum);

		idx_t lower_count_pos = lower_expressions.size();
		lower_expressions.push_back(BindPlainAggregate(input, "count", std::move(count_arg)));
		lower_expressions.back()->Cast<BoundAggregateExpression>().filter = std::move(filter_for_count);
		rewrites.push_back({sample_avg_name, lower_value_pos, optional_idx(lower_count_pos), true, -bounds.sum_bound,
		                    bounds.sum_bound, true, bounds.count_bound, false});
		return;
	} else if (name == "min" || name == "max") {
		// Per-PU MIN/MAX of the value; per-lane sample MIN/MAX then median/mean release. No per-lane
		// magnitude clip — the release terminal already clamps each lane answer to the public
		// [dp_sass_minmax_lower_bound, dp_sass_minmax_upper_bound] output domain.
		children.push_back(CopyMaybeRemapped(*aggr.children[0], index_map));
		lower_expressions.push_back(BindPlainAggregate(input, name, std::move(children)));
		rewrites.push_back({name == "min" ? sample_min_name : sample_max_name, lower_value_pos, optional_idx(), false,
		                    0.0, 0.0, false, 0.0, false});
	} else {
		throw InvalidInputException("dp_sample_median: only COUNT, COUNT(DISTINCT), SUM, AVG, MIN, and MAX aggregates "
		                            "are supported");
	}
	lower_expressions.back()->Cast<BoundAggregateExpression>().filter = CopyFilterMaybeRemapped(aggr, index_map);
}

static unique_ptr<Expression> BuildSampleMedianAggregateRef(OptimizerExtensionInput &input,
                                                            const PuPreAggregationInfo &pre_agg,
                                                            const SampleAggregateRewrite &rewrite) {
	auto lower_type = pre_agg.lower_agg->types[pre_agg.num_original_groups + 1 + rewrite.lower_value_pos];
	unique_ptr<Expression> lower_ref = make_uniq<BoundColumnRefExpression>(
	    lower_type, ColumnBinding(pre_agg.lower_agg_index, rewrite.lower_value_pos));
	if (!rewrite.raw_lower_value) {
		lower_ref = BoundCastExpression::AddCastToType(input.context, std::move(lower_ref), LogicalType::DOUBLE);
		if (rewrite.clip_value) {
			lower_ref = ClipToBounds(input, std::move(lower_ref), rewrite.value_lower, rewrite.value_upper,
			                         LogicalType::DOUBLE);
		}
	}

	vector<unique_ptr<Expression>> children;
	children.push_back(pre_agg.pu_hash_ref->Copy());
	children.push_back(std::move(lower_ref));
	if (rewrite.lower_count_pos.IsValid()) {
		auto lower_count_type =
		    pre_agg.lower_agg->types[pre_agg.num_original_groups + 1 + rewrite.lower_count_pos.GetIndex()];
		unique_ptr<Expression> lower_count_ref = make_uniq<BoundColumnRefExpression>(
		    lower_count_type, ColumnBinding(pre_agg.lower_agg_index, rewrite.lower_count_pos.GetIndex()));
		auto count_ref =
		    BoundCastExpression::AddCastToType(input.context, std::move(lower_count_ref), LogicalType::DOUBLE);
		if (rewrite.clip_count) {
			count_ref = ClipToBounds(input, std::move(count_ref), 0.0, rewrite.count_upper, LogicalType::DOUBLE);
		}
		children.push_back(std::move(count_ref));
	}
	return BindAggregateLocal(input, rewrite.sample_name, std::move(children));
}

static void RewriteAggregateToSampleMedian(OptimizerExtensionInput &input, LogicalAggregate *agg,
                                           unique_ptr<Expression> pu_hash_expr, const DpSassContributionBounds &bounds,
                                           bool apply_distinct_support_filter, bool distinct_count_is_pu_key = false,
                                           bool variable_m = false,
                                           const vector<bool> *nullable_sum_outputs = nullptr) {
	if (IsDpSampleCountDistinct(agg)) {
		if (distinct_count_is_pu_key) {
			RewritePuDistinctCountToSampleMedian(input, agg, std::move(pu_hash_expr), variable_m);
			return;
		}
		D_ASSERT(bounds.has_integer_count_bound);
		int64_t count_bound = bounds.count_bound_integer;
		int64_t group_bound =
		    !agg->groups.empty() ? GetRequiredDpIntegerBound(input.context, "dp_max_groups_contributed", "dp_sass") : 1;
		RewriteDistinctCountToSampleMedian(input, agg, std::move(pu_hash_expr), count_bound, group_bound,
		                                   apply_distinct_support_filter, variable_m);
		return;
	}

	vector<unique_ptr<Expression>> lower_expressions;
	lower_expressions.reserve(agg->expressions.size());
	vector<SampleAggregateRewrite> rewrites;
	rewrites.reserve(agg->expressions.size());
	vector<unique_ptr<Expression>> variable_m_distinct_filters;
	bool has_variable_m_distinct = false;
	for (auto &expr : agg->expressions) {
		auto &aggr = expr->Cast<BoundAggregateExpression>();
		string name = StringUtil::Lower(aggr.function.name);
		if (name == "count" && aggr.IsDistinct()) {
			has_variable_m_distinct = true;
			break;
		}
	}
	if (variable_m && has_variable_m_distinct) {
		D_ASSERT(bounds.has_integer_count_bound);
		variable_m_distinct_filters =
		    PrepareVariableMDistinctListFilters(input, agg, *pu_hash_expr, bounds.count_bound_integer);
	}
	for (idx_t ai = 0; ai < agg->expressions.size(); ai++) {
		auto &expr = agg->expressions[ai];
		auto &aggr = expr->Cast<BoundAggregateExpression>();
		const Expression *extra_filter =
		    ai < variable_m_distinct_filters.size() ? variable_m_distinct_filters[ai].get() : nullptr;
		bool nullable_sum = nullable_sum_outputs && ai < nullable_sum_outputs->size() && (*nullable_sum_outputs)[ai];
		BuildSampleMedianLowerExpression(input, aggr, nullptr, bounds, lower_expressions, ai, rewrites, variable_m,
		                                 nullable_sum, extra_filter);
	}

	auto pre_agg = InsertPuPreAggregation(input, agg, std::move(lower_expressions), std::move(pu_hash_expr));
	int64_t group_bound =
	    !agg->groups.empty() ? GetRequiredDpIntegerBound(input.context, "dp_max_groups_contributed", "dp_sass") : 1;
	ApplyMaxGroupsContributed(input, agg, pre_agg, group_bound);

	for (idx_t ai = 0; ai < agg->expressions.size(); ai++) {
		auto &rewrite = rewrites[ai];
		auto &original = agg->expressions[ai]->Cast<BoundAggregateExpression>();
		PRIVACY_DEBUG_PRINT("[DP_SAMPLE_MEDIAN] rewrite " + original.function.name + " -> " + rewrite.sample_name);
		agg->expressions[ai] = BuildSampleMedianAggregateRef(input, pre_agg, rewrite);
	}
	agg->ResolveOperatorTypes();
}

static unique_ptr<Expression>
BuildStableDpSassPuHashExpression(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                                  LogicalAggregate *agg, const PrivacyCompatibilityResult &check,
                                  const vector<string> &privacy_units, const PrivPUHashingSetup &pu_setup,
                                  bool allow_self_joins) {
	if (pu_setup.pu_names.size() != 1) {
		return nullptr;
	}
	const auto &pu_table_name = pu_setup.pu_names[0];
	auto meta_it = check.table_metadata.find(pu_table_name);
	if (meta_it == check.table_metadata.end() || meta_it->second.pks.empty()) {
		throw InternalException("dp_sass: PU table '" + pu_table_name + "' has no PRIVACY_KEY metadata");
	}

	LogicalGet *key_get = FindAccessibleGetInSubtree(plan, agg, pu_table_name);
	vector<string> key_columns = meta_it->second.pks;
	if (!key_get) {
		vector<LogicalGet *> gets;
		FindAllGetNodes(plan.get(), gets);
		auto chain = ExtractFKChain(check, gets, privacy_units, /*include_fk_cols=*/true, allow_self_joins);
		if (chain.tables.size() >= 2 && !chain.fk_cols.empty()) {
			const string &linked_table = chain.tables[chain.tables.size() - 2];
			key_get = FindAccessibleGetInSubtree(plan, agg, linked_table);
			if (key_get) {
				key_columns = {chain.fk_cols.back()};
			}
		}
	}
	if (!key_get) {
		return nullptr;
	}

	vector<unique_ptr<Expression>> key_exprs;
	key_exprs.reserve(key_columns.size());
	for (auto &key_column : key_columns) {
		idx_t proj_idx = EnsureProjectedColumn(*key_get, key_column);
		if (proj_idx == DConstants::INVALID_INDEX) {
			throw InternalException("dp_sass: failed to project PU key column '" + key_column + "'");
		}
		auto col_index = key_get->GetColumnIds()[proj_idx];
		auto col_type = key_get->GetColumnType(col_index);
		ColumnBinding source_binding(key_get->table_index, proj_idx);
		auto propagated = PropagateSingleBinding(*plan, key_get->table_index, source_binding, col_type, agg);
		if (propagated.table_index == DConstants::INVALID_INDEX) {
			return nullptr;
		}
		key_exprs.push_back(make_uniq<BoundColumnRefExpression>(col_type, propagated));
	}
	auto pu_hash_expr = BuildXorHash(input, std::move(key_exprs));
	pu_hash_expr = BoundCastExpression::AddCastToType(input.context, std::move(pu_hash_expr), LogicalType::UBIGINT);
	pu_hash_expr->alias = "dp_sass_lane_key";
	PRIVACY_DEBUG_PRINT("[DP_SAMPLE_MEDIAN] using stable hash-based sample lanes");
	return pu_hash_expr;
}

// ----------------------------------------------------------------------------
// Entry point
// ----------------------------------------------------------------------------

// Per-mode clipping + sensitivity. Returns one sensitivity per DP aggregate (in agg->expressions
// order, before any support aggregate is appended) and sets `per_pu` when a per-PU
// pre-aggregation was inserted (so the support aggregate counts PU groups).
static vector<double> ClipAndComputeSensitivities(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                                                  LogicalAggregate *agg, const PrivacyCompatibilityResult &check,
                                                  const DPFKChain &chain, const string &mech, DPSensitivityKind kind,
                                                  const vector<AvgInfo> &avg_infos, double epsilon, bool &per_pu,
                                                  double self_join_factor) {
	if (kind == DPSensitivityKind::GLOBAL) {
		// Standard DP: per-PU contribution clipping → sensitivity = the bound (data-independent).
		return ApplyPerPuClipping(input, plan, agg, check, chain, mech, avg_infos, per_pu);
	}
	// Elastic DP: per-row clipping + smooth elastic sensitivity from the join max-frequencies.
	per_pu = false;
	vector<double> sum_bounds(agg->expressions.size(), 0.0);
	if (AggregateContainsSum(agg)) {
		sum_bounds = GetDpSumContributionBounds(input.context, mech, agg, avg_infos, false, 0.0);
		ClipSumInputs(input, agg, sum_bounds);
	}
	double es = ComputeElasticSensitivity(input.context, chain, epsilon);
	es *= self_join_factor;
	vector<double> sens;
	sens.reserve(agg->expressions.size());
	for (idx_t i = 0; i < agg->expressions.size(); i++) {
		sens.push_back(ElasticSensitivity(i, agg->expressions[i]->Cast<BoundAggregateExpression>(), sum_bounds, es));
	}
	return sens;
}

// Shared finalize tail for both Laplace DP modes: append the admin-provided support aggregate,
// suppress low-support groups when privacy_min_group_count is set, split ε across aggregates,
// inject Laplace noise, and (for AVG) layer the ratio projection. `agg_sens` carries one raw
// sensitivity per DP aggregate; `per_pu` reflects whether the input was per-PU pre-aggregated.
static void FinalizeDPLaplace(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan, LogicalAggregate *agg,
                              const DPFKChain &chain, const string &mech, const vector<AvgInfo> &avg_infos,
                              const vector<double> &agg_sens, const vector<LogicalType> &output_types,
                              idx_t n_original_aggs, idx_t n_groups, double epsilon, bool noise_enabled, bool per_pu,
                              bool allow_group_suppression) {
	// Partition selection. The released set of group keys is data-dependent, so a grouped query must
	// privatize it (Google DP / Wilson et al.): noise the per-group COUNT(DISTINCT pu) — the
	// AddSupportAggregate output — and keep groups whose noisy η' clears τ. Partitions are private by
	// default, matching dp_sass (no public-partition opt-out in any mode).
	//   - dp_standard: automatic for every grouped query, with Laplace(C_u/ε_η) noise and the computed
	//     τ = 1 − C_u·log(2 − 2(1−δ_η)^{1/C_u})/ε_η. This needs δ_η, so grouped dp_standard is (ε,δ)-DP
	//     (ungrouped stays pure ε). ε_η = ε/(c+1) is reserved from the budget.
	//   - dp_elastic: still on the legacy opt-in raw threshold here; its private auto-threshold needs
	//     the value channel's (ε,δ) to be split against τ first (pending follow-up).
	double k = static_cast<double>(n_original_aggs);
	bool apply_support_filter = false;
	double tau = 0.0;
	double support_noise_scale = 0.0;
	double support_gate = 0.0; // positive → AddSupportAggregate appends the η aggregate
	double budget_units = k;   // ε split across the k user aggregates; +1 reserves ε_η for η
	if (mech == "dp_standard") {
		apply_support_filter = allow_group_suppression && n_groups > 0;
		if (apply_support_filter) {
			double delta = 0.0;
			if (!TryGetDpDelta(input.context, delta) || !std::isfinite(delta) || delta <= 0.0 || delta >= 0.5) {
				throw InvalidInputException(
				    "dp_standard: grouped queries require dp_delta in (0, 0.5) for DP partition selection (the "
				    "released set of group keys is data-dependent). Ungrouped dp_standard queries remain pure ε-DP.");
			}
			// C_u = max output groups one PU can affect. This is required for every grouped dp_standard
			// query, including single-table PU queries where repeated PRIVACY_KEY values may span groups.
			double cu =
			    static_cast<double>(GetRequiredDpIntegerBound(input.context, "dp_max_groups_contributed", mech));
			double eps_eta = epsilon / (k + 1.0);
			double delta_eta = delta / (k + 1.0);
			tau = ComputeWilsonPartitionThreshold(eps_eta, delta_eta, cu);
			double manual_threshold = 0.0;
			if (TryGetPrivacyMinGroupCount(input.context, manual_threshold)) {
				tau = std::max(tau, manual_threshold);
			}
			support_noise_scale = noise_enabled ? (cu / eps_eta) : 0.0;
			support_gate = 1.0;
			budget_units = k + 1.0;
			PRIVACY_DEBUG_PRINT("[" + mech + "] partition selection: eps_eta=" + std::to_string(eps_eta) +
			                    " tau=" + std::to_string(tau) + " noise_scale=" + std::to_string(support_noise_scale));
		}
	} else {
		// dp_elastic: row-level FLEX baseline (public/enumerated partitions). privacy_min_group_count
		// here is a RAW, un-noised admin support filter — NOT DP partition selection. Under FLEX's
		// public-partition assumption it's a utility convenience; a row-level-DP-safe version would need
		// a noised τ with C_u = 1 (the row-level branch). Deferred — dp_elastic is the FLEX baseline.
		double support_threshold = 0.0;
		bool want_support = TryGetPrivacyMinGroupCount(input.context, support_threshold);
		apply_support_filter = want_support && allow_group_suppression;
		tau = support_threshold;
		support_gate = support_threshold; // raw threshold, no noise
	}
	auto support_pos = AddSupportAggregate(input, plan, agg, chain, apply_support_filter ? support_gate : 0.0, per_pu);
	idx_t n_dp_aggs = support_pos.IsValid() ? support_pos.GetIndex() : agg->expressions.size();
	LogicalOperator *noise_anchor = agg;
	if (apply_support_filter) {
		noise_anchor = ApplySupportFilter(input, plan, agg, support_pos.GetIndex(), tau, support_noise_scale);
	}

	auto avg_components = BuildAvgComponentSet(avg_infos);

	// Build per-aggregate Laplace scales. The ε budget is split across `budget_units` cells: the k
	// user-visible aggregates, plus 1 for the partition-selection count η when it is active. So each
	// aggregate gets ε/budget_units. AVG splits its share again into half per component (SUM, COUNT).
	vector<double> agg_scales;
	agg_scales.reserve(n_dp_aggs);
	for (idx_t ai = 0; ai < n_dp_aggs; ai++) {
		double sens = agg_sens[ai];
		double scale = noise_enabled ? (sens / epsilon) : 0.0;
		if (budget_units > 1.0) {
			scale *= budget_units; // budget split across user aggregates (+ η when thresholding)
		}
		if (avg_components.count(ai)) {
			scale *= 2.0; // additional split: each AVG component uses half its allocation
		}
		agg_scales.push_back(scale);
		PRIVACY_DEBUG_PRINT("[" + mech + "] agg sensitivity=" + std::to_string(sens) +
		                    " scale=" + std::to_string(scale));
	}

	auto noise_proj = WrapAggregateWithLaplace(input, plan, agg, noise_anchor, agg_scales, output_types);

	// Wrap AVG above the noise projection so upstream operators — including HAVING
	// that references AVG — get remapped from the raw SUM to the ratio output.
	if (!avg_infos.empty()) {
		WrapAvgRatioProjection(input, plan, noise_proj, avg_infos, n_groups, n_original_aggs, noise_proj.proj_ptr,
		                       false);
	}

#if PRIVACY_DEBUG
	PRIVACY_DEBUG_PRINT("=== PLAN AFTER " + mech + " TRANSFORMATION ===");
	plan->Print();
#endif
}

// Shared Laplace-mechanism pipeline for the two additive-noise DP modes. They share the FK chain
// extraction, AVG decomposition, τ-suppression, budget split, and Laplace injection; they differ
// only in how contributions are clipped and how sensitivity is derived (see DPSensitivityKind /
// ClipAndComputeSensitivities).
static void CompileDPLaplaceQuery(const PrivacyCompatibilityResult &check, OptimizerExtensionInput &input,
                                  unique_ptr<LogicalOperator> &plan, const vector<string> &privacy_units,
                                  const string &query_hash, const string &mech, DPSensitivityKind kind) {
	(void)query_hash;
	PRIVACY_DEBUG_PRINT("[" + mech + "] CompileDPLaplaceQuery: start");

	double epsilon = GetValidatedDpEpsilon(input.context, mech);

	double self_join_factor = 1.0;
	bool allow_self_joins = false;
	if (mech == "dp_elastic") {
		self_join_factor = ValidateDPSelfJoins(plan, mech);
		allow_self_joins = self_join_factor > 1.0;
	} else if (mech == "dp_standard") {
		allow_self_joins = ValidateDPSelfJoins(plan, mech) > 1.0;
	}

	auto fk_chain = ExtractDPFKChain(plan, privacy_units, check, allow_self_joins);
	auto *agg = CheckDPAggregates(plan, mech);

	// Rewrite AVG(x) → SUM(x) + COUNT(*) before bound/clipping checks.
	// Each AVG uses ε/2 per component so the combined cost is still ε-DP.
	idx_t avg_count = CountAvgAggregates(agg);
	AvgBounds avg_bounds;
	const AvgBounds *avg_bounds_ptr = nullptr;
	if (avg_count > 0) {
		avg_bounds = GetAvgBounds(input.context, mech, avg_count, /*allow_sass_legacy=*/false);
		avg_bounds_ptr = &avg_bounds;
	}
	bool use_bounded_mean = avg_count > 0;
	auto avg_infos = RewriteAvgAggregates(input, agg, use_bounded_mean, avg_bounds_ptr);
	idx_t n_original_aggs = agg->expressions.size() - avg_infos.size();
	idx_t n_groups = agg->groups.size();

	// Capture the desired final output type of each DP aggregate *before* clipping. Standard-DP
	// per-PU clipping rewrites e.g. COUNT→SUM(clipped count), changing the type; the noise
	// projection casts back to these originals so upstream column references stay valid.
	agg->ResolveOperatorTypes();
	vector<LogicalType> output_types;
	output_types.reserve(agg->expressions.size());
	for (idx_t i = 0; i < agg->expressions.size(); i++) {
		output_types.push_back(agg->types[n_groups + i]);
	}

	// When privacy_noise=false the compilation pipeline still runs (clipping, FK chain)
	// but Laplace scale is zeroed → dp_noise returns the value unchanged.
	// This mirrors pac_mi=0 for PAC and enables deterministic testing.
	bool noise_enabled = IsPacNoiseEnabled(input.context, true);

	bool per_pu = false;
	auto agg_sens = ClipAndComputeSensitivities(input, plan, agg, check, fk_chain, mech, kind, avg_infos, epsilon,
	                                            per_pu, self_join_factor);

	FinalizeDPLaplace(input, plan, agg, fk_chain, mech, avg_infos, agg_sens, output_types, n_original_aggs, n_groups,
	                  epsilon, noise_enabled, per_pu, true);
}

void CompileDPElasticQuery(const PrivacyCompatibilityResult &check, OptimizerExtensionInput &input,
                           unique_ptr<LogicalOperator> &plan, const vector<string> &privacy_units,
                           const string &query_hash) {
	CompileDPLaplaceQuery(check, input, plan, privacy_units, query_hash, "dp_elastic",
	                      DPSensitivityKind::ELASTIC_SMOOTH);
}

void CompileDPStandardQuery(const PrivacyCompatibilityResult &check, OptimizerExtensionInput &input,
                            unique_ptr<LogicalOperator> &plan, const vector<string> &privacy_units,
                            const string &query_hash) {
	CompileDPLaplaceQuery(check, input, plan, privacy_units, query_hash, "dp_standard", DPSensitivityKind::GLOBAL);
}

void CompileDPSampleMedianQuery(const PrivacyCompatibilityResult &check, OptimizerExtensionInput &input,
                                unique_ptr<LogicalOperator> &plan, const vector<string> &privacy_units,
                                const string &query_hash) {
	(void)query_hash;
	PRIVACY_DEBUG_PRINT("[DP_SAMPLE_MEDIAN] CompileDPSampleMedianQuery: start");

	double epsilon = GetValidatedDpEpsilon(input.context, "dp_sample_median");
	string release_method = GetDpSassReleaseMethod(input.context);
	int sass_m = GetDpSassM(input.context);
	bool variable_m = sass_m > DP_SASS_DEFAULT_M;
	bool allow_self_joins = ValidateDPSelfJoins(plan, "dp_sample_median") > 1.0;
	auto fk_chain = ExtractDPFKChain(plan, privacy_units, check, allow_self_joins);
	auto *single_agg = CheckDPAggregates(plan, "dp_sass");
	if (!variable_m && TryCompileDPSassFastCountStar(input, plan, single_agg, fk_chain, epsilon)) {
		return;
	}
	if (variable_m) {
		if (GetDpSampleLanes(input.context) != 1) {
			throw InvalidInputException("dp_sass: dp_sass_m > 64 currently requires dp_sample_lanes = 1");
		}
		PRIVACY_DEBUG_PRINT("[DP_SAMPLE_MEDIAN] using experimental variable-m SAA path m=" + std::to_string(sass_m));
	}

	double delta = 0.0;

	auto pu_setup = PreparePlanForPUHashing(check, input, plan, privacy_units);
	auto *agg = single_agg;
	CheckDPAggregateNode(agg, "dp_sass", true);
	bool distinct_count_is_pu_key =
	    IsSinglePuKeyDistinctCount(plan, agg, check, privacy_units, pu_setup, allow_self_joins);
	auto pu_hash_expr =
	    BuildStableDpSassPuHashExpression(input, plan, agg, check, privacy_units, pu_setup, allow_self_joins);
	if (!pu_hash_expr) {
		throw InvalidInputException("dp_sass: could not build a stable privacy-unit hash for this query shape");
	}

	// AVG estimator selection. 'lane_average' (default) fills empty lanes with a public value before
	// averaging. 'nonempty_lane_average' releases a noisy shifted SUM and populated-lane COUNT, then
	// divides them. 'ratio' decomposes the original AVG into row-level SUM and COUNT SAA cells.
	idx_t avg_count = CountAvgAggregates(agg);
	string avg_method = GetDpSassAvgMethod(input.context);
	bool use_nonempty_lane_average = avg_method == "nonempty_lane_average" && avg_count > 0;
	if (use_nonempty_lane_average && release_method != "average") {
		throw InvalidInputException(
		    "dp_sass: dp_sass_avg_method='nonempty_lane_average' requires dp_sass_release='average'");
	}
	if (use_nonempty_lane_average) {
		PRIVACY_DEBUG_PRINT("[DP_SAMPLE_MEDIAN] AVG non-empty-lane mode: split each AVG cell budget equally "
		                    "between shifted SUM and populated-lane COUNT");
	}
	idx_t explicit_sum_count = CountExplicitSumAggregates(agg);
	string sum_method = explicit_sum_count > 0 ? GetDpSassSumMethod(input.context) : "lane_average";
	bool use_nonempty_sum_average = sum_method == "nonempty_lane_average" && explicit_sum_count > 0;
	if (use_nonempty_sum_average && release_method != "average") {
		throw InvalidInputException(
		    "dp_sass: dp_sass_sum_method='nonempty_lane_average' requires dp_sass_release='average'");
	}
	if (use_nonempty_sum_average) {
		PRIVACY_DEBUG_PRINT("[DP_SAMPLE_MEDIAN] SUM non-empty-lane mode: use the clipped per-PU SUM bound for "
		                    "numerator sensitivity and privately release populated-lane COUNT");
	}
	AvgBounds avg_bounds;
	const AvgBounds *avg_bounds_ptr = nullptr;
	if (avg_count > 0) {
		avg_bounds = GetAvgBounds(input.context, "dp_sass", avg_count, /*allow_sass_legacy=*/true);
		avg_bounds_ptr = &avg_bounds;
	}
	vector<AvgInfo> avg_infos;
	double ratio_avg_lower = 0.0;
	double ratio_avg_upper = 0.0;
	if (avg_method == "ratio" && AggregateContainsAvg(agg)) {
		// Read + validate the public AVG output domain even in ratio mode (so an inverted range is
		// still caught), and clamp the released ratio into it below — matching the lane_average
		// estimator's output domain (free DP post-processing).
		ratio_avg_lower = avg_bounds.domains[0].lower;
		ratio_avg_upper = avg_bounds.domains[0].upper;
		PRIVACY_DEBUG_PRINT("[DP_SAMPLE_MEDIAN] AVG ratio mode: decomposing AVG -> SUM + COUNT "
		                    "(two independent SAA mechanisms), ratio clamped to the public avg domain");
		avg_infos = RewriteAvgAggregates(input, agg, /*use_bounded_mean=*/false, avg_bounds_ptr);
	}
	std::unordered_set<idx_t> avg_components = BuildAvgComponentSet(avg_infos);

	// AVG→SUM+COUNT decomposition (ratio mode) appends a COUNT per AVG; the appended components are not
	// separate user aggregates, so exclude them from the budget-splitting count k (AVG stays one cell).
	idx_t n_original_aggs = agg->expressions.size() - avg_infos.size();
	idx_t n_groups = agg->groups.size();
	// The GUPT-style mean release ('average') is pure ε-DP for the release itself, but grouped
	// queries still need δ for private partition selection. The smooth-sensitivity median release
	// uses δ for both partition selection and the per-cell smooth-sensitivity release.
	if (release_method != "average" || n_groups > 0) {
		if (!TryGetDpDelta(input.context, delta)) {
			throw InvalidInputException("dp_sample_median: dp_delta must be set. Use SET dp_delta=<value> or PRAGMA "
			                            "refresh_dp_stats(<epsilon>).");
		}
		if (delta <= 0.0 || delta >= 0.5 || !std::isfinite(delta)) {
			throw InvalidInputException("dp_sample_median: dp_delta must be in (0, 0.5)");
		}
	}
	auto contribution_bounds = GetDpSassContributionBounds(input.context, agg, avg_infos);

	vector<LogicalType> output_types;
	output_types.reserve(agg->expressions.size());
	vector<double> lower_bounds;
	vector<double> upper_bounds;
	vector<double> empty_defaults; // public in-range value used to fill empty lanes (per aggregate kind)
	lower_bounds.reserve(agg->expressions.size());
	upper_bounds.reserve(agg->expressions.size());
	empty_defaults.reserve(agg->expressions.size());
	double k = static_cast<double>(n_original_aggs);
	// Cross-group contribution bound C_u (dp_max_groups_contributed): one PU may affect up to
	// C_u output groups, each released independently. By sequential composition that costs the PU
	// C_u · (per-aggregate budget), so we split each aggregate's budget by C_u as well as by k.
	// Ungrouped queries have C_u = 1. This makes C_u enter the guarantee, not just filter rows.
	int64_t group_bound =
	    n_groups > 0 ? GetRequiredDpIntegerBound(input.context, "dp_max_groups_contributed", "dp_sass") : 1;

	// DP partition selection (Algorithm 2, SIMD-SAA^udp). The set of released group keys is itself
	// data-dependent, so for any grouped query we privatize the auxiliary η = COUNT(DISTINCT pu)
	// per group with pure ε_η-DP Laplace noise and release only groups with η' ≥ τ. No user setting
	// is needed: τ is computed from the budget. Ungrouped queries have a single, data-independent
	// output row and need no partition selection.
	//
	// Budget: reserve ε_η = ε/(c+1) (δ_η = δ/(c+1)) for partition selection, leaving each of the c
	// smooth-sensitivity cells ε' = (ε−ε_η)/(c·C_u) = ε/((c+1)·C_u). Ungrouped queries split
	// ε/(c·C_u) as before (no η).
	bool apply_support_filter = n_groups > 0;
	double tau = 0.0;
	double support_noise_scale = 0.0;
	if (apply_support_filter) {
		if (!(delta > 0.0)) {
			throw InvalidInputException(
			    "dp_sass: grouped queries require dp_delta > 0 for DP partition selection (the released set of "
			    "group keys is data-dependent). The pure-ε 'average' release cannot privately threshold an "
			    "unbounded group domain — set dp_delta, or remove the GROUP BY.");
		}
		double eps_eta = epsilon / (k + 1.0);
		double delta_eta = delta / (k + 1.0);
		double cu = static_cast<double>(group_bound);
		// Google DP / Wilson et al. threshold: P[releasing a group backed by a single PU] ≤ δ_η.
		tau = ComputeWilsonPartitionThreshold(eps_eta, delta_eta, cu);
		double manual_threshold = 0.0;
		if (TryGetPrivacyMinGroupCount(input.context, manual_threshold)) {
			tau = std::max(tau, manual_threshold);
		}
		bool noise_enabled = GetBooleanSetting(input.context, "privacy_noise", true);
		// One PU changes the per-group distinct count in up to C_u groups by 1 → L1 sensitivity C_u.
		support_noise_scale = noise_enabled ? (cu / eps_eta) : 0.0;
		PRIVACY_DEBUG_PRINT("[DP_SAMPLE_MEDIAN] partition selection: eps_eta=" + std::to_string(eps_eta) +
		                    " tau=" + std::to_string(tau) + " noise_scale=" + std::to_string(support_noise_scale));
	}
	double budget_divisor = (apply_support_filter ? (k + 1.0) : k) * static_cast<double>(group_bound);
	vector<double> epsilons;
	vector<double> deltas;
	epsilons.reserve(agg->expressions.size());
	deltas.reserve(agg->expressions.size());
	vector<idx_t> sample_output_positions;
	sample_output_positions.reserve(agg->expressions.size());
	vector<bool> nonempty_lane_releases;
	vector<double> nonempty_lane_baselines;
	vector<double> nonempty_lane_sensitivities;
	vector<bool> nullable_sum_outputs(agg->expressions.size(), false);
	nonempty_lane_releases.reserve(agg->expressions.size());
	nonempty_lane_baselines.reserve(agg->expressions.size());
	nonempty_lane_sensitivities.reserve(agg->expressions.size());
	double sum_lane_rescale = 1.0;
	if (GetDpSassRescale(input.context)) {
		sum_lane_rescale = variable_m ? static_cast<double>(sass_m) : DpSampleRescale(GetDpSampleLanes(input.context));
	}
	idx_t avg_output_idx = 0;
	idx_t count_count = 0;
	idx_t sum_count = 0;
	for (idx_t ai = 0; ai < agg->expressions.size(); ai++) {
		auto &aggr = agg->expressions[ai]->Cast<BoundAggregateExpression>();
		string name = StringUtil::Lower(aggr.function.name);
		if (avg_components.count(ai)) {
			continue;
		}
		if (name == "count" || name == "count_star") {
			count_count++;
		} else if (name != "avg" && name != "min" && name != "max") {
			sum_count++;
		}
	}
	auto count_output_domains = GetDpSassCountOutputDomains(input.context, count_count);
	auto sum_output_domains = GetDpSassSumOutputDomains(input.context, sum_count);
	idx_t count_output_idx = 0;
	idx_t sum_output_idx = 0;
	for (idx_t ai = 0; ai < agg->expressions.size(); ai++) {
		auto &aggr = agg->expressions[ai]->Cast<BoundAggregateExpression>();
		string name = StringUtil::Lower(aggr.function.name);
		bool avg_sum_component = FindAvgInfoForSumPos(avg_infos, ai) != nullptr;
		output_types.push_back(name == "avg" ? LogicalType::DOUBLE : aggr.return_type);
		if (avg_sum_component) {
			double output_bound = GetRequiredDpBound(input.context, "dp_sass_sum_output_bound", "dp_sass");
			lower_bounds.push_back(-output_bound);
			upper_bounds.push_back(output_bound);
			empty_defaults.push_back(0.0);
		} else if (FindAvgInfoForCountPos(avg_infos, ai)) {
			double output_bound = GetRequiredDpBound(input.context, "dp_sass_count_output_bound", "dp_sass");
			lower_bounds.push_back(0.0);
			upper_bounds.push_back(output_bound);
			empty_defaults.push_back(0.0);
		} else if (name == "count" || name == "count_star") {
			D_ASSERT(count_output_idx < count_output_domains.size());
			auto output_domain = count_output_domains[count_output_idx++];
			lower_bounds.push_back(output_domain.lower);
			upper_bounds.push_back(output_domain.upper);
			empty_defaults.push_back(0.0); // an empty subsample counts 0
		} else if (name == "avg") {
			D_ASSERT(avg_output_idx < avg_bounds.domains.size());
			double avg_lower = avg_bounds.domains[avg_output_idx].lower;
			double avg_upper = avg_bounds.domains[avg_output_idx].upper;
			lower_bounds.push_back(avg_lower);
			upper_bounds.push_back(avg_upper);
			empty_defaults.push_back((avg_lower + avg_upper) / 2.0); // no identity for mean → neutral midpoint
			avg_output_idx++;
		} else if (name == "min" || name == "max") {
			double mm_lower = GetRequiredFiniteSetting(input.context, "dp_sass_minmax_lower_bound", "dp_sass");
			double mm_upper = GetRequiredFiniteSetting(input.context, "dp_sass_minmax_upper_bound", "dp_sass");
			if (mm_lower >= mm_upper) {
				throw InvalidInputException("dp_sass: dp_sass_minmax_lower_bound must be smaller than "
				                            "dp_sass_minmax_upper_bound");
			}
			lower_bounds.push_back(mm_lower);
			upper_bounds.push_back(mm_upper);
			// MIN identity is +∞ (→ clamp to upper); MAX identity is −∞ (→ clamp to lower).
			empty_defaults.push_back(name == "min" ? mm_upper : mm_lower);
		} else {
			D_ASSERT(sum_output_idx < sum_output_domains.size());
			auto output_domain = sum_output_domains[sum_output_idx++];
			lower_bounds.push_back(output_domain.lower);
			upper_bounds.push_back(output_domain.upper);
			empty_defaults.push_back(0.0); // an empty subsample sums to 0
		}
		bool nonempty_avg = use_nonempty_lane_average && name == "avg";
		bool nonempty_sum = use_nonempty_sum_average && name == "sum" && !avg_sum_component;
		nonempty_lane_releases.push_back(nonempty_avg || nonempty_sum);
		if (nonempty_avg) {
			nonempty_lane_baselines.push_back(lower_bounds.back());
			nonempty_lane_sensitivities.push_back(upper_bounds.back() - lower_bounds.back());
		} else if (nonempty_sum) {
			double contribution_sensitivity = GetDpSassSumContributionBound(contribution_bounds, ai) * sum_lane_rescale;
			if (!std::isfinite(contribution_sensitivity) || contribution_sensitivity <= 0.0) {
				throw InvalidInputException("dp_sass: non-empty-lane SUM sensitivity is not finite and positive");
			}
			nonempty_lane_baselines.push_back(std::max(lower_bounds.back(), std::min(upper_bounds.back(), 0.0)));
			nonempty_lane_sensitivities.push_back(contribution_sensitivity);
			nullable_sum_outputs[ai] = true;
		} else {
			nonempty_lane_baselines.push_back(0.0);
			nonempty_lane_sensitivities.push_back(0.0);
		}
		// A 'ratio' AVG splits its cell budget in half across its SUM and COUNT components.
		double cell_divisor = avg_components.count(ai) ? (budget_divisor * 2.0) : budget_divisor;
		epsilons.push_back(epsilon / cell_divisor);
		deltas.push_back(delta / cell_divisor);
		sample_output_positions.push_back(ai);
	}

	bool distinct_support_filter = apply_support_filter && IsDpSampleCountDistinct(agg);
	RewriteAggregateToSampleMedian(input, agg, std::move(pu_hash_expr), contribution_bounds, distinct_support_filter,
	                               distinct_count_is_pu_key, variable_m, &nullable_sum_outputs);

	LogicalOperator *projection_anchor = agg;
	if (distinct_support_filter) {
		// The distinct rewrite exposed the per-group COUNT(DISTINCT pu) support as expr[1]; release
		// position 0 (the counter mask) stays in sample_output_positions. Same noised-τ filter as below.
		projection_anchor = ApplySupportFilter(input, plan, agg, 1, tau, support_noise_scale);
	} else if (apply_support_filter) {
		// η = COUNT(DISTINCT pu): count_star over the pu∪G pre-aggregation equals the distinct-PU
		// count per group. ApplySupportFilter adds Laplace(C_u/ε_η) noise and keeps groups with η' ≥ τ.
		idx_t support_pos = agg->expressions.size();
		agg->expressions.push_back(BindPlainAggregate(input, "count_star", nullptr));
		agg->ResolveOperatorTypes();
		projection_anchor = ApplySupportFilter(input, plan, agg, support_pos, tau, support_noise_scale);
	}

	auto noise_proj =
	    WrapSampleMedianProjection(input, plan, agg, epsilons, deltas, output_types, lower_bounds, upper_bounds,
	                               empty_defaults, projection_anchor, release_method, sample_output_positions,
	                               nonempty_lane_releases, nonempty_lane_baselines, nonempty_lane_sensitivities);

	// 'ratio' AVG: divide the independently-noised SUM and COUNT releases (noised_sum / max(noised_count, 1)),
	// then clamp into the public avg domain. Skipped under stability-query mode, which emits raw per-lane
	// recorded values for diagnostics rather than a released ratio.
	bool stability_query_mode = GetBooleanSetting(input.context, "dp_sass_stability_query_mode", false);
	if (!avg_infos.empty() && !stability_query_mode) {
		WrapAvgRatioProjection(input, plan, noise_proj, avg_infos, n_groups, n_original_aggs, noise_proj.proj_ptr,
		                       /*null_on_nonpositive_count=*/false, /*clamp_output=*/true, ratio_avg_lower,
		                       ratio_avg_upper);
	}
	(void)noise_proj;

#if PRIVACY_DEBUG
	PRIVACY_DEBUG_PRINT("=== PLAN AFTER DP_SAMPLE_MEDIAN TRANSFORMATION ===");
	plan->Print();
#endif
}

} // namespace duckdb
