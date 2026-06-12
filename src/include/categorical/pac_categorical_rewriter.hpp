//
// PAC Categorical Query Rewriter
//
// This file declares the rewriting phase of categorical query handling.
// Detection is in pac_categorical_detection.hpp/.cpp.
//
// Rewriting:
// - Replaces pac_* aggregates with pac_*_counters variants (return LIST<DOUBLE>)
// - Wraps comparisons with pac_gt/pac_lt/etc. functions (return UBIGINT mask)
// - Adds priv_filter at the outermost filter to make final probabilistic decision
//
// Example transformation for TPC-H Q20:
//   BEFORE: ps_availqty > (SELECT 0.5 * as_sum(hash, l_quantity) FROM ...)
//   AFTER:  priv_filter(pac_gt(ps_availqty, (SELECT 0.5 * as_sum(hash, l_quantity) FROM ...)))
//
// Created by ila on 1/23/26.
//

#ifndef PAC_CATEGORICAL_REWRITER_HPP
#define PAC_CATEGORICAL_REWRITER_HPP

#include "categorical/pac_categorical_detection.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/function/lambda_functions.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/function/scalar/struct_functions.hpp"

namespace duckdb {

// The kind of wrapping to apply after BuildCounterListTransform
enum class PacWrapKind {
	PAC_NOISED, // Projection: list_transform -> as_noised -> optional cast
	PAC_FILTER, // Filter/Join: list_transform -> priv_filter
	PAC_SELECT  // Filter/Join with outer PAC aggregate: priv_select_<cmp> or priv_select(hash, list<bool>)
};

// Detect and rewrite categorical query patterns to use counters and mask-based selection.
// This modifies the plan in-place. No-op if no categorical patterns are found.
void RewriteCategoricalQuery(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan,
                             const PacAggregateInfoMap &pac_agg_info = {});

// Shared between rewriter and lambda module
vector<PacBindingInfo> FindCounterBindings(Expression *expr,
                                           const unordered_map<uint64_t, ColumnBinding> &counter_bindings);
bool ReferencesCounterBinding(Expression *expr, const unordered_map<uint64_t, ColumnBinding> &counter_bindings);

// ==========================================================================================
// Rewrite-specific inlined helper methods
// ==========================================================================================

// Get struct field name for index (a, b, c, ..., z, aa, ab, ...)
static inline string GetStructFieldName(idx_t index) {
	if (index < 26) {
		return string(1, static_cast<char>('a' + static_cast<unsigned char>(index)));
	}
	// For indices >= 26, use aa, ab, etc.
	return string(1, static_cast<char>('a' + static_cast<unsigned char>(index / 26 - 1))) +
	       string(1, static_cast<char>('a' + static_cast<unsigned char>(index % 26)));
}

// ============================================================================
// Algebraic simplification: try to emit priv_filter_<cmp> instead of lambda
// ============================================================================
// Given a comparison expression with one PAC binding, try to algebraically
// move arithmetic from the list side to the scalar side, then emit a single
// priv_filter_<cmp>(scalar, counters) call instead of list_transform + priv_filter.

// Resolve the priv_{filter,select}[_sass]_<cmp> SQL function name for a comparison.
// Internal helper: callers should use the GetPacFilterCmpName / GetPacSelectCmpName wrappers.
enum class PacCmpKind : uint8_t { FILTER = 0, SELECT = 1 };

static inline const char *GetPacCmpFuncName(ExpressionType cmp_type, PacCmpKind kind, const string &privacy_mode) {
	int suf;
	switch (cmp_type) {
	case ExpressionType::COMPARE_GREATERTHAN:
		suf = 0;
		break;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		suf = 1;
		break;
	case ExpressionType::COMPARE_LESSTHAN:
		suf = 2;
		break;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		suf = 3;
		break;
	case ExpressionType::COMPARE_EQUAL:
		suf = 4;
		break;
	case ExpressionType::COMPARE_NOTEQUAL:
		suf = 5;
		break;
	default:
		return nullptr;
	}
	// [kind][sass][suffix]: 2 × 2 × 6 — one stable const char* per registered SQL function.
	static const char *const TABLE[2][2][6] = {
	    {{"priv_filter_gt", "priv_filter_gte", "priv_filter_lt", "priv_filter_lte", "priv_filter_eq",
	      "priv_filter_neq"},
	     {"priv_filter_sass_gt", "priv_filter_sass_gte", "priv_filter_sass_lt", "priv_filter_sass_lte",
	      "priv_filter_sass_eq", "priv_filter_sass_neq"}},
	    {{"priv_select_gt", "priv_select_gte", "priv_select_lt", "priv_select_lte", "priv_select_eq",
	      "priv_select_neq"},
	     {"priv_select_sass_gt", "priv_select_sass_gte", "priv_select_sass_lt", "priv_select_sass_lte",
	      "priv_select_sass_eq", "priv_select_sass_neq"}},
	};
	int sass_idx = privacy_mode == "dp_sass" ? 1 : 0;
	return TABLE[static_cast<int>(kind)][sass_idx][suf];
}

// Thin wrappers preserved for existing call sites in pac_categorical_lambdas.cpp
// and pac_derived_rewriter.cpp.
static inline const char *GetPacFilterCmpName(ExpressionType cmp_type, const string &privacy_mode = "pac") {
	return GetPacCmpFuncName(cmp_type, PacCmpKind::FILTER, privacy_mode);
}
static inline const char *GetPacSelectCmpName(ExpressionType cmp_type, const string &privacy_mode = "pac") {
	return GetPacCmpFuncName(cmp_type, PacCmpKind::SELECT, privacy_mode);
}

// Flip a comparison when swapping sides (PAC was on left, move to right)
static inline ExpressionType FlipComparison(ExpressionType cmp_type) {
	switch (cmp_type) {
	case ExpressionType::COMPARE_GREATERTHAN:
		return ExpressionType::COMPARE_LESSTHAN;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ExpressionType::COMPARE_LESSTHANOREQUALTO;
	case ExpressionType::COMPARE_LESSTHAN:
		return ExpressionType::COMPARE_GREATERTHAN;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return ExpressionType::COMPARE_GREATERTHANOREQUALTO;
	case ExpressionType::COMPARE_EQUAL:
		return ExpressionType::COMPARE_EQUAL;
	case ExpressionType::COMPARE_NOTEQUAL:
		return ExpressionType::COMPARE_NOTEQUAL;
	default:
		return cmp_type;
	}
}

// Try to extract a double value from an expression, looking through casts.
// Returns true and sets out_val if the expression is a constant that can be cast to DOUBLE.
static inline bool TryGetConstantDouble(Expression *expr, double &out_val) {
	while (expr->type == ExpressionType::OPERATOR_CAST) {
		expr = expr->Cast<BoundCastExpression>().child.get();
	}
	if (expr->type != ExpressionType::VALUE_CONSTANT) {
		return false;
	}
	auto &const_expr = expr->Cast<BoundConstantExpression>();
	if (const_expr.value.IsNull()) {
		return false;
	}
	Value double_val;
	string error_msg;
	if (const_expr.value.DefaultTryCastAs(LogicalType::DOUBLE, double_val, &error_msg)) {
		out_val = double_val.GetValue<double>();
		return true;
	}
	return false;
}

// Check if an expression is a positive constant (value > 0), looking through casts
static inline bool IsPositiveConstant(Expression *expr) {
	double val;
	return TryGetConstantDouble(expr, val) && val > 0.0;
}

// Given a binary arithmetic op and which child holds the PAC counter, return the inverse op
// to move the scalar operand to the other side of a comparison.
// Commutative ops (+, *) work regardless of pac_child position.
// Non-commutative ops (-, /) only work when counter is on the left (pac_child == 0).
// Sets needs_positive_check when the scalar operand must be > 0 to preserve inequality direction.
static inline const char *GetInverseArithmeticOp(const string &fname, int pac_child, bool &needs_positive_check) {
	needs_positive_check = false;
	if (fname == "+") {
		return "-";
	} else if (fname == "-") {
		return pac_child == 0 ? "+" : nullptr;
	} else if (fname == "*") {
		needs_positive_check = true;
		return "/";
	} else if (fname == "/" && pac_child == 0) {
		needs_positive_check = true;
		return "*";
	}
	return nullptr;
}

} // namespace duckdb

#endif // PAC_CATEGORICAL_REWRITER_HPP
