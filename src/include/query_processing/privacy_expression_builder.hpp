//
// Created by ila on 1/6/26.
//

#ifndef PRIVACY_EXPRESSION_BUILDER_HPP
#define PRIVACY_EXPRESSION_BUILDER_HPP

#include "duckdb.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "metadata/privacy_compatibility_check.hpp"

namespace duckdb {

class BoundAggregateExpression;

// Ensure a column is projected in a LogicalGet and return its projection index
// Returns DConstants::INVALID_INDEX if the column cannot be found
idx_t EnsureProjectedColumn(LogicalGet &g, const string &col_name);

// Ensure PK columns are present in a LogicalGet's column_ids and projection_ids
void AddPKColumns(LogicalGet &get, const vector<string> &pks);

// Helper to ensure rowid is present in the output columns of a LogicalGet
void AddRowIDColumn(LogicalGet &get);

// Hash one or more column expressions: hash(c1) XOR hash(c2) XOR ...
unique_ptr<Expression> BuildXorHash(OptimizerExtensionInput &input, vector<unique_ptr<Expression>> cols);

// Build hash expression for the given LogicalGet's primary key columns
unique_ptr<Expression> BuildXorHashFromPKs(OptimizerExtensionInput &input, LogicalGet &get, const vector<string> &pks);

// Insert a LogicalProjection above a LogicalGet that computes hash(key_columns) as an extra column.
// Remaps all existing references to the get's bindings using ColumnBindingReplacer.
// Returns the ColumnBinding for the new hash column in the projection's output.
ColumnBinding InsertHashProjectionAboveGet(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                                           LogicalGet &get, const vector<string> &key_columns, bool use_rowid);

// Caching wrapper around InsertHashProjectionAboveGet. Keyed by get.table_index.
// Returns cached binding if hash projection was already inserted for this get.
ColumnBinding GetOrInsertHashProjection(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                                        LogicalGet &get, const vector<string> &key_columns, bool use_rowid,
                                        std::unordered_map<idx_t, ColumnBinding> &cache);

// Insert a hash projection above a CTE_SCAN (LogicalCTERef) node.
// The CTE_SCAN must expose the key columns by name in its bound_columns.
// Returns the ColumnBinding for the hash column, or INVALID if key columns not found.
ColumnBinding InsertHashProjectionAboveCTERef(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                                              LogicalCTERef &cte_ref, const vector<string> &key_columns);

// Build AND expression from multiple hash expressions (for multiple PUs)
unique_ptr<Expression> BuildAndFromHashes(OptimizerExtensionInput &input, vector<unique_ptr<Expression>> &hash_exprs);

// Bind a PAC aggregate function (as_sum, as_count, etc.) with hash + value arguments,
// and an optional correction factor.
unique_ptr<Expression> BindPacAggregate(OptimizerExtensionInput &input, const string &pac_func_name,
                                        unique_ptr<Expression> hash_expr, unique_ptr<Expression> value_expr,
                                        unique_ptr<Expression> correction_expr = nullptr);

// Bind a plain DuckDB aggregate function (sum, count, count_star, etc.) by name.
unique_ptr<Expression> BindPlainAggregate(OptimizerExtensionInput &input, const string &func_name,
                                          vector<unique_ptr<Expression>> children,
                                          AggregateType aggr_type = AggregateType::NON_DISTINCT);

// Convenience overload for zero-or-one argument aggregates.
unique_ptr<Expression> BindPlainAggregate(OptimizerExtensionInput &input, const string &func_name,
                                          unique_ptr<Expression> arg,
                                          AggregateType aggr_type = AggregateType::NON_DISTINCT);

struct PuPreAggregationInfo {
	LogicalAggregate *lower_agg;
	idx_t num_original_groups;
	idx_t lower_agg_index;
	unique_ptr<Expression> pu_hash_ref;
};

struct DistinctAggregateBranchResult {
	unique_ptr<LogicalOperator> subtree;
	idx_t group_index;
	idx_t agg_index;
	vector<idx_t> original_agg_indices;
	vector<LogicalType> aggregate_types;
};

struct JoinedDistinctAggregateBranches {
	unique_ptr<LogicalOperator> joined;
	idx_t left_group_index;
	vector<DistinctAggregateBranchResult> branches;
	std::unordered_map<idx_t, std::pair<idx_t, idx_t>> aggregate_map;
};

class DistinctAggregateBranchBuilder {
public:
	virtual ~DistinctAggregateBranchBuilder() {
	}

