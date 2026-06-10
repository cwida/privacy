-- Naive PAC sample-and-aggregate (explicit sample-table join + pac_aggregate terminal).
-- Q3: SELECT SUM(AdvEngineID), COUNT(*), AVG(ResolutionWidth) FROM hits;
WITH samples AS (
    SELECT u.UserID AS pu_key, s.sample_id
    FROM (SELECT DISTINCT UserID FROM hits) u
    CROSS JOIN generate_series(1, 128) AS s(sample_id)
    WHERE random() < 0.5
),
per_sample AS (
    SELECT s.sample_id,
           SUM(h.AdvEngineID) AS sum_adv,
           COUNT(*) AS cnt,
           AVG(h.ResolutionWidth) AS avg_rw
    FROM samples s JOIN hits h ON h.UserID = s.pu_key
    GROUP BY s.sample_id
)
SELECT pac_aggregate(array_agg(sum_adv ORDER BY sample_id), array_agg(cnt ORDER BY sample_id), 1.0/128, 3) AS sum_adv,
       pac_aggregate(array_agg(cnt ORDER BY sample_id), array_agg(cnt ORDER BY sample_id), 1.0/128, 3) AS c,
       pac_aggregate(array_agg(avg_rw ORDER BY sample_id), array_agg(cnt ORDER BY sample_id), 1.0/128, 3) AS avg_rw
FROM per_sample;
