-- Naive PAC sample-and-aggregate (explicit sample-table join + pac_aggregate terminal).
-- Q28: SELECT CounterID, AVG(STRLEN(URL)) AS l, COUNT(*) AS c FROM hits WHERE URL <> '' GROUP BY CounterID HAVING COUNT(*) > 100000 ORDER BY l DESC LIMIT 25;
WITH samples AS (
    SELECT u.UserID AS pu_key, s.sample_id
    FROM (SELECT DISTINCT UserID FROM hits) u
    CROSS JOIN generate_series(1, 128) AS s(sample_id)
    WHERE random() < 0.5
),
per_sample AS (
    SELECT s.sample_id, h.CounterID, AVG(STRLEN(h.URL)) AS l, COUNT(*) AS cnt
    FROM samples s JOIN hits h ON h.UserID = s.pu_key
    WHERE h.URL <> ''
    GROUP BY s.sample_id, h.CounterID
)
SELECT CounterID,
       pac_aggregate(array_agg(l ORDER BY sample_id), array_agg(cnt ORDER BY sample_id), 1.0/128, 3) AS l,
       pac_aggregate(array_agg(cnt ORDER BY sample_id), array_agg(cnt ORDER BY sample_id), 1.0/128, 3) AS c
FROM per_sample
GROUP BY CounterID
HAVING c > 100000
ORDER BY l DESC LIMIT 25;
