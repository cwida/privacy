-- Handwritten PAC SQL, following the compiled aggregate scopes and scalar output semantics.
WITH asa_states AS (SELECT s_store_name, s_store_id, as_sum(priv_hash(hash(ss_customer_sk)), CAST(CASE WHEN d_day_name='Sunday' THEN ss_sales_price ELSE NULL END AS DOUBLE)) AS sun_sales, as_sum(priv_hash(hash(ss_customer_sk)), CAST(CASE WHEN d_day_name='Monday' THEN ss_sales_price ELSE NULL END AS DOUBLE)) AS mon_sales, as_sum(priv_hash(hash(ss_customer_sk)), CAST(CASE WHEN d_day_name='Tuesday' THEN ss_sales_price ELSE NULL END AS DOUBLE)) AS tue_sales, as_sum(priv_hash(hash(ss_customer_sk)), CAST(CASE WHEN d_day_name='Wednesday' THEN ss_sales_price ELSE NULL END AS DOUBLE)) AS wed_sales, as_sum(priv_hash(hash(ss_customer_sk)), CAST(CASE WHEN d_day_name='Thursday' THEN ss_sales_price ELSE NULL END AS DOUBLE)) AS thu_sales, as_sum(priv_hash(hash(ss_customer_sk)), CAST(CASE WHEN d_day_name='Friday' THEN ss_sales_price ELSE NULL END AS DOUBLE)) AS fri_sales, as_sum(priv_hash(hash(ss_customer_sk)), CAST(CASE WHEN d_day_name='Saturday' THEN ss_sales_price ELSE NULL END AS DOUBLE)) AS sat_sales FROM date_dim,
     store_sales,
     store
WHERE d_date_sk = ss_sold_date_sk
  AND s_store_sk = ss_store_sk
  AND s_gmt_offset = -5
  AND d_year = 2000
GROUP BY s_store_name,
         s_store_id
),
asa_ranked AS (SELECT * FROM asa_states ORDER BY s_store_name, s_store_id, priv_mean(sun_sales), priv_mean(mon_sales), priv_mean(tue_sales), priv_mean(wed_sales), priv_mean(thu_sales), priv_mean(fri_sales), priv_mean(sat_sales) LIMIT 100)
SELECT s_store_name, s_store_id, CAST(as_noised(sun_sales) AS DECIMAL(38,2)) AS sun_sales, CAST(as_noised(mon_sales) AS DECIMAL(38,2)) AS mon_sales, CAST(as_noised(tue_sales) AS DECIMAL(38,2)) AS tue_sales, CAST(as_noised(wed_sales) AS DECIMAL(38,2)) AS wed_sales, CAST(as_noised(thu_sales) AS DECIMAL(38,2)) AS thu_sales, CAST(as_noised(fri_sales) AS DECIMAL(38,2)) AS fri_sales, CAST(as_noised(sat_sales) AS DECIMAL(38,2)) AS sat_sales FROM asa_ranked ORDER BY s_store_name, s_store_id, sun_sales, mon_sales, tue_sales, wed_sales, thu_sales, fri_sales, sat_sales;
