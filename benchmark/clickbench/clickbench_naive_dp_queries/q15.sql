-- Naive DP sample-and-aggregate: 64 hash-assigned lanes + dp_aggregate (median + smooth-sensitivity Laplace).
-- Structurally identical to the matching naive-PAC query; only the lane assignment and terminal differ.
-- Q15: SearchEngineID, SearchPhrase, COUNT(*) WHERE SearchPhrase<>'' GROUP BY ... ORDER BY c DESC LIMIT 10
WITH samples AS (
    -- each privacy unit (UserID) assigned to ~8 of 64 lanes via an approximate SQL hash
    SELECT u.UserID AS pu_key, l.lane_id AS sample_id
    FROM (SELECT DISTINCT UserID FROM hits) u
    CROSS JOIN generate_series(0, 63) AS l(lane_id)
    WHERE (hash(u.UserID, l.lane_id) % 64) < 8
),
per_sample AS (
    SELECT s.sample_id, h.SearchEngineID AS SearchEngineID, h.SearchPhrase AS SearchPhrase, COUNT(*) AS c
    FROM samples s JOIN hits h ON h.UserID = s.pu_key
    WHERE h.SearchPhrase <> ''
    GROUP BY s.sample_id, SearchEngineID, SearchPhrase
)
SELECT SearchEngineID,
       SearchPhrase,
       dp_aggregate(array_agg((c * 8.0)::FLOAT ORDER BY sample_id), array_agg(0.0), 1.0, 1e-6, 8, 0.0, 1000000000.0) AS c
FROM per_sample
GROUP BY SearchEngineID, SearchPhrase
ORDER BY c DESC
LIMIT 10;
