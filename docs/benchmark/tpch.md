# TPC-H Benchmarks

TPC-H supplies the paper's end-to-end AS performance and DP utility workloads. The AS and DP experiments use separate
standalone executables.

## AS Performance

The main benchmark compares DuckDB, SIMD-AS at `m=64`, a naive repeated-query estimate, a simple-hash control,
disjoint `GROUP BY sid`, expanded overlapping-world execution, and hardcoded VerdictDB variational rewrites.

```bash
cmake --build build/release --target as_tpch_benchmark

build/release/extension/privacy/as_tpch_benchmark \
  --config benchmark/configs/utility/as_tpch_sf30.json --dry-run

build/release/extension/privacy/as_tpch_benchmark \
  --config benchmark/configs/utility/as_tpch_sf30.json
```

The runner generates the configured TPC-H database when it is absent. Handwritten query variants live in:

- `benchmark/tpch/tpch_as_queries/`
- `benchmark/tpch/tpch_as_simple_hash_queries/`
- `benchmark/tpch/tpch_naive_as_queries/`
- `benchmark/tpch/tpch_sid_queries/` (shared by the disjoint and expanded representations)
- `benchmark/tpch/tpch_matched_simd_queries/` (ten validated one-hot SIMD counterparts)
- `benchmark/tpch/tpch_verdictdb_queries/`

The VerdictDB POC follows the released `sigmod18` implementation: it creates an untimed, reproducible 1% sample of
`CUSTOMER`, stores one of VerdictDB's default 100 partition identifiers per sampled customer, propagates that
identifier through joins, computes per-partition estimates, and combines them into the implementation's weighted
estimate and normal-approximation 95% error bound. Only query shapes represented in the VerdictDB directory are run;
each query is labelled by the scope of its implementation. The runner also writes a sibling `*_verdictdb_utility.csv` containing each
estimate, exact TPC-H answer, absolute and relative error, and whether the reported interval covers the exact answer.
Timing uses trial zero. Utility uses 20 independently salted customer samples by default, configurable with
`--verdictdb-utility-trials=N`; the SF30 config requests 100. Each output row includes its trial, missing-group/value
status, null or zero-width interval status, and interval containment. Degenerate COUNT intervals inherited from the
artifact formula are diagnostics, not evidence of calibrated uncertainty. Customer clustering and manual nested
rewrites require empirical validation; no nominal 95% coverage guarantee is asserted for this POC.

Sampling uses `md5_number_lower` on explicit `verdict_sample_<trial>:<customer>` and
`verdict_partition_<trial>:<customer>` strings, followed by modulo 100. This supplies deterministic,
domain-separated pseudorandom assignments without DuckDB's multi-argument hash/XOR correlation. Setup logs report
actual occupied partitions. The earlier `hash(customer, salt)` sampler restricted the 1% sample to 25 partition IDs;
its VerdictDB timing/utility results are invalid and must not be reused. Ordinary SID/SIMD membership is unaffected.

The audit expands comparison coverage to all 19 queries in the AS benchmark:

| CSV mode | Queries | Scope |
| --- | --- | --- |
| `VerdictDB-artifact-formula` | 1, 3, 5, 6, 7, 8, 9, 10, 12, 14, 19 | Released aggregate combination/error formulas on the fixed CUSTOMER sample; no automatic planner. |
| `VerdictDB-customer-adapted` | 4, 13, 17, 18, 21, 22 | Manual rewrites preserving customer ownership, exact entity-local predicates, or partition-specific nested aggregates. Not captured native VerdictDB SQL. |
| `VerdictDB-exact-fallback` | 15, 20 | Exact DuckDB queries, explicitly recorded with `m=0`; no sampling-utility trials. These are POC fallback choices, not assertions about every native VerdictDB plan. |

Q3/Q10/Q18 select top-k outputs. Missing exact top-k groups and returned groups outside the exact top-k are retained
as `missing_estimate` and `missing_exact`; numeric errors alone do not measure ranking utility. Their customer/order
groups may occur in only one partition, making estimated intervals NULL. Q13 preserves exact per-customer order
counts, including zero-order customers, before extrapolating the outer histogram. Q17 and Q22 compute their
thresholds separately within each sampled partition. Q15 involves an extremum of aggregate results; Q20 returns
a supplier set rather than an outer numeric estimate, so both use explicit exact fallbacks pending a validated rewrite.

