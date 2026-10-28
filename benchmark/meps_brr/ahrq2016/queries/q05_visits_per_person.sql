-- AHRQ use_events_2016: full-sample SQL; not a repeated-estimation baseline.
WITH visits AS (
  SELECT person_id, count(*) AS n,
         count(*) FILTER (WHERE physician = 1) AS n_physician
  FROM events WHERE kind = 'OB' GROUP BY person_id
)
SELECT sum(p.weight * coalesce(v.n, 0)) / nullif(sum(p.weight), 0) AS mean_office_visits,
       sum(p.weight * coalesce(v.n_physician, 0)) / nullif(sum(p.weight), 0) AS mean_office_physician_visits
FROM persons_input p LEFT JOIN visits v USING (person_id)
WHERE true;
