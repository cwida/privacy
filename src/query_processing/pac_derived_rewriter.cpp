//
// PAC Derived Table Rewriter
//
// Handles read and write paths for derived_pu tables (materialized views of PU data).
// Derived_pu tables store PAC counter lists (LIST<FLOAT>, 64 elements) instead of scalar values.
//
// Write path (ConvertDerivedPuToCounters):
//   Converts as_noised_* aggregates to as_* counter variants in INSERT/CTAS plans,
//   so the table stores raw counter lists instead of finalized noised scalars.
//
// Read path (InjectPacFinalizeForDerivedPu):
//   Rewrites comparisons on counter columns to priv_filter_<cmp>(scalar, counters)
//   calls, which evaluate all 64 subsamples via majority voting. Non-comparison
//   contexts (projections, ORDER BY) are wrapped with priv_finalize() instead.
//   Runs as a post-optimizer rule (after DuckDB's built-in optimizers).
//
// Key challenges:
//   - DuckDB's optimizer reorders columns and inserts intermediate projections,
//     so we use op->types (resolved output) for binding identification rather than
//     position-based matching against the original GET.
//   - DuckDB's filter pushdown may push comparisons (including BETWEEN) into
//     GET.table_filters before our post-optimizer runs. RewriteCounterFiltersFromGets
//     pulls these out and converts them to priv_filter_<cmp> calls.
//   - AGGREGATE operators (e.g. first() in scalar subqueries) need raw FLOAT[] input,
//     so priv_finalize injection is suppressed inside aggregate subtrees.
//

#include "query_processing/pac_derived_rewriter.hpp"

#include "aggregates/as_aggregate.hpp"
#include "categorical/pac_categorical_detection.hpp"
#include "categorical/pac_categorical_rewriter.hpp"
#include "metadata/privacy_metadata_manager.hpp"
#include "privacy_debug.hpp"
#include "utils/privacy_helpers.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

// --- Helpers ---

static LogicalType CounterListType() {
	return LogicalType::LIST(PacFloatLogicalType());
}

