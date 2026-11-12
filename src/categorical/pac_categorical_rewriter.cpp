//
// PAC Categorical Query Rewriter - Implementation
//
// See pac_categorical_rewriter.hpp for design documentation.
//
// Created by ila on 1/23/26.
//
#include "privacy_debug.hpp"
#include "categorical/pac_categorical_lambdas.hpp"
#include "aggregates/as_aggregate.hpp"
#include "utils/privacy_helpers.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"

namespace duckdb {

// Find counter bindings in an expression using the known counter_bindings set (no plan tracing).
// Used during the rewrite phase where counter_bindings tracks all bindings rewritten to counters.
vector<PacBindingInfo> FindCounterBindings(Expression *expr,
                                           const unordered_map<uint64_t, ColumnBinding> &counter_bindings) {
	vector<PacBindingInfo> result;
	unordered_map<uint64_t, idx_t> seen;
	std::function<void(Expression *)> Collect = [&](Expression *e) {
		if (e->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			auto &col_ref = e->Cast<BoundColumnRefExpression>();
			uint64_t h = HashBinding(col_ref.BindingMutable());
			if (counter_bindings.count(h) && !seen.count(h)) {
				PacBindingInfo info;
				info.binding = col_ref.BindingMutable();
				info.index = result.size();
				seen[h] = info.index;
				result.push_back(info);
			}
		}
		ExpressionIterator::EnumerateChildren(*e, [&](Expression &child) { Collect(&child); });
	};
	Collect(expr);
	return result;
}

// Register a binding as a counter binding, optionally tracing to its origin.
static void AddCounterBinding(unordered_map<uint64_t, ColumnBinding> &counter_bindings, const ColumnBinding &binding,
                              const ColumnBinding &origin = ColumnBinding()) {
	counter_bindings[HashBinding(binding)] = (origin.table_index.index != DConstants::INVALID_INDEX) ? origin : binding;
}

// For a projection expression, trace back through counter_bindings to find the origin binding.
// If the expression is a simple col_ref to a known counter, returns its tracked origin; otherwise returns the binding
// itself.
static ColumnBinding TraceCounterOrigin(const unique_ptr<Expression> &expr,
                                        const unordered_map<uint64_t, ColumnBinding> &counter_bindings) {
	if (expr->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		auto &col_ref = expr->Cast<BoundColumnRefExpression>();
		auto it = counter_bindings.find(HashBinding(col_ref.BindingMutable()));
		if (it != counter_bindings.end()) {
			return it->second;
		}
	}
	return ColumnBinding(); // invalid — AddCounterBinding will use the binding itself
}

// Check if an expression references any counter binding.
bool ReferencesCounterBinding(Expression *expr, const unordered_map<uint64_t, ColumnBinding> &counter_bindings) {
	if (expr->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		return counter_bindings.count(HashBinding(expr->Cast<BoundColumnRefExpression>().BindingMutable())) > 0;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(*expr, [&](Expression &child) {
		if (!found) {
			found = ReferencesCounterBinding(&child, counter_bindings);
		}
	});
	return found;
}

// with pac_*_list variants that aggregate element-wise
// Try to rebind an aggregate expression at index i to its _list variant.
// Returns true if the aggregate was successfully rebound.
// The aggregate's value child is extracted (dropping the hash for PAC aggregates),
// its type set to LIST<FLOAT>, and the aggregate rebound to the _list variant.
static bool TryRebindToListVariant(LogicalAggregate &agg, idx_t i, ClientContext &context) {
	auto &agg_expr = agg.expressions[i];
	if (agg_expr->GetExpressionType() != ExpressionType::BOUND_AGGREGATE) {
		return false;
	}
	auto &bound_agg = agg_expr->Cast<BoundAggregateExpression>();
	if (bound_agg.GetChildrenMutable().empty()) {
		return false;
	}
	// PAC/PAC_counters aggregates have (hash, value); standard aggregates have (value).
	// For _list variants we only need the value child.
	bool is_pac_style = (IsPacAggregate(bound_agg.FunctionMutable().GetName().GetIdentifierName()) ||
	                     IsPacCountersAggregate(bound_agg.FunctionMutable().GetName().GetIdentifierName())) &&
	                    bound_agg.GetChildrenMutable().size() > 1;
	idx_t value_child_idx = is_pac_style ? 1 : 0;
	string base_name = GetBasePacAggregateName(
	    bound_agg.FunctionMutable().GetName().GetIdentifierName()); // strips _counters if present
	string list_name = GetListAggregateVariant(base_name, GetPrivacyMode(context));
	if (list_name.empty()) {
		// Generic aggregate (e.g., first()) over counter input — rebind with same name
		if (bound_agg.GetChildrenMutable()[value_child_idx]->GetReturnType() ==
		    LogicalType::LIST(PacFloatLogicalType())) {
			list_name = bound_agg.FunctionMutable().GetName().GetIdentifierName();
		} else {
			return false;
		}
	}
	vector<unique_ptr<Expression>> children;
	auto child_copy = bound_agg.GetChildrenMutable()[value_child_idx]->Copy();
	children.push_back(std::move(child_copy));
	auto new_aggr = RebindAggregate(context, list_name, std::move(children), bound_agg.IsDistinct());
	if (!new_aggr) {
		return false;
	}
	agg.expressions[i] = std::move(new_aggr);
	idx_t types_index = agg.groups.size() + i;
	if (types_index < agg.types.size()) {
		agg.types[types_index] = LogicalType::LIST(PacFloatLogicalType());
	}
	return true;
}

// Replace aggregates whose input references a counter binding with _list variants.
// Called during RewriteBottomUp so projections above get properly list_transform'd.
// Adds output bindings of converted aggregates to counter_bindings.
static void ReplaceAggregatesOverCounters(LogicalOperator *op, ClientContext &context,
                                          unordered_map<uint64_t, ColumnBinding> &counter_bindings) {
	if (op->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return;
	}
	auto &agg = op->Cast<LogicalAggregate>();
	for (idx_t i = 0; i < agg.expressions.size(); i++) {
		auto &agg_expr = agg.expressions[i];
		if (agg_expr->GetExpressionType() != ExpressionType::BOUND_AGGREGATE) {
			continue;
		}
		auto &bound_agg = agg_expr->Cast<BoundAggregateExpression>();
		if (bound_agg.GetChildrenMutable().empty()) {
			continue;
		}
		idx_t value_child_idx = IsPacAggregate(bound_agg.FunctionMutable().GetName().GetIdentifierName()) &&
		                                bound_agg.GetChildrenMutable().size() > 1
		                            ? 1
		                            : 0;
		if (!ReferencesCounterBinding(bound_agg.GetChildrenMutable()[value_child_idx].get(), counter_bindings)) {
			continue;
		}
		if (TryRebindToListVariant(agg, i, context)) {
			AddCounterBinding(counter_bindings,
			                  ColumnBinding(TableIndex(agg.aggregate_index.index), ProjectionIndex(i)));
		}
	}
}

// Minimal processing for filter-pattern projection slots:
// update counter col_ref types but don't wrap with any terminal.
// The parent filter/join will expand these expressions and generate list_transform + priv_filter.
static void PrepareFilterPatternSlot(LogicalProjection &proj, idx_t i,
                                     const unordered_map<uint64_t, ColumnBinding> &counter_bindings) {
	auto &expr = proj.expressions[i];
	auto pac_bindings = FindCounterBindings(expr.get(), counter_bindings);
	if (pac_bindings.empty()) {
		return;
	}
	auto list_type = LogicalType::LIST(PacFloatLogicalType());
	// col_ref types are already fixed by the universal type fix above.
	// Update projection output type
	if (i < proj.types.size()) {
		if (expr->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			proj.types[i] = list_type;
		} else if (expr->GetExpressionType() == ExpressionType::BOUND_FUNCTION) {
			proj.types[i] = expr->GetReturnType();
		} else if (expr->GetExpressionType() == ExpressionType::CASE_EXPR &&
		           IsScalarSubqueryWrapper(expr->Cast<BoundCaseExpression>())) {
			// first(value) now returns a counter list, but DuckDB's scalar-subquery
			// cardinality-check CASE still has its original scalar result type.
			auto &wrapper = expr->Cast<BoundCaseExpression>();
			if (wrapper.Else().GetReturnType() == list_type) {
				wrapper.CaseChecksMutable()[0].then_expr = BoundCastExpression::AddDefaultCastToType(
				    std::move(wrapper.CaseChecksMutable()[0].then_expr), list_type);
				wrapper.SetReturnType(list_type);
				proj.types[i] = list_type;
				PRIVACY_DEBUG_PRINT("Categorical rewrite: preserving counter lists through scalar-subquery wrapper");
			}
		}
	}
}

// Unified expression rewrite: build list_transform over PAC counters and wrap.
// pac_bindings: all PAC aggregate bindings in the expression
// expr: the expression to transform (boolean for filter, numeric for projection)
// wrap_kind: determines terminal function and type parameters
// target_type: for PAC_NOISED, cast result to this type (ignored for PAC_FILTER/PAC_SELECT)
// priv_hash: for PAC_SELECT, the hash binding to pass to priv_select/priv_select_<cmp>
static unique_ptr<Expression>
RewriteExpressionWithCounters(OptimizerExtensionInput &input, const vector<PacBindingInfo> &pac_bindings,
                              Expression *expr, const unordered_map<uint64_t, ColumnBinding> &counter_bindings,
                              PacWrapKind wrap_kind, const LogicalType &target_type = PacFloatLogicalType(),
                              ColumnBinding priv_hash = ColumnBinding()) {
	if (pac_bindings.empty()) {
		return nullptr;
	}
	// Try optimized priv_{filter,select}_<cmp> for filter/select comparisons (single binding, simple comparison).
	// TryRewriteFilterComparison always returns the (possibly simplified) expression when it can
	// do any work. Check if the result is a priv_{filter,select}_<cmp> call (full optimization succeeded)
	// or a simplified comparison (partial — pass to lambda path which benefits from the rewriting).
	unique_ptr<Expression> simplified_expr;
	if (wrap_kind == PacWrapKind::PAC_FILTER || wrap_kind == PacWrapKind::PAC_SELECT) {
		auto cmp_result = TryRewriteFilterComparison(input, pac_bindings, expr, counter_bindings, wrap_kind, priv_hash);
		if (cmp_result) {
			if (cmp_result->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
				auto &func = cmp_result->Cast<BoundFunctionExpression>();
				if (func.FunctionMutable().GetName().GetIdentifierName().rfind("priv_filter_", 0) == 0 ||
				    func.FunctionMutable().GetName().GetIdentifierName().rfind("priv_select_", 0) == 0) {
					return cmp_result; // Full optimization to priv_{filter,select}_<cmp>
				}
			}
			simplified_expr = std::move(cmp_result); // Partial simplification — use for the lambda path
		}
	}
	Expression *expr_for_lambda = simplified_expr ? simplified_expr.get() : expr;

	LogicalType result_element_type =
	    (wrap_kind == PacWrapKind::PAC_NOISED) ? PacFloatLogicalType() : LogicalType::BOOLEAN;
	auto list_expr =
	    BuildCategoricalLambdas(input, pac_bindings, expr_for_lambda, counter_bindings, result_element_type);
	if (list_expr) {
		if (wrap_kind == PacWrapKind::PAC_NOISED) {
			auto noised = input.optimizer.BindScalarFunction("as_noised", std::move(list_expr));
			if (target_type != PacFloatLogicalType()) {
				noised = BoundCastExpression::AddDefaultCastToType(std::move(noised), target_type);
			}
			return noised;
		} else if (wrap_kind == PacWrapKind::PAC_SELECT) {
			auto hash_ref = make_uniq<BoundColumnRefExpression>("pac_pu", LogicalType::UBIGINT, priv_hash);
			return input.optimizer.BindScalarFunction("priv_select", std::move(hash_ref), std::move(list_expr));
		} else {
			return input.optimizer.BindScalarFunction("priv_filter", std::move(list_expr));
		}
	}
	return nullptr;
}

// Wrap counter column references with as_noised.
// After converting as_noised_sum → as_sum (LIST<FLOAT> output),
// expressions may still reference the aggregate with the original type (e.g. DECIMAL).
// This wraps those references with as_noised() to convert back to scalar, then casts to original type.
static void WrapCounterRefsWithNoised(unique_ptr<Expression> &expr,
                                      const unordered_map<uint64_t, ColumnBinding> &counter_bindings,
                                      OptimizerExtensionInput &input) {
	if (expr->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		auto &col_ref = expr->Cast<BoundColumnRefExpression>();
		if (counter_bindings.count(HashBinding(col_ref.BindingMutable()))) {
			auto original_type = col_ref.GetReturnType();
			auto list_type = LogicalType::LIST(PacFloatLogicalType());
			// If the universal type fix already changed the col_ref to LIST<FLOAT>,
			// we can't cast back to it — just use as_noised's natural scalar return type.
			bool needs_cast = (original_type != PacFloatLogicalType() && original_type != list_type);
			// Ensure col_ref type is LIST<FLOAT> for correct as_noised binding.
			col_ref.SetReturnType(list_type);
			unique_ptr<Expression> noised = input.optimizer.BindScalarFunction("as_noised", expr->Copy());
			if (needs_cast) {
				noised = BoundCastExpression::AddDefaultCastToType(std::move(noised), original_type);
			}
			expr = std::move(noised);
			return;
		}
	}
	ExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<Expression> &child) { WrapCounterRefsWithNoised(child, counter_bindings, input); });
}

