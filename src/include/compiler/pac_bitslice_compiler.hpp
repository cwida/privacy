//
// Created by ila on 12/21/25.
//

#ifndef PAC_BITSLICE_COMPILER_HPP
#define PAC_BITSLICE_COMPILER_HPP

#include "duckdb.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "metadata/privacy_compatibility_check.hpp"
#include "categorical/pac_categorical_detection.hpp"
#include "query_processing/pac_plan_traversal.hpp"

namespace duckdb {

// forward-declare LogicalGet to avoid including heavy headers in this header
class LogicalGet;
class LogicalAggregate;

struct PacPUHashingSetup {
	CTETableMap cte_map;
	vector<string> pu_names;

	PacPUHashingSetup(CTETableMap cte_map, vector<string> pu_names)
	    : cte_map(std::move(cte_map)), pu_names(std::move(pu_names)) {
	}
};

struct PacAggregateHashInfo {
	LogicalAggregate *aggregate;
	unique_ptr<Expression> hash_expr;
	ColumnBinding hash_binding;
	double correction;

	PacAggregateHashInfo(LogicalAggregate *aggregate, unique_ptr<Expression> hash_expr, ColumnBinding hash_binding,
	                     double correction)
	    : aggregate(aggregate), hash_expr(std::move(hash_expr)), hash_binding(hash_binding), correction(correction) {
	}
};

// Helper to ensure PK columns are present in a LogicalGet's column_ids and projection_ids.
void AddPKColumns(LogicalGet &get, const vector<string> &pks);

// Helper to inspect FK paths and populate lists of GETs present/missing. Defined in pac_bitslice_compiler.cpp
void PopulateGetsFromFKPath(const PrivacyCompatibilityResult &check, vector<string> &gets_present,
                            vector<string> &gets_missing, string &start_table_out, vector<string> &target_pus_out);

// Reuse the PAC FK-join materialization path so downstream code can find and propagate PU hashes.
PacPUHashingSetup PreparePlanForPUHashing(const PrivacyCompatibilityResult &check, OptimizerExtensionInput &input,
                                          unique_ptr<LogicalOperator> &plan, const vector<string> &privacy_units);

// Shared PU hash propagation. Returns one propagated hash expression per aggregate selected by PAC's
// target-aggregate discovery, without rebinding the aggregate expressions.
vector<PacAggregateHashInfo> BuildPUHashExpressionsForAggregates(OptimizerExtensionInput &input,
                                                                 unique_ptr<LogicalOperator> &plan,
                                                                 const vector<string> &pu_table_names,
                                                                 const PrivacyCompatibilityResult &check,
                                                                 const CTETableMap &cte_map);

// Unified aggregate transformation: builds hash expressions from PU/FK tables and
// transforms aggregates to use PAC functions. Works for both direct-PU and FK-joined plans.
void ModifyPlanWithPU(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                      const vector<string> &pu_table_names, const PrivacyCompatibilityResult &check,
                      const CTETableMap &cte_map, PacAggregateInfoMap &pac_agg_info);

// Bitslice-style PAC compiler entrypoint
void CompilePacBitsliceQuery(const PrivacyCompatibilityResult &check, OptimizerExtensionInput &input,
                             unique_ptr<LogicalOperator> &plan, const vector<string> &privacy_units,
                             const string &query, const string &query_hash);

} // namespace duckdb

#endif // PAC_BITSLICE_COMPILER_HPP
