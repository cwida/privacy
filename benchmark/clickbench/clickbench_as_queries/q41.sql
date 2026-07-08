SELECT URLHash, EventDate, as_noised_count(as_key) AS PageViews
FROM (
    SELECT URLHash, EventDate, priv_hash(hash(rowid)) AS as_key
    FROM hits
    WHERE CounterID = 62
      AND EventDate >= '2013-07-01'
      AND EventDate <= '2013-07-31'
      AND IsRefresh = 0
      AND TraficSourceID IN (-1, 6)
      AND RefererHash = 3594120000172545465
) h
GROUP BY URLHash, EventDate
ORDER BY PageViews DESC
LIMIT 10
OFFSET 100;
