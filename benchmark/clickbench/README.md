# ClickBench privacy benchmark

`pac_clickhouse_benchmark` runs the ClickHouse `hits` (ClickBench) queries through the
`privacy` extension and measures runtime (and, where applicable, utility) for several
privacy mechanisms against a non-private baseline.

## Running

```bash
# build first: GEN=ninja make
build/release/extension/privacy/pac_clickhouse_benchmark [options]
```

Options:

| Flag | Meaning |
|---|---|
| `--micro` | Use a smaller dataset (`clickbench_micro.db`) for quick testing |
| `--db <path>` | DuckDB database file |
| `--queries <dir>` | Directory with `create.sql`, `load.sql`, `queries.sql`, `setup.sql` |
| `--out <csv>` | Output CSV path (auto-named if omitted) |
| `--modes <csv>` | Which mechanisms to measure: subset of `pac,dp_standard,dp_elastic,dp_sass` (default: all four) |
| `--run-naive` | Also run the explicit sample-table-join "naive" variants for the sampling mechanisms in `--modes` |

The non-private **`baseline`** always runs regardless of `--modes`.

Each selected DP/PAC mechanism runs **once per query** at a single operating point —
`dp_epsilon = 1.0` and the inferred "perfect" sensitivity bound (no epsilon/sensitivity sweep).

## Modes in the output `mode` column

- `baseline` — plain DuckDB, no privacy.
- `PAC`, `dp_standard`, `dp_elastic`, `dp_sass` — the four mechanisms (vectorized rewrite path).
- `naive_pac`, `naive_dp` — explicit-SQL sample-and-aggregate baselines (only with `--run-naive`).

## Naive queries

The naive variants live in:

- `clickbench_naive_pac_queries/qNN.sql`
- `clickbench_naive_dp_queries/qNN.sql`

They exist only for the **privatizable aggregate** queries (`SUM`/`COUNT`/`AVG` over `hits`
grouping on non-protected, non-`UserID` columns). Queries with no naive form — `COUNT(DISTINCT
UserID)`, `MIN`/`MAX`, row dumps (`ORDER BY … LIMIT`), and `GROUP BY UserID` — have no file and
are skipped automatically by the runner.

Both families share one skeleton and differ only in the terminal UDF:

- **PAC naive**: 128 random sub-samples per privacy unit (`WHERE random() < 0.5`), per-sub-sample
  `SUM`/`COUNT`/`AVG`, then `pac_aggregate(answers, counts, mi, k)` → mean-of-sub-samples + PAC noise.
- **DP naive**: 64 lanes, each privacy unit assigned to ~8 of them via an **approximate** SQL hash
  (`hash(UserID, lane_id) % 64 < 8`), per-lane answers rescaled by `64/8`, then
  `dp_aggregate(answers, counts, eps, delta, lanes, lower, upper)` → clipped median of the 64
  lane answers + smooth-sensitivity Laplace noise (wraps `dp_smooth_median_noise`; the `counts`
  argument is present only for signature symmetry with `pac_aggregate` and is unused by the
  median path).

> **The DP naive variant is a runtime / structural baseline, not a utility-faithful one.** Its
> lane assignment is approximate (the real `DpSampleHash` 6-bit-chunk bitmask is C++-only and
> cannot be reproduced in plain SQL), and its domain bounds are hand-set generous constants
> rather than the per-query inferred bounds used by the real `dp_sass` rewrite. The runner records
> only **timing + success** for naive rows; their numeric values are expected to be noise-dominated
> and should not be compared to the vectorized path for accuracy.
