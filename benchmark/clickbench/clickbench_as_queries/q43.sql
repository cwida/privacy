SELECT M, as_noised_count(as_key) AS PageViews
FROM (
    SELECT DATE_TRUNC('minute', EventTime) AS M, priv_hash(hash(rowid)) AS as_key
    FROM hits
    WHERE CounterID = 62
      AND EventDate >= '2013-07-14'
      AND EventDate <= '2013-07-15'
      AND IsRefresh = 0
      AND DontCountHits = 0
) h
GROUP BY M
ORDER BY M
LIMIT 10
OFFSET 1000;
