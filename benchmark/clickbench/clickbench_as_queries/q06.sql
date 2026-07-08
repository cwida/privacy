SELECT as_noised_count(as_key) AS c
FROM (
    SELECT SearchPhrase, priv_hash(hash(min(rowid))) AS as_key
    FROM hits
    WHERE SearchPhrase IS NOT NULL
    GROUP BY SearchPhrase
) h;
