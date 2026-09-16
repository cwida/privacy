# Google DP attacks under filter changes

This document audits Google's C++ differential privacy library, aiming to find fallacies and edge cases where an attacker could observe the presence of an individual in a dataset. The experiments use add/remove adjacency: neighboring datasets `D` and `D'` differ in one protected contribution. For a pure epsilon-DP mechanism `M`, every observable event `S` must satisfy both:

```text
Pr[M(D)  in S] <= exp(epsilon) * Pr[M(D') in S]
Pr[M(D') in S] <= exp(epsilon) * Pr[M(D)  in S]
```

Equivalently, with equal membership priors, the optimal attacker must satisfy:

```text
membership accuracy <= exp(epsilon) / (1 + exp(epsilon))
```

At epsilon 0.5, this ceiling is:

```text
exp(0.5) / (1 + exp(0.5)) = 0.622459... = 62.246%
```

Therefore, DP does not require membership accuracy to remain at 50%. Depending on the stated budget, even a single correctly implemented query may formally permit membership accuracy around 70%.

For example, at epsilon 1 the equal-prior pure-DP ceiling is `exp(1) / (1 + exp(1)) = 73.106%`. An observed 70% attack is therefore not, by itself, evidence of a violation at epsilon 1. For repeated queries, the comparison must instead use the
composed transcript budget. For approximate `(epsilon, delta)`-DP, a useful analogous equal-prior bound is `(exp(epsilon) + delta) / (1 + exp(epsilon))`.

## 1. Filter membership-inference attacks

### Experimental setup

The baseline filter experiments used:

- total epsilon 1.0, split as 0.3 for ApproxBounds and 0.7 for the bounded SUM;
- 46 base-2 magnitude bins;
- ordinary contributions between 50 and 150;
- high contributions normally equal to 100,000,000 or the bin boundary 134,217,728;
- balanced membership accuracy with equal priors; and
- neighboring worlds differing in whether one target passes the filter.

`U` denotes the privately selected upper clipping bound. For the positive sums here, the conditional noise scale is approximately `U / epsilon_sum`.

As a concrete example, imagine querying the total wealth of people matching a narrow filter such as one ZIP code. The attacker already knows 500 ordinary contributors and 81 high-value “billionaire” contributors in that cohort. The only uncertainty is whether one target billionaire is also present. Thus the absent dataset contains 81 high contributors, while its add-one neighbor contains 82. The attacker submits the same private
SUM in both possible worlds and observes its noised output. Although the target changes only one contribution, it can move the high-value histogram bin across ApproxBounds' noisy support threshold. This changes how often Google selects a small clipping bound near 256 versus a high bound near 134,217,728, which in turn changes both
the clipped SUM and its noise scale.

### One-query filter attack

The strongest construction keeps 500 ordinary contributors so the query always succeeds. It places 81 known high-value contributors at the ApproxBounds selection threshold and asks whether one target raises that count to 82. ApproxBounds chooses either:

```text
U_low  = 256
U_high = 134,217,728
```

Over 20,000 trials:

| Observable | Target absent: 81 high rows | Target present: 82 high rows |
|---|---:|---:|
| High bound selected | 8,582 | 11,444 |
| High-bound probability | 42.91% | 57.22% |
| Failures | 0 | 0 |

The target changes the probability of selecting a bound that is 524,288 times larger. The corresponding SUM noise scales are approximately 366 and 191,739,611.

An optimal likelihood attack uses the complete mixture distribution: both the probability of each latent bound and the target's clipped contribution conditional on that bound. The selected bound need not be returned explicitly because the final SUM's center and scale reveal the mixture component.

| High contribution | One-query attack | Complete pure-DP ceiling |
|---:|---:|---:|
| 100,000,000 | 63.52% | 73.11% at epsilon 1 |
| 134,217,728 | 65.15% | 73.11% at epsilon 1 |

These attacks are strong but do not falsify the correctly composed epsilon-1 mechanism. Higher single-query budgets gave higher inference while remaining below their corresponding ceilings:

| Total epsilon | Bounds/SUM split | Target transition | Attack | Pure-DP ceiling |
|---:|---:|---:|---:|---:|
| 1 | 0.3 / 0.7 | 81 versus 82 | 64.81% | 73.11% |
| 2 | 0.6 / 1.4 | 40 versus 41 | 75.42% | 88.08% |
| 3 | 0.9 / 2.1 | 27 versus 28 | 83.68% | 95.26% |
| 5 | 1.5 / 3.5 | 16 versus 17 | 92.77% | 99.33% |

### Very low total budgets

We repeated the threshold construction with a 50/50 bounds/SUM split and much smaller total budgets. The complete-output likelihood attack remained near chance and did not significantly exceed the corresponding ceiling:

| Total epsilon | Bounds/SUM split | One-query attack | Pure-DP ceiling | Median relative error |
|---:|---:|---:|---:|---:|
| 0.01 | 0.005 / 0.005 | 50.24% | 50.250% | 60.0% |
| 0.001 | 0.0005 / 0.0005 | 50.06% | 50.025% | 66.8% |

The small excess in the second row is within Monte Carlo uncertainty. Thus reducing epsilon suppresses this valid filter-MIA channel, but automatic bounding becomes extremely inaccurate. A preliminary grouped-query comparison found:

| Query | Epsilon | Google error | Full filterless error |
|---|---:|---:|---:|
| Orders/month balance | 0.01 | 98.98% | 98.85% |
| Orders/month price | 0.01 | 89.28% | 96.46% |
| Both queries | 0.001 | approximately 100% | approximately 100% |

Filterless removes the query-dependent bound-selection channel, but does not guarantee useful answers at such small per-query budgets.

### Correlated private-filter attack

A different filter can break an end-to-end claim by amplifying one raw-row change into many aggregate contributions. The private table contains 100 background people known to the attacker and may contain one additional target:

```sql
-- Compute one private global Boolean: is the target anywhere in the table?
WITH target_state AS (
  SELECT COUNT(*) > 0 AS target_present
  FROM private_people
  WHERE privacy_unit = :target_id
),

-- The target's existence controls whether every row passes, including the 100 background rows unrelated to the target.
filtered_rows AS (
  SELECT 1.0 AS contribution
  FROM private_people
  CROSS JOIN target_state
  WHERE target_state.target_present
)

SELECT DP_BOUNDED_SUM(
  contribution,
  lower => 0.0,
  upper => 1.0,
  epsilon => 0.5
)
FROM filtered_rows;
```

The two worlds are therefore:

| World | Raw private table | `target_present` | Rows given to the DP sum |
|---|---:|---:|---:|
| `D0`: target absent | 100 known background rows | false | 0 |
| `D1`: target present | the same 100 rows plus the target | true | 101 |

At the raw-table boundary, `D0` and `D1` are valid add-one neighbors. After this global private filter, however, the downstream aggregate receives zero versus 101 contributions. This differs from a stable row-local filter, where adding one person can add at most that person's contribution. A compiler that treats the global filter as row-local and calibrates for sensitivity 1 adds noise at scale 2 instead of the required scale 202 for this bounded cohort.

We tested the current primitive with public bounds `[0,1]`, epsilon 0.5, and 20,000
releases per world:

| Filter | Effective aggregate change | Accuracy | Classification |
|---|---:|---:|---|
| Stable row-local predicate | one contribution | 60.682% | Within the 62.246% ceiling |
| Correlated private predicate | 101 contributions | 100.000% | Breaks a nominal end-to-end epsilon-0.5 claim |

The same distinction remains visible at very small budgets over 100,000 releases per world:

| Epsilon | Stable row-local filter | Correlated private filter | Nominal ceiling |
|---:|---:|---:|---:|
| 0.01 | 50.224% | 70.025% | 50.250% |
| 0.001 | 49.927% | 52.574% | 50.025% |

The correlated construction behaves like sensitivity 101 rather than sensitivity 1. Lowering epsilon therefore reduces the attack, but does not restore the incorrectly claimed end-to-end bound.

The stable control produced overlapping output ranges:

```text
target absent:  [-23.201, 20.357]
target present: [-19.705, 21.239]
```

The correlated filter produced separated ranges:

```text
target absent:  [-15.557, 16.680]
target present: [80.677, 118.107]
```

### What did not work

Several intuitive filter attacks did not falsify the guarantee:

- Increasing one outlier from 1,000 to one trillion did not move the median bound beyond 256.
- One high-value contributor among 10,000 ordinary contributors produced only 50.53% scale-attack accuracy.
- Ten high contributors were insufficient to select their magnitude bin. With very small populations, both worlds returned `FailedPrecondition`, so the status provided no membership signal.
- Comparing zero with 100 billionaires was nearly perfectly distinguishable, but those datasets differ by 100 privacy units and are not an individual neighboring pair.
- The valid 99-versus-100 billionaire comparison produced 50.73% accuracy.
- Near the failure threshold, the strongest status-only attack was 54.41%. That is below the 57.44% ceiling for the epsilon-0.3 ApproxBounds channel.

## 2. Floating-point implementation attacks

Google's documented attack model explicitly claims protection against floating-point
non-associativity and input-order attacks. The following tests observe only the released value and therefore fall inside that model.

### Fixed-bound sum reordering

Current fixed-bound `BoundedSum<T>` uses:

```cpp
partial_sum_ += Clamp<T>(lower_, upper_, t);
```

For `T = float`, this accumulator is not permutation invariant. We used public bounds `[1,2]` and the neighboring ordered inputs:

```text
D_present:
  2^24 copies of 1.0f, followed by 2^23 copies of 2.0f

D_absent:
  2^23 copies of 2.0f, followed by (2^24 - 1) copies of 1.0f
```

Ignoring order, the worlds differ by exactly one target contribution. At epsilon 0.5, Google calibrates then noise to ideal sensitivity 2, but the implemented sums differ by approximately `2^24`.

| World | Released range over 100 trials |
|---|---:|
| Target absent | 16,777,191--16,777,238 |
| Target present | 33,554,420--33,554,452 |

A fixed threshold achieved 100% accuracy over 200 decisions, far above the 62.246% ceiling.

### Four-row overflow and NaN attack

An even smaller construction uses `M = std::numeric_limits<float>::max()` and fixed bounds `[-M,M]`:

```text
D_present order: M, M, -M, 0_target
D_absent order:  M, -M, M
```

As multisets the worlds differ only by the zero-valued target, and both ideal sums equal `M`. In float32,
`M + M` overflows. The present-world noise path then returns NaN, while the absent order cancels before the
final `M`.

Over 50,000 epsilon-0.5 releases per world:

| Output class | Target absent | Target present |
|---|---:|---:|
| Finite | 15,907 | 0 |
| Positive infinity | 24,874 | 0 |
| Negative infinity | 9,219 | 0 |
| NaN | 0 | 50,000 |

The classifier `predict present iff output is NaN` achieved 100% over 100,000 decisions. For the event
`S = {output is NaN}`, the observed probabilities are zero and one, which cannot satisfy pure DP for any finite epsilon.

### Fixed-bound mean and variance

With bounds `[1,2]`, values `1.0f` and `1.75f`, and the same `2^24`/`2^23` order pattern:

| Aggregate | Target absent | Target present | Accuracy |
|---|---:|---:|---:|
| `BoundedMean<float>` | 1.249999833--1.250000211 | 1.166666534--1.166666939 | 100% (20/20) |
| `BoundedVariance<float>` | exactly 0.25 | exactly 0.0 | 100% (20/20) |

Variance stores its ordinary sum in float and its sum of squares in double. The inconsistent moments land outside the feasible variance interval, and final clamping maps the neighboring worlds to opposite endpoints.

### ApproxBounds partial-sum attack

Automatic-bounds sum, mean, and variance share a second vulnerable representation. Values are decomposed into floating partials for logarithmic bins. Values `1.25f` and `1.75f` share the `(1,2]` bin but contribute different remainders.

We used:

```text
D_present:
  2^24 copies of 1.25f, followed by 2^23 copies of 1.75f

D_absent:
  2^23 copies of 1.75f, followed by (2^24 - 1) copies of 1.25f
```

The relevant pre-noise partials were 12,582,912 and 6,990,506: a separation of 5,592,406. The real mechanism used total epsilon 0.5, assigned epsilon 0.25 to ApproxBounds, and selected the same bounds `[-2,2]` in the observed neighboring releases. The sum stage's nominal Laplace scale was 8.

| Aggregate | Target absent | Target present | Accuracy |
|---|---:|---:|---:|
| Automatic `BoundedSum<float>` | 23,767,694--23,767,744 | 29,360,104--29,360,146 | 100% (40/40) |
| Automatic `BoundedMean<float>` | 1.277777580--1.277778355 | 1.499998915--1.500000300 | 100% (20/20) |
| Automatic `BoundedVariance<float>` | exactly 0.25 | exactly 0.0 | 100% (20/20) |

### Very low total budgets

Making the noise much larger did not repair the order-dependent accumulator:

| Mechanism | Epsilon | Target absent | Target present | Accuracy |
|---|---:|---:|---:|---:|
| Fixed bounds `[1,2]` | 0.01 | 16,776,879--16,778,102 | 33,553,646--33,555,032 | 100% |
| Fixed bounds `[1,2]` | 0.001 | 16,771,559--16,783,530 | 33,549,816--33,557,408 | 100% |
| ApproxBounds `[-2,2]` | 0.01 | 23,767,652--23,767,810 | 29,360,352--29,360,424 | 100% |
| ApproxBounds `[-2,2]` | 0.001 | 23,765,700--23,768,192 | 29,359,784--29,361,404 | 100% |

The fixed-bound rows used ten trials per world and the slower ApproxBounds rows used two. Although the sample is small, the neighboring ranges remain separated by millions while the nominal noise grows only to thousands. The filterless approximate SUM returned the correct order-independent totals, 35,651,584 and 35,651,582.75, eliminating this arithmetic channel before DP noise is added.

## 3. Repeated querying and composition

Fresh mechanisms on overlapping private data compose sequentially. Under basic pure-DP composition:

```text
epsilon_transcript = epsilon_1 + epsilon_2 + ... + epsilon_q
```

Post-processing one release is free, but rerunning a query with fresh noise is another release. Disjoint background cohorts do not make queries parallel when the same target participates in all of them.

### Reaching 99% accuracy with epsilon 0.5 per query

The strongest sound repeated-query construction allocated:

```text
epsilon_total  = 0.5 per query
epsilon_bounds = 0.495998196
epsilon_sum    = 0.004001804
```

The neighboring filters contained 49 versus 50 high-value contributors plus 500 ordinary contributors that guaranteed a result. Over 50,000 releases per world, the high bound was selected 19,562 times without the target and 30,503 times with it.

A held-out full-transcript likelihood attack obtained:

| Fresh overlapping queries | Attack accuracy | Basic composed epsilon | Interpretation |
|---:|---:|---:|---|
| 1 | 60.71% | 0.5 | Below the 62.246% ceiling |
| 10 | 75.24% | 5 | Permitted by composition |
| 20 | 82.84% | 10 | Permitted by composition |
| 50 | 95.40% | 25 | Permitted by composition |
| 100 | **99.40%** | 50 | Permitted by composition |
| 150 | 100% observed | 75 | No errors in the finite sample |
| 200 | 100% observed | 100 | No errors in the finite sample |

The 100-query result used 250 held-out transcripts per world. Its approximate 95% Wilson interval was 98.25% to 99.80%. This is not an epsilon-0.5 transcript violation: epsilon 0.5 is the per-query cost, while the basic composed transcript cost is epsilon 50.

At epsilon 2 per query, the same effect is faster:

| Queries | Accuracy | Basic composed epsilon |
|---:|---:|---:|
| 1 | 75.42% | 2 |
| 2 | 81.30% | 4 |
| 3 | 85.96% | 6 |
| 5 | 91.85% | 10 |
| 10 | 97.95% | 20 |

Large per-query budgets can also make one release highly identifying. At total epsilon 5, a SUM-heavy split reached 95.61% in one query and 99.76% in three; all remained within their large composed ceilings.

### Repetition at very low per-query budgets

For the sound ApproxBounds threshold channel, an exact one-release mixture likelihood with a normal approximation for accumulated log-likelihood gave:

| Epsilon/query | Queries | Attack accuracy | Composed epsilon | Composed ceiling |
|---:|---:|---:|---:|---:|
| 0.01 | 100 | 51.41% | 1 | 73.11% |
| 0.01 | 1,000 | 54.45% | 10 | 99.995% |
| 0.01 | 10,000 | 63.80% | 100 | approximately 100% |
| 0.001 | 100 | 50.14% | 0.1 | 52.50% |
| 0.001 | 1,000 | 50.45% | 1 | 73.11% |
| 0.001 | 10,000 | 51.41% | 10 | 99.995% |

This is ordinary sequential composition, not a violation. In contrast, repetition amplifies the correlated-filter sensitivity error: approximately 26 epsilon-0.01 queries or 2,200 epsilon-0.001 queries reach about 99% accuracy. Their nominal composed ceilings are only 56.46% and 90.03%, respectively. The discrepancy persists because each query was calibrated for one changed contribution even though the private predicate changes 101.
