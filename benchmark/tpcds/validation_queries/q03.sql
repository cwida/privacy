-- Explicit AS query; tpcds_* macros normalize the public aggregate scaling and NULL semantics.
WITH stats AS (
 SELECT dt.d_year AS d_year,
        item.i_brand_id AS brand_id,
        item.i_brand AS brand,
        tpcds_sum((priv_hash(hash(ss_customer_sk))), ss_ext_sales_price) AS sum_agg,
        tpcds_count((priv_hash(hash(ss_customer_sk))), 1) AS presence
 FROM main.date_dim dt,
     main.store_sales,
     main.item,
     (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE ss_customer_sk = asa_owner0.asa_key0
  AND dt.d_date_sk = store_sales.ss_sold_date_sk
  AND store_sales.ss_item_sk = item.i_item_sk
  AND item.i_manufact_id = 128
  AND dt.d_moy=11
GROUP BY dt.d_year,
         main.item.i_brand,
         main.item.i_brand_id
), answers AS (
 SELECT world, d_year,
        brand_id,
        brand,
        sum_agg[world+1] AS sum_agg
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1] > 0
)
SELECT * FROM answers
QUALIFY row_number() OVER (PARTITION BY world ORDER BY d_year, sum_agg DESC, brand_id) <= 100
ORDER BY world, d_year, sum_agg DESC, brand_id;
