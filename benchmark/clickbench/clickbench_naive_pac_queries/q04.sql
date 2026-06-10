-- Naive PAC sample-and-aggregate (explicit sample-table join + pac_aggregate terminal).
-- Q4: SELECT AVG(UserID) FROM hits;
WITH samples AS (
    SELECT u.UserID AS pu_key, s.sample_id
    FROM (SELECT DISTINCT UserID FROM hits) u
    CROSS JOIN generate_series(1, 128) AS s(sample_id)
    WHERE random() < 0.5
),
per_sample AS (
    SELECT s.sample_id, AVG(h.UserID) AS avg_uid, COUNT(*) AS cnt
    FROM samples s JOIN hits h ON h.UserID = s.pu_key
    GROUP BY s.sample_id
)
SELECT pac_aggregate(array_agg(avg_uid ORDER BY sample_id),
                     array_agg(cnt ORDER BY sample_id), 1.0/128, 3) AS avg_uid
FROM per_sample;
