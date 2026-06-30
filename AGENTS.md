# AGENTS.md

This file provides guidance to Codex when working in this repository.

## Working With The User

When you are stuck, either unable to fix a bug after 2-3 attempts or tempted to work around the actual problem by redefining the objective, stop and ask the user for directions. Explain the specific problem clearly, for example: `priv_clip_sum(UBIGINT, DOUBLE) has no matching overload; should I add a DOUBLE overload or cast?`

Always test changes with real queries before declaring success, not just unit tests. Unit tests with wide boolean thresholds can pass even when the code is fundamentally broken.

Never execute git commands that could lose code. Ask the user before destructive git operations.

## Development Rules

- New features must have tests. Ask the user whether to create a new test file or extend an existing one in `test/sql/`.
- Never remove a failing test to "fix" a failure. Fix the underlying bug.
- Never weaken tests to match current behavior. If a test exposes a bug, keep the test and fix the bug.
- Before implementing anything, search the existing codebase for similar patterns or solutions. Reuse local helpers and patterns before inventing new ones.
- Factor shared logic into helpers rather than duplicating code. Check `src/include/utils/` and existing helpers in the file being edited.
- Never edit the `duckdb/` submodule. DuckDB source is read-only. PAC logic lives in `src/` and `test/`.
- Keep the paper in mind. The PAC mechanism is described in `SIMD-PAC-DB: Pretty Performant PAC Privacy` (`https://arxiv.org/abs/2603.15023`).
- Add `PRIVACY_DEBUG_PRINT` statements at major code flow points when changing compilation or rewrite logic. Use `src/include/privacy_debug.hpp`; it compiles out when `PRIVACY_DEBUG` is 0.

## What This Extension Does

`privacy` is a DuckDB extension that automatically privatizes SQL aggregate queries. It supports two privacy mechanisms selected with `SET privacy_mode`:

- `pac` (default): PAC Privacy, empirical membership-inference resistance. Maintains 64 parallel counters per aggregate, one per world bit, and adds noise calibrated to query variance across subsamples. This is not differential privacy.
- `dp_elastic`: Elastic sensitivity DP. Derives per-query sensitivity from the join tree's max frequencies and injects Laplace noise calibrated to `sensitivity / epsilon`. Only `dp_elastic` provides a formal epsilon-DP guarantee.

Both modes share the same DDL: `PRIVACY_KEY`, `PRIVACY_LINK`, `PROTECTED`, and `SET PU`.

## Build And Test

```bash
GEN=ninja make
make test
build/release/test/unittest "test/sql/priv_sum.test"
build/release/extension/privacy/pac_test_runner
```

Build outputs go to `build/release/`.

## Compilation Pipeline

The optimizer hook runs in `pre_optimize_function`, before DuckDB's built-in optimizers.

Implications:

- Call `plan->ResolveOperatorTypes()` before accessing `LogicalProjection::types`.
- WHERE filters are still separate `FILTER` nodes.
- LIMIT is a separate root node.
- DuckDB optimizers run automatically after the transformed plan is returned.

### PAC Pipeline

1. Compatibility check: `privacy_compatibility_check.cpp` decides if the query needs PAC rewrite and rejects unsupported patterns.
2. FK join injection: `pac_bitslice_add_fkjoins.cpp` adds missing joins to reach PU tables.
3. Aggregate transformation: `pac_bitslice_compiler.cpp` and `pac_expression_builder.cpp` replace aggregates with PAC aggregate functions and insert `priv_hash` projections above scans.
4. Categorical rewrite: `pac_categorical_rewriter.cpp` converts PAC aggregates in filters or comparisons to counter-list expressions with `priv_filter` and `priv_select` terminals.
5. AVG decomposition: `pac_avg_rewriter.cpp` rewrites `priv_noised_avg` into `priv_noised_div(priv_sum, priv_count)`.
6. Clip rewrite: `pac_expression_builder.cpp:RewriteClipAggregates` inserts lower per-PU pre-aggregation with clipping when `priv_clip_support` is set.

### DP-Elastic Pipeline

