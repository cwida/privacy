# Where filterless bounds help

## TL;DR

- Filterless strongly wins when a narrow filter has too few rows for query-local ApproxBounds but the unfiltered population has a stable value scale.
- Per-group filterless bounds win when SQL groups have very different scales; frozen metadata can also save budget across query families.
- Approximate SUM removes Google's order-dependent floating-point channel, and filter-independent bounds can reduce filter-membership leakage.
- The implementation now prevents noisy empty high bins from winning at low epsilon by using `max(configured_support, noise_aware_threshold)`.
- Google ApproxBounds already uses this noise-aware threshold idea. It relaxes the threshold on the same noisy histogram and returns no bound when the data remains insufficient.
- This does not normally suppress a rare filter: support is measured on the unfiltered histogram. It does suppress bound selection when even the full histogram cannot distinguish a real bin from noise.

This document collects workloads where filter-independent bounds improve privacy or utility over query-local automatic bounds. The comparisons use the same total epsilon and include noise on the filterless histogram. Successful membership inference is classified against the relevant DP ceiling; accuracy above 50% alone is not a violation.

## 1. Rare filters with a stable global scale

The clearest utility case is a large population whose values have a stable scale, followed by a narrow filter. Query-local ApproxBounds sees too few qualifying privacy units and returns no result, while filterless bounds use the much larger unfiltered population.

We used per-PU values of 100 and allocated 5% of epsilon to bounds and 95% to the SUM. Google results use upstream C++ commit `b5b0286`; filterless results use the implemented aggregate with a noised histogram.

| Total epsilon | Filtered PUs | Unfiltered PUs | Google ApproxBounds | Filterless median error | Filterless p90 error |
|---:|---:|---:|---:|---:|---:|
| 1 | 25 | 10,000 | 1,000/1,000 failures | 3.76% | 12.27% |
| 0.1 | 250 | 10,000 | 1,000/1,000 failures | 3.70% | 12.27% |
| 0.01 | 2,500 | 100,000 | 1,000/1,000 failures | 3.80% | 12.24% |

This applies to rare diagnoses, fraud cohorts, unusual customer segments, and narrow dashboards when the value scale remains representative of the full population.

Even when Google finds a bound, filterless can spend less of the query budget on selection. At epsilon 1 with 50 qualifying PUs, Google needed half the budget for reliable ApproxBounds and had about 3.5% median error. Filterless used 5% for bounds and had 1.88% median error.

## 2. Groups with different value scales

Query-wide automatic bounds can be dominated by one high-scale group. Filterless maintains a separate histogram per released group, so ordinary groups do not inherit the outlier group's noise scale.

We used 100 public groups with 1,000 disjoint privacy units each. Ninety-nine groups had per-PU values of 100 and one group had values of 10,000. At epsilon 1:

| Metric | Google query-wide bound | Filterless per-group bounds |
|---|---:|---:|
| Overall relative L1 error | 8.64% | 1.44% |
| p90 overall error | 9.80% | 1.76% |
| Median small-group error | 11.96% | 0.093% |

The overall improvement was about 6x and the typical small-group improvement was about 128x. This comparison applies to a SQL mechanism using one query-wide bound. Google could narrow the gap by running separately bounded per-group mechanisms when parallel composition is valid.

## 3. Privacy advantages

### Constant-mean filtered AVG

Consider neighboring filter outcomes with 34 versus 35 qualifying people, all contributing 128. The true AVG is 128 in both worlds, so participation should ideally be difficult to infer.

At epsilon 1, the Google ApproxBounds success/failure channel alone produced 58.22% membership accuracy. The implemented filterless AVG, whose histogram included the same full population in both filter worlds, produced 50.82% held-out accuracy with a residual-magnitude classifier. Both results are below the epsilon-1 ceiling of 73.11%; this is lower empirical leakage, not a Google DP violation.

### Numerical aggregation

Google's floating SUM accumulator allowed 100% membership inference through input reordering even at epsilon 0.001. The filterless approximate SUM returned the correct order-independent neighboring totals and removed this arithmetic channel before noise was calibrated.

### Query families

A frozen filterless histogram can be paid for once and reused. For 100 queries where Google spends 0.3 epsilon on bounds and 0.7 on each value:

```text
Google transcript:     100 * (0.3 + 0.7) = 100 epsilon
Filterless transcript: 0.3 + 100 * 0.7   = 70.3 epsilon
```

This gives the same value noise with 29.7% less basic-composition cost. Under a fixed transcript budget, the saved budget can instead reduce answer noise. The current implementation recomputes bound noise per query, so realizing this benefit requires cached metadata and a session accountant.

## 4. Noise-aware histogram thresholds

The initial low-budget filterless experiments used a fixed support threshold of 100. This caused catastrophic utility failures: when histogram epsilon was small, noise in an empty high-magnitude bin often exceeded 100, so the mechanism selected an enormous clipping bound.

The threshold should grow with the histogram noise. Let:

```text
s       = histogram sensitivity / epsilon_bounds
B       = number of bins per sign
rho     = desired probability that every empty bin stays below the threshold
p       = rho^(1 / (2B))
tau     = LaplaceQuantile(p, scale=s)
        = -s * log(2 * (1 - p))
```

For small failure probability `beta = 1 - rho`, this is approximately:

```text
tau ~= s * log(B / beta)
```

A conservative filterless rule is therefore:

```text
effective_support = max(dp_filterless_clip_support, tau)
```

The configured support remains the semantic minimum crowd size, while `tau` prevents noisy empty bins from winning. Counts and other nonnegative components can use only the number of candidate positive bins; using positive and negative bins is a conservative common rule.

### Does Google DP do this?

Yes. Google's `ApproxBounds` computes exactly this kind of threshold. In `cc/algorithms/approx-bounds.h`, it:

1. adds Laplace noise to every positive and negative histogram bin;
2. computes `p = success_probability^(1 / (2 * num_bins))`;
3. uses the noise mechanism's `Quantile(p)` as the bin threshold;
4. selects the outermost bins above that threshold; and
5. returns `FailedPrecondition` if no credible bin exists.

The default success probability is `1 - 10^-9`. If no bin passes, Google reuses the same already-noised histogram while relaxing the failure probability by a factor of ten, stopping before it becomes worse than `10^-6`. Reusing the same DP histogram for these threshold checks is post-processing and does not spend more privacy budget.

Our `FindSupportedBin` noises each bin at scale

```text
sample_weight * max_groups / epsilon_bounds
```

and now compares the noised result with the larger of `dp_filterless_clip_support` and the noise-aware threshold above, using an empty-histogram failure probability of `10^-6`. This change concerns utility and false-bound probability; the noised histogram was already DP even with the poorly chosen fixed threshold.

## 5. Where filterless does not win

Broad filters often favor Google because query-local bounds are tighter. At epsilon 1, two TPC-H checks gave Google versus full-filterless error of 7.29% versus 16.98% and 1.38% versus 11.63%. Heavy-tailed and very fine grouped queries were also generally poor for both mechanisms.

The emerging operating envelope is therefore:

- filterless wins on sparse filters with representative full-domain values;
- per-group filterless bounds win when group scales differ substantially;
- frozen metadata wins across reusable query families;
- approximate aggregation avoids order-dependent floating-point channels; and
- Google often wins when a broad filter permits a much tighter query-local bound.
