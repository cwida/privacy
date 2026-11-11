-- Explicit AS query; tpcds_* macros normalize the public aggregate scaling and NULL semantics.
WITH stats AS (
 SELECT w_substr,
        sm_type,
        LOWER(cc_name) AS cc_name_lower,
        tpcds_sum((priv_hash(hash(cs_bill_customer_sk))), CASE WHEN cs_ship_date_sk - cs_sold_date_sk <= 30 THEN 1 ELSE 0 END) AS bucket0,
        tpcds_sum((priv_hash(hash(cs_bill_customer_sk))), CASE WHEN cs_ship_date_sk - cs_sold_date_sk > 30 AND cs_ship_date_sk - cs_sold_date_sk <= 60 THEN 1 ELSE 0 END) AS bucket1,
        tpcds_sum((priv_hash(hash(cs_bill_customer_sk))), CASE WHEN cs_ship_date_sk - cs_sold_date_sk > 60 AND cs_ship_date_sk - cs_sold_date_sk <= 90 THEN 1 ELSE 0 END) AS bucket2,
        tpcds_sum((priv_hash(hash(cs_bill_customer_sk))), CASE WHEN cs_ship_date_sk - cs_sold_date_sk > 90 AND cs_ship_date_sk - cs_sold_date_sk <= 120 THEN 1 ELSE 0 END) AS bucket3,
        tpcds_sum((priv_hash(hash(cs_bill_customer_sk))), CASE WHEN cs_ship_date_sk - cs_sold_date_sk > 120 THEN 1 ELSE 0 END) AS bucket4,
        tpcds_count((priv_hash(hash(cs_bill_customer_sk))), 1) AS presence
 FROM main.catalog_sales ,
  (SELECT SUBSTRING(w_warehouse_name,1,20) w_substr, *
   FROM main.warehouse) AS sq1 ,
     main.ship_mode ,
     main.call_center ,
     main.date_dim,
     (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE cs_bill_customer_sk = asa_owner0.asa_key0
  AND d_month_seq BETWEEN 1200 AND 1200 + 11
  AND cs_ship_date_sk = d_date_sk
  AND cs_warehouse_sk = w_warehouse_sk
  AND cs_ship_mode_sk = sm_ship_mode_sk
  AND cs_call_center_sk = cc_call_center_sk
GROUP BY w_substr ,
         sm_type ,
         cc_name
), answers AS (
 SELECT world, w_substr,
        sm_type,
        cc_name_lower,
        bucket0[world+1] AS bucket0,
        bucket1[world+1] AS bucket1,
        bucket2[world+1] AS bucket2,
        bucket3[world+1] AS bucket3,
        bucket4[world+1] AS bucket4
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1] > 0
)
SELECT * FROM answers
QUALIFY row_number() OVER (PARTITION BY world ORDER BY w_substr NULLS FIRST, sm_type NULLS FIRST, cc_name_lower NULLS FIRST) <= 100
ORDER BY world, w_substr NULLS FIRST, sm_type NULLS FIRST, cc_name_lower NULLS FIRST;
