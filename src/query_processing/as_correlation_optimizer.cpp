#include "query_processing/as_correlation_optimizer.hpp"

#include "duckdb/common/enums/optimizer_type.hpp"
#include "duckdb/execution/operator/join/physical_comparison_join.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/join_filter_pushdown_optimizer.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/optimizer/partitioned_execution.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/expression_barrier.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_dependent_join.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/planner_extension.hpp"
#include "privacy_debug.hpp"

namespace duckdb {

static bool ContainsASAggregate(LogicalOperator &op) {
	bool found = false;
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expr) {
		ExpressionIterator::VisitExpression<BoundAggregateExpression>(**expr, [&](const BoundAggregateExpression &agg) {
			found |= StringUtil::StartsWith(agg.Function().GetName().GetIdentifierName(), "as_");
		});
	});
	for (auto &child : op.children) {
		found |= ContainsASAggregate(*child);
	}
	return found;
}

static bool ContainsReorderingBarrier(LogicalOperator &op) {
	bool found = false;
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expr) {
		found |= (*expr)->IsVolatile() || ExpressionBarrier::Contains(**expr);
	});
	for (auto &child : op.children) {
		found |= ContainsReorderingBarrier(*child);
	}
	return found;
}

// The outer filter may still be one AND expression containing the noisy
// predicate. Separate safe predicates on passthrough columns before the input
// becomes a materialized correlation domain, so noise is drawn only after them.
static void PushCorrelatedInputFilters(ClientContext &context, LogicalFilter &filter) {
	if (filter.children[0]->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return;
	}
	auto &projection = filter.children[0]->Cast<LogicalProjection>();
	if (projection.children[0]->type != LogicalOperatorType::LOGICAL_DEPENDENT_JOIN || projection.expressions.empty() ||
	    projection.expressions[0]->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION ||
	    !projection.expressions[0]->IsVolatile()) {
		return;
	}
	auto release_name =
	    projection.expressions[0]->Cast<BoundFunctionExpression>().Function().GetName().GetIdentifierName();
	if (release_name != "as_noised" && release_name != "as_noised_div") {
		return;
	}
	auto &join = projection.children[0]->Cast<LogicalDependentJoin>();
	if (join.join_type != JoinType::SINGLE || !join.perform_delim || join.HasProjectionMap()) {
		return;
	}
	auto input_bindings = join.children[0]->GetColumnBindings();
	filter.SplitPredicates();
	vector<unique_ptr<Expression>> remaining;
	vector<unique_ptr<Expression>> selected;
	for (auto &expression : filter.expressions) {
		// Literal casts have not passed through constant folding at post-bind.
		// Evaluate only these casts before checking whether a predicate can throw.
		ExpressionIterator::EnumerateExpression(expression, [&](unique_ptr<Expression> &node) {
			if (BoundCastExpression::IsCast(*node) && node->IsFoldable() &&
			    BoundCastExpression::Child(node->Cast<BoundFunctionExpression>()).GetExpressionClass() ==
			        ExpressionClass::BOUND_CONSTANT) {
				Value value;
				if (ExpressionExecutor::TryEvaluateScalar(context, *node, value)) {
					node = make_uniq<BoundConstantExpression>(std::move(value));
				}
			}
		});
		bool safe = !expression->IsVolatile() && !expression->CanThrow() && !ExpressionBarrier::Contains(*expression);
		ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
		    *expression, [&](const BoundColumnRefExpression &ref) {
			    if (ref.Depth() != 0 || ref.Binding().table_index != projection.table_index ||
			        projection.GetExpression(ref.Binding()).GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				    safe = false;
				    return;
			    }
			    auto binding = projection.GetExpression(ref.Binding()).Cast<BoundColumnRefExpression>().Binding();
			    safe &= std::find(input_bindings.begin(), input_bindings.end(), binding) != input_bindings.end();
		    });
		if (!safe) {
			remaining.push_back(std::move(expression));
			continue;
		}
		ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
		    expression, [&](BoundColumnRefExpression &ref, unique_ptr<Expression> &expr) {
			    expr = projection.GetExpression(ref.Binding()).Copy();
		    });
		selected.push_back(std::move(expression));
	}
	filter.expressions = std::move(remaining);
	if (!selected.empty()) {
		auto input_filter = make_uniq<LogicalFilter>();
		input_filter->expressions = std::move(selected);
		input_filter->children.push_back(std::move(join.children[0]));
		join.children[0] = std::move(input_filter);
		PRIVACY_DEBUG_PRINT("[AS] Applied deterministic outer filters before shared counter aggregation");
	}
}

