#ifndef PAC_DERIVED_REWRITER_HPP
#define PAC_DERIVED_REWRITER_HPP

#include "duckdb.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {

// Write path: convert priv_noised_* → priv_* counter variants for DML targeting derived_pu tables.
void ConvertDerivedPuToCounters(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

// Read path: inject priv_finalize for LIST<FLOAT> columns from derived_pu tables.
void InjectPacFinalizeForDerivedPu(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

// Check if the plan reads from any derived_pu table with counter columns.
bool HasDerivedPuCounterGets(LogicalOperator *plan);

} // namespace duckdb

#endif // PAC_DERIVED_REWRITER_HPP
