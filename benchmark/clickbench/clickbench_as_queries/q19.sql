SELECT UserID, m, SearchPhrase, as_noised_count(as_key) AS c
FROM (
    SELECT UserID, extract(minute FROM EventTime) AS m, SearchPhrase, priv_hash(hash(rowid)) AS as_key
    FROM hits
) h
GROUP BY UserID, m, SearchPhrase
ORDER BY c DESC
LIMIT 10;
