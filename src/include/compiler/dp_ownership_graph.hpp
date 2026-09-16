#pragma once

#include "duckdb.hpp"

namespace duckdb {

struct DPOwnershipEdgeAnalysis {
	string child_table;
	vector<string> local_columns;
	string parent_table;
	vector<string> referenced_columns;
	uint64_t observed_max_frequency;
	uint64_t child_max_owned_rows;
	uint64_t child_max_owners_per_row;
};

struct DPOwnershipAnalysis {
	string fact_table;
	string pu_table;
	uint64_t graph_max_affected_pus;
	uint64_t observed_max_affected_pus;
	vector<DPOwnershipEdgeAnalysis> edges;
};

// Computes exact, data-dependent diagnostics. Its output is not differentially private
// and must not be used as sensitivity metadata by a DP compiler.
DPOwnershipAnalysis AnalyzeObservedDPOwnershipGraph(ClientContext &context, const string &fact_table);

} // namespace duckdb