// Rewrite a single projection expression: update col_ref types, build list_transform + as_noised terminal.
// Only called for non-filter-pattern projections (filter patterns are handled by the parent filter/join).
// Returns true if the slot remains a counter binding (non-terminal pass-through).
static bool RewriteProjectionExpression(OptimizerExtensionInput &input, LogicalProjection &proj, idx_t i,
                                        LogicalOperator *plan_root, bool is_terminal,
                                        const unordered_map<uint64_t, ColumnBinding> &counter_bindings) {
	auto &expr = proj.expressions[i];
	if (IsAlreadyWrappedInPacNoised(expr.get())) {
		return false;
	}
	auto pac_bindings = FindCounterBindings(expr.get(), counter_bindings);
	if (pac_bindings.empty()) {
		return false;
	}
	// Simple column reference to a counter list.
	// After the universal col_ref type fix, the col_ref type is LIST<FLOAT>.
	// proj.types[i] still holds the original scalar type (e.g. HUGEINT).
	if (expr->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		auto original_type = (i < proj.types.size()) ? proj.types[i] : expr->GetReturnType();
		if (is_terminal) {
			unique_ptr<Expression> result = input.optimizer.BindScalarFunction("as_noised", expr->Copy());
			if (original_type != PacFloatLogicalType()) {
				result = BoundCastExpression::AddDefaultCastToType(std::move(result), original_type);
			}
			proj.expressions[i] = std::move(result);
			proj.types[i] = original_type;
			return false; // terminal — scalar output
		} else if (i < proj.types.size()) {
			proj.types[i] = LogicalType::LIST(PacFloatLogicalType());
		}
		return true; // non-terminal pass-through — still a counter binding
	}
	if (!IsNumericalType(expr->GetReturnType())) {
		WrapCounterRefsWithNoised(expr, counter_bindings, input);
		return false;
	}
	// Arithmetic over PAC aggregates: build list_transform + as_noised terminal.
	Expression *expr_to_clone = expr.get();
	if (expr->GetExpressionType() == ExpressionType::OPERATOR_CAST &&
	    expr->Cast<BoundFunctionExpression>().GetReturnType() != PacFloatLogicalType()) {
		expr_to_clone = expr->Cast<BoundFunctionExpression>().GetChildrenMutable()[0].get();
	}
	if (!is_terminal) {
		auto result =
		    BuildCategoricalLambdas(input, pac_bindings, expr_to_clone, counter_bindings, PacFloatLogicalType());
		if (result) {
			PRIVACY_DEBUG_PRINT("Categorical rewrite: preserving per-world projection #" +
			                    std::to_string(proj.table_index.index) + "." + std::to_string(i));
			proj.expressions[i] = std::move(result);
			proj.types[i] = proj.expressions[i]->GetReturnType();
			return true;
		}
		return false;
	}
	auto result = RewriteExpressionWithCounters(input, pac_bindings, expr_to_clone, counter_bindings,
	                                            PacWrapKind::PAC_NOISED, expr->GetReturnType());
	if (result) {
		proj.expressions[i] = std::move(result);
		proj.types[i] = expr->GetReturnType();
	}
	return false;
}

