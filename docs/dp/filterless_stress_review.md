# Filterless implementation stress review

> **2026-08-28 update:** The reviewed revision's deterministic-RNG and inverse-CDF floating-point
> blockers are now fixed by fresh operating-system entropy and granular Laplace sampling. Stable
> noise nonces and their SASS identity window were removed. The remaining production blockers in
> this review—analyst-accessible noise disabling/raw bounds and missing transcript accounting—still
> apply.

## TL;DR

- Tuple selection, per-PU pre-aggregation, group capping, supported aggregates, and the physical plan matched the intended design in the tested cases.
- The idealized mechanism with a noised histogram was consistent with epsilon 0.5 in a held-out support-boundary audit. This is evidence against that counterexample, not a proof of DP.
- The current implementation cannot yet make an end-to-end information-theoretic DP claim. Its noise is deterministically seeded from a query counter or the user-selectable `privacy_seed`, and `privacy_noise=false` can disable it.
- The optional raw-histogram mode decisively failed the same epsilon 0.5 audit: 97.19% membership accuracy and a 99%-confidence empirical epsilon lower bound of 4.44.
- Fixed sampling is important for performance. On the rare-filter workload, `p=6` was 6.5x faster than full `p=0` for the ungrouped query and 21x faster for the grouped query, but remained 2.2x-3x slower than `dp_standard`.
- All 3,635 SQLLogicTest assertions and the C++ extension test runner passed.

This review used branch `filterless` on 2026-08-27. It tests implementation behavior; it does not replace a proof, a cryptographic RNG review, or an accountant.

## 1. Correctness and tuple selection

The main relational test used 10,000 privacy units with three event rows each. Exactly 3,000 rows passed the predicate. With `dp_filterless_sample_bits=6`, the independent raw-hash calculation and `EXPLAIN ANALYZE` agreed exactly:

| Stage | Rows or PUs |
|---|---:|
| Input scan | 30,000 rows |
| Predicate-active rows | 3,000 rows |
| Sampled PUs | 162 PUs |
| Retained by `X OR sampled(PU)` | 3,439 rows |
| Logical PUs after per-PU pre-aggregation | 3,115 PUs |

The 3,439 retained rows equal all 3,000 active rows plus the inactive rows belonging to sampled PUs. The 3,115 logical PUs equal 3,000 active PUs plus 115 sampled PUs that had no active row.

With noise disabled for deterministic correctness checks:

- `SUM`, `COUNT(*)`, `COUNT(nullable_column)`, and `AVG` exactly matched the raw filtered query for both `p=0` and `p=6` when the selected bound did not clip the input;
- NULL predicates were treated as inactive;
- an empty filter returned `SUM=0`, `COUNT=0`, and `AVG=NULL`;
- an implicit two-hop `PRIVACY_LINK` chain and the equivalent explicit join both returned `SUM=3600`, `COUNT=1800`;
- a public side-table join preserved the seven expected groups; and
- `dp_max_groups_contributed=1` matched an independent `DENSE_RANK() OVER (PARTITION BY pu ORDER BY group_key)` reference exactly.

The relevant rewrite is centralized in `BuildFilterlessEncodedPuInput`, lower answer/histogram construction, and `ApplyFilterlessMaxGroups` in `src/compiler/privacy_mechanisms.cpp`.

## 2. Plan shape

The ungrouped optimized plan had the intended order:

```text
scan
  -> materialize PU hash and original predicate
  -> filter: X OR sampled(PU)
  -> encode predicate in PU bit 0
  -> group by logical PU
       answer partials FILTER active
       histogram partials FILTER sampled
  -> filterless SUM/COUNT aggregates
  -> AVG ratio projection
```

The grouped plan additionally contained:

```text
per-(group, PU) lower aggregate
  -> DENSE_RANK partitioned by logical PU
  -> rank <= dp_max_groups_contributed
  -> per-group filterless aggregates + active-PU support count
  -> noised Wilson partition-selection threshold
  -> final projections / AVG ratio
```

