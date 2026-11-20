-- Explicit AS query; tpcds_* macros normalize the public aggregate scaling and NULL semantics.
WITH stats AS (
 SELECT s_store_name,
        s_store_id,
        tpcds_sum((priv_hash(hash(ss_customer_sk))), CASE WHEN d_day_name='Sunday' THEN ss_sales_price ELSE NULL END) AS sun_sales,
        tpcds_sum((priv_hash(hash(ss_customer_sk))), CASE WHEN d_day_name='Monday' THEN ss_sales_price ELSE NULL END) AS mon_sales,
        tpcds_sum((priv_hash(hash(ss_customer_sk))), CASE WHEN d_day_name='Tuesday' THEN ss_sales_price ELSE NULL END) AS tue_sales,
        tpcds_sum((priv_hash(hash(ss_customer_sk))), CASE WHEN d_day_name='Wednesday' THEN ss_sales_price ELSE NULL END) AS wed_sales,
        tpcds_sum((priv_hash(hash(ss_customer_sk))), CASE WHEN d_day_name='Thursday' THEN ss_sales_price ELSE NULL END) AS thu_sales,
        tpcds_sum((priv_hash(hash(ss_customer_sk))), CASE WHEN d_day_name='Friday' THEN ss_sales_price ELSE NULL END) AS fri_sales,
        tpcds_sum((priv_hash(hash(ss_customer_sk))), CASE WHEN d_day_name='Saturday' THEN ss_sales_price ELSE NULL END) AS sat_sales,
        tpcds_count((priv_hash(hash(ss_customer_sk))), 1) AS presence
 FROM main.date_dim,
     main.store_sales,
     main.store,
     (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE ss_customer_sk = asa_owner0.asa_key0
  AND d_date_sk = ss_sold_date_sk
  AND s_store_sk = ss_store_sk
  AND s_gmt_offset = -5
  AND d_year = 2000
GROUP BY s_store_name,
         s_store_id
), answers AS (
 SELECT world, s_store_name,
        s_store_id,
        sun_sales[world+1] AS sun_sales,
        mon_sales[world+1] AS mon_sales,
        tue_sales[world+1] AS tue_sales,
        wed_sales[world+1] AS wed_sales,
        thu_sales[world+1] AS thu_sales,
        fri_sales[world+1] AS fri_sales,
        sat_sales[world+1] AS sat_sales
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1] > 0
)
SELECT * FROM answers
QUALIFY row_number() OVER (PARTITION BY world ORDER BY s_store_name, s_store_id, sun_sales, mon_sales, tue_sales, wed_sales, thu_sales, fri_sales, sat_sales) <= 100
ORDER BY world, s_store_name, s_store_id, sun_sales, mon_sales, tue_sales, wed_sales, thu_sales, fri_sales, sat_sales;
