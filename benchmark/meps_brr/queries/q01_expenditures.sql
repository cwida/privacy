-- AHRQ exercise_1a: population totals, means, prevalence, and mean among spenders.
SELECT p.person_id, p.mask_lo, p.mask_hi, m.metric, m.numerator, m.denominator, m.is_ratio
FROM persons p,
LATERAL (VALUES
 ('total_expenditure', p.weight * p.expenditure, 0.0, false),
 ('mean_expenditure', p.weight * p.expenditure, p.weight, true),
 ('percent_with_expenditure', 100.0 * p.weight * (p.expenditure > 0)::INTEGER, p.weight, true),
 ('mean_among_spenders', p.weight * p.expenditure,
     CASE WHEN p.expenditure > 0 THEN p.weight ELSE 0 END, true)
) m(metric, numerator, denominator, is_ratio)
