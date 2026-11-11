-- Explicit AS query; tpcds_* macros normalize the public aggregate scaling and NULL semantics.
WITH stats AS (
 SELECT i_item_id,
        i_item_desc,
        s_state,
        tpcds_count((priv_hash(hash(ss_customer_sk))), ss_quantity) AS v0_count,
        tpcds_avg((priv_hash(hash(ss_customer_sk))), ss_quantity) AS v0_avg,
        tpcds_stddev_samp((priv_hash(hash(ss_customer_sk))), ss_quantity) AS v0_stddev_samp,
        tpcds_count((priv_hash(hash(ss_customer_sk))), sr_return_quantity) AS v1_count,
        tpcds_avg((priv_hash(hash(ss_customer_sk))), sr_return_quantity) AS v1_avg,
        tpcds_stddev_samp((priv_hash(hash(ss_customer_sk))), sr_return_quantity) AS v1_stddev_samp,
        tpcds_count((priv_hash(hash(ss_customer_sk))), cs_quantity) AS v2_count,
        tpcds_avg((priv_hash(hash(ss_customer_sk))), cs_quantity) AS v2_avg,
        tpcds_stddev_samp((priv_hash(hash(ss_customer_sk))), cs_quantity) AS v2_stddev_samp,
        tpcds_count((priv_hash(hash(ss_customer_sk))), 1) AS presence
 FROM main.store_sales,
     main.store_returns,
     main.catalog_sales,
     main.date_dim d1,
     main.date_dim d2,
     main.date_dim d3,
     main.store,
     main.item,
     (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE ss_customer_sk = asa_owner0.asa_key0
  AND d1.d_quarter_name = '2001Q1'
  AND d1.d_date_sk = ss_sold_date_sk
  AND i_item_sk = ss_item_sk
  AND s_store_sk = ss_store_sk
  AND ss_customer_sk = sr_customer_sk
  AND ss_item_sk = sr_item_sk
  AND ss_ticket_number = sr_ticket_number
  AND sr_returned_date_sk = d2.d_date_sk
  AND d2.d_quarter_name IN ('2001Q1',
                            '2001Q2',
                            '2001Q3')
  AND sr_customer_sk = cs_bill_customer_sk
  AND sr_item_sk = cs_item_sk
  AND cs_sold_date_sk = d3.d_date_sk
  AND d3.d_quarter_name IN ('2001Q1',
                            '2001Q2',
                            '2001Q3')
GROUP BY i_item_id,
         i_item_desc,
         s_state
), answers AS (
 SELECT world, i_item_id,
        i_item_desc,
        s_state,
        v0_count[world+1] AS store_sales_quantitycount,
        v0_avg[world+1] AS store_sales_quantityave,
        v0_stddev_samp[world+1] AS store_sales_quantitystdev,
        v0_stddev_samp[world+1]/v0_avg[world+1] AS store_sales_quantitycov,
        v1_count[world+1] AS store_returns_quantitycount,
        v1_avg[world+1] AS store_returns_quantityave,
        v1_stddev_samp[world+1] AS store_returns_quantitystdev,
        v1_stddev_samp[world+1]/v1_avg[world+1] AS store_returns_quantitycov,
        v2_count[world+1] AS catalog_sales_quantitycount,
        v2_avg[world+1] AS catalog_sales_quantityave,
        v2_stddev_samp[world+1] AS catalog_sales_quantitystdev,
        v2_stddev_samp[world+1]/v2_avg[world+1] AS catalog_sales_quantitycov
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1] > 0
)
SELECT * FROM answers
QUALIFY row_number() OVER (PARTITION BY world ORDER BY i_item_id NULLS FIRST, i_item_desc NULLS FIRST, s_state NULLS FIRST) <= 100
ORDER BY world, i_item_id NULLS FIRST, i_item_desc NULLS FIRST, s_state NULLS FIRST;