// A volatile scalar release belongs to each outer row, but the counter
// aggregates underneath it are deterministic and can be shared by correlation
// key. Move only the release above SINGLE before DuckDB chooses its domain.
static void LiftCorrelatedNoise(PlannerExtensionInput &input, unique_ptr<LogicalOperator> &op,
                                BindingReplacementGraph &replacements) {
	for (idx_t i = 0; i < op->children.size(); i++) {
		BindingReplacementGraph child_replacements;
		ColumnBindingRewrite::RewriteChild(
		    op, i, [&](unique_ptr<LogicalOperator> &child, BindingReplacementGraph &child_changes) {
			    LiftCorrelatedNoise(input, child, child_changes);
			    child_replacements.Merge(child_changes);
		    });
		replacements.Merge(child_replacements);
	}
	if (op->type == LogicalOperatorType::LOGICAL_FILTER) {
		PushCorrelatedInputFilters(input.context, op->Cast<LogicalFilter>());
	}
	if (op->type != LogicalOperatorType::LOGICAL_DEPENDENT_JOIN || op->HasProjectionMap()) {
		return;
	}
	auto &join = op->Cast<LogicalDependentJoin>();
	if (join.join_type != JoinType::SINGLE || join.condition || join.correlated_columns.empty() ||
	    join.children[1]->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return;
	}
	auto &projection = join.children[1]->Cast<LogicalProjection>();
	if (projection.expressions.size() != 1 ||
	    projection.expressions[0]->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION ||
	    projection.children[0]->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY ||
	    !projection.children[0]->Cast<LogicalAggregate>().groups.empty() ||
	    ContainsReorderingBarrier(*projection.children[0])) {
		return;
	}
	auto &release = projection.expressions[0]->Cast<BoundFunctionExpression>();
	auto name = release.Function().GetName().GetIdentifierName();
	if ((name != "as_noised" && name != "as_noised_div") ||
	    release.Function().GetStability() != FunctionStability::VOLATILE) {
		return;
	}
	for (auto &argument : release.GetChildren()) {
		if (argument->IsVolatile() || ExpressionBarrier::Contains(*argument)) {
			return;
		}
		bool correlated = false;
		ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
		    *argument, [&](const BoundColumnRefExpression &ref) { correlated |= ref.Depth() != 0; });
		if (correlated) {
			return;
		}
	}
	CorrelatedColumns shared_columns;
	for (idx_t i = 0; i < join.correlated_columns.size(); i++) {
		if (!join.perform_delim && i == join.correlated_columns.GetDelimIndex()) {
			continue;
		}
		auto &column = join.correlated_columns[i];
		// Keep DuckDB's row-identity strategy for nested, non-deduplicable keys.
		if (column.type.IsNested()) {
			return;
		}
		shared_columns.AddColumnToBack(column);
	}
	if (shared_columns.empty()) {
		return;
	}
	join.ResolveOperatorTypes();
	auto left_bindings = join.children[0]->GetColumnBindings();
	auto release_index = projection.table_index;
	projection.table_index = input.binder.GenerateTableIndex();
	auto expression = std::move(projection.expressions[0]);
	projection.expressions = std::move(expression->Cast<BoundFunctionExpression>().GetChildrenMutable());
	for (idx_t i = 0; i < projection.expressions.size(); i++) {
		expression->Cast<BoundFunctionExpression>().GetChildrenMutable().push_back(make_uniq<BoundColumnRefExpression>(
		    projection.expressions[i]->GetReturnType(), ColumnBinding(projection.table_index, ProjectionIndex(i))));
	}
	vector<unique_ptr<Expression>> expressions;
	expressions.push_back(std::move(expression));
	for (idx_t i = 0; i < left_bindings.size(); i++) {
		expressions.push_back(make_uniq<BoundColumnRefExpression>(join.children[0]->types[i], left_bindings[i]));
		replacements.Add(left_bindings[i], ColumnBinding(release_index, ProjectionIndex(i + 1)));
	}
	join.correlated_columns = std::move(shared_columns);
	join.perform_delim = true;
	auto lifted = make_uniq<LogicalProjection>(release_index, std::move(expressions));
	lifted->children.push_back(std::move(op));
	op = std::move(lifted);
	op->ResolveOperatorTypes();
	PRIVACY_DEBUG_PRINT(
	    "[AS] Shared deterministic correlated counters while retaining one volatile release per outer row");
}

static void ShareCorrelatedNoiseInputs(PlannerExtensionInput &input, BoundStatement &statement) {
	if (!statement.plan || !Settings::Get<EnableOptimizerSetting>(input.context) ||
	    DBConfig::GetConfig(input.context).IsOptimizerDisabled(OptimizerType::EXTENSION) ||
	    !ContainsASAggregate(*statement.plan)) {
		return;
	}
	BindingReplacementGraph replacements;
	LiftCorrelatedNoise(input, statement.plan, replacements);
}

// Unlike an ordinary anti join, RIGHT_ANTI preserves the build side. Probe rows
// with no possible build-side match may therefore be filtered without changing
// any output row. Carry that restriction to both sides of an inner ownership join.
static void GetOwnershipFilterTargets(LogicalOperator &op, vector<JoinFilterPushdownColumn> columns,
                                      vector<PushdownFilterTarget> &targets) {
	if (columns.empty()) {
		return;
	}
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = op.Cast<LogicalProjection>();
		vector<JoinFilterPushdownColumn> projected;
		for (auto &column : columns) {
			if (column.probe_column_index.table_index == projection.table_index &&
			    JoinFilterPushdownUtil::PushdownJoinFilterExpression(
			        projection.GetExpression(column.probe_column_index), column)) {
				projected.push_back(std::move(column));
			}
		}
		if (!projected.empty()) {
			GetOwnershipFilterTargets(*op.children[0], std::move(projected), targets);
		}
		return;
	}
	if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
		GetOwnershipFilterTargets(*op.children[0], std::move(columns), targets);
		return;
	}
	if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		auto &join = op.Cast<LogicalComparisonJoin>();
		if (join.join_type != JoinType::INNER) {
			return;
		}
		for (auto &condition : join.conditions) {
			if (!condition.IsComparison() || condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL ||
			    condition.GetLHS().GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
			    condition.GetRHS().GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
			    condition.GetLHS().GetReturnType() != condition.GetRHS().GetReturnType()) {
				continue;
			}
			auto left = condition.GetLHS().Cast<BoundColumnRefExpression>().Binding();
			auto right = condition.GetRHS().Cast<BoundColumnRefExpression>().Binding();
			auto count = columns.size();
			for (idx_t i = 0; i < count; i++) {
				if (columns[i].probe_column_index != left && columns[i].probe_column_index != right) {
					continue;
				}
				auto other = columns[i];
				other.probe_column_index = other.probe_column_index == left ? right : left;
				bool present = false;
				for (auto &column : columns) {
					present |= column.probe_column_index == other.probe_column_index &&
					           column.join_filter_idx == other.join_filter_idx;
				}
				if (!present) {
					columns.push_back(std::move(other));
				}
			}
		}
		for (auto &child : op.children) {
			auto bindings = child->GetColumnBindings();
			vector<JoinFilterPushdownColumn> child_columns;
			for (auto &column : columns) {
				for (auto &binding : bindings) {
					if (binding == column.probe_column_index) {
						child_columns.push_back(column);
						break;
					}
				}
			}
			if (!child_columns.empty()) {
				GetOwnershipFilterTargets(*child, std::move(child_columns), targets);
			}
		}
		return;
	}
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		JoinFilterPushdownOptimizer::GetPushdownFilterTargets(op, std::move(columns), targets);
	}
}

