//
// Created by ila on 1/16/26.
//

#include "query_processing/pac_projection_propagation.hpp"
#include "privacy_debug.hpp"
#include "utils/privacy_helpers.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_delim_get.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

// Helper to check if an operator is a join type that has projection maps
static bool IsJoinWithProjectionMap(LogicalOperatorType type) {
	return type == LogicalOperatorType::LOGICAL_JOIN || type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
	       type == LogicalOperatorType::LOGICAL_DELIM_JOIN || type == LogicalOperatorType::LOGICAL_ANY_JOIN ||
	       type == LogicalOperatorType::LOGICAL_ASOF_JOIN || type == LogicalOperatorType::LOGICAL_DEPENDENT_JOIN;
}

struct PathEntry {
	LogicalOperator *op;
	idx_t child_idx; // Which child led to the target (0 = left, 1 = right for joins)
};

// Find path from an operator to a specific table_index, stopping at nested aggregates.
// Matches both LogicalGet and LogicalProjection (for hash projections inserted above gets).
static bool FindDirectPathToSource(LogicalOperator *current, idx_t target_table_index, vector<PathEntry> &path,
                                   bool is_start = true) {
	if (!current) {
		return false;
	}

	// Check if this operator matches the target table_index
	if (current->type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = current->Cast<LogicalGet>();
		if (get.table_index.index == target_table_index) {
			return true;
		}
	} else if (current->type == LogicalOperatorType::LOGICAL_CTE_REF) {
		if (current->Cast<LogicalCTERef>().table_index.index == target_table_index) {
			return true;
		}
	} else if (current->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &proj = current->Cast<LogicalProjection>();
		if (proj.table_index.index == target_table_index) {
			return true;
		}
	}

	// If this is a nested aggregate, check if it IS the target or if the target is below it.
	// Nested aggregates don't pass through bindings, but PropagateSingleBinding handles
	// them by adding the binding as a group column — so we continue the search.
	if (!is_start && current->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		auto &agg = current->Cast<LogicalAggregate>();
		if (agg.group_index.index == target_table_index || agg.aggregate_index.index == target_table_index) {
			return true;
		}
		// Continue searching through the aggregate's children — PropagateSingleBinding
		// will add the binding as a group column to pass it through.
	}

	for (idx_t child_idx = 0; child_idx < current->children.size(); child_idx++) {
		if (FindDirectPathToSource(current->children[child_idx].get(), target_table_index, path, false)) {
			// Skip the starting aggregate (it's the target, not an intermediate operator)
			// but include nested aggregates so PropagateSingleBinding can add group columns.
			if (!is_start) {
				path.push_back({current, child_idx});
			}
			return true;
		}
	}
	return false;
}

// Ensure a single column_index is present in a projection map. Returns true if it was already there.
static bool EnsureInProjectionMap(vector<ProjectionIndex> &proj_map, ProjectionIndex column_index) {
	if (proj_map.empty()) {
		return true; // Empty map means all columns pass through
	}
	for (auto &idx : proj_map) {
		if (idx == column_index) {
			return true;
		}
	}
	proj_map.push_back(column_index);
	return false;
}

// Verify a binding exists in an operator's output bindings
static bool BindingInOutput(LogicalOperator &op, const ColumnBinding &binding) {
	auto bindings = op.GetColumnBindings();
	for (auto &b : bindings) {
		if (b.table_index.index == binding.table_index.index && b.column_index == binding.column_index) {
			return true;
		}
	}
	return false;
}

