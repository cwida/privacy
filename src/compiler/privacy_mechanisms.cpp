#include "compiler/privacy_mechanisms.hpp"
#include "aggregates/pac_aggregate.hpp"
#include "utils/privacy_helpers.hpp"
#include "compiler/privacy_compiler.hpp"
#include "query_processing/pac_plan_traversal.hpp"
#include "query_processing/pac_expression_builder.hpp"
#include "privacy_debug.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

#include <cmath>

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
	idx_t sum_pos;   // position of the AVG→SUM in the rewritten aggregate (in-place)
	idx_t count_pos; // position of the appended COUNT(*) in the rewritten aggregate
};

static unique_ptr<Expression> BindScalarLocal(OptimizerExtensionInput &input, const string &func_name,
                                              vector<unique_ptr<Expression>> children) {
	FunctionBinder function_binder(input.context);
	ErrorData error;
	auto expr = function_binder.BindScalarFunction(DEFAULT_SCHEMA, func_name, std::move(children), error);
	if (error.HasError()) {
		throw InternalException("dp_elastic: failed to bind scalar function '" + func_name + "': " + error.Message());
	}
	return expr;
}

// Replace each AVG(x) with SUM(x) in-place and append a COUNT(*) at the end.
// Returns info pairing each (sum_pos, count_pos) for post-processing.
static vector<AvgInfo> RewriteAvgAggregates(OptimizerExtensionInput &input, LogicalAggregate *agg) {
	vector<AvgInfo> avg_infos;
	idx_t n_original = agg->expressions.size();
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

		agg->expressions[i] = BindPlainAggregate(input, "sum", std::move(arg));
		agg->expressions[i]->Cast<BoundAggregateExpression>().filter = std::move(filter_for_sum);

		idx_t count_pos = agg->expressions.size();
		agg->expressions.push_back(BindPlainAggregate(input, "count_star", nullptr));
		agg->expressions.back()->Cast<BoundAggregateExpression>().filter = std::move(filter_for_count);

		avg_infos.push_back({i, count_pos});
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
                                const vector<string> &privacy_units, bool include_fk_cols) {
	(void)privacy_units;

	// Self-join check: no table name may appear more than once in the plan
	std::unordered_map<string, int> name_count;
	for (auto *g : gets) {
		string name = StringUtil::Lower(g->GetTable()->name);
		name_count[name]++;
		if (name_count[name] > 1) {
			throw InvalidInputException("dp_elastic: self-joins are not supported (table '" + g->GetTable()->name +
			                            "' appears more than once)");
		}
	}

	// Single-table case: no non-PU tables → chain is just the PU itself, ES = 1
	if (check.scanned_non_pu_tables.empty()) {
		if (check.scanned_pu_tables.size() != 1) {
			throw InvalidInputException("dp_elastic: expected exactly one privacy unit table, found " +
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
		throw InvalidInputException("dp_elastic: query touches multiple privacy unit tables (" + pu_list +
		                            "). Only one privacy unit is supported per query.");
	}

	// Find non-PU table with the longest FK path — this is the root of the chain.
	// fk_paths contains the full path to the PU (including the PU itself) for every scanned
	// non-PU table, whether or not the PU is explicitly joined in the query.
	const vector<string> *longest_path = nullptr;
	for (auto &non_pu : check.scanned_non_pu_tables) {
		auto it = check.fk_paths.find(non_pu);
		if (it == check.fk_paths.end() || it->second.empty()) {
			throw InvalidInputException("dp_elastic: table '" + non_pu +
			                            "' has no PRIVACY_LINK path to a privacy unit table. "
			                            "Declare the link with ALTER TABLE ADD PRIVACY_LINK.");
		}
		if (!longest_path || it->second.size() > longest_path->size()) {
			longest_path = &it->second;
		}
	}
	D_ASSERT(longest_path);

	// Build a set of tables in the longest path for membership checks
	std::unordered_set<string> path_set;
	for (auto &t : *longest_path) {
		path_set.insert(StringUtil::Lower(t));
	}

	// All non-PU scanned tables must appear in the longest path (linear chain requirement)
	for (auto &non_pu : check.scanned_non_pu_tables) {
		if (path_set.find(StringUtil::Lower(non_pu)) == path_set.end()) {
			throw InvalidInputException("dp_elastic: star/diamond joins are not yet supported. Table '" + non_pu +
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
			throw InternalException("dp_elastic: no metadata for table '" + from_table + "'");
		}
		bool found_fk = false;
		for (auto &fk : meta_it->second.fks) {
			if (StringUtil::Lower(fk.first) == StringUtil::Lower(to_table)) {
				if (fk.second.empty()) {
					throw InternalException("dp_elastic: empty FK column list for " + from_table + " → " + to_table);
				}
				chain.fk_cols.push_back(fk.second[0]);
				found_fk = true;
				break;
			}
		}
		if (!found_fk) {
			throw InternalException("dp_elastic: could not find FK column from '" + from_table + "' to '" + to_table +
			                        "'");
		}
	}

	return chain;
}

static LogicalAggregate *CheckDPAggregates(unique_ptr<LogicalOperator> &plan, const string &mechanism_name) {
	bool allow_min_max = mechanism_name == "dp_sass";
	vector<LogicalAggregate *> aggs;
	FindAllAggregates(plan, aggs);
	if (aggs.size() != 1) {
		throw InvalidInputException(mechanism_name + ": expected exactly one aggregate, found " +
		                            std::to_string(aggs.size()));
	}
	auto *agg = aggs[0];
	for (auto &expr : agg->expressions) {
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			throw InvalidInputException(mechanism_name + ": unsupported non-aggregate expression in aggregate node");
		}
		auto &aggr = expr->Cast<BoundAggregateExpression>();
		if (aggr.IsDistinct()) {
			throw InvalidInputException(mechanism_name + ": DISTINCT aggregates are not supported");
		}
		string name = StringUtil::Lower(aggr.function.name);
		bool is_count = name == "count" || name == "count_star";
		bool is_sum_avg = name == "sum" || name == "avg";
		bool is_min_max = name == "min" || name == "max";
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
	return agg;
}

static DPFKChain ExtractDPElasticFKChain(unique_ptr<LogicalOperator> &plan, const vector<string> &privacy_units,
                                         const PrivacyCompatibilityResult &check) {
	vector<LogicalGet *> gets;
	FindAllGetNodes(plan.get(), gets);
	return ExtractFKChain(check, gets, privacy_units, true);
}

static void CheckDPFKChainShape(unique_ptr<LogicalOperator> &plan, const vector<string> &privacy_units,
                                const PrivacyCompatibilityResult &check) {
	vector<LogicalGet *> gets;
	FindAllGetNodes(plan.get(), gets);
	ExtractFKChain(check, gets, privacy_units, false);
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
		throw InvalidInputException("dp_elastic: failed to compute mf_K for " + table_name + "." + fk_col +
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
	// k* ≈ n/β; add margin and cap to avoid impractically long loops
	double n = static_cast<double>(mf_values.size());
	int k_max = static_cast<int>(std::min(n / beta + 200.0, 100000.0));
	k_max = std::max(k_max, 1000);
	for (int k = 1; k <= k_max; k++) {
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
		    "dp_elastic: dp_delta must be set. Use SET dp_delta=<value> or PRAGMA refresh_dp_stats(<epsilon>).");
	}
	if (delta <= 0.0 || !std::isfinite(delta)) {
		throw InvalidInputException("dp_elastic: dp_delta must be a positive finite number (got " +
		                            std::to_string(delta) + ")");
	}
	if (delta >= 0.5) {
		throw InvalidInputException("dp_elastic: dp_delta must be < 0.5 for meaningful (ε,δ)-DP (got " +
		                            std::to_string(delta) + ")");
	}

	double beta = epsilon / (2.0 * std::log(2.0 / delta));
	double ses = ComputeSmoothElasticSensitivity(mf_values, beta);
	PRIVACY_DEBUG_PRINT("[DP_ELASTIC] smooth sensitivity: beta=" + std::to_string(beta) +
	                    " SES=" + std::to_string(ses));
	return 2.0 * ses;
}

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

// Replace sum child with greatest(least(cast(v, DOUBLE), C), -C), cast back to original type
static void ClipSumInputs(OptimizerExtensionInput &input, LogicalAggregate *agg, double bound) {
	for (auto &expr : agg->expressions) {
		auto &aggr = expr->Cast<BoundAggregateExpression>();
		if (aggr.function.name != "sum" || aggr.children.empty()) {
			continue;
		}
		auto original_type = aggr.children[0]->return_type;
		auto as_double =
		    BoundCastExpression::AddCastToType(input.context, std::move(aggr.children[0]), LogicalType::DOUBLE);
		auto upper_bound = make_uniq<BoundConstantExpression>(Value::DOUBLE(bound));
		auto lower_bound = make_uniq<BoundConstantExpression>(Value::DOUBLE(-bound));
		auto upper_clipped = input.optimizer.BindScalarFunction("least", std::move(as_double), std::move(upper_bound));
		auto clipped = input.optimizer.BindScalarFunction("greatest", std::move(upper_clipped), std::move(lower_bound));
		auto restored = BoundCastExpression::AddCastToType(input.context, std::move(clipped), original_type);
		aggr.children[0] = std::move(restored);
	}
	agg->ResolveOperatorTypes();
}

// Laplace scale = sensitivity / epsilon; sensitivity = es for COUNT, es * sum_bound for SUM
static double Sensitivity(const BoundAggregateExpression &aggr, double sum_bound, double es) {
	const string &name = aggr.function.name;
	if (name == "count" || name == "count_star") {
		return es;
	}
	if (name == "sum") {
		return es * sum_bound;
	}
	throw InternalException("dp_elastic: Sensitivity received unsupported '" + name + "'");
}

static unique_ptr<Expression> BuildSupportKeyExpression(unique_ptr<LogicalOperator> &plan, const DPFKChain &chain) {
	if (chain.tables.size() > 2) {
		throw InvalidInputException("dp_elastic: privacy_min_group_count currently supports single-table PU queries "
		                            "and one-hop PRIVACY_LINK chains only");
	}
	if (chain.tables.size() == 1) {
		return nullptr;
	}

	auto *get = FindTableScanInSubtree(plan.get(), chain.tables[0]);
	if (!get) {
		throw InternalException("dp_elastic: could not find support source table '" + chain.tables[0] + "'");
	}
	idx_t proj_idx = EnsureProjectedColumn(*get, chain.fk_cols[0]);
	if (proj_idx == DConstants::INVALID_INDEX) {
		throw InternalException("dp_elastic: failed to project support key column '" + chain.fk_cols[0] + "'");
	}
	auto col_index = get->GetColumnIds()[proj_idx];
	auto col_type = get->GetColumnType(col_index);
	return make_uniq<BoundColumnRefExpression>(col_type, ColumnBinding(get->table_index, proj_idx));
}

static optional_idx AddSupportAggregate(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                                        LogicalAggregate *agg, const DPFKChain &chain, double threshold) {
	if (threshold <= 0.0 || !std::isfinite(threshold)) {
		return optional_idx();
	}

	auto support_key = BuildSupportKeyExpression(plan, chain);
	if (support_key) {
		agg->expressions.push_back(BindPlainAggregate(input, "count", std::move(support_key), AggregateType::DISTINCT));
	} else {
		agg->expressions.push_back(BindPlainAggregate(input, "count_star", nullptr));
	}
	agg->ResolveOperatorTypes();
	return optional_idx(agg->expressions.size() - 1);
}

static LogicalFilter *ApplySupportFilter(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                                         LogicalAggregate *agg, idx_t support_pos, double threshold) {
	auto support_ref = make_uniq<BoundColumnRefExpression>(agg->types[agg->groups.size() + support_pos],
	                                                       ColumnBinding(agg->aggregate_index, support_pos));
	auto support_double =
	    BoundCastExpression::AddCastToType(input.context, std::move(support_ref), LogicalType::DOUBLE);
	auto threshold_const = make_uniq<BoundConstantExpression>(Value::DOUBLE(threshold));
	auto condition = make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
	                                                      std::move(support_double), std::move(threshold_const));

	auto filter = make_uniq<LogicalFilter>();
	filter->expressions.push_back(std::move(condition));

	auto *slot = FindOperatorSlotByPointer(plan, agg);
	if (!slot) {
		throw InternalException("dp_elastic: could not locate aggregate slot for support filter");
	}
	filter->children.push_back(std::move(*slot));
	filter->ResolveOperatorTypes();
	auto *filter_ptr = filter.get();
	*slot = std::move(filter);
	return filter_ptr;
}

// ----------------------------------------------------------------------------
// Wrap aggregate output — inserts LogicalProjection above `agg` with
// priv_laplace_noise applied to each DP-target column, cast back to original type.
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
		replacer.replacement_bindings.emplace_back(ColumnBinding(source_group_idx, gi), ColumnBinding(proj_idx, gi));
	}
	for (idx_t ai = 0; ai < n_aggs; ai++) {
		replacer.replacement_bindings.emplace_back(ColumnBinding(source_agg_idx, source_agg_offset + ai),
		                                           ColumnBinding(proj_idx, n_groups + ai));
	}
	replacer.stop_operator = proj_ptr;
	replacer.VisitOperator(*plan);

	return {proj_idx, proj_ptr, std::move(proj_types)};
}