// A scan on the build side can finish before its parent anti join publishes
// runtime filters. Keep a wide, weakly filtered scan on the probe side when a
// smaller key-only source can build the ownership lookup instead.
static idx_t ScanCardinality(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		return op.has_estimated_cardinality ? op.estimated_cardinality : 0;
	}
	if ((op.type == LogicalOperatorType::LOGICAL_PROJECTION || op.type == LogicalOperatorType::LOGICAL_FILTER) &&
	    op.children.size() == 1) {
		return ScanCardinality(*op.children[0]);
	}
	if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN &&
	    op.Cast<LogicalComparisonJoin>().join_type == JoinType::SEMI &&
	    op.children[1]->type == LogicalOperatorType::LOGICAL_CTE_REF) {
		return ScanCardinality(*op.children[0]);
	}
	return 0;
}

static void RestoreOutputLayout(unique_ptr<LogicalOperator> &op, const vector<ColumnBinding> &old_bindings) {
	auto bindings = op->GetColumnBindings();
	auto layout = make_uniq<LogicalFilter>();
	for (auto &old_binding : old_bindings) {
		for (idx_t i = 0; i < bindings.size(); i++) {
			if (bindings[i] == old_binding) {
				layout->projection_map.emplace_back(i);
				break;
			}
		}
	}
	D_ASSERT(layout->projection_map.size() == old_bindings.size());
	layout->children.push_back(std::move(op));
	op = std::move(layout);
	op->ResolveOperatorTypes();
}

// Range joins can produce large intermediates before a later multi-key join
// restricts them, as when inventory is matched to sales before matching dates.
static bool ContainsRangeJoin(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		for (auto &condition : op.Cast<LogicalComparisonJoin>().conditions) {
			if (condition.IsComparison()) {
				auto type = condition.GetComparisonType();
				if (type == ExpressionType::COMPARE_LESSTHAN || type == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
				    type == ExpressionType::COMPARE_GREATERTHAN ||
				    type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
					return true;
				}
			}
		}
	}
	for (auto &child : op.children) {
		if (ContainsRangeJoin(*child)) {
			return true;
		}
	}
	return false;
}

// (A SEMI/ANTI B) INNER F = (A INNER F) SEMI/ANTI B when F's
// predicates use only A and F. Apply the filtered scan before testing existence,
// so its reduced domain can also restrict the ownership joins in B.
// Sampled ordinary plans also permit the INNER case when F uses only one child.
static bool PushFilteredJoin(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &op, bool include_inner) {
	if (op->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}
	auto &inner = op->Cast<LogicalComparisonJoin>();
	if (inner.join_type != JoinType::INNER) {
		return false;
	}
	for (idx_t side = 0; side < 2; side++) {
		auto *membership_slot = &inner.children[side];
		while (include_inner && (*membership_slot)->type == LogicalOperatorType::LOGICAL_FILTER &&
		       (*membership_slot)->Cast<LogicalFilter>().expressions.empty()) {
			membership_slot = &(*membership_slot)->children[0];
		}
		if ((*membership_slot)->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
			continue;
		}
		auto &membership = (*membership_slot)->Cast<LogicalComparisonJoin>();
		auto filter_cardinality = ScanCardinality(*inner.children[1 - side]);
		if (!filter_cardinality && include_inner && inner.conditions.size() >= 2 && ContainsRangeJoin(membership)) {
			auto size = inner.children[1 - side]->EstimateCardinality(input.context);
			if (size < membership.EstimateCardinality(input.context)) {
				filter_cardinality = size;
			}
		}
		if (!filter_cardinality || (!JoinFilterPushdownOptimizer::IsFiltering(inner.children[1 - side]) &&
		                            (!include_inner || inner.conditions.size() < 2))) {
			continue;
		}
		idx_t preserved;
		if (membership.join_type == JoinType::SEMI || membership.join_type == JoinType::ANTI) {
			preserved = 0;
		} else if (membership.join_type == JoinType::RIGHT_SEMI || membership.join_type == JoinType::RIGHT_ANTI) {
			preserved = 1;
		} else if (include_inner && membership.join_type == JoinType::INNER) {
			preserved = 2;
			for (idx_t candidate = 0; candidate < 2; candidate++) {
				auto bindings = membership.children[candidate]->GetColumnBindings();
				bool matches = true;
				for (auto &condition : inner.conditions) {
					if (!condition.IsComparison()) {
						matches = false;
						break;
					}
					auto &key = side == 0 ? condition.GetLHS() : condition.GetRHS();
					ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
					    key, [&](const BoundColumnRefExpression &ref) {
						    matches &= std::find(bindings.begin(), bindings.end(), ref.Binding()) != bindings.end();
					    });
				}
				if (matches) {
					preserved = candidate;
					break;
				}
			}
			if (preserved == 2) {
				continue;
			}
		} else {
			continue;
		}
		bool safe = !ContainsReorderingBarrier(inner);
		for (auto join : {&inner, &membership}) {
			for (auto &condition : join->conditions) {
				safe &= condition.IsComparison() && !condition.GetLHS().CanThrow() && !condition.GetRHS().CanThrow();
			}
		}
		if (!safe) {
			continue;
		}
		auto old_bindings = inner.GetColumnBindings();
		auto result = std::move(*membership_slot);
		inner.children[side] = std::move(result->children[preserved]);
		// The membership predicate still needs columns pruned from the old final
		// INNER output. Retain them below it and restore the original layout above.
		inner.left_projection_map.clear();
		inner.right_projection_map.clear();
		membership.left_projection_map.clear();
		membership.right_projection_map.clear();
		inner.filter_pushdown.reset();
		if (include_inner) {
			// These joins now have different inputs; their cached estimates describe the old tree.
			inner.has_estimated_cardinality = false;
			membership.has_estimated_cardinality = false;
		}
		PushFilteredJoin(input, op, include_inner);
		if (!DBConfig::GetConfig(input.context).IsOptimizerDisabled(OptimizerType::JOIN_FILTER_PUSHDOWN)) {
			JoinFilterPushdownOptimizer filters(input.optimizer);
			filters.VisitOperator(*op);
		}
		result->children[preserved] = std::move(op);
		op = std::move(result);
		RestoreOutputLayout(op, old_bindings);
		PRIVACY_DEBUG_PRINT("[AS] Applied a filtered join before semi/anti membership checks");
		return true;
	}
	return false;
}

