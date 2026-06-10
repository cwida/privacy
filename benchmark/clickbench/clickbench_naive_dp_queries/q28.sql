-- Naive DP sample-and-aggregate: 64 hash-assigned lanes + dp_aggregate (median + smooth-sensitivity Laplace).
-- Structurally identical to the matching naive-PAC query; only the lane assignment and terminal differ.
-- Q28: CounterID, AVG(STRLEN(URL)), COUNT(*) WHERE URL<>'' GROUP BY CounterID HAVING c>100000 ORDER BY l DESC LIMIT 25
WITH samples AS (
    -- each privacy unit (UserID) assigned to ~8 of 64 lanes via an approximate SQL hash
    SELECT u.UserID AS pu_key, l.lane_id AS sample_id
    FROM (SELECT DISTINCT UserID FROM hits) u
    CROSS JOIN generate_series(0, 63) AS l(lane_id)
    WHERE (hash(u.UserID, l.lane_id) % 64) < 8
),
per_sample AS (
    SELECT s.sample_id, h.CounterID AS CounterID, AVG(STRLEN(h.URL)) AS l, COUNT(*) AS c
    FROM samples s JOIN hits h ON h.UserID = s.pu_key
    WHERE h.URL <> ''
    GROUP BY s.sample_id, CounterID
)
SELECT CounterID,
       dp_aggregate(array_agg(l::FLOAT ORDER BY sample_id), array_agg(0.0), 1.0, 1e-6, 8, 0.0, 1000000.0) AS l,
       dp_aggregate(array_agg((c * 8.0)::FLOAT ORDER BY sample_id), array_agg(0.0), 1.0, 1e-6, 8, 0.0, 1000000000.0) AS c
FROM per_sample
GROUP BY CounterID
HAVING c > 100000
ORDER BY l DESC
LIMIT 25;
