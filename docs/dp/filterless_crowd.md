# Filterless CROWD DP — revised design note

Revision of `filterless-crowd-dp-proposal.pdf` (10 June 2026, "Filterless Black-Box Private
SQL with Universal Bound Calculation"), incorporating four changes that came out of
simulating it. Evidence for every claim here is in
[`attacks/filterless_sim_results.md`](../../attacks/filterless_sim_results.md), reproducible
with `attacks/filterless_sim.py`; the mechanism is also written out by hand in
`attacks/filterless_example.sql`.

Nothing in this document is implemented in the extension yet.

---

## 1. What the mechanism is

**Contribution bounding, measured filterlessly.** Every DP-for-SQL system needs a bound on
how much one privacy unit can move an answer, because that bound *is* the noise. The options
in use today are: ask the analyst (`dp_sum_bound` — a guess), assume the worst case (FLEX —
poor utility), or estimate per query with privacy budget (Wilson's `APPROX_BOUNDS`).

This mechanism derives the bound once from the **unfiltered** domain, freezes it, and reuses
it for every query in a template family. Predicates become Boolean-valued expressions inside
the measure rather than row-dropping filters, so the bound calculation never sees the
analyst's filter:

```
Σ_{rows satisfying φ} e(row)   ⟿   Σ_{all rows} 1{φ(row)} · e(row)
```

Write `a(u,g)` for PU `u`'s **full-domain** contribution to group `g`, and `t_q(u,g)` for its
contribution under query `q`. Bounds come from `a`, answers from `t_q`. Since
`t_q(u,g) ≤ a(u,g)` always, a bound derived from `a` is valid for every query in the family.

## 2. Metadata pass — once per (measure, grouping), no filters

Three quantities, all decided by the **CROWD rule**: bin values by powers of `f`, and take
the top of the highest bin that enough distinct PUs land in, so an isolated outlier can never
set a bound.

| quantity | what it bounds | support test |
|---|---|---|
| `B_g` | one PU's contribution to one group | distinct PUs in bin, per group |
| `D_s` | one PU's **total** across groups (the ℓ1 norm) | distinct PUs in bin, global |
| `G*` | which group keys may be released | distinct PUs per group |

```sql
-- full-domain per-PU contributions (materialised, IVM-maintained)
CREATE TABLE pu_contrib AS
SELECT pu, g, sum(measure) AS a FROM … GROUP BY 1, 2;

-- per-group bound: highest supported power-of-f bin
CREATE TABLE group_bound AS
WITH bins AS (SELECT g, floor(log(a)/log(f)) AS bin, count(*) AS support
              FROM pu_contrib WHERE a > 0 GROUP BY 1, 2)
SELECT g, pow(f, max(bin) + 1) AS b_g FROM bins
WHERE support + laplace(1/ε_meta) >= τ GROUP BY g;      -- noised, see §5

-- crowd norm: the same rule one level up, on the per-PU totals. one scalar.
CREATE TABLE crowd_norm AS
WITH norms AS (SELECT pu, sum(least(a, b_g)) AS n_u
               FROM pu_contrib JOIN group_bound USING (g) GROUP BY pu),
     bins  AS (SELECT floor(log(n_u)/log(f)) AS bin, count(*) AS support
               FROM norms WHERE n_u > 0 GROUP BY 1)
SELECT pow(f, max(bin) + 1) AS d_s FROM bins
WHERE support + laplace(1/ε_meta) >= τ;                 -- noised, see §5
```

`D_s` depends on the measure and the grouping, **not on the filter** — the same freshness
class as `B_g`. It is one extra rollup over a table the metadata pass already materialises.
Empirically it is also invariant to grouping width (identical across four groupings from 5 to
395 groups), so in practice it may be one scalar per measure rather than per (measure,
grouping) — worth confirming on more measures before relying on it.

## 3. Query pass — the only place the filter appears

```sql
WITH per_pu AS (                      -- pre-aggregate per PU, with the filter
    SELECT pu, g, sum(case when φ then measure else 0 end) AS t FROM … GROUP BY 1, 2
), clipped AS (                       -- clip to the frozen per-group bound
    SELECT pu, g, least(t, b_g) AS c FROM per_pu JOIN group_bound USING (g)
), scaled AS (                        -- ℓ1-clip each PU's vector to the rung bound
    SELECT pu, g, c * least(1.0, D_r / sum(c) OVER (PARTITION BY pu)) AS c FROM clipped
)
SELECT g, sum(c) + laplace(D_r / ε_value) FROM scaled GROUP BY g;
```

`D_r = D_s / f^r` where `r` is the rung chosen for this query (§4). After the ℓ1 clip, one
PU's released vector has ℓ1 norm at most `D_r`, so `D_r` **is** the sensitivity —
deterministically, with no random truncation and no `C_u`.

The ℓ1 scaling is a single window aggregate inside the per-PU pre-aggregation the plan
already computes. Group keys absent from `group_bound` drop out at the join, which doubles as
the released group universe.

## 4. Rung selection — what makes selective queries usable

A frozen bound cannot shrink with the filter; that is its security property and also its
ceiling. Below roughly 7% selectivity the frozen `D_s` leaves the noise sized to the whole
domain while the answers shrink, giving 50–670% relative error where Wilson gets ~1%. No
purely frozen variant escapes this.

But the frozen bounds already sit on an exponential ladder, and nothing requires clipping at
the **top** rung. Per query:

1. Pay `ε_select` for a Laplace-noised histogram of the per-PU **filtered** norms over the
   ladder. Each PU falls in exactly one bin, so ℓ1 sensitivity is 1 and the perturbation is
   tiny.
2. Choose the rung on that noisy histogram — free, it is post-processing:
   ```
   argmin_r [ Σ_b noisy_b · max(0, f^(b+0.5) − D_s/f^r)  +  |G*| · (D_s/f^r) / ε_value ]
   ```
   i.e. trade the mass a rung clips against the noise it saves.
3. Release at that rung with `Laplace(D_r / ε_value)`.

Two details that matter and were both wrong in the first attempt:

- **Only the norm bound moves with the rung.** `B_g` stays frozen. Scaling `B_g` too
  introduces per-group clipping bias that a norm histogram cannot predict, and the selection
  degrades badly.
- **Score counts, not mass.** A clipped-mass score has per-PU sensitivity `D_s`, the same
  order as the score differences, so an exponential mechanism over it picks near-randomly
  (56% error at the tail versus 2.9% for the histogram rule).

This is Wilson's `APPROX_BOUNDS` objective confined to a frozen public ladder and capped by
the frozen rung: the analyst can steer only downward, in factor-`f` steps, inside a range
fixed before the query was written.

## 5. Noised support thresholds

Every `count ≥ s` test in §2 is a **hard threshold**, and a bin or group sitting exactly at
`s` turns a public quantity into a deterministic membership test — 100% attack accuracy, no
noise in the way. Both `D_s` and `G*` have this. The fix is the τ-mechanism the extension
already implements (`privacy_mechanisms.cpp:ComputeWilsonPartitionThreshold`):

```
release the bin/group iff   count + Laplace(1/ε_meta)  ≥  τ = s + m/ε_meta
```

Counts have sensitivity 1, so this is (ε_meta, δ_meta)-DP. **The margin, not the noise, is
what closes it** — at `ε_meta = 1` with `m = 0`, 65–67% of the signal survives; with `m = 3`
it drops to 50.6%, at a cost of `τ = 353` instead of `350`.

## 6. Privacy accounting

| channel | when paid | mechanism |
|---|---|---|
| `ε_meta`, `δ_meta` | **once per session/database** | noised support thresholds for `B_g`, `D_s`, `G*` |
| `ε_select` | per query | sensitivity-1 noisy norm histogram |
| `ε_value` | per query | Laplace at scale `D_r` |

For each fixed rung the release is `ε_value`-DP, and the rung is a function of an
`ε_select`-DP output, so ordinary adaptive composition gives `ε_select + ε_value` per query
on top of the one-time `ε_meta`.

With §5 in place the metadata is itself DP, so **Assumption 8.1 (static database, public
metadata) is no longer needed** — it becomes an efficiency statement rather than a privacy
premise. And the cost has the right shape:

| | bound cost over N queries |
|---|---|
| Wilson et al. | `N · ε_bounds` |
| this mechanism | `ε_meta + N · ε_select`, with `ε_select` ≈ 0.1ε |

The per-query bound cost tends to zero as the workload grows. **That is the pitch** — a
one-time bound cost instead of a per-query one — not "no privacy budget for bounds", which
§4 shows cannot work.

## 7. Changes from the 10 June note

| # | change | why |
|---|---|---|
| 1 | `D_s` = CROWD-supported norm + ℓ1 clip, replacing eq. (25)'s `Δ̄₁ = max_u` | `max_u` is not CROWD-protected. One PU that is fat and wide drives it to `k·B_g` (7M → 207M at 395 groups, 104% error). Worse, under Assumption 8.1 it is a *deterministic* membership test for the norm-defining PU: removing the target moves it 7,154,829 → 6,481,821 while `D_s` does not move at all. This is a correctness fix, not a utility one. |
| 2 | per-query rung selection from a noisy norm histogram | without it the mechanism is unusable below ~7% selectivity; with it, 0.3–2.9% error across a 100%→0.11% ladder, matching or beating Wilson everywhere |
| 3 | noised τ on every support threshold (`D_s`, `G*`) | hard `count ≥ s` gives 100% MIA on an engineered knife-edge; noised τ with a margin gives 50.6% |
| 4 | `f = 2` rather than `f = 4` | the CROWD rule overshoots by up to `f`; at `f = 4` robustness costs 2× the noise, at `f = 2` almost nothing. Doubling the bin count is free. |

Optional: with change 1 in place, `B_g` can be dropped entirely at no measurable utility cost,
reducing the frozen metadata to one sensitivity-1 scalar. Keep it for per-group pollution
control and because the clipped MIN/MAX construction (Rem. 7.1) uses it as the clip cap.

## 8. What holds up unchanged

- **Sensitivity is self-limiting under wider groupings** — identical at 5 and at 395 groups,
  because once `B_g` stops binding the norm is just the fattest PU's total footprint. Wilson's
  `C_u · B_G` grows with `C_u`, and lowering `C_u` buys truncation bias instead. Rem. 2.1
  holds and strengthens as the grouping widens.
- **Single-cell outliers cannot touch either bound** — a 10⁶× spike in one group moves
  nothing. The safe-expression language of §9 is defence in depth, not the only line.
- **Coalitions are held to `s−1`** — 1, 10, 100, 349 colluding fat-and-wide PUs change
  nothing; 350 trips it. The rung is even harder to move and resists up to `s`.
- **Filter steering is capped** — the frozen bound is a ceiling the rung cannot exceed, and
  the rung shows no membership signal down to a two-PU population, nor across a crafted
  20-query filter family.

## 9. Open items

1. **Updates.** Rem. 10.1 argues the update surface is coarse; with §5 it also has a cost —
   re-deriving metadata after writes means re-paying `ε_meta`. The refresh policy needs an
   accounting rule (how stale may metadata be, and does an IVM refresh recharge in full).
2. **Coalition damage is unbounded.** At `s` colluders the bound jumped ×2048 here and scales
   with how fat they make themselves. It is a utility DoS rather than an inference channel,
   but nothing caps it.
3. **Coverage.** Everything simulated is a single nonnegative additive aggregate. Still open:
   the signed two-histogram case (§9.2), MIN/MAX under grouping (Prop. 8.3 is stated for one
   group; the ℓ1 argument across `G*` is missing), AVG as a ratio, and the ε split across `c`
   aggregates.
4. **Metadata surface.** "Universal" means universal across *filters*, not across measures or
   groupings, so the materialisation is per (measure, grouping) template. The note does not
   cost this; the IVM'd max-contribution view is the intended answer.
5. **Nested CASE** yields up to `2^N` bound expressions (§9 iv), which real queries will hit.
6. **Safe-expression language vs "black-box SQL".** These pull against each other. Auditing
   how many TPC-H / ClickBench queries survive §9's restrictions is a concrete number worth
   having.