static void PushFilteredJoins(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &op,
                              bool include_inner = false) {
	for (auto &child : op->children) {
		PushFilteredJoins(input, child, include_inner);
	}
	PushFilteredJoin(input, op, include_inner);
}

static bool ContainsSamplingHash(LogicalOperator &op) {
	bool found = false;
	auto visit = [&](unique_ptr<Expression> &expr) {
		ExpressionIterator::EnumerateExpression(expr, [&](Expression &node) {
			if (node.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
				found |= node.Cast<BoundFunctionExpression>().Function().GetName() == Identifier("priv_hash");
			}
		});
	};
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expr) { visit(*expr); });
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto visit_filter = [&](TableFilter &filter) {
			if (filter.filter_type == TableFilterType::EXPRESSION_FILTER) {
				visit(filter.Cast<ExpressionFilter>().expr);
			}
		};
		auto &filters = op.Cast<LogicalGet>().table_filters;
		for (auto &filter : filters) {
			visit_filter(filter.Filter());
		}
		for (auto &filter : filters.GetMultiColumnFilters()) {
			visit_filter(*filter);
		}
	}
	for (auto &child : op.children) {
		found |= ContainsSamplingHash(*child);
	}
	return found;
}

// Lift deterministic projections above a join so a range join below them can
// receive a later multi-key restriction. Keep the public columns in order and
// let DuckDB rewrite each parent boundary, including its projection maps.
static void LiftSamplingJoinProjections(unique_ptr<LogicalOperator> &op, BindingReplacementGraph &replacements,
                                        bool &changed) {
	auto old_output = op->GetColumnBindings();
	auto rewrite_child = [&](unique_ptr<LogicalOperator> &child, BindingReplacementGraph &bindings) {
		LiftSamplingJoinProjections(child, bindings, changed);
	};
	for (idx_t i = 0; i < op->children.size(); i++) {
		ColumnBindingRewrite::RewriteChild(op, i, rewrite_child);
	}
	if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		auto &join = op->Cast<LogicalComparisonJoin>();
		if (join.join_type == JoinType::INNER && join.conditions.size() >= 2 && ContainsRangeJoin(*join.children[0])) {
			ColumnBindingRewrite::RewriteChild(op, 0,
			                                   [](unique_ptr<LogicalOperator> &child, BindingReplacementGraph &) {
				                                   while (child->type == LogicalOperatorType::LOGICAL_FILTER &&
				                                          child->Cast<LogicalFilter>().expressions.empty()) {
					                                   auto unwrapped = std::move(child->children[0]);
					                                   child = std::move(unwrapped);
				                                   }
			                                   });
		}
		if (join.join_type == JoinType::INNER && join.conditions.size() >= 2 &&
		    join.children[0]->type == LogicalOperatorType::LOGICAL_PROJECTION && ContainsRangeJoin(*join.children[0]) &&
		    !ContainsReorderingBarrier(join)) {
			auto &projection = join.children[0]->Cast<LogicalProjection>();
			bool safe = true;
			for (auto &expression : projection.expressions) {
				safe &= !expression->CanThrow();
			}
			for (auto &condition : join.conditions) {
				safe &= condition.IsComparison();
			}
			if (safe) {
				auto join_output = join.GetColumnBindings();
				auto right_bindings = join.children[1]->GetColumnBindings();
				join.children[1]->ResolveOperatorTypes();
				BindingReplacementGraph lifted_bindings;
				for (auto &condition : join.conditions) {
					for (auto expression : {&condition.LeftReference(), &condition.RightReference()}) {
						ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
						    *expression, [&](BoundColumnRefExpression &ref, unique_ptr<Expression> &node) {
							    if (ref.Binding().table_index == projection.table_index) {
								    node = projection.GetExpression(ref.Binding()).Copy();
							    }
						    });
					}
				}
				for (idx_t i = 0; i < right_bindings.size(); i++) {
					lifted_bindings.Add(
					    right_bindings[i],
					    ColumnBinding(projection.table_index, ProjectionIndex(projection.expressions.size())));
					projection.expressions.push_back(
					    make_uniq<BoundColumnRefExpression>(join.children[1]->types[i], right_bindings[i]));
				}
				join.left_projection_map.clear();
				join.right_projection_map.clear();
				join.filter_pushdown.reset();
				auto lifted = std::move(join.children[0]);
				join.children[0] = std::move(lifted->children[0]);
				lifted->children[0] = std::move(op);
				lifted->has_estimated_cardinality = false;
				op = std::move(lifted);
				ColumnBindingRewrite::RewriteChild(op, 0, rewrite_child);
				for (auto &binding : join_output) {
					binding = lifted_bindings.Resolve(binding);
				}
				RestoreOutputLayout(op, join_output);
				changed = true;
			}
		}
	}
	auto new_output = op->GetColumnBindings();
	D_ASSERT(old_output.size() == new_output.size());
	for (idx_t i = 0; i < old_output.size(); i++) {
		replacements.Add(old_output[i], new_output[i]);
	}
}

