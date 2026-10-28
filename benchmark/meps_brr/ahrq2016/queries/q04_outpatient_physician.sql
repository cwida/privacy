-- AHRQ use_events_2016: full-sample SQL; not a repeated-estimation baseline.
SELECT sum(p.weight) AS event_count,
       sum(p.weight * e.self) / nullif(sum(p.weight), 0) AS mean_self,
       sum(p.weight * e.private) / nullif(sum(p.weight), 0) AS mean_private,
       sum(p.weight * e.medicare) / nullif(sum(p.weight), 0) AS mean_medicare,
       sum(p.weight * e.medicaid) / nullif(sum(p.weight), 0) AS mean_medicaid,
       sum(p.weight * e.other) / nullif(sum(p.weight), 0) AS mean_other
FROM persons_input p JOIN events e USING (person_id)
WHERE e.kind = 'OP' AND e.physician = 1;
