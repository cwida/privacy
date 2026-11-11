-- Handwritten PAC aggregates; automatic query rewriting is disabled by the benchmark.
SELECT i_item_id,
       ca_country,
       ca_state,
       ca_county,
       (CAST(as_noised_sum(priv_hash(hash(c_customer_sk)), cast(cs_quantity AS decimal(12, 2))) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(c_customer_sk)), cast(cs_quantity AS decimal(12, 2))) AS DOUBLE))      agg1,
       (CAST(as_noised_sum(priv_hash(hash(c_customer_sk)), cast(cs_list_price AS decimal(12, 2))) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(c_customer_sk)), cast(cs_list_price AS decimal(12, 2))) AS DOUBLE))    agg2,
       (CAST(as_noised_sum(priv_hash(hash(c_customer_sk)), cast(cs_coupon_amt AS decimal(12, 2))) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(c_customer_sk)), cast(cs_coupon_amt AS decimal(12, 2))) AS DOUBLE))    agg3,
       (CAST(as_noised_sum(priv_hash(hash(c_customer_sk)), cast(cs_sales_price AS decimal(12, 2))) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(c_customer_sk)), cast(cs_sales_price AS decimal(12, 2))) AS DOUBLE))   agg4,
       (CAST(as_noised_sum(priv_hash(hash(c_customer_sk)), cast(cs_net_profit AS decimal(12, 2))) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(c_customer_sk)), cast(cs_net_profit AS decimal(12, 2))) AS DOUBLE))    agg5,
       (CAST(as_noised_sum(priv_hash(hash(c_customer_sk)), cast(c_birth_year AS decimal(12, 2))) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(c_customer_sk)), cast(c_birth_year AS decimal(12, 2))) AS DOUBLE))     agg6,
       (CAST(as_noised_sum(priv_hash(hash(c_customer_sk)), cast(cd1.cd_dep_count AS decimal(12, 2))) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(c_customer_sk)), cast(cd1.cd_dep_count AS decimal(12, 2))) AS DOUBLE)) agg7
FROM catalog_sales,
     customer_demographics cd1,
     customer_demographics cd2,
     customer,
     customer_address,
     date_dim,
     item
WHERE cs_sold_date_sk = d_date_sk
  AND cs_item_sk = i_item_sk
  AND cs_bill_cdemo_sk = cd1.cd_demo_sk
  AND cs_bill_customer_sk = c_customer_sk
  AND cd1.cd_gender = 'F'
  AND cd1.cd_education_status = 'Unknown'
  AND c_current_cdemo_sk = cd2.cd_demo_sk
  AND c_current_addr_sk = ca_address_sk
  AND c_birth_month IN (1,
                        6,
                        8,
                        9,
                        12,
                        2)
  AND d_year = 1998
  AND ca_state IN ('MS',
                   'IN',
                   'ND',
                   'OK',
                   'NM',
                   'VA',
                   'MS')
GROUP BY ROLLUP (i_item_id,
    ca_country,
    ca_state,
    ca_county)
ORDER BY ca_country NULLS FIRST,
    ca_state NULLS FIRST,
    ca_county NULLS FIRST,
    i_item_id NULLS FIRST
LIMIT 100;
