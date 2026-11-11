-- Handwritten PAC SQL, following the compiled aggregate scopes and scalar output semantics.
WITH asa_states AS (SELECT dt.d_year AS d_year, item.i_brand_id AS brand_id, item.i_brand AS brand, as_sum(priv_hash(hash(ss_customer_sk)), CAST(ss_ext_sales_price AS DOUBLE)) AS ext_price FROM date_dim dt,
     store_sales,
     item
WHERE dt.d_date_sk = store_sales.ss_sold_date_sk
  AND store_sales.ss_item_sk = item.i_item_sk
  AND item.i_manager_id = 1
  AND dt.d_moy=11
  AND dt.d_year=2000
GROUP BY dt.d_year,
         item.i_brand,
         item.i_brand_id
),
asa_ranked AS (SELECT * FROM asa_states ORDER BY d_year, priv_mean(ext_price) DESC, brand_id LIMIT 100)
SELECT d_year, brand_id, brand, CAST(as_noised(ext_price) AS DECIMAL(38,2)) AS ext_price FROM asa_ranked ORDER BY d_year, ext_price DESC, brand_id;
