SELECT AdvEngineID, as_noised_count(as_key) AS c
FROM (
    SELECT AdvEngineID, priv_hash(hash(rowid)) AS as_key
    FROM hits
    WHERE AdvEngineID <> 0
) h
GROUP BY AdvEngineID
ORDER BY c DESC;
