# Filterless DP bounds: evaluation against Google DP

> This document evaluates the earlier frozen, complete-PU bound-channel design. The `filterless`
> branch now implements the related per-query two-channel variant: every qualifying PU contributes
> to the answer, while only a fixed hash sample contributes complete unfiltered per-PU values to
> bound selection. See [filterless_encoded_pu.md](filterless_encoded_pu.md).

Analysts want to run the same aggregate with many predicates without supplying public numeric
bounds for every filtered population. A tempting implementation applies `WHERE`, takes the exact
minimum and maximum of the remaining private contributions, and uses those values for clipping and
noise calibration. That is unsafe: whether a high-value customer satisfies the predicate changes
the selected bound and therefore the output's noise distribution. An attacker testing a sensitive
attribute of a known billionaire can learn whether that person is in the filtered population from
this scale change, even when the bound itself is never returned.

Google DP already fixes this problem. It either requires explicit bounds to be public or spends
privacy budget to infer query-local bounds. Filterless DP is not a fix for a flaw in correctly
configured Google DP. It is an alternative for a repeated-query database workload: privately select
reusable lower and upper bounds once from the unfiltered population, then keep those bounds fixed
across later predicates. The hoped-for benefit is avoiding query-local bound selection on every
query; the cost is that an unfiltered bound can be much wider than a particular filtered query
needs. The evaluation below compares that accounting and utility tradeoff directly with Google's
private implicit-bound approach.

## Running example

Suppose an attacker knows that a particular billionaire has extremely high spending but wants to
learn whether that customer belongs to the sensitive `private_banking` segment. The attacker runs:

```sql
SELECT date_trunc('month', o.order_date) AS month,
       SUM(l.extended_price) AS revenue
FROM customers AS c
JOIN orders AS o USING (customer_id)
JOIN lineitems AS l USING (order_id)
WHERE c.segment = 'private_banking'
GROUP BY month;
```

The two private worlds differ only in the target customer's segment. In one world the billionaire
does not pass the filter; in the other, the billionaire does. All customers, including the target,
remain in the underlying database in both worlds. This models the actual inference question: “is
the billionaire in this filtered population?”, not “does the database contain any billionaire?”

The privacy unit is a customer. Before clipping, the database must therefore reduce the join to one
partial aggregate per customer and output month after applying the query predicate:

```sql
SELECT c.customer_id,
       date_trunc('month', o.order_date) AS month,
       SUM(l.extended_price) AS customer_month_revenue
FROM customers AS c
JOIN orders AS o USING (customer_id)
JOIN lineitems AS l USING (order_id)
WHERE c.segment = 'private_banking'
GROUP BY c.customer_id, month;
```

Write this partial as `t(u,g)`, for customer `u` and month `g`. The clip is applied to this sum. It
is not applied independently to each line item. A billionaire with 20,000 ordinary-sized purchases
must first become one large customer/month contribution; otherwise every row could pass a row-level
clip while the customer's total remained unbounded.

The month grouping is essential. A lifetime total per customer is not equivalent to a monthly
total: a customer can concentrate most spending in one month. Every output grouping key must remain
in the privacy-unit preaggregation.

## The leak from an unsafe data-dependent bound

Suppose an implementation applies the filter and then uses the exact observed maximum
customer/month contribution:

```text
U_Q(D) = max over customers passing Q and months of t_Q(u,g)
scale  = C_u * U_Q(D) / epsilon
```

When the billionaire fails the predicate, `U_Q(D)` might be EUR 10,000. When the same customer
passes it, `U_Q(D)` might be EUR 100 million. The released SUM therefore comes from a narrow
Laplace distribution in one private world and a very wide one in the other. Differential privacy
must hold even when an attacker knows the target's spending and the other rows. Under that standard
auxiliary knowledge, the attacker can center the output under each membership hypothesis and use
the residual magnitude to distinguish the two scales. Repeating equivalent releases makes the
variance visually obvious, but repetition is not the root problem: Laplace distributions with
data-dependent scales have an unbounded likelihood ratio in their tails, so this mechanism has no
finite epsilon guarantee even for one release.

