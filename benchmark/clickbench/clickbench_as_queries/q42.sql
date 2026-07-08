SELECT WindowClientWidth, WindowClientHeight, as_noised_count(as_key) AS PageViews
FROM (
    SELECT WindowClientWidth, WindowClientHeight, priv_hash(hash(rowid)) AS as_key
    FROM hits
    WHERE CounterID = 62
      AND EventDate >= '2013-07-01'
      AND EventDate <= '2013-07-31'
      AND IsRefresh = 0
      AND DontCountHits = 0
      AND URLHash = 2868770270353813622
) h
GROUP BY WindowClientWidth, WindowClientHeight
ORDER BY PageViews DESC
LIMIT 10
OFFSET 10000;