static NoiseProjection WrapAggregateWithLaplace(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                                                LogicalAggregate *agg, LogicalOperator *insert_above,
                                                const vector<double> &agg_scales) {
	idx_t n_groups = agg->groups.size();
	idx_t n_aggs = agg_scales.size();
	idx_t group_idx = agg->group_index;
	idx_t agg_idx = agg->aggregate_index;

	auto agg_types = agg->types;
	D_ASSERT(agg_types.size() >= n_groups + n_aggs);

	vector<unique_ptr<Expression>> proj_exprs;
	proj_exprs.reserve(n_groups + n_aggs);

	for (idx_t gi = 0; gi < n_groups; gi++) {
		proj_exprs.push_back(make_uniq<BoundColumnRefExpression>(agg_types[gi], ColumnBinding(group_idx, gi)));
	}

	for (idx_t ai = 0; ai < n_aggs; ai++) {
		auto agg_col_type = agg_types[n_groups + ai];
		auto col_ref = make_uniq<BoundColumnRefExpression>(agg_col_type, ColumnBinding(agg_idx, ai));
		double scale = agg_scales[ai];
		if (scale <= 0.0 || !std::isfinite(scale)) {
			proj_exprs.push_back(std::move(col_ref));
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
		unique_ptr<Expression> noised = BindScalarLocal(input, "priv_laplace_noise", std::move(noise_children));
		if (agg_col_type != LogicalType::DOUBLE) {
			noised = BoundCastExpression::AddCastToType(input.context, std::move(noised), agg_col_type);
		}
		proj_exprs.push_back(std::move(noised));
	}

	idx_t proj_idx = input.optimizer.binder.GenerateTableIndex();
	auto projection = make_uniq<LogicalProjection>(proj_idx, std::move(proj_exprs));
	return InsertProjectionAndRemap(plan, std::move(projection), insert_above, proj_idx, group_idx, agg_idx, n_groups,
	                                n_aggs, 0, "dp_elastic: could not locate noise insertion slot in plan");
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
                       idx_t n_original_aggs, LogicalOperator *insert_above, bool null_on_nonpositive_count) {
	D_ASSERT(!avg_infos.empty());

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
			auto col_type = noise_proj.proj_types[n_groups + ai];
			proj_exprs.push_back(
			    make_uniq<BoundColumnRefExpression>(col_type, ColumnBinding(noise_proj.proj_idx, n_groups + ai)));
		} else {
			// AVG: compute CAST(noised_sum AS DOUBLE) / CAST(noised_count AS DOUBLE)
			const AvgInfo &info = *avg_info;
			auto sum_type = noise_proj.proj_types[n_groups + info.sum_pos];
			auto cnt_type = noise_proj.proj_types[n_groups + info.count_pos];

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

			// DuckDB registers "/" as "divide" for scalar functions
			auto denominator = make_cnt_dbl();
			if (!null_on_nonpositive_count) {
				auto one = make_uniq<BoundConstantExpression>(Value::DOUBLE(1.0));
				denominator = input.optimizer.BindScalarFunction("greatest", std::move(denominator), std::move(one));
				proj_exprs.push_back(input.optimizer.BindScalarFunction("/", make_sum_dbl(), std::move(denominator)));
			} else {
				auto ratio = input.optimizer.BindScalarFunction("/", make_sum_dbl(), std::move(denominator));
				auto case_expr = make_uniq<BoundCaseExpression>(LogicalType::DOUBLE);
				BoundCaseCheck check;
				check.when_expr =
				    make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO, make_cnt_dbl(),
				                                         make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0)));
				check.then_expr = make_uniq<BoundConstantExpression>(Value(LogicalType::DOUBLE));
				case_expr->case_checks.push_back(std::move(check));
				case_expr->else_expr = std::move(ratio);
				proj_exprs.push_back(std::move(case_expr));
			}
		}
	}

	idx_t ratio_idx = input.optimizer.binder.GenerateTableIndex();
	auto ratio_proj = make_uniq<LogicalProjection>(ratio_idx, std::move(proj_exprs));
	auto inserted = InsertProjectionAndRemap(plan, std::move(ratio_proj), insert_above, ratio_idx, noise_proj.proj_idx,
	                                         noise_proj.proj_idx, n_groups, n_original_aggs, n_groups,
	                                         "dp_elastic: could not locate insert point for AVG ratio projection");
	return {inserted.proj_idx, inserted.proj_ptr};
}

