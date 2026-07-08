SELECT as_noised_min(priv_hash(hash(rowid)), EventDate) AS min_event_date,
       as_noised_max(priv_hash(hash(rowid)), EventDate) AS max_event_date
FROM hits;
