-- Explicit AS query; tpcds_* macros normalize the public aggregate scaling and NULL semantics.
WITH stats AS (
 SELECT i_brand_id AS brand_id,
        i_brand AS brand,
        i_manufact_id,
        i_manufact,
        tpcds_sum((priv_hash(hash(ss_customer_sk))), ss_ext_sales_price) AS ext_price,
        tpcds_count((priv_hash(hash(ss_customer_sk))), 1) AS presence
 FROM main.date_dim,
     main.store_sales,
     main.item,
     main.customer,
     main.customer_address,
     main.store,
     (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE ss_customer_sk = asa_owner0.asa_key0
  AND d_date_sk = ss_sold_date_sk
  AND ss_item_sk = i_item_sk
  AND i_manager_id=8
  AND d_moy=11
  AND d_year=1998
  AND ss_customer_sk = c_customer_sk
  AND c_current_addr_sk = ca_address_sk
  AND SUBSTRING(ca_zip, 1, 5) <> SUBSTRING(s_zip, 1, 5)
  AND ss_store_sk = s_store_sk
GROUP BY i_brand,
         i_brand_id,
         i_manufact_id,
         i_manufact
), answers AS (
 SELECT world, brand_id,
        brand,
        i_manufact_id,
        i_manufact,
        ext_price[world+1] AS ext_price
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1] > 0
)
SELECT * FROM answers
QUALIFY row_number() OVER (PARTITION BY world ORDER BY ext_price DESC, brand, brand_id, i_manufact_id, i_manufact) <= 100
ORDER BY world, ext_price DESC, brand, brand_id, i_manufact_id, i_manufact;