// Insert a pass-through projection below filter_child_slot that replaces the hash column
// with pac_select_expr. Returns the new hash binding in the projection's output.
// Updates all bindings above via ColumnBindingReplacer.
static ColumnBinding InsertPacSelectProjection(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                                               unique_ptr<LogicalOperator> &filter_child_slot,
                                               const ColumnBinding &hash_binding,
                                               unique_ptr<Expression> pac_select_expr) {
	filter_child_slot->ResolveOperatorTypes();
	auto old_bindings = filter_child_slot->GetColumnBindings();
	idx_t num_cols = old_bindings.size();

	idx_t proj_table_index = input.optimizer.binder.GenerateTableIndex().index;
	vector<unique_ptr<Expression>> proj_expressions;

	// Find which position the hash binding occupies
	idx_t hash_position = num_cols;
	for (idx_t i = 0; i < num_cols; i++) {
		if (old_bindings[i] == hash_binding) {
			hash_position = i;
			break;
		}
	}
	for (idx_t i = 0; i < num_cols; i++) {
		if (i == hash_position) {
			proj_expressions.push_back(nullptr); // placeholder — filled below
		} else {
			proj_expressions.push_back(
			    make_uniq<BoundColumnRefExpression>(filter_child_slot->types[i], old_bindings[i]));
		}
	}
	if (hash_position < num_cols) {
		proj_expressions[hash_position] = std::move(pac_select_expr);
	} else {
		// Hash binding was not in direct output — append it
		proj_expressions.push_back(std::move(pac_select_expr));
	}

	auto projection = make_uniq<LogicalProjection>(TableIndex(proj_table_index), std::move(proj_expressions));
	projection->children.push_back(std::move(filter_child_slot));
	projection->ResolveOperatorTypes();

	LogicalOperator *proj_ptr = projection.get();
	filter_child_slot = std::move(projection);

	// Remap old bindings → new projection bindings
	ColumnBindingReplacer replacer;
	for (idx_t i = 0; i < num_cols; i++) {
		replacer.replacement_bindings.emplace_back(old_bindings[i],
		                                           ColumnBinding(TableIndex(proj_table_index), ProjectionIndex(i)));
	}
	replacer.stop_operator = proj_ptr;
	replacer.VisitOperator(*plan);

	return ColumnBinding(TableIndex(proj_table_index), ProjectionIndex(hash_position));
}