The plan used `filterless_approx_sum` for floating per-PU SUMs, so ordinary order-dependent `DOUBLE SUM` did not run before clipping. Component budgets also matched the source calculation: visible aggregates split epsilon equally, AVG split its share between SUM and COUNT, and grouped queries reserved one additional share for partition selection.

## 3. Supported-query matrix

The implementation matched the currently documented surface:

| Query shape | Result |
|---|---|
| `SUM`, `COUNT`, `AVG`, including mixed expressions | supported |
| Multiple visible aggregates | supported |
| `WHERE` with multiple conjuncts | supported |
| No `WHERE` | supported |
| Empty filtered input | supported |
| `GROUP BY`, `HAVING`, `ORDER BY`, `LIMIT` | supported |
| Direct PU join | supported |
| Implicit and explicit linear two-hop FK chains | supported |
| Public side-table join | supported |
| Aggregate-local `FILTER` | explicitly rejected |
| `MIN` / `MAX` | explicitly rejected |
| `COUNT(DISTINCT)` and other `DISTINCT` aggregates | rejected |
| Volatile predicate | explicitly rejected |
| More than one input filter node | explicitly rejected |
| CTEs and subqueries | explicitly rejected by the shared DP shape check |
| Protected group key | explicitly rejected |
| Accessible self-join | explicitly rejected |

One diagnostic is misleading: filterless `COUNT(DISTINCT x)` is rejected with “only `COUNT(DISTINCT x)` is supported for DISTINCT.” The rejection is correct, but the message incorrectly suggests that filterless supports the query.

## 4. Privacy audit

### Claim tested

- Privacy unit: one logical PU.
- Adjacency: add or remove one PU.
- Ungrouped definition: one pure epsilon-DP release.
- Grouped definition: one `(epsilon, delta)` release including the selected group keys.
- Transcript: returned values, NULL/empty results, released group keys, errors, deterministic-seed behavior, and repeated queries.
- Required mechanism configuration: histogram noise enabled, value noise enabled, finite contribution/group bounds, and correct composition across visible aggregates and partition selection.

### Idealized budget and sensitivity review

For one component, a sampled PU changes one histogram bin in each of at most `C_u` groups by weight `2^p`. The implemented histogram sensitivity is therefore `2^p * C_u`. Given a privately selected bound `U`, the clipped value channel has add/remove sensitivity `U * C_u`. Splitting the component budget between those two mechanisms and composing them is sound under an ideal DP noise mechanism. The grouped active-support mechanism separately uses sensitivity `C_u` and the Wilson partition-selection threshold.

The new noise-aware support threshold is post-processing of the noised histogram. It changes false-bound utility, not the histogram's privacy cost.

### Held-out empirical audit

The implementation was sampled on neighboring inputs at a bound-selection knife edge:

- total epsilon `0.5`;
- histogram/value split `0.125 / 0.375`;
- 300 active low-bin background PUs;
- 138 inactive high-bin background PUs;
- neighboring world adds one active high-bin PU;
- 20,000 samples per world, split equally into event discovery and confirmation; and
- the attacker observed the final scalar release, not the internal bound.

Distinct aggregate nonces were treated as pseudorandom draws to exercise the idealized randomized code path. The confidence calculation assumes independent draws, so it is a diagnostic of the mechanism logic rather than a repair for the deterministic-seeding blocker below.

| Configuration | Held-out accuracy | Point empirical epsilon | 99% epsilon lower bound | epsilon-0.5 ceiling |
|---|---:|---:|---:|---:|
| Noised histogram | 57.76% | 0.326 | 0.267 | 62.25% |
| Raw histogram | 97.19% | 4.742 | 4.437 | 62.25% |

The noised result is expected inference within the tested budget. It does not prove DP. The raw-histogram result is a statistically significant contradiction of epsilon 0.5 for that configuration.

### End-to-end blockers

