SELECT UserID, as_noised_count(as_key) AS c
FROM (
    SELECT UserID, priv_hash(hash(rowid)) AS as_key
    FROM hits
) h
GROUP BY UserID
ORDER BY c DESC
LIMIT 10;
