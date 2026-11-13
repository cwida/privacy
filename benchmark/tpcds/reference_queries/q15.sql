-- Explicit AS query; tpcds_* macros normalize the public aggregate scaling and NULL semantics.
WITH stats AS (
 SELECT ca_zip,
        tpcds_sum((priv_hash(hash(cs_bill_customer_sk))), cs_sales_price) AS total,
        tpcds_count((priv_hash(hash(cs_bill_customer_sk))), 1) AS presence
 FROM main.catalog_sales,
     main.customer,
     main.customer_address,
     main.date_dim,
     (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE cs_bill_customer_sk = asa_owner0.asa_key0
  AND cs_bill_customer_sk = c_customer_sk
  AND c_current_addr_sk = ca_address_sk
  AND (SUBSTRING(ca_zip, 1, 5) IN ('85669',
                                '86197',
                                '88274',
                                '83405',
                                '86475',
                                '85392',
                                '85460',
                                '80348',
                                '81792')
       OR ca_state IN ('CA',
                       'WA',
                       'GA')
       OR cs_sales_price > 500)
  AND cs_sold_date_sk = d_date_sk
  AND d_qoy = 2
  AND d_year = 2001
GROUP BY ca_zip
), answers AS (
 SELECT world, ca_zip,
        total[world+1] AS total
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1] > 0
)
SELECT * FROM answers
QUALIFY row_number() OVER (PARTITION BY world ORDER BY ca_zip NULLS FIRST) <= 100
ORDER BY world, ca_zip NULLS FIRST;
