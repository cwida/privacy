#include "compiler/dp_elastic_compiler.hpp"
#include "utils/privacy_helpers.hpp"
#include "query_processing/pac_plan_traversal.hpp"
#include "query_processing/pac_expression_builder.hpp"
#include "privacy_debug.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
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

// Bind a plain aggregate function (sum, count_star, etc.) by name.
static unique_ptr<Expression> BindAggregateLocal(OptimizerExtensionInput &input, const string &func_name,
                                                 unique_ptr<Expression> arg,
                                                 AggregateType aggr_type = AggregateType::NON_DISTINCT) {
	FunctionBinder function_binder(input.context);
	ErrorData error;
	vector<unique_ptr<Expression>> children;
	vector<LogicalType> arg_types;
	if (arg) {
		arg_types.push_back(arg->return_type);
		children.push_back(std::move(arg));
	}
	auto &entry = Catalog::GetSystemCatalog(input.context)
	                  .GetEntry<AggregateFunctionCatalogEntry>(input.context, DEFAULT_SCHEMA, func_name);
	auto best = function_binder.BindFunction(entry.name, entry.functions, arg_types, error);
	if (!best.IsValid()) {
		throw InternalException("dp_elastic: failed to bind aggregate '" + func_name + "'");
	}
	auto func = entry.functions.GetFunctionByOffset(best.GetIndex());
	return function_binder.BindAggregateFunction(func, std::move(children), nullptr, aggr_type);
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

		agg->expressions[i] = BindAggregateLocal(input, "sum", std::move(arg));
		agg->expressions[i]->Cast<BoundAggregateExpression>().filter = std::move(filter_for_sum);

		idx_t count_pos = agg->expressions.size();
		agg->expressions.push_back(BindAggregateLocal(input, "count_star", nullptr));
		agg->expressions.back()->Cast<BoundAggregateExpression>().filter = std::move(filter_for_count);

		avg_infos.push_back({i, count_pos});
	}
	if (!avg_infos.empty()) {
		agg->ResolveOperatorTypes();
	}
	return avg_infos;
}

// ----------------------------------------------------------------------------
// Plan walkers
// ----------------------------------------------------------------------------

static void CollectGetNodes(LogicalOperator *op, vector<LogicalGet *> &out) {
	if (op->type == LogicalOperatorType::LOGICAL_GET) {
		out.push_back(&op->Cast<LogicalGet>());
	}
	for (auto &child : op->children) {
		CollectGetNodes(child.get(), out);
	}
}

// Find the unique_ptr slot holding `target` in the plan. Returns nullptr if not found.
static unique_ptr<LogicalOperator> *FindSlotForOperator(unique_ptr<LogicalOperator> &root, LogicalOperator *target) {
	if (root.get() == target) {
		return &root;
	}
	for (auto &child : root->children) {
		auto *slot = FindSlotForOperator(child, target);
		if (slot) {
			return slot;
		}
	}
	return nullptr;
}

// ----------------------------------------------------------------------------
// FK chain extraction — validates linear chain structure, detects self-joins
// ----------------------------------------------------------------------------

