SELECT as_noised_count(as_key) AS c
FROM (
    SELECT UserID, priv_hash(hash(min(rowid))) AS as_key
    FROM hits
    WHERE UserID IS NOT NULL
    GROUP BY UserID
) h;
