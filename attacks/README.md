# DP contribution-bounding experiments

Harnesses behind `docs/dp/bound_derivation.md` (full chronological record, including superseded
numbers) and `docs/dp/findings.pdf` (the cleaned-up write-up).

All scripts assume `duckdb.connect(config={'threads': 2})`, attach every `.db` READ_ONLY, and never
write to one. Two of them have OOM-killed this machine; see **Resource limits** below.

## Where to start

| file | what it answers |
|---|---|
| `broad_benchmark.py` | headline utility, 14 queries x 3 datasets, vs Google DP as published |
| `fineness_sweep.py` | the shared harness: `Cells`, `approx_bounds`, `google_values`, `tau` |
| `ku_families.py` | which `k_u` distribution shape decides the winner (the scope condition) |
| `em_cu_vs_manual.py` | automatic `C_u` selection vs what an analyst would supply |
| `mia_full_stack.py` | membership-inference attacks; demonstrates the delta finding |

## By topic

**l1 norm clip** — `fineness_sweep.py`, `geometry_matched.py`, `oracle_bounds.py`,
`where_google_wins.py`, `ku_families.py`, `clip_geometry.py`, `dual_clip.py`, `attack_benchmark.py`

**Frozen partition set** — `frozen_partition.py`, `frozen_vs_google.py`, `full_stack.py`,
`frozen_vs_perquery_cu.py`

**Automatic `C_u` (exponential mechanism)** — `em_cu.py`, `em_cu_vs_manual.py`, `cu_automatic.py`,
`em_cu_sass.py`, `em_cu_sass_eps.py`, `joint_quantiles.py`

**All-or-Frozen** — `all_or_frozen_v2.py` (current), `aof_threshold_check.py`,
`aof_and_vs_statistic.py`, `aof_at_cu1.py`, `all_or_frozen_privacy.py`

**Privacy validation** — `mia_full_stack.py`, `mia_frozen_repeated.py`, `multi_agg_adaptive.py`,
`adaptive_target_selection.py`, `joins_and_arrivals.py`

**Partition selection / votes** — `vote_gaussian.py`, `vote_geometry.py`, `dataset_profile.py`,
`group_support_profile.py`

## Superseded — do not take numbers from these

- `all_or_frozen_full.py` — fan-out join inflates support ~11x; dedicated-count variant omits
  truncation. Superseded by `all_or_frozen_v2.py`.
- `taubinding_headtohead.py` — vote truncation indexes sorted arrays into unsorted ones, a 9x
  epsilon violation. Superseded by `fineness_sweep.py`.
- `automatic_cu_future.py` — written by a background agent, never run or verified by me.

## Resource limits

`day|region` (10.6M cells) and a 60-point oracle sweep have each OOM-killed this laptop. Before
running anything, count the number of full passes over the cell arrays it implies — that product,
not the per-pass cost, is what kills the machine. `geometry_matched.py` and `full_stack.py` enforce
`--max-cells`; `fineness_sweep.py` does not.
