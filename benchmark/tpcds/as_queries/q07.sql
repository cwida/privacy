-- Handwritten PAC aggregates; automatic query rewriting is disabled by the benchmark.
SELECT i_item_id,
       (CAST(as_noised_sum(priv_hash(hash(ss_customer_sk)), ss_quantity) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(ss_customer_sk)), ss_quantity) AS DOUBLE)) agg1,
       (CAST(as_noised_sum(priv_hash(hash(ss_customer_sk)), ss_list_price) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(ss_customer_sk)), ss_list_price) AS DOUBLE)) agg2,
       (CAST(as_noised_sum(priv_hash(hash(ss_customer_sk)), ss_coupon_amt) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(ss_customer_sk)), ss_coupon_amt) AS DOUBLE)) agg3,
       (CAST(as_noised_sum(priv_hash(hash(ss_customer_sk)), ss_sales_price) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(ss_customer_sk)), ss_sales_price) AS DOUBLE)) agg4
FROM store_sales,
     customer_demographics,
     date_dim,
     item,
     promotion
WHERE ss_sold_date_sk = d_date_sk
  AND ss_item_sk = i_item_sk
  AND ss_cdemo_sk = cd_demo_sk
  AND ss_promo_sk = p_promo_sk
  AND cd_gender = 'M'
  AND cd_marital_status = 'S'
  AND cd_education_status = 'College'
  AND (p_channel_email = 'N'
       OR p_channel_event = 'N')
  AND d_year = 2000
GROUP BY i_item_id
ORDER BY i_item_id
LIMIT 100;
