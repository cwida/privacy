-- Handwritten PAC aggregates; automatic query rewriting is disabled by the benchmark.
SELECT as_noised_count(priv_hash(hash(ss_customer_sk)), 1::BIGINT)
FROM store_sales ,
     household_demographics,
     time_dim,
     store
WHERE ss_sold_time_sk = time_dim.t_time_sk
  AND ss_hdemo_sk = household_demographics.hd_demo_sk
  AND ss_store_sk = s_store_sk
  AND time_dim.t_hour = 20
  AND time_dim.t_minute >= 30
  AND household_demographics.hd_dep_count = 7
  AND store.s_store_name = 'ese'
ORDER BY as_noised_count(priv_hash(hash(ss_customer_sk)), 1::BIGINT)
LIMIT 100;