1. Compatibility check: shared structural checks; PU columns can only appear inside aggregates.
2. FK chain extraction: `dp_elastic_compiler.cpp:ExtractFKChain` validates a linear FK path to the PU.
3. AVG rewrite: `RewriteAvgAggregates` replaces `AVG(x)` with `SUM(x)` plus `COUNT(*)`, preserving filters.
4. Clipping: `ClipSumInputs` wraps SUM arguments in `greatest(least(v, C), -C)`, where `C = dp_sum_bound`.
5. Sensitivity computation: `ComputeMfK` and `ComputeElasticSensitivity`.
6. Budget split: with `k` user-visible aggregates each gets `epsilon / k`; AVG components split further.
7. Noise injection: `WrapAggregateWithLaplace` adds a projection above the aggregate.
8. Tau thresholding: `ApplyGroupSuppression` drops low-SNR groups when `privacy_min_group_count` is set.
9. AVG ratio projection: `WrapAvgRatioProjection` computes noised SUM / noised COUNT and remaps upstream references.

## Aggregate Naming

| Name pattern | Returns | Purpose |
|---|---|---|
| `priv_sum/count/min/max` | `LIST<FLOAT>` | 64 counters for categorical and clip rewrites |
| `priv_noised_sum/count/min/max` | scalar | fused counters plus noise |
| `priv_clip_sum/count/min/max` | `LIST<FLOAT>` | counters with per-level support clipping |
| `priv_noised_clip_sum/count/min/max` | scalar | fused clip plus noise |

`pac_noised_*` is the fused version of `priv_noised(pac_*())`. The unfused form is used when expressions operate on counters.

## Key Architectural Rules

- `priv_hash` is always computed in a projection above the scan, never inside an aggregate.
- Precomputed bindings become stale after `RewriteBottomUp`; recompute bindings at point of use.
- Use `binder.GenerateTableIndex()` for new table indices.
- Always call `ResolveOperatorTypes()` after creating or modifying a `LogicalAggregate`.
- `priv_noised_sum` on DECIMAL uses `BindDecimalPacSum` to dispatch by physical type and set return type to `DECIMAL(38, scale)`. New sum variants need the same pattern.

## Key Source Files

- `src/core/privacy_optimizer.cpp`: optimizer hook entry point.
- `src/compiler/pac_bitslice_compiler.cpp`: PAC compilation orchestrator.
- `src/compiler/dp_elastic_compiler.cpp`: DP-elastic compilation.
- `src/query_processing/pac_expression_builder.cpp`: aggregate modification, clip rewrite, expression binding.
- `src/query_processing/pac_plan_traversal.cpp`: plan traversal utilities.
- `src/include/aggregates/pac_aggregate.hpp`: `PacBindData`, noise calibration, p-tracking.
- `src/categorical/pac_categorical_rewriter.cpp`: categorical query transformation.
- `src/metadata/privacy_compatibility_check.cpp`: shared compatibility checks.

## Debugging

Set `#define PRIVACY_DEBUG 1` in `src/include/privacy_debug.hpp` for stderr trace output. Use `EXPLAIN` to inspect transformed plans.

## DDL Examples

```sql
ALTER TABLE customer ADD PRIVACY_KEY (c_custkey);
ALTER TABLE customer SET PU;
ALTER TABLE orders ADD PRIVACY_LINK (o_custkey) REFERENCES customer (c_custkey);

SET privacy_mode = 'pac';
SET pac_mi = 0;
SET privacy_seed = 42;
SET priv_clip_support = 40;
SET privacy_min_group_count = 4;

SET privacy_mode = 'dp_elastic';
SET dp_epsilon = 1.0;
SET dp_delta = 1e-6;
SET dp_sum_bound = 1000;
SET privacy_min_group_count = 4;
```

## Code Style

Follow DuckDB's clang-tidy and clang-format configuration:

- Classes/enums: `CamelCase`
- Functions: `CamelCase`
- Variables, parameters, members: `lower_case`
- Constants/static/constexpr: `UPPER_CASE`
- Macros: `UPPER_CASE`
- Typedefs: `lower_case_t`
- Tabs for indentation, width 4
- Column limit 120
- Braces on the same line as the statement
- Pointers right-aligned: `int *ptr`
- No short functions on one line

Run `make format-fix` to auto-format.

## Attack Evaluation

Attack scripts live in `attacks/`. Results are documented in `attacks/clip_attack_results.md`.

```bash
bash attacks/clip_attack_test.sh 2>/dev/null
bash attacks/clip_multirow_test.sh 2>/dev/null
bash attacks/clip_hardzero_stress.sh 2>/dev/null
```
