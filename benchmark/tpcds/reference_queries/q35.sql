-- Same-owner EXISTS is static after entity inclusion; shipping-customer EXISTS needs the bill-owner mask.
WITH catalog_presence AS (
SELECT cs_ship_customer_sk,bit_or(priv_hash(hash(cs_bill_customer_sk))) AS membership
          FROM main.catalog_sales,
               main.date_dim, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE cs_bill_customer_sk=asa_owner.asa_key AND  cs_sold_date_sk = d_date_sk
            AND d_year = 2002
            AND d_qoy < 4
GROUP BY cs_ship_customer_sk
), stats AS (SELECT ca_state,
cd_gender,
cd_marital_status,
cd_dep_count,
cd_dep_employed_count,
cd_dep_college_count,
tpcds_count((priv_hash(hash(c.c_customer_sk)) & ((CASE WHEN EXISTS(SELECT *
          FROM main.web_sales,
               main.date_dim
          WHERE c.c_customer_sk = ws_bill_customer_sk
            AND ws_sold_date_sk = d_date_sk
            AND d_year = 2002
            AND d_qoy < 4) THEN 18446744073709551615::UBIGINT ELSE 0::UBIGINT END) | coalesce(catalog_presence.membership,0::UBIGINT))),1) AS n,
tpcds_min((priv_hash(hash(c.c_customer_sk)) & ((CASE WHEN EXISTS(SELECT *
          FROM main.web_sales,
               main.date_dim
          WHERE c.c_customer_sk = ws_bill_customer_sk
            AND ws_sold_date_sk = d_date_sk
            AND d_year = 2002
            AND d_qoy < 4) THEN 18446744073709551615::UBIGINT ELSE 0::UBIGINT END) | coalesce(catalog_presence.membership,0::UBIGINT))),cd_dep_count) AS min1,
tpcds_max((priv_hash(hash(c.c_customer_sk)) & ((CASE WHEN EXISTS(SELECT *
          FROM main.web_sales,
               main.date_dim
          WHERE c.c_customer_sk = ws_bill_customer_sk
            AND ws_sold_date_sk = d_date_sk
            AND d_year = 2002
            AND d_qoy < 4) THEN 18446744073709551615::UBIGINT ELSE 0::UBIGINT END) | coalesce(catalog_presence.membership,0::UBIGINT))),cd_dep_count) AS max1,
tpcds_avg((priv_hash(hash(c.c_customer_sk)) & ((CASE WHEN EXISTS(SELECT *
          FROM main.web_sales,
               main.date_dim
          WHERE c.c_customer_sk = ws_bill_customer_sk
            AND ws_sold_date_sk = d_date_sk
            AND d_year = 2002
            AND d_qoy < 4) THEN 18446744073709551615::UBIGINT ELSE 0::UBIGINT END) | coalesce(catalog_presence.membership,0::UBIGINT))),cd_dep_count) AS avg1,
tpcds_min((priv_hash(hash(c.c_customer_sk)) & ((CASE WHEN EXISTS(SELECT *
          FROM main.web_sales,
               main.date_dim
          WHERE c.c_customer_sk = ws_bill_customer_sk
            AND ws_sold_date_sk = d_date_sk
            AND d_year = 2002
            AND d_qoy < 4) THEN 18446744073709551615::UBIGINT ELSE 0::UBIGINT END) | coalesce(catalog_presence.membership,0::UBIGINT))),cd_dep_employed_count) AS min2,
tpcds_max((priv_hash(hash(c.c_customer_sk)) & ((CASE WHEN EXISTS(SELECT *
          FROM main.web_sales,
               main.date_dim
          WHERE c.c_customer_sk = ws_bill_customer_sk
            AND ws_sold_date_sk = d_date_sk
            AND d_year = 2002
            AND d_qoy < 4) THEN 18446744073709551615::UBIGINT ELSE 0::UBIGINT END) | coalesce(catalog_presence.membership,0::UBIGINT))),cd_dep_employed_count) AS max2,
tpcds_avg((priv_hash(hash(c.c_customer_sk)) & ((CASE WHEN EXISTS(SELECT *
          FROM main.web_sales,
               main.date_dim
          WHERE c.c_customer_sk = ws_bill_customer_sk
            AND ws_sold_date_sk = d_date_sk
            AND d_year = 2002
            AND d_qoy < 4) THEN 18446744073709551615::UBIGINT ELSE 0::UBIGINT END) | coalesce(catalog_presence.membership,0::UBIGINT))),cd_dep_employed_count) AS avg2,
tpcds_min((priv_hash(hash(c.c_customer_sk)) & ((CASE WHEN EXISTS(SELECT *
          FROM main.web_sales,
               main.date_dim
          WHERE c.c_customer_sk = ws_bill_customer_sk
            AND ws_sold_date_sk = d_date_sk
            AND d_year = 2002
            AND d_qoy < 4) THEN 18446744073709551615::UBIGINT ELSE 0::UBIGINT END) | coalesce(catalog_presence.membership,0::UBIGINT))),cd_dep_college_count) AS min3,
tpcds_max((priv_hash(hash(c.c_customer_sk)) & ((CASE WHEN EXISTS(SELECT *
          FROM main.web_sales,
               main.date_dim
          WHERE c.c_customer_sk = ws_bill_customer_sk
            AND ws_sold_date_sk = d_date_sk
            AND d_year = 2002
            AND d_qoy < 4) THEN 18446744073709551615::UBIGINT ELSE 0::UBIGINT END) | coalesce(catalog_presence.membership,0::UBIGINT))),cd_dep_college_count) AS max3,
tpcds_avg((priv_hash(hash(c.c_customer_sk)) & ((CASE WHEN EXISTS(SELECT *
          FROM main.web_sales,
               main.date_dim
          WHERE c.c_customer_sk = ws_bill_customer_sk
            AND ws_sold_date_sk = d_date_sk
            AND d_year = 2002
            AND d_qoy < 4) THEN 18446744073709551615::UBIGINT ELSE 0::UBIGINT END) | coalesce(catalog_presence.membership,0::UBIGINT))),cd_dep_college_count) AS avg3
FROM main.customer c CROSS JOIN 
     main.customer_address ca CROSS JOIN 
     main.customer_demographics LEFT JOIN catalog_presence ON c.c_customer_sk=cs_ship_customer_sk
WHERE c.c_current_addr_sk = ca.ca_address_sk
  AND cd_demo_sk = c.c_current_cdemo_sk
  
AND EXISTS(SELECT *
     FROM main.store_sales,
          main.date_dim
     WHERE c.c_customer_sk = ss_customer_sk
       AND ss_sold_date_sk = d_date_sk
       AND d_year = 2002
       AND d_qoy < 4)
GROUP BY ca_state, cd_gender, cd_marital_status, cd_dep_count, cd_dep_employed_count, cd_dep_college_count
), answers AS (SELECT world, ca_state, cd_gender, cd_marital_status, cd_dep_count, n[world+1] AS cnt1, min1[world+1] AS min1, max1[world+1] AS max1, avg1[world+1] AS avg1, cd_dep_employed_count, n[world+1] AS cnt2, min2[world+1] AS min2, max2[world+1] AS max2, avg2[world+1] AS avg2, cd_dep_college_count, n[world+1] AS cnt3, min3[world+1] AS min3, max3[world+1] AS max3, avg3[world+1] AS avg3 FROM stats CROSS JOIN range(64) worlds(world) WHERE n[world+1]>0)
SELECT * FROM answers QUALIFY row_number() OVER (PARTITION BY world ORDER BY ca_state NULLS FIRST,
         cd_gender NULLS FIRST,
         cd_marital_status NULLS FIRST,
         cd_dep_count NULLS FIRST,
         cd_dep_employed_count NULLS FIRST,
         cd_dep_college_count NULLS FIRST)<=100 ORDER BY world,ca_state NULLS FIRST,
         cd_gender NULLS FIRST,
         cd_marital_status NULLS FIRST,
         cd_dep_count NULLS FIRST,
         cd_dep_employed_count NULLS FIRST,
         cd_dep_college_count NULLS FIRST;
