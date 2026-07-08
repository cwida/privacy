SELECT SearchEngineID, SearchPhrase, as_noised_count(as_key) AS c
FROM (
    SELECT SearchEngineID, SearchPhrase, priv_hash(hash(rowid)) AS as_key
    FROM hits
    WHERE SearchPhrase <> ''
) h
GROUP BY SearchEngineID, SearchPhrase
ORDER BY c DESC
LIMIT 10;
