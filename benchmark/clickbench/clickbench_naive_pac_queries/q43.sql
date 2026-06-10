-- Naive PAC sample-and-aggregate (explicit sample-table join + pac_aggregate terminal).
-- Q43: SELECT DATE_TRUNC('minute', EventTime) AS M, COUNT(*) AS PageViews FROM hits WHERE CounterID = 62 AND EventDate >= '2013-07-14' AND EventDate <= '2013-07-15' AND IsRefresh = 0 AND DontCountHits = 0 GROUP BY DATE_TRUNC('minute', EventTime) ORDER BY DATE_TRUNC('minute', EventTime) LIMIT 10 OFFSET 1000;
WITH samples AS (
    SELECT u.UserID AS pu_key, s.sample_id
    FROM (SELECT DISTINCT UserID FROM hits) u
    CROSS JOIN generate_series(1, 128) AS s(sample_id)
    WHERE random() < 0.5
),
per_sample AS (
    SELECT s.sample_id, DATE_TRUNC('minute', h.EventTime) AS M, COUNT(*) AS cnt
    FROM samples s JOIN hits h ON h.UserID = s.pu_key
    WHERE h.CounterID = 62 AND h.EventDate >= '2013-07-14' AND h.EventDate <= '2013-07-15'
      AND h.IsRefresh = 0 AND h.DontCountHits = 0
    GROUP BY s.sample_id, M
)
SELECT M,
       pac_aggregate(array_agg(cnt ORDER BY sample_id),
                     array_agg(cnt ORDER BY sample_id), 1.0/128, 3) AS PageViews
FROM per_sample
GROUP BY M
ORDER BY M LIMIT 10 OFFSET 1000;