The controlled representation comparison is `Disjoint-SID` versus `Matched-SIMD64`: both use the full input and
`hash(customer_key) % 64` memberships. Matched SIMD divides the counter interface's 2x rescaling back out and returns
per-world rows. Before timing, the runner checks result equivalence with tolerance `1e-3 + 1e-4 * max(abs(a),abs(b))`
for FLOAT outputs. The 1%/100-partition AQP runtime is a different workload and must not be reported as a direct SIMD
speedup. The SID/expanded Q17 inner average is world-specific; results produced before the September 17 correction
are invalid for its semantic/performance comparison.

The sample choice is intentionally fixed to `CUSTOMER` so VerdictDB and SIMD-ASA use the same statistical unit. This
evaluates VerdictDB's scalar-SID rewrite with one sampled relation and exact joined relations; it does not reproduce
VerdictDB's automatic sample planner or its SID reassignment for joins between two independently sampled relations.

### Revision note: interpreting VerdictDB utility

The fixed 1% CUSTOMER experiment is not a measurement of native VerdictDB's utility policy. The
[VerdictDB paper](https://arxiv.org/pdf/1804.00770) selects among available samples under an I/O budget and can
fall back to exact execution; an optional accuracy requirement also triggers exact execution when estimated errors
are too large (§§2.3–2.4). Its evaluation uses 1% uniform/universe samples but allows up to an 80% stratified-sample
budget, and executes TPC-H Q3, Q8, and Q15 exactly (§§6.1–6.2). Appendix B.1 reports approximate-answer relative
errors of 0.03%–2.57%; these are not results for a fixed 1% CUSTOMER design.

Consequently, missing groups, unstable top-k answers, or poorly calibrated intervals in this POC must not be
attributed to the complete VerdictDB system. A performance comparison should hold sampled worlds and estimators
fixed and verify equivalent answers. ASA does not inherently improve statistical accuracy; improved accuracy by
using larger samples or more replicates within the same time budget would require a separate experiment.

R5.O1 requests related-work positioning, not a VerdictDB benchmark. Retain this investigation as optional material
for the revised paper, not a promised revision experiment; R5.O7 instead requests the repeated-DuckDB baseline.

The paper's formal variational-subsampling construction differs from the released `sigmod18` artifact. It uses
`n_s=sqrt(n)` by default and permits `sid=0` for sample rows assigned to no variational subsample, whereas the artifact
assigns every offline-sample row to one stored partition. This POC follows the executable artifact and keeps the
discrepancy explicit.

The variable-`m` experiment uses the same runner and database:

```bash
build/release/extension/privacy/as_tpch_benchmark \
  --config benchmark/configs/utility/as_tpch_sf30_m_sweep.json
```

Each query and mode receives one untimed warmup immediately before its measured executions.

## DP Utility and Runtime

`dp_benchmark_runner` evaluates DuckDB, bounded DP, elastic DP, and SAA over Q01, Q05, Q06, Q14, and Q19.

```bash
cmake --build build/release --target dp_benchmark_runner

build/release/extension/privacy/dp_benchmark_runner \
  --config benchmark/configs/utility/dp_tpch_sf30_utility.json --dry-run

build/release/extension/privacy/dp_benchmark_runner \
  --config benchmark/configs/utility/dp_tpch_sf30_utility.json
```

The customer is the privacy unit. `lineitem` reaches it through `orders`. The runner first records private-query
runtime, then computes exact references and diagnostics in an untimed pass. Timing rows are checkpointed after each
dataset entry.

The Q01 AVG configurations isolate output columns and grouping choices:

```bash
build/release/extension/privacy/dp_benchmark_runner \
  --config benchmark/configs/utility/dp_tpch_sf30_q01_avg_columns.json

build/release/extension/privacy/dp_benchmark_runner \
  --config benchmark/configs/utility/dp_tpch_sf30_q01_avg_groups.json
```

## Plot Results

```bash
Rscript --vanilla benchmark/tpch/plot_tpch_results.R \
  benchmark/results/utility/as_tpch_sf30.csv benchmark/results/figures

Rscript --vanilla benchmark/tpch/plot_tpch_as_m_sweep.R \
  benchmark/results/utility/as_tpch_sf30.csv \
  benchmark/results/utility/as_tpch_sf30_m_sweep.csv \
  benchmark/results/figures
```

See [Utility evaluation](utility.md) for bound and metric definitions. See [repro.md](../../repro.md) for every paper
config and plotting command.