static DPFKChain ExtractFKChain(const PrivacyCompatibilityResult &check, const vector<LogicalGet *> &gets,
                                const vector<string> &privacy_units) {
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

// ----------------------------------------------------------------------------
// Eligibility check (Phase D: allows linear FK join chains)
// ----------------------------------------------------------------------------

struct DPEligibility {
	LogicalAggregate *top_agg;
	DPFKChain fk_chain;
};

static DPEligibility CheckDPEligibility(unique_ptr<LogicalOperator> &plan, const vector<string> &privacy_units,
                                        const PrivacyCompatibilityResult &check) {
	vector<LogicalGet *> gets;
	CollectGetNodes(plan.get(), gets);

	// Validates self-joins, PU count, and linear chain structure; returns the chain
	DPFKChain fk_chain = ExtractFKChain(check, gets, privacy_units);

	vector<LogicalAggregate *> aggs;
	FindAllAggregates(plan, aggs);
	if (aggs.size() != 1) {
		throw InvalidInputException("dp_elastic: expected exactly one aggregate, found " + std::to_string(aggs.size()));
	}
	auto *agg = aggs[0];
	for (auto &expr : agg->expressions) {
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			throw InvalidInputException("dp_elastic: unsupported non-aggregate expression in aggregate node");
		}
		auto &a = expr->Cast<BoundAggregateExpression>();
		if (a.IsDistinct()) {
			throw InvalidInputException("dp_elastic: DISTINCT aggregates are not supported");
		}
		const string &name = a.function.name;
		if (name != "count" && name != "count_star" && name != "sum" && name != "avg") {
			throw InvalidInputException("dp_elastic: only COUNT, SUM, and AVG are supported (got '" + name + "')");
		}
		if ((name == "sum" || name == "avg") && a.children.size() != 1) {
			throw InvalidInputException("dp_elastic: " + name + " must have exactly one argument");
		}
		if ((name == "sum" || name == "avg") && !a.children[0]->return_type.IsNumeric()) {
			throw InvalidInputException("dp_elastic: " + name + " argument must be a numeric type (got '" +
			                            a.children[0]->return_type.ToString() + "')");
		}
	}
	return {agg, std::move(fk_chain)};
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

static LogicalGet *FindGetForTable(unique_ptr<LogicalOperator> &plan, const string &table_name) {
	vector<LogicalGet *> gets;
	CollectGetNodes(plan.get(), gets);
	string needle = StringUtil::Lower(table_name);
	for (auto *get : gets) {
		if (StringUtil::Lower(get->GetTable()->name) == needle) {
			return get;
		}
	}
	return nullptr;
}

static unique_ptr<Expression> BuildSupportKeyExpression(unique_ptr<LogicalOperator> &plan, const DPFKChain &chain) {
	if (chain.tables.size() > 2) {
		throw InvalidInputException("dp_elastic: privacy_min_group_count currently supports single-table PU queries "
		                            "and one-hop PRIVACY_LINK chains only");
	}
	if (chain.tables.size() == 1) {
		return nullptr;
	}

	auto *get = FindGetForTable(plan, chain.tables[0]);
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
		agg->expressions.push_back(BindAggregateLocal(input, "count", std::move(support_key), AggregateType::DISTINCT));
	} else {
		agg->expressions.push_back(BindAggregateLocal(input, "count_star", nullptr));
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

	auto *slot = FindSlotForOperator(plan, agg);
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
// dp_laplace_noise applied to each DP-target column, cast back to original type.
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
		unique_ptr<Expression> noised =
		    input.optimizer.BindScalarFunction("dp_laplace_noise", std::move(value_expr), std::move(scale_expr));
		if (agg_col_type != LogicalType::DOUBLE) {
			noised = BoundCastExpression::AddCastToType(input.context, std::move(noised), agg_col_type);
		}
		proj_exprs.push_back(std::move(noised));
	}

	idx_t proj_idx = input.optimizer.binder.GenerateTableIndex();
	auto projection = make_uniq<LogicalProjection>(proj_idx, std::move(proj_exprs));

	auto *slot = FindSlotForOperator(plan, insert_above);
	if (!slot) {
		throw InternalException("dp_elastic: could not locate noise insertion slot in plan");
	}
	auto old_node = std::move(*slot);
	projection->children.push_back(std::move(old_node));
	projection->ResolveOperatorTypes();
	LogicalProjection *proj_ptr = projection.get();
	auto proj_types = projection->types;
	*slot = std::move(projection);

	// Remap bindings above the inserted projection
	ColumnBindingReplacer replacer;
	for (idx_t gi = 0; gi < n_groups; gi++) {
		replacer.replacement_bindings.emplace_back(ColumnBinding(group_idx, gi), ColumnBinding(proj_idx, gi));
	}
	for (idx_t ai = 0; ai < n_aggs; ai++) {
		replacer.replacement_bindings.emplace_back(ColumnBinding(agg_idx, ai), ColumnBinding(proj_idx, n_groups + ai));
	}
	replacer.stop_operator = proj_ptr;
	replacer.VisitOperator(*plan);

	return {proj_idx, proj_ptr, std::move(proj_types)};
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
                       idx_t n_original_aggs, LogicalOperator *insert_above) {
	D_ASSERT(!avg_infos.empty());

	// Build lookup: original-agg-position → AvgInfo
	std::unordered_map<idx_t, const AvgInfo *> avg_lookup;
	for (auto &info : avg_infos) {
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
		auto it = avg_lookup.find(ai);
		if (it == avg_lookup.end()) {
			// Non-AVG: pass the noised value through
			auto col_type = noise_proj.proj_types[n_groups + ai];
			proj_exprs.push_back(
			    make_uniq<BoundColumnRefExpression>(col_type, ColumnBinding(noise_proj.proj_idx, n_groups + ai)));
		} else {
			// AVG: compute CAST(noised_sum AS DOUBLE) / greatest(CAST(noised_count AS DOUBLE), 1.0)
			const AvgInfo &info = *it->second;
			auto sum_type = noise_proj.proj_types[n_groups + info.sum_pos];
			auto cnt_type = noise_proj.proj_types[n_groups + info.count_pos];
			auto sum_ref = make_uniq<BoundColumnRefExpression>(
			    sum_type, ColumnBinding(noise_proj.proj_idx, n_groups + info.sum_pos));
			auto cnt_ref = make_uniq<BoundColumnRefExpression>(
			    cnt_type, ColumnBinding(noise_proj.proj_idx, n_groups + info.count_pos));
			auto sum_dbl = BoundCastExpression::AddCastToType(input.context, std::move(sum_ref), LogicalType::DOUBLE);
			auto cnt_dbl = BoundCastExpression::AddCastToType(input.context, std::move(cnt_ref), LogicalType::DOUBLE);
			auto one = make_uniq<BoundConstantExpression>(Value::DOUBLE(1.0));
			auto safe_cnt = input.optimizer.BindScalarFunction("greatest", std::move(cnt_dbl), std::move(one));
			// DuckDB registers "/" as "divide" for scalar functions
			auto ratio = input.optimizer.BindScalarFunction("/", std::move(sum_dbl), std::move(safe_cnt));
			proj_exprs.push_back(std::move(ratio));
		}
	}

	idx_t ratio_idx = input.optimizer.binder.GenerateTableIndex();
	auto ratio_proj = make_uniq<LogicalProjection>(ratio_idx, std::move(proj_exprs));

	auto *slot = FindSlotForOperator(plan, insert_above);
	if (!slot) {
		throw InternalException("dp_elastic: could not locate insert point for AVG ratio projection");
	}
	auto old_node = std::move(*slot);
	ratio_proj->children.push_back(std::move(old_node));
	ratio_proj->ResolveOperatorTypes();
	auto *ratio_ptr = ratio_proj.get();
	*slot = std::move(ratio_proj);

	// Remap upstream references: noise_proj group/agg cols → ratio_proj cols
	ColumnBindingReplacer replacer;
	for (idx_t gi = 0; gi < n_groups; gi++) {
		replacer.replacement_bindings.emplace_back(ColumnBinding(noise_proj.proj_idx, gi),
		                                           ColumnBinding(ratio_idx, gi));
	}
	for (idx_t ai = 0; ai < n_original_aggs; ai++) {
		replacer.replacement_bindings.emplace_back(ColumnBinding(noise_proj.proj_idx, n_groups + ai),
		                                           ColumnBinding(ratio_idx, n_groups + ai));
	}
	// Extra COUNT(*) positions are consumed inside ratio_proj — no upstream references
	replacer.stop_operator = ratio_ptr;
	replacer.VisitOperator(*plan);

	return {ratio_idx, ratio_ptr};
}

