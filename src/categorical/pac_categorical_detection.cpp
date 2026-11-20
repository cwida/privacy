//
// PAC Categorical Pattern Detection - Implementation
//
// See pac_categorical_detection.hpp for design documentation.
//
// Created by ila on 1/23/26.
//
#include "privacy_debug.hpp"
#include "categorical/pac_categorical_detection.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"

namespace duckdb {

// Follow every source, including both COUNT copies in DuckDB's nullable-count
// wrapper and aggregates over aggregates. A projection may depend on several
// stochastic outputs; retaining only its first source prematurely finalizes the rest.
static void CollectPacSources(const Expression &expr, LogicalOperator &root, vector<ColumnBinding> &sources,
                              unordered_set<uint64_t> &visited) {
	if (IsAlreadyWrappedInPacNoised(&expr)) {
		return;
	}
	if (expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		auto binding = expr.Cast<BoundColumnRefExpression>().Binding();
		if (!visited.insert(HashBinding(binding)).second) {
			return;
		}
		auto *source = FindOperatorByTableIndex(&root, binding.table_index.index);
		if (!source) {
			return;
		}
		if (source->type == LogicalOperatorType::LOGICAL_PROJECTION) {
			auto &projection = source->Cast<LogicalProjection>();
			if (binding.column_index < projection.expressions.size()) {
				CollectPacSources(*projection.expressions[binding.column_index], root, sources, visited);
			}
		} else if (source->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
			auto &aggregate = source->Cast<LogicalAggregate>();
			if (binding.table_index != aggregate.aggregate_index ||
			    binding.column_index >= aggregate.expressions.size()) {
				return;
			}
			auto &aggregate_expr = *aggregate.expressions[binding.column_index];
			if (aggregate_expr.GetExpressionType() == ExpressionType::BOUND_AGGREGATE &&
			    IsAnyPacAggregate(
			        aggregate_expr.Cast<BoundAggregateExpression>().Function().GetName().GetIdentifierName())) {
				sources.push_back(binding);
			} else {
				CollectPacSources(aggregate_expr, root, sources, visited);
			}
		} else if (source->type == LogicalOperatorType::LOGICAL_CTE_REF) {
			auto &ref = source->Cast<LogicalCTERef>();
			PRIVACY_DEBUG_PRINT("Categorical detection: tracing CTE reader #" + std::to_string(ref.table_index.index));
			auto *cte = FindMaterializedCTE(&root, ref.cte_index.index);
			if (cte) {
				auto bindings = cte->children[0]->GetColumnBindings();
				if (binding.column_index < bindings.size()) {
					BoundColumnRefExpression definition_ref(expr.GetReturnType(), bindings[binding.column_index]);
					CollectPacSources(definition_ref, root, sources, visited);
				}
			}
		}
		return;
	}
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { CollectPacSources(child, root, sources, visited); });
}

vector<PacBindingInfo> FindAllPacBindingsInExpression(Expression *expr, LogicalOperator *plan_root) {
	vector<PacBindingInfo> result;
	unordered_set<uint64_t> seen;
	std::function<void(const Expression &)> collect = [&](const Expression &expression) {
		if (expression.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			auto binding = expression.Cast<BoundColumnRefExpression>().Binding();
			if (seen.insert(HashBinding(binding)).second) {
				vector<ColumnBinding> sources;
				unordered_set<uint64_t> visited;
				CollectPacSources(expression, *plan_root, sources, visited);
				for (auto &source : sources) {
					PacBindingInfo info;
					info.binding = binding;
					info.source_binding = source;
					info.index = result.size();
					result.push_back(info);
				}
			}
		}
		ExpressionIterator::EnumerateChildren(expression, collect);
	};
	collect(*expr);
	return result;
}

