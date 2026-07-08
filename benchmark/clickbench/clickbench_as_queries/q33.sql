SELECT WatchID,
       ClientIP,
       as_noised_count(as_key) AS c,
       as_noised_sum(as_key, CAST(IsRefresh AS DOUBLE)) AS sum_refresh,
       as_noised_avg(as_key, ResolutionWidth) AS avg_rw
FROM (
    SELECT WatchID, ClientIP, IsRefresh, ResolutionWidth, priv_hash(hash(rowid)) AS as_key
    FROM hits
) h
GROUP BY WatchID, ClientIP
ORDER BY c DESC
LIMIT 10;