// CTE readers have independent bindings and cache their input types. Preserve
// counter lineage across the materialization boundary before rewriting consumers.
static void SyncCTECounterBindings(LogicalOperator &op, LogicalMaterializedCTE &cte,
                                   unordered_map<uint64_t, ColumnBinding> &counter_bindings) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &ref = op.Cast<LogicalCTERef>();
		if (ref.cte_index == cte.table_index) {
			auto bindings = cte.children[0]->GetColumnBindings();
			for (idx_t i = 0; i < bindings.size() && i < ref.chunk_types.size(); i++) {
				auto origin = counter_bindings.find(HashBinding(bindings[i]));
				if (origin != counter_bindings.end()) {
					ref.chunk_types[i] = cte.children[0]->types[i];
					AddCounterBinding(counter_bindings, ColumnBinding(ref.table_index, ProjectionIndex(i)),
					                  origin->second);
				}
			}
			ref.ResolveOperatorTypes();
		}
	}
	for (auto &child : op.children) {
		SyncCTECounterBindings(*child, cte, counter_bindings);
	}
}

// Single bottom-up rewrite pass.
// Processes children first, then current operator. Handles:
// - AGGREGATE: convert as_noised_sum → as_sum (counters), then aggregate-over-counters → as_sum (list)
// - PROJECTION: update simple col_ref types, build list_transform + as_noised for arithmetic
// - FILTER (in rewrite_map): build list_transform + priv_filter
// - JOIN (in rewrite_map): rewrite conditions (two-list → CROSS_PRODUCT+FILTER, single-list → double-lambda)
static void RewriteBottomUp(unique_ptr<LogicalOperator> &op_ptr, OptimizerExtensionInput &input,
                            unique_ptr<LogicalOperator> &plan,
                            const unordered_map<LogicalOperator *, unordered_set<idx_t>> &pattern_lookup,
                            vector<CategoricalPatternInfo> &patterns, unordered_set<uint64_t> &replaced_mark_bindings,
                            unordered_map<uint64_t, ColumnBinding> &counter_bindings,
                            bool inside_cte_definition = false) {
	auto *op = op_ptr.get();
	for (idx_t ci = 0; ci < op->children.size(); ci++) { // Recurse into children first (bottom-up)
		// Child 0 of MATERIALIZED_CTE is the CTE definition — its output types must remain
		// stable (numeric, not LIST) because CTE_SCAN consumers may re-aggregate the results.
		bool is_cte = op->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE;
		bool generated_correlation_cte =
		    is_cte &&
		    StringUtil::StartsWith(op->Cast<LogicalMaterializedCTE>().ctename.GetIdentifierName(), "__duckdb_delim_");
		bool child_in_cte = inside_cte_definition || (is_cte && ci == 0 && !generated_correlation_cte);
		if (is_cte && ci == 1) {
			op->children[0]->ResolveOperatorTypes();
			SyncCTECounterBindings(*op->children[1], op->Cast<LogicalMaterializedCTE>(), counter_bindings);
		}
		RewriteBottomUp(op->children[ci], input, plan, pattern_lookup, patterns, replaced_mark_bindings,
		                counter_bindings, child_in_cte);
	}
	// After processing children, check if this FILTER references a mark column
	// from a MARK_JOIN that was replaced by CROSS_PRODUCT + FILTER(priv_filter_eq).
	// The priv_filter_eq below already handles the filtering, so this FILTER is
	// now redundant with a dangling mark column reference. Remove it.
	if (op->type == LogicalOperatorType::LOGICAL_FILTER && !replaced_mark_bindings.empty()) {
		auto &filter = op->Cast<LogicalFilter>();
		bool has_dangling_mark = false;
		for (auto &fexpr : filter.expressions) {
			if (fexpr->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
				auto &ref = fexpr->Cast<BoundColumnRefExpression>();
				if (replaced_mark_bindings.count(HashBinding(ref.BindingMutable()))) {
					has_dangling_mark = true;
					break;
				}
			}
		}
		if (has_dangling_mark && !filter.children.empty()) {
			op_ptr = std::move(filter.children[0]);
			return;
		}
	}
	LogicalOperator *plan_root = plan.get();
	// Handle non-categorical FILTER (HAVING) BEFORE the universal type fix,
	// so col_ref types still hold the original scalar type (e.g. DECIMAL).
	// WrapCounterRefsWithNoised will set the col_ref type to LIST<FLOAT> for as_noised binding,
	// then cast the result back to the original scalar type so the parent comparison stays valid.
	if (op->type == LogicalOperatorType::LOGICAL_FILTER && !inside_cte_definition && !counter_bindings.empty()) {
		auto it = pattern_lookup.find(op);
		if (it == pattern_lookup.end()) {
			auto &filter = op->Cast<LogicalFilter>();
			for (auto &filter_expr : filter.expressions) {
				WrapCounterRefsWithNoised(filter_expr, counter_bindings, input);
			}
		}
	}
	// Universal col_ref type fix: after children have been processed (and populated counter_bindings),
	// fix stale col_ref types in the current operator's expressions. When PAC aggregates are converted
	// to counters, output types change (e.g. DECIMAL → LIST<FLOAT>). Expressions in all operators
	// may reference the old types. Fix them before operator-specific semantic work.
	// Skip universal type fix for ordering/limit operators — they need scalar types.
	// The terminal projection below them handles as_noised wrapping.
	bool skip_type_fix =
	    (op->type == LogicalOperatorType::LOGICAL_ORDER_BY || op->type == LogicalOperatorType::LOGICAL_TOP_N ||
	     op->type == LogicalOperatorType::LOGICAL_LIMIT);
	if (!inside_cte_definition && !counter_bindings.empty() && !skip_type_fix) {
		auto list_type = LogicalType::LIST(PacFloatLogicalType());
		LogicalOperatorVisitor::EnumerateExpressions(*op, [&](unique_ptr<Expression> *expr_ptr) {
			std::function<void(unique_ptr<Expression> &)> Fix = [&](unique_ptr<Expression> &e) {
				if (e->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
					auto &col_ref = e->Cast<BoundColumnRefExpression>();
					if (counter_bindings.count(HashBinding(col_ref.BindingMutable()))) {
						col_ref.SetReturnType(list_type);
					}
				}
				ExpressionIterator::EnumerateChildren(*e, Fix);
			};
			Fix(*expr_ptr);
		});
	}
	if (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY && !inside_cte_definition) {
		// === AGGREGATE: convert PAC aggregates to _counters, then check aggregates-over-counters ===
		auto &agg = op->Cast<LogicalAggregate>();

		// First: replace standard/PAC aggregates whose input references counter bindings
		// with _list variants. Must happen before _counters conversion below so that
		// the projection rewrite (later in this pass) sees _list outputs.
		ReplaceAggregatesOverCounters(op, input.context, counter_bindings);

		// Convert PAC aggregates to _counters variants — only the specific
		// expressions that are sources of categorical patterns.
		for (idx_t i = 0; i < agg.expressions.size(); i++) {
			auto &agg_expr = agg.expressions[i];
			if (agg_expr->GetExpressionType() != ExpressionType::BOUND_AGGREGATE) {
				continue;
			}
			auto &bound_agg = agg_expr->Cast<BoundAggregateExpression>();
			if (!IsPacAggregate(bound_agg.FunctionMutable().GetName().GetIdentifierName())) {
				continue;
			}
			// Check if this specific aggregate expression is a source of any pattern
			ColumnBinding this_binding {TableIndex(agg.aggregate_index.index), ProjectionIndex(i)};
			bool convert = false;
			for (auto &p : patterns) {
				for (auto &bi : p.pac_bindings) {
					if (bi.source_binding == this_binding) {
						convert = true;
						break;
					}
				}
				if (convert) {
					break;
				}
			}
			if (!convert) {
				continue;
			}
			string counters_name = GetCountersVariant(bound_agg.FunctionMutable().GetName().GetIdentifierName());
			vector<unique_ptr<Expression>> children;
			for (auto &child_expr : bound_agg.GetChildrenMutable()) {
				children.push_back(child_expr->Copy());
			}
			auto new_aggr = RebindAggregate(input.context, counters_name, std::move(children), bound_agg.IsDistinct());
			if (!new_aggr) { // Fallback: just rename the function in place
				bound_agg.FunctionMutable().SetName(Identifier(counters_name));
				bound_agg.FunctionMutable().SetReturnType(LogicalType::LIST(PacFloatLogicalType()));
				agg_expr->SetReturnType(LogicalType::LIST(PacFloatLogicalType()));
			} else {
				agg.expressions[i] = std::move(new_aggr);
			}
			AddCounterBinding(counter_bindings,
			                  ColumnBinding(TableIndex(agg.aggregate_index.index), ProjectionIndex(i)));
			idx_t types_index = agg.groups.size() + i;
			if (types_index < agg.types.size()) {
				agg.types[types_index] = LogicalType::LIST(PacFloatLogicalType());
			}
		}
	} else if (op->type == LogicalOperatorType::LOGICAL_PROJECTION &&
	           !inside_cte_definition) { // === PROJECTION: rewrite PAC expressions ===
		auto &proj = op->Cast<LogicalProjection>();
		// Intermediate projections compute valid per-world values; only the
		// user-visible output projection finalizes them into scalar answers.
		auto *output = plan_root;
		while (output && !output->children.empty()) {
			if (output->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
				output = output->children[1].get();
			} else if (output->type == LogicalOperatorType::LOGICAL_ORDER_BY ||
			           output->type == LogicalOperatorType::LOGICAL_TOP_N ||
			           output->type == LogicalOperatorType::LOGICAL_LIMIT) {
				output = output->children[0].get();
			} else {
				break;
			}
		}
		for (idx_t i = 0; i < proj.expressions.size(); i++) {
			if (RewriteProjectionExpression(input, proj, i, plan_root, output == op, counter_bindings)) {
				AddCounterBinding(counter_bindings, ColumnBinding(proj.table_index, ProjectionIndex(i)),
				                  TraceCounterOrigin(proj.expressions[i], counter_bindings));
			}
		}
	} else if (op->type == LogicalOperatorType::LOGICAL_FILTER &&
	           !inside_cte_definition) { // === FILTER: rewrite expressions with priv_filter ===
		auto it = pattern_lookup.find(op);
		if (it != pattern_lookup.end()) {
			auto &filter = op->Cast<LogicalFilter>();
			for (auto expr_idx : it->second) {
				if (expr_idx >= filter.expressions.size()) {
					continue;
				}
				auto &filter_expr = filter.expressions[expr_idx];
				// Projection arithmetic has already been lifted to per-world lists.
				auto pac_bindings = FindCounterBindings(filter_expr.get(), counter_bindings);
				if (pac_bindings.empty()) {
					continue;
				}
				// Determine wrap kind: PAC_SELECT if outer pac aggregate exists and setting enabled.
				// HAVING filters are excluded during detection, so any FILTER pattern here
				// is a genuine categorical filter (WHERE or subquery predicate).
				PacWrapKind wrap_kind = PacWrapKind::PAC_FILTER;
				ColumnBinding pattern_hash;
				for (auto &p : patterns) {
					if (p.parent_op == op && p.has_outer_pac_hash) {
						pattern_hash = p.outer_pac_hash;
						if (GetBooleanSetting(input.context, "priv_select", true)) {
							wrap_kind = PacWrapKind::PAC_SELECT;
						}
						break;
					}
				}
				auto result = RewriteExpressionWithCounters(input, pac_bindings, filter_expr.get(), counter_bindings,
				                                            wrap_kind, PacFloatLogicalType(), pattern_hash);
				if (result) {
					if (wrap_kind == PacWrapKind::PAC_SELECT) {
						// Insert projection below filter replacing hash with priv_select result
						auto new_hash =
						    InsertPacSelectProjection(input, plan, op->children[0], pattern_hash, std::move(result));
						// Replace filter condition with priv_hash <> 0 for majority-vote decision
						filter.expressions[expr_idx] = BoundComparisonExpression::Create(
						    ExpressionType::COMPARE_NOTEQUAL,
						    make_uniq<BoundColumnRefExpression>("pac_pu", LogicalType::UBIGINT, new_hash),
						    make_uniq<BoundConstantExpression>(Value::UBIGINT(0)));
					} else {
						filter.expressions[expr_idx] = std::move(result);
					}
				}
			}
		}
	} else if ((op->type == LogicalOperatorType::LOGICAL_ORDER_BY || op->type == LogicalOperatorType::LOGICAL_TOP_N) &&
	           !inside_cte_definition && !counter_bindings.empty()) {
		// ORDER BY / TOP N that references counter bindings need scalar values.
		// col_ref types are still scalar (skip_type_fix), so WrapCounterRefsWithNoised
		// fixes types to LIST<FLOAT> for as_noised binding and casts back to the original type.
		if (op->type == LogicalOperatorType::LOGICAL_ORDER_BY) {
			auto &order = op->Cast<LogicalOrder>();
			for (auto &o : order.orders) {
				WrapCounterRefsWithNoised(o.expression, counter_bindings, input);
			}
		} else {
			auto &topn = op->Cast<LogicalTopN>();
			for (auto &o : topn.orders) {
				WrapCounterRefsWithNoised(o.expression, counter_bindings, input);
			}
		}
	} else if ((op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
	            op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN) &&
	           !inside_cte_definition) { // === JOIN: rewrite comparison conditions ===
		auto it = pattern_lookup.find(op);
		if (it == pattern_lookup.end()) {
			return;
		}
		// Determine wrap kind for join patterns (same logic as FILTER above)
		PacWrapKind join_wrap_kind = PacWrapKind::PAC_FILTER;
		ColumnBinding join_pattern_hash;
		for (auto &p : patterns) {
			if (p.parent_op == op && p.has_outer_pac_hash) {
				join_pattern_hash = p.outer_pac_hash;
				if (GetBooleanSetting(input.context, "priv_select", true)) {
					join_wrap_kind = PacWrapKind::PAC_SELECT;
				}
				break;
			}
		}
		auto &join = op->Cast<LogicalComparisonJoin>();
		unique_ptr<Expression> comparison;
		vector<unique_ptr<Expression>> ordinary_conditions;
		for (auto &condition : join.conditions) {
			auto expression = BoundComparisonExpression::Create(
			    condition.GetComparisonType(), condition.LeftReference()->Copy(), condition.RightReference()->Copy());
			if (!ReferencesCounterBinding(expression.get(), counter_bindings)) {
				ordinary_conditions.push_back(std::move(expression));
			} else if (comparison) {
				comparison = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
				                                                   std::move(comparison), std::move(expression));
			} else {
				comparison = std::move(expression);
			}
		}
		if (!comparison) {
			return;
		}
		// Evaluate all world-dependent conditions together, then make one
		// categorical decision. Keep ordinary join predicates outside the lambda
		// so DuckDB can still recover its usual hash joins during optimization.
		auto bindings = FindCounterBindings(comparison.get(), counter_bindings);
		auto pac_expr = RewriteExpressionWithCounters(input, bindings, comparison.get(), counter_bindings,
		                                              join_wrap_kind, PacFloatLogicalType(), join_pattern_hash);
		if (!pac_expr) {
			return;
		}
		auto cross_product = LogicalCrossProduct::Create(std::move(join.children[0]), std::move(join.children[1]));
		auto filter = make_uniq<LogicalFilter>();
		if (join_wrap_kind == PacWrapKind::PAC_SELECT) {
			auto new_hash =
			    InsertPacSelectProjection(input, plan, cross_product, join_pattern_hash, std::move(pac_expr));
			filter->expressions.push_back(BoundComparisonExpression::Create(
			    ExpressionType::COMPARE_NOTEQUAL,
			    make_uniq<BoundColumnRefExpression>("pac_pu", LogicalType::UBIGINT, new_hash),
			    make_uniq<BoundConstantExpression>(Value::UBIGINT(0))));
		} else {
			filter->expressions.push_back(std::move(pac_expr));
		}
		for (auto &condition : ordinary_conditions) {
			filter->expressions.push_back(std::move(condition));
		}
		filter->children.push_back(std::move(cross_product));
		for (auto &pattern : patterns) {
			if (pattern.parent_op == op) {
				pattern.parent_op = nullptr;
			}
		}
		if (join.join_type == JoinType::MARK) {
			replaced_mark_bindings.insert(HashBinding(ColumnBinding(join.mark_index, ProjectionIndex(0))));
		}
		op_ptr = std::move(filter);
	}
}

