#ifndef PAC_AVG_REWRITER_HPP
#define PAC_AVG_REWRITER_HPP

#include "duckdb.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {

// Post-processing: replace as_noised_avg/as_avg with sum/count + division.
// Runs as a standalone optimizer so it works for both compiler-generated and user-written as_avg() SQL.
void RewritePacAvgToDiv(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

} // namespace duckdb

#endif // PAC_AVG_REWRITER_HPP
