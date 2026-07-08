SELECT as_noised_avg(as_key, CAST(UserID AS DOUBLE)) AS avg_uid
FROM (
    SELECT UserID, priv_hash(hash(rowid)) AS as_key
    FROM hits
) h;
