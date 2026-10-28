-- AHRQ use_events_2016: explicit AS rewrite; masks constructed in the timed query.
WITH visits AS (
  SELECT person_id, count(*) AS n,
         count(*) FILTER (WHERE physician = 1) AS n_physician
  FROM events WHERE kind = 'OB' GROUP BY person_id
)
SELECT list_transform(list_zip(as_sum(p.mask_lo, p.weight * coalesce(v.n, 0)), as_sum(p.mask_lo, p.weight)),
    x -> x[1] / nullif(x[2], 0)) AS mean_office_visits_lo,
       list_transform(list_zip(as_sum(p.mask_hi, p.weight * coalesce(v.n, 0)), as_sum(p.mask_hi, p.weight)),
    x -> x[1] / nullif(x[2], 0)) AS mean_office_visits_hi,
       list_transform(list_zip(as_sum(p.mask_lo, p.weight * coalesce(v.n_physician, 0)), as_sum(p.mask_lo, p.weight)),
    x -> x[1] / nullif(x[2], 0)) AS mean_office_physician_visits_lo,
       list_transform(list_zip(as_sum(p.mask_hi, p.weight * coalesce(v.n_physician, 0)), as_sum(p.mask_hi, p.weight)),
    x -> x[1] / nullif(x[2], 0)) AS mean_office_physician_visits_hi
FROM persons_as p LEFT JOIN visits v USING (person_id)
WHERE true;