// Find a CTE reader on the output tuple stream, without crossing an aggregation
// or the definition side of another CTE. Correlation-key deduplication must not
// acquire the entity mask as an extra grouping key.
static LogicalCTERef *FindOutputCTERef(LogicalOperator &op, TableIndex cte_index) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &ref = op.Cast<LogicalCTERef>();
		return ref.cte_index == cte_index ? &ref : nullptr;
	}
	if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return nullptr;
	}
	if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		return FindOutputCTERef(*op.children[1], cte_index);
	}
	for (idx_t i = 0; i < op.children.size(); i++) {
		if (IsJoinWithProjectionMap(op.type)) {
			auto join_type = op.Cast<LogicalJoin>().join_type;
			if ((i == 1 &&
			     (join_type == JoinType::SEMI || join_type == JoinType::ANTI || join_type == JoinType::MARK)) ||
			    (i == 0 && (join_type == JoinType::RIGHT_SEMI || join_type == JoinType::RIGHT_ANTI))) {
				continue;
			}
		}
		auto *ref = FindOutputCTERef(*op.children[i], cte_index);
		if (ref) {
			return ref;
		}
	}
	return nullptr;
}

static void SyncCTERefColumns(LogicalOperator &op, TableIndex cte_index, const vector<LogicalType> &types) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &ref = op.Cast<LogicalCTERef>();
		if (ref.cte_index == cte_index) {
			while (ref.chunk_types.size() < types.size()) {
				ref.bound_columns.emplace_back("__privacy_mask_" + std::to_string(ref.chunk_types.size()));
				ref.chunk_types.push_back(types[ref.chunk_types.size()]);
			}
			ref.ResolveOperatorTypes();
		}
	}
	for (auto &child : op.children) {
		SyncCTERefColumns(*child, cte_index, types);
	}
}

