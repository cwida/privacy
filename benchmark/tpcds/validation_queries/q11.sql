-- Shared AS states per channel/customer/year; joins align the world index.
WITH year_total_stats AS (
SELECT c_customer_id customer_id,
          c_first_name customer_first_name,
          c_last_name customer_last_name,
          c_preferred_cust_flag customer_preferred_cust_flag,
          c_birth_country customer_birth_country,
          c_login customer_login,
          c_email_address customer_email_address,
          d_year dyear,
          tpcds_sum(priv_hash(hash(c_customer_sk)), ss_ext_list_price-ss_ext_discount_amt) year_total,
          's' sale_type,
 tpcds_count(priv_hash(hash(c_customer_sk)),1) AS presence
FROM main.customer,
        main.store_sales,
        main.date_dim, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE c_customer_sk=asa_owner.asa_key AND c_customer_sk = ss_customer_sk
     AND ss_sold_date_sk = d_date_sk
   GROUP BY c_customer_id,
            c_first_name,
            c_last_name,
            c_preferred_cust_flag,
            c_birth_country,
            c_login,
            c_email_address,
            d_year

UNION ALL
 SELECT c_customer_id customer_id,
                    c_first_name customer_first_name,
                    c_last_name customer_last_name,
                    c_preferred_cust_flag customer_preferred_cust_flag,
                    c_birth_country customer_birth_country,
                    c_login customer_login,
                    c_email_address customer_email_address,
                    d_year dyear,
                    tpcds_sum(priv_hash(hash(c_customer_sk)), ws_ext_list_price-ws_ext_discount_amt) year_total,
                    'w' sale_type,
 tpcds_count(priv_hash(hash(c_customer_sk)),1) AS presence
FROM main.customer,
        main.web_sales,
        main.date_dim, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE c_customer_sk=asa_owner.asa_key AND c_customer_sk = ws_bill_customer_sk
     AND ws_sold_date_sk = d_date_sk
   GROUP BY c_customer_id,
            c_first_name,
            c_last_name,
            c_preferred_cust_flag,
            c_birth_country,
            c_login,
            c_email_address,
            d_year
), year_total AS (
SELECT world, customer_id, customer_first_name, customer_last_name, customer_preferred_cust_flag, customer_birth_country, customer_login, customer_email_address, dyear, year_total[world+1] AS year_total, sale_type
FROM year_total_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
)
SELECT t_s_firstyear.world AS world, t_s_secyear.customer_id,
       t_s_secyear.customer_first_name,
       t_s_secyear.customer_last_name,
       t_s_secyear.customer_preferred_cust_flag
FROM year_total t_s_firstyear,
     year_total t_s_secyear,
     year_total t_w_firstyear,
     year_total t_w_secyear
WHERE t_s_firstyear.world=t_s_secyear.world AND t_s_firstyear.world=t_w_firstyear.world AND t_s_firstyear.world=t_w_secyear.world AND  t_s_secyear.customer_id = t_s_firstyear.customer_id
  AND t_s_firstyear.customer_id = t_w_secyear.customer_id
  AND t_s_firstyear.customer_id = t_w_firstyear.customer_id
  AND t_s_firstyear.sale_type = 's'
  AND t_w_firstyear.sale_type = 'w'
  AND t_s_secyear.sale_type = 's'
  AND t_w_secyear.sale_type = 'w'
  AND t_s_firstyear.dyear = 2001
  AND t_s_secyear.dyear = 2001+1
  AND t_w_firstyear.dyear = 2001
  AND t_w_secyear.dyear = 2001+1
  AND t_s_firstyear.year_total > 0
  AND t_w_firstyear.year_total > 0
  AND CASE
          WHEN t_w_firstyear.year_total > 0 THEN (t_w_secyear.year_total*1.0000) / t_w_firstyear.year_total
          ELSE 0.0
      END > CASE
                WHEN t_s_firstyear.year_total > 0 THEN (t_s_secyear.year_total*1.0000) / t_s_firstyear.year_total
                ELSE 0.0
            END
QUALIFY row_number() OVER (PARTITION BY t_s_firstyear.world ORDER BY t_s_secyear.customer_id NULLS FIRST,
         t_s_secyear.customer_first_name NULLS FIRST,
         t_s_secyear.customer_last_name NULLS FIRST,
         t_s_secyear.customer_preferred_cust_flag NULLS FIRST) <= 100
ORDER BY t_s_firstyear.world, t_s_secyear.customer_id NULLS FIRST,
         t_s_secyear.customer_first_name NULLS FIRST,
         t_s_secyear.customer_last_name NULLS FIRST,
         t_s_secyear.customer_preferred_cust_flag NULLS FIRST;
