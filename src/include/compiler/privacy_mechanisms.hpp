#ifndef PRIVACY_MECHANISMS_HPP
#define PRIVACY_MECHANISMS_HPP

#include "duckdb.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "metadata/privacy_compatibility_check.hpp"

namespace duckdb {

// Strategy entry points used by the unified privacy compiler, selected via `SET privacy_mode`.

// dp_standard: global-sensitivity Laplace, pure ε-DP.
void CompileDPStandardQuery(const PrivacyCompatibilityResult &check, OptimizerExtensionInput &input,
                            unique_ptr<LogicalOperator> &plan, const vector<string> &privacy_units,
                            const string &query_hash);

// dp_elastic: smooth elastic-sensitivity Laplace, (ε,δ)-DP.
void CompileDPElasticQuery(const PrivacyCompatibilityResult &check, OptimizerExtensionInput &input,
                           unique_ptr<LogicalOperator> &plan, const vector<string> &privacy_units,
                           const string &query_hash);

void CompileDPSampleMedianQuery(const PrivacyCompatibilityResult &check, OptimizerExtensionInput &input,
                                unique_ptr<LogicalOperator> &plan, const vector<string> &privacy_units,
                                const string &query_hash);

} // namespace duckdb

#endif // PRIVACY_MECHANISMS_HPP
