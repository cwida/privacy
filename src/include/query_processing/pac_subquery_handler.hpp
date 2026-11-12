//
// PAC Subquery Handler
//
// This file contains functions for handling correlated subqueries and DELIM_JOIN operations
// in PAC query compilation. These functions help manage column accessibility and propagation
// across subquery boundaries.
//
// Created by ila on 1/22/26.
//

#ifndef PAC_SUBQUERY_HANDLER_HPP
#define PAC_SUBQUERY_HANDLER_HPP

#include "duckdb.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"

namespace duckdb {

// Result of adding a binding to a DELIM_JOIN.
struct DelimColumnResult {
	ColumnBinding binding;
	LogicalType type;

	bool IsValid() const {
		return binding.table_index.index != DConstants::INVALID_INDEX;
	}
};

// Add a pre-computed binding (e.g., from a hash projection) to a DELIM_JOIN.
// The source_table_index is used to locate the source operator in the DELIM_JOIN's left child.
DelimColumnResult AddBindingToDelimJoin(unique_ptr<LogicalOperator> &plan, idx_t source_table_index,
                                        ColumnBinding source_binding, const LogicalType &source_type,
                                        LogicalAggregate *target_agg);

} // namespace duckdb

#endif // PAC_SUBQUERY_HANDLER_HPP
