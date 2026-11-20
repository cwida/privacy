-- Explicit AS query; tpcds_* macros normalize the public aggregate scaling and NULL semantics.
WITH stats AS (
 SELECT tpcds_count((priv_hash(hash(ss_customer_sk))), 1) AS total,
        tpcds_count((priv_hash(hash(ss_customer_sk))), 1) AS presence
 FROM main.store_sales ,
     main.household_demographics,
     main.time_dim,
     main.store,
     (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE ss_customer_sk = asa_owner0.asa_key0
  AND ss_sold_time_sk = time_dim.t_time_sk
  AND ss_hdemo_sk = household_demographics.hd_demo_sk
  AND ss_store_sk = s_store_sk
  AND time_dim.t_hour = 20
  AND time_dim.t_minute >= 30
  AND household_demographics.hd_dep_count = 7
  AND store.s_store_name = 'ese'
), answers AS (
 SELECT world, total[world+1] AS total
 FROM stats CROSS JOIN range(64) worlds(world)
)
SELECT * FROM answers
QUALIFY row_number() OVER (PARTITION BY world ORDER BY total) <= 100
ORDER BY world, total;