// Returns true if the expression tree contains a priv_finalize call.
static bool ExpressionContainsPacFinalize(Expression &e) {
	if (e.GetExpressionType() == ExpressionType::BOUND_FUNCTION &&
	    e.Cast<BoundFunctionExpression>().FunctionMutable().GetName().GetIdentifierName() == "priv_finalize") {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(e, [&](Expression &child) {
		if (!found && ExpressionContainsPacFinalize(child)) {
			found = true;
		}
	});
	return found;
}

static bool ExpressionReferencesFinalizedBinding(Expression &e, const unordered_set<uint64_t> &finalized_bindings) {
	if (e.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		auto &col_ref = e.Cast<BoundColumnRefExpression>();
		return finalized_bindings.count(HashBinding(col_ref.BindingMutable())) > 0;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(e, [&](Expression &child) {
		if (!found && ExpressionReferencesFinalizedBinding(child, finalized_bindings)) {
			found = true;
		}
	});
	return found;
}

// ============================================================================
// Write path: convert as_noised_* → as_* counter variants for derived_pu DML
// ============================================================================

static void ConvertAggregatesRecursive(OptimizerExtensionInput &input, LogicalOperator *op,
                                       unordered_set<uint64_t> &converted) {
	if (!op) {
		return;
	}
	for (auto &child : op->children) {
		ConvertAggregatesRecursive(input, child.get(), converted);
	}
	if (op->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return;
	}

	auto &agg = op->Cast<LogicalAggregate>();
	auto list_type = CounterListType();

	for (idx_t i = 0; i < agg.expressions.size(); i++) {
		auto &expr = agg.expressions[i];
		if (expr->GetExpressionType() != ExpressionType::BOUND_AGGREGATE) {
			continue;
		}

		auto &bound_agg = expr->Cast<BoundAggregateExpression>();
		if (!IsPacAggregate(bound_agg.FunctionMutable().GetName().GetIdentifierName())) {
			continue;
		}

		string counters_name = GetCountersVariant(bound_agg.FunctionMutable().GetName().GetIdentifierName());
		if (counters_name.empty()) {
			continue;
		}

		vector<unique_ptr<Expression>> children;
		for (auto &child_expr : bound_agg.GetChildrenMutable()) {
			children.push_back(child_expr->Copy());
		}

		auto new_aggr = RebindAggregate(input.context, counters_name, std::move(children), bound_agg.IsDistinct());
		if (new_aggr) {
			PRIVACY_DEBUG_PRINT("[PAC DERIVED WRITE] Rebound " + bound_agg.Function().GetName().GetIdentifierName() +
			                    " → " +
			                    new_aggr->Cast<BoundAggregateExpression>().Function().GetName().GetIdentifierName());
			agg.expressions[i] = std::move(new_aggr);
		} else {
			bound_agg.FunctionMutable().SetName(Identifier(counters_name));
			bound_agg.FunctionMutable().SetReturnType(list_type);
			expr->SetReturnType(list_type);
		}

		idx_t types_index = agg.groups.size() + i;
		if (types_index < agg.types.size()) {
			agg.types[types_index] = list_type;
		}

		converted.insert(HashBinding(ColumnBinding(TableIndex(agg.aggregate_index.index), ProjectionIndex(i))));
	}
}

static void FixStaleColumnRefTypes(LogicalOperator *op, const unordered_set<uint64_t> &converted) {
	if (!op) {
		return;
	}
	auto list_type = CounterListType();
	LogicalOperatorVisitor::EnumerateExpressions(*op, [&](unique_ptr<Expression> *expr_ptr) {
		std::function<void(unique_ptr<Expression> &)> Fix = [&](unique_ptr<Expression> &e) {
			if (e->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
				auto &col_ref = e->Cast<BoundColumnRefExpression>();
				if (converted.count(HashBinding(col_ref.BindingMutable()))) {
					col_ref.SetReturnType(list_type);
				}
			}
			ExpressionIterator::EnumerateChildren(*e, Fix);
		};
		Fix(*expr_ptr);
	});
	for (auto &child : op->children) {
		FixStaleColumnRefTypes(child.get(), converted);
	}
}

static void StripRedundantCasts(LogicalOperator *op) {
	if (!op) {
		return;
	}
	for (auto &child : op->children) {
		StripRedundantCasts(child.get());
	}
	std::function<void(unique_ptr<Expression> &)> Strip = [&](unique_ptr<Expression> &e) {
		ExpressionIterator::EnumerateChildren(*e, Strip);
		if (e->GetExpressionType() == ExpressionType::OPERATOR_CAST) {
			auto &cast = e->Cast<BoundFunctionExpression>();
			if (cast.GetChildrenMutable()[0]->GetReturnType() == cast.GetReturnType()) {
				e = std::move(cast.GetChildrenMutable()[0]);
			}
		}
	};
	LogicalOperatorVisitor::EnumerateExpressions(*op, [&](unique_ptr<Expression> *expr_ptr) { Strip(*expr_ptr); });
}

void ConvertDerivedPuToCounters(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	unordered_set<uint64_t> converted;
	ConvertAggregatesRecursive(input, plan.get(), converted);
	if (!converted.empty()) {
		FixStaleColumnRefTypes(plan.get(), converted);
		StripRedundantCasts(plan.get());
	}
}

// ============================================================================
// Read path: inject priv_finalize for SELECT on derived_pu tables
// ============================================================================

// Find all GET operators that scan derived_pu tables with counter columns.
static void FindDerivedPuGets(LogicalOperator *op, unordered_set<idx_t> &derived_table_indices) {
	if (!op) {
		return;
	}
	for (auto &child : op->children) {
		FindDerivedPuGets(child.get(), derived_table_indices);
	}
	if (op->type != LogicalOperatorType::LOGICAL_GET) {
		return;
	}
	auto &get = op->Cast<LogicalGet>();
	auto entry = get.GetTable();
	if (!entry) {
		return;
	}
	auto &mgr = PrivacyMetadataManager::Get();
	auto *meta = mgr.GetTableMetadata(entry->name.GetIdentifierName());
	if (!meta || !meta->derived_pu) {
		return;
	}
	auto list_type = CounterListType();
	for (idx_t i = 0; i < get.returned_types.size(); i++) {
		if (get.returned_types[i] == list_type) {
			derived_table_indices.insert(get.table_index.index);
			return;
		}
	}
}

// Returns true if the expression is (or contains at its root) a counter column ref.
static bool IsCounterColumnRef(Expression &e, const LogicalType &list_type) {
	if (e.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF && e.GetReturnType() == list_type) {
		return true;
	}
	// Also catch CAST(counter_col → scalar) inserted by the binder
	if (e.GetExpressionType() == ExpressionType::OPERATOR_CAST) {
		auto &cast = e.Cast<BoundFunctionExpression>();
		return IsCounterColumnRef(*cast.GetChildrenMutable()[0], list_type);
	}
	return false;
}

// Extract the raw counter column ref (stripping binder-inserted casts).
static unique_ptr<Expression> ExtractCounterRef(unique_ptr<Expression> e, const LogicalType &list_type) {
	if (e->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF && e->GetReturnType() == list_type) {
		return e;
	}
	if (e->GetExpressionType() == ExpressionType::OPERATOR_CAST) {
		auto &cast = e->Cast<BoundFunctionExpression>();
		return ExtractCounterRef(std::move(cast.GetChildrenMutable()[0]), list_type);
	}
	return e;
}

// Ensure the scalar side is cast to PAC_FLOAT for priv_filter_<cmp>.
// Strips binder-inserted CAST(scalar→FLOAT[]) and FLOAT[] constants back to scalars.
static unique_ptr<Expression> CastScalarToPacFloat(unique_ptr<Expression> scalar, const LogicalType &pac_float_type,
                                                   const LogicalType &list_type) {
	if (scalar->GetReturnType() == pac_float_type) {
		return scalar;
	}
	// Strip any binder-inserted CAST to FLOAT[] (scalar→LIST<FLOAT>)
	if (scalar->GetExpressionType() == ExpressionType::OPERATOR_CAST) {
		auto &cast = scalar->Cast<BoundFunctionExpression>();
		if (cast.GetReturnType() == list_type && cast.GetChildrenMutable()[0]->GetReturnType() != list_type) {
			scalar = std::move(cast.GetChildrenMutable()[0]);
		}
	}
	// Also unwrap FLOAT[] constants back to scalar
	if (scalar->GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
		auto &constant = scalar->Cast<BoundConstantExpression>();
		if (constant.GetReturnType() == list_type && !constant.GetValue().IsNull()) {
			auto &list_children = ListValue::GetChildren(constant.GetValue());
			if (!list_children.empty()) {
				auto scalar_val = list_children[0].DefaultCastAs(pac_float_type);
				return make_uniq<BoundConstantExpression>(std::move(scalar_val));
			}
		}
	}
	if (scalar->GetReturnType() != pac_float_type) {
		return BoundCastExpression::AddDefaultCastToType(std::move(scalar), pac_float_type);
	}
	return scalar;
}

static void SyncMaterializedCTERefTypes(LogicalOperator *op, idx_t cte_index, const vector<LogicalType> &cte_types,
                                        const LogicalType &list_type, const LogicalType &finalized_type,
                                        unordered_set<uint64_t> &finalized_bindings) {
	if (!op) {
		return;
	}
	if (op->type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &cte_ref = op->Cast<LogicalCTERef>();
		if (cte_ref.cte_index.index == cte_index) {
			auto bindings = cte_ref.GetColumnBindings();
			for (idx_t i = 0; i < cte_types.size() && i < cte_ref.chunk_types.size(); i++) {
				if (cte_ref.chunk_types[i] == list_type && cte_types[i] == finalized_type && i < bindings.size()) {
					finalized_bindings.insert(HashBinding(bindings[i]));
				}
				cte_ref.chunk_types[i] = cte_types[i];
				if (i < cte_ref.types.size()) {
					cte_ref.types[i] = cte_types[i];
				}
			}
		}
	}
	for (auto &child : op->children) {
		SyncMaterializedCTERefTypes(child.get(), cte_index, cte_types, list_type, finalized_type, finalized_bindings);
	}
}

// Rewrite a comparison expression on a counter column to a priv_filter_<cmp> call.
// Returns the rewritten expression, or nullptr if the comparison doesn't involve a counter column.
static unique_ptr<Expression> RewriteComparisonToFilterCmp(OptimizerExtensionInput &input, BoundFunctionExpression &cmp,
                                                           const LogicalType &list_type,
                                                           const LogicalType &finalized_type) {
	bool left_is_counter = IsCounterColumnRef(*cmp.GetChildrenMutable()[0], list_type);
	bool right_is_counter = IsCounterColumnRef(*cmp.GetChildrenMutable()[1], list_type);
	if (!left_is_counter && !right_is_counter) {
		return nullptr;
	}
	// Both sides are counters — can't rewrite
	if (left_is_counter && right_is_counter) {
		return nullptr;
	}
	bool counter_on_left = left_is_counter;
	ExpressionType effective = counter_on_left ? FlipComparison(cmp.GetExpressionType()) : cmp.GetExpressionType();
	const char *resolved = GetPacFilterCmpName(effective, GetPrivacyMode(input.context));
	if (!resolved) {
		return nullptr;
	}
	string func_name = resolved;
	auto counter_ref = ExtractCounterRef(
	    std::move(counter_on_left ? cmp.GetChildrenMutable()[0] : cmp.GetChildrenMutable()[1]), list_type);
	auto scalar_expr =
	    CastScalarToPacFloat(std::move(counter_on_left ? cmp.GetChildrenMutable()[1] : cmp.GetChildrenMutable()[0]),
	                         finalized_type, list_type);
	PRIVACY_DEBUG_PRINT("[PAC DERIVED READ] Rewriting comparison to " + func_name);
	return input.optimizer.BindScalarFunction(Identifier(func_name), std::move(scalar_expr), std::move(counter_ref));
}

// Rewrite a BETWEEN expression on a counter column to priv_filter_<cmp> AND conjunction.
// total BETWEEN low AND high → priv_filter_lte(low, counter) AND priv_filter_gte(high, counter)
static unique_ptr<Expression>
RewriteBetweenToFilterCmp(OptimizerExtensionInput &input, unique_ptr<Expression> counter_ref1,
                          unique_ptr<Expression> counter_ref2, unique_ptr<Expression> lower,
                          unique_ptr<Expression> upper, bool lower_inclusive, bool upper_inclusive,
                          const LogicalType &finalized_type, const LogicalType &list_type) {
	auto lower_scalar = CastScalarToPacFloat(std::move(lower), finalized_type, list_type);
	auto upper_scalar = CastScalarToPacFloat(std::move(upper), finalized_type, list_type);
	// counter >= low → low <= counter → priv_filter_lte(low, counter)
	// counter <= high → high >= counter → priv_filter_gte(high, counter)
	string privacy_mode = GetPrivacyMode(input.context);
	string lower_func = GetPacFilterCmpName(
	    lower_inclusive ? ExpressionType::COMPARE_LESSTHANOREQUALTO : ExpressionType::COMPARE_LESSTHAN, privacy_mode);
	string upper_func = GetPacFilterCmpName(upper_inclusive ? ExpressionType::COMPARE_GREATERTHANOREQUALTO
	                                                        : ExpressionType::COMPARE_GREATERTHAN,
	                                        privacy_mode);
	auto lower_call =
	    input.optimizer.BindScalarFunction(Identifier(lower_func), std::move(lower_scalar), std::move(counter_ref1));
	auto upper_call =
	    input.optimizer.BindScalarFunction(Identifier(upper_func), std::move(upper_scalar), std::move(counter_ref2));
	PRIVACY_DEBUG_PRINT("[PAC DERIVED READ] Rewriting BETWEEN to " + lower_func + " AND " + upper_func);
	return make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(lower_call),
	                                             std::move(upper_call));
}

// Wrap counter column refs with priv_finalize, bottom-up through the plan.
//
// Uses op->types (resolved after DuckDB's optimizer) to identify counter columns
// because the optimizer may reorder columns, making position-based matching unreliable.
//
// Comparisons involving counter columns are rewritten to priv_filter_<cmp>(scalar, counters)
// which evaluates all 64 subsamples using majority voting. This works for scan-level
// filters too because priv_filter_<cmp> returns BOOLEAN.
//
// Non-comparison contexts (projections, ORDER BY, etc.) still use priv_finalize wrapping.
//
// Suppresses wrapping inside AGGREGATE subtrees — aggregates (e.g. first() in scalar
// subqueries, SUM() over counter columns) need raw FLOAT[] input. priv_finalize is
// applied above the aggregate via projection wrapping or the registered implicit cast.
static void WrapCounterRefsWithFinalize(OptimizerExtensionInput &input, LogicalOperator *op,
                                        const unordered_set<idx_t> &derived_table_indices,
                                        unordered_set<uint64_t> &finalized_bindings, bool suppress = false) {
	if (!op) {
		return;
	}
	// Suppress wrapping inside AGGREGATE subtrees (e.g. first() in scalar subqueries needs
	// raw FLOAT[] input).
	bool child_suppress = suppress || (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY);
	if (op->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE && op->children.size() > 1) {
		auto &cte = op->Cast<LogicalMaterializedCTE>();
		WrapCounterRefsWithFinalize(input, op->children[0].get(), derived_table_indices, finalized_bindings,
		                            child_suppress);
		SyncMaterializedCTERefTypes(op->children[1].get(), cte.table_index.index, op->children[0]->types,
		                            CounterListType(), PacFloatLogicalType(), finalized_bindings);
		WrapCounterRefsWithFinalize(input, op->children[1].get(), derived_table_indices, finalized_bindings,
		                            child_suppress);
	} else {
		for (auto &child : op->children) {
			WrapCounterRefsWithFinalize(input, child.get(), derived_table_indices, finalized_bindings, child_suppress);
		}
	}

	auto list_type = CounterListType();
	auto finalized_type = PacFloatLogicalType();

	// Skip GET nodes and aggregate/suppressed subtrees — nothing to wrap
	if (op->type == LogicalOperatorType::LOGICAL_GET || suppress ||
	    op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return;
	}

	// Fix stale column refs that were finalized by a child operator
	std::function<void(unique_ptr<Expression> &)> FixExpr = [&](unique_ptr<Expression> &e) {
		if (e->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			auto &col_ref = e->Cast<BoundColumnRefExpression>();
			if (finalized_bindings.count(HashBinding(col_ref.BindingMutable()))) {
				col_ref.SetReturnType(finalized_type);
			}
		}
		ExpressionIterator::EnumerateChildren(*e, FixExpr);
		if (e->GetReturnType() == list_type && ExpressionReferencesFinalizedBinding(*e, finalized_bindings)) {
			e->SetReturnType(finalized_type);
		}
		if (e->GetExpressionType() == ExpressionType::COMPARE_BETWEEN) {
			auto &between = e->Cast<BoundFunctionExpression>();
			FixExpr(between.GetChildrenMutable()[0]);
			FixExpr(between.GetChildrenMutable()[1]);
			FixExpr(between.GetChildrenMutable()[2]);
		}
	};
	LogicalOperatorVisitor::EnumerateExpressions(*op, [&](unique_ptr<Expression> *expr_ptr) { FixExpr(*expr_ptr); });

	// Rewrite comparisons on counter columns to priv_filter_<cmp> calls,
	// and wrap remaining counter column refs with priv_finalize.
	std::function<void(unique_ptr<Expression> &)> WrapExpr = [&](unique_ptr<Expression> &e) {
		// Skip already-wrapped expressions
		if (e->GetExpressionType() == ExpressionType::BOUND_FUNCTION) {
			auto &func_name = e->Cast<BoundFunctionExpression>().FunctionMutable().GetName().GetIdentifierName();
			if (func_name == "priv_finalize" || StringUtil::StartsWith(func_name, "priv_filter_")) {
				return;
			}
		}
		// Skip binder-inserted CAST(FLOAT[] → scalar) — the registered cast handles finalization
		if (e->GetExpressionType() == ExpressionType::OPERATOR_CAST) {
			auto &cast = e->Cast<BoundFunctionExpression>();
			if (cast.GetChildrenMutable()[0]->GetReturnType() == list_type && cast.GetReturnType() != list_type) {
				return;
			}
		}

		// Rewrite BoundComparisonExpression where one side is a counter column
		if (BoundComparisonExpression::IsComparison(*e)) {
			auto &cmp = e->Cast<BoundFunctionExpression>();
			// Recurse into the scalar side first (it may contain nested expressions)
			bool left_is_counter = IsCounterColumnRef(*cmp.GetChildrenMutable()[0], list_type);
			bool right_is_counter = IsCounterColumnRef(*cmp.GetChildrenMutable()[1], list_type);
			if (left_is_counter && !right_is_counter) {
				WrapExpr(cmp.GetChildrenMutable()[1]);
			} else if (right_is_counter && !left_is_counter) {
				WrapExpr(cmp.GetChildrenMutable()[0]);
			}
			auto rewritten = RewriteComparisonToFilterCmp(input, cmp, list_type, finalized_type);
			if (rewritten) {
				e = std::move(rewritten);
				return;
			}
			// Both sides are counters or no counter — fall through to priv_finalize wrapping
		}

		// Rewrite BoundBetweenExpression where input is a counter column
		if (e->GetExpressionType() == ExpressionType::COMPARE_BETWEEN) {
			auto &between = e->Cast<BoundFunctionExpression>();
			if (IsCounterColumnRef(*between.GetChildrenMutable()[0], list_type)) {
				WrapExpr(between.GetChildrenMutable()[1]);
				WrapExpr(between.GetChildrenMutable()[2]);
				auto counter_ref1 = ExtractCounterRef(between.GetChildrenMutable()[0]->Copy(), list_type);
				auto counter_ref2 = ExtractCounterRef(std::move(between.GetChildrenMutable()[0]), list_type);
				e = RewriteBetweenToFilterCmp(
				    input, std::move(counter_ref1), std::move(counter_ref2), std::move(between.GetChildrenMutable()[1]),
				    std::move(between.GetChildrenMutable()[2]), BoundBetweenExpression::LowerInclusive(between),
				    BoundBetweenExpression::UpperInclusive(between), finalized_type, list_type);
				return;
			}
			// Non-counter BETWEEN: recurse into fields manually (not visited by EnumerateChildren)
			WrapExpr(between.GetChildrenMutable()[0]);
			WrapExpr(between.GetChildrenMutable()[1]);
			WrapExpr(between.GetChildrenMutable()[2]);
		}

		ExpressionIterator::EnumerateChildren(*e, WrapExpr);

		if (e->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			auto &col_ref = e->Cast<BoundColumnRefExpression>();
			if (col_ref.GetReturnType() == list_type) {
				PRIVACY_DEBUG_PRINT("[PAC DERIVED READ] Wrapping binding (" +
				                    std::to_string(col_ref.Binding().table_index.index) + "," +
				                    std::to_string(col_ref.Binding().column_index) + ") with priv_finalize");
				e = input.optimizer.BindScalarFunction("priv_finalize", std::move(e));
			}
		}
		// Retarget CAST(scalar→FLOAT[]) to CAST(scalar→FLOAT), strip no-op casts
		if (e->GetExpressionType() == ExpressionType::OPERATOR_CAST) {
			auto &cast = e->Cast<BoundFunctionExpression>();
			if (cast.GetReturnType() == list_type && cast.GetChildrenMutable()[0]->GetReturnType() != list_type) {
				e = BoundCastExpression::AddDefaultCastToType(std::move(cast.GetChildrenMutable()[0]), finalized_type);
			} else if (cast.GetChildrenMutable()[0]->GetReturnType() == cast.GetReturnType()) {
				e = std::move(cast.GetChildrenMutable()[0]);
			}
		}
		// Convert FLOAT[] constants back to scalar
		if (e->GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
			auto &constant = e->Cast<BoundConstantExpression>();
			if (constant.GetReturnType() == list_type && !constant.GetValue().IsNull()) {
				auto &list_children = ListValue::GetChildren(constant.GetValue());
				if (!list_children.empty()) {
					auto scalar_val = list_children[0].DefaultCastAs(finalized_type);
					e = make_uniq<BoundConstantExpression>(std::move(scalar_val));
				}
			}
		}
	};
	LogicalOperatorVisitor::EnumerateExpressions(*op, [&](unique_ptr<Expression> *expr_ptr) { WrapExpr(*expr_ptr); });

	// Update operator output types after wrapping
	if (op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &proj = op->Cast<LogicalProjection>();
		for (idx_t i = 0; i < proj.expressions.size(); i++) {
			if (i < proj.types.size()) {
				proj.types[i] = proj.expressions[i]->GetReturnType();
			}
			// Only mark as finalized if the expression actually produces a counter type.
			// Expressions that CONTAIN priv_finalize in sub-expressions (e.g. CASE WHEN priv_finalize(x) > 100 ...)
			// may return a different type (e.g. VARCHAR) and should NOT be marked finalized.
			bool is_counter_output = (proj.expressions[i]->GetReturnType() == finalized_type) ||
			                         (proj.expressions[i]->GetReturnType() == list_type &&
			                          ExpressionContainsPacFinalize(*proj.expressions[i]));
			if (is_counter_output) {
				finalized_bindings.insert(
				    HashBinding(ColumnBinding(TableIndex(proj.table_index.index), ProjectionIndex(i))));
				if (i < proj.types.size() && proj.types[i] == list_type) {
					proj.types[i] = finalized_type;
				}
				if (proj.expressions[i]->GetReturnType() == list_type) {
					proj.expressions[i]->SetReturnType(finalized_type);
				}
			}
		}
	} else {
		// For pass-through operators (DISTINCT, ORDER BY, etc.): sync types from child
		for (idx_t i = 0; i < op->types.size(); i++) {
			if (op->types[i] == list_type) {
				for (auto &child : op->children) {
					if (i < child->types.size() && child->types[i] == finalized_type) {
						op->types[i] = finalized_type;
						auto out_bindings = op->GetColumnBindings();
						if (i < out_bindings.size()) {
							finalized_bindings.insert(HashBinding(out_bindings[i]));
						}
					}
				}
			}
		}
	}
}

// Rewrite scan-level table_filters on counter columns into priv_filter_<cmp> expressions.
// DuckDB's filter pushdown may push comparisons (including BETWEEN) into GET.table_filters
// before our post-optimizer runs. The scan executor doesn't support LIST types, so we pull
// these filters out and convert them to priv_filter_<cmp> calls in a FILTER operator.
static void RewriteCounterFiltersFromGets(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &op,
                                          const unordered_set<idx_t> &derived_table_indices) {
	if (!op) {
		return;
	}
	for (auto &child : op->children) {
		RewriteCounterFiltersFromGets(input, child, derived_table_indices);
	}
	if (op->type != LogicalOperatorType::LOGICAL_GET) {
		return;
	}
	auto &get = op->Cast<LogicalGet>();
	if (!derived_table_indices.count(get.table_index.index)) {
		return;
	}
	auto list_type = CounterListType();
	auto finalized_type = PacFloatLogicalType();
	vector<idx_t> to_pull;
	const auto &col_ids = get.GetColumnIds();
	for (auto &entry : get.table_filters) {
		auto col_idx = entry.GetIndex();
		// DuckDB 2.0 keys filters by scan projection, not physical table column.
		if (col_idx < col_ids.size() && get.GetColumnType(col_ids[col_idx]) == list_type) {
			to_pull.push_back(col_idx);
		}
	}
	if (to_pull.empty()) {
		return;
	}
	auto out_bindings = op->GetColumnBindings();
	vector<unique_ptr<Expression>> filter_exprs;
	for (auto filter_col_idx : to_pull) {
		auto &filter = get.table_filters.GetFilterByColumnIndexMutable(ProjectionIndex(filter_col_idx));
		auto col_type = get.GetColumnType(col_ids[filter_col_idx]);
		// Find the output binding for this column
		ColumnBinding binding;
		bool found = false;
		for (idx_t i = 0; i < col_ids.size() && i < out_bindings.size(); i++) {
			if ((get.projection_ids.empty() ? i : idx_t(get.projection_ids[i])) == filter_col_idx) {
				binding = out_bindings[i];
				found = true;
				break;
			}
		}
		if (!found) {
			// Filter-only columns may have been pruned from the scan output. Expose
			// them again so the pulled-up privacy predicate can evaluate them.
			D_ASSERT(!get.projection_ids.empty());
			get.projection_ids.emplace_back(filter_col_idx);
			binding = ColumnBinding(get.table_index, ProjectionIndex(filter_col_idx));
			get.ResolveOperatorTypes();
		}
		auto col_ref = make_uniq<BoundColumnRefExpression>(col_type, binding);
		auto expr = filter.ToExpression(*col_ref);
		// Rewrite the expression to priv_filter_<cmp> calls
		if (expr->GetExpressionType() == ExpressionType::COMPARE_BETWEEN) {
			auto &between = expr->Cast<BoundFunctionExpression>();
			auto counter1 = make_uniq<BoundColumnRefExpression>(col_type, binding);
			auto counter2 = make_uniq<BoundColumnRefExpression>(col_type, binding);
			filter_exprs.push_back(RewriteBetweenToFilterCmp(
			    input, std::move(counter1), std::move(counter2), std::move(between.GetChildrenMutable()[1]),
			    std::move(between.GetChildrenMutable()[2]), BoundBetweenExpression::LowerInclusive(between),
			    BoundBetweenExpression::UpperInclusive(between), finalized_type, list_type));
		} else if (BoundComparisonExpression::IsComparison(*expr)) {
			auto &cmp = expr->Cast<BoundFunctionExpression>();
			auto rewritten = RewriteComparisonToFilterCmp(input, cmp, list_type, finalized_type);
			filter_exprs.push_back(rewritten ? std::move(rewritten) : std::move(expr));
		} else {
			filter_exprs.push_back(std::move(expr));
		}
		get.table_filters.RemoveFilterByColumnIndex(ProjectionIndex(filter_col_idx));
		PRIVACY_DEBUG_PRINT("[PAC DERIVED READ] Rewrote table_filter to priv_filter_<cmp> for counter column " +
		                    std::to_string(filter_col_idx));
	}
	auto filter_op = make_uniq<LogicalFilter>();
	for (auto &expr : filter_exprs) {
		filter_op->expressions.push_back(std::move(expr));
	}
	filter_op->children.push_back(std::move(op));
	filter_op->ResolveOperatorTypes();
	op = std::move(filter_op);
}

// --- Public API ---

bool HasDerivedPuCounterGets(LogicalOperator *plan) {
	unordered_set<idx_t> indices;
	FindDerivedPuGets(plan, indices);
	return !indices.empty();
}

void InjectPacFinalizeForDerivedPu(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	unordered_set<idx_t> derived_table_indices;
	FindDerivedPuGets(plan.get(), derived_table_indices);
	if (derived_table_indices.empty()) {
		return;
	}
	// derived_pu counter tables hold PAC 64-world counters and are finalized/compared with the
	// PAC categorical terminals (priv_finalize, priv_filter_<cmp>). Those terminals are NOT
	// differentially private, so reading them under a DP mode would silently route a DP query
	// through an unbudgeted PAC decision. Reject instead — this is the resilience guard that
	// guarantees no DP session can reach a PAC categorical terminal.
	string privacy_mode = GetPrivacyMode(input.context);
	if (privacy_mode != "pac") {
		throw InvalidInputException(
		    "derived_pu counter tables are a PAC-only feature and cannot be read under privacy_mode='" + privacy_mode +
		    "'. Set privacy_mode='pac' to query materialized counter columns.");
	}
	PRIVACY_DEBUG_PRINT("[PAC DERIVED READ] Injecting priv_finalize for " +
	                    std::to_string(derived_table_indices.size()) + " derived_pu table(s)");
	// Rewrite scan-level filters on counter columns to priv_filter_<cmp> calls.
	// DuckDB's filter pushdown may push comparisons (including BETWEEN) into table_filters
	// before our post-optimizer runs. The scan executor doesn't support LIST types.
	RewriteCounterFiltersFromGets(input, plan, derived_table_indices);
	unordered_set<uint64_t> finalized_bindings;
	WrapCounterRefsWithFinalize(input, plan.get(), derived_table_indices, finalized_bindings);
	plan->ResolveOperatorTypes();
}

} // namespace duckdb