// Convert aggregates whose value child references a counter binding to _list variants.
// Catches cases ReplaceAggregatesOverCounters misses (DELIM_JOIN paths where tracing
// through the plan fails but counter_bindings has the correct information).
static void ConvertPacAggregatesToListVariants(LogicalOperator &plan, ClientContext &context,
                                               unordered_map<uint64_t, ColumnBinding> &counter_bindings) {
	for (auto &child : plan.children) {
		ConvertPacAggregatesToListVariants(*child, context, counter_bindings);
	}
	if (plan.type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return;
	}
	auto &agg = plan.Cast<LogicalAggregate>();
	for (idx_t i = 0; i < agg.expressions.size(); i++) {
		if (agg.expressions[i]->GetExpressionType() != ExpressionType::BOUND_AGGREGATE) {
			continue;
		}
		if (counter_bindings.count(
		        HashBinding(ColumnBinding(TableIndex(agg.aggregate_index.index), ProjectionIndex(i))))) {
			continue;
		}
		auto &bound_agg = agg.expressions[i]->Cast<BoundAggregateExpression>();
		if (bound_agg.GetChildrenMutable().empty() ||
		    IsPacListAggregate(bound_agg.FunctionMutable().GetName().GetIdentifierName())) {
			continue;
		}
		idx_t value_idx = (IsPacAggregate(bound_agg.FunctionMutable().GetName().GetIdentifierName()) ||
		                   IsPacCountersAggregate(bound_agg.FunctionMutable().GetName().GetIdentifierName())) &&
		                          bound_agg.GetChildrenMutable().size() > 1
		                      ? 1
		                      : 0;
		if (!ReferencesCounterBinding(bound_agg.GetChildrenMutable()[value_idx].get(), counter_bindings)) {
			continue;
		}
		if (TryRebindToListVariant(agg, i, context)) {
			AddCounterBinding(counter_bindings,
			                  ColumnBinding(TableIndex(agg.aggregate_index.index), ProjectionIndex(i)));
		}
	}
}

