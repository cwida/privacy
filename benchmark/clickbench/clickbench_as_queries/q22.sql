SELECT SearchPhrase, MIN(URL), as_noised_count(as_key) AS c
FROM (
    SELECT SearchPhrase, URL, priv_hash(hash(rowid)) AS as_key
    FROM hits
    WHERE URL LIKE '%google%' AND SearchPhrase <> ''
) h
GROUP BY SearchPhrase
ORDER BY c DESC
LIMIT 10;