	virtual unique_ptr<LogicalOperator>
	BuildDistinctBranch(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> child,
	                    const vector<unique_ptr<Expression>> &groups, unique_ptr<Expression> hash_expr,
	                    unique_ptr<Expression> distinct_expr, const vector<std::pair<idx_t, string>> &agg_specs,
	                    const std::unordered_map<idx_t, idx_t> *index_map, idx_t &out_group_index,
	                    idx_t &out_agg_index) = 0;

	virtual unique_ptr<LogicalOperator> BuildNonDistinctBranch(
	    OptimizerExtensionInput &input, unique_ptr<LogicalOperator> child, const vector<unique_ptr<Expression>> &groups,
	    unique_ptr<Expression> hash_expr, const vector<std::pair<idx_t, const BoundAggregateExpression *>> &agg_specs,
	    const std::unordered_map<idx_t, idx_t> *index_map, idx_t &out_group_index, idx_t &out_agg_index) = 0;
};

// Build the shared multi-branch shape for mixed/multiple DISTINCT aggregates.
// Mechanism-specific callers provide branch builders for the aggregate expressions themselves.
JoinedDistinctAggregateBranches BuildJoinedDistinctAggregateBranches(OptimizerExtensionInput &input,
                                                                     unique_ptr<LogicalOperator> &plan,
                                                                     LogicalAggregate *agg,
                                                                     unique_ptr<Expression> hash_expr,
                                                                     DistinctAggregateBranchBuilder &builder);

// Insert a lower aggregate grouped by [original groups..., PU hash], move the current aggregate child below it,
// and rewrite the top aggregate's groups to reference the lower group output. The caller owns aggregate-specific
// lower expressions and must rewrite the top aggregate expressions after this helper returns.
PuPreAggregationInfo InsertPuPreAggregation(OptimizerExtensionInput &input, LogicalAggregate *agg,
                                            vector<unique_ptr<Expression>> lower_expressions,
                                            unique_ptr<Expression> pu_hash_expr);

// Bind bit_or(hash_expr) with an IS NOT NULL filter on filter_col_expr.
// Returns the bound aggregate expression.
unique_ptr<Expression> BindBitOrAggregate(OptimizerExtensionInput &input, unique_ptr<Expression> hash_expr,
                                          unique_ptr<Expression> filter_col_expr);

// Modify aggregate expressions to use PAC functions (replaces the aggregate loop logic).
// correction: multiplicative factor for multi-PU queries (2^(m-1) where m = number of PUs).
void ModifyAggregatesWithPacFunctions(OptimizerExtensionInput &input, LogicalAggregate *agg,
                                      unique_ptr<Expression> &hash_input_expr, unique_ptr<LogicalOperator> &plan,
                                      double correction = 1.0);

// Rewrite PAC aggregates to use clipping variants when priv_clip_support is set.
// Inserts a lower aggregate with plain DuckDB aggregates (GROUP BY groups + PU hash),
// and rewrites the top aggregate to use as_noised_clip_* / as_clip_* functions.
// Skips insertion if child already groups by PU key (Q13 exception).
void RewriteClipAggregates(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                           const PrivacyCompatibilityResult &check, const vector<string> &privacy_units);

} // namespace duckdb

#endif // PRIVACY_EXPRESSION_BUILDER_HPP
