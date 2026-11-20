-- Explicit AS query; tpcds_* macros normalize the public aggregate scaling and NULL semantics.
WITH stats AS (
 SELECT i_item_id,
        ca_country,
        ca_state,
        ca_county,
        tpcds_avg((priv_hash(hash(cs_bill_customer_sk))), CAST(cs_quantity AS DECIMAL(12,2))) AS agg1,
        tpcds_avg((priv_hash(hash(cs_bill_customer_sk))), CAST(cs_list_price AS DECIMAL(12,2))) AS agg2,
        tpcds_avg((priv_hash(hash(cs_bill_customer_sk))), CAST(cs_coupon_amt AS DECIMAL(12,2))) AS agg3,
        tpcds_avg((priv_hash(hash(cs_bill_customer_sk))), CAST(cs_sales_price AS DECIMAL(12,2))) AS agg4,
        tpcds_avg((priv_hash(hash(cs_bill_customer_sk))), CAST(cs_net_profit AS DECIMAL(12,2))) AS agg5,
        tpcds_avg((priv_hash(hash(cs_bill_customer_sk))), CAST(c_birth_year AS DECIMAL(12,2))) AS agg6,
        tpcds_avg((priv_hash(hash(cs_bill_customer_sk))), CAST(cd1.cd_dep_count AS DECIMAL(12,2))) AS agg7,
        tpcds_count((priv_hash(hash(cs_bill_customer_sk))), 1) AS presence,
        GROUPING(i_item_id, ca_country, ca_state, ca_county) AS grouping_level
 FROM main.catalog_sales,
     main.customer_demographics cd1,
     main.customer_demographics cd2,
     main.customer,
     main.customer_address,
     main.date_dim,
     main.item,
     (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE cs_bill_customer_sk = asa_owner0.asa_key0
  AND cs_sold_date_sk = d_date_sk
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
), answers AS (
 SELECT world, i_item_id,
        ca_country,
        ca_state,
        ca_county,
        agg1[world+1] AS agg1,
        agg2[world+1] AS agg2,
        agg3[world+1] AS agg3,
        agg4[world+1] AS agg4,
        agg5[world+1] AS agg5,
        agg6[world+1] AS agg6,
        agg7[world+1] AS agg7
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1] > 0 OR grouping_level = 15
)
SELECT * FROM answers
QUALIFY row_number() OVER (PARTITION BY world ORDER BY ca_country NULLS FIRST, ca_state NULLS FIRST, ca_county NULLS FIRST, i_item_id NULLS FIRST) <= 100
ORDER BY world, ca_country NULLS FIRST, ca_state NULLS FIRST, ca_county NULLS FIRST, i_item_id NULLS FIRST;
