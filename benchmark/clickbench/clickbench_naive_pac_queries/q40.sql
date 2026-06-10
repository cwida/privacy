-- Naive PAC sample-and-aggregate (explicit sample-table join + pac_aggregate terminal).
-- Q40: SELECT TraficSourceID, SearchEngineID, AdvEngineID, CASE WHEN (SearchEngineID = 0 AND AdvEngineID = 0) THEN Referer ELSE '' END AS Src, URL AS Dst, COUNT(*) AS PageViews FROM hits WHERE CounterID = 62 AND EventDate >= '2013-07-01' AND EventDate <= '2013-07-31' AND IsRefresh = 0 GROUP BY TraficSourceID, SearchEngineID, AdvEngineID, Src, Dst ORDER BY PageViews DESC LIMIT 10 OFFSET 1000;
WITH samples AS (
    SELECT u.UserID AS pu_key, s.sample_id
    FROM (SELECT DISTINCT UserID FROM hits) u
    CROSS JOIN generate_series(1, 128) AS s(sample_id)
    WHERE random() < 0.5
),
per_sample AS (
    SELECT s.sample_id, h.TraficSourceID, h.SearchEngineID, h.AdvEngineID,
           CASE WHEN (h.SearchEngineID = 0 AND h.AdvEngineID = 0) THEN h.Referer ELSE '' END AS Src,
           h.URL AS Dst, COUNT(*) AS cnt
    FROM samples s JOIN hits h ON h.UserID = s.pu_key
    WHERE h.CounterID = 62 AND h.EventDate >= '2013-07-01' AND h.EventDate <= '2013-07-31' AND h.IsRefresh = 0
    GROUP BY s.sample_id, h.TraficSourceID, h.SearchEngineID, h.AdvEngineID, Src, Dst
)
SELECT TraficSourceID, SearchEngineID, AdvEngineID, Src, Dst,
       pac_aggregate(array_agg(cnt ORDER BY sample_id),
                     array_agg(cnt ORDER BY sample_id), 1.0/128, 3) AS PageViews
FROM per_sample
GROUP BY TraficSourceID, SearchEngineID, AdvEngineID, Src, Dst
ORDER BY PageViews DESC LIMIT 10 OFFSET 1000;
