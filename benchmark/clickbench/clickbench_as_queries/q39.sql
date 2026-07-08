SELECT URL, as_noised_count(as_key) AS PageViews
FROM (
    SELECT URL, priv_hash(hash(rowid)) AS as_key
    FROM hits
    WHERE CounterID = 62
      AND EventDate >= '2013-07-01'
      AND EventDate <= '2013-07-31'
      AND IsRefresh = 0
      AND IsLink <> 0
      AND IsDownload = 0
) h
GROUP BY URL
ORDER BY PageViews DESC
LIMIT 10
OFFSET 1000;
