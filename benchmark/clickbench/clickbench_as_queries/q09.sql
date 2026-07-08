SELECT RegionID, as_noised_count(as_key) AS u
FROM (
    SELECT RegionID, UserID, priv_hash(hash(min(rowid))) AS as_key
    FROM hits
    WHERE UserID IS NOT NULL
    GROUP BY RegionID, UserID
) h
GROUP BY RegionID
ORDER BY u DESC
LIMIT 10;
