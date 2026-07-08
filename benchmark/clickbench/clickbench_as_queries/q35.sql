SELECT 1, URL, as_noised_count(as_key) AS c
FROM (
    SELECT URL, priv_hash(hash(rowid)) AS as_key
    FROM hits
) h
GROUP BY URL
ORDER BY c DESC
LIMIT 10;
