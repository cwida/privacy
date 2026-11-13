-- Explicit AS query; tpcds_* macros normalize the public aggregate scaling and NULL semantics.
WITH stats AS (
 SELECT i_item_id,
        i_item_desc,
        s_store_id,
        s_store_name,
        tpcds_sum((priv_hash(hash(ss_customer_sk))), ss_quantity) AS store_sales_quantity,
        tpcds_sum((priv_hash(hash(ss_customer_sk))), sr_return_quantity) AS store_returns_quantity,
        tpcds_sum((priv_hash(hash(ss_customer_sk))), cs_quantity) AS catalog_sales_quantity,
        tpcds_count((priv_hash(hash(ss_customer_sk))), 1) AS presence
 FROM main.store_sales,
     main.store_returns,
     main.catalog_sales,
     main.date_dim d1,
     main.date_dim d2,
     main.date_dim d3,
     main.store,
     main.item,
     (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE ss_customer_sk = asa_owner0.asa_key0
  AND d1.d_moy = 9
  AND d1.d_year = 1999
  AND d1.d_date_sk = ss_sold_date_sk
  AND i_item_sk = ss_item_sk
  AND s_store_sk = ss_store_sk
  AND ss_customer_sk = sr_customer_sk
  AND ss_item_sk = sr_item_sk
  AND ss_ticket_number = sr_ticket_number
  AND sr_returned_date_sk = d2.d_date_sk
  AND d2.d_moy BETWEEN 9 AND 9 + 3
  AND d2.d_year = 1999
  AND sr_customer_sk = cs_bill_customer_sk
  AND sr_item_sk = cs_item_sk
  AND cs_sold_date_sk = d3.d_date_sk
  AND d3.d_year IN (1999,
                    1999+1,
                    1999+2)
GROUP BY i_item_id,
         i_item_desc,
         s_store_id,
         s_store_name
), answers AS (
 SELECT world, i_item_id,
        i_item_desc,
        s_store_id,
        s_store_name,
        store_sales_quantity[world+1] AS store_sales_quantity,
        store_returns_quantity[world+1] AS store_returns_quantity,
        catalog_sales_quantity[world+1] AS catalog_sales_quantity
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1] > 0
)
SELECT * FROM answers
QUALIFY row_number() OVER (PARTITION BY world ORDER BY i_item_id, i_item_desc, s_store_id, s_store_name) <= 100
ORDER BY world, i_item_id, i_item_desc, s_store_id, s_store_name;