// Move a smaller C before A x B (or A JOIN B in ordinary sampled plans).
// Keep every predicate and the A,B,C output order through A JOIN (B JOIN C).
static void ReorderSamplingJoins(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &op,
                                 bool include_inner = false) {
	for (auto &child : op->children) {
		ReorderSamplingJoins(input, child, include_inner);
	}
	if (op->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return;
	}
	auto &join = op->Cast<LogicalComparisonJoin>();
	if (join.join_type != JoinType::INNER || ContainsReorderingBarrier(join)) {
		return;
	}
	auto *left_slot = &join.children[0];
	while (include_inner && (*left_slot)->type == LogicalOperatorType::LOGICAL_FILTER &&
	       (*left_slot)->Cast<LogicalFilter>().expressions.empty()) {
		left_slot = &(*left_slot)->children[0];
	}
	auto &cross = **left_slot;
	if (cross.type != LogicalOperatorType::LOGICAL_CROSS_PRODUCT &&
	    (!include_inner || cross.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
	     cross.Cast<LogicalComparisonJoin>().join_type != JoinType::INNER)) {
		return;
	}
	if (cross.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		if (!ContainsRangeJoin(cross)) {
			return;
		}
		for (auto &condition : cross.Cast<LogicalComparisonJoin>().conditions) {
			if (!condition.IsComparison() || condition.GetLHS().CanThrow() || condition.GetRHS().CanThrow()) {
				return;
			}
		}
	}
	auto a_size = cross.children[0]->EstimateCardinality(input.context);
	auto c_size = join.children[1]->EstimateCardinality(input.context);
	if (!c_size || c_size >= a_size) {
		return;
	}
	auto a = cross.children[0]->GetColumnBindings();
	auto b = cross.children[1]->GetColumnBindings();
	vector<bool> to_inner;
	idx_t inner_count = 0;
	for (auto &condition : join.conditions) {
		if (!condition.IsComparison() || condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL) {
			return;
		}
		ColumnBinding key;
		if (include_inner) {
			JoinFilterPushdownColumn left_column, right_column;
			if (condition.GetLHS().CanThrow() || condition.GetRHS().CanThrow() ||
			    !JoinFilterPushdownUtil::PushdownJoinFilterExpression(condition.GetLHS(), left_column) ||
			    !JoinFilterPushdownUtil::PushdownJoinFilterExpression(condition.GetRHS(), right_column)) {
				return;
			}
			key = left_column.probe_column_index;
		} else {
			if (condition.GetLHS().GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
			    condition.GetRHS().GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				return;
			}
			key = condition.GetLHS().Cast<BoundColumnRefExpression>().Binding();
		}
		bool is_b = std::find(b.begin(), b.end(), key) != b.end();
		if (!is_b && std::find(a.begin(), a.end(), key) == a.end()) {
			return;
		}
		to_inner.push_back(is_b);
		inner_count += is_b;
	}
	if (!inner_count || inner_count == join.conditions.size()) {
		return;
	}
	auto old_bindings = join.GetColumnBindings();
	auto inner = make_uniq<LogicalComparisonJoin>(JoinType::INNER);
	vector<JoinCondition> outer_conditions;
	for (idx_t i = 0; i < join.conditions.size(); i++) {
		(to_inner[i] ? inner->conditions : outer_conditions).push_back(std::move(join.conditions[i]));
	}
	if (cross.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		for (auto &condition : cross.Cast<LogicalComparisonJoin>().conditions) {
			outer_conditions.push_back(std::move(condition));
		}
	}
	inner->children.push_back(std::move(cross.children[1]));
	inner->children.push_back(std::move(join.children[1]));
	auto left = std::move(cross.children[0]);
	join.children[0] = std::move(left);
	join.children[1] = std::move(inner);
	join.conditions = std::move(outer_conditions);
	join.left_projection_map.clear();
	join.right_projection_map.clear();
	join.filter_pushdown.reset();
	join.ResolveOperatorTypes();
	if (!DBConfig::GetConfig(input.context).IsOptimizerDisabled(OptimizerType::JOIN_FILTER_PUSHDOWN)) {
		JoinFilterPushdownOptimizer filters(input.optimizer);
		filters.VisitOperator(join);
	}
	RestoreOutputLayout(op, old_bindings);
	PRIVACY_DEBUG_PRINT("[AS] Reordered sampled joins to reduce intermediate rows");
	if (include_inner) {
		ReorderSamplingJoins(input, op, true);
	}
}

// Resolve a witness key through projections to a key-only ownership lookup.
// Restricting this lookup to outer-domain keys cannot change an existence test.
static unique_ptr<LogicalOperator> *FindDomainLookup(unique_ptr<LogicalOperator> &op, const Expression &key,
                                                     unique_ptr<Expression> &lookup_key) {
	JoinFilterPushdownColumn column;
	if (!JoinFilterPushdownUtil::PushdownJoinFilterExpression(key, column)) {
		return nullptr;
	}
	if (op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = op->Cast<LogicalProjection>();
		if (column.probe_column_index.table_index != projection.table_index) {
			return nullptr;
		}
		auto projected = key.Copy();
		ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
		    projected, [&](BoundColumnRefExpression &ref, unique_ptr<Expression> &expression) {
			    expression = projection.GetExpression(ref.Binding()).Copy();
		    });
		return FindDomainLookup(op->children[0], *projected, lookup_key);
	}
	if (op->type == LogicalOperatorType::LOGICAL_FILTER) {
		return FindDomainLookup(op->children[0], key, lookup_key);
	}
	if (op->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return nullptr;
	}
	auto &join = op->Cast<LogicalComparisonJoin>();
	if (join.join_type != JoinType::INNER) {
		return nullptr;
	}
	for (auto &condition : join.conditions) {
		if (!condition.IsComparison() || condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL ||
		    condition.GetLHS().GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
		    condition.GetRHS().GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
		    condition.GetLHS().GetReturnType() != condition.GetRHS().GetReturnType()) {
			continue;
		}
		auto left = condition.GetLHS().Cast<BoundColumnRefExpression>().Binding();
		auto right = condition.GetRHS().Cast<BoundColumnRefExpression>().Binding();
		if (column.probe_column_index != left && column.probe_column_index != right) {
			continue;
		}
		for (idx_t side = 0; side < 2; side++) {
			if (join.children[side]->types.size() == 1 && ScanCardinality(*join.children[side])) {
				if (key.CanThrow()) {
					return nullptr;
				}
				lookup_key = key.Copy();
				ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
				    lookup_key, [&](BoundColumnRefExpression &ref, unique_ptr<Expression> &expression) {
					    expression = (side == 0 ? condition.GetLHS() : condition.GetRHS()).Copy();
				    });
				return &join.children[side];
			}
		}
	}
	return nullptr;
}

