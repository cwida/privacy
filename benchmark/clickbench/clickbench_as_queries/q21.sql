SELECT as_noised_count(priv_hash(hash(rowid))) AS c
FROM hits
WHERE URL LIKE '%google%';
