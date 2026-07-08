SELECT UserID, SearchPhrase, as_noised_count(as_key) AS c
FROM (
    SELECT UserID, SearchPhrase, priv_hash(hash(rowid)) AS as_key
    FROM hits
) h
GROUP BY UserID, SearchPhrase
ORDER BY c DESC
LIMIT 10;
