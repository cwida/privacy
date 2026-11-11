-- Handwritten PAC SQL, following the compiled aggregate scopes and scalar output semantics.
SELECT ca_state,
       cd_gender,
       cd_marital_status,
       cd_dep_count,
       as_noised_count(priv_hash(hash(c.c_customer_sk)), 1::BIGINT) cnt1,
       as_noised_min(priv_hash(hash(c.c_customer_sk)), cd_dep_count) min1,
       as_noised_max(priv_hash(hash(c.c_customer_sk)), cd_dep_count) max1,
       (CAST(as_noised_sum(priv_hash(hash(c.c_customer_sk)), cd_dep_count) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(c.c_customer_sk)), cd_dep_count) AS DOUBLE)) avg1,
       cd_dep_employed_count,
       as_noised_count(priv_hash(hash(c.c_customer_sk)), 1::BIGINT) cnt2,
       as_noised_min(priv_hash(hash(c.c_customer_sk)), cd_dep_employed_count) min2,
       as_noised_max(priv_hash(hash(c.c_customer_sk)), cd_dep_employed_count) max2,
       (CAST(as_noised_sum(priv_hash(hash(c.c_customer_sk)), cd_dep_employed_count) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(c.c_customer_sk)), cd_dep_employed_count) AS DOUBLE)) avg2,
       cd_dep_college_count,
       as_noised_count(priv_hash(hash(c.c_customer_sk)), 1::BIGINT) cnt3,
       as_noised_min(priv_hash(hash(c.c_customer_sk)), cd_dep_college_count),
       as_noised_max(priv_hash(hash(c.c_customer_sk)), cd_dep_college_count),
       (CAST(as_noised_sum(priv_hash(hash(c.c_customer_sk)), cd_dep_college_count) AS DOUBLE) / CAST(as_noised_count(priv_hash(hash(c.c_customer_sk)), cd_dep_college_count) AS DOUBLE))
FROM customer c,
     customer_address ca,
     customer_demographics
WHERE c.c_current_addr_sk = ca.ca_address_sk
  AND cd_demo_sk = c.c_current_cdemo_sk
  AND EXISTS
    (SELECT *
     FROM store_sales,
          date_dim
     WHERE c.c_customer_sk = ss_customer_sk
       AND ss_sold_date_sk = d_date_sk
       AND d_year = 2002
       AND d_qoy < 4)
  AND (EXISTS
         (SELECT *
          FROM web_sales,
               date_dim
          WHERE c.c_customer_sk = ws_bill_customer_sk
            AND ws_sold_date_sk = d_date_sk
            AND d_year = 2002
            AND d_qoy < 4)
       OR EXISTS
         (SELECT *
          FROM catalog_sales,
               date_dim
          WHERE c.c_customer_sk = cs_ship_customer_sk
            AND cs_sold_date_sk = d_date_sk
            AND d_year = 2002
            AND d_qoy < 4))
GROUP BY ca_state,
         cd_gender,
         cd_marital_status,
         cd_dep_count,
         cd_dep_employed_count,
         cd_dep_college_count
ORDER BY ca_state NULLS FIRST,
         cd_gender NULLS FIRST,
         cd_marital_status NULLS FIRST,
         cd_dep_count NULLS FIRST,
         cd_dep_employed_count NULLS FIRST,
         cd_dep_college_count NULLS FIRST
LIMIT 100;
