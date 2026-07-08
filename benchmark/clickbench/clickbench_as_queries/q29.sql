SELECT *
FROM (
    SELECT k,
           as_noised_avg(as_key, STRLEN(Referer)) AS l,
           as_noised_count(as_key) AS c,
           MIN(Referer) AS min_referer
    FROM (
        SELECT REGEXP_REPLACE(Referer, '^https?://(?:www\.)?([^/]+)/.*$', '\1') AS k,
               Referer,
               priv_hash(hash(rowid)) AS as_key
        FROM hits
        WHERE Referer <> ''
    ) h
    GROUP BY k
) agg
WHERE c > 100000
ORDER BY l DESC
LIMIT 25;
