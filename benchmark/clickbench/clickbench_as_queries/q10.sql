WITH value_agg AS (
    SELECT RegionID,
           as_noised_sum(as_key, AdvEngineID) AS sum_adv,
           as_noised_count(as_key) AS c,
           as_noised_avg(as_key, ResolutionWidth) AS avg_rw
    FROM (
        SELECT RegionID, AdvEngineID, ResolutionWidth, priv_hash(hash(rowid)) AS as_key
        FROM hits
    ) h
    GROUP BY RegionID
),
distinct_users AS (
    SELECT RegionID, as_noised_count(as_key) AS u
    FROM (
        SELECT RegionID, UserID, priv_hash(hash(min(rowid))) AS as_key
        FROM hits
        WHERE UserID IS NOT NULL
        GROUP BY RegionID, UserID
    ) h
    GROUP BY RegionID
)
SELECT value_agg.RegionID, sum_adv, c, avg_rw, u
FROM value_agg
JOIN distinct_users USING (RegionID)
ORDER BY c DESC
LIMIT 10;
