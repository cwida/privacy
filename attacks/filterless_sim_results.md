# Filterless / CROWD universal bounds — simulation results

Simulation of `filterless-crowd-dp-proposal.pdf` (10 June 2026) outside the extension,
via `attacks/filterless_sim.py`. Purpose: decide whether the proposal is worth
implementing in C++ before writing any.

TPC-H, PU = `customer` (`o_custkey`), measure = `SUM(l_extendedprice)` over
`lineitem ⋈ orders`, query filter `l_shipmode in ('AIR','REG AIR')`, ε = 1, one
aggregate (no budget split). Metric = median over released groups of the mean
relative error `|released − true| / true`, split into a deterministic part
(clipping / truncation bias) and the Laplace part.

Mechanisms:

| name | bound | sensitivity | notes |
|---|---|---|---|
| `google` | scalar, from **filtered** per-(u,g) partials | `C_u · B_G` | random truncation to `C_u` groups per user |
| `google_nocu` | same | `max_u k_u · B_G` | no truncation — isolates truncation's cost |
| `filterless` | per-group `B_g`, from **full domain** | `Δ̄₁ = max_u Σ_g min(a(u,g), B_g)` | the note, eq. (16) + (25) |
| `fl_l1crowd` | same `B_g` | `D_s` = CROWD-supported per-user norm | **not in the note** — see finding 2 |

`google` is a stand-in: Wilson's `APPROX_BOUNDS` is modelled by the same exponential-bin
CROWD rule applied to the filtered partials, pooled to one scalar. This isolates the two
real differences (filtered vs full-domain, per-group vs global) from any difference in how
a bound gets picked. It also means the baseline is *flattered*: real `APPROX_BOUNDS` spends
privacy budget and this one doesn't. Truncation is scored by its exact expectation
(`P[keep g] = min(1, C_u/k_u)`) rather than sampled. `C_u` defaults to the p99 fan-out,
which is a generous setting for Wilson.

---

## Finding 1 — Δ̄₁ does not grow with GROUP BY width

sf1, `f = 4`, benign data:

| group-by | groups | Δ̄₁ | `C_u · B_G` |
|---|---|---|---|
| `o_orderpriority` | 5 | 7,154,829 | 5,242,880 |
| year-quarter | 27 | 7,154,829 | 16,777,216 |
| year-month | 80 | 7,154,829 | 19,922,944 |
| year-month × priority | 395 | 7,154,829 | 22,020,096 |

Δ̄₁ is **identical** at every width. When `B_g` does not bind,
`Δ̄₁ = max_u Σ_g a(u,g) = max_u (that user's total footprint)` — and refining the grouping
only splits a user's footprint across more groups without changing its sum. Wilson's
`C_u · B_G` grows with `C_u`, and shrinking `C_u` to compensate buys bias instead.

This answers the objection raised before reading the note (that an uncapped Δ̄₁ would lose
on wide group-bys). It does not — the exact norm is *self-limiting*, and the wider the
group-by, the more the note's approach wins.

## Finding 2 — but Δ̄₁ is not CROWD-protected, and one user can break it

Eq. (25) is a bare `max_u`. `B_g` is crowd-protected; the norm built from it is not. A
single user who is both **fat** (above `B_g`) and **wide** (present in every group) drives
`Δ̄₁ → k · B_g` and inflates the noise for everybody. Simulated by taking the largest
customer, giving it a row in every group, and scaling it ×1000:

| group-by | Δ̄₁ benign | Δ̄₁ attacked | `D_s` (either) |
|---|---|---|---|
| `o_orderpriority` | 6,931,895 | 10,485,760 | 8,388,608 |
| year-quarter | 7,154,829 | 27,787,264 | 8,388,608 |
| year-month | 7,154,829 | 41,680,896 | 8,388,608 |
| year-month × priority | 7,154,829 | 207,093,760 | 8,388,608 |

Δ̄₁ inflates by up to 29×, and the inflation is proportional to the group count. Wilson
survives this case **precisely because `C_u` truncation caps the norm** — the mechanism the
note proposes to remove. So Remark 2.1 is right about the benign case and wrong about the
adversarial one, and `C_u` is not only an approximation crutch.

It is also a leak, not just a utility problem: the noise scale tracks one identifiable
user's total footprint exactly.

**The repair is the note's own primitive, applied one level up.** Instead of `max_u`, take
the top of the highest exponential bin of the per-user norms `n_u = Σ_g min(a(u,g), B_g)`
whose distinct-PU support is ≥ s — i.e. run `priv_max` on the norm distribution — and
ℓ1-clip each user's contribution vector to that `D_s`. Sensitivity is then exactly `D_s`,
deterministically, with no random truncation error. `D_s = 8,388,608` in **every** row
above, benign and attacked: it is invariant by construction, because moving it requires s
users to be simultaneously fat and wide.

