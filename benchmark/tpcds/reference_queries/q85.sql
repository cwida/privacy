-- Explicit AS query; tpcds_* macros normalize the public aggregate scaling and NULL semantics.
WITH stats AS (
 SELECT SUBSTRING(r_reason_desc,1,20) AS reason,
        tpcds_avg((priv_hash(hash(ws_bill_customer_sk)) & priv_hash(hash(wr_returning_customer_sk))), ws_quantity) AS avg1,
        tpcds_avg((priv_hash(hash(ws_bill_customer_sk)) & priv_hash(hash(wr_returning_customer_sk))), wr_refunded_cash) AS avg2,
        tpcds_avg((priv_hash(hash(ws_bill_customer_sk)) & priv_hash(hash(wr_returning_customer_sk))), wr_fee) AS avg3,
        tpcds_count((priv_hash(hash(ws_bill_customer_sk)) & priv_hash(hash(wr_returning_customer_sk))), 1) AS presence
 FROM main.web_sales,
     main.web_returns,
     main.web_page,
     main.customer_demographics cd1,
     main.customer_demographics cd2,
     main.customer_address,
     main.date_dim,
     main.reason,
     (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0, (SELECT c_customer_sk AS asa_key1 FROM main.customer) asa_owner1
WHERE ws_bill_customer_sk = asa_owner0.asa_key0 AND wr_returning_customer_sk = asa_owner1.asa_key1
  AND ws_web_page_sk = wp_web_page_sk
  AND ws_item_sk = wr_item_sk
  AND ws_order_number = wr_order_number
  AND ws_sold_date_sk = d_date_sk
  AND d_year = 2000
  AND cd1.cd_demo_sk = wr_refunded_cdemo_sk
  AND cd2.cd_demo_sk = wr_returning_cdemo_sk
  AND ca_address_sk = wr_refunded_addr_sk
  AND r_reason_sk = wr_reason_sk
  AND ( ( cd1.cd_marital_status = 'M'
         AND cd1.cd_marital_status = cd2.cd_marital_status
         AND cd1.cd_education_status = 'Advanced Degree'
         AND cd1.cd_education_status = cd2.cd_education_status
         AND ws_sales_price BETWEEN 100.00 AND 150.00 )
       OR ( cd1.cd_marital_status = 'S'
           AND cd1.cd_marital_status = cd2.cd_marital_status
           AND cd1.cd_education_status = 'College'
           AND cd1.cd_education_status = cd2.cd_education_status
           AND ws_sales_price BETWEEN 50.00 AND 100.00 )
       OR ( cd1.cd_marital_status = 'W'
           AND cd1.cd_marital_status = cd2.cd_marital_status
           AND cd1.cd_education_status = '2 yr Degree'
           AND cd1.cd_education_status = cd2.cd_education_status
           AND ws_sales_price BETWEEN 150.00 AND 200.00 ) )
  AND ( ( ca_country = 'United States'
         AND ca_state IN ('IN',
                          'OH',
                          'NJ')
         AND ws_net_profit BETWEEN 100 AND 200)
       OR ( ca_country = 'United States'
           AND ca_state IN ('WI',
                            'CT',
                            'KY')
           AND ws_net_profit BETWEEN 150 AND 300)
       OR ( ca_country = 'United States'
           AND ca_state IN ('LA',
                            'IA',
                            'AR')
           AND ws_net_profit BETWEEN 50 AND 250) )
GROUP BY r_reason_desc
), answers AS (
 SELECT world, reason,
        avg1[world+1] AS avg1,
        avg2[world+1] AS avg2,
        avg3[world+1] AS avg3
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1] > 0
)
SELECT * FROM answers
QUALIFY row_number() OVER (PARTITION BY world ORDER BY reason, avg1, avg2, avg3) <= 100
ORDER BY world, reason, avg1, avg2, avg3;