static NoiseProjection WrapSampleMedianProjection(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                                                  LogicalAggregate *agg, const vector<double> &epsilons,
                                                  const vector<double> &deltas, const vector<LogicalType> &output_types,
                                                  LogicalOperator *insert_above) {
	idx_t n_groups = agg->groups.size();
	idx_t n_aggs = output_types.size();
	D_ASSERT(epsilons.size() == n_aggs);
	D_ASSERT(deltas.size() == n_aggs);

	vector<unique_ptr<Expression>> proj_exprs;
	proj_exprs.reserve(n_groups + n_aggs);
	for (idx_t gi = 0; gi < n_groups; gi++) {
		proj_exprs.push_back(make_uniq<BoundColumnRefExpression>(agg->types[gi], ColumnBinding(agg->group_index, gi)));
	}

	int sample_lanes = GetDpSampleLanes(input.context);
	for (idx_t ai = 0; ai < n_aggs; ai++) {
		vector<unique_ptr<Expression>> children;
		children.push_back(
		    make_uniq<BoundColumnRefExpression>(agg->types[n_groups + ai], ColumnBinding(agg->aggregate_index, ai)));
		children.push_back(make_uniq<BoundConstantExpression>(Value::DOUBLE(epsilons[ai])));
		children.push_back(make_uniq<BoundConstantExpression>(Value::DOUBLE(deltas[ai])));
		children.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(sample_lanes)));
		unique_ptr<Expression> noised = BindScalarLocal(input, "priv_smooth_median_noise", std::move(children));
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

