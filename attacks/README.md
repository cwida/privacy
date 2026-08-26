# DP contribution-bounding experiments

Harnesses behind `docs/dp/bound_derivation.md` (full chronological record, including superseded
numbers), `docs/dp/findings.pdf` (the cleaned-up write-up), and
`docs/dp/filterless_value_bounds.md` (DP filterless bounds versus Google implicit DP).

All scripts assume `duckdb.connect(config={'threads': 2})`, attach every `.db` READ_ONLY, and never
write to one. Two of them have OOM-killed this machine; see **Resource limits** below.

## Where to start

| file | what it answers |
|---|---|
| `broad_benchmark.py` | reusable DP filterless bounds vs Google DP, including stable filtered cohorts |
| `filterless_dp_attack.py` | billionaire filter-membership attack: filterless DP, Google implicit DP, unsafe max |
| `filterless_one_lane_prototype.py` | simulated one-AS-lane metadata utility and worst-lane filter attack |
| `filterless_complete_pu_poc.py` | complete-PU two-channel sampling with row filters, noisy bins, and TPC-H |
| `filterless_sampling_benchmark.py` | sampled nonqualifying PU/group execution and HLL support error |
| `filterless_three_way_benchmark.py` | current fixed-sample filterless utility and relational timing vs full filterless and Google-style DP |
| `filterless_encoded_attack.py` | filter-change and database-update knife-edge attacks on the fixed-sample histogram |
| `fineness_sweep.py` | the shared harness: `Cells`, `approx_bounds`, `google_values`, `tau` |
| `ku_families.py` | which `k_u` distribution shape decides the winner (the scope condition) |
| `em_cu_vs_manual.py` | automatic `C_u` selection vs what an analyst would supply |
| `mia_full_stack.py` | membership-inference attacks; demonstrates the delta finding |

## By topic

**Filterless DP bounds** — `filterless_dp_attack.py`; `broad_benchmark.py --datasets stable
--trials 30` runs the low-cardinality filtered-cohort crossover workload.

**One-lane filterless prototype** — `filterless_one_lane_prototype.py` simulates the existing AS
hash-lane assignment, checks that the mean of all 64 inverse-probability lane estimates exactly
recovers the full histogram, compares the safe hybrid with query-local exponential-bin selection,
and runs the worst-selected-lane membership attack. It does not yet exercise a compiler rewrite.

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
