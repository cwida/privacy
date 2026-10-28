-- AHRQ use_events_2016: one official BRR replicate; direct flag, no packed mask.
SELECT sum(p.rep_weight) AS event_count,
       sum(p.rep_weight * e.self) / nullif(sum(p.rep_weight), 0) AS mean_self,
       sum(p.rep_weight * e.private) / nullif(sum(p.rep_weight), 0) AS mean_private,
       sum(p.rep_weight * e.medicare) / nullif(sum(p.rep_weight), 0) AS mean_medicare,
       sum(p.rep_weight * e.medicaid) / nullif(sum(p.rep_weight), 0) AS mean_medicaid,
       sum(p.rep_weight * e.other) / nullif(sum(p.rep_weight), 0) AS mean_other
FROM (SELECT p.*, 2.0 * p.weight * b.BRR@REPLICATE@ AS rep_weight
      FROM persons_input p JOIN brr b USING (person_id, panel)) p JOIN events e USING (person_id)
WHERE e.kind = 'OB';