static void RewriteAggregateToSampleMedian(OptimizerExtensionInput &input, LogicalAggregate *agg,
                                           unique_ptr<Expression> pu_hash_expr) {
	vector<unique_ptr<Expression>> lower_expressions;
	lower_expressions.reserve(agg->expressions.size());
	for (auto &expr : agg->expressions) {
		auto &aggr = expr->Cast<BoundAggregateExpression>();
		vector<unique_ptr<Expression>> children;
		if (aggr.function.name == "count_star") {
			lower_expressions.push_back(BindPlainAggregate(input, "count_star", std::move(children)));
		} else if (aggr.function.name == "count") {
			if (!aggr.children.empty()) {
				children.push_back(aggr.children[0]->Copy());
			}
			lower_expressions.push_back(BindPlainAggregate(input, "count", std::move(children)));
		} else if (aggr.function.name == "sum") {
			children.push_back(aggr.children[0]->Copy());
			lower_expressions.push_back(BindPlainAggregate(input, "sum", std::move(children)));
		} else if (aggr.function.name == "min" || aggr.function.name == "max") {
			children.push_back(aggr.children[0]->Copy());
			lower_expressions.push_back(BindPlainAggregate(input, aggr.function.name, std::move(children)));
		} else {
			throw InvalidInputException("dp_sample_median: only COUNT, SUM, MIN, and MAX aggregates are supported");
		}
		lower_expressions.back()->Cast<BoundAggregateExpression>().filter = aggr.filter ? aggr.filter->Copy() : nullptr;
	}

	auto pre_agg = InsertPuPreAggregation(input, agg, std::move(lower_expressions), std::move(pu_hash_expr));

	for (idx_t ai = 0; ai < agg->expressions.size(); ai++) {
		auto lower_type = pre_agg.lower_agg->types[pre_agg.num_original_groups + 1 + ai];
		unique_ptr<Expression> lower_ref =
		    make_uniq<BoundColumnRefExpression>(lower_type, ColumnBinding(pre_agg.lower_agg_index, ai));
		lower_ref = BoundCastExpression::AddCastToType(input.context, std::move(lower_ref), LogicalType::DOUBLE);

		vector<unique_ptr<Expression>> children;
		children.push_back(pre_agg.pu_hash_ref->Copy());
		children.push_back(std::move(lower_ref));
		auto &original = agg->expressions[ai]->Cast<BoundAggregateExpression>();
		string sample_name = "priv_sample_sum";
		if (original.function.name == "min") {
			sample_name = "priv_sample_min";
		} else if (original.function.name == "max") {
			sample_name = "priv_sample_max";
		}
		PRIVACY_DEBUG_PRINT("[DP_SAMPLE_MEDIAN] rewrite " + original.function.name + " -> " + sample_name);
		agg->expressions[ai] = BindPlainAggregate(input, sample_name, std::move(children));
	}
	agg->ResolveOperatorTypes();
}