void RewriteCategoricalQuery(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                             const PacAggregateInfoMap &pac_agg_info) {
	// Detect categorical patterns
	vector<CategoricalPatternInfo> patterns;
	DetectCategoricalPatterns(plan.get(), plan.get(), patterns, false, pac_agg_info);
	if (patterns.empty()) {
		return;
	}
	// Build lightweight lookup: operator → set of expression indices
	unordered_map<LogicalOperator *, unordered_set<idx_t>> pattern_lookup;
	for (auto &p : patterns) {
		if (p.parent_op) {
			pattern_lookup[p.parent_op].insert(p.expr_index);
		}
	}
	// Bottom-up rewrite pass
	// - Aggregates: as_noised_sum → as_sum (counters), then aggregate-over-counters → as_sum (list)
	// - Projections: update col_ref types, build list_transform + as_noised/pass-through
	// - Filters: build list_transform + priv_filter
	// - Joins: rewrite conditions (two-list → CROSS_PRODUCT+FILTER, single-list → double-lambda)
	unordered_set<uint64_t> replaced_mark_bindings;
	unordered_map<uint64_t, ColumnBinding> counter_bindings;
	RewriteBottomUp(plan, input, plan, pattern_lookup, patterns, replaced_mark_bindings, counter_bindings);
	// Fix up aggregates that now consume LIST (from inner _counters) to _list variants.
	ConvertPacAggregatesToListVariants(*plan, input.context, counter_bindings);
	plan->ResolveOperatorTypes();
}

} // namespace duckdb
