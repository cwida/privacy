-- Damico (2009), Table 2: one official BRR replicate; direct flag, no packed mask.
SELECT sum(p.rep_weight * p.expenditure) / nullif(sum(p.rep_weight), 0) AS mean_expenditure
FROM (SELECT p.*, 2.0 * p.weight * b.BRR@REPLICATE@ AS rep_weight
      FROM persons_input p JOIN brr b USING (person_id, panel)) p
WHERE p.age > 17 AND p.age < 65;