// ----------------------------------------------------------------------------
// Entry point
// ----------------------------------------------------------------------------

void CompileDPElasticQuery(const PrivacyCompatibilityResult &check, OptimizerExtensionInput &input,
                           unique_ptr<LogicalOperator> &plan, const vector<string> &privacy_units,
                           const string &query_hash) {
	(void)query_hash;
	PRIVACY_DEBUG_PRINT("[DP_ELASTIC] CompileDPElasticQuery: start");

	double epsilon = GetValidatedDpEpsilon(input.context, "dp_elastic");

	auto fk_chain = ExtractDPElasticFKChain(plan, privacy_units, check);
	auto *agg = CheckDPAggregates(plan, "dp_elastic");

	// Rewrite AVG(x) → SUM(x) + COUNT(*) before bound/clipping checks.
	// Each AVG uses ε/2 per component so the combined cost is still ε-DP.
	auto avg_infos = RewriteAvgAggregates(input, agg);
	idx_t n_original_aggs = agg->expressions.size() - avg_infos.size();
	idx_t n_groups = agg->groups.size();

	double support_threshold = 0.0;
	bool apply_support_filter = TryGetPrivacyMinGroupCount(input.context, support_threshold);
	auto support_pos = AddSupportAggregate(input, plan, agg, fk_chain, support_threshold);
	idx_t n_dp_aggs = support_pos.IsValid() ? support_pos.GetIndex() : agg->expressions.size();
	LogicalOperator *noise_anchor = agg;
	if (apply_support_filter) {
		noise_anchor = ApplySupportFilter(input, plan, agg, support_pos.GetIndex(), support_threshold);
	}

	auto avg_components = BuildAvgComponentSet(avg_infos);

	double sum_bound = 0.0;
	bool has_sum = AggregateContainsSum(agg); // SUM or AVG→SUM
	if (has_sum) {
		if (!TryGetDpSumBound(input.context, sum_bound)) {
			throw InvalidInputException(
			    "dp_elastic: dp_sum_bound must be set for SUM and AVG aggregates (SET dp_sum_bound = <C>)");
		}
		if (sum_bound <= 0.0 || !std::isfinite(sum_bound)) {
			throw InvalidInputException("dp_elastic: dp_sum_bound must be a positive finite number (got " +
			                            std::to_string(sum_bound) + ")");
		}
		ClipSumInputs(input, agg, sum_bound);
	}

	// Compute smooth elastic sensitivity using the required dp_delta setting
	double es = ComputeElasticSensitivity(input.context, fk_chain, epsilon);

	// When privacy_noise=false the compilation pipeline still runs (clipping, FK chain)
	// but Laplace scale is zeroed → priv_laplace_noise returns the value unchanged.
	// This mirrors pac_mi=0 for PAC and enables deterministic testing.
	bool noise_enabled = IsPacNoiseEnabled(input.context, true);

	// Build per-aggregate Laplace scales. With k user-visible aggregates we split ε equally
	// (sequential composition) → each gets ε/k. AVG splits its share again into ε/(2k) per
	// component (SUM and COUNT). Effective scale multiplier:
	//   - non-AVG aggregate:           k       (using ε/k)
	//   - AVG-derived SUM and COUNT:   2k      (each using ε/(2k))
	double k = static_cast<double>(n_original_aggs);
	vector<double> agg_scales;
	agg_scales.reserve(n_dp_aggs);
	for (idx_t ai = 0; ai < n_dp_aggs; ai++) {
		auto &aggr = agg->expressions[ai]->Cast<BoundAggregateExpression>();
		double sens = Sensitivity(aggr, sum_bound, es);
		double scale = noise_enabled ? (sens / epsilon) : 0.0;
		if (k > 1.0) {
			scale *= k; // budget split across user-visible aggregates
		}
		if (avg_components.count(ai)) {
			scale *= 2.0; // additional split: each AVG component uses half its allocation
		}
		agg_scales.push_back(scale);
		PRIVACY_DEBUG_PRINT("[DP_ELASTIC] agg '" + aggr.function.name + "' sensitivity=" + std::to_string(sens) +
		                    " scale=" + std::to_string(scale));
	}

	auto noise_proj = WrapAggregateWithLaplace(input, plan, agg, noise_anchor, agg_scales);

	// Wrap AVG above the noise projection so upstream operators — including HAVING
	// that references AVG — get remapped from the raw SUM to the ratio output.
	if (!avg_infos.empty()) {
		WrapAvgRatioProjection(input, plan, noise_proj, avg_infos, n_groups, n_original_aggs, noise_proj.proj_ptr,
		                       false);
	}

#if PRIVACY_DEBUG
	PRIVACY_DEBUG_PRINT("=== PLAN AFTER DP_ELASTIC TRANSFORMATION ===");
	plan->Print();
#endif
}

