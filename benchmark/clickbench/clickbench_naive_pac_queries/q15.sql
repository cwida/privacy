-- Naive PAC sample-and-aggregate (explicit sample-table join + pac_aggregate terminal).
-- Q15: SELECT SearchEngineID, SearchPhrase, COUNT(*) AS c FROM hits WHERE SearchPhrase <> '' GROUP BY SearchEngineID, SearchPhrase ORDER BY c DESC LIMIT 10;
WITH samples AS (
    SELECT u.UserID AS pu_key, s.sample_id
    FROM (SELECT DISTINCT UserID FROM hits) u
    CROSS JOIN generate_series(1, 128) AS s(sample_id)
    WHERE random() < 0.5
),
per_sample AS (
    SELECT s.sample_id, h.SearchEngineID, h.SearchPhrase, COUNT(*) AS cnt
    FROM samples s JOIN hits h ON h.UserID = s.pu_key
    WHERE h.SearchPhrase <> ''
    GROUP BY s.sample_id, h.SearchEngineID, h.SearchPhrase
)
SELECT SearchEngineID, SearchPhrase,
       pac_aggregate(array_agg(cnt ORDER BY sample_id),
                     array_agg(cnt ORDER BY sample_id), 1.0/128, 3) AS c
FROM per_sample
GROUP BY SearchEngineID, SearchPhrase
ORDER BY c DESC LIMIT 10;
