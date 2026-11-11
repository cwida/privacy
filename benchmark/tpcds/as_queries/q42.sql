-- Handwritten PAC SQL, following the compiled aggregate scopes and scalar output semantics.
WITH asa_states AS (SELECT dt.d_year AS d_year, item.i_category_id AS i_category_id, item.i_category AS i_category, as_sum(priv_hash(hash(ss_customer_sk)), CAST(ss_ext_sales_price AS DOUBLE)) AS sum_agg FROM date_dim dt,
     store_sales,
     item
WHERE dt.d_date_sk = store_sales.ss_sold_date_sk
  AND store_sales.ss_item_sk = item.i_item_sk
  AND item.i_manager_id = 1
  AND dt.d_moy=11
  AND dt.d_year=2000
GROUP BY dt.d_year,
         item.i_category_id,
         item.i_category
),
asa_ranked AS (SELECT * FROM asa_states ORDER BY priv_mean(sum_agg) DESC, d_year, i_category_id, i_category LIMIT 100)
SELECT d_year, i_category_id, i_category, CAST(as_noised(sum_agg) AS DECIMAL(38,2)) AS sum_agg FROM asa_ranked ORDER BY sum_agg DESC, d_year, i_category_id, i_category;
