-- Explicit AS query; tpcds_* macros normalize the public aggregate scaling and NULL semantics.
WITH stats AS (
 SELECT i_item_id,
        tpcds_avg((priv_hash(hash(ss_customer_sk))), ss_quantity) AS agg1,
        tpcds_avg((priv_hash(hash(ss_customer_sk))), ss_list_price) AS agg2,
        tpcds_avg((priv_hash(hash(ss_customer_sk))), ss_coupon_amt) AS agg3,
        tpcds_avg((priv_hash(hash(ss_customer_sk))), ss_sales_price) AS agg4,
        tpcds_count((priv_hash(hash(ss_customer_sk))), 1) AS presence
 FROM main.store_sales,
     main.customer_demographics,
     main.date_dim,
     main.item,
     main.promotion,
     (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE ss_customer_sk = asa_owner0.asa_key0
  AND ss_sold_date_sk = d_date_sk
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
), answers AS (
 SELECT world, i_item_id,
        agg1[world+1] AS agg1,
        agg2[world+1] AS agg2,
        agg3[world+1] AS agg3,
        agg4[world+1] AS agg4
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1] > 0
)
SELECT * FROM answers
QUALIFY row_number() OVER (PARTITION BY world ORDER BY i_item_id) <= 100
ORDER BY world, i_item_id;
