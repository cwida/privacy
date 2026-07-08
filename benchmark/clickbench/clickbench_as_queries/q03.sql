SELECT as_noised_sum(as_key, AdvEngineID) AS sum_adv,
       as_noised_count(as_key) AS c,
       as_noised_avg(as_key, ResolutionWidth) AS avg_rw
FROM (
    SELECT AdvEngineID, ResolutionWidth, priv_hash(hash(rowid)) AS as_key
    FROM hits
) h;
