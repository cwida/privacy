-- Explicit AS query; tpcds_* macros normalize the public aggregate scaling and NULL semantics.
WITH stats AS (
 SELECT i_brand_id AS brand_id,
        i_brand AS brand,
        tpcds_sum((priv_hash(hash(ss_customer_sk))), ss_ext_sales_price) AS ext_price,
        tpcds_count((priv_hash(hash(ss_customer_sk))), 1) AS presence
 FROM main.date_dim,
     main.store_sales,
     main.item,
     (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE ss_customer_sk = asa_owner0.asa_key0
  AND d_date_sk = ss_sold_date_sk
  AND ss_item_sk = i_item_sk
  AND i_manager_id=28
  AND d_moy=11
  AND d_year=1999
GROUP BY i_brand,
         i_brand_id
), answers AS (
 SELECT world, brand_id,
        brand,
        ext_price[world+1] AS ext_price
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1] > 0
)
SELECT * FROM answers
QUALIFY row_number() OVER (PARTITION BY world ORDER BY ext_price DESC, brand_id) <= 100
ORDER BY world, ext_price DESC, brand_id;
