-- Explicit AS query; tpcds_* macros normalize the public aggregate scaling and NULL semantics.
WITH stats AS (
 SELECT s_store_name,
        s_company_id,
        s_street_number,
        s_street_name,
        s_street_type,
        s_suite_number,
        s_city,
        s_county,
        s_state,
        s_zip,
        tpcds_sum((priv_hash(hash(ss_customer_sk))), CASE WHEN sr_returned_date_sk - ss_sold_date_sk <= 30 THEN 1 ELSE 0 END) AS bucket0,
        tpcds_sum((priv_hash(hash(ss_customer_sk))), CASE WHEN sr_returned_date_sk - ss_sold_date_sk > 30 AND sr_returned_date_sk - ss_sold_date_sk <= 60 THEN 1 ELSE 0 END) AS bucket1,
        tpcds_sum((priv_hash(hash(ss_customer_sk))), CASE WHEN sr_returned_date_sk - ss_sold_date_sk > 60 AND sr_returned_date_sk - ss_sold_date_sk <= 90 THEN 1 ELSE 0 END) AS bucket2,
        tpcds_sum((priv_hash(hash(ss_customer_sk))), CASE WHEN sr_returned_date_sk - ss_sold_date_sk > 90 AND sr_returned_date_sk - ss_sold_date_sk <= 120 THEN 1 ELSE 0 END) AS bucket3,
        tpcds_sum((priv_hash(hash(ss_customer_sk))), CASE WHEN sr_returned_date_sk - ss_sold_date_sk > 120 THEN 1 ELSE 0 END) AS bucket4,
        tpcds_count((priv_hash(hash(ss_customer_sk))), 1) AS presence
 FROM main.store_sales,
     main.store_returns,
     main.store,
     main.date_dim d1,
     main.date_dim d2,
     (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE ss_customer_sk = asa_owner0.asa_key0
  AND d2.d_year = 2001
  AND d2.d_moy = 8
  AND ss_ticket_number = sr_ticket_number
  AND ss_item_sk = sr_item_sk
  AND ss_sold_date_sk = d1.d_date_sk
  AND sr_returned_date_sk = d2.d_date_sk
  AND ss_customer_sk = sr_customer_sk
  AND ss_store_sk = s_store_sk
GROUP BY s_store_name,
         s_company_id,
         s_street_number,
         s_street_name,
         s_street_type,
         s_suite_number,
         s_city,
         s_county,
         s_state,
         s_zip
), answers AS (
 SELECT world, s_store_name,
        s_company_id,
        s_street_number,
        s_street_name,
        s_street_type,
        s_suite_number,
        s_city,
        s_county,
        s_state,
        s_zip,
        bucket0[world+1] AS bucket0,
        bucket1[world+1] AS bucket1,
        bucket2[world+1] AS bucket2,
        bucket3[world+1] AS bucket3,
        bucket4[world+1] AS bucket4
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1] > 0
)
SELECT * FROM answers
QUALIFY row_number() OVER (PARTITION BY world ORDER BY s_store_name, s_company_id, s_street_number, s_street_name, s_street_type, s_suite_number, s_city, s_county, s_state, s_zip) <= 100
ORDER BY world, s_store_name, s_company_id, s_street_number, s_street_name, s_street_type, s_suite_number, s_city, s_county, s_state, s_zip;
