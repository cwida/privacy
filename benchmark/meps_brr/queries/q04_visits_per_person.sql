-- Retain persons without office-based visits in the denominator.
WITH visits AS (
 SELECT person_id, count(*) AS n, count(*) FILTER (WHERE physician = 1) AS n_physician
 FROM events WHERE kind = 'OB' GROUP BY person_id
)
SELECT p.person_id, p.mask_lo, p.mask_hi, m.metric, p.weight * m.n AS numerator,
       p.weight AS denominator, true AS is_ratio
FROM persons p LEFT JOIN visits v USING (person_id),
LATERAL (VALUES ('mean_office_visits', coalesce(v.n, 0)),
                ('mean_office_physician_visits', coalesce(v.n_physician, 0))) m(metric, n)