void CompileDPSampleMedianQuery(const PrivacyCompatibilityResult &check, OptimizerExtensionInput &input,
                                unique_ptr<LogicalOperator> &plan, const vector<string> &privacy_units,
                                const string &query_hash) {
	(void)query_hash;
	PRIVACY_DEBUG_PRINT("[DP_SAMPLE_MEDIAN] CompileDPSampleMedianQuery: start");

	throw InvalidInputException(
	    "dp_sass is disabled for automatic private query rewriting: SAA + smooth sensitivity requires enforced "
	    "per-PU contribution bounds and sample-output/domain bounds. Use privacy_mode='dp_elastic' for formal DP.");

	double epsilon = GetValidatedDpEpsilon(input.context, "dp_sample_median");
	double delta = 0.0;
	if (!TryGetDpDelta(input.context, delta)) {
		throw InvalidInputException(
		    "dp_sample_median: dp_delta must be set. Use SET dp_delta=<value> or PRAGMA refresh_dp_stats(<epsilon>).");
	}
	if (delta <= 0.0 || delta >= 0.5 || !std::isfinite(delta)) {
		throw InvalidInputException("dp_sample_median: dp_delta must be in (0, 0.5)");
	}

	CheckDPFKChainShape(plan, privacy_units, check);
	auto *agg = CheckDPAggregates(plan, "dp_sass");

	auto avg_infos = RewriteAvgAggregates(input, agg);
	idx_t n_original_aggs = agg->expressions.size() - avg_infos.size();
	idx_t n_groups = agg->groups.size();

	vector<LogicalType> output_types;
	output_types.reserve(agg->expressions.size());
	auto avg_components = BuildAvgComponentSet(avg_infos);
	double k = static_cast<double>(n_original_aggs);
	vector<double> epsilons;
	vector<double> deltas;
	epsilons.reserve(agg->expressions.size());
	deltas.reserve(agg->expressions.size());
	for (idx_t ai = 0; ai < agg->expressions.size(); ai++) {
		auto &aggr = agg->expressions[ai]->Cast<BoundAggregateExpression>();
		output_types.push_back(avg_components.count(ai) ? LogicalType::DOUBLE : aggr.return_type);
		double split = avg_components.count(ai) ? (2.0 * k) : k;
		epsilons.push_back(epsilon / split);
		deltas.push_back(delta / split);
	}

	auto pu_setup = PreparePlanForPUHashing(check, input, plan, privacy_units);
	auto hash_infos = BuildPUHashExpressionsForAggregates(input, plan, pu_setup.pu_names, check, pu_setup.cte_map);
	if (hash_infos.empty()) {
		throw InvalidInputException("dp_sample_median: no aggregate with a reachable privacy-unit hash found");
	}
	if (hash_infos.size() != 1 || hash_infos[0].aggregate != agg) {
		throw InvalidInputException("dp_sample_median: expected exactly one privacy-unit aggregate");
	}
	RewriteAggregateToSampleMedian(input, agg, std::move(hash_infos[0].hash_expr));

	LogicalOperator *projection_anchor = agg;
	double support_threshold = 0.0;
	bool apply_support_filter = TryGetPrivacyMinGroupCount(input.context, support_threshold);
	if (apply_support_filter) {
		idx_t support_pos = agg->expressions.size();
		agg->expressions.push_back(BindPlainAggregate(input, "count_star", nullptr));
		agg->ResolveOperatorTypes();
		projection_anchor = ApplySupportFilter(input, plan, agg, support_pos, support_threshold);
	}

	auto noise_proj = WrapSampleMedianProjection(input, plan, agg, epsilons, deltas, output_types, projection_anchor);
	if (!avg_infos.empty()) {
		WrapAvgRatioProjection(input, plan, noise_proj, avg_infos, n_groups, n_original_aggs, noise_proj.proj_ptr,
		                       true);
	}

#if PRIVACY_DEBUG
	PRIVACY_DEBUG_PRINT("=== PLAN AFTER DP_SAMPLE_MEDIAN TRANSFORMATION ===");
	plan->Print();
#endif
}

} // namespace duckdb
