-- Handwritten PAC aggregates; automatic query rewriting is disabled by the benchmark.
SELECT i_item_id,
       i_item_desc,
       s_state,
       as_noised_count(priv_hash(hash(ss_customer_sk)), ss_quantity) AS store_sales_quantitycount,
       (CAST(as_noised_sum(priv_hash(hash(ss_customer_sk)), ss_quantity) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(ss_customer_sk)), ss_quantity) AS DOUBLE)) AS store_sales_quantityave,
       as_noised_stddev_samp(priv_hash(hash(ss_customer_sk)), ss_quantity) AS store_sales_quantitystdev,
       as_noised_stddev_samp(priv_hash(hash(ss_customer_sk)), ss_quantity)/(CAST(as_noised_sum(priv_hash(hash(ss_customer_sk)), ss_quantity) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(ss_customer_sk)), ss_quantity) AS DOUBLE)) AS store_sales_quantitycov,
       as_noised_count(priv_hash(hash(ss_customer_sk)), sr_return_quantity) AS store_returns_quantitycount,
       (CAST(as_noised_sum(priv_hash(hash(ss_customer_sk)), sr_return_quantity) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(ss_customer_sk)), sr_return_quantity) AS DOUBLE)) AS store_returns_quantityave,
       as_noised_stddev_samp(priv_hash(hash(ss_customer_sk)), sr_return_quantity) AS store_returns_quantitystdev,
       as_noised_stddev_samp(priv_hash(hash(ss_customer_sk)), sr_return_quantity)/(CAST(as_noised_sum(priv_hash(hash(ss_customer_sk)), sr_return_quantity) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(ss_customer_sk)), sr_return_quantity) AS DOUBLE)) AS store_returns_quantitycov,
       as_noised_count(priv_hash(hash(ss_customer_sk)), cs_quantity) AS catalog_sales_quantitycount,
       (CAST(as_noised_sum(priv_hash(hash(ss_customer_sk)), cs_quantity) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(ss_customer_sk)), cs_quantity) AS DOUBLE)) AS catalog_sales_quantityave,
       as_noised_stddev_samp(priv_hash(hash(ss_customer_sk)), cs_quantity) AS catalog_sales_quantitystdev,
       as_noised_stddev_samp(priv_hash(hash(ss_customer_sk)), cs_quantity)/(CAST(as_noised_sum(priv_hash(hash(ss_customer_sk)), cs_quantity) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(ss_customer_sk)), cs_quantity) AS DOUBLE)) AS catalog_sales_quantitycov
FROM store_sales,
     store_returns,
     catalog_sales,
     date_dim d1,
     date_dim d2,
     date_dim d3,
     store,
     item
WHERE d1.d_quarter_name = '2001Q1'
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
ORDER BY i_item_id NULLS FIRST,
         i_item_desc NULLS FIRST,
         s_state NULLS FIRST
LIMIT 100;