1. **Deterministic noise is a formal blocker.** `GetDpNoiseSeed` uses either the user-selectable `privacy_seed` or a deterministic active-query counter, then samples with `std::mt19937_64`. Three fresh processes executing the same query sequence returned the identical result `11995.600975172258`. A fixed public seed makes a nonconstant mechanism deterministic, which cannot satisfy finite information-theoretic DP on neighboring data.
2. **Noise can be disabled.** `privacy_noise=false` removes histogram, value, and partition-selection noise. This must be test-only and inaccessible to an analyst in any DP deployment.
3. **Raw bounds can be selected.** `dp_filterless_noise_bounds=false` is currently the default and failed the audit above. This option must be removed rather than documented as a safe DP configuration.
4. **There is no session accountant.** Syntactically different queries receive different sticky-noise nonces. Repetition is ordinary sequential composition, but the extension does not enforce a transcript budget. A one-query claim must not be presented as a session guarantee.
5. **Finite-precision Laplace remains unproved.** Inverse-CDF `DOUBLE` Laplace sampling has the known finite-support/rounding concerns described by Mironov. Approximate SUM fixes input-order arithmetic; it does not fix the noise sampler.

Primary references for these obligations include [Dwork et al. on the Laplace mechanism](https://people.csail.mit.edu/asmith/PS/sensitivity-tcc-final.pdf), [Mironov on floating-point DP](https://www.microsoft.com/en-us/research/wp-content/uploads/2012/10/lsbs.pdf), and [Kifer et al. on implementation auditing](https://arxiv.org/abs/2002.04049).

## 5. Performance

The end-to-end synthetic benchmark used 2,000,000 rows, 100,000 PUs, 20 rows per PU, four DuckDB threads, and a 1%-PU rare predicate. Medians exclude one warm-up run. DP rows used value noise; filterless also used histogram noise.

| Query | Raw SQL | `dp_standard` | Filterless `p=6` | Filterless `p=0` |
|---|---:|---:|---:|---:|
| Rare ungrouped `SUM, COUNT` | 1 ms | 2 ms | 6 ms | 39 ms |
| Rare grouped `SUM, COUNT`, 1,000 groups | 2 ms | 10 ms | 22 ms | 466 ms |

For the ungrouped query, sampling made filterless 6.5x faster than full `p=0`; for the grouped query it was about 21x faster. It remained about 3x slower than `dp_standard` ungrouped and 2.2x slower grouped. On a broad 50% predicate, filterless `p=6` took about 20 ms versus 2 ms raw, because qualifying rows cannot be sampled away.

These numbers are workload- and compression-specific. They show the execution envelope, not a general speedup claim. The main costs are scanning the base relation, retaining every qualifying row, per-PU/group pre-aggregation, the dense-rank group cap, and per-cell histogram noise. Fine grouping and broad filters are the worst cases.

## 6. Tests run

- Ninja release build.
- `test/sql/dp_filterless.test`: 135 assertions.
- Complete SQL suite: 3,635 assertions across 34 cases.
- C++ extension runner: compiler, metadata, traversal, parser, and serialization tests all passed.
- Real correctness, plan, support-matrix, neighboring-dataset, and runtime queries described above.

## TODO

- **P0: remove `dp_filterless_noise_bounds`.** Always privatize private histogram support, always reserve the configured bounds-budget fraction, delete the raw branch/default, update documentation, and extend `test/sql/dp_filterless.test` so a raw-histogram configuration cannot be reintroduced.
- **P0: replace deterministic query-counter/analyst-seeded DP noise.** Use service-owned unpredictable entropy and a DP-safe sampler. Keep deterministic injection only in a test-only interface that is unavailable to analysts.
- **P0: make `privacy_noise=false` inaccessible in production DP sessions.**
- **P1: define and enforce transcript accounting.** State whether epsilon applies to one query or a session, then charge repeated and adaptive queries accordingly.
- **P1: resolve the finite-precision proof obligation** with a discrete/snapped mechanism or an implementation whose finite arithmetic has a reviewed guarantee.
- **P2: add persistent plan-shape and supported-query matrix tests**, including the two-hop chain and the sampled tuple-count invariant.
- **P2: correct the filterless `COUNT(DISTINCT)` rejection message.**
- **P2: add a repeatable end-to-end filterless benchmark** with rare, broad, grouped, and no-filter workloads; track `p=0`, sampled filterless, `dp_standard`, wall time, and retained-row counts.
