WITH value_agg AS (
    SELECT SearchPhrase, MIN(URL) AS min_url, MIN(Title) AS min_title, as_noised_count(as_key) AS c
    FROM (
        SELECT SearchPhrase, URL, Title, priv_hash(hash(rowid)) AS as_key
        FROM hits
        WHERE Title LIKE '%Google%' AND URL NOT LIKE '%.google.%' AND SearchPhrase <> ''
    ) h
    GROUP BY SearchPhrase
),
distinct_users AS (
    SELECT SearchPhrase, as_noised_count(as_key) AS u
    FROM (
        SELECT SearchPhrase, UserID, priv_hash(hash(min(rowid))) AS as_key
        FROM hits
        WHERE Title LIKE '%Google%' AND URL NOT LIKE '%.google.%' AND SearchPhrase <> '' AND UserID IS NOT NULL
        GROUP BY SearchPhrase, UserID
    ) h
    GROUP BY SearchPhrase
)
SELECT value_agg.SearchPhrase, min_url, min_title, c, u
FROM value_agg
JOIN distinct_users USING (SearchPhrase)
ORDER BY c DESC
LIMIT 10;
