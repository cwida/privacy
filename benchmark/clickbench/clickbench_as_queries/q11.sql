SELECT MobilePhoneModel, as_noised_count(as_key) AS u
FROM (
    SELECT MobilePhoneModel, UserID, priv_hash(hash(min(rowid))) AS as_key
    FROM hits
    WHERE MobilePhoneModel <> '' AND UserID IS NOT NULL
    GROUP BY MobilePhoneModel, UserID
) h
GROUP BY MobilePhoneModel
ORDER BY u DESC
LIMIT 10;
