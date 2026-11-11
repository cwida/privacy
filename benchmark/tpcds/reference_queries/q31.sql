-- Fact scans feed AS once; subsequent processing is over reduced aggregate results.
WITH ss_stats AS (
SELECT ca_county,
          d_qoy,
          d_year,
          tpcds_sum(priv_hash(hash(ss_customer_sk)), ss_ext_sales_price) AS store_sales,
 tpcds_count(priv_hash(hash(ss_customer_sk)),1) AS presence
FROM main.store_sales,
        main.date_dim,
        main.customer_address, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE ss_customer_sk=asa_owner.asa_key AND ss_sold_date_sk = d_date_sk
     AND ss_addr_sk=ca_address_sk
   GROUP BY ca_county,
            d_qoy,
            d_year
),
ss AS (
SELECT world, ca_county, d_qoy, d_year, store_sales[world+1] AS store_sales
FROM ss_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
ws_stats AS (
SELECT ca_county,
          d_qoy,
          d_year,
          tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), ws_ext_sales_price) AS web_sales,
 tpcds_count(priv_hash(hash(ws_bill_customer_sk)),1) AS presence
FROM main.web_sales,
        main.date_dim,
        main.customer_address, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE ws_bill_customer_sk=asa_owner.asa_key AND ws_sold_date_sk = d_date_sk
     AND ws_bill_addr_sk=ca_address_sk
   GROUP BY ca_county,
            d_qoy,
            d_year
),
ws AS (
SELECT world, ca_county, d_qoy, d_year, web_sales[world+1] AS web_sales
FROM ws_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
)
SELECT ss1.world AS world, ss1.ca_county ,
       ss1.d_year ,
       (ws2.web_sales*1.0000)/ws1.web_sales web_q1_q2_increase ,
       (ss2.store_sales*1.0000)/ss1.store_sales store_q1_q2_increase ,
       (ws3.web_sales*1.0000)/ws2.web_sales web_q2_q3_increase ,
       (ss3.store_sales*1.0000)/ss2.store_sales store_q2_q3_increase
FROM ss ss1 ,
     ss ss2 ,
     ss ss3 ,
     ws ws1 ,
     ws ws2 ,
     ws ws3
WHERE ss1.world=ss2.world AND ss1.world=ss3.world AND ss1.world=ws1.world AND ss1.world=ws2.world AND ss1.world=ws3.world AND  ss1.d_qoy = 1
  AND ss1.d_year = 2000
  AND ss1.ca_county = ss2.ca_county
  AND ss2.d_qoy = 2
  AND ss2.d_year = 2000
  AND ss2.ca_county = ss3.ca_county
  AND ss3.d_qoy = 3
  AND ss3.d_year = 2000
  AND ss1.ca_county = ws1.ca_county
  AND ws1.d_qoy = 1
  AND ws1.d_year = 2000
  AND ws1.ca_county = ws2.ca_county
  AND ws2.d_qoy = 2
  AND ws2.d_year = 2000
  AND ws1.ca_county = ws3.ca_county
  AND ws3.d_qoy = 3
  AND ws3.d_year = 2000
  AND CASE
          WHEN ws1.web_sales > 0 THEN (ws2.web_sales*1.0000)/ws1.web_sales
          ELSE NULL
      END > CASE
                WHEN ss1.store_sales > 0 THEN (ss2.store_sales*1.0000)/ss1.store_sales
                ELSE NULL
            END
  AND CASE
          WHEN ws2.web_sales > 0 THEN (ws3.web_sales*1.0000)/ws2.web_sales
          ELSE NULL
      END > CASE
                WHEN ss2.store_sales > 0 THEN (ss3.store_sales*1.0000)/ss2.store_sales
                ELSE NULL
            END
ORDER BY ss1.world, ss1.ca_county;
