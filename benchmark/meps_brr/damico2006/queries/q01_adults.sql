-- Damico (2009), Table 2: full-sample SQL; not a repeated-estimation baseline.
SELECT sum(p.weight * p.expenditure) / nullif(sum(p.weight), 0) AS mean_expenditure
FROM persons_input p
WHERE p.age > 17;