## Finding 3 — with the repair, filterless wins essentially everywhere

sf1, `f = 2`, ε = 1, s = 350, median relative error:

| group-by | groups | | `filterless` | `fl_l1crowd` | `google` |
|---|---|---|---|---|---|
| `o_orderpriority` | 5 | benign | 0.1% | 0.1% | **0.0%** |
| | | attacked | 2.8% | 2.8% | 2.8% |
| year-quarter | 27 | benign | 0.3% | **0.3%** | 0.4% |
| | | attacked | 2.0% | **1.8%** | 1.9% |
| year-month | 80 | benign | **0.9%** | 1.0% | 1.2% |
| | | attacked | 6.2% | **4.2%** | 4.4% |
| year-month × priority | 395 | benign | **4.3%** | 5.0% | 6.6% |
| | | attacked | 104.4% | **16.8%** | 17.1% |

`fl_l1crowd` is at or ahead of `google` in every configuration and never blows up.
Wilson wins only on the narrowest group-by, where `C_u = 5` equals the maximum fan-out so
truncation costs nothing and `C_u · B_G < D_s`.

sf10, year-month, `f = 2`: errors are uniformly small (0.1–0.2% benign, 0.4–1.1% attacked)
and the ordering is unchanged — Δ̄₁ 7,244,710 → 83,361,792 under attack, `D_s` 8,388,608
throughout. The mechanism differences matter most at smaller data, tighter ε, and narrower
groups.

## Finding 4 — use `f = 2`, not `f = 4`

The bin factor is the price of the CROWD rule: `D_s` overshoots the true norm by up to `f`.
At `f = 4`, `D_s = 16.8M` against Δ̄₁ = 7.2M (134% overshoot) and robustness costs 2× the
noise. At `f = 2`, `D_s = 8.4M` (17% overshoot) and it costs almost nothing —
year-month benign goes 0.9% (`filterless`) vs 1.0% (`fl_l1crowd`). `f = 2` doubles the
number of bins, which is free.

## Finding 5 — bound stability confirmed, but the leak is coarse

Claim 2 (Rem. 3.1). Narrowing the filter around the highest-`c_acctbal` customer, s = 20,
`f = 2`, sf1:

| filter | PUs left | `B_G` (filtered) | `B_g` (filterless) |
|---|---|---|---|
| `c_acctbal >= -1000` | 99,996 | 1,048,576 | 1,048,576 |
| `c_acctbal >= 5000` | 45,250 | 1,048,576 | 1,048,576 |
| `c_acctbal >= 9800` | 1,822 | 1,048,576 | 1,048,576 |
| `c_acctbal >= 9990` | 100 | 524,288 | 1,048,576 |
| `c_acctbal >= 9998` | 19 | 0 (collapsed) | 1,048,576 |

The filterless bound is fixed, as claimed. The filtered bound does move — but only by one
bin level before collapsing entirely once the surviving population drops below s. So the
instability is real and the analyst does learn something from it, but the channel is one
bin wide, which is consistent with the note's own Rem. 10.1 argument that exponential
binning makes the surface coarse. This is the weakest of the empirical results: it
supports the design rather than proving Wilson unsafe.

---

## Caveats

- SUM only, nonnegative, single aggregate. No signed two-histogram case (§9.2), no
  MIN/MAX, no ε split across aggregates, no AVG ratio.
- `google` is a stand-in for `APPROX_BOUNDS`, and a flattering one (spends no budget).
- The attacked configuration is synthetic (one customer ×1000, present in every group). It
  is a legitimate adversary under the note's own threat model, and skewed real data would
  produce a weaker version of the same effect, but the magnitudes here are not a
  measurement of natural skew.
- Nothing here tests the metadata-freshness question (Rem. 10.1) or the cost of the
  full-domain pass. `--skew-spread` inflates fan-out without measuring what materialising
  `a(u,g)` costs.
- Δ̄₁ and `D_s` are still computed from the data and never noised. Finding 2 fixes
  *robustness*, not the closed-world assumption of Assumption 8.1.

## Reproduce

```bash
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --groupby month -f 2
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --groupby month_priority -f 2 \
        --skew 1000 --skew-spread
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 -f 2 -s 20 --attack
python3 attacks/filterless_sim.py --db tpch_sass_sf10.db --groupby month -f 2
```
