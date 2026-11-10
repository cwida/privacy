-- All time-bucket counters share the fact/dimension joins.
WITH stats AS (SELECT tpcds_count(CASE WHEN t_hour=8 AND t_minute >= 30 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,1) AS h8_30_to_9,
tpcds_count(CASE WHEN t_hour=9 AND t_minute < 30 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,1) AS h9_to_9_30,
tpcds_count(CASE WHEN t_hour=9 AND t_minute >= 30 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,1) AS h9_30_to_10,
tpcds_count(CASE WHEN t_hour=10 AND t_minute < 30 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,1) AS h10_to_10_30,
tpcds_count(CASE WHEN t_hour=10 AND t_minute >= 30 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,1) AS h10_30_to_11,
tpcds_count(CASE WHEN t_hour=11 AND t_minute < 30 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,1) AS h11_to_11_30,
tpcds_count(CASE WHEN t_hour=11 AND t_minute >= 30 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,1) AS h11_30_to_12,
tpcds_count(CASE WHEN t_hour=12 AND t_minute < 30 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,1) AS h12_to_12_30
FROM main.store_sales,
        main.household_demographics,
        main.time_dim,
        main.store, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE ss_customer_sk=asa_owner.asa_key AND  ss_sold_time_sk = time_dim.t_time_sk
     AND ss_hdemo_sk = household_demographics.hd_demo_sk
     AND ss_store_sk = s_store_sk
     AND ((household_demographics.hd_dep_count = 4
           AND household_demographics.hd_vehicle_count<=4+2)
          OR (household_demographics.hd_dep_count = 2
              AND household_demographics.hd_vehicle_count<=2+2)
          OR (household_demographics.hd_dep_count = 0
              AND household_demographics.hd_vehicle_count<=0+2))
     AND store.s_store_name = 'ese'
)
SELECT world,h8_30_to_9[world+1] AS h8_30_to_9, h9_to_9_30[world+1] AS h9_to_9_30, h9_30_to_10[world+1] AS h9_30_to_10, h10_to_10_30[world+1] AS h10_to_10_30, h10_30_to_11[world+1] AS h10_30_to_11, h11_to_11_30[world+1] AS h11_to_11_30, h11_30_to_12[world+1] AS h11_30_to_12, h12_to_12_30[world+1] AS h12_to_12_30 FROM stats CROSS JOIN range(64) worlds(world) ORDER BY world;
