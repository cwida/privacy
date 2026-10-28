-- AHRQ use_events_2016: one official BRR replicate; direct flag, no packed mask.
WITH visits AS (
  SELECT person_id, count(*) AS n,
         count(*) FILTER (WHERE physician = 1) AS n_physician
  FROM events WHERE kind = 'OB' GROUP BY person_id
)
SELECT sum(p.rep_weight * coalesce(v.n, 0)) / nullif(sum(p.rep_weight), 0) AS mean_office_visits,
       sum(p.rep_weight * coalesce(v.n_physician, 0)) / nullif(sum(p.rep_weight), 0) AS mean_office_physician_visits
FROM (SELECT p.*, 2.0 * p.weight * b.BRR@REPLICATE@ AS rep_weight
      FROM persons_input p JOIN brr b USING (person_id, panel)) p LEFT JOIN visits v USING (person_id)
WHERE true;