static ColumnBinding PropagateBinding(LogicalOperator &plan_root, idx_t source_table_index,
                                      ColumnBinding source_binding, const LogicalType &source_type,
                                      LogicalOperator *target, bool include_target) {
	ColumnBinding invalid {TableIndex(DConstants::INVALID_INDEX), ProjectionIndex(DConstants::INVALID_INDEX)};

	vector<PathEntry> path_ops;
	if (!FindDirectPathToSource(target, source_table_index, path_ops, !include_target)) {
#if PRIVACY_DEBUG
		PRIVACY_DEBUG_PRINT("PropagateSingleBinding: No direct path from aggregate to source #" +
		                    std::to_string(source_table_index));
#endif
		return invalid;
	}

	if (path_ops.empty()) {
		// Aggregate reads directly from source — binding is already valid
		return source_binding;
	}

	// Propagate through each operator (bottom-up: closest to source first)
	ColumnBinding current = source_binding;

	for (auto &entry : path_ops) {
		auto *op = entry.op;
		idx_t child_idx = entry.child_idx;

		if (op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
			auto &proj = op->Cast<LogicalProjection>();

			// Check if this binding is already in the projection's expressions
			idx_t existing_idx = DConstants::INVALID_INDEX;
			for (idx_t i = 0; i < proj.expressions.size(); i++) {
				if (proj.expressions[i]->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
					auto &expr_ref = proj.expressions[i]->Cast<BoundColumnRefExpression>();
					if (expr_ref.BindingMutable() == current) {
						existing_idx = i;
						break;
					}
				}
			}

			if (existing_idx != DConstants::INVALID_INDEX) {
				current = ColumnBinding(TableIndex(proj.table_index.index), ProjectionIndex(existing_idx));
			} else {
				// Add a passthrough column ref for this binding
				auto col_ref = make_uniq<BoundColumnRefExpression>(source_type, current);
				proj.expressions.push_back(std::move(col_ref));
				current =
				    ColumnBinding(TableIndex(proj.table_index.index), ProjectionIndex(proj.expressions.size() - 1));
			}
			proj.ResolveOperatorTypes();

		} else if (op->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE && child_idx == 0) {
			auto &cte = op->Cast<LogicalMaterializedCTE>();
			auto *ref = FindOutputCTERef(*cte.children[1], cte.table_index);
			if (!ref) {
				return invalid;
			}
			cte.children[0]->ResolveOperatorTypes();
			auto bindings = cte.children[0]->GetColumnBindings();
			auto column = std::find(bindings.begin(), bindings.end(), current);
			if (column == bindings.end()) {
				return invalid;
			}
			auto position = idx_t(column - bindings.begin());
			cte.column_count = bindings.size();
			SyncCTERefColumns(*cte.children[1], cte.table_index, cte.children[0]->types);
			current = PropagateBinding(plan_root, ref->table_index.index,
			                           ColumnBinding(ref->table_index, ProjectionIndex(position)), source_type,
			                           cte.children[1].get(), true);
			if (current.table_index.index == DConstants::INVALID_INDEX) {
				return invalid;
			}
			PRIVACY_DEBUG_PRINT("PropagateSingleBinding: propagated mask through CTE #" +
			                    std::to_string(cte.table_index.index));
			cte.ResolveOperatorTypes();

		} else if (IsJoinWithProjectionMap(op->type)) {
			auto &join = op->Cast<LogicalJoin>();
			bool is_delim_join = (op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN);

			// Check for incompatible join types
			if ((join.join_type == JoinType::RIGHT_SEMI || join.join_type == JoinType::RIGHT_ANTI) && child_idx == 0) {
				return invalid;
			}
			if ((join.join_type == JoinType::SEMI || join.join_type == JoinType::ANTI ||
			     join.join_type == JoinType::MARK) &&
			    child_idx == 1) {
				return invalid;
			}

			// DELIM_JOIN from left child
			if (is_delim_join && child_idx == 0) {
				EnsureInProjectionMap(join.left_projection_map, current.column_index);
				join.ResolveOperatorTypes();
				// Binding passes through unchanged for DELIM_JOIN left child
				continue;
			}

			// Normal join: ensure column is in the appropriate projection map
			if (child_idx == 0) {
				EnsureInProjectionMap(join.left_projection_map, current.column_index);
			} else {
				EnsureInProjectionMap(join.right_projection_map, current.column_index);
			}
			join.ResolveOperatorTypes();

			// Verify binding is in the output (joins pass through child bindings)
			if (!BindingInOutput(join, current)) {
#if PRIVACY_DEBUG
				PRIVACY_DEBUG_PRINT("PropagateSingleBinding: WARNING - binding [" +
				                    std::to_string(current.table_index.index) + "." +
				                    std::to_string(current.column_index) + "] not found in join output");
#endif
			}
			// Binding identity preserved through joins

		} else if (op->type == LogicalOperatorType::LOGICAL_FILTER) {
			auto &filter = op->Cast<LogicalFilter>();
			EnsureInProjectionMap(filter.projection_map, current.column_index);
			filter.ResolveOperatorTypes();
			// Binding identity preserved through filters

		} else if (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
			auto &agg = op->Cast<LogicalAggregate>();
			// Check if the binding already passes through as a group column
			bool found_in_groups = false;
			for (idx_t i = 0; i < agg.groups.size(); i++) {
				if (agg.groups[i]->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
					auto &col_ref = agg.groups[i]->Cast<BoundColumnRefExpression>();
					if (col_ref.BindingMutable() == current) {
						current = ColumnBinding(TableIndex(agg.group_index.index), ProjectionIndex(i));
						found_in_groups = true;
						break;
					}
				}
			}
			if (!found_in_groups) {
				// Aggregate blocks this binding — cannot propagate
				return invalid;
			}

		} else {
			op->ResolveOperatorTypes();
		}
	}

	return current;
}

ColumnBinding PropagateSingleBindingToOperator(LogicalOperator &plan_root, idx_t source_table_index,
                                               ColumnBinding source_binding, const LogicalType &source_type,
                                               LogicalOperator *target) {
	return PropagateBinding(plan_root, source_table_index, source_binding, source_type, target, false);
}

ColumnBinding PropagateSingleBinding(LogicalOperator &plan_root, idx_t source_table_index, ColumnBinding source_binding,
                                     const LogicalType &source_type, LogicalAggregate *target_agg) {
	return PropagateSingleBindingToOperator(plan_root, source_table_index, source_binding, source_type, target_agg);
}

} // namespace duckdb