static bool IsNullPreservingKey(const Expression &key) {
	if (key.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		return true;
	}
	if (!BoundCastExpression::IsCast(key)) {
		return false;
	}
	auto &cast = key.Cast<BoundFunctionExpression>();
	return BoundCastExpression::CastIsInvertible(BoundCastExpression::SourceType(cast), cast.GetReturnType()) &&
	       IsNullPreservingKey(BoundCastExpression::Child(cast));
}

static void AddAntiJoinFilter(OptimizerExtensionInput &input, LogicalComparisonJoin &join, bool filtered_domain);

static bool ContainsDomainReference(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF || op.type == LogicalOperatorType::LOGICAL_DELIM_GET) {
		return true;
	}
	for (auto &child : op.children) {
		if (ContainsDomainReference(*child)) {
			return true;
		}
	}
	return false;
}

// A single materialized outer stream supplies both the output rows and the key
// domain of multiple existence tests. Explicit semi joins restrict key-only
// ownership builds before their hash tables are allocated, rather than relying
// on runtime filters that may arrive after those builds have already finished.
static bool ShareMembershipDomain(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                                  unique_ptr<LogicalOperator> &op) {
	vector<LogicalComparisonJoin *> memberships;
	auto rows = &op;
	while (true) {
		if ((*rows)->type == LogicalOperatorType::LOGICAL_FILTER && (*rows)->expressions.empty()) {
			rows = &(*rows)->children[0];
			continue;
		}
		if ((*rows)->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
			break;
		}
		auto &join = (*rows)->Cast<LogicalComparisonJoin>();
		if (join.join_type == JoinType::SEMI || join.join_type == JoinType::ANTI) {
			memberships.push_back(&join);
			rows = &join.children[0];
		} else if (join.join_type == JoinType::RIGHT_SEMI || join.join_type == JoinType::RIGHT_ANTI) {
			memberships.push_back(&join);
			rows = &join.children[1];
		} else {
			break;
		}
	}
	if (memberships.size() >= 2 && (*rows)->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN &&
	    (*rows)->Cast<LogicalComparisonJoin>().join_type == JoinType::INNER &&
	    ((ScanCardinality(*(*rows)->children[0]) && JoinFilterPushdownOptimizer::IsFiltering((*rows)->children[0])) ||
	     (ScanCardinality(*(*rows)->children[1]) && JoinFilterPushdownOptimizer::IsFiltering((*rows)->children[1]))) &&
	    !ContainsReorderingBarrier(*op) && !ContainsDomainReference(**rows)) {
		auto bindings = (*rows)->GetColumnBindings();
		vector<unique_ptr<LogicalOperator> *> lookups;
		vector<unique_ptr<Expression>> lookup_keys;
		vector<idx_t> domain_keys;
		vector<ExpressionType> comparisons;
		vector<JoinCondition *> domain_conditions;
		for (auto membership : memberships) {
			auto preserved = membership->join_type == JoinType::SEMI || membership->join_type == JoinType::ANTI ? 0 : 1;
			for (auto &condition : membership->conditions) {
				if (!condition.IsComparison() ||
				    (condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL &&
				     condition.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM)) {
					continue;
				}
				auto &domain_key = preserved == 0 ? condition.GetLHS() : condition.GetRHS();
				auto &witness_key = preserved == 0 ? condition.GetRHS() : condition.GetLHS();
				if (domain_key.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
				    !domain_key.GetReturnType().IsIntegral()) {
					continue;
				}
				auto binding = domain_key.Cast<BoundColumnRefExpression>().Binding();
				auto entry = std::find(bindings.begin(), bindings.end(), binding);
				if (entry == bindings.end()) {
					continue;
				}
				unique_ptr<Expression> key;
				auto lookup = FindDomainLookup(membership->children[1 - preserved], witness_key, key);
				if (!lookup || key->GetReturnType() != domain_key.GetReturnType()) {
					continue;
				}
				// The strict ownership equality already excludes NULL keys. A
				// lossless cast preserves that proof even after introducing CTE refs.
				auto comparison = condition.GetComparisonType();
				if (comparison == ExpressionType::COMPARE_NOT_DISTINCT_FROM && IsNullPreservingKey(*key)) {
					comparison = ExpressionType::COMPARE_EQUAL;
				}
				lookups.push_back(lookup);
				lookup_keys.push_back(std::move(key));
				domain_keys.push_back(idx_t(entry - bindings.begin()));
				comparisons.push_back(comparison);
				domain_conditions.push_back(&condition);
				break;
			}
		}
		if (lookups.size() == memberships.size()) {
			auto cte_index = input.optimizer.binder.GenerateTableIndex();
			auto row_index = input.optimizer.binder.GenerateTableIndex();
			auto types = (*rows)->types;
			vector<Identifier> names;
			for (idx_t i = 0; i < types.size(); i++) {
				names.emplace_back("domain_" + std::to_string(i));
			}
			auto producer = std::move(*rows);
			*rows = make_uniq<LogicalCTERef>(row_index, cte_index, types, names);
			for (idx_t i = 0; i < lookups.size(); i++) {
				auto &condition = *domain_conditions[i];
				if (condition.GetComparisonType() != comparisons[i]) {
					condition = JoinCondition(condition.GetLHS().Copy(), condition.GetRHS().Copy(), comparisons[i]);
				}
				auto domain_index = input.optimizer.binder.GenerateTableIndex();
				auto reference = make_uniq<LogicalCTERef>(domain_index, cte_index, types, names);
				auto key_index = domain_keys[i];
				auto key = make_uniq<BoundColumnRefExpression>(types[key_index],
				                                               ColumnBinding(domain_index, ProjectionIndex(key_index)));
				auto filter = make_uniq<LogicalComparisonJoin>(JoinType::SEMI);
				filter->conditions.emplace_back(std::move(lookup_keys[i]), std::move(key), comparisons[i]);
				filter->children.push_back(std::move(*lookups[i]));
				filter->children.push_back(std::move(reference));
				filter->ResolveOperatorTypes();
				if (!DBConfig::GetConfig(input.context).IsOptimizerDisabled(OptimizerType::JOIN_FILTER_PUSHDOWN)) {
					JoinFilterPushdownOptimizer filters(input.optimizer);
					filters.VisitOperator(*filter);
				}
				*lookups[i] = std::move(filter);
			}
			ColumnBindingReplacer replacer;
			replacer.AddReplacements(bindings, (*rows)->GetColumnBindings());
			replacer.VisitOperator(*plan);
			plan = make_uniq<LogicalMaterializedCTE>(Identifier("as_membership_domain"), cte_index, types.size(),
			                                         std::move(producer), std::move(plan),
			                                         CTEMaterialize::CTE_MATERIALIZE_ALWAYS);
			plan->ResolveOperatorTypes();
			if (!DBConfig::GetConfig(input.context).IsOptimizerDisabled(OptimizerType::JOIN_FILTER_PUSHDOWN)) {
				for (auto membership : memberships) {
					AddAntiJoinFilter(input, *membership, true);
				}
			}
			PRIVACY_DEBUG_PRINT("[AS] Shared the correlation domain across ownership lookups");
			return true;
		}
	}
	for (auto &child : op->children) {
		if (ShareMembershipDomain(input, plan, child)) {
			return true;
		}
	}
	return false;
}

