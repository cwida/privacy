SELECT URL, as_noised_count(as_key) AS PageViews
FROM (
    SELECT URL, priv_hash(hash(rowid)) AS as_key
    FROM hits
    WHERE CounterID = 62
      AND EventDate >= '2013-07-01'
      AND EventDate <= '2013-07-31'
      AND DontCountHits = 0
      AND IsRefresh = 0
      AND URL <> ''
) h
GROUP BY URL
ORDER BY PageViews DESC
LIMIT 10;
