SELECT TraficSourceID, SearchEngineID, AdvEngineID, Src, Dst,
       as_noised_count(as_key) AS PageViews
FROM (
    SELECT TraficSourceID,
           SearchEngineID,
           AdvEngineID,
           CASE WHEN (SearchEngineID = 0 AND AdvEngineID = 0) THEN Referer ELSE '' END AS Src,
           URL AS Dst,
           priv_hash(hash(rowid)) AS as_key
    FROM hits
    WHERE CounterID = 62
      AND EventDate >= '2013-07-01'
      AND EventDate <= '2013-07-31'
      AND IsRefresh = 0
) h
GROUP BY TraficSourceID, SearchEngineID, AdvEngineID, Src, Dst
ORDER BY PageViews DESC
LIMIT 10
OFFSET 1000;
