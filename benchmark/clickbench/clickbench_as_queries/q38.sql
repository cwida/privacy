SELECT Title, as_noised_count(as_key) AS PageViews
FROM (
    SELECT Title, priv_hash(hash(rowid)) AS as_key
    FROM hits
    WHERE CounterID = 62
      AND EventDate >= '2013-07-01'
      AND EventDate <= '2013-07-31'
      AND DontCountHits = 0
      AND IsRefresh = 0
      AND Title <> ''
) h
GROUP BY Title
ORDER BY PageViews DESC
LIMIT 10;
