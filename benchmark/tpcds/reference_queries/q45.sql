-- Explicit AS query; tpcds_* macros normalize the public aggregate scaling and NULL semantics.
WITH stats AS (
 SELECT ca_zip,
        ca_city,
        tpcds_sum((priv_hash(hash(ws_bill_customer_sk))), ws_sales_price) AS total,
        tpcds_count((priv_hash(hash(ws_bill_customer_sk))), 1) AS presence
 FROM main.web_sales,
     main.customer,
     main.customer_address,
     main.date_dim,
     main.item,
     (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE ws_bill_customer_sk = asa_owner0.asa_key0
  AND ws_bill_customer_sk = c_customer_sk
  AND c_current_addr_sk = ca_address_sk
  AND ws_item_sk = i_item_sk
  AND (SUBSTRING(ca_zip,1,5) IN ('85669',
                              '86197',
                              '88274',
                              '83405',
                              '86475',
                              '85392',
                              '85460',
                              '80348',
                              '81792')
       OR i_item_id IN
         (SELECT i_item_id
          FROM main.item
          WHERE i_item_sk IN (2,
                              3,
                              5,
                              7,
                              11,
                              13,
                              17,
                              19,
                              23,
                              29) ))
  AND ws_sold_date_sk = d_date_sk
  AND d_qoy = 2
  AND d_year = 2001
GROUP BY ca_zip,
         ca_city
), answers AS (
 SELECT world, ca_zip,
        ca_city,
        total[world+1] AS total
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1] > 0
)
SELECT * FROM answers
QUALIFY row_number() OVER (PARTITION BY world ORDER BY ca_zip, ca_city) <= 100
ORDER BY world, ca_zip, ca_city;
