-- Naive DP sample-and-aggregate: 64 hash-assigned lanes + dp_aggregate (median + smooth-sensitivity Laplace).
-- Structurally identical to the matching naive-PAC query; only the lane assignment and terminal differ.
-- Q39: URL, COUNT(*) PageViews [...IsLink<>0,IsDownload=0] GROUP BY URL ORDER BY PageViews DESC LIMIT 10 OFFSET 1000
WITH samples AS (
    -- each privacy unit (UserID) assigned to ~8 of 64 lanes via an approximate SQL hash
    SELECT u.UserID AS pu_key, l.lane_id AS sample_id
    FROM (SELECT DISTINCT UserID FROM hits) u
    CROSS JOIN generate_series(0, 63) AS l(lane_id)
    WHERE (hash(u.UserID, l.lane_id) % 64) < 8
),
per_sample AS (
    SELECT s.sample_id, h.URL AS URL, COUNT(*) AS PageViews
    FROM samples s JOIN hits h ON h.UserID = s.pu_key
    WHERE h.CounterID = 62 AND h.EventDate >= '2013-07-01' AND h.EventDate <= '2013-07-31' AND h.IsRefresh = 0 AND h.IsLink <> 0 AND h.IsDownload = 0
    GROUP BY s.sample_id, URL
)
SELECT URL,
       dp_aggregate(array_agg((PageViews * 8.0)::FLOAT ORDER BY sample_id), array_agg(0.0), 1.0, 1e-6, 8, 0.0, 1000000000.0) AS PageViews
FROM per_sample
GROUP BY URL
ORDER BY PageViews DESC
LIMIT 10 OFFSET 1000;
