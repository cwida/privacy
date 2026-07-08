SELECT SearchPhrase, as_noised_count(as_key) AS u
FROM (
    SELECT SearchPhrase, UserID, priv_hash(hash(min(rowid))) AS as_key
    FROM hits
    WHERE SearchPhrase <> '' AND UserID IS NOT NULL
    GROUP BY SearchPhrase, UserID
) h
GROUP BY SearchPhrase
ORDER BY u DESC
LIMIT 10;
