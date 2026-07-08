SELECT MobilePhone, MobilePhoneModel, as_noised_count(as_key) AS u
FROM (
    SELECT MobilePhone, MobilePhoneModel, UserID, priv_hash(hash(min(rowid))) AS as_key
    FROM hits
    WHERE MobilePhoneModel <> '' AND UserID IS NOT NULL
    GROUP BY MobilePhone, MobilePhoneModel, UserID
) h
GROUP BY MobilePhone, MobilePhoneModel
ORDER BY u DESC
LIMIT 10;