// Check if a binding in a FILTER traces to the filter's own child aggregate (HAVING clause).
// HAVING predicates should use scalar as_noised comparison, not categorical priv_filter,
// because priv_filter's majority-vote decision can disagree with the as_noised aggregate
// values that appear alongside it in the output.
static bool IsHavingBinding(LogicalOperator *filter_op, const ColumnBinding &binding) {
	if (!filter_op || filter_op->children.empty()) {
		return false;
	}
	// Walk through projections to find the child aggregate.
	// Also check if the binding references any projection we walk through
	// (handles multi-branch case where Projection replaces the original Aggregate).
	LogicalOperator *child = filter_op->children[0].get();
	while (child && child->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &proj = child->Cast<LogicalProjection>();
		if (binding.table_index.index == proj.table_index.index) {
			return true;
		}
		if (child->children.empty()) {
			break;
		}
		child = child->children[0].get();
	}
	if (child && child->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		auto &agg = child->Cast<LogicalAggregate>();
		return binding.table_index.index == agg.aggregate_index.index ||
		       binding.table_index.index == agg.group_index.index;
	}
	return false;
}

// Recursively search the plan for categorical patterns (plan-aware version)
// Now detects ANY filter expression containing a PAC aggregate, not just comparisons
// outer_pac_hash: if an outer PAC aggregate was found above, this is the hash binding resolved to the current level
void DetectCategoricalPatterns(LogicalOperator *op, LogicalOperator *plan_root,
                               vector<CategoricalPatternInfo> &patterns, bool inside_aggregate,
                               const PacAggregateInfoMap &pac_agg_info, ColumnBinding outer_pac_hash) {
	// Track if we're entering an aggregate
	bool now_inside_aggregate = inside_aggregate || (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY);

	// Track outer PAC hash for children: updated when we find a PAC aggregate or pass through a projection
	ColumnBinding hash_for_children = outer_pac_hash;

	// When entering a PAC aggregate, extract the hash binding.
	// Fast path: use pre-computed info from the bitslice compiler if available.
	// Fallback: extract from children[0] of the first PAC aggregate expression.
	if (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		auto &agg = op->Cast<LogicalAggregate>();
		auto it = pac_agg_info.find(agg.aggregate_index.index);
		if (it != pac_agg_info.end()) {
			hash_for_children = it->second.hash_binding;
		} else {
			for (auto &agg_expr : agg.expressions) {
				if (agg_expr->GetExpressionType() == ExpressionType::BOUND_AGGREGATE) {
					auto &bound_agg = agg_expr->Cast<BoundAggregateExpression>();
					if (IsPacAggregate(bound_agg.FunctionMutable().GetName().GetIdentifierName()) &&
					    !bound_agg.GetChildrenMutable().empty() &&
					    bound_agg.GetChildrenMutable()[0]->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
						hash_for_children =
						    bound_agg.GetChildrenMutable()[0]->Cast<BoundColumnRefExpression>().BindingMutable();
						break;
					}
				}
			}
		}
	}

	// Resolve hash binding through projections (follow col_ref chains)
	if (hash_for_children.table_index.index != DConstants::INVALID_INDEX &&
	    op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &proj = op->Cast<LogicalProjection>();
		if (hash_for_children.table_index.index == proj.table_index.index &&
		    hash_for_children.column_index < proj.expressions.size()) {
			auto &expr = proj.expressions[hash_for_children.column_index];
			if (expr->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
				hash_for_children = expr->Cast<BoundColumnRefExpression>().BindingMutable();
			}
		}
	}

	// Check filter expressions - detect ANY boolean expression containing a PAC aggregate
	if (op->type == LogicalOperatorType::LOGICAL_FILTER) {
		auto &filter = op->Cast<LogicalFilter>();
		for (idx_t i = 0; i < filter.expressions.size(); i++) {
			auto &filter_expr = filter.expressions[i];
			// Find ALL PAC aggregate bindings in this expression (not just single)
			auto pac_bindings = FindAllPacBindingsInExpression(filter_expr.get(), plan_root);
			if (pac_bindings.empty()) {
				continue;
			}
			// Skip HAVING predicates: if ALL bindings trace to the filter's own child
			// aggregate, this is a HAVING clause. HAVING should use scalar as_noised
			// comparison, not categorical priv_filter, because priv_filter's majority-vote
			// can disagree with as_noised values shown in the same row.
			bool all_having = true;
			for (auto &bi : pac_bindings) {
				if (!IsHavingBinding(op, bi.binding)) {
					all_having = false;
					break;
				}
			}
			if (all_having) {
				continue;
			}
			{
				CategoricalPatternInfo info;
				info.parent_op = op;
				info.expr_index = i;
				info.pac_bindings = std::move(pac_bindings);
				info.outer_pac_hash = hash_for_children;
				info.has_outer_pac_hash = (hash_for_children.table_index.index != DConstants::INVALID_INDEX);
				patterns.push_back(info);
			}
		}
	} else if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
	           op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		auto &join = op->Cast<LogicalComparisonJoin>();
		for (idx_t i = 0; i < join.conditions.size(); i++) {
			auto &cond = join.conditions[i];
			vector<PacBindingInfo> bindings;
			unordered_set<uint64_t> seen;
			for (auto *side : {cond.LeftReference().get(), cond.RightReference().get()}) {
				for (auto &binding : FindAllPacBindingsInExpression(side, plan_root)) {
					if (seen.insert(HashBinding(binding.source_binding)).second) {
						binding.index = bindings.size();
						bindings.push_back(binding);
					}
				}
			}
			if (bindings.empty()) {
				continue;
			}
			CategoricalPatternInfo info;
			info.parent_op = op;
			info.expr_index = i;
			info.pac_bindings = std::move(bindings);
			info.outer_pac_hash = hash_for_children;
			info.has_outer_pac_hash = (hash_for_children.table_index.index != DConstants::INVALID_INDEX);
			patterns.push_back(std::move(info));
		}
	} else if (!inside_aggregate && patterns.empty() && op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		// Check projection expressions for arithmetic involving multiple PAC aggregates
		// This handles cases like Q08: sum(CASE...)/sum(volume) in SELECT list
		// NOTE: Only check projections if we haven't already found filter/join patterns,
		// because those patterns will handle the projections via RewriteProjectionsWithCounters.
		// We only want standalone projection patterns (no filter/join categorical patterns).
		auto &proj = op->Cast<LogicalProjection>();
		for (idx_t i = 0; i < proj.expressions.size(); i++) {
			auto &expr = proj.expressions[i];
			if (expr->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
				continue; // Skip simple column references - they don't need rewriting
			}
			// Find ALL PAC aggregate bindings in this expression
			auto pac_bindings = FindAllPacBindingsInExpression(expr.get(), plan_root);
			if (pac_bindings.empty()) {
				continue;
			}
			// Check if this expression has arithmetic with PAC aggregates
			// - Multiple PAC bindings (e.g., as_sum(...) / as_sum(...))
			// - Or single PAC binding with arithmetic (e.g., as_sum(...) * 0.5)
			bool is_arithmetic_with_pac = false;
			if (pac_bindings.size() >= 2) { // Multiple PAC aggregates - definitely needs lambda rewrite
				is_arithmetic_with_pac = true;
			} else if (pac_bindings.size() == 1) {
				// Single aggregate - check if it's in an arithmetic expression (not just a column ref or simple cast)
				if (expr->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF &&
				    expr->GetExpressionType() != ExpressionType::OPERATOR_CAST) {
					is_arithmetic_with_pac = true;
				} else if (expr->GetExpressionType() == ExpressionType::OPERATOR_CAST) {
					// Check if cast contains arithmetic
					auto &cast_expr = expr->Cast<BoundFunctionExpression>();
					if (cast_expr.GetChildrenMutable()[0]->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
						is_arithmetic_with_pac = true;
					}
				}
			}
			if (is_arithmetic_with_pac) {
				if (!IsNumericalType(expr->GetReturnType())) {
					continue; // Only create pattern if result is numerical (as_noised only works on numbers)
				}
				CategoricalPatternInfo info;
				info.parent_op = op;
				info.expr_index = i;
				info.pac_bindings = std::move(pac_bindings);
				patterns.push_back(std::move(info));
			}
		}
	}
	for (auto &child : op->children) {
		DetectCategoricalPatterns(child.get(), plan_root, patterns, now_inside_aggregate, pac_agg_info,
		                          hash_for_children);
	}
}

} // namespace duckdb
