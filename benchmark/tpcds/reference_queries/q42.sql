-- Explicit AS query; tpcds_* macros normalize the public aggregate scaling and NULL semantics.
WITH stats AS (
 SELECT dt.d_year AS d_year,
        item.i_category_id AS i_category_id,
        item.i_category AS i_category,
        tpcds_sum((priv_hash(hash(ss_customer_sk))), ss_ext_sales_price) AS total,
        tpcds_count((priv_hash(hash(ss_customer_sk))), 1) AS presence
 FROM main.date_dim dt,
     main.store_sales,
     main.item,
     (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE ss_customer_sk = asa_owner0.asa_key0
  AND dt.d_date_sk = store_sales.ss_sold_date_sk
  AND store_sales.ss_item_sk = item.i_item_sk
  AND item.i_manager_id = 1
  AND dt.d_moy=11
  AND dt.d_year=2000
GROUP BY dt.d_year,
         main.item.i_category_id,
         main.item.i_category
), answers AS (
 SELECT world, d_year,
        i_category_id,
        i_category,
        total[world+1] AS total
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1] > 0
)
SELECT * FROM answers
QUALIFY row_number() OVER (PARTITION BY world ORDER BY total DESC, d_year, i_category_id, i_category) <= 100
ORDER BY world, total DESC, d_year, i_category_id, i_category;
