-- Damico (2009), Table 2: explicit AS rewrite; masks constructed in the timed query.
SELECT list_transform(list_zip(as_sum(p.mask_lo, p.weight * p.expenditure), as_sum(p.mask_lo, p.weight)),
    x -> x[1] / nullif(x[2], 0)) AS mean_expenditure_lo
FROM persons_as p
WHERE p.age > 17;
