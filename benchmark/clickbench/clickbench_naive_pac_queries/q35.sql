-- Naive PAC sample-and-aggregate (explicit sample-table join + pac_aggregate terminal).
-- Q35: SELECT 1, URL, COUNT(*) AS c FROM hits GROUP BY 1, URL ORDER BY c DESC LIMIT 10;
WITH samples AS (
    SELECT u.UserID AS pu_key, s.sample_id
    FROM (SELECT DISTINCT UserID FROM hits) u
    CROSS JOIN generate_series(1, 128) AS s(sample_id)
    WHERE random() < 0.5
),
per_sample AS (
    SELECT s.sample_id, h.URL, COUNT(*) AS cnt
    FROM samples s JOIN hits h ON h.UserID = s.pu_key
    GROUP BY s.sample_id, h.URL
)
SELECT 1, URL,
       pac_aggregate(array_agg(cnt ORDER BY sample_id),
                     array_agg(cnt ORDER BY sample_id), 1.0/128, 3) AS c
FROM per_sample
GROUP BY URL
ORDER BY c DESC LIMIT 10;
