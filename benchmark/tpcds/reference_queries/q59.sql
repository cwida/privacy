-- A shared per-store/week AS result is reused across both years.
WITH wss_stats AS (
SELECT d_week_seq,
          ss_store_sk,
          tpcds_sum(priv_hash(hash(ss_customer_sk)), CASE
                  WHEN (d_day_name='Sunday') THEN ss_sales_price
                  ELSE NULL
              END) sun_sales,
          tpcds_sum(priv_hash(hash(ss_customer_sk)), CASE
                  WHEN (d_day_name='Monday') THEN ss_sales_price
                  ELSE NULL
              END) mon_sales,
          tpcds_sum(priv_hash(hash(ss_customer_sk)), CASE
                  WHEN (d_day_name='Tuesday') THEN ss_sales_price
                  ELSE NULL
              END) tue_sales,
          tpcds_sum(priv_hash(hash(ss_customer_sk)), CASE
                  WHEN (d_day_name='Wednesday') THEN ss_sales_price
                  ELSE NULL
              END) wed_sales,
          tpcds_sum(priv_hash(hash(ss_customer_sk)), CASE
                  WHEN (d_day_name='Thursday') THEN ss_sales_price
                  ELSE NULL
              END) thu_sales,
          tpcds_sum(priv_hash(hash(ss_customer_sk)), CASE
                  WHEN (d_day_name='Friday') THEN ss_sales_price
                  ELSE NULL
              END) fri_sales,
          tpcds_sum(priv_hash(hash(ss_customer_sk)), CASE
                  WHEN (d_day_name='Saturday') THEN ss_sales_price
                  ELSE NULL
              END) sat_sales,
 tpcds_count(priv_hash(hash(ss_customer_sk)),1) AS presence
FROM main.store_sales,
        main.date_dim, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE ss_customer_sk=asa_owner.asa_key AND d_date_sk = ss_sold_date_sk
   GROUP BY d_week_seq,
            ss_store_sk
), wss AS (
SELECT world, d_week_seq, ss_store_sk, sun_sales[world+1] AS sun_sales, mon_sales[world+1] AS mon_sales, tue_sales[world+1] AS tue_sales, wed_sales[world+1] AS wed_sales, thu_sales[world+1] AS thu_sales, fri_sales[world+1] AS fri_sales, sat_sales[world+1] AS sat_sales
FROM wss_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
)
SELECT y.world AS world, s_store_name1,
       s_store_id1,
       d_week_seq1,
       sun_sales1/sun_sales2 AS sun_sales_ratio,
       mon_sales1/mon_sales2 AS mon_sales_ratio,
       tue_sales1/tue_sales2 AS tue_sales_ratio,
       wed_sales1/wed_sales2 AS wed_sales_ratio,
       thu_sales1/thu_sales2 AS thu_sales_ratio,
       fri_sales1/fri_sales2 AS fri_sales_ratio,
       sat_sales1/sat_sales2 AS sat_sales_ratio
FROM
  (SELECT wss.world, s_store_name s_store_name1,
          wss.d_week_seq d_week_seq1,
          s_store_id s_store_id1,
          sun_sales sun_sales1,
          mon_sales mon_sales1,
          tue_sales tue_sales1,
          wed_sales wed_sales1,
          thu_sales thu_sales1,
          fri_sales fri_sales1,
          sat_sales sat_sales1
   FROM wss,
        main.store,
        main.date_dim d
   WHERE d.d_week_seq = wss.d_week_seq
     AND ss_store_sk = s_store_sk
     AND d_month_seq BETWEEN 1212 AND 1212 + 11) y,
  (SELECT wss.world, s_store_name s_store_name2,
          wss.d_week_seq d_week_seq2,
          s_store_id s_store_id2,
          sun_sales sun_sales2,
          mon_sales mon_sales2,
          tue_sales tue_sales2,
          wed_sales wed_sales2,
          thu_sales thu_sales2,
          fri_sales fri_sales2,
          sat_sales sat_sales2
   FROM wss,
        main.store,
        main.date_dim d
   WHERE d.d_week_seq = wss.d_week_seq
     AND ss_store_sk = s_store_sk
     AND d_month_seq BETWEEN 1212 + 12 AND 1212 + 23) x
WHERE y.world=x.world AND  s_store_id1=s_store_id2
  AND d_week_seq1=d_week_seq2-52
QUALIFY row_number() OVER (PARTITION BY y.world ORDER BY s_store_name1 NULLS FIRST,
         s_store_id1 NULLS FIRST,
         d_week_seq1 NULLS FIRST) <= 100
ORDER BY y.world, s_store_name1 NULLS FIRST,
         s_store_id1 NULLS FIRST,
         d_week_seq1 NULLS FIRST;
