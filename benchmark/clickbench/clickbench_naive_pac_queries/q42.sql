-- Naive PAC sample-and-aggregate (explicit sample-table join + pac_aggregate terminal).
-- Q42: SELECT WindowClientWidth, WindowClientHeight, COUNT(*) AS PageViews FROM hits WHERE CounterID = 62 AND EventDate >= '2013-07-01' AND EventDate <= '2013-07-31' AND IsRefresh = 0 AND DontCountHits = 0 AND URLHash = 2868770270353813622 GROUP BY WindowClientWidth, WindowClientHeight ORDER BY PageViews DESC LIMIT 10 OFFSET 10000;
WITH samples AS (
    SELECT u.UserID AS pu_key, s.sample_id
    FROM (SELECT DISTINCT UserID FROM hits) u
    CROSS JOIN generate_series(1, 128) AS s(sample_id)
    WHERE random() < 0.5
),
per_sample AS (
    SELECT s.sample_id, h.WindowClientWidth, h.WindowClientHeight, COUNT(*) AS cnt
    FROM samples s JOIN hits h ON h.UserID = s.pu_key
    WHERE h.CounterID = 62 AND h.EventDate >= '2013-07-01' AND h.EventDate <= '2013-07-31'
      AND h.IsRefresh = 0 AND h.DontCountHits = 0 AND h.URLHash = 2868770270353813622
    GROUP BY s.sample_id, h.WindowClientWidth, h.WindowClientHeight
)
SELECT WindowClientWidth, WindowClientHeight,
       pac_aggregate(array_agg(cnt ORDER BY sample_id),
                     array_agg(cnt ORDER BY sample_id), 1.0/128, 3) AS PageViews
FROM per_sample
GROUP BY WindowClientWidth, WindowClientHeight
ORDER BY PageViews DESC LIMIT 10 OFFSET 10000;
