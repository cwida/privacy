-- Naive DP sample-and-aggregate: 64 hash-assigned lanes + dp_aggregate (median + smooth-sensitivity Laplace).
-- Structurally identical to the matching naive-PAC query; only the lane assignment and terminal differ.
-- Q4: AVG(UserID)
WITH samples AS (
    -- each privacy unit (UserID) assigned to ~8 of 64 lanes via an approximate SQL hash
    SELECT u.UserID AS pu_key, l.lane_id AS sample_id
    FROM (SELECT DISTINCT UserID FROM hits) u
    CROSS JOIN generate_series(0, 63) AS l(lane_id)
    WHERE (hash(u.UserID, l.lane_id) % 64) < 8
),
per_sample AS (
    SELECT s.sample_id, AVG(h.UserID) AS avg_uid
    FROM samples s JOIN hits h ON h.UserID = s.pu_key
    GROUP BY s.sample_id
)
SELECT dp_aggregate(array_agg(avg_uid::FLOAT ORDER BY sample_id), array_agg(0.0), 1.0, 1e-6, 8, -1e+19, 1e+19) AS avg_uid
FROM per_sample;
