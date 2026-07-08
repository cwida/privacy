SELECT SearchEngineID,
       ClientIP,
       as_noised_count(as_key) AS c,
       as_noised_sum(as_key, CAST(IsRefresh AS DOUBLE)) AS sum_refresh,
       as_noised_avg(as_key, ResolutionWidth) AS avg_rw
FROM (
    SELECT SearchEngineID, ClientIP, IsRefresh, ResolutionWidth, priv_hash(hash(rowid)) AS as_key
    FROM hits
    WHERE SearchPhrase <> ''
) h
GROUP BY SearchEngineID, ClientIP
ORDER BY c DESC
LIMIT 10;