For a concrete single-release test, take `C_u = 1` and epsilon 1. After centering at the candidate
true answer, an absolute error above EUR 1 million has probability about `exp(-100)` when the bound
is EUR 10,000, but about `exp(-0.01) = 99%` when the bound is EUR 100 million. The attacker can
therefore distinguish “fails the filter” from “passes the filter” from the residual magnitude with
overwhelming probability. The changing SUM is not the issue isolated by this test; the changing
noise distribution is.

Clipping and noise calibration must use a bound obtained independently of the private data or by a
separately private mechanism. Merely adding Laplace noise after reading the exact maximum is not DP.

Google BigQuery explicitly requires explicit contribution bounds to come from public information.
If explicit bounds are omitted, BigQuery derives implicit bounds differentially privately after the
predicate, spends part of epsilon on that step, clamps each privacy unit's per-group aggregate, and
then calibrates noise to the privately selected bounds. Its selected bound may respond to the
filtered data, but that response is itself DP and included in the privacy accounting. Proper Google
DP therefore does not have the unsafe exact-maximum leak described above. See Google's
[DP overview](https://docs.cloud.google.com/bigquery/docs/differential-privacy) and
[aggregate reference](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/aggregate-dp-functions).

## Filterless DP

Filterless DP also selects the bound privately, but does so once on the unfiltered population and
reuses it for a family of later predicates. In the running example, changing whether the billionaire
matches `segment = 'private_banking'` does not change the population used for this one-time release:
the customer is present in that population in both worlds. The reusable bound distribution is
therefore independent of this predicate membership. A sufficiently rare billionaire is also
unlikely to make a high-magnitude bin pass the public support threshold.

For nonnegative SUM, the bound release is:

1. Compute every unfiltered `t(u,g)` after joins and `(privacy unit, group)` preaggregation.
2. For each privacy unit, retain `m(u) = max_g t(u,g)`. Each person now contributes one number to
   bound selection, regardless of how many groups they occupy.
3. Place `m(u)` in one of 30 public factor-4 magnitude bins.
4. Add `Laplace(1/epsilon_0)` independently to every bin count. The full histogram has L1
   sensitivity one under add/remove privacy-unit adjacency.
5. Select the upper edge of the highest bin whose noisy count reaches the public support target.
   This selection is post-processing of an `epsilon_0`-DP histogram.

The resulting scalar is `U`. A filtered query then computes its own `t_Q(u,g)` and clamps

```text
clipped_Q(u,g) = min(max(t_Q(u,g), L), U).
```

For the nonnegative workloads evaluated here, `L = 0` is implied by the expression. For signed SUM,
positive maxima and negative magnitudes require two histograms and a corresponding split of the
bound-selection budget.

The use of one maximum per privacy unit is important. Histogramming every `(u,g)` partial before a
cross-group cap would give the histogram sensitivity equal to the number of groups touched by that
person. Reducing to `m(u)` keeps bound selection independent of the later `C_u` choice.

## `C_u` remains on the query path

`[L,U]` bounds one person's value in one output group. `C_u` bounds how many output groups that
person may affect. They are different parameters.

For each filtered query, `C_u` is selected privately from the distribution of groups per person.
The query retains at most `C_u` groups for each person, clamps every retained partial to `[L,U]`,
and adds noise calibrated to

```text
value sensitivity = C_u * (U - L).
```

Thus filterless DP removes only per-query lower/upper-bound selection. It does not freeze `C_u`, and
it does not remove contribution bounding.

## Privacy accounting

The benchmark compares a workload of `N = 20` queries at the same composed epsilon:

```text
Google DP total      = N * epsilon
Filterless DP total  = epsilon_0 + N * (epsilon - epsilon_0 / N)
                     = N * epsilon.
```

Both deployable arms additionally spend 0.01 epsilon per query selecting `C_u`; this cost is taken
from their respective query budgets. Google spends a bounds budget on every query. Filterless
spends `epsilon_0 = 1` once and charges its amortized cost, 0.05, to each of 20 queries.

For delta, Google's per-query allowance is split equally between implicit bound selection and
private partition selection. The filterless histogram is pure DP, so filterless uses its per-query
delta only for partition selection.

This comparison only makes sense as a repeated-query workload. For one isolated query, filterless
cannot claim the amortization advantage.

## Filter-membership attack

The experiment contains 10,000 filtered customers with contributions between 50 and 150, plus a
known customer contributing EUR 100 million. Both private worlds contain that billionaire. In the
first world the customer fails the predicate; in the neighboring world the customer passes it.
Each trial releases one noisy SUM at total epsilon 1: 0.3 for private bound selection and 0.7 for the
clipped sum. The attacker sees only the released SUM and chooses the best tested mean-shift or
variance threshold over 200,000 trials.

“Raw output” is the end-to-end attack on the actual release. “Scale diagnostic” centers each sample
at the mechanism's clipped true answer under its corresponding hypothesis before classification.
This intentionally gives the diagnostic oracle knowledge: it removes the mean change caused by the
target and measures only whether the residual noise scale reveals predicate membership. It is not a
claim that the two true query answers are identical.

| Mechanism | Median `U`, fails predicate | Median `U`, passes predicate | Raw output | Scale diagnostic |
|---|---:|---:|---:|---:|
| Filterless DP, private factor-4 histogram | 256 | 256 | 64.90% | 50.15% |
| Google implicit DP, private base-2 histogram | 256 | 256 | 64.85% | 50.20% |
| Unsafe exact observed maximum | 150 | 100,000,000 | 99.95% | 99.95% |

For equal priors, the maximum classification accuracy permitted by pure epsilon-DP at epsilon 1 is
`e^epsilon / (1 + e^epsilon) = 73.11%`. This is a reference for the pure-DP filterless mechanism;
Google's implicit-bound mechanism also uses delta. In the scale diagnostic, both private mechanisms
are at chance while the unsafe maximum is almost perfectly identifiable from the residual magnitude.

This empirical test is a regression check, not the privacy proof. The proof comes from the
sensitivity-one histogram, post-processing, clipping, and sequential composition.

## Utility comparison against Google DP

The utility benchmark executes the real joins and SQL groupings before applying either mechanism.
Both mechanisms then use the same private `C_u`, random cross-group truncation, private partition
selection, Laplace value noise, and relative-L1 scoring. A suppressed group is scored as returning
zero. Lower error is better.

The Google baseline uses query-local base-2 implicit bounds and selects `C_u` privately. Filterless
uses its private factor-4 unfiltered bound with the amortized budget above.

`g-wide*` is an ablation: it assumes a public/oracle power-of-two bound wide enough that none of the
observed contributions are clipped. It is not “unbounded DP”—an unbounded SUM has infinite
sensitivity—and it is not deployable as measured.

Results use epsilon 1, delta 1e-6 and 30 deterministic trials.

| TPC-H SF1 query | Groups | Google `U` | Filterless `U` | Google DP | g-wide* | Filterless DP |
|---|---:|---:|---:|---:|---:|---:|
| Order revenue / month, 1 join | 80 | 524,288 | 1,048,576 | 5.04% | 18.39% | 9.76% |
| Lineitem revenue / month, 2 joins | 84 | 524,288 | 1,048,576 | 5.25% | 15.66% | 17.00% |
| Revenue / month + nation | 2,077 | 524,288 | 1,048,576 | 100.00% | 100.00% | 100.00% |
| Revenue / month + priority | 415 | 262,144 | 1,048,576 | 100.00% | 100.00% | 100.00% |
| Revenue / day | 2,524 | 131,072 | 262,144 | 100.00% | 100.00% | 100.00% |
| COUNT / month + nation | 2,077 | 8 | 16 | 100.00% | 100.00% | 100.00% |
| Quantity / month + nation | 2,077 | 256 | 1,024 | 100.00% | 100.00% | 100.00% |
| Revenue / year + nation | 175 | 2,097,152 | 4,194,304 | 4.92% | 16.62% | 17.52% |
| Automobile / month + nation | 2,079 | 524,288 | 1,048,576 | 100.00% | 100.00% | 100.00% |
| Revenue / month + supplier nation, 4 joins | 2,077 | 131,072 | 262,144 | 100.00% | 100.00% | 100.00% |

| Synthetic query | Groups | Google `U` | Filterless `U` | Google DP | g-wide* | Filterless DP |
|---|---:|---:|---:|---:|---:|---:|
| Bounded SUM | 12 | 1,024 | 1,024 | 1.67% | 1.46% | 1.54% |
| Log-skew SUM | 120 | 64 | 256 | 100.00% | 99.98% | 100.00% |
| Pareto SUM | 1,200 | 512 | 16,384 | 100.00% | 100.00% | 100.00% |
| Billionaires excluded by predicate | 120 | 512 | 1,024 | 3.43% | 3.01% | 6.42% |
| Everyone, including a few billionaires | 120 | 512 | 1,024 | 10.60% | 100.00% | 11.99% |
| COUNT | 1,200 | 4 | 4 | 100.00% | 100.00% | 100.00% |

### Stable filtered cohorts

A targeted workload tests the case most favorable to reusable bounds. A raw event table contains
10,000 customers with the same bounded value distribution. Deterministic `WHERE` predicates select
40, 80, 200 or 500 customers without selecting on value. Each query returns either one scalar SUM
or four groups. Filterless learns its bound from all 10,000 customers; Google learns a query-local
bound only from the selected customers. The SQL aggregation and filtering run in DuckDB before the
DP mechanisms are applied.

At `N = 20` reused queries:

| Filtered customers | Output groups | Google `U` | Filterless `U` | Google DP | Filterless DP |
|---:|---:|---:|---:|---:|---:|
| 40 | 1 | fixed failure bound | 16,384 | 100.00% | 11.73% |
| 80 | 1 | 8,192 | 16,384 | 5.88% | 5.16% |
| 200 | 1 | 8,192 | 16,384 | 2.43% | 1.91% |
| 500 | 1 | 8,192 | 16,384 | 0.27% | 0.55% |
| 40 | 4 | fixed failure bound | 4,096 | 100.00% | 100.00% |
| 80 | 4 | 2,048 | 4,096 | 100.00% | 97.41% |
| 200 | 4 | 2,048 | 4,096 | 56.12% | 48.77% |
| 500 | 4 | 2,048 | 4,096 | 1.51% | 2.79% |

The 40-customer scalar query is a real “Google bad, filterless useful” case in this harness.
Google's query-local `ApproxBounds` histogram has too little support for any magnitude bin to clear
its DP threshold. With no analyst-supplied public bound, the benchmark uses its fixed public failure
bound, `2^46`; the resulting noise makes the answer useless. Returning no answer instead would have
the same utility score. Filterless has 10,000 records for its one-time histogram, selects 16,384,
and returns the scalar SUM with 11.73% relative error.

This is a narrow crossover, not a general dominance result. At 80 and 200 customers, Google can
select a useful bound but still pays the per-query bound budget, so filterless is modestly better.
At 500 customers, Google both selects a bound and makes it twice as tight as the factor-4 filterless
bound, giving 0.27% rather than 0.55% error. With four output groups, private partition selection
dominates at small cohort sizes, so neither bound strategy is useful until support grows.

The number of queries sharing the bound changes the 40-customer scalar result as follows:

| Queries sharing bound | Google DP | Filterless DP |
|---:|---:|---:|
| 5 | 100.00% | 17.09% |
| 20 | 100.00% | 11.73% |
| 100 | 100.00% | 11.26% |

Most of the large difference comes from pooling 10,000 customers for bound selection, not from
amortization alone. Amortization explains the filterless improvement from 17.09% to 11.26% as reuse
increases. Google's per-query result is unchanged because every filtered query still estimates its
bound from the same 40-customer cohort.

## Findings

Filterless DP closes the predicate-dependent noise-scale channel as effectively as Google implicit
DP in the tested filter-membership attack. Both private mechanisms hold the median bound at 256
when the billionaire switches from failing to passing the predicate; the unsafe query-local exact
maximum changes by six orders of magnitude.

Filterless DP does not generally improve single-query utility. Its reusable bound sees the full
population and is commonly wider than the bound Google selects after applying the query predicate.
On monthly lineitem revenue, the factor-of-two bound difference corresponds to 17.00% filterless
error versus 5.25% for Google. When the predicate excludes the billionaire tail, filterless is
6.42% versus Google's 3.43%.

The all-customer billionaire workload exposes the unavoidable tradeoff. Both private mechanisms
select a bound below the sparse billionaire level and therefore clip billionaire contributions.
Google obtains 10.60% error and filterless 11.99%. A bound wide enough to avoid clipping gives the
wide-bound ablation approximately 100% error because its noise scale is enormous. Protecting the
presence of a rare, dominant contributor and accurately reporting that contributor's full value
are conflicting objectives.

Filterless can recover its one-time metadata cost on stable, coarse workloads. On the bounded
12-group synthetic query it reaches 1.54% versus Google at 1.67%. More importantly, a scalar query
over a stable 40-customer cohort gives 11.73% filterless error while Google's query-local bound
selection cannot clear its support threshold and yields 100% error. That large win requires a small
filtered cohort drawn from the same value distribution as a much larger reusable population. At
500 customers Google has enough local support and its tighter bound wins, 0.27% versus 0.55%.

Fine grouping overwhelms either bounds strategy. At hundreds or thousands of sparse groups,
private partition selection suppresses most cells and relative error approaches 100%. This is
caused by low privacy-unit support and large cross-group footprints, not by the number of SQL joins
itself.

## Filterless grouping from the meeting notes

This is separate from reusable value bounds. Value bounds determine clipping and noise scale;
group selection determines which `GROUP BY` keys may appear in the result. Peter's stated scope is
a fixed public result-key universe: groups released once from the unfiltered relation remain result
rows after later predicates, even when their filtered truth is zero. Under that contract, utility
should be scored on the nonempty filtered groups rather than penalising the mechanism for returning
the additional public rows.

### Do query-dependent Google bins leak?

No, provided the exponential-bin counts are DP-noised, their privacy budget is charged, and every
later operation is calibrated to the selected bound. The predicate may change the noised histogram,
the selected clipping bound and the final noise scale; that dependence is protected by adaptive
composition. An exact histogram, an exact maximum, or an unnoised HLL used for the same decision
does leak. Sampling or sketching alone is not DP.

### Filterless group set plus clipping

The existing matched benchmark was rerun on TPC-H SF1 with monthly grouping, epsilon 1, delta 1e-6,
ten trials, and `N = 20` queries amortising the one-time group-set release. `Google τ` uses
query-local private partition selection. `Filterless groups` uses the same Google value clipping
but a DP group set released from the unfiltered relation. The table uses Peter's scope and scores
only groups whose filtered truth is nonzero:

| `WHERE` predicate | Google τ | Filterless groups + Google clipping |
|---|---:|---:|
| none | 1.39% | 0.91% |
| `c_acctbal >= 8000` | 4.28% | 3.39% |
| `c_acctbal >= 9900` | 99.43% | 36.37% |
| `l_shipdate >= DATE '1996-01-01'` | 0.64% | 0.61% |
| `l_shipdate >= DATE '1998-01-01'` | 0.25% | 0.24% |
| `c_nationkey < 5` | 4.06% | 3.15% |
| 1998 date predicate and `c_nationkey < 5` | 1.29% | 1.17% |

On this metric, filterless grouping improves every tested predicate. Under normal SQL
result-set scoring, the group-side predicates expose the tradeoff Peter declared irrelevant: the
frozen mechanism emits noise for public months outside the filtered date range, and charging those
extra rows can reverse the result.

### Sampling nonqualifying privacy units

The timing experiment below manually implements one deterministic hash sample in SQL. It does not
invoke the extension's existing accelerated-stochastic (AS) aggregates. It therefore measures the
physical plan that drops approximately 63/64 of nonqualifying `(PU, group)` pairs, not the cost of
maintaining 64 AS lanes.

The proposed execution shortcut evaluates the predicate, retains every qualifying `(PU, group)`,
and hash-samples nonqualifying `(PU, group)` pairs. Classification must happen after privacy-unit /
group preaggregation: a customer with any qualifying row in a group is active once, rather than
being counted in both populations. The valid support estimator uses two sketches:

```text
estimated unfiltered support
    = HLL(qualifying PUs) + r * HLL(sampled nonqualifying PUs),
```

where `1/r` is the sampling rate. Inserting the same PU into one HLL `r` times does nothing, so the
weight cannot be applied inside a single sketch.

On TPC-H SF1, sampling nonqualifiers at 1/64 is 2.8–4.4× faster than fully filterless
preaggregation, but remains 20–50% slower than an ordinary filtered query. All filtered SUMs are
bit-exact because no qualifying row is sampled away. This does not mean end-to-end utility is
unchanged: the experiment did not use the sampled support estimate to make the final group-release
decision.

| Grouping / predicate | Full filterless | Sampled 1/64 | Speedup | Normal filtered | Exact-sample support MdAE / p95 | HLL support MdAE / p95 |
|---|---:|---:|---:|---:|---:|---:|
| month / customer balance | 0.544 s | 0.123 s | 4.42× | 0.105 s | 2.48% / 7.64% | 8.47% / 81.79% |
| month / quantity | 0.554 s | 0.196 s | 2.83× | 0.138 s | 2.61% / 7.15% | 8.73% / 70.41% |
| month + nation / customer balance | 0.655 s | 0.149 s | 4.41× | 0.127 s | 12.39% / 37.59% | 15.72% / 77.99% |
| month + nation / quantity | 0.669 s | 0.229 s | 2.92× | 0.166 s | 11.52% / 34.77% | 15.09% / 66.10% |

At 1/8 sampling, exact-sample median support error drops to 0.87–4.52%, but speedup falls to
2.4–3.1×. DuckDB's HLL has 26.7–29.4% p95 error even without sampling; multiplying a sampled HLL
by 64 magnifies its tail error. HLL is therefore suitable as a query-planning estimate here, but
not as a knife-edge group-release decision without a conservative margin and DP protection.

Without HLL, if a group has `N` nonqualifying PUs and each is retained with probability `q`, the
inverse-probability estimate is unbiased but has relative standard deviation
`sqrt((1-q)/(N*q))`. At `q = 1/64` this is approximately `sqrt(63/N)`. Sampling therefore leaves
the filtered aggregate value unchanged but can worsen group-selection utility through false
suppression or false release near the threshold. That end-to-end effect still needs to be measured.

The existing AS engine already provides deterministic PU-to-lane assignment and rescales each
one-of-64 lane by approximately 64. It does not currently expose Peter's asymmetric rewrite as one
operation: qualifying PUs must affect the full filtered answer, while nonqualifying PUs affect only
one cover lane. Maintaining all 64 lanes also processes every nonqualifying PU once, so it is not
equivalent to the SQL plan that physically drops 63/64 of them. Reusing AS requires a dedicated
rewrite and a separate performance/utility benchmark.

#### One-lane end-to-end prototype

The narrow prototype now exists in `attacks/filterless_one_lane_prototype.py`. It simulates the
existing AS hash-lane assignment but does not yet invoke a compiler rewrite or an AS aggregate. It
assumes each PU contributes one nonnegative value to one group (`C_u = 1`). Qualifying PUs enter
the metadata with weight 1; nonqualifying PUs in the selected AS lane enter with weight 64. It builds a joint
`(group, factor-4 value bin)` histogram, freezes metadata from a 20% predicate, and safely falls
back to a query-local DP histogram for groups not selected by that metadata. As a deterministic
sanity check, averaging the estimators from all 64 lanes must exactly reproduce the unfiltered
histogram.

Because a selected nonqualifying PU can change one metadata cell by 64, the metadata histogram is
noised and thresholded at sensitivity 64. The final filtered SUM still has sensitivity equal to its
selected clipping bound: the factor 64 applies to metadata construction, not to qualifying values
in the released aggregate. The comparison below gives both mechanisms the same per-query split
(`epsilon_gate = 0.3`, `epsilon_value = 0.7`); filterless spends an additional one-time
`epsilon_0 = 1` on reusable metadata. It therefore is not a same-total-budget comparison for a
single isolated query.

With 65,536 PUs and 30 deterministic trials, bounded values show no utility difference: the
filterless and query-local mechanisms choose the same useful bins or the hybrid falls back. For
factor-4-spread values, the representative results are:

| Groups / predicate selectivity | Query-local relative L1 | One-lane filterless relative L1 | Result |
|---|---:|---:|---|
| 4 / 100% | 0.25% | 1.16% | filterless 4.6× worse |
| 4 / 20% | 1.26% | 2.12% | filterless 1.7× worse |
| 4 / 2% | 100.00% | 13.51% | filterless 7.4× better |
| 12 / 100% | 0.88% | 1.65% | filterless 1.9× worse |
| 12 / 20% | 3.80% | 4.54% | filterless 1.2× worse |
| 120 / 20% | 100.00% | 100.00% | neither selects useful groups |

The useful case is real but narrow: a coarse group has enough reusable population support to
survive sensitivity-64 metadata thresholding while the 2% query-local histogram is too sparse.
For denser queries, the frozen metadata can select a wider value bin than the query needs and add
more SUM noise. For fine groupings, splitting support across `(group, bin)` cells defeats both
mechanisms.

In the worst selected lane, changing the target from predicate-qualifying (weight 1) to
nonqualifying (weight 64) is a mean shift of 63. Noise calibrated incorrectly to sensitivity 1
allows 99.95% attack accuracy. Standard Laplace calibration to sensitivity 64 reduces the measured
accuracy to 69.43%, below the 73.11% equal-prior ceiling for pure `epsilon = 1` DP. Thus rescaling by
64 is usable only if the metadata mechanism also pays for sensitivity 64; the PAC closeness of the
lane estimate does not provide the standard-DP guarantee.

In this first prototype, sampled metadata is filter-dependent even when the target unfiltered histogram is not.
For two different 20% predicates over the same 65,536 PUs, the prototype held the table, selected
lane, and Laplace noise draw fixed. Across all 64 possible selected lanes, the two sampled
histograms differed by 8.57% median relative L1 (10.68% p95). Their median sampling error relative
to the same full histogram was 14.58% and 13.96%. Most runs still selected the same four bounds,
but at p95 one of four groups selected a different bound solely because the predicate changed.
The DP noise scale remains `64 / epsilon`; what changes is the input histogram underneath that
noise. Recomputing this construction for another predicate is therefore a new query-dependent DP
mechanism invocation. Only a previously frozen noised result can be reused without another spend.

#### Corrected complete-PU bound channel

`attacks/filterless_complete_pu_poc.py` tests a corrected two-channel rewrite for arbitrary row
predicates. The physical scan still retains `X OR sampled`, but the bound histogram no longer
combines exact qualifying partials with sampled nonqualifiers:

```text
answer channel = every row satisfying X
bound channel  = every row for a fixed sample of complete PUs
```

After preaggregation, the answer channel contains the exact filtered contribution for every
`(PU, group)`. The bound channel contains a complete unfiltered contribution for a sampled PU or
nothing for an unsampled PU. A sampled contribution enters its raw magnitude bin with weight
`64/k` when `k` of 64 lanes are retained. The bin counts are DP-noised at add/remove sensitivity
`(64/k) * C_u`; qualifying contributions do not enter the bound histogram merely because they
qualify.

DuckDB `EXCEPT ALL` checks prove both channels equal their direct SQL definitions for three
different row predicates. The complete-PU bound channel is bit-identical across those filters. The
same equivalence check passes on TPC-H SF1 for customer lifetime order totals while the answer uses
the row predicate `o_orderdate >= DATE '1998-01-01'`. The earlier partial-PU objection therefore
does not apply to this two-channel variant.

Sampling error falls predictably with more lanes, but fine groups remain difficult:

| Data / groups | 1/64 MdAE | 2/64 MdAE | 4/64 MdAE | 8/64 MdAE |
|---|---:|---:|---:|---:|
| bounded synthetic / 12 | 14.52% | 10.37% | 7.21% | 4.99% |
| bounded synthetic / 120 | 46.42% | 33.18% | 22.75% | 15.57% |
| skewed synthetic / 12 | 17.63% | 12.27% | 8.55% | 5.95% |
| TPC-H nation / 25 | 14.16% | 9.93% | 6.99% | 4.79% |

At a nonprivate support-40 reference boundary, the TPC-H clipping-bin match rate rises from 90.81%
at 1/64 to 94.88%, 99.69%, and 100% respectively. Histogram L1 alone is not a safe proxy for the
bound: on the one-group skewed data, 1/64 has only 4.64% histogram error but selects the same
support-40 bound in just 40.62% of lanes because rare high bins sit near the threshold. Weighted
counts also make threshold behavior discrete and sometimes nonmonotonic as `k` changes.

With DP-noised bins (`epsilon_bounds = 0.3`) and SUM noise (`epsilon_value = 0.7`), TPC-H exposes a
sharp sampling-degree cliff:

| Mechanism | Relative L1 | Groups obtaining a bound | Mean selected bound |
|---|---:|---:|---:|
| exact full filterless | 3.13% | 100.0% | 16,777,216 |
| query-local | 0.28% | 100.0% | 1,048,576 |
| complete-PU 1/64 | 91.51% | 8.5% | 4,194,304 |
| complete-PU 2/64 | 0.78% | 100.0% | 4,194,304 |
| complete-PU 4/64 | 0.78% | 100.0% | 4,194,304 |
| complete-PU 8/64 | 0.78% | 100.0% | 4,194,304 |

One lane fails because sensitivity-64 noise raises the private bin threshold above most nation/bin
support. Two lanes halve both the inverse-probability weight and DP sensitivity and cross that
threshold. The sampled mechanisms' lower error than exact full filterless is not proof that they
reproduce the full bound: their higher-sensitivity private selector chooses a 4x tighter bound,
which happens not to clip much of this filtered TPC-H workload. A different filter containing the
missed tail can turn that apparent win into clipping bias.

This variant fixes filter dependence and row-filter correctness. It does not remove the central
utility tradeoff: increasing `k` improves sampling accuracy, lowers DP histogram sensitivity, and
reduces false suppression, but processes more unfiltered cover rows. It also still requires an
enforced cross-group `C_u`; the PoC deliberately uses one group per PU to isolate the sampling
question.

The sampled plan also has a runtime side channel: more qualifying rows means more rows enter the
aggregation. It is an internal performance optimisation only if execution time and intermediate
cardinality are outside the privacy interface.

### Where τ runs and the safe hybrid

Standard private partition selection runs after `WHERE`: preaggregate by `(PU, group)`, cap each
PU's group footprint, count distinct qualifying PUs with DP noise, then apply τ. Running τ before
the analyst predicate instead creates the one-time filterless group set. Later queries do not need
τ for those already public keys and must return them even at zero filtered support.

The safe hybrid is therefore an OR, not an unprotected mechanism switch:

```text
release group g iff
    g is in the DP-frozen filterless group set
    OR a query-local DP group/bin histogram clears its threshold.
```

Google-style exponential bins can supply the second gate if they count distinct PUs per
`(group, bin)`, enforce the cross-group contribution cap, account for all bins in the threshold,
and spend privacy budget. A value histogram without group keys, a row-count histogram, or an HLL
estimate cannot safely replace that gate.

The knife-edge attack changes one `(group, bin)` count from `support-1` to `support`:

| Group gate | Membership attack accuracy |
|---|---:|
| Query-local DP exponential bin | 65.81% |
| DP-frozen filterless group set | 50.00% |
| Unnoised exact or HLL support gate | 100.00% |

The filterless value-bound attack remains separate: after clipping, its noise-scale diagnostic is
50.15%, versus 50.20% for Google implicit bounds and 99.95% for an unsafe exact post-filter bound.

## Joins and other aggregates

Join count is not a sensitivity parameter. What matters is the relation produced after the join:
the distribution of `t(u,g)` and the number of groups per privacy unit. A one-to-many join can
inflate the partial aggregate; an additional many-to-one dimension join may change nothing; a
many-to-many join can multiply facts and widen the bound dramatically.

COUNT follows the same process with per-person/per-group counts. AVG requires separately bounded
SUM and COUNT components and becomes unstable when the noisy denominator is small. Signed SUM
requires private lower and upper histograms. MIN and MAX need separate analysis because they do not
receive the averaging benefit of SUM and are dominated more easily by rare extrema.

## Decision

Filterless DP is technically viable without analyst-supplied public numeric bounds. Its reusable
private histogram prevents a later predicate from switching the bound or noise scale when a known
billionaire enters the filtered population, and its one-time cost can be amortized across a repeated
workload. Google implicit bounds already protect the same inference by selecting query-local bounds
privately; filterless changes the accounting and utility tradeoff, not the basic privacy guarantee.

The evaluation does not show a general utility improvement over Google DP. Filterless is competitive
when later predicates preserve the unfiltered contribution distribution and queries are coarse. It
is worse when predicates remove the heavy tail, and neither mechanism is useful on very fine sparse
groupings at the tested privacy parameters.
