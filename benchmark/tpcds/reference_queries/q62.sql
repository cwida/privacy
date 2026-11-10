-- Explicit AS query; tpcds_* macros normalize the public aggregate scaling and NULL semantics.
WITH stats AS (
 SELECT w_substr,
        sm_type,
        web_name,
        tpcds_sum((priv_hash(hash(ws_bill_customer_sk))), CASE WHEN ws_ship_date_sk - ws_sold_date_sk <= 30 THEN 1 ELSE 0 END) AS bucket0,
        tpcds_sum((priv_hash(hash(ws_bill_customer_sk))), CASE WHEN ws_ship_date_sk - ws_sold_date_sk > 30 AND ws_ship_date_sk - ws_sold_date_sk <= 60 THEN 1 ELSE 0 END) AS bucket1,
        tpcds_sum((priv_hash(hash(ws_bill_customer_sk))), CASE WHEN ws_ship_date_sk - ws_sold_date_sk > 60 AND ws_ship_date_sk - ws_sold_date_sk <= 90 THEN 1 ELSE 0 END) AS bucket2,
        tpcds_sum((priv_hash(hash(ws_bill_customer_sk))), CASE WHEN ws_ship_date_sk - ws_sold_date_sk > 90 AND ws_ship_date_sk - ws_sold_date_sk <= 120 THEN 1 ELSE 0 END) AS bucket3,
        tpcds_sum((priv_hash(hash(ws_bill_customer_sk))), CASE WHEN ws_ship_date_sk - ws_sold_date_sk > 120 THEN 1 ELSE 0 END) AS bucket4,
        tpcds_count((priv_hash(hash(ws_bill_customer_sk))), 1) AS presence
 FROM main.web_sales,
  (SELECT SUBSTRING(w_warehouse_name,1,20) w_substr,
          *
   FROM main.warehouse) sq1,
     main.ship_mode,
     main.web_site,
     main.date_dim,
     (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE ws_bill_customer_sk = asa_owner0.asa_key0
  AND d_month_seq BETWEEN 1200 AND 1200 + 11
  AND ws_ship_date_sk = d_date_sk
  AND ws_warehouse_sk = w_warehouse_sk
  AND ws_ship_mode_sk = sm_ship_mode_sk
  AND ws_web_site_sk = web_site_sk
GROUP BY w_substr,
         sm_type,
         web_name
), answers AS (
 SELECT world, w_substr,
        sm_type,
        web_name,
        bucket0[world+1] AS bucket0,
        bucket1[world+1] AS bucket1,
        bucket2[world+1] AS bucket2,
        bucket3[world+1] AS bucket3,
        bucket4[world+1] AS bucket4
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1] > 0
)
SELECT * FROM answers
QUALIFY row_number() OVER (PARTITION BY world ORDER BY w_substr NULLS FIRST, sm_type NULLS FIRST, web_name NULLS FIRST) <= 100
ORDER BY world, w_substr NULLS FIRST, sm_type NULLS FIRST, web_name NULLS FIRST;