static void PrepareOwnershipProbe(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &op) {
	if (op->type == LogicalOperatorType::LOGICAL_PROJECTION || op->type == LogicalOperatorType::LOGICAL_FILTER) {
		PrepareOwnershipProbe(input, op->children[0]);
		return;
	}
	if (op->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return;
	}
	auto &join = op->Cast<LogicalComparisonJoin>();
	if (join.join_type != JoinType::INNER || join.children[0]->types.size() != 1 ||
	    join.children[1]->types.size() <= 1 || !JoinFilterPushdownOptimizer::IsFiltering(join.children[1]) ||
	    ContainsReorderingBarrier(*join.children[0])) {
		return;
	}
	auto left_rows = ScanCardinality(*join.children[0]);
	auto right_rows = ScanCardinality(*join.children[1]);
	if (!left_rows || left_rows >= right_rows / 2 || !join.children[1]->has_estimated_cardinality ||
	    join.children[1]->estimated_cardinality < left_rows / 2) {
		return;
	}
	auto old_bindings = join.GetColumnBindings();
	std::swap(join.children[0], join.children[1]);
	std::swap(join.left_projection_map, join.right_projection_map);
	for (auto &condition : join.conditions) {
		if (condition.IsComparison()) {
			condition.Swap();
		}
	}
	join.filter_pushdown.reset();
	JoinFilterPushdownOptimizer filters(input.optimizer);
	filters.VisitOperator(join);
	RestoreOutputLayout(op, old_bindings);
	PRIVACY_DEBUG_PRINT("[AS] Kept the filtered ownership scan on the anti-join probe pipeline");
}

static void AddAntiJoinFilter(OptimizerExtensionInput &input, LogicalComparisonJoin &join, bool filtered_domain) {
	if (join.join_type != JoinType::RIGHT_ANTI || join.filter_pushdown ||
	    (!filtered_domain && !JoinFilterPushdownOptimizer::IsFiltering(join.children[1])) ||
	    ContainsReorderingBarrier(*join.children[0])) {
		return;
	}
	// Null-safe equality can be normalized only with a non-null proof. Without
	// that proof, filtering NULL probe keys could remove a real NULL=NULL match.
	for (auto &condition : join.conditions) {
		if (!condition.IsComparison() || condition.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			continue;
		}
		if ((condition.GetLeftStats() && !condition.GetLeftStats()->CanHaveNull()) ||
		    (condition.GetRightStats() && !condition.GetRightStats()->CanHaveNull())) {
			condition =
			    JoinCondition(condition.GetLHS().Copy(), condition.GetRHS().Copy(), ExpressionType::COMPARE_EQUAL);
		}
	}
	PhysicalComparisonJoin::ReorderConditions(join.conditions);
	auto info = make_uniq<JoinFilterPushdownInfo>();
	vector<JoinFilterPushdownColumn> columns;
	for (idx_t i = 0; i < join.conditions.size(); i++) {
		auto &condition = join.conditions[i];
		if (!condition.IsComparison() || condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL) {
			continue;
		}
		JoinFilterPushdownColumn column;
		if (!JoinFilterPushdownUtil::PushdownJoinFilterExpression(condition.GetLHS(), column)) {
			continue;
		}
		column.join_filter_idx = info->join_condition.size();
		columns.push_back(std::move(column));
		info->join_condition.push_back(i);
	}
	if (columns.empty()) {
		return;
	}
	if (!DBConfig::GetConfig(input.context).IsOptimizerDisabled(OptimizerType::BUILD_SIDE_PROBE_SIDE)) {
		PrepareOwnershipProbe(input, join.children[0]);
	}
	vector<PushdownFilterTarget> targets;
	GetOwnershipFilterTargets(*join.children[0], std::move(columns), targets);
	if (targets.empty()) {
		return;
	}
	for (auto &index : info->join_condition) {
		for (auto name : {"min", "max"}) {
			vector<unique_ptr<Expression>> arguments;
			arguments.push_back(join.conditions[index].GetRHS().Copy());
			auto function = GetBuiltinAggregateFunction(input.context, name, {arguments[0]->GetReturnType()});
			FunctionBinder binder(input.context);
			auto aggregate = binder.BindAggregateFunction(std::move(function), std::move(arguments), nullptr,
			                                              AggregateType::NON_DISTINCT);
			if (aggregate->GetChildren().size() != 1) {
				return;
			}
			info->min_max_aggregates.push_back(std::move(aggregate));
		}
	}
	for (auto &target : targets) {
		auto &get = target.get;
		if (!get.dynamic_filters) {
			get.dynamic_filters = make_shared_ptr<DynamicTableFilterSet>();
		}
		JoinFilterPushdownFilter filter;
		filter.dynamic_filters = get.dynamic_filters;
		filter.columns = std::move(target.columns);
		if (get.has_estimated_cardinality) {
			filter.estimated_scan_cardinality = get.estimated_cardinality;
		}
		info->probe_info.push_back(std::move(filter));
	}
	info->build_side_has_filter = true;
	join.filter_pushdown = std::move(info);
	PRIVACY_DEBUG_PRINT("[AS] Pushed right-anti membership keys into ownership join inputs");
}

