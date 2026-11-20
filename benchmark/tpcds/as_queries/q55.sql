-- Handwritten PAC SQL, following the compiled aggregate scopes and scalar output semantics.
WITH asa_states AS (SELECT i_brand_id AS brand_id, i_brand AS brand, as_sum(priv_hash(hash(ss_customer_sk)), CAST(ss_ext_sales_price AS DOUBLE)) AS ext_price FROM date_dim,
     store_sales,
     item
WHERE d_date_sk = ss_sold_date_sk
  AND ss_item_sk = i_item_sk
  AND i_manager_id=28
  AND d_moy=11
  AND d_year=1999
GROUP BY i_brand,
         i_brand_id
),
asa_ranked AS (SELECT * FROM asa_states ORDER BY priv_mean(ext_price) DESC, brand_id LIMIT 100)
SELECT brand_id, brand, CAST(as_noised(ext_price) AS DECIMAL(38,2)) AS ext_price FROM asa_ranked ORDER BY ext_price DESC, brand_id;
