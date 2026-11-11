-- Explicit AS query; tpcds_* macros normalize the public aggregate scaling and NULL semantics.
WITH stats AS (
 SELECT tpcds_sum((priv_hash(hash(ss_customer_sk))), ss_quantity) AS total,
        tpcds_count((priv_hash(hash(ss_customer_sk))), 1) AS presence
 FROM main.store_sales,
     main.store,
     main.customer_demographics,
     main.customer_address,
     main.date_dim,
     (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE ss_customer_sk = asa_owner0.asa_key0
  AND s_store_sk = ss_store_sk
  AND ss_sold_date_sk = d_date_sk
  AND d_year = 2000
  AND ((cd_demo_sk = ss_cdemo_sk
        AND cd_marital_status = 'M'
        AND cd_education_status = '4 yr Degree'
        AND ss_sales_price BETWEEN 100.00 AND 150.00)
       OR (cd_demo_sk = ss_cdemo_sk
           AND cd_marital_status = 'D'
           AND cd_education_status = '2 yr Degree'
           AND ss_sales_price BETWEEN 50.00 AND 100.00)
       OR (cd_demo_sk = ss_cdemo_sk
           AND cd_marital_status = 'S'
           AND cd_education_status = 'College'
           AND ss_sales_price BETWEEN 150.00 AND 200.00))
  AND ((ss_addr_sk = ca_address_sk
        AND ca_country = 'United States'
        AND ca_state IN ('CO',
                         'OH',
                         'TX')
        AND ss_net_profit BETWEEN 0 AND 2000)
       OR (ss_addr_sk = ca_address_sk
           AND ca_country = 'United States'
           AND ca_state IN ('OR',
                            'MN',
                            'KY')
           AND ss_net_profit BETWEEN 150 AND 3000)
       OR (ss_addr_sk = ca_address_sk
           AND ca_country = 'United States'
           AND ca_state IN ('VA',
                            'CA',
                            'MS')
           AND ss_net_profit BETWEEN 50 AND 25000))
), answers AS (
 SELECT world, total[world+1] AS total
 FROM stats CROSS JOIN range(64) worlds(world)
)
SELECT * FROM answers
ORDER BY world;