// ----------------------------------------------------------------------------
// Entry point
// ----------------------------------------------------------------------------

void CompileDPElasticQuery(const PrivacyCompatibilityResult &check, OptimizerExtensionInput &input,
                           unique_ptr<LogicalOperator> &plan, const vector<string> &privacy_units,
                           const string &query_hash) {
	(void)query_hash;
	PRIVACY_DEBUG_PRINT("[DP_ELASTIC] CompileDPElasticQuery: start");

	double epsilon = GetDpEpsilon(input.context, 1.0);
	if (epsilon <= 0.0 || !std::isfinite(epsilon)) {
		throw InvalidInputException("dp_elastic: dp_epsilon must be a positive finite number (got " +
		                            std::to_string(epsilon) + ")");
	}

	plan->ResolveOperatorTypes();

	auto eligibility = CheckDPEligibility(plan, privacy_units, check);
	auto *agg = eligibility.top_agg;

	// Rewrite AVG(x) → SUM(x) + COUNT(*) before bound/clipping checks.
	// Each AVG uses ε/2 per component so the combined cost is still ε-DP.
	auto avg_infos = RewriteAvgAggregates(input, agg);
	idx_t n_original_aggs = agg->expressions.size() - avg_infos.size();
	idx_t n_groups = agg->groups.size();

	Value threshold_val;
	double support_threshold = 0.0;
	bool apply_support_filter =
	    input.context.TryGetCurrentSetting("privacy_min_group_count", threshold_val) && !threshold_val.IsNull() &&
	    (support_threshold = threshold_val.GetValue<double>()) > 0.0 && std::isfinite(support_threshold);
	auto support_pos = AddSupportAggregate(input, plan, agg, eligibility.fk_chain, support_threshold);
	idx_t n_dp_aggs = support_pos.IsValid() ? support_pos.GetIndex() : agg->expressions.size();
	LogicalOperator *noise_anchor = agg;
	if (apply_support_filter) {
		noise_anchor = ApplySupportFilter(input, plan, agg, support_pos.GetIndex(), support_threshold);
	}

	// Build fast lookup: which positions are AVG-derived (need budget split)?
	std::unordered_set<idx_t> avg_positions;
	for (auto &info : avg_infos) {
		avg_positions.insert(info.sum_pos);
		avg_positions.insert(info.count_pos);
	}

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
	double es = ComputeElasticSensitivity(input.context, eligibility.fk_chain, epsilon);

	// When privacy_noise=false the compilation pipeline still runs (clipping, FK chain)
	// but Laplace scale is zeroed → dp_laplace_noise returns the value unchanged.
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
		if (avg_positions.count(ai)) {
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
		WrapAvgRatioProjection(input, plan, noise_proj, avg_infos, n_groups, n_original_aggs, noise_proj.proj_ptr);
	}

#if PRIVACY_DEBUG
	PRIVACY_DEBUG_PRINT("=== PLAN AFTER DP_ELASTIC TRANSFORMATION ===");
	plan->Print();
#endif
}

} // namespace duckdb