static void AddAntiJoinFilters(OptimizerExtensionInput &input, LogicalOperator &op) {
	for (auto &child : op.children) {
		AddAntiJoinFilters(input, *child);
	}
	if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		AddAntiJoinFilter(input, op.Cast<LogicalComparisonJoin>(), false);
	}
}

static void OptimizeASCorrelations(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	auto has_aggregate = ContainsASAggregate(*plan);
	if (!has_aggregate && !ContainsSamplingHash(*plan)) {
		return;
	}
	if (!DBConfig::GetConfig(input.context).IsOptimizerDisabled(OptimizerType::JOIN_ORDER)) {
		if (!has_aggregate) {
			// Reassociation can expose a projected key behind another join.
			// Lift it, then push the newly formed filtering relation deeper.
			bool lifted;
			do {
				ReorderSamplingJoins(input, plan);
				PushFilteredJoins(input, plan, true);
				ReorderSamplingJoins(input, plan, true);
				lifted = false;
				if (!DBConfig::GetConfig(input.context).IsOptimizerDisabled(OptimizerType::PROJECTION_PULLUP)) {
					BindingReplacementGraph bindings;
					LiftSamplingJoinProjections(plan, bindings, lifted);
				}
			} while (lifted);
		} else {
			ReorderSamplingJoins(input, plan);
		}
	}
	if (!has_aggregate) {
		return;
	}
	if (!DBConfig::GetConfig(input.context).IsOptimizerDisabled(OptimizerType::JOIN_ORDER)) {
		PushFilteredJoins(input, plan);
		ShareMembershipDomain(input, plan, plan);
	}
	if (!DBConfig::GetConfig(input.context).IsOptimizerDisabled(OptimizerType::JOIN_FILTER_PUSHDOWN)) {
		AddAntiJoinFilters(input, *plan);
	}
}

// Compression projections hide grouping keys from the later partitioning pass.
// Partition deterministic ordinary inputs before compression, leaving stochastic
// aggregates and correlated/CTE plans to the existing optimization pipeline.
static void PartitionOrdinaryASInputs(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &root,
                                      unique_ptr<LogicalOperator> &op) {
	if (op->type == LogicalOperatorType::LOGICAL_DEPENDENT_JOIN ||
	    op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN ||
	    op->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE ||
	    op->type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) {
		return;
	}
	if (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY && !ContainsASAggregate(*op) &&
	    !ContainsReorderingBarrier(*op)) {
		// The core tracer supports only projection/filter chains ending in one scan.
		// Match that shape here so its recursive traversal cannot enter a CTE or
		// correlated subtree before the guards above have seen it.
		auto *source = op->children[0].get();
		while ((source->type == LogicalOperatorType::LOGICAL_PROJECTION ||
		        source->type == LogicalOperatorType::LOGICAL_FILTER) &&
		       source->children.size() == 1) {
			source = source->children[0].get();
		}
		if (source->type == LogicalOperatorType::LOGICAL_GET && source->children.empty()) {
			auto original_type = op->type;
			PartitionedExecution partitioned_execution(input.optimizer, root);
			partitioned_execution.Optimize(op);
			if (op->type != original_type) {
				PRIVACY_DEBUG_PRINT("[AS] Partitioned deterministic aggregate input before compression");
			}
			return;
		}
	}
	for (auto &child : op->children) {
		PartitionOrdinaryASInputs(input, root, child);
	}
}

static void PartitionASInputs(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	if (!ContainsASAggregate(*plan) ||
	    DBConfig::GetConfig(input.context).IsOptimizerDisabled(OptimizerType::PARTITIONED_EXECUTION)) {
		return;
	}
	PartitionOrdinaryASInputs(input, plan, plan);
}

void RegisterASCorrelationOptimizer(DatabaseInstance &db) {
	PlannerExtension planner;
	planner.post_bind_function = ShareCorrelatedNoiseInputs;
	PlannerExtension::Register(DBConfig::GetConfig(db), std::move(planner));
	OptimizerExtension extension;
	extension.pre_optimize_function = PartitionASInputs;
	extension.optimize_function = OptimizeASCorrelations;
	OptimizerExtension::Register(DBConfig::GetConfig(db), std::move(extension));
}

} // namespace duckdb
