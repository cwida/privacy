SELECT *
FROM (
    SELECT CounterID,
           as_noised_avg(as_key, STRLEN(URL)) AS l,
           as_noised_count(as_key) AS c
    FROM (
        SELECT CounterID, URL, priv_hash(hash(rowid)) AS as_key
        FROM hits
        WHERE URL <> ''
    ) h
    GROUP BY CounterID
) agg
WHERE c > 100000
ORDER BY l DESC
LIMIT 25;
