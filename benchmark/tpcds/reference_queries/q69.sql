-- Same-owner EXISTS is static after entity inclusion; shipping-customer EXISTS needs the bill-owner mask.
WITH catalog_presence AS (
SELECT cs_ship_customer_sk,bit_or(priv_hash(hash(cs_bill_customer_sk))) AS membership
          FROM main.catalog_sales,
               main.date_dim, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE cs_bill_customer_sk=asa_owner.asa_key AND  cs_sold_date_sk = d_date_sk
            AND d_year = 2001
            AND d_moy BETWEEN 4 AND 4+2
GROUP BY cs_ship_customer_sk
), stats AS (SELECT cd_gender,
cd_marital_status,
cd_education_status,
cd_purchase_estimate,
cd_credit_rating,
tpcds_count((priv_hash(hash(c.c_customer_sk)) & ~coalesce(catalog_presence.membership,0::UBIGINT)),1) AS n
FROM main.customer c CROSS JOIN
     main.customer_address ca CROSS JOIN
     main.customer_demographics LEFT JOIN catalog_presence ON c.c_customer_sk=cs_ship_customer_sk
WHERE c.c_current_addr_sk = ca.ca_address_sk
  AND ca_state IN ('KY',
                   'GA',
                   'NM')
  AND cd_demo_sk = c.c_current_cdemo_sk

AND EXISTS(SELECT *
     FROM main.store_sales,
          main.date_dim
     WHERE c.c_customer_sk = ss_customer_sk
       AND ss_sold_date_sk = d_date_sk
       AND d_year = 2001
       AND d_moy BETWEEN 4 AND 4+2)
AND NOT EXISTS(SELECT *
          FROM main.web_sales,
               main.date_dim
          WHERE c.c_customer_sk = ws_bill_customer_sk
            AND ws_sold_date_sk = d_date_sk
            AND d_year = 2001
            AND d_moy BETWEEN 4 AND 4+2)
GROUP BY cd_gender, cd_marital_status, cd_education_status, cd_purchase_estimate, cd_credit_rating
), answers AS (SELECT world, cd_gender, cd_marital_status, cd_education_status, n[world+1] AS cnt1, cd_purchase_estimate, n[world+1] AS cnt2, cd_credit_rating, n[world+1] AS cnt3 FROM stats CROSS JOIN range(64) worlds(world) WHERE n[world+1]>0)
SELECT * FROM answers QUALIFY row_number() OVER (PARTITION BY world ORDER BY cd_gender,
         cd_marital_status,
         cd_education_status,
         cd_purchase_estimate,
         cd_credit_rating)<=100 ORDER BY world,cd_gender,
         cd_marital_status,
         cd_education_status,
         cd_purchase_estimate,
         cd_credit_rating;
