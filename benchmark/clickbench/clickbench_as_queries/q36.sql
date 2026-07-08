SELECT ClientIP, ClientIP - 1, ClientIP - 2, ClientIP - 3, as_noised_count(as_key) AS c
FROM (
    SELECT ClientIP, priv_hash(hash(rowid)) AS as_key
    FROM hits
) h
GROUP BY ClientIP, ClientIP - 1, ClientIP - 2, ClientIP - 3
ORDER BY c DESC
LIMIT 10;
