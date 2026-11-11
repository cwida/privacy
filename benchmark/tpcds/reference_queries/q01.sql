-- Correlated thresholds use only the matching world of the reduced return aggregates.
WITH return_stats AS (
SELECT sr_customer_sk AS ctr_customer_sk,
          sr_store_sk AS ctr_store_sk,
          tpcds_sum(priv_hash(hash(sr_customer_sk)), sr_return_amt) AS ctr_total_return,
 tpcds_count(priv_hash(hash(sr_customer_sk)),1) AS presence
FROM main.store_returns,
        main.date_dim, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE sr_customer_sk=asa_owner.asa_key AND sr_returned_date_sk = d_date_sk
     AND d_year = 2000
   GROUP BY sr_customer_sk,
            sr_store_sk
), customer_total_return AS (
SELECT world, ctr_customer_sk, ctr_store_sk, ctr_total_return[world+1] AS ctr_total_return
FROM return_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
)
SELECT ctr1.world AS world, c_customer_id
FROM customer_total_return ctr1,
     main.store,
     main.customer
WHERE ctr1.ctr_total_return >
    (SELECT avg(ctr_total_return)*1.2
     FROM customer_total_return ctr2
     WHERE ctr1.world=ctr2.world AND ctr1.ctr_store_sk = ctr2.ctr_store_sk)
  AND s_store_sk = ctr1.ctr_store_sk
  AND s_state = 'TN'
  AND ctr1.ctr_customer_sk = c_customer_sk
QUALIFY row_number() OVER (PARTITION BY ctr1.world ORDER BY c_customer_id) <= 100
ORDER BY ctr1.world, c_customer_id;
