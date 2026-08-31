# Private contribution-bound derivation

This is a short historical summary of the experiments that led to the current filterless design.
The implementation and its supported SQL shapes are documented in
[`filterless_encoded_pu.md`](filterless_encoded_pu.md). Earlier value-bound designs are summarized
in [`filterless_value_bounds.md`](filterless_value_bounds.md).

## Problem

A bounded DP aggregate needs public contribution limits or a private mechanism that selects them.
Reading an exact maximum after `WHERE` is unsafe: one person's inclusion can change the clipping
bound, which can change clipping, the output center, and the noise scale.

Google DP's ApproxBounds addresses data-dependent value bounds by privatizing a geometric
histogram. Its privacy cost must be composed with the final aggregate. The selected bound need not
be returned explicitly to be observable; the output distribution can still reveal it.

The filterless design asks a narrower systems question: can the histogram input be made independent
of the query predicate while retaining every qualifying row in the answer?

## Resulting design

For privacy unit `PU` and predicate `X`, the scan retains:

```text
X OR sampled(PU)
```

The plan then builds two channels:

```text
answer channel    = filtered contribution from every PU satisfying X
histogram channel = complete unfiltered contribution from a fixed sample of PUs
```

Only sampled PUs affect bound selection. Qualifying PUs outside the sample affect the answer but
not the histogram. The fixed sample is defined by high bits of a stable PU hash, and sampled
histogram contributions receive inverse-probability weight `2^p`.

This construction makes the histogram distribution invariant when only the predicate changes on a
fixed database. It does not make the histogram public: adding or removing a sampled PU can still
change it.

The implementation therefore noises histogram support with L1 sensitivity:

```text
2^p * C_u
```

where `C_u` is the maximum number of groups one PU may affect. Each aggregate component splits its
epsilon between private bound selection and the final value release.

## What survived review

- Query-independent histogram inputs remove the predicate-dependent bound channel. This simplifies
  the proof and makes output scales stable across filters.
- A raw histogram is not safe under database updates, even when it is filter-independent. Private
  histogram selection and composition are required.
- Sampling reduces unfiltered relational work, but its inverse-probability weight increases
  histogram sensitivity by the same factor. Sampling is therefore a performance tradeoff, not free
  privacy or utility.
- Automatic bounds can be mathematically DP and still provide weak practical membership
  resistance at a large epsilon. Accuracy above 50% is not by itself a violation; for equal priors,
  pure epsilon-DP permits accuracy up to `exp(epsilon) / (1 + exp(epsilon))`.
- Group existence is part of the transcript. Private group keys require a public key universe or
  separately accounted partition selection.
- Repeated queries require composition. Filter independence does not make multiple fresh releases
  free.

## What did not survive

- Exact maxima, unnoised histogram supports, and unnoised HLL-based release gates are unsafe
  data-dependent decisions.
- Large utility headlines from early experiments did not survive fully tuning both mechanisms,
  widening budget-split searches, or changing datasets and metrics.
- TPC-H alone was not representative. Benefits narrowed or changed sign on workloads where most
  privacy units touched only one group.
- Total relative L1 error can hide aggressive group suppression. Utility evaluation must charge
  missing true groups as released zero and should also report per-group behavior.
- A fixed budget split or fixed `C_u` can make either mechanism look artificially strong.

These negative results are retained as design constraints, not as reproducible benchmark claims;
the exploratory harnesses that produced them were removed during repository cleanup.

## Maintained attack audit

The single maintained Python harness is:

```bash
python3 attacks/filterless_attacks.py
```

It contains two experiments:

1. A same-database, different-predicate scale diagnostic. This is not a neighboring-database DP
   test; it checks whether the filter changes the bound channel.
2. An add/remove sampled-PU test at a histogram support boundary. This compares the removed unsafe
   raw-histogram control with the current Laplace-noised histogram model.

Only the second private-histogram result is compared with the pure-DP membership-accuracy ceiling.
Failure to exceed that ceiling is a negative empirical result, not a proof of the full SQL
mechanism.

## Remaining obligations

- Prove the joint bound-selection and value-release mechanism for every supported aggregate type.
- Include returned values, selected groups, suppression, errors, and numeric saturation in the
  observable transcript.
- Validate contribution limits through joins and grouped plans at the privacy-unit level.
- Account for AVG as composed SUM and COUNT components.
- Treat metadata refresh and every fresh query as additional privacy expenditure.
- Evaluate utility on multiple datasets with independently tuned baselines.
